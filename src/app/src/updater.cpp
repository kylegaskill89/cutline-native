#include "cutline/app/updater.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

#include <winhttp.h>

namespace cutline::app {
namespace {

/// What the server is told we are. A User-Agent is not optional in practice:
/// some hosts answer a request without one with a refusal rather than a file.
constexpr const wchar_t* kAgent = L"Cutline";

/// The largest installer this will fetch. Generous for what it is, and a limit
/// rather than none: a download with no ceiling is a way to fill somebody's
/// disk from a URL they never saw.
constexpr std::size_t kInstallerLimit = std::size_t{512} << 20;

/// Closes a WinHTTP handle on the way out of a scope. There are three of these
/// per request and every early return has to release all of them, which is
/// exactly the shape that leaks when it is written by hand.
class Handle {
 public:
  explicit Handle(HINTERNET handle = nullptr) noexcept : handle_(handle) {}
  ~Handle() {
    if (handle_ != nullptr) WinHttpCloseHandle(handle_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

  [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

[[nodiscard]] std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(0, size)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

[[nodiscard]] std::string last_error(std::string_view what) {
  return std::format("{} (error {})", what, GetLastError());
}

/// Everything a request needs, pulled out of a URL.
struct Target {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

[[nodiscard]] std::expected<Target, std::string> split(std::string_view url) {
  // https only, checked here as well as in the manifest parser. This function
  // is reachable from more than one place, and "the caller checked" is how an
  // unchecked path eventually appears.
  if (!url.starts_with("https://")) return std::unexpected("only https addresses are fetched");

  const std::wstring wide = widen(url);
  std::array<wchar_t, 256> host{};
  std::array<wchar_t, 2048> path{};

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host.data();
  parts.dwHostNameLength = static_cast<DWORD>(host.size());
  parts.lpszUrlPath = path.data();
  parts.dwUrlPathLength = static_cast<DWORD>(path.size());

  if (WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) == FALSE) {
    return std::unexpected(last_error("that address could not be read"));
  }
  return Target{.host = std::wstring(parts.lpszHostName, parts.dwHostNameLength),
                .path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength),
                .port = parts.nPort};
}

/// Fetches a URL, calling `sink` with each chunk.
///
/// `on_length` is told the total when the server declares one, which is what a
/// progress bar needs and what a chunked response does not have.
[[nodiscard]] std::expected<void, std::string> fetch(
    std::string_view url, std::size_t limit,
    const std::function<bool(const char*, std::size_t)>& sink,
    const std::function<void(std::size_t)>& on_length = {}) {
  const auto target = split(url);
  if (!target.has_value()) return std::unexpected(target.error());

  const Handle session(WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) return std::unexpected(last_error("cannot start a web request"));

  const Handle connection(
      WinHttpConnect(session.get(), target->host.c_str(), target->port, 0));
  if (!connection) return std::unexpected(last_error("cannot reach the server"));

  const Handle request(WinHttpOpenRequest(connection.get(), L"GET", target->path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) return std::unexpected(last_error("cannot make the request"));

  // Redirects are followed, which is the whole reason a permanent manifest URL
  // works: GitHub answers `/releases/latest/download/...` with a redirect to
  // whichever release is newest. Cross-host redirects are allowed because that
  // is exactly what happens — the asset itself lives on a different host from
  // the page — and every hop is still https, which `WINHTTP_FLAG_SECURE` and
  // the option below are what enforce.
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

  if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                         0, 0, 0) == FALSE) {
    return std::unexpected(last_error("cannot send the request"));
  }
  if (WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return std::unexpected(last_error("no answer from the server"));
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return std::unexpected(last_error("the server's answer could not be read"));
  }
  if (status != 200) {
    // Said as a number, because 404 and 403 mean quite different things to
    // whoever has to work out why nothing is updating.
    return std::unexpected(std::format("the server answered {}", status));
  }

  if (on_length) {
    DWORD length = 0;
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request.get(),
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
                            WINHTTP_NO_HEADER_INDEX) != FALSE) {
      on_length(static_cast<std::size_t>(length));
    }
  }

  std::array<char, 16384> buffer{};
  std::size_t total = 0;
  for (;;) {
    DWORD read = 0;
    if (WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                        &read) == FALSE) {
      return std::unexpected(last_error("the download stopped"));
    }
    if (read == 0) break;

    total += read;
    if (total > limit) return std::unexpected("that download is larger than expected");
    if (!sink(buffer.data(), read)) return std::unexpected("the download was stopped");
  }
  return {};
}

}  // namespace

std::expected<std::string, std::string> fetch_text(std::string_view url, std::size_t limit) {
  std::string out;
  const auto appended = [&out](const char* data, std::size_t size) {
    out.append(data, size);
    return true;
  };
  if (const auto ok = fetch(url, limit, appended); !ok.has_value()) {
    return std::unexpected(ok.error());
  }
  return out;
}

// ------------------------------------------------------------------ updater --

struct Updater::Impl {
  std::thread worker;

  mutable std::mutex lock;
  State state = State::Idle;
  editor::Release release;
  std::filesystem::path installer;
  std::string failure;
  std::atomic<double> progress{0.0};

  std::function<void()> on_change;

  /// Sets the state and tells whoever is listening, on the worker's thread.
  void settle(State next, std::string message = {}) {
    {
      const std::lock_guard held(lock);
      state = next;
      failure = std::move(message);
    }
    if (on_change) on_change();
  }

  /// Joins whatever ran last. A worker per job rather than one that lives
  /// forever: these happen when a button is pressed, minutes apart, and a
  /// thread parked on a condition variable for the life of the application
  /// would be more machinery than the job is worth.
  void settle_worker() {
    if (worker.joinable()) worker.join();
  }
};

Updater::Updater() : impl_(std::make_unique<Impl>()) {}

Updater::~Updater() {
  // The worker holds a pointer into `impl_`, so it has to finish first. A check
  // is seconds and a download is bounded by the limit above; neither is a wait
  // worth being clever about.
  impl_->settle_worker();
}

void Updater::set_on_change(std::function<void()> on_change) {
  impl_->on_change = std::move(on_change);
}

void Updater::check(editor::Version current, std::string url) {
  if (state() == State::Checking || state() == State::Downloading) return;
  impl_->settle_worker();
  impl_->settle(State::Checking);

  impl_->worker = std::thread([raw = impl_.get(), current, url = std::move(url)] {
    const auto text = fetch_text(url);
    if (!text.has_value()) {
      raw->settle(State::Failed, text.error());
      return;
    }

    const auto release = editor::parse_release_manifest(*text);
    if (!release.has_value()) {
      raw->settle(State::Failed, release.error());
      return;
    }

    {
      const std::lock_guard held(raw->lock);
      raw->release = *release;
    }
    // "Nothing newer" is a distinct answer from "nothing happened". Somebody
    // who pressed the button is owed one.
    raw->settle(editor::update_available(current, release->version) ? State::Available
                                                                    : State::UpToDate);
  });
}

void Updater::download() {
  if (state() != State::Available) return;
  impl_->settle_worker();
  impl_->progress.store(0.0);
  impl_->settle(State::Downloading);

  impl_->worker = std::thread([raw = impl_.get()] {
    editor::Release release;
    {
      const std::lock_guard held(raw->lock);
      release = raw->release;
    }

    // Named for the version, in the temporary directory. Not beside the
    // running program: that is very often somewhere the user cannot write, and
    // failing to update because the install directory is read-only would be a
    // strange thing to be told.
    std::error_code error;
    const std::filesystem::path target =
        std::filesystem::temp_directory_path(error) /
        std::format("Cutline-{}-Setup.exe", release.version.to_string());
    if (error) {
      raw->settle(State::Failed, "there is nowhere to download to");
      return;
    }

    std::vector<std::uint8_t> bytes;
    std::size_t expected = 0;
    const auto keep = [&bytes](const char* data, std::size_t size) {
      bytes.insert(bytes.end(), data, data + size);
      return true;
    };
    const auto sized = [&expected, raw](std::size_t total) {
      expected = total;
      raw->progress.store(0.0);
    };

    if (const auto ok = fetch(release.installer, kInstallerLimit,
                              [&](const char* data, std::size_t size) {
                                if (!keep(data, size)) return false;
                                if (expected > 0) {
                                  raw->progress.store(static_cast<double>(bytes.size()) /
                                                      static_cast<double>(expected));
                                }
                                return true;
                              },
                              sized);
        !ok.has_value()) {
      raw->settle(State::Failed, ok.error());
      return;
    }

    // Before it touches the disk. A file that failed this check is never worth
    // having: one left lying about beside a real installer is an invitation,
    // and writing it first and deleting it after is a window where it exists.
    if (!editor::digest_matches(release.sha256, editor::sha256_hex(bytes))) {
      raw->settle(State::Failed,
                  "the download did not match the published checksum, so it has been discarded");
      return;
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
      raw->settle(State::Failed, "the download could not be written");
      return;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
      raw->settle(State::Failed, "the download could not be written");
      return;
    }

    {
      const std::lock_guard held(raw->lock);
      raw->installer = target;
    }
    raw->progress.store(1.0);
    raw->settle(State::Ready);
  });
}

Updater::State Updater::state() const noexcept {
  const std::lock_guard held(impl_->lock);
  return impl_->state;
}

double Updater::progress() const noexcept { return impl_->progress.load(); }

editor::Release Updater::found() const {
  const std::lock_guard held(impl_->lock);
  return impl_->release;
}

std::filesystem::path Updater::installer() const {
  const std::lock_guard held(impl_->lock);
  return impl_->installer;
}

std::string Updater::error() const {
  const std::lock_guard held(impl_->lock);
  return impl_->failure;
}

}  // namespace cutline::app
