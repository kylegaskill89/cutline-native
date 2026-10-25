#pragma once

/// Graphics templates: a graphic kept to be used again, Premiere's Motion
/// Graphics templates in this application's own format.
///
/// One file each, `<name>.cutgfx` in `%APPDATA%\Cutline\Graphics Templates`:
/// the graphic as a project stores it, how long it runs, and the pictures its
/// image layers show, carried inside so a template moved to another machine
/// still has them. Reading one puts those pictures in a folder beside the
/// templates and points the layers at them there.
///
/// A handful are built in, as Premiere's Essential Graphics panel comes with
/// some: a lower third, a title card, a credits roll.

#include "cutline/core/model.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

struct GraphicTemplate {
  std::string name;
  core::Graphic graphic;
  /// How long a clip made from it runs, in seconds.
  double length = 5.0;
  /// The file it came from; empty for a built-in.
  std::filesystem::path file;

  [[nodiscard]] bool built_in() const noexcept { return file.empty(); }
};

/// The templates that come with the application, in the order Browse lists them.
[[nodiscard]] std::vector<GraphicTemplate> built_in_templates();

/// Where saved templates are kept.
[[nodiscard]] std::filesystem::path default_templates_folder();

/// Saves a graphic as a template in `folder`, under its name, replacing one of
/// the same name. Refuses a name that is empty, or that a file cannot have.
/// The pictures of its image layers go inside the file; one that cannot be read
/// is said, rather than leaving a template that is quietly missing it.
[[nodiscard]] std::expected<std::filesystem::path, std::string> save_template(
    const std::filesystem::path& folder, const std::string& name, const core::Graphic& graphic,
    double length);

/// Reads one template, putting its pictures where its layers can find them.
[[nodiscard]] std::expected<GraphicTemplate, std::string> read_template(
    const std::filesystem::path& file);

/// Every template saved in `folder`, by name. One that cannot be read is
/// passed over rather than costing the rest.
[[nodiscard]] std::vector<GraphicTemplate> read_templates(const std::filesystem::path& folder);

/// Whether a file is a template by its name: this application's own, or a
/// Motion Graphics template.
[[nodiscard]] bool is_template_file(const std::filesystem::path& file);

/// Reads a Motion Graphics template (.mogrt). One made in After Effects is
/// refused for good, because it is an After Effects project that only After
/// Effects can draw. One made in Premiere is refused for now, as its layers are
/// kept in a form nobody outside Adobe has written down. Either way the
/// message names the template and says which.
[[nodiscard]] std::expected<GraphicTemplate, std::string> read_mogrt(
    const std::filesystem::path& file);

/// Puts a template file into `folder` so Browse lists it, as Premiere's
/// Install Motion Graphics Template does, replacing one of the same name. A
/// file that cannot be read as a template is refused, saying why, and nothing
/// is left behind.
[[nodiscard]] std::expected<std::filesystem::path, std::string> install_template(
    const std::filesystem::path& folder, const std::filesystem::path& file);

/// Takes a saved template away. False when there was none.
bool delete_template(const std::filesystem::path& file);

/// Puts a template on the timeline at `at`, as a new graphic as long as the
/// template says, on the track Premiere would choose.
[[nodiscard]] core::Project add_template_at(core::Project project, const GraphicTemplate& made,
                                            double at, std::string* clip_id = nullptr);

/// Base64, for the pictures inside a template file. Exposed for its tests.
[[nodiscard]] std::string to_base64(std::string_view bytes);
[[nodiscard]] std::expected<std::string, std::string> from_base64(std::string_view text);

}  // namespace cutline::editor
