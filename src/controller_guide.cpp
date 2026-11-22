#include "controller_guide.h"

#include "platform.h"
#include "server_json.h"
#include "strings.h"

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Source = ControllerBindings::Source;
using server_json::text;

const char* const kFrameProfile = "/interaction_profiles/valve/frame_controller_valve";
// SteamVR's profile for each interaction profile the app suggests bindings
// for (simple has no drawing), under its drivers folder.
const std::pair<const char*, const char*> kProfiles[] = {
    {kFrameProfile, "frame_controller/resources/input/frame_controller_profile.json"},
    {"/interaction_profiles/valve/index_controller",
     "indexcontroller/resources/input/index_controller_profile.json"},
    {"/interaction_profiles/oculus/touch_controller", "oculus/resources/input/touch_profile.json"},
};
// SteamVR's names for OpenXR's controls where they differ (Touch's left Menu
// is its system button).
const std::pair<const char*, const char*> kSteamvrNames[] = {
    {"squeeze", "grip"}, {"shoulder", "bumper"}, {"thumbstick", "joystick"}, {"menu", "system"}};
// The order controls are named in.
const char* const kOrder[] = {"trigger", "shoulder", "select",   "squeeze", "a",    "b",
                              "x",       "y",        "thumbstick", "trackpad", "dpad_up",
                              "dpad_left", "dpad_right", "dpad_down", "menu", "view"};

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Discarded (not an object) if missing or not JSON; SteamVR's may have comments.
json readJson(const fs::path& path) { return json::parse(readFile(path), nullptr, false, true); }

// SteamVR's folder, as it registers itself in OpenVR's paths file, else the
// Steam Frame's.
fs::path steamvrPath() {
  std::vector<fs::path> files;
  for (const auto& [variable, under] : {std::pair{"XDG_CONFIG_HOME", ""}, {"HOME", ".config"}})
    if (const char* dir = std::getenv(variable); dir && *dir)
      files.push_back(fs::path(dir) / under / "openvr/openvrpaths.vrpath");
  for (const fs::path& file : files) {
    const json paths = readJson(file);
    if (!paths.is_object() || !paths.contains("runtime") || !paths["runtime"].is_array()) continue;
    for (const json& runtime : paths["runtime"]) {
      std::error_code error;
      if (runtime.is_string() && fs::is_directory(fs::path(runtime.get<std::string>()) / "drivers", error))
        return runtime.get<std::string>();
    }
  }
  return "/opt/steamvr";
}

int rank(const std::string& control) {
  const auto it = std::find(std::begin(kOrder), std::end(kOrder), control);
  return static_cast<int>(it - std::begin(kOrder));
}

std::string join(const std::vector<std::string>& names, const ControllerStrings& t) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += i + 1 == names.size() ? t.orLast : t.comma;
    out += names[i];
  }
  return out;
}

std::string handPath(int hand) { return hand ? "/user/hand/right" : "/user/hand/left"; }

// The SVG's units per binding point: SteamVR's binding screen places them on
// the picture at its natural size, the browser's default 300 × 150 box for a
// picture with only a viewBox.
double pointScale(RsvgHandle* svg, double boxW, double boxH) {
  double w = 0, h = 0;
  if (rsvg_handle_get_intrinsic_size_in_pixels(svg, &w, &h) && w > 0 && h > 0) return boxH / h;
  return std::max(boxW / 300, boxH / 150);
}

ControllerArt::Image render(RsvgHandle* svg, double width, double height) {
  const int w = std::max(1, static_cast<int>(std::lround(width)));
  const int h = std::max(1, static_cast<int>(std::lround(height)));
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t* cr = cairo_create(surface);
  const RsvgRectangle viewport{0, 0, double(w), double(h)};
  const bool ok = rsvg_handle_render_document(svg, cr, &viewport, nullptr);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  if (!ok || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return nullptr;
  }
  return ControllerArt::Image(surface, cairo_surface_destroy);
}

RsvgHandle* openSvg(const fs::path& file) {
  const std::string data = readFile(file);
  if (data.empty()) return nullptr;
  return rsvg_handle_new_from_data(reinterpret_cast<const guint8*>(data.data()), data.size(), nullptr);
}

// The viewBox, else the intrinsic size; false if neither.
bool svgBox(RsvgHandle* svg, double& w, double& h) {
  gboolean hasViewBox = FALSE;
  RsvgRectangle viewBox{};
  rsvg_handle_get_intrinsic_dimensions(svg, nullptr, nullptr, nullptr, nullptr, &hasViewBox, &viewBox);
  if (hasViewBox && viewBox.width > 0 && viewBox.height > 0) {
    w = viewBox.width, h = viewBox.height;
    return true;
  }
  return rsvg_handle_get_intrinsic_size_in_pixels(svg, &w, &h) && w > 0 && h > 0;
}

// On the Steam Frame without its own profile, SteamVR moves Touch's bindings
// onto the Frame's controls by its remapping file's Touch layout: the
// sources as they are on the Frame.
void moveOntoFrame(ControllerBindings& b, const json& remapping) {
  if (!remapping.is_object() || !remapping.contains("layouts") || !remapping["layouts"].is_array())
    return;
  std::map<std::string, std::vector<std::string>> to;
  auto paths = [](const json& j) {
    std::vector<std::string> out;
    if (j.is_string()) out.push_back(j.get<std::string>());
    if (j.is_array())
      for (const json& p : j)
        if (p.is_string()) out.push_back(p.get<std::string>());
    return out;
  };
  auto otherHand = [](std::string path) {
    const std::string left = handPath(0), right = handPath(1);
    if (path.rfind(left, 0) == 0) return right + path.substr(left.size());
    if (path.rfind(right, 0) == 0) return left + path.substr(right.size());
    return path;
  };
  for (const json& layout : remapping["layouts"]) {
    if (text(layout, "from_controller_type") != "oculus_touch") continue;
    std::vector<std::pair<std::string, std::vector<std::string>>> mirrored;
    if (layout.contains("autoremappings") && layout["autoremappings"].is_array())
      for (const json& e : layout["autoremappings"]) {
        const std::string from = text(e, "from");
        const std::vector<std::string> targets = paths(e.is_object() ? e.value("to", json()) : json());
        if (from.empty() || targets.empty()) continue;
        to[from] = targets;
        const auto mirror = e.find("mirror");
        if (mirror == e.end() || !mirror->is_boolean() || mirror->get<bool>()) {
          std::vector<std::string> other;
          for (const std::string& t : targets) other.push_back(otherHand(t));
          mirrored.push_back({otherHand(from), other});
        }
      }
    for (auto& [from, targets] : mirrored) to.emplace(from, targets);  // the explicit ones win
    if (layout.contains("remappings") && layout["remappings"].is_array())
      for (const json& e : layout["remappings"])
        if (text(e, "remapping_mode") == "replace" && e.contains("from") && e.contains("to")) {
          const std::string from = text(e["from"], "path"), target = text(e["to"], "path");
          if (!from.empty() && !target.empty()) to[from] = {target};
        }
  }
  // `one`: each control onto one (a twist's keep their number).
  auto move = [&](std::vector<Source>& sources, bool one) {
    std::vector<Source> moved;
    for (const Source& s : sources) {
      std::string control = controlOf(s.input);
      const std::string feature = s.input.substr(std::min(s.input.size(), s.input.find('/', 7)));
      if (control == "squeeze") control = "grip";
      if (control == "thumbstick") control = "joystick";
      const auto it = to.find(handPath(s.hand) + "/input/" + control);
      if (it == to.end()) {
        moved.push_back(s);
        continue;
      }
      for (const std::string& target : it->second) {
        const int hand = target.rfind(handPath(1), 0) == 0;
        std::string c = controlOf(target.substr(handPath(hand).size()));
        for (const auto& [openxr, steamvr] : kSteamvrNames)
          if (c == steamvr && std::string(openxr) != "menu") c = openxr;
        Source m{hand, "/input/" + c + feature};
        if (one || std::find(moved.begin(), moved.end(), m) == moved.end()) moved.push_back(m);
        if (one) break;
      }
    }
    sources = moved;
  };
  for (int i = 0; i < kControlActions; ++i) {
    move(b.sources[i], false);
    move(b.gripSources[i], false);
  }
  for (ControllerBindings::Twist& twist : b.twists) move(twist.controls, true);
}

}  // namespace

std::string capitalized(const std::string& line) {
  if (line.empty() || !g_utf8_validate(line.c_str(), -1, nullptr)) return line;
  char first[6];
  const int n = g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(line.c_str())), first);
  return std::string(first, n) + (g_utf8_next_char(line.c_str()));
}

bool ControllerBindings::operator==(const ControllerBindings& o) const {
  for (int i = 0; i < kControlActions; ++i)
    if (sources[i] != o.sources[i] || gripSources[i] != o.gripSources[i]) return false;
  return twists == o.twists && profiles[0] == o.profiles[0] && profiles[1] == o.profiles[1];
}

std::string controlOf(const std::string& input) {
  if (input.rfind("/input/", 0) != 0) return std::string();
  const std::string c = input.substr(7, input.find('/', 7) - 7);
  for (const auto& [openxr, steamvr] : kSteamvrNames)
    if (c == steamvr && std::string(openxr) != "menu") return openxr;
  return c;
}

void sortSources(std::vector<Source>& sources) {
  std::stable_sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) {
    return rank(controlOf(a.input)) < rank(controlOf(b.input));
  });
}

std::string controlNames(std::vector<Source> sources, const ControllerStrings& t) {
  sortSources(sources);
  const char* const directions[] = {"dpad_up", "dpad_down", "dpad_left", "dpad_right"};
  std::vector<std::string> names, dpad;
  size_t dpadAt = 0;
  for (const Source& s : sources) {
    const std::string c = controlOf(s.input);
    const auto direction = std::find(std::begin(directions), std::end(directions), c);
    if (direction != std::end(directions)) {
      if (dpad.empty()) dpadAt = names.size();
      const char* d = t.directions[direction - std::begin(directions)];
      if (std::find(dpad.begin(), dpad.end(), d) == dpad.end()) dpad.push_back(d);
      continue;
    }
    std::string name;
    if (c.size() == 1 && c[0] >= 'a' && c[0] <= 'z') name = std::string(1, c[0] - 'a' + 'A');
    else if (c == "trigger") name = t.trigger;
    else if (c == "shoulder") name = t.bumper;
    else if (c == "squeeze") name = t.grip;
    else if (c == "thumbstick") name = s.input.find("/click") != std::string::npos ? t.stickClick : t.stick;
    else if (c == "trackpad") name = t.trackpad;
    else if (c == "menu") name = t.menuButton;
    else if (c == "view") name = t.viewButton;
    else if (c == "select") name = t.selectButton;
    else name = c;
    if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  if (!dpad.empty()) {
    char line[160];
    std::snprintf(line, sizeof(line), t.dpad, join(dpad, t).c_str());
    names.insert(names.begin() + dpadAt, line);
  }
  return join(names, t);
}

std::vector<Source> handSources(const ControllerBindings& bindings, ControlAction action, int hand) {
  for (const int h : {hand, 1 - hand}) {
    std::vector<Source> out;
    for (const Source& s : bindings.sources[static_cast<int>(action)])
      if (s.hand == h) out.push_back(s);
    sortSources(out);
    if (!out.empty()) return out;
  }
  return {};
}

std::string controlGlyph(const std::string& input, ControlAction action, bool upDown) {
  const std::string c = controlOf(input);
  if (c == "a" || c == "b" || c == "x" || c == "y") return "button_" + c;
  if (c == "trigger" || c == "shoulder" || c == "select") return "button_trigger";
  if (c == "squeeze") return "button_grip";
  if (c == "menu" || c == "view") return "button_menu";
  if (c == "system") return "button_system";
  if (c == "thumbstick" || c == "trackpad")
    return "button_" + c + (action != ControlAction::Stick ? "_click" : upDown ? "_scroll" : "_move");
  for (const auto& [dpad, glyph] : {std::pair{"dpad_up", "north"}, {"dpad_down", "south"},
                                    {"dpad_left", "west"}, {"dpad_right", "east"}})
    if (c == dpad) return std::string("button_trackpad_dpad_") + glyph;
  return std::string();
}

std::shared_ptr<const ControllerArt> loadControllerArt(const ControllerBindings& bindings, double w,
                                                       double h, double glyph, double scale,
                                                       std::shared_ptr<const ControllerArt> reuse) {
  auto art = std::make_shared<ControllerArt>();
  art->bindings = bindings;
  const fs::path steamvr = steamvrPath();
  const fs::path drivers = steamvr / "drivers";

  // The drawings: of the profile in use (the right hand's, else the left's);
  // on the Steam Frame always the Frame controller's.
  std::string profile = bindings.profiles[1].empty() ? bindings.profiles[0] : bindings.profiles[1];
  if (kStandaloneHeadset && !profile.empty() && profile != kFrameProfile) {
    moveOntoFrame(art->bindings, readJson(drivers / "frame_controller/resources/input/frame_controller_remapping.json"));
    profile = kFrameProfile;
  }
  fs::path profileFile;
  for (const auto& [openxr, file] : kProfiles)
    if (profile == openxr) profileFile = drivers / file;
  const json p = profileFile.empty() ? json() : readJson(profileFile);
  const fs::path resources = profileFile.parent_path().parent_path();
  for (int hand = 0; hand < 2 && p.is_object() && w > 0; ++hand) {
    const char* side = hand ? "right" : "left";
    std::string image = text(p.value(std::string("input_bindingui_") + side, json()), "image");
    // "{driver}/icons/…": that driver's resources, the profile's own for its
    // resource root.
    const size_t end = image.find("}/");
    if (!image.empty() && image[0] == '{' && end != std::string::npos) {
      const std::string root = image.substr(1, end - 1);
      const fs::path dir = root == text(p, "resource_root") ? resources : drivers / root / "resources";
      image = (dir / image.substr(end + 2)).string();
    }
    if (image.empty()) continue;
    ControllerArt::Drawing& d = art->hands[hand];
    d.file = image;
    RsvgHandle* svg = openSvg(image);
    double boxW = 0, boxH = 0;
    if (!svg || !svgBox(svg, boxW, boxH)) {
      if (svg) g_object_unref(svg);
      continue;
    }
    const double fit = std::min(w / boxW, h / boxH);
    d.width = boxW * fit, d.height = boxH * fit;
    const ControllerArt::Drawing* old = reuse ? &reuse->hands[hand] : nullptr;
    d.image = old && old->file == image && old->width == d.width && old->height == d.height
                  ? old->image
                  : render(svg, d.width * scale, d.height * scale);
    // The bound controls' binding points; the left hand's are mirrored, as
    // SteamVR's binding screen places them.
    const double k = pointScale(svg, boxW, boxH) * fit;
    g_object_unref(svg);
    const json sources = p.value("input_source", json());
    for (int a = 0; a < kControlActions && sources.is_object(); ++a)
      for (const Source& s : art->bindings.sources[a]) {
        const std::string c = controlOf(s.input);
        if (s.hand != hand || d.points.count(c)) continue;
        std::vector<std::string> names{c};
        for (const auto& [openxr, steamvr] : kSteamvrNames)
          if (c == openxr) names.push_back(steamvr);
        for (const std::string& name : names) {
          const json source = sources.value("/input/" + name, json());
          if (!source.is_object() || (source.contains("side") && text(source, "side") != side))
            continue;
          const json at = source.value("binding_image_point", json());
          if (!at.is_array() || at.size() < 2 || !at[0].is_number() || !at[1].is_number()) continue;
          const double x = at[0].get<double>() * k, y = at[1].get<double>() * k;
          d.points[c] = {hand ? x : d.width - x, y};
          break;
        }
      }
  }

  std::set<std::string> glyphs{"button_trigger"};
  for (int a = 0; a < kControlActions; ++a)
    for (const auto* sources : {&art->bindings.sources[a], &art->bindings.gripSources[a]})
      for (const Source& s : *sources)
        for (bool upDown : {false, true}) {
          const std::string name = controlGlyph(s.input, static_cast<ControlAction>(a), upDown);
          if (!name.empty()) glyphs.insert(name);
        }
  for (const ControllerBindings::Twist& twist : art->bindings.twists)
    for (const Source& s : twist.controls) {
      const std::string name = controlGlyph(s.input, ControlAction::Select, false);
      if (!name.empty()) glyphs.insert(name);
    }
  const fs::path callouts = steamvr / "resources/webinterface/dashboard/images/bindingcallouts";
  for (const std::string& name : glyphs) {
    if (reuse && reuse->glyphs.count(name)) {
      art->glyphs[name] = reuse->glyphs.at(name);
      continue;
    }
    if (RsvgHandle* svg = openSvg(callouts / (name + ".svg"))) {
      if (ControllerArt::Image image = render(svg, glyph * scale, glyph * scale))
        art->glyphs[name] = image;
      g_object_unref(svg);
    }
  }
  return art;
}
