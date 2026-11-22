#include "stash.h"

#include "app_dirs.h"
#include "library.h"
#include "run_log.h"
#include "stash_store.h"
#include "strings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

double msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

uintmax_t folderSize(const fs::path& dir) {
  std::error_code ec;
  uintmax_t total = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) total += it->file_size(ec);
  return total;
}

// The time one query takes, in microseconds: the median of many.
double timeQuery(const StashCatalog& catalog, const StashFilter& filter, const StashOrder& order,
                 const std::vector<StashHistory>& history, size_t& matches) {
  std::vector<double> times;
  for (int i = 0; i < 101; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const StashResult result = catalog.query(filter, order, &history);
    times.push_back(msSince(start) * 1000);
    matches = result.scenes.size();
  }
  std::nth_element(times.begin(), times.begin() + 50, times.end());
  return times[50];
}

}  // namespace

int runStashSyncDiagnostic(const StashSettings& settings) {
  const char* name = mediaServer(settings.kind).name();
  if (settings.url.empty()) {
    std::printf("%s: %s\n", name, strings().stash.setupHint);
    return 0;
  }
  hideInRunLog(urlHost(settings.url), kHiddenServer);
  std::printf("%s: syncing from %s\n", name, settings.url.c_str());
  // The library's formats and history, if it is free (not open in a
  // running VideoFrameVR).
  Library library;
  library.load();
  const StashLibraryFormats formats = library.formatsByHash();

  StashStatus status;
  const auto start = std::chrono::steady_clock::now();
  const auto catalog = stashSyncOnce(settings, formats, status);
  const double totalMs = msSince(start);
  if (!catalog) {
    std::printf("%s: sync failed: %s\n", name, status.error.c_str());
    return 1;
  }
  if (!status.error.empty()) std::printf("%s: %s\n", name, status.error.c_str());
  const StashCatalogStats& stats = catalog->stats();

  std::printf("Server: %s %s\n", name, catalog->version().c_str());
  std::printf("Scenes: %d on the server, %d with a file here, %d from its stream, %d not listed\n",
              stats.stashScenes, stats.listed, stats.streamed,
              stats.stashScenes - stats.listed - stats.streamed);
  for (const StashCatalogFolder& f : catalog->folders()) {
    const std::string here = f.folder.empty() ? "" : settings.localPath(f.folder);
    std::printf("  %s: %d scenes, %d here, %d from its stream%s%s\n",
                f.folder.empty() ? "in none of the server's folders" : f.folder.c_str(), f.scenes,
                f.listed, f.streamed, here.empty() ? "" : ", at ", here.c_str());
  }
  std::printf("  VR %d (own format %d, frame %d, name %d, the server's tag %d), 2D %d\n", stats.vr,
              stats.vrBy[1], stats.vrBy[2], stats.vrBy[3], stats.vrBy[4], stats.listed + stats.streamed - stats.vr);
  int covers = 0;
  for (const StashScene& s : catalog->scenes()) covers += !s.cover.empty();
  std::printf("  with a cover %d\n", covers);
  int shown = 0, images = 0;
  for (const StashPerformer& p : catalog->performers()) {
    shown += stashShown(p.gender, false);
    images += !p.image.empty();
  }
  std::printf("Performers: %zu (%d shown without the male performers setting, %d with an image)\n",
              catalog->performers().size(), shown, images);
  int logos = 0, withScenes = 0;
  for (const StashStudio& s : catalog->studios()) {
    logos += !s.image.empty();
    withScenes += s.scenes > 0;
  }
  std::printf("Studios: %zu (%d with scenes here, %d with a logo)\n", catalog->studios().size(),
              withScenes, logos);
  int mergedGroups = 0;
  for (const StashTag& t : catalog->tags()) mergedGroups += t.ids.size() > 1;
  std::printf("Tags: %d on the server, %zu after merging duplicates (%d merged from 2 or more)\n",
              stats.stashTags, catalog->tags().size(), mergedGroups);
  std::printf("Groups: %zu\n", catalog->groups().size());
  if (library.available())
    std::printf("Library: %d fingerprinted videos, %d of them on the server\n", stats.libraryVideos,
                stats.libraryMatched);
  else
    std::printf("Library: not available (in use?): no own formats, no history\n");

  std::printf("Timings: fetch %.0f ms (%.1f MB), parse %.0f ms, store %.0f ms, files %.0f ms, "
              "catalog %.0f ms; %.0f ms in all\n",
              status.fetchMs, status.bytes / 1e6, status.parseMs, status.storeMs, stats.checkMs,
              stats.buildMs, totalMs);
  {
    const auto loadStart = std::chrono::steady_clock::now();
    StashStore store(mediaServer(settings.kind));
    StashData data;
    if (store.load(data)) {
      const double loadMs = msSince(loadStart);
      const StashCatalogStats copy = StashCatalog::build(data, settings, formats)->stats();
      std::printf("  from the local copy (at start): load %.0f ms, files %.0f ms, catalog %.0f ms\n",
                  loadMs, copy.checkMs, copy.buildMs);
    }
  }

  // Queries over every scene, with the facets.
  const std::vector<StashHistory> history = historyOf(*catalog, library);
  int topTag = -1, topPerformer = -1, topHair = -1;
  {
    StashFilter vr;
    vr.format = StashFormat::VR;
    const StashResult r = catalog->query(vr, {}, &history);
    if (!r.tags.empty()) topTag = r.tags.front().index;
    if (!r.performers.empty()) topPerformer = r.performers.front().index;
    const auto& hair = r.looks[static_cast<size_t>(StashLookKind::Hair)];
    if (!hair.empty()) topHair = hair.front().index;
  }
  struct Case {
    const char* name;
    StashFilter filter;
    StashOrder order;
  };
  std::vector<Case> cases;
  cases.push_back({"all scenes, by release date", {}, {}});
  {
    Case c{"VR, by title", {}, {StashSort::Title, false, 0}};
    c.filter.format = StashFormat::VR;
    cases.push_back(c);
  }
  {
    Case c{"VR, top performer and tag, by rating", {}, {StashSort::Rating, true, 0}};
    c.filter.format = StashFormat::VR;
    if (topPerformer >= 0) c.filter.performers.include = {topPerformer};
    if (topTag >= 0) c.filter.tags.include = {topTag};
    cases.push_back(c);
  }
  {
    Case c{"looks, cast, resolution, history, random", {}, {StashSort::Random, true, 42}};
    if (topHair >= 0) c.filter.looks[static_cast<size_t>(StashLookKind::Hair)].include = {topHair};
    c.filter.looks[static_cast<size_t>(StashLookKind::Cup)].include = {
        static_cast<int>(StashCup::C), static_cast<int>(StashCup::D)};
    c.filter.women = stashBit(StashWomen::One);
    c.filter.men = stashBit(StashMen::None) | stashBit(StashMen::One);
    c.filter.resolutions = stashBit(StashResolution::VR8K) | stashBit(StashResolution::VR7K) |
                           stashBit(StashResolution::VR6K) | stashBit(StashResolution::VR5K);
    c.filter.history = stashBit(StashHistoryOption::Unplayed);
    c.filter.addedSince = {static_cast<int64_t>(std::time(nullptr)) - 30 * 86400};
    cases.push_back(c);
  }
  {
    Case c{"text search \"an\", tags any of, by plays", {}, {StashSort::Plays, true, 0}};
    c.filter.text = "an";
    c.filter.tags.all = false;
    for (int i = 0; i < 3 && i < static_cast<int>(catalog->tags().size()); ++i)
      c.filter.tags.include.push_back(i);
    cases.push_back(c);
  }
  std::printf("Queries over %zu scenes (filter, facets and order; median of 101):\n",
              catalog->scenes().size());
  for (const Case& c : cases) {
    size_t matches = 0;
    const double us = timeQuery(*catalog, c.filter, c.order, history, matches);
    std::printf("  %-44s %5zu scenes  %6.0f us\n", c.name, matches, us);
  }
  {
    const auto historyStart = std::chrono::steady_clock::now();
    historyOf(*catalog, library);
    std::printf("  the library's history of every scene: %.2f ms\n", msSince(historyStart));
  }

  std::error_code ec;
  const fs::path copy = StashStore::path(mediaServer(settings.kind));
  std::printf("Files: %s %.1f MB", copy.filename().c_str(), fs::file_size(copy, ec) / 1e6);
  const fs::path imageCache = cacheDir() / mediaServer(settings.kind).id();
  if (fs::is_directory(imageCache, ec))
    std::printf(", image cache %.0f MB\n", folderSize(imageCache) / 1e6);
  else
    std::printf(", no image cache\n");
  return 0;
}
