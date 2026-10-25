/// The document and the files around it: importing media and making titles,
/// mattes and adjustment layers, opening, saving and starting again,
/// recovery copies, updates, and the platform's file pickers, message boxes
/// and sounds.

#include "app.hpp"

namespace cutline::tool {

/// Brings files into the project's pool.
///
/// Several at once, because a day's rushes arrive as a folder rather than as a
/// file: the dialog allows a multiple selection and everything picked is
/// probed, filed and labelled as a single edit.
void import_media(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // Room for many names rather than for one path. A multi-select dialog packs
  // the directory and every chosen filename into this one buffer, and a
  // selection too large for it is refused outright rather than truncated — so
  // the size is the real limit on how much can be brought in at once, and
  // MAX_PATH would have made that limit one file.
  std::vector<wchar_t> buffer(64 * 1024, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.main.window;
  dialog.lpstrFilter =
      L"Media\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wav;*.mp3;*.flac;*.png;*.jpg\0"
      L"All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  // OFN_EXPLORER is named because it is what separates the chosen names with
  // nulls. Without it a multiple selection comes back separated by spaces,
  // which cannot be taken apart again once a filename contains one.
  dialog.Flags =
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog) == FALSE) return;

  const std::vector<std::filesystem::path> paths = cutline::editor::chosen_paths(buffer);
  if (paths.empty()) return;

  // Probed before anything is applied, so a folder with one unreadable file in
  // it still brings in the rest. The alternative — stopping at the first
  // refusal — makes one bad file cost the whole selection.
  std::vector<cutline::editor::MediaSource> sources;
  sources.reserve(paths.size());
  std::vector<std::string> refused;
  for (const std::filesystem::path& path : paths) {
    auto source = cutline::app::probe_source(path.string());
    if (source.has_value()) {
      sources.push_back(std::move(*source));
    } else {
      refused.push_back(path.filename().string() + " — " + source.error());
    }
  }

  // Into the pool and nowhere else. Importing is filing something away, not
  // editing with it — and a file that lands on the timeline the moment it is
  // read is one that has to be undone before the sequence can be looked at.
  // Placing it is a gesture of its own: a drag onto a track, or a drop.
  std::vector<std::string> ids;
  if (!sources.empty()) {
    cutline::core::Project next = cutline::editor::import_media(
        app.session.project(), sources, &ids, app.settings.still_length);
    app.session.apply(labelled_new_media(app, std::move(next), ids));
    refresh_all(app);
  }

  // Selected in the pool, so it is obvious where it went — a browser that has
  // silently grown by one is how somebody imports the same file twice. The
  // first of a batch rather than the last, because that is where what arrived
  // begins and scrolling down from it shows the rest.
  const auto first = std::ranges::find_if(ids, [](const std::string& id) { return !id.empty(); });
  if (app.browser != nullptr && first != ids.end()) {
    const auto& items = app.browser->items();
    const auto found = std::ranges::find(items, *first, &cutline::ui::MediaItem::id);
    if (found != items.end()) {
      app.browser->select(static_cast<std::size_t>(found - items.begin()));
    }
    // And into the source monitor, because selecting a row from here does not
    // go through the browser's own callback — it is set, not chosen. Without
    // this the pool showed the new file highlighted and the source monitor
    // said "No source", which is two panels disagreeing about what is selected.
    show_source(app, *first);
  }

  // Said last, so the complaint is about what did not arrive rather than
  // standing between the selection and everything that did. Listed in full up
  // to a point and then counted, because a dialog taller than the screen has
  // an OK button nobody can reach.
  if (!refused.empty()) {
    constexpr std::size_t kMostNamed = 8;
    std::string message = refused.size() == 1 ? "Could not read that file.\n\n"
                                              : "Could not read " + std::to_string(refused.size()) +
                                                    " of the files chosen.\n\n";
    for (std::size_t i = 0; i < refused.size() && i < kMostNamed; ++i) {
      message += refused[i] + "\n";
    }
    if (refused.size() > kMostNamed) {
      message += "and " + std::to_string(refused.size() - kMostNamed) + " more.";
    }
    complain(app.main.window, message);
  }
#else
  complain(app.main.window, "This build has no media layer, so there is nothing to import with.");
#endif
}

/// Makes a title, drops it at the playhead, and selects it.
///
/// Selected because the next thing anyone wants is to type: the inspector shows
/// a title's own text and styling for whatever is selected, so creating one and
/// leaving the selection elsewhere would mean making a card and then having to
/// go and find it.
void add_title(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_graphic_at(app.session.project(),
                                                    cutline::editor::default_title(),
                                                    app.session.playhead(), {}, &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_matte(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_color_matte_at(
      app.session.project(), cutline::editor::MatteFill{}, app.session.playhead(), {},
      &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_adjustment(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_adjustment_layer_at(app.session.project(),
                                                             app.session.playhead(), {},
                                                             &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

/// The system's open or save dialog.
[[nodiscard]] std::optional<std::filesystem::path> choose_file(HWND owner, bool saving) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  // Doubly terminated, and embedded nulls separate the pairs, which is why
  // this is a raw literal rather than anything that counts its own length.
  dialog.lpstrFilter = L"Cutline project\0*.cutline\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"cutline";
  dialog.Flags = saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                        : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

  const BOOL chosen = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// The system's folder picker, for the one setting that names a directory.
///
/// `IFileDialog` with `FOS_PICKFOLDERS` rather than the old
/// `SHBrowseForFolder`, which draws a tree from Windows 2000 and cannot be
/// typed into.
///
/// COM is initialised here rather than at startup because this is the only
/// thing on this thread that needs it, and it is undone on the way out unless
/// somebody else had already claimed the thread for another apartment — in
/// which case the dialog still works and the claim is not ours to release.
[[nodiscard]] std::optional<std::filesystem::path> choose_folder(HWND owner) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool ours = SUCCEEDED(com);
  const auto finish = [ours] {
    if (ours) CoUninitialize();
  };

  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    finish();
    return std::nullopt;
  }
  const auto release = [&] {
    dialog->Release();
    finish();
  };

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  }
  dialog->SetTitle(L"Where proxies go");

  if (FAILED(dialog->Show(owner))) {
    release();
    return std::nullopt;
  }

  IShellItem* item = nullptr;
  if (FAILED(dialog->GetResult(&item))) {
    release();
    return std::nullopt;
  }

  PWSTR path = nullptr;
  std::optional<std::filesystem::path> chosen;
  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
    chosen = std::filesystem::path(path);
    CoTaskMemFree(path);
  }
  item->Release();
  release();
  return chosen;
}

/// Where an export should go. Its own dialog rather than a parameter on the
/// one above, because the filters and the default extension are the whole of
/// what that function does and threading them through would leave neither
/// caller readable.
[[nodiscard]] std::optional<std::filesystem::path> choose_movie_file(HWND owner,
                                                                  std::string_view extension) {
  std::array<wchar_t, MAX_PATH> buffer{};

  const bool mov = extension == ".mov";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = mov ? L"QuickTime movie\0*.mov\0All files\0*.*\0"
                           : L"MP4 video\0*.mp4\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = mov ? L"mov" : L"mp4";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

[[nodiscard]] std::optional<std::filesystem::path> choose_picture_to_open(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"Pictures\0*.png;*.jpg;*.jpeg\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// Where a snapshot should go. A third dialog for the same reason the second
/// exists: the filter and the default extension are the whole of what these
/// functions do.
[[nodiscard]] std::optional<std::filesystem::path> choose_image_file(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"PNG image\0*.png\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"png";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

void complain(HWND owner, const std::string& message) {
  MessageBoxA(owner, message.c_str(), "Cutline", MB_OK | MB_ICONWARNING);
}

/// Plays what the person's Windows sound scheme says an event sounds like,
/// when they have asked for sounds at all.
///
/// By alias, and with no default: an event the scheme leaves silent stays
/// silent rather than falling back to the default beep, and nothing Microsoft
/// recorded is shipped with this. Asynchronous, so a menu never waits on a
/// sound to open.
void play_sound(const App& app, cutline::ui::Sound sound) {
  if (!app.settings.sounds) return;
  const wchar_t* alias = nullptr;
  switch (sound) {
    case cutline::ui::Sound::MenuPopup: alias = L"MenuPopup"; break;
    case cutline::ui::Sound::MenuCommand: alias = L"MenuCommand"; break;
    case cutline::ui::Sound::Refused: alias = L".Default"; break;
    case cutline::ui::Sound::Done: alias = L"SystemNotification"; break;
    case cutline::ui::Sound::Failed: alias = L"SystemHand"; break;
  }
  if (alias != nullptr) PlaySoundW(alias, nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

void give_sounds(App& app, WidgetHost& host) {
  host.set_sound_player([&app](cutline::ui::Sound sound) { play_sound(app, sound); });
}

/// Writes a recovery copy if one is due. Called from the autosave timer.
///
/// Silent about failure, and deliberately: this is a background courtesy, and a
/// message box interrupting an edit to say that a file the user never asked for
/// could not be written would be worse than the thing it is warning about. It
/// simply tries again at the next tick.
void poll_autosave(App& app) {
  const std::uint64_t revision = app.session.revision();
  if (!cutline::editor::autosave_due(app.autosave, app.session.modified(), revision,
                                     std::chrono::steady_clock::now(),
                                     std::chrono::seconds(app.settings.autosave_seconds))) {
    return;
  }

  if (cutline::editor::write_autosave(app.session.path(), app.session.project(),
                                      app.settings.autosave_versions)) {
    app.autosave = {.written_at = std::chrono::steady_clock::now(),
                    .written_revision = revision,
                    .ever_written = true};
  }
}

/// Throws away the recovery copy and forgets there was one.
///
/// After a save, and on the way out. What is left in the recovery directory is
/// then exactly what was never recovered from, which is what makes finding one
/// at startup mean something.
void clear_autosave(App& app, const std::filesystem::path& document) {
  cutline::editor::discard_autosave(document);
  app.autosave = {};
}

/// Offers back a recovery copy of `document`, if there is one newer than the
/// document itself. Returns whether it was taken.
///
/// Asked rather than restored silently. A recovery copy is a guess about what
/// somebody wanted, and one that opens a different project from the one they
/// double-clicked without saying so is a guess made too confidently.
[[nodiscard]] bool offer_recovery(App& app, const std::filesystem::path& document) {
  const auto found = cutline::editor::find_recovery(document);
  if (!found.has_value()) return false;

  const std::string what =
      document.empty() ? std::string("an unsaved project")
                       : document.filename().string();
  const std::string message =
      "Cutline has unsaved changes to " + what +
      " from a session that did not close normally.\n\nRecover them?";
  if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    // Refused once is refused: keeping it would offer the same changes again
    // every time the project was opened.
    clear_autosave(app, document);
    return false;
  }

  const auto loaded = cutline::editor::read_project(found->path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not read the recovered project.\n\n" + loaded.error());
    return false;
  }

  // Opened against the *document's* path, so saving writes where the project
  // lives rather than into the recovery directory. It stays modified, because
  // it is: what is on disk is still the older version.
  app.session.reset(loaded->project, document);
  app.session.mark_unsaved();
  refresh_all(app);
  return true;
}

/// Asks whether there is a newer version.
///
/// Only ever from the button. Nothing checks on its own: an editor that phones
/// home the moment it opens is one that has decided on the user's behalf that
/// it may, and this one has no telemetry and no reason to start.
void check_for_updates(App& app) {
  using State = cutline::app::Updater::State;
  switch (app.updater.state()) {
    case State::Available:
      // Already found; pressing again gets on with it.
      app.updater.download();
      break;
    case State::Ready:
      settle_update(app);
      break;
    case State::Checking:
    case State::Downloading:
      break;
    default:
      app.updater.check(cutline::editor::Version{cutline::core::kVersionMajor,
                                                 cutline::core::kVersionMinor,
                                                 cutline::core::kVersionPatch});
      if (app.version_button != nullptr) app.version_button->set_text("Checking...");
      mark_dirty(app);
      break;
  }
}

/// Acts on whatever the updater has just decided.
///
/// Every branch that offers something asks first. An editor that downloaded and
/// ran an installer because somebody pressed a button labelled with a version
/// number would be doing rather more than it said it would.
void settle_update(App& app) {
  using State = cutline::app::Updater::State;
  const std::string running = std::string("v") + std::string(cutline::core::kVersion);

  switch (app.updater.state()) {
    case State::UpToDate:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      MessageBoxA(app.main.window, ("Cutline " + running + " is the latest version.").c_str(),
                  "Cutline", MB_OK | MB_ICONINFORMATION);
      break;

    case State::Available: {
      const cutline::editor::Release found = app.updater.found();
      const std::string message =
          "Cutline " + found.version.to_string() + " is available.\n\n" +
          (found.notes.empty() ? std::string("") : found.notes + "\n\n") + "Download it now?";
      if (app.version_button != nullptr) {
        app.version_button->set_text("Update to " + found.version.to_string());
      }
      if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        app.updater.download();
      }
      break;
    }

    case State::Ready: {
      // The installer replaces the running program, so the editor has to go
      // first — and anything unsaved has to be dealt with before it does.
      if (!confirm_discard(app)) break;
      if (MessageBoxA(app.main.window,
                      "The update is ready.\n\nCutline will close while it installs.", "Cutline",
                      MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        break;
      }

      const std::wstring path = app.updater.installer().wstring();
      const auto started = reinterpret_cast<INT_PTR>(
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      // Anything at or below 32 is a failure code rather than an instance
      // handle, which is this API's way of saying so.
      if (started <= 32) {
        complain(app.main.window, "The installer would not start. It is at\n\n" +
                                      app.updater.installer().string());
        break;
      }
      PostMessageW(app.main.window, WM_CLOSE, 0, 0);
      break;
    }

    case State::Failed:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      complain(app.main.window, "Could not check for updates.\n\n" + app.updater.error());
      break;

    case State::Downloading:
      if (app.version_button != nullptr) {
        app.version_button->set_text(
            std::format("{:.0f}%", app.updater.progress() * 100.0));
      }
      break;

    case State::Idle:
      break;
  }
  mark_dirty(app);
}

/// Opens a named project, having already established that it is wanted.
///
/// Split from the menu command so the same path serves a file named on the
/// command line — which is what "Open with", a shortcut with an argument, and a
/// project dragged onto the executable all amount to. Without it the
/// application read `--check` and `--benchmark` and silently ignored everything
/// else, so every one of those opened an empty editor.
void open_project_at(App& app, const std::filesystem::path& path) {
  if (offer_recovery(app, path)) return;

  const auto loaded = cutline::editor::read_project(path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not open that project.\n\n" + loaded.error());
    return;
  }

  app.session.reset(loaded->project, path);
  refresh_all(app);

  // Warnings are not failures: a project whose footage has moved still opens,
  // and saying so is more use than refusing it.
  if (!loaded->warnings.empty()) {
    std::string message = "The project opened with warnings:\n";
    for (const std::string& warning : loaded->warnings) message += "\n" + warning;
    complain(app.main.window, message);
  }
}

void open_project(App& app) {
  // Before the file dialog, so somebody who decides to save first is not asked
  // to pick a file twice.
  if (!confirm_discard(app)) return;

  const auto path = choose_file(app.main.window, false);
  if (!path.has_value()) return;

  open_project_at(app, *path);
}

/// Acts on a file named on the command line.
///
/// A project opens; anything the importer recognises goes into the pool, which
/// is what "Open with" on a video should reasonably do and is the same place
/// importing puts it. Anything else is said out loud rather than ignored — a
/// mistyped name that opens an empty editor looks exactly like a crash on
/// startup.
void open_from_command_line(App& app, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    complain(app.main.window, "There is no file at\n\n" + path.string());
    return;
  }

  if (cutline::editor::has_project_extension(path)) {
    open_project_at(app, path);
    return;
  }

#if CUTLINE_HAVE_PREVIEW
  if (cutline::editor::looks_like_media(path.string())) {
    const auto source = cutline::app::probe_source(path.string());
    if (!source.has_value()) {
      complain(app.main.window, "Could not read that file.\n\n" + source.error());
      return;
    }
    std::string id;
    cutline::core::Project next = cutline::editor::import_media(
        app.session.project(), *source, &id, app.settings.still_length);
    const std::array<std::string, 1> ids{id};
    app.session.apply(labelled_new_media(app, std::move(next), ids));
    refresh_all(app);
    if (app.browser != nullptr && !id.empty()) app.browser->select_id(id);
    show_source(app, id);
    return;
  }
#endif

  complain(app.main.window,
           path.filename().string() + " is not a project or a media file.");
}

/// Returns whether it was written, so a cancelled dialog is not mistaken for
/// a successful save.
bool save_project(App& app, bool ask_where) {
  std::filesystem::path path = app.session.path();
  if (ask_where || path.empty()) {
    const auto chosen = choose_file(app.main.window, true);
    if (!chosen.has_value()) return false;
    path = cutline::editor::with_project_extension(*chosen);
  }

  const auto written = cutline::editor::write_project(path, app.session.project());
  if (!written.has_value()) {
    complain(app.main.window, "Could not save the project.\n\n" + written.error());
    return false;
  }

  // The old document's recovery copy goes before the new path is recorded, or
  // "Save As" would leave one behind under the name it used to have.
  clear_autosave(app, app.session.path());
  app.session.mark_saved(path);
  clear_autosave(app, path);
  refresh_title(app);
  return true;
}

/// Asks what to do about unsaved work, and reports whether to carry on.
///
/// Three answers, not two. "Are you sure?" with only Yes and No makes
/// cancelling and discarding the same button, and one of those throws away an
/// afternoon — so Save writes and continues, Discard continues, and Cancel does
/// not. A save that is cancelled at the file dialog cancels the whole thing,
/// which is the only reading of it that cannot lose anything.
[[nodiscard]] bool confirm_discard(App& app) {
  if (!app.session.modified()) return true;

  // The file's own name, not `document_title` — that one carries the asterisk
  // that *means* unsaved, and "unsaved changes to Untitled *" says it twice and
  // reads like a filename with a typo in it.
  const std::string name = app.session.path().empty()
                               ? std::string("this project")
                               : app.session.path().filename().string();
  const int answer = MessageBoxA(
      app.main.window,
      ("There are unsaved changes to " + name + ".\n\nSave them before closing?").c_str(),
      "Cutline", MB_YESNOCANCEL | MB_ICONWARNING);

  if (answer == IDCANCEL) return false;
  if (answer == IDYES) return save_project(app, false);
  return true;
}

void new_project(App& app) {
  if (!confirm_discard(app)) return;
  clear_autosave(app, app.session.path());
  app.session.reset(new_project_model());
  refresh_all(app);
}

}  // namespace cutline::tool
