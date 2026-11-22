#include "menu_stash_covers.h"

#include "file_util.h"
#include "menu_stash.h"
#include "menu_style.h"
#include "offline.h"

#include <cairo.h>

#include <numeric>
#include <set>

// Stash in the menu's existing views (docs/stash.md): the file browser's and
// the library's videos that Stash knows show its cover, its title with the
// performers and studio, and the studio's logo on the tile. Everything here
// runs only while Stash is on; with it off the menu has no StashCovers and
// these views are as they were.

namespace {

// A studio's mark sits in the tile's top right corner, inset from it, at
// most this part of the picture's width and height.
constexpr double kLogoInset = 10;
constexpr double kLogoWidth = 0.36, kLogoHeight = 0.2;
constexpr double kLogoOpacity = 0.75;  // as on Stash's own cards
// The shadow: the logo's outline blurred by three box passes of this
// radius (texture pixels), about a Gaussian of 3 pixels.
constexpr int kShadowRadius = 3;
constexpr int kShadowPad = 3 * kShadowRadius;
constexpr size_t kMaxLogos = 64;  // a few screens of tiles
// New shadows made in one frame (up to a few milliseconds each); the rest
// the next frames.
constexpr int kLogosPerFrame = 2;

std::string joined(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return a + "  ·  " + b;
}

// One pass of a box blur of radius r along rows (step 1) or columns (step
// = the row's length), from `in` into `out`.
void boxBlur(const std::vector<float>& in, std::vector<float>& out, int length, int count,
             int step, int stride, int r) {
  const float scale = 1.0f / (2 * r + 1);
  for (int line = 0; line < count; ++line) {
    const size_t base = static_cast<size_t>(line) * stride;
    float sum = 0;
    for (int i = -r; i <= r; ++i)
      if (i >= 0 && i < length) sum += in[base + static_cast<size_t>(i) * step];
    for (int i = 0; i < length; ++i) {
      out[base + static_cast<size_t>(i) * step] = sum * scale;
      const int add = i + r + 1, drop = i - r;
      if (add < length) sum += in[base + static_cast<size_t>(add) * step];
      if (drop >= 0) sum -= in[base + static_cast<size_t>(drop) * step];
    }
  }
}

// Many studios' logos come on an opaque white or light box while others
// have none. When the w × h logo at (x0, y0) has such a box (most of its
// opaque edge light, and most of the edge opaque), the box becomes
// see-through: the area of its colour joined to the edge, and the holes
// of its colour in black or grey letters (the inside of an O). Each of
// those pixels and the ones next to them keep only what differs from the
// box's colour, so that the edges of the letters blend into what is behind
// instead of leaving a pale fringe. Other areas of the box's colour, such
// as light letters on a coloured band, stay.
bool clearLightBox(uint32_t* pixels, int stride, int x0, int y0, int w, int h) {
  if (w < 3 || h < 3) return false;
  const auto at = [&](int x, int y) -> uint32_t& {
    return pixels[static_cast<size_t>(y0 + y) * stride + x0 + x];
  };
  const auto channel = [](uint32_t p, int shift) {
    const uint32_t a = p >> 24;
    return a ? ((p >> shift) & 255) / static_cast<float>(a) : 0.0f;
  };
  int edge = 0, opaque = 0, light = 0;
  float key[3] = {0, 0, 0};
  const auto count = [&](int x, int y) {
    const uint32_t p = at(x, y);
    ++edge;
    if ((p >> 24) < 217) return;  // 0.85
    ++opaque;
    float least = 1;
    for (int i = 0; i < 3; ++i) least = std::min(least, channel(p, 16 - 8 * i));
    if (least < 0.85f) return;
    ++light;
    for (int i = 0; i < 3; ++i) key[i] += channel(p, 16 - 8 * i);
  };
  for (int x = 0; x < w; ++x) count(x, 0), count(x, h - 1);
  for (int y = 1; y < h - 1; ++y) count(0, y), count(w - 1, y);
  if (opaque < edge / 2 || light < opaque * 0.7) return false;
  for (float& k : key) k /= light;
  // How much of a pixel is not the box: the least coverage of a colour
  // that over the box's colour gives it, 0-1.
  const auto unlike = [&](uint32_t p) {
    float most = 0;
    for (int i = 0; i < 3; ++i) {
      const float c = channel(p, 16 - 8 * i), k = key[i];
      if (c < k)
        most = std::max(most, (k - c) / k);
      else if (k < 0.98f)
        most = std::max(most, (c - k) / (1 - k));
    }
    return most < 0.04f ? 0.0f : std::min(1.0f, most);  // compression noise
  };
  const auto boxLike = [&](uint32_t p) { return (p >> 24) >= 217 && unlike(p) < 0.5f; };
  // 0 not reached, 1 the box, 2 next to it, 3 looked at for a hole.
  std::vector<uint8_t> state(static_cast<size_t>(w) * h, 0);
  const auto index = [&](int x, int y) { return static_cast<size_t>(y) * w + x; };
  std::vector<std::pair<int, int>> todo;
  const auto reach = [&](int x, int y) {
    uint8_t& s = state[index(x, y)];
    if (s == 1) return;
    if (boxLike(at(x, y))) {
      s = 1;
      todo.emplace_back(x, y);
    } else {
      s = 2;
    }
  };
  const auto spread = [&] {
    while (!todo.empty()) {
      const auto [x, y] = todo.back();
      todo.pop_back();
      if (x > 0) reach(x - 1, y);
      if (x < w - 1) reach(x + 1, y);
      if (y > 0) reach(x, y - 1);
      if (y < h - 1) reach(x, y + 1);
    }
  };
  for (int x = 0; x < w; ++x) reach(x, 0), reach(x, h - 1);
  for (int y = 1; y < h - 1; ++y) reach(0, y), reach(w - 1, y);
  spread();
  // Holes: areas of the box's colour inside, all around them black or grey,
  // and the box within a letter's stroke of them (light letters on a black
  // band are further from it).
  const auto darkGrey = [&](uint32_t p) {
    const float r = channel(p, 16), g = channel(p, 8), b = channel(p, 0);
    return (p >> 24) >= 128 && std::max({r, g, b}) - std::min({r, g, b}) < 0.12f &&
           0.2126f * r + 0.7152f * g + 0.0722f * b < 0.75f;  // its edge's blend too
  };
  const int stroke = std::max(3, static_cast<int>(std::lround(h * 0.075 + 1.5)));
  std::vector<int> seen(state.size(), 0);
  int hole = 0;
  std::vector<std::pair<int, int>> ring;
  const auto nearBox = [&](const std::vector<std::pair<int, int>>& from) {
    ++hole;
    ring = from;
    for (const auto& [x, y] : from) seen[index(x, y)] = hole;
    for (int step = 0; step < stroke && !ring.empty(); ++step) {
      std::vector<std::pair<int, int>> next;
      for (const auto& [x, y] : ring) {
        const std::pair<int, int> around[] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& [nx, ny] : around) {
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t i = index(nx, ny);
          if (seen[i] == hole) continue;
          if (state[i] == 1) return true;
          seen[i] = hole;
          next.emplace_back(nx, ny);
        }
      }
      ring = std::move(next);
    }
    return false;
  };
  std::vector<std::pair<int, int>> area;
  for (int y = 1; y < h - 1; ++y)
    for (int x = 1; x < w - 1; ++x) {
      if (state[index(x, y)] || !boxLike(at(x, y))) continue;
      area.clear();
      area.emplace_back(x, y);
      state[index(x, y)] = 3;
      int around = 0, dark = 0;
      for (size_t i = 0; i < area.size(); ++i) {
        const auto [ax, ay] = area[i];
        const std::pair<int, int> next[] = {{ax - 1, ay}, {ax + 1, ay}, {ax, ay - 1}, {ax, ay + 1}};
        for (const auto& [nx, ny] : next) {
          uint8_t& s = state[index(nx, ny)];
          if (s == 3) continue;
          if (s != 1 && boxLike(at(nx, ny))) {
            s = 3;
            area.emplace_back(nx, ny);
          } else {
            ++around;
            if (darkGrey(at(nx, ny))) ++dark;
          }
        }
      }
      if (dark < around * 0.85 || !nearBox(area)) continue;
      for (const auto& [ax, ay] : area) state[index(ax, ay)] = 0;
      for (const auto& [ax, ay] : area) reach(ax, ay);
      spread();
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t s = state[index(x, y)];
      if (s != 1 && s != 2) continue;
      uint32_t& p = at(x, y);
      const uint32_t a = p >> 24;
      if (!a) continue;
      // Premultiplied, a channel c of the colour that over the box's
      // colour k with coverage `keep` gives it is c - k (1 - keep).
      const float keep = unlike(p);
      uint32_t out = static_cast<uint32_t>(std::lround(keep * a)) << 24;
      for (int i = 0; i < 3; ++i) {
        const float c = std::clamp(channel(p, 16 - 8 * i) - key[i] * (1 - keep), 0.0f, keep);
        out |= static_cast<uint32_t>(std::lround(c * a)) << (16 - 8 * i);
      }
      p = out;
    }
  return true;
}

// The logo fitted into boxW × boxH pixels, on a soft shadow of its own
// shape: dark under a light logo, light around a dark one, so that both
// read on any cover. kShadowPad pixels of room on each side.
cairo_surface_t* shadowedLogo(cairo_surface_t* source, int boxW, int boxH) {
  const int sw = cairo_image_surface_get_width(source), sh = cairo_image_surface_get_height(source);
  if (sw <= 0 || sh <= 0 || boxW <= 0 || boxH <= 0) return nullptr;
  const double scale = std::min(static_cast<double>(boxW) / sw, static_cast<double>(boxH) / sh);
  const int lw = std::max(1, static_cast<int>(std::lround(sw * scale)));
  const int lh = std::max(1, static_cast<int>(std::lround(sh * scale)));
  const int w = lw + 2 * kShadowPad, h = lh + 2 * kShadowPad;
  cairo_surface_t* logo = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  {
    cairo_t* cr = cairo_create(logo);
    cairo_translate(cr, kShadowPad, kShadowPad);
    cairo_scale(cr, static_cast<double>(lw) / sw, static_cast<double>(lh) / sh);
    cairo_set_source_surface(cr, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);  // BEST: 3-10 times as long
    // Edges as opaque as the picture's own, for clearLightBox to see a box.
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_rectangle(cr, 0, 0, sw, sh);
    cairo_fill(cr);
    cairo_destroy(cr);
  }
  cairo_surface_flush(logo);
  auto* pixels = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(logo));
  const int stride = cairo_image_surface_get_stride(logo) / 4;
  // How light it is, weighted by coverage (premultiplied: the colour is
  // already scaled by it).
  const auto luminance = [](uint32_t p) {
    return 0.2126f * ((p >> 16) & 255) + 0.7152f * ((p >> 8) & 255) + 0.0722f * (p & 255);
  };
  // Grey: little difference between the channels for its coverage.
  const auto grey = [](uint32_t p) {
    const uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    return std::max({r, g, b}) - std::min({r, g, b}) < 0.25f * (p >> 24);
  };
  bool dark = false, lighten = false;
  const auto measure = [&] {
    double light = 0, lightCover = 0, darkCover = 0, cover = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const uint32_t p = pixels[static_cast<size_t>(y) * stride + x];
        const float l = luminance(p);
        light += l;
        cover += p >> 24;
        if (l > 0.6f * (p >> 24)) lightCover += p >> 24;
        if (grey(p) && l < 0.35f * (p >> 24)) darkCover += p >> 24;
      }
    dark = cover > 0 && light / cover < 0.3;
    // Black, grey or nearly grey lettering with nothing light (often a
    // logo that came on a white box) is drawn light, as most logos are,
    // rather than dark on the dark menu: its grey parts get the opposite
    // lightness, coloured parts stay. A logo with light parts too, such as
    // light letters on a dark badge, keeps its own look.
    lighten = lightCover < 0.08 * cover && darkCover > 0.25 * cover;
  };
  std::vector<uint32_t> boxed(static_cast<size_t>(stride) * h);
  std::copy(pixels, pixels + boxed.size(), boxed.begin());
  const bool cleared = clearLightBox(pixels, stride, kShadowPad, kShadowPad, lw, lh);
  measure();
  // A dark logo that can't be made light keeps its box, to stay readable.
  if (cleared && dark && !lighten) {
    std::copy(boxed.begin(), boxed.end(), pixels);
    measure();
  }
  if (lighten) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint32_t& p = pixels[static_cast<size_t>(y) * stride + x];
        const uint32_t a = p >> 24;
        if (!a || !grey(p)) continue;
        const float shift = a - 2 * luminance(p);
        const auto flip = [&](int s) {
          const float c = ((p >> s) & 255) + shift;
          return static_cast<uint32_t>(std::lround(std::clamp(c, 0.0f, static_cast<float>(a)))) << s;
        };
        p = a << 24 | flip(16) | flip(8) | flip(0);
      }
    dark = false;
  }
  std::vector<float> alpha(static_cast<size_t>(w) * h), blurred(alpha.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      alpha[static_cast<size_t>(y) * w + x] = (pixels[static_cast<size_t>(y) * stride + x] >> 24) / 255.0f;
  for (int pass = 0; pass < 3; ++pass) {
    boxBlur(alpha, blurred, w, h, 1, w, kShadowRadius);
    boxBlur(blurred, alpha, h, w, w, 1, kShadowRadius);
  }
  // The logo over its shadow.
  const float strength = dark ? 0.6f : 0.85f;
  const float shade = dark ? 255.0f : 0.0f;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint32_t& p = pixels[static_cast<size_t>(y) * stride + x];
      const float s = std::min(1.0f, alpha[static_cast<size_t>(y) * w + x] * 1.6f) * strength;
      const float keep = 1.0f - (p >> 24) / 255.0f;
      const float sa = s * 255.0f * keep, sc = s * shade * keep;
      const auto add = [](uint32_t channel, float value) {
        return static_cast<uint32_t>(std::min(255.0f, channel + value + 0.5f));
      };
      p = add(p >> 24, sa) << 24 | add((p >> 16) & 255, sc) << 16 | add((p >> 8) & 255, sc) << 8 |
          add(p & 255, sc);
    }
  cairo_surface_mark_dirty(logo);
  return logo;
}

}  // namespace

StashCovers::StashCovers(Stash& stash) : stash_(stash) {}

StashCovers::~StashCovers() {
  for (auto& [key, logo] : logos_)
    if (logo.surface) cairo_surface_destroy(logo.surface);
}

bool StashCovers::changed() {
  if (stash_.generation() == generation_) return false;
  generation_ = stash_.generation();
  return true;
}

const StashScene* StashCovers::scene(const std::string& path) const {
  const int i = sceneIndex(path);
  return i >= 0 ? &catalog()->scenes()[i] : nullptr;
}

int StashCovers::sceneIndex(const std::string& path) const {
  const StashCatalog* c = catalog();
  if (!c || path.empty()) return -1;
  int i = stashSceneAt(*c, path);
  if (i < 0 && offline_)
    if (const Offline::Download* d = offline_->find(path)) {
      if (d->source != path) i = c->sceneByPath(d->source);
      if (i < 0 && !d->oshash.empty()) i = c->sceneByOshash(d->oshash);
    }
  return i;
}

std::string StashCovers::performers(const StashScene& scene) const {
  const StashCatalog* c = catalog();
  std::string names;
  for (const int i : scene.performers) {
    const StashPerformer& p = c->performers()[i];
    if (!stashShown(p.gender, stash_.settings().malePerformers)) continue;
    if (!names.empty()) names += ", ";
    names += p.name;
  }
  return names;
}

std::string StashCovers::studio(const StashScene& scene) const {
  return scene.studio >= 0 ? catalog()->studios()[scene.studio].name : std::string();
}

std::string StashCovers::title(const std::string& path) const {
  if (!titles()) return {};
  const StashScene* s = scene(path);
  return s ? s->title : std::string();
}

cairo_surface_t* StashCovers::cover(const std::string& path, int width) const {
  if (!covers()) return nullptr;
  const StashScene* s = scene(path);
  if (!s || s->cover.empty()) return nullptr;
  return stash_.images().find(Stash::coverRequest(*s, width));
}

bool StashCovers::coverComing(const std::string& path) const {
  if (!covers()) return false;
  const StashScene* s = scene(path);
  // The width does not matter for whether it failed.
  return s && !s->cover.empty() && !stash_.images().failed(Stash::coverRequest(*s, 0));
}

cairo_surface_t* StashCovers::logo(const std::string& path, int w, int h, int& pad,
                                   std::string& name, double top, double bottom, bool& coming) {
  name.clear();
  pad = kShadowPad;
  coming = false;
  const StashScene* s = covers() ? scene(path) : nullptr;
  if (!s || s->studio < 0) return nullptr;
  const StashStudio& studio = catalog()->studios()[s->studio];
  bool none = false;
  cairo_surface_t* surface = studioLogo(studio, w, h, pad, none, top, bottom);
  if (none) name = studio.name;
  coming = !surface && !none;
  return surface;
}

cairo_surface_t* StashCovers::studioLogo(const StashStudio& studio, int w, int h, int& pad,
                                         bool& none, double top, double bottom) {
  pad = kShadowPad;
  const StashImages::Request request = Stash::logoRequest(studio, w);
  none = studio.image.empty() || stash_.images().failed(request);
  if (none) return nullptr;
  const std::string key = studio.image + " " + std::to_string(w) + "x" + std::to_string(h);
  const auto found = logos_.find(key);
  if (found != logos_.end()) {
    found->second.used = ++clock_;
    return found->second.surface;
  }
  cairo_surface_t* source = stash_.images().find(request);
  if (!source) return nullptr;
  if (madeThisFrame_ >= kLogosPerFrame) {
    waitingTop_ = logosWaiting_ ? std::min(waitingTop_, top) : top;
    waitingBottom_ = logosWaiting_ ? std::max(waitingBottom_, bottom) : bottom;
    logosWaiting_ = true;
    return nullptr;
  }
  ++madeThisFrame_;
  if (logos_.size() >= kMaxLogos) {
    auto oldest = logos_.begin();
    for (auto it = logos_.begin(); it != logos_.end(); ++it)
      if (it->second.used < oldest->second.used) oldest = it;
    if (oldest->second.surface) cairo_surface_destroy(oldest->second.surface);
    logos_.erase(oldest);
  }
  Logo& logo = logos_[key];
  logo.surface = shadowedLogo(source, w, h);
  logo.used = ++clock_;
  return logo.surface;
}

void StashCovers::want(const std::vector<std::string>& paths, int coverWidth, size_t shown,
                       int logoWidth) {
  std::vector<StashImages::Request> requests = std::move(first_);
  first_.clear();
  std::set<int> studios;
  auto logos = [&] {
    for (size_t i = 0; i < std::min(shown, paths.size()); ++i) {
      const StashScene* s = scene(paths[i]);
      if (!s || s->studio < 0 || !studios.insert(s->studio).second) continue;
      const StashStudio& studio = catalog()->studios()[s->studio];
      if (!studio.image.empty()) requests.push_back(Stash::logoRequest(studio, logoWidth));
    }
  };
  if (covers()) {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (i == shown) logos();  // after the covers on screen, before those around them
      const StashScene* s = scene(paths[i]);
      if (s && !s->cover.empty()) requests.push_back(Stash::coverRequest(*s, coverWidth));
    }
    if (shown >= paths.size()) logos();
  }
  stash_.images().want(requests);
}

bool StashCovers::matches(const FolderListing::Entry& entry, const std::string& query) const {
  if (query.empty()) return true;
  const StashScene* s = titles() && !entry.directory ? scene(entry.path) : nullptr;
  if (!s) return FolderListing::matches(entry, query);
  FolderListing::Entry all;
  all.directory = true;  // the whole of the name is looked through
  all.name = std::filesystem::path(entry.name).stem().string() + " " + s->title + " " +
             performers(*s) + " " + studio(*s);
  return FolderListing::matches(all, query);
}

// ---- the menu's hooks ----

void Menu::setStash(Stash* stash) {
  stopPreview();
  stashCovers_.reset(stash ? new StashCovers(*stash) : nullptr);
  if (stashCovers_) stashCovers_->setOffline(offline_);
  // The cache fills with the covers as the grid and the list draw them.
  constexpr float scale = menu_geometry::kRasterScale;
  if (stash)
    stash->prefetchCovers(
        {static_cast<int>(kCardPictureW * scale), static_cast<int>(kRowPictureW * scale)});
  for (LibraryItem& item : libraryPage_.items) item.title.clear();
  if (!libraryPage_.items.empty()) sortLibrary();
  arrangeByTitles();
  libraryPage_.listStale = true;
  // The Stash pages: kept for the session, shown while it is on.
  stash_ = stash;
  if (stash)
    StashPages::attach(*this);
  else if (stashListPage())
    showStashSettings();
  else if (page_ == Page::StashSceneDetails)
    showPage(Page::Main);
  dirty_ = true;
}

void Menu::updateStash() {
  StashCovers& stash = *stashCovers_;
  if (stash.changed()) {  // other titles, covers or neither
    if (!libraryPage_.items.empty() && fieldOf(libraryPage_.sort) == kByName) sortLibrary();
    arrangeByTitles();
    libraryPage_.listStale = true;
    dirty_ = true;
  }
  // Logos left out of the last frame: their rows, or all if the list moved.
  double top = 0, bottom = 0;
  if (stash.nextFrame(top, bottom)) {
    if (list_.drawn == list_.offset + list_.fraction)
      browserChanged(top, bottom);
    else
      browserChanged();
  }
  // Drawn in batches, as the frame thumbnails.
  if (stash.stash().imagesArrived() &&
      (page_ == Page::Files || page_ == Page::Library || page_ == Page::Offline || stashPage()))
    thumbnailsPending_ = true;
}

bool Menu::stashCoversShown() const { return stashCovers_ && stashCovers_->covers(); }

std::string StashPages::playingTitle(const Menu& m, const std::string& fallback) {
  const std::string title = m.stashCovers_ ? m.stashCovers_->title(m.view_.currentFile) : "";
  return title.empty() ? fallback : title;
}

std::string Menu::describeFromStash(Button& b, const std::string& path) const {
  const StashCovers& stash = *stashCovers_;
  const StashScene* s = stash.titles() ? stash.scene(path) : nullptr;
  if (!s) return {};
  const std::string performers = stash.performers(*s);
  // A tile with its studio's mark on the picture doesn't name it again.
  b.byline = b.card && stash.covers() ? performers : joined(performers, stash.studio(*s));
  if (s->title.empty()) return {};
  std::string fileName = std::move(b.label);
  b.label = s->title;
  return fileName;
}

void Menu::wantStashImages(const std::vector<std::string>& paths, size_t shown, double pictureW) {
  constexpr float scale = menu_geometry::kRasterScale;
  stashCovers_->want(paths, static_cast<int>(std::ceil(pictureW * scale)), shown,
                     static_cast<int>(std::lround(pictureW * kLogoWidth * scale)));
}

void Menu::drawStudioMark(cairo_t* cr, const std::string& path, double x, double y, double w,
                          double h) const {
  constexpr float scale = menu_geometry::kRasterScale;
  int pad = 0;
  std::string name;
  bool coming = false;
  cairo_surface_t* logo =
      stashCovers_->logo(path, static_cast<int>(std::lround(w * kLogoWidth * scale)),
                         static_cast<int>(std::lround(h * kLogoHeight * scale)), pad, name, y,
                         y + h * kLogoHeight + 2 * kLogoInset, coming);
  const double right = x + w - kLogoInset, top = y + kLogoInset;
  // No logo: the studio's name, as the play count is shown.
  constexpr int nameSize = 22;
  constexpr double nameH = 40;
  const double nameW = name.empty() ? 0 : std::min(textWidth(name, nameSize, true) + 28, w * 0.6);
  if (coming) {
    logosComing_.insert(path);
  } else if (const auto it = logosComing_.find(path); it != logosComing_.end()) {
    // As a picture (pictureOf()), a logo that came, or the name in place of
    // one that failed, is shown once a paint reached all of it; until then
    // picturesChanged() redraws the tile.
    const double cornerW = w * kLogoWidth + kLogoInset + kShadowPad / scale;
    if (logo ? paintedPart(cr, x + w - cornerW, y, cornerW, h * kLogoHeight + 2 * kLogoInset) ==
                   Painted::All
             : name.empty() || paintedPart(cr, right - nameW, top, nameW, nameH) == Painted::All)
      logosComing_.erase(it);
  }
  if (logo) {
    // At its pixels' size, on the texture's pixel grid: as sharp as it gets.
    const double lw = cairo_image_surface_get_width(logo);
    cairo_save(cr);
    cairo_translate(cr, onPixel(right - (lw - pad) / scale), onPixel(top - pad / scale));
    cairo_scale(cr, 1 / scale, 1 / scale);
    cairo_set_source_surface(cr, logo, 0, 0);
    cairo_paint_with_alpha(cr, kLogoOpacity);
    cairo_restore(cr);
  } else if (!name.empty()) {
    roundedRect(cr, right - nameW, top, nameW, nameH, 14);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    drawText(cr, name, right - nameW + 14, top, nameW - 28, nameH, nameSize, true, false, 1, 1, 1,
             0.9);
  }
}

// Sorted by name, videos with Stash's title are sorted by it, among the
// others by their file's name; folders stay first, as they were.
void Menu::arrangeByTitles() {
  if (!stashCovers_ || fieldOf(files_.sort) != kByName || files_.kind != FileKind::Video) return;
  const std::vector<FolderListing::Entry>& entries = files_.listing.entries();
  std::vector<std::string> keys(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string name = entries[i].directory ? "" : stashCovers_->title(entries[i].path);
    keys[i] = name.empty() ? entries[i].nameKey : fileNameSortKey(foldFileName(name));
  }
  const bool down = files_.sort == FolderListing::Order::NameDown;
  std::vector<size_t> order(entries.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (entries[a].directory || entries[b].directory)
      return entries[a].directory && !entries[b].directory;
    if (keys[a] != keys[b])
      return down ? keys[b] < keys[a] : keys[a] < keys[b];
    return down ? entries[b].nameKey < entries[a].nameKey
                : entries[a].nameKey < entries[b].nameKey;
  });
  files_.listing.arrange(order);
  if (files_.records.size() != order.size()) return;
  std::vector<VideoRecord> records;
  records.reserve(order.size());
  for (const size_t i : order) records.push_back(files_.records[i]);
  files_.records = std::move(records);
}
