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
    cairo_paint(cr);
    cairo_destroy(cr);
  }
  cairo_surface_flush(logo);
  auto* pixels = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(logo));
  const int stride = cairo_image_surface_get_stride(logo) / 4;
  // How light it is, weighted by coverage (premultiplied: the colour is
  // already scaled by it).
  double light = 0, cover = 0;
  std::vector<float> alpha(static_cast<size_t>(w) * h), blurred(alpha.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint32_t p = pixels[static_cast<size_t>(y) * stride + x];
      const float a = (p >> 24) / 255.0f;
      alpha[static_cast<size_t>(y) * w + x] = a;
      light += 0.2126 * ((p >> 16) & 255) + 0.7152 * ((p >> 8) & 255) + 0.0722 * (p & 255);
      cover += p >> 24;
    }
  const bool dark = cover > 0 && light / cover < 0.3;
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
  for (LibraryItem& item : libraryItems_) item.title.clear();
  if (!libraryItems_.empty()) sortLibrary();
  arrangeByTitles();
  libraryListStale_ = true;
  // The Stash pages: kept for the session, shown while it is on.
  stash_ = stash;
  if (stash)
    StashPages::attach(*this);
  else if (stashListPage())
    showPage(Page::Settings);
  else if (page_ == Page::StashSceneDetails)
    showPage(Page::Main);
  dirty_ = true;
}

void Menu::updateStash() {
  StashCovers& stash = *stashCovers_;
  if (stash.changed()) {  // other titles, covers or neither
    if (!libraryItems_.empty() && fieldOf(librarySort_) == kByName) sortLibrary();
    arrangeByTitles();
    libraryListStale_ = true;
    dirty_ = true;
  }
  // Logos left out of the last frame: their rows, or all if the list moved.
  double top = 0, bottom = 0;
  if (stash.nextFrame(top, bottom)) {
    if (drawnScroll_ == scrollOffset_ + scrollFraction_)
      browserChanged(top, bottom);
    else
      browserChanged();
  }
  // Drawn in batches, as the frame thumbnails.
  if (stash.stash().imagesArrived() &&
      (page_ == Page::Files || page_ == Page::Library || stashPage()))
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
  if (coming) {
    logosComing_.insert(path);
  } else if (const auto it = logosComing_.find(path); it != logosComing_.end()) {
    // As a picture (pictureOf()), a logo that came is shown once a paint
    // reached all of its corner; until then picturesChanged() redraws the tile.
    const double cornerW = w * kLogoWidth + kLogoInset + kShadowPad / scale;
    if (!logo || paintedPart(cr, x + w - cornerW, y, cornerW, h * kLogoHeight + 2 * kLogoInset) ==
                     Painted::All)
      logosComing_.erase(it);
  }
  const double right = x + w - kLogoInset, top = y + kLogoInset;
  if (logo) {
    // At its pixels' size, on the texture's pixel grid: as sharp as it gets.
    const double lw = cairo_image_surface_get_width(logo);
    cairo_save(cr);
    cairo_translate(cr, onPixel(right - (lw - pad) / scale), onPixel(top - pad / scale));
    cairo_scale(cr, 1 / scale, 1 / scale);
    cairo_set_source_surface(cr, logo, 0, 0);
    cairo_paint_with_alpha(cr, kLogoOpacity);
    cairo_restore(cr);
  } else if (!name.empty()) {  // no logo: the name, as the play count is shown
    constexpr int size = 22;
    constexpr double ph = 40;
    const double pw = std::min(textWidth(name, size, true) + 28, w * 0.6);
    roundedRect(cr, right - pw, top, pw, ph, 14);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    drawText(cr, name, right - pw + 14, top, pw - 28, ph, size, true, false, 1, 1, 1, 0.9);
  }
}

// Sorted by name, videos with Stash's title are sorted by it, among the
// others by their file's name; folders stay first, as they were.
void Menu::arrangeByTitles() {
  if (!stashCovers_ || fieldOf(sortMode_) != kByName || fileKind_ != FileKind::Video) return;
  const std::vector<FolderListing::Entry>& entries = listing_.entries();
  std::vector<std::string> keys(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string name = entries[i].directory ? "" : stashCovers_->title(entries[i].path);
    keys[i] = name.empty() ? entries[i].nameKey : fileNameSortKey(foldFileName(name));
  }
  const bool down = sortMode_ == FolderListing::Order::NameDown;
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
  listing_.arrange(order);
  if (records_.size() != order.size()) return;
  std::vector<VideoRecord> records;
  records.reserve(order.size());
  for (const size_t i : order) records.push_back(records_[i]);
  records_ = std::move(records);
}
