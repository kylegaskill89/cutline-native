#include "cutline/engine/player.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/audio_mixer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <future>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <windows.h>

#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
// Deliberately after mmdeviceapi.h, which is what brings in the property-key
// machinery this header uses. Sorting these alphabetically does not compile.
#include <functiondiscoverykeys_devpkey.h>

#include <wrl/client.h>

namespace cutline::engine {
namespace {

using Microsoft::WRL::ComPtr;

/// How much audio the device buffers. Shorter means the playhead responds to a
/// seek sooner; too short and a scheduling hiccup becomes an audible gap. Thirty
/// milliseconds is the usual compromise, and shared-mode WASAPI will round it
/// to whatever period the engine actually runs at.
constexpr REFERENCE_TIME kBufferDuration = 30 * 10000;  // in 100-ns units

/// Longer than any buffer period, so a wait that times out means the device has
/// stopped rather than merely being slow.
constexpr DWORD kWaitTimeoutMs = 2000;

/// Releases a COM apartment on scope exit, so an early return cannot leave the
/// thread uninitialised-but-counted.
class ComApartment {
 public:
  explicit ComApartment(HRESULT hr) noexcept : owned_(SUCCEEDED(hr)) {}
  ~ComApartment() {
    if (owned_) CoUninitialize();
  }
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

 private:
  bool owned_;
};

/// A Win32 event that closes itself.
class EventHandle {
 public:
  EventHandle() noexcept : handle_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
  ~EventHandle() {
    if (handle_ != nullptr) CloseHandle(handle_);
  }
  EventHandle(const EventHandle&) = delete;
  EventHandle& operator=(const EventHandle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

 private:
  HANDLE handle_;
};

[[nodiscard]] std::string describe(HRESULT hr) {
  return std::format("0x{:08x}", static_cast<unsigned>(hr));
}

/// Whether a mix format is the 32-bit float this can write directly.
///
/// Shared mode always mixes in float, so this is really a guard against a
/// device reporting something unexpected rather than a case worth converting
/// for.
[[nodiscard]] bool is_float32(const WAVEFORMATEX* format) noexcept {
  if (format == nullptr) return false;
  if (format->wBitsPerSample != 32) return false;
  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
  }
  return false;
}

/// The device's friendly name, for logging. Failure is not worth reporting —
/// an unnamed device still plays.
[[nodiscard]] std::string friendly_name(IMMDevice* device) {
  ComPtr<IPropertyStore> properties;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return "audio device";

  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name = "audio device";
  if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
      value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, nullptr,
                                         nullptr);
    if (size > 1) {
      name.assign(static_cast<std::size_t>(size) - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, name.data(), size, nullptr, nullptr);
    }
  }
  PropVariantClear(&value);
  return name;
}

/// What the render thread reports back once it has a device, so `create` can
/// fail with a real message instead of returning a player that never plays.
struct Opened {
  int sample_rate = 0;
  int channels = 0;
  std::string device;
  bool silent = false;
};

}  // namespace

struct Player::Impl {
  std::thread thread;
  std::atomic<bool> quit{false};
  std::atomic<bool> running{false};
  std::atomic<bool> at_end{false};

  /// Frames the device has actually played, and the timeline position that
  /// corresponds to zero of them. Together these are the playhead.
  std::atomic<std::int64_t> played{0};
  std::atomic<double> base{0.0};

  /// When `played` was last written, in steady-clock nanoseconds.
  ///
  /// The device only reports progress once per buffer period, which on some
  /// endpoints is 30 ms or more. Reporting the playhead as a staircase of those
  /// updates caps the preview's frame rate at the audio callback rate — it
  /// measured 29 fps on a 60 fps project, and looked like a slow compositor
  /// rather than what it was. So the audio clock is an *anchor* and wall time
  /// interpolates between anchors: still audio-mastered, since every update
  /// snaps back to what the card has really played.
  std::atomic<std::int64_t> anchor{0};

  /// The furthest position already handed out, so playback never appears to
  /// step backwards.
  ///
  /// Interpolating between anchors is not monotonic on its own: when a new
  /// anchor lands, the extrapolation it replaces may have run past it, and the
  /// next reading is lower. That is a few milliseconds of wobble, and it is
  /// invisible right up until something downstream treats a backwards request
  /// as a seek — which the frame renderer does, at roughly seventeen times the
  /// cost of decoding the next frame.
  mutable std::atomic<double> furthest{0.0};

  /// A seek waiting to be applied by the render thread. Applying it there
  /// rather than here is what keeps the mixer touched by one thread only.
  std::mutex control;
  bool seek_pending = false;
  double seek_target = 0.0;

  std::unique_ptr<AudioMixer> mixer;
  double timeline = 0.0;

  int rate = 48000;
  int channels = 2;
  std::string device;
  bool silent = true;

  mutable std::mutex error_lock;
  std::string failure;

  std::vector<float> block;

  void set_error(std::string message) {
    const std::lock_guard lock(error_lock);
    if (failure.empty()) failure = std::move(message);
    running.store(false, std::memory_order_release);
  }

  void render(const core::Project& project, PlayerSettings settings,
              std::promise<std::expected<Opened, std::string>> ready);
};

void Player::Impl::render(const core::Project& project, PlayerSettings settings,
                          std::promise<std::expected<Opened, std::string>> ready) {
  // Multi-threaded apartment: this thread owns the device objects and nothing
  // else touches them, so there is no message pump to keep alive.
  const ComApartment apartment(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

  const auto fail = [&ready](std::string message) {
    ready.set_value(std::unexpected(std::move(message)));
  };

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
      FAILED(hr)) {
    return fail(std::format("cannot enumerate audio devices: {}", describe(hr)));
  }

  ComPtr<IMMDevice> endpoint;
  if (HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
      FAILED(hr)) {
    return fail(std::format("no default audio output device: {}", describe(hr)));
  }

  ComPtr<IAudioClient> client;
  if (HRESULT hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
      FAILED(hr)) {
    return fail(std::format("cannot open the audio device: {}", describe(hr)));
  }

  WAVEFORMATEX* mix = nullptr;
  if (HRESULT hr = client->GetMixFormat(&mix); FAILED(hr) || mix == nullptr) {
    return fail(std::format("cannot read the device's mix format: {}", describe(hr)));
  }
  // Owned by us from here; freed on every path out.
  const struct MixFormat {
    WAVEFORMATEX* value;
    ~MixFormat() { CoTaskMemFree(value); }
  } owned_mix{mix};

  if (!is_float32(mix)) {
    return fail(std::format("the device does not mix in 32-bit float ({} bits, tag {})",
                            mix->wBitsPerSample, mix->wFormatTag));
  }

  // The device's format wins over the requested one: shared mode would resample
  // anything else, and doing that twice is worse than mixing at its rate.
  rate = static_cast<int>(mix->nSamplesPerSec);
  channels = static_cast<int>(mix->nChannels);
  (void)settings;

  const EventHandle wake;
  if (!wake.valid()) return fail("cannot create an audio event");

  if (HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDuration, 0,
                                      mix, nullptr);
      FAILED(hr)) {
    return fail(std::format("cannot initialise the audio device: {}", describe(hr)));
  }
  if (HRESULT hr = client->SetEventHandle(wake.get()); FAILED(hr)) {
    return fail(std::format("cannot arm the audio device: {}", describe(hr)));
  }

  UINT32 buffer_frames = 0;
  if (HRESULT hr = client->GetBufferSize(&buffer_frames); FAILED(hr)) {
    return fail(std::format("cannot size the audio buffer: {}", describe(hr)));
  }

  ComPtr<IAudioRenderClient> render_client;
  if (HRESULT hr = client->GetService(IID_PPV_ARGS(&render_client)); FAILED(hr)) {
    return fail(std::format("cannot get the audio render client: {}", describe(hr)));
  }

  // Everything that touches a file happens here, before the first deadline.
  auto built = AudioMixer::create(project, {.sample_rate = rate, .channels = channels});
  if (!built) return fail(built.error());
  mixer = std::move(*built);

  device = friendly_name(endpoint.Get());
  timeline = core::timeline_duration(project);
  silent = mixer->silent();

  block.assign(static_cast<std::size_t>(buffer_frames) * static_cast<std::size_t>(channels),
               0.0f);

  ready.set_value(Opened{rate, channels, device, silent});

  bool started = false;
  double cursor = 0.0;  ///< timeline time the next frame mixed will represent

  while (!quit.load(std::memory_order_acquire)) {
    // A seek is picked up here rather than applied by the caller, so the mixer
    // is only ever touched by this thread.
    bool flush = false;
    {
      const std::lock_guard lock(control);
      if (seek_pending) {
        cursor = seek_target;
        seek_pending = false;
        flush = true;
      }
    }

    if (flush) {
      if (started) {
        // Drop what is already queued, or the jump is preceded by a moment of
        // the old position.
        client->Stop();
        client->Reset();
        started = false;
      }
      mixer->reset();
      base.store(cursor, std::memory_order_release);
      played.store(0, std::memory_order_release);
      at_end.store(false, std::memory_order_release);
    }

    if (!running.load(std::memory_order_acquire)) {
      if (started) {
        client->Stop();
        started = false;
      }
      // Nothing to do until something changes; polling briefly keeps this
      // simple and costs nothing next to the audio buffer's own period.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (!started) {
      if (HRESULT hr = client->Start(); FAILED(hr)) {
        set_error(std::format("cannot start the audio device: {}", describe(hr)));
        return;
      }
      started = true;
    } else if (WaitForSingleObject(wake.get(), kWaitTimeoutMs) != WAIT_OBJECT_0) {
      set_error("the audio device stopped responding");
      return;
    }

    UINT32 padding = 0;
    if (HRESULT hr = client->GetCurrentPadding(&padding); FAILED(hr)) {
      set_error(std::format("cannot query the audio buffer: {}", describe(hr)));
      return;
    }

    const UINT32 wanted = buffer_frames - padding;
    if (wanted == 0) continue;

    BYTE* target = nullptr;
    if (HRESULT hr = render_client->GetBuffer(wanted, &target); FAILED(hr)) {
      set_error(std::format("cannot get an audio buffer: {}", describe(hr)));
      return;
    }

    const auto lanes = static_cast<std::size_t>(channels);
    const std::span<float> out(reinterpret_cast<float*>(target),
                               static_cast<std::size_t>(wanted) * lanes);

    if (auto ok = mixer->mix(cursor, out); !ok) {
      render_client->ReleaseBuffer(wanted, AUDCLNT_BUFFERFLAGS_SILENT);
      set_error(ok.error());
      return;
    }

    if (HRESULT hr = render_client->ReleaseBuffer(wanted, 0); FAILED(hr)) {
      set_error(std::format("cannot submit audio: {}", describe(hr)));
      return;
    }

    cursor += static_cast<double>(wanted) / rate;

    // What has been *played*, not what has been submitted: those differ by the
    // buffer depth, and using the latter would run the picture ahead of the
    // sound.
    UINT32 remaining = 0;
    if (SUCCEEDED(client->GetCurrentPadding(&remaining))) {
      const auto submitted = static_cast<std::int64_t>(
          std::llround((cursor - base.load(std::memory_order_acquire)) * rate));
      played.store(std::max<std::int64_t>(0, submitted - remaining),
                   std::memory_order_release);
      // Written after `played`, so a reader that sees a fresh anchor is
      // guaranteed to see the frame count it belongs to.
      anchor.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                   std::memory_order_release);
    }

    if (timeline > 0.0 && cursor >= timeline) {
      at_end.store(true, std::memory_order_release);
      running.store(false, std::memory_order_release);
    }
  }

  if (started) client->Stop();
}

Player::Player(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Player::~Player() {
  impl_->quit.store(true, std::memory_order_release);
  impl_->running.store(false, std::memory_order_release);
  if (impl_->thread.joinable()) impl_->thread.join();
}

std::expected<std::unique_ptr<Player>, std::string> Player::create(const core::Project& project,
                                                                    PlayerSettings settings) {
  auto impl = std::make_unique<Impl>();

  std::promise<std::expected<Opened, std::string>> promise;
  auto ready = promise.get_future();

  // The project is copied into the thread: a player must not depend on the
  // caller keeping its project alive, and the mixer takes its own copy anyway.
  impl->thread = std::thread([raw = impl.get(), project, settings,
                              promise = std::move(promise)]() mutable {
    raw->render(project, settings, std::move(promise));
  });

  auto opened = ready.get();
  if (!opened) {
    impl->quit.store(true, std::memory_order_release);
    if (impl->thread.joinable()) impl->thread.join();
    return std::unexpected(opened.error());
  }

  impl->rate = opened->sample_rate;
  impl->channels = opened->channels;
  impl->device = opened->device;
  impl->silent = opened->silent;

  return std::unique_ptr<Player>(new Player(std::move(impl)));
}

void Player::play() {
  if (impl_->at_end.load(std::memory_order_acquire)) seek(0.0);
  impl_->running.store(true, std::memory_order_release);
}

void Player::pause() { impl_->running.store(false, std::memory_order_release); }

bool Player::playing() const noexcept { return impl_->running.load(std::memory_order_acquire); }

double Player::position() const noexcept {
  const double base = impl_->base.load(std::memory_order_acquire);
  const auto played = impl_->played.load(std::memory_order_acquire);
  const double anchored = base + static_cast<double>(played) / impl_->rate;

  if (!impl_->running.load(std::memory_order_acquire)) return anchored;

  const auto stamp = impl_->anchor.load(std::memory_order_acquire);
  if (stamp == 0) return anchored;

  // Interpolate from the last thing the card told us. Bounded so a stalled
  // render thread cannot run the picture away from the sound: past this the
  // playhead simply stops, which reads as a freeze rather than as drift.
  constexpr double kMaxExtrapolation = 0.25;
  const std::chrono::steady_clock::time_point at{
      std::chrono::steady_clock::duration{stamp}};
  const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - at)
                           .count();
  const double interpolated = anchored + std::clamp(since, 0.0, kMaxExtrapolation);

  double seen = impl_->furthest.load(std::memory_order_acquire);
  while (interpolated > seen) {
    if (impl_->furthest.compare_exchange_weak(seen, interpolated, std::memory_order_acq_rel)) {
      return interpolated;
    }
  }
  return std::max(seen, interpolated);
}

void Player::seek(double seconds) {
  const std::lock_guard lock(impl_->control);
  impl_->seek_pending = true;
  impl_->seek_target = std::max(0.0, seconds);
  // Published immediately so a caller reading `position` straight after a seek
  // sees where it asked to go, rather than where playback still is.
  impl_->base.store(impl_->seek_target, std::memory_order_release);
  impl_->played.store(0, std::memory_order_release);
  // Cleared, or the position would be extrapolated forward from an anchor
  // belonging to where playback used to be.
  impl_->anchor.store(0, std::memory_order_release);
  // A seek is the one time the playhead is allowed to move backwards.
  impl_->furthest.store(impl_->seek_target, std::memory_order_release);
}

double Player::duration() const noexcept { return impl_->timeline; }

bool Player::finished() const noexcept { return impl_->at_end.load(std::memory_order_acquire); }

const std::string& Player::device_name() const noexcept { return impl_->device; }
int Player::sample_rate() const noexcept { return impl_->rate; }
int Player::channels() const noexcept { return impl_->channels; }
bool Player::silent() const noexcept { return impl_->silent; }

std::string Player::error() const {
  const std::lock_guard lock(impl_->error_lock);
  return impl_->failure;
}

}  // namespace cutline::engine
