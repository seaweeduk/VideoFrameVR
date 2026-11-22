#include "menu_parts.h"

// Edit places: the navigation's places in a group across the page, a row
// each: its name in a field (to rename it), where it is, whether it shows
// and its moves; Reset at the group's title, Done under it.
void Menu::layoutPlacesPage() {
  if (!navigation_.placeRename.path.empty()) {
    layoutPlaceRename();
    return;
  }
  const PlacesStrings& t = text().places;
  auto place = [this](std::function<void(Places&)> change) {
    return [this, change] {
      change(navigation_.places);
      dirty_ = true;
      return MenuCommand{};
    };
  };
  PageLayout page(*this, t.edit, t.hint);
  const double groupW = PageLayout::columnX(2) + PageLayout::kColumnW - PageLayout::columnX(0);
  GroupLayout group = page.groupAt(0, PageLayout::columnX(0), PageLayout::kTop, groupW, t.title);
  // The group's Reset, at its title row's right end (PageLayout::group()).
  {
    constexpr int size = kit::kGroupTitleSize;
    Button b = textButton(t.reset, Icon::None, place([](Places& all) { all.resetOrder(); }));
    b.panel = kBrowser;
    b.bare = b.dimInk = true;
    b.fontSize = size;
    b.h = 36;
    b.w = textWidth(t.reset, size, false) + 2 * 14;
    b.x = PageLayout::columnX(0) + groupW - kit::kGroupPadX - b.w;
    b.y = PageLayout::kTop + (kit::kGroupTitle - b.h) / 2;
    b.radius = kit::kPieceCorner;
    b.tooltip = t.tipReset;
    buttons_.push_back(b);
  }
  constexpr double nameW = 520, moveW = kit::kRowH, in = kit::kSpacePiece;
  const double switchW = switchWidth(t.show);
  const auto& places = navigation_.places.places();
  for (size_t i = 0; i < places.size(); ++i) {
    const Places::Place& p = places[i];
    const GroupLayout::Box row = group.room(kit::kRowH);
    Field name;
    name.text = p.label;
    name.tooltip = t.tipRename;
    name.action = [this, path = p.path, own = p.ownName, label = p.label] {
      navigation_.placeRename = {path, own, label};
      editing_ = true;
      dirty_ = true;
      return MenuCommand{};
    };
    addField(row.x, row.y, nameW, name);
    const double downX = row.x + row.w - moveW, upX = downX - in - moveW;
    const double switchX = upX - kit::kSpace - switchW;
    const double locationX = row.x + nameW + kit::kSpace + kit::kSpace;
    addLabel(locationX, row.y, switchX - kit::kSpace - locationX, row.h, shownLocation(p.path),
             kit::kWellSize, true);
    addSwitch(switchX, row.y, switchW, row.h, t.show, !p.hidden,
              place([path = p.path, hidden = p.hidden](Places& all) {
                all.setHidden(path, !hidden);
              }));
    for (const auto& [by, icon, tip, at] : {std::tuple{-1, Icon::MoveUp, t.tipMoveUp, upX},
                                           std::tuple{1, Icon::MoveDown, t.tipMoveDown, downX}}) {
      if ((by < 0 && i == 0) || (by > 0 && i + 1 == places.size())) continue;
      Button& move = addRound(icon, tip, place([path = p.path, by = by](Places& all) {
                                all.move(path, by);
                              }), at, row.y, moveW);
      move.panel = kBrowser;
      move.bare = true;
      move.radius = kit::kCorner;
    }
  }
  // Done, filled, under the group at its right end.
  Button done = textButton(strings().done, Icon::None, [this] {
    showBrowser();
    return MenuCommand{};
  }, true);
  done.panel = kBrowser;
  done.w = textWidth(done.label, kTextSize, false) + 2 * 20;
  done.x = PageLayout::columnX(0) + groupW - done.w;
  done.y = page.bottom(0) + kit::kSpace;
  done.h = kit::kRowH;
  done.radius = kit::kCorner;
  done.fontSize = kTextSize;
  buttons_.push_back(done);
}

// One place's name in its field, in a group alone on the page over the
// keyboard: Enter or Save keeps it, empty gives the place its own name back
// (faint in the field while it is empty).
void Menu::layoutPlaceRename() {
  const PlacesStrings& t = text().places;
  char title[256];
  std::snprintf(title, sizeof(title), t.renameTitle, navigation_.placeRename.ownName.c_str());
  PageLayout page(*this, t.title);
  GroupLayout group = page.group(0, title);
  Field field;
  field.text = navigation_.placeRename.text;
  field.example = navigation_.placeRename.ownName;
  field.typing = editing_;
  field.action = [this] {
    editing_ = true;
    return MenuCommand{};
  };
  group.field("", field);
  group.note(t.renameHint);
  Action save{t.save, "", [this] {
                savePlaceName();
                return MenuCommand{};
              }};
  save.primary = true;
  group.buttons({save, {strings().cancel, "", [this] {
                          navigation_.placeRename = {};
                          editing_ = false;
                          dirty_ = true;
                          return MenuCommand{};
                        }}});
  if (editing_) layoutKeyboard();
}

void Menu::savePlaceName() {
  navigation_.places.rename(navigation_.placeRename.path, navigation_.placeRename.text);
  navigation_.placeRename = {};
  editing_ = false;
  dirty_ = true;
}
