#include "cutline/editor/workspace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace cutline::editor {
namespace {

using nlohmann::json;

[[nodiscard]] std::string shown(const std::filesystem::path& path) {
  return path.generic_string();
}

// --------------------------------------------------------------- the layout --
//
// Written by hand rather than through the library's macros, because a node is a
// tagged union in all but name: a tab group and a split share no fields, and
// generating one shape for both would put five empty keys in every node.

[[nodiscard]] json node_to_json(const ui::DockNode& node) {
  json out = json::object();
  if (node.is_split()) {
    out["split"] = node.axis == ui::Axis::Horizontal ? "horizontal" : "vertical";
    out["fractions"] = node.fractions;

    json children = json::array();
    for (const ui::DockNode& child : node.children) children.push_back(node_to_json(child));
    out["children"] = std::move(children);
    return out;
  }
  out["tabs"] = node.panels;
  out["active"] = node.active;
  return out;
}

[[nodiscard]] ui::DockNode node_from_json(const json& value) {
  ui::DockNode node;
  if (!value.is_object()) return node;

  if (value.contains("split")) {
    node.kind = ui::DockKind::Split;
    node.axis = value.value("split", std::string{"horizontal"}) == "vertical"
                    ? ui::Axis::Vertical
                    : ui::Axis::Horizontal;
    node.fractions = value.value("fractions", std::vector<double>{});

    if (const auto found = value.find("children"); found != value.end() && found->is_array()) {
      for (const json& child : *found) node.children.push_back(node_from_json(child));
    }
    return node;
  }

  node.kind = ui::DockKind::Tabs;
  node.panels = value.value("tabs", std::vector<ui::PanelId>{});
  node.active = value.value("active", std::size_t{0});
  return node;
}

[[nodiscard]] json layout_to_json(const ui::DockLayout& layout) {
  json out = json::object();
  out["root"] = node_to_json(layout.root);

  json windows = json::array();
  for (const ui::FloatingDock& window : layout.floating) {
    windows.push_back(json{
        {"id", window.id},
        {"bounds", {window.bounds.x, window.bounds.y, window.bounds.width, window.bounds.height}},
        {"root", node_to_json(window.root)},
    });
  }
  out["floating"] = std::move(windows);
  return out;
}

[[nodiscard]] ui::DockLayout layout_from_json(const json& value) {
  ui::DockLayout layout;
  if (!value.is_object()) return layout;

  if (const auto root = value.find("root"); root != value.end()) {
    layout.root = node_from_json(*root);
  }

  if (const auto found = value.find("floating"); found != value.end() && found->is_array()) {
    for (const json& entry : *found) {
      if (!entry.is_object()) continue;

      ui::FloatingDock window;
      window.id = entry.value("id", std::string{});
      if (const auto root = entry.find("root"); root != entry.end()) {
        window.root = node_from_json(*root);
      }
      if (const auto box = entry.find("bounds");
          box != entry.end() && box->is_array() && box->size() == 4) {
        window.bounds = ui::Rect{(*box)[0].get<double>(), (*box)[1].get<double>(),
                                 (*box)[2].get<double>(), (*box)[3].get<double>()};
      }
      // A window with no name cannot be matched to a real one, and two with the
      // same name would fight over it.
      if (window.id.empty()) window.id = ui::fresh_window_id(layout);
      layout.floating.push_back(std::move(window));
    }
  }

  // Whatever shape the file was in, this is the shape the rest of the code
  // relies on. `normalise` being idempotent is what makes it safe to run over
  // a document of unknown provenance.
  ui::normalise(layout);
  return layout;
}

[[nodiscard]] ui::DockNode tabs(std::vector<ui::PanelId> panels) {
  return ui::DockNode::tabs(std::move(panels));
}

[[nodiscard]] ui::DockNode split(ui::Axis axis, std::vector<ui::DockNode> children,
                                 std::vector<double> fractions) {
  ui::DockNode node = ui::DockNode::split(axis, std::move(children));
  node.fractions = std::move(fractions);
  ui::normalise(node);
  return node;
}

}  // namespace

// ------------------------------------------------------------- workspaces --

ui::DockLayout* Workspaces::current() noexcept {
  const auto found = std::ranges::find(named, active, &Workspace::name);
  if (found != named.end()) return &found->layout;
  return named.empty() ? nullptr : &named.front().layout;
}

const ui::DockLayout* Workspaces::current() const noexcept {
  return const_cast<Workspaces*>(this)->current();
}

std::vector<Workspace> built_in_workspaces() {
  using ui::Axis;

  // Every built-in names every panel, and there is a test that says so. The
  // alternative is not "the panel is somewhere sensible instead" — it is
  // `reconcile_panels` opening it wherever the first tab group happens to be,
  // which put the master fader in a column too narrow to hold it.

  // Editing: the pool and the inspector down the left, the picture over the
  // timeline. The arrangement most of the work happens in. The scopes and the
  // master share the monitor's group, since all three are things you look at
  // rather than work in, and only one at a time.
  Workspace editing;
  editing.name = "Editing";
  editing.layout.root =
      split(Axis::Horizontal,
            {tabs({"project", "effects", "library"}),
             split(Axis::Vertical, {tabs({"monitor", "scopes", "audio"}), tabs({"timeline"})},
                   {0.58, 0.42})},
            {0.26, 0.74});

  // Colour: the picture as large as it will go, the controls beside it, and
  // the timeline reduced to what is needed to move between shots. The scopes
  // go in the side column rather than over the picture — grading means reading
  // both at once.
  Workspace colour;
  colour.name = "Colour";
  colour.layout.root =
      split(Axis::Horizontal,
            {split(Axis::Vertical, {tabs({"monitor"}), tabs({"timeline"})}, {0.74, 0.26}),
             split(Axis::Vertical, {tabs({"scopes"}), tabs({"effects", "library", "project", "audio"})},
                   {0.5, 0.5})},
            {0.72, 0.28});

  // Audio: the timeline given most of the room, because that is where the
  // waveforms are, with the picture kept small for reference. The master gets
  // a column of its own here rather than a tab — a meter behind a tab is a
  // meter nobody is watching, and this is the arrangement for watching it.
  Workspace audio;
  audio.name = "Audio";
  audio.layout.root =
      split(Axis::Vertical,
            {split(Axis::Horizontal,
                   {tabs({"project", "effects", "library", "scopes"}), tabs({"monitor"}), tabs({"audio"})},
                   {0.32, 0.54, 0.14}),
             tabs({"timeline"})},
            {0.42, 0.58});

  return {std::move(editing), std::move(colour), std::move(audio)};
}

Workspaces default_workspaces() {
  Workspaces workspaces;
  workspaces.named = built_in_workspaces();
  workspaces.active = workspaces.named.front().name;
  return workspaces;
}

bool activate_workspace(Workspaces& workspaces, std::string_view name) {
  if (std::ranges::find(workspaces.named, name, &Workspace::name) == workspaces.named.end()) {
    return false;
  }
  if (workspaces.active == name) return false;
  workspaces.active = name;
  return true;
}

bool reset_workspace(Workspaces& workspaces, std::string_view name) {
  const auto mine = std::ranges::find(workspaces.named, name, &Workspace::name);
  if (mine == workspaces.named.end()) return false;

  const std::vector<Workspace> originals = built_in_workspaces();
  const auto original = std::ranges::find(originals, name, &Workspace::name);
  // One the user made up has no definition to go back to, and inventing one
  // would throw their arrangement away for nothing.
  if (original == originals.end()) return false;
  if (mine->layout == original->layout) return false;

  mine->layout = original->layout;
  return true;
}

bool add_workspace(Workspaces& workspaces, std::string name) {
  if (name.empty()) return false;

  const ui::DockLayout* showing = workspaces.current();
  const ui::DockLayout copied = showing == nullptr ? ui::DockLayout{} : *showing;

  if (const auto found = std::ranges::find(workspaces.named, name, &Workspace::name);
      found != workspaces.named.end()) {
    found->layout = copied;
  } else {
    workspaces.named.push_back(Workspace{.name = name, .layout = copied});
  }
  workspaces.active = std::move(name);
  return true;
}

bool remove_workspace(Workspaces& workspaces, std::string_view name) {
  // Something has to be on screen.
  if (workspaces.named.size() <= 1) return false;

  const std::vector<Workspace> originals = built_in_workspaces();
  // A built-in has no other way back once it is gone.
  if (std::ranges::find(originals, name, &Workspace::name) != originals.end()) return false;

  const auto found = std::ranges::find(workspaces.named, name, &Workspace::name);
  if (found == workspaces.named.end()) return false;

  workspaces.named.erase(found);
  if (workspaces.active == name) workspaces.active = workspaces.named.front().name;
  return true;
}

void settle(Workspaces& workspaces, std::span<const ui::PanelId> known) {
  if (workspaces.named.empty()) workspaces.named = built_in_workspaces();

  for (Workspace& workspace : workspaces.named) {
    ui::reconcile_panels(workspace.layout, known);
  }
  if (std::ranges::find(workspaces.named, workspaces.active, &Workspace::name) ==
      workspaces.named.end()) {
    workspaces.active = workspaces.named.front().name;
  }
}

// ------------------------------------------------------------ persistence --

std::string to_json(const Workspaces& workspaces, int indent) {
  json out;
  out["version"] = kWorkspaceSchemaVersion;
  out["active"] = workspaces.active;

  json all = json::array();
  for (const Workspace& workspace : workspaces.named) {
    all.push_back(json{{"name", workspace.name}, {"layout", layout_to_json(workspace.layout)}});
  }
  out["workspaces"] = std::move(all);
  return out.dump(indent);
}

std::expected<Workspaces, std::string> workspaces_from_json(std::string_view text) {
  const json document = json::parse(text, nullptr, false);
  if (document.is_discarded()) return std::unexpected("that is not a workspace file");
  if (!document.is_object()) return std::unexpected("a workspace file has to be an object");

  const int version = document.value("version", 0);
  if (version > kWorkspaceSchemaVersion) {
    return std::unexpected("that workspace file was written by a newer version of Cutline");
  }

  Workspaces workspaces;
  workspaces.active = document.value("active", std::string{});

  if (const auto found = document.find("workspaces");
      found != document.end() && found->is_array()) {
    for (const json& entry : *found) {
      if (!entry.is_object()) continue;

      Workspace workspace;
      workspace.name = entry.value("name", std::string{});
      if (workspace.name.empty()) continue;  // unnameable, and so unreachable
      if (const auto layout = entry.find("layout"); layout != entry.end()) {
        workspace.layout = layout_from_json(*layout);
      }
      workspaces.named.push_back(std::move(workspace));
    }
  }
  return workspaces;
}

std::filesystem::path default_workspace_path() {
  // Beside the user's other application data, which is where a preference
  // belongs. Falling back to the working directory keeps this usable on a
  // machine where the variable is not set rather than failing to save at all.
  const char* roaming = std::getenv("APPDATA");
  std::filesystem::path base = roaming == nullptr ? std::filesystem::path{"."}
                                                  : std::filesystem::path{roaming};
  return base / "Cutline" / "workspaces.json";
}

std::expected<Workspaces, std::string> read_workspaces(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  // Not an error: it means nobody has arranged anything yet.
  if (!file) return default_workspaces();

  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));

  return workspaces_from_json(text.str());
}

std::expected<void, std::string> write_workspaces(const std::filesystem::path& path,
                                                  const Workspaces& workspaces) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }

  // Beside the target so the rename stays within one filesystem and is
  // therefore atomic, exactly as a project is written.
  std::filesystem::path staging = path;
  staging += ".saving";

  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(workspaces);
    file.flush();
    if (!file) return std::unexpected("could not write " + shown(staging));
  }

  std::filesystem::rename(staging, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + shown(path) + ": " + error.message());
  }
  return {};
}

}  // namespace cutline::editor
