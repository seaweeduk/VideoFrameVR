#include "menu_parts.h"

#include "menu_list_parts.h"

// Presets on Video settings' Display: a group with the performance
// presets as tiles, Custom while the settings were changed by hand, saving
// them as one of the user's own and the button to the list of those. The
// list applies, moves, renames and deletes them; saving and renaming take a
// name over the keyboard.

namespace {

constexpr double kListW = 640;  // the list of the user's presets, a column of it

MenuCommand userPresetCommand(MenuCommand::Type type, const std::string& path, double value = 0) {
  MenuCommand c;
  c.type = type;
  c.path = path;
  c.value = value;
  return c;
}

}  // namespace

void Menu::layoutPresetGroup(PageLayout& page, int column) {
  const PresetStrings& t = text().presets;
  GroupLayout group = page.group(column, t.label);
  const size_t box = groups_.size() - 1;
  const std::vector<UserPreset>* own = view_.userPresetsListed > 0 ? view_.userPresets : nullptr;
  const int shown = own ? view_.userPresetShown : -1;
  // A tile for each performance preset, saying the eye images' pixels
  // across (and the refresh rate) it sets. As keys: a click redraws only
  // what it changes (the group, the rows whose settings change;
  // Menu::update()), never the whole menu at once while the eye images may
  // be switching.
  std::vector<Tile> tiles;
  const bool stops = view_.resolutionStops != nullptr;
  const int base = view_.resolutionBase;
  // What they say changes only with what the page shows of them.
  char key[160];
  std::snprintf(key, sizeof(key), "%p %p %d %d %d %d", static_cast<const void*>(view_.presets),
                static_cast<const void*>(&text()), base, stops, view_.refreshRate > 0,
                view_.foveationAvailable);
  PresetTileText& said = presetTiles_;
  if (said.key != key) {
    said.key = key;
    for (int i = 0; i < kPerformancePresetCount; ++i) {
      const auto preset = static_cast<PerformancePreset>(i);
      said.offered[i] = view_.presets && view_.presets->offered(preset);
      if (!said.offered[i]) continue;
      const Settings s = view_.presets->with(preset, defaultSettings());
      char sub[64];
      if (base > 0)
        std::snprintf(sub, sizeof(sub), "%ld",
                      stops && s.videoResolutionPixels > 0
                          ? static_cast<long>(s.videoResolutionPixels)
                          : std::lround(base * s.videoResolution / 100.0));
      else
        std::snprintf(sub, sizeof(sub), "%.2f×", s.videoResolution / 100.0);
      said.sub[i] = sub;
      if (view_.refreshRate > 0) {
        std::snprintf(sub, sizeof(sub), text().refreshRateChoice, s.refreshRate);
        said.sub[i] += " · ";
        said.sub[i] += sub;
      }
      said.tip[i] = presetSummary(s);
    }
  }
  for (int i = 0; i < kPerformancePresetCount; ++i) {
    const auto preset = static_cast<PerformancePreset>(i);
    if (!said.offered[i]) continue;
    tiles.push_back({Icon::None, t.names[i], said.sub[i], said.tip[i],
                     shown < 0 && view_.performancePreset == preset,
                     command(MenuCommand::ApplyPreset, i), true});
  }
  // Changed by hand: lit as Foveation's Custom is, and picking a preset
  // puts its settings back.
  if (shown < 0 && view_.performancePreset == PerformancePreset::Custom)
    tiles.push_back({Icon::None, t.custom, {}, t.tipCustom, true, [] { return MenuCommand{}; }, true});
  if (!tiles.empty()) group.tiles(static_cast<int>(tiles.size()), kit::kTileMin, tiles);
  // Saving the settings as one of the user's own, and the list of them
  // (named for the one shown as chosen), once they are listed.
  if (own) {
    Row row = group.button(t.saveAs, [this] {
      openPresetName({}, {});
      return MenuCommand{};
    });
    if (!own->empty()) {
      const bool open = dropdown_ == Dropdown::Presets;
      const std::string name =
          shown >= 0 && shown < static_cast<int>(own->size()) ? (*own)[shown].name : t.myPresets;
      row.endButton(name, [this] {
        const bool opening = dropdown_ != Dropdown::Presets;
        dropdown_ = opening ? Dropdown::Presets : Dropdown::None;
        presetDeleting_.clear();
        presetListPage_ = -1;
        MenuCommand c;
        if (opening) c.type = MenuCommand::ListUserPresets;  // as the folder has them now
        return c;
      }, true, open);
      Button& list = row.control();
      list.tooltip = t.myPresets;
      // As wide as its name: beside the save button where that fits, else
      // on a row of its own under it, from the same left edge.
      const Button& save = buttons_[buttons_.size() - 2];
      if (list.x < save.x + save.w + kit::kSpace) {
        const GroupLayout::Box under = group.room(kit::kRowH);
        list.x = save.x;
        list.w = std::min(list.w, under.x + under.w - list.x);
        list.y = under.y;
      }
      if (open) {
        const Button opener = list;
        layoutPresetList(opener);
      }
    }
  }
  const Group& g = groups_[box];
  presetBox_[0] = g.x;
  presetBox_[1] = g.y;
  presetBox_[2] = g.w;
  presetBox_[3] = g.h;
}

double Menu::presetColumnBottom() const {
  double bottom = presetBox_[1] + presetBox_[3];
  for (const Group& g : groups_)
    if (g.x == presetBox_[0]) bottom = std::max(bottom, g.y + g.h);
  return bottom;
}

// Under its button, in the pop surface (drawPopups()): the user's presets
// in their order as a list's items, each to apply, move, rename or delete
// (asked in its row), then Save as preset…. Columns side by side when there
// are more than the page's height holds, as many as the page is wide for.
// More than those hold: it starts higher, over its button, up to the top of
// the panel as shown, and beyond that it shows them a page at a time.
void Menu::layoutPresetList(const Button& opener) {
  const PresetStrings& t = text().presets;
  const std::vector<UserPreset>& own = *view_.userPresets;
  const int count = static_cast<int>(own.size());
  constexpr double pad = 12;  // popupBox()'s, around the items
  constexpr double rowH = strip::kListItemH, step = rowH + kit::kSpacePiece;
  constexpr double headH = 40;
  constexpr double left = kPageX + kit::kPanelInset + pad, right = kBrowserWidth - kit::kPanelInset - pad;
  constexpr int most = static_cast<int>((right - left + 2 * pad) / (kListW + 2 * pad));
  static_assert(most >= 2, "the pager goes beside Save as preset");
  const double bottom = kBrowserHeight - kit::kPanelInset - pad - step;  // the rows', over Save
  const auto fits = [&](double top) {
    return std::max(1, static_cast<int>((bottom - top - headH) / step));
  };
  const double below = opener.y + opener.h + kit::kSpaceTight + pad;
  double top = below;
  int perColumn = fits(top);
  if (count > perColumn * most) {
    // Its box (popupBox()) inside the panel as alonePanelTop() makes it.
    const double highest = std::ceil(pageTop_) + kit::kSpace + kit::kPanelInset + pad;
    const int rows = (count + most - 1) / most;
    top = std::max(std::min(highest, below), std::min(below, bottom - rows * step - headH));
    perColumn = fits(top);
  }
  const double rowsTop = top + headH;
  const bool paged = count > perColumn * most;
  const int pageSize = perColumn * most;
  const int pages = paged ? (count + pageSize - 1) / pageSize : 1;
  if (presetListPage_ < 0)  // opened: on the one shown
    presetListPage_ = paged && view_.userPresetShown >= 0 ? view_.userPresetShown / pageSize : 0;
  presetListPage_ = std::clamp(presetListPage_, 0, pages - 1);
  const int firstShown = paged ? presetListPage_ * pageSize : 0;
  const int endShown = paged ? std::min(count, firstShown + pageSize) : count;
  const int columns = paged ? most : std::max(1, (count + perColumn - 1) / perColumn);
  const double total = columns * kListW + (columns - 1) * 2 * pad;
  const double x0 = std::clamp(opener.x + pad, left, std::max(left, right - total));
  auto label = [&](const std::string& text, double x, double y, double w, double h, bool dim) {
    Button& b = addLabel(x, y, w, h, text, dim ? kit::kGroupTitleSize : kit::kRowSize, dim);
    b.popup = true;
  };
  label(t.myPresets, x0 + kit::kSpace, top, kListW - 2 * kit::kSpace, headH - kit::kSpaceTight, true);
  if (own.empty()) label(t.none, x0 + kit::kSpace, rowsTop, kListW - 2 * kit::kSpace, rowH, true);

  auto item = [&](Button b, double x, double y, double w) {
    b.panel = kBrowser;
    b.popup = true;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = rowH;
    b.fontSize = kit::kRowSize;
    b.radius = kit::kCorner;
    buttons_.push_back(b);
    return &buttons_.back();
  };
  auto round = [&](Icon icon, const char* tip, std::function<MenuCommand()> action, double x,
                   double y) {
    Button& b = addRound(icon, tip, std::move(action), x, y, rowH);
    b.panel = kBrowser;
    b.popup = true;
    b.bare = true;
    b.radius = kit::kCorner;
  };
  constexpr double gap = kit::kSpacePiece;
  const double nameW = kListW - 4 * rowH - 4 * gap;
  for (int i = firstShown; i < endShown; ++i) {
    const UserPreset& preset = own[i];
    const double x = x0 + ((i - firstShown) / perColumn) * (kListW + 2 * pad);
    const double y = rowsTop + ((i - firstShown) % perColumn) * step;
    if (preset.file == presetDeleting_) {
      char ask[256];
      std::snprintf(ask, sizeof(ask), t.deleteAsk, preset.name.c_str());
      const double deleteW = textWidth(t.deleteButton, kit::kRowSize, false) + 2 * 20;
      const double cancelW = textWidth(text().cancel, kit::kRowSize, false) + 2 * 20;
      const double deleteX = x + kListW - deleteW, cancelX = deleteX - kit::kSpaceTight - cancelW;
      label(ask, x + kit::kSpace, y, cancelX - kit::kSpace - x - kit::kSpace, rowH, false);
      item(textButton(text().cancel, Icon::None,
                      [this] {
                        presetDeleting_.clear();
                        dirty_ = true;
                        return MenuCommand{};
                      }),
           cancelX, y, cancelW)
          ->raised = true;
      item(textButton(t.deleteButton, Icon::None,
                      [this, file = preset.file] {
                        presetDeleting_.clear();
                        return userPresetCommand(MenuCommand::DeleteUserPreset, file);
                      }),
           deleteX, y, deleteW)
          ->danger = true;
      continue;
    }
    Button* name = item(textButton(preset.name, Icon::None,
                                   [this, file = preset.file] {
                                     dropdown_ = Dropdown::None;
                                     return userPresetCommand(MenuCommand::ApplyUserPreset, file);
                                   },
                                   i == view_.userPresetShown),
                        x, y, nameW);
    name->listItem = true;
    if (view_.presets) name->tooltip = presetSummary(view_.presets->with(preset, defaultSettings()));
    double bx = x + nameW + gap;
    // Moved off the page shown, the page goes with it.
    if (i > 0)
      round(Icon::MoveUp, t.tipMoveUp, [this, file = preset.file, off = i == firstShown && paged] {
        if (off) --presetListPage_;
        return userPresetCommand(MenuCommand::MoveUserPreset, file, -1);
      }, bx, y);
    bx += rowH + gap;
    if (i + 1 < count)
      round(Icon::MoveDown, t.tipMoveDown, [this, file = preset.file, off = i + 1 == endShown && paged] {
        if (off) ++presetListPage_;
        return userPresetCommand(MenuCommand::MoveUserPreset, file, 1);
      }, bx, y);
    bx += rowH + gap;
    round(Icon::Pencil, t.tipRename, [this, file = preset.file, n = preset.name] {
      openPresetName(file, n);
      return MenuCommand{};
    }, bx, y);
    bx += rowH + gap;
    round(Icon::Trash, t.tipDelete, [this, file = preset.file] {
      presetDeleting_ = file;
      dirty_ = true;
      return MenuCommand{};
    }, bx, y);
  }
  const int rows = std::min(std::max(count, 1), perColumn);
  Button save = textButton(t.saveAs, Icon::Plus, [this] {
    openPresetName({}, {});
    return MenuCommand{};
  });
  save.listItem = save.bare = save.on = true;  // in the accent: an action
  item(save, x0, rowsTop + rows * step, kListW);
  if (!paged) return;
  // The pages, as the places' (layoutNavigation()): beside Save as preset,
  // under the last column.
  const double px = x0 + (columns - 1) * (kListW + 2 * pad), py = rowsTop + rows * step;
  auto turn = [&](Icon icon, int by, double x, bool disabled) {
    Button& b = addRound(icon, {}, [this, by] {
      presetListPage_ += by;
      return MenuCommand{};
    }, x, py, rowH);
    b.panel = kBrowser;
    b.popup = b.bare = true;
    b.radius = kit::kCorner;
    b.iconSize = 26;
    b.disabled = disabled;
  };
  turn(Icon::ChevronLeft, -1, px, presetListPage_ == 0);
  turn(Icon::Chevron, 1, px + kListW - rowH, presetListPage_ + 1 >= pages);
  Button dots;
  dots.panel = kBrowser;
  dots.popup = true;
  dots.listLook = static_cast<int>(ListLook::PagerDots);
  dots.x = px + rowH;
  dots.y = py;
  dots.w = kListW - 2 * rowH;
  dots.h = rowH;
  dots.sliderValue = presetListPage_;
  dots.sliderMax = pages;
  buttons_.push_back(dots);
}

void Menu::openPresetName(const std::string& file, const std::string& name) {
  presetName_ = {true, file, name, name};
  presetDeleting_.clear();
  dropdown_ = Dropdown::None;
  editing_ = true;
  dirty_ = true;
}

Menu::PresetNameCheck Menu::checkPresetName() const {
  PresetNameCheck check;
  const std::string name = presetName(presetName_.text);
  if (name.empty()) return check;
  const PresetStrings& t = text().presets;
  // Built-in names in English (what other languages' presets are saved
  // with) and as shown.
  for (const char* builtIn : {"Low", "Balanced", "High", "Custom", t.names[0], t.names[1],
                              t.names[2], t.custom})
    if (samePresetName(name, builtIn)) {
      check.kind = PresetNameCheck::BuiltIn;
      return check;
    }
  check.kind = PresetNameCheck::Fine;
  if (!view_.userPresets) return check;
  for (size_t i = 0; i < view_.userPresets->size(); ++i) {
    const UserPreset& p = (*view_.userPresets)[i];
    if (p.file != presetName_.file && samePresetName(p.name, name)) {
      check.kind = PresetNameCheck::Other;
      check.preset = static_cast<int>(i);
      break;
    }
  }
  return check;
}

// Save (Replace over the user's preset of that name) or Rename; back to the
// list.
MenuCommand Menu::submitPresetName() {
  const PresetNameCheck check = checkPresetName();
  const bool renaming = !presetName_.file.empty();
  if (check.kind == PresetNameCheck::Empty || check.kind == PresetNameCheck::BuiltIn ||
      (renaming && check.kind == PresetNameCheck::Other))
    return {};
  MenuCommand c = renaming ? userPresetCommand(MenuCommand::RenameUserPreset,
                                               presetName_.file + "\n" + presetName_.text)
                           : userPresetCommand(MenuCommand::SaveUserPreset, presetName_.text);
  presetName_ = {};
  editing_ = false;
  dropdown_ = Dropdown::Presets;
  dirty_ = true;
  return c;
}

// The Name field in a group alone on the page, the keyboard under it: what
// the preset holds, why a name can't be used, and Save, Replace or Rename.
void Menu::layoutPresetName() {
  const PresetStrings& t = text().presets;
  const bool renaming = !presetName_.file.empty();
  char line[1024];
  if (renaming)
    std::snprintf(line, sizeof(line), t.renameTitle, presetName_.name.c_str());
  else
    std::snprintf(line, sizeof(line), "%s", t.saveTitle);
  PageLayout page(*this, text().videoSettings);
  GroupLayout group = page.group(0, line);
  group.labelsFor({t.name});
  Field field;
  field.text = presetName_.text;
  field.typing = editing_;
  field.action = [this] {
    editing_ = true;
    return MenuCommand{};
  };
  group.field(t.name, field);

  // What it holds: the settings now, or the preset's.
  Settings values;
  bool known = !renaming;
  if (!renaming) {
    values.videoResolution = view_.resolutionStops ? 0 : view_.videoResolution;
    values.videoResolutionPixels = view_.resolutionStops ? view_.videoResolution : 0;
    values.foveation = view_.foveation;
    values.videoMipmaps = view_.videoMipmaps;
    values.antiShimmer = view_.antiShimmer;
    values.sharpen = view_.sharpen;
    values.toneLook = view_.toneLook;
    values.tvGamma = view_.tvGamma;
    values.refreshRate = view_.refreshRate;
  } else if (view_.userPresets && view_.presets) {
    for (const UserPreset& p : *view_.userPresets)
      if (p.file == presetName_.file) {
        values = view_.presets->with(p, defaultSettings());
        known = true;
      }
  }
  if (known) {
    std::snprintf(line, sizeof(line), renaming ? t.itsSettings : t.savesThis,
                  presetSummary(values).c_str());
    group.note(line, true, 5);
  }
  const PresetNameCheck check = checkPresetName();
  const std::string typed = presetName(presetName_.text);
  const char* note = renaming ? t.renameHint : t.saveHint;
  bool warning = true;
  switch (check.kind) {
    case PresetNameCheck::BuiltIn:
      std::snprintf(line, sizeof(line), t.builtInName, typed.c_str());
      break;
    case PresetNameCheck::Other:
      std::snprintf(line, sizeof(line), renaming ? t.nameTaken : t.replaceWarning,
                    (*view_.userPresets)[check.preset].name.c_str());
      break;
    default:
      std::snprintf(line, sizeof(line), "%s", note);
      warning = false;
  }
  // A name that can't be used says why, as a failed state; a replace warns.
  if (warning && check.kind == PresetNameCheck::BuiltIn)
    group.status(Lamp::Failed, line);
  else if (warning && renaming)
    group.status(Lamp::Failed, line);
  else
    group.note(line, warning, 2);

  const bool replace = !renaming && check.kind == PresetNameCheck::Other;
  const bool fine = check.kind == PresetNameCheck::Fine || replace;
  Action ok{renaming ? t.rename : replace ? t.replace : t.save, "", [this] { return submitPresetName(); }};
  ok.disabled = !fine;
  ok.primary = true;
  group.buttons({ok, {text().cancel, "", [this] {
                        presetName_ = {};
                        editing_ = false;
                        dropdown_ = Dropdown::Presets;
                        dirty_ = true;
                        return MenuCommand{};
                      }}});
  if (editing_) layoutKeyboard();
}

// "Resolution: 1.75× · 3780, Foveation: High, Video mipmaps: Off, …": what
// a preset sets, as the page's rows name it.
std::string Menu::presetSummary(const Settings& s) const {
  const Strings& t = text();
  const bool stops = view_.resolutionStops != nullptr;
  const int base = view_.resolutionBase;
  std::string out;
  auto add = [&](const char* label, const std::string& value) {
    char item[256];
    std::snprintf(item, sizeof(item), t.presets.item, label, value.c_str());
    if (!out.empty()) out += t.presets.separator;
    out += item;
  };
  add(t.eyeResolution,
      resolutionText(stops && base > 0 ? s.videoResolutionPixels * 100.0 / base : s.videoResolution,
                     base));
  if (kStandaloneHeadset && view_.foveationAvailable)
    add(t.foveation, t.foveationLevels[std::clamp(static_cast<int>(s.foveation), 0, 4)]);
  if (view_.refreshRate > 0) {
    char hz[32];
    std::snprintf(hz, sizeof(hz), t.refreshRateChoice, s.refreshRate);
    add(t.refreshRate, hz);
  }
  add(t.videoMipmaps, s.videoMipmaps ? t.on : t.off);
  if (!s.videoMipmaps)
    add(t.antiShimmer, t.antiShimmerLevels[std::clamp(static_cast<int>(s.antiShimmer), 0, 2)]);
  char sharpen[16];
  std::snprintf(sharpen, sizeof(sharpen), "%.2f", s.sharpen / 100.0);
  add(t.sharpen, s.sharpen > 0 ? sharpen : t.off);
  add(t.toneLook, s.toneLook ? t.on : t.off);
  add(t.tvGamma, s.tvGamma ? t.on : t.off);
  return out;
}
