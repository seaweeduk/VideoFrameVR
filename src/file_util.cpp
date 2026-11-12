#include "file_util.h"

#include "stash_text.h"
#include "remote.h"

#include <glib.h>
#include <langinfo.h>
#include <locale.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

bool hasExtension(const fs::path& p, std::initializer_list<const char*> extensions) {
  const std::string ext = lowerCase(p.extension().string());
  return std::any_of(extensions.begin(), extensions.end(), [&](const char* e) { return ext == e; });
}

}  // namespace

std::string lowerCase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool isVideoFile(const fs::path& p) {
  return hasExtension(p, {".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".ts", ".m2ts", ".wmv",
                          ".flv", ".mpg", ".mpeg", ".iso"});
}

bool isPhotoFile(const fs::path& p) {
  return hasExtension(p, {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".gif", ".tif", ".tiff",
                          ".avif", ".heic", ".jxl"});
}

bool isSubtitleFile(const fs::path& p) {
  return hasExtension(p, {".srt", ".ass", ".ssa", ".vtt", ".sub", ".sup"});
}

bool fileNameLess(const std::string& a, const std::string& b) {
  return lowerCase(a) < lowerCase(b);
}

bool binOrLost(const std::string& name) {
  const std::string lower = lowerCase(name);
  return lower == "lost+found" || lower == ".trash" || lower.rfind(".trash-", 0) == 0 ||
         lower == ".trashes" || lower == "$recycle.bin" || lower == "recycler" || lower == "recycled" ||
         lower == ".recycle" || lower == "#recycle" || lower == "@recycle";
}

bool validUtf8(const std::string& s) {
  return g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr);
}

std::string foldFileName(const std::string& name) {
  if (validUtf8(name)) return stashFold(name);
  std::string folded = name;
  for (char& c : folded)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return folded;
}

std::string fileNameSortKey(const std::string& folded) {
  if (!validUtf8(folded)) return folded;
  // Use the user's collation without changing the render thread's locale.
  static const locale_t locale = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, "", nullptr);
  static const std::string collation = locale ? nl_langinfo_l(_NL_LOCALE_NAME(LC_COLLATE), locale) : "C";
  static const bool byteOrder = collation == "POSIX" || collation == "C" ||
                               collation.compare(0, 2, "C.") == 0;
  const locale_t previous = locale ? uselocale(locale) : nullptr;
  std::string result;
  size_t start = 0;
  for (;;) {
    const size_t end = std::min(folded.find('.', start), folded.size());
    // Keep extensions separate, so ep1.mp4 precedes ep10.mp4. Unlike
    // g_utf8_collate_key_for_filename(), digits retain their lexical order.
    // C collation has no alphabetic weights: decomposing puts é with e.
    gchar* key = byteOrder ? g_utf8_normalize(folded.data() + start, end - start, G_NORMALIZE_DEFAULT)
                           : g_utf8_collate_key(folded.data() + start, end - start);
    result += key;
    g_free(key);
    if (end == folded.size()) break;
    result += '\0';
    start = end + 1;
  }
  if (locale) uselocale(previous);
  return result;
}

namespace {

// The last listing, reused for a few seconds: a slideshow asks for the next
// photo every few seconds, and listing a big folder on a network share takes
// a noticeable moment.
struct Listing {
  std::string folder;
  bool photos = false;
  std::chrono::steady_clock::time_point when;
  std::vector<std::string> names;
};
struct ListingCache {
  std::mutex mutex;
  Listing listing;
  unsigned generation = 0;
};
const auto mediaCache = std::make_shared<ListingCache>();

// The videos (or photos) in the folder of `current`, in file browser order.
std::vector<std::string> sameKindNames(const std::string& current, ListingCache& shared) {
  const bool photos = isPhotoFile(current);
  const std::string folder = remote::isRemote(current) ? remote::parent(current)
                                                : fs::path(current).parent_path().string();
  const auto now = std::chrono::steady_clock::now();
  unsigned generation;
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    generation = shared.generation;
    const Listing& cache = shared.listing;
    if (cache.folder == folder && cache.photos == photos &&
        now - cache.when < std::chrono::seconds(10))
      return cache.names;
  }
  std::vector<std::string> names;
  std::error_code ec;
  if (remote::isRemote(current)) {
    std::vector<remote::Entry> entries;
    if (remote::list(folder, entries) != remote::Status::Ok) return {};
    for (const auto& item : entries)
      if (!item.folder && !item.name.empty() && item.name[0] != '.' &&
          (photos ? isPhotoFile(item.name) : isVideoFile(item.name))) names.push_back(item.name);
  } else for (fs::directory_iterator it(folder, ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto& item = *it;
    std::error_code typeError;
    const std::string name = item.path().filename().string();
    if (!name.empty() && name[0] != '.' && item.is_regular_file(typeError) &&
        (photos ? isPhotoFile(item.path()) : isVideoFile(item.path())))
      names.push_back(name);
  }
  if (ec) return {};
  std::vector<std::pair<std::string, std::string>> keyed;
  keyed.reserve(names.size());
  for (std::string& name : names)
    keyed.emplace_back(fileNameSortKey(foldFileName(name)), std::move(name));
  std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  names.clear();
  for (auto& item : keyed) names.push_back(std::move(item.second));
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (generation == shared.generation)
      shared.listing = {folder, photos, now, names};
  }
  return names;
}

}  // namespace

void forgetFolderListing() {
  std::lock_guard<std::mutex> lock(mediaCache->mutex);
  mediaCache->listing = Listing();
  ++mediaCache->generation;
}

std::future<std::string> mediaNeighbor(const std::string& current, int step, bool wrap) {
  std::packaged_task<std::string()> task([current, step, wrap, cache = mediaCache]() -> std::string {
    const fs::path path(current);
    const std::string folder = remote::isRemote(current) ? remote::parent(current)
                                                : path.parent_path().string();
    const std::string prefix = folder + (!folder.empty() && folder.back() != '/' ? "/" : "");
    auto pick = [&](const std::vector<std::string>& names) {
      const auto it = std::find(names.begin(), names.end(), path.filename().string());
      const long index = std::distance(names.begin(), it) + step;
      if (it != names.end() && index >= 0 && index < static_cast<long>(names.size()))
        return prefix + names[index];
      return wrap && !names.empty() ? prefix + names.front() : std::string();
    };
    std::string next = pick(sameKindNames(current, *cache));
    // The listing can be 10 s old: a local file deleted or moved since then
    // means listing the folder again.
    std::error_code ec;
    if (!next.empty() && !remote::isRemote(next) && !fs::exists(next, ec)) {
      {
        std::lock_guard<std::mutex> lock(cache->mutex);
        cache->listing = Listing();
        ++cache->generation;
      }
      next = pick(sameKindNames(current, *cache));
    }
    return next;
  });
  auto result = task.get_future();
  std::thread(std::move(task)).detach();
  return result;
}

void queueConfigWork(std::function<void()> work) {
  struct Writer {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool quit = false;
    std::thread worker{[this] {
      while (true) {
        std::function<void()> job;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [&] { return quit || !jobs.empty(); });
          if (jobs.empty()) return;
          job = std::move(jobs.front());
          jobs.pop_front();
        }
        job();
      }
    }};
    ~Writer() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
      }
      wake.notify_one();
      worker.join();
    }
  };
  static Writer writer;
  {
    std::lock_guard<std::mutex> lock(writer.mutex);
    writer.jobs.push_back(std::move(work));
  }
  writer.wake.notify_one();
}

bool drainConfigWork(double seconds) {
  auto done = std::make_shared<std::promise<void>>();
  auto finished = done->get_future();
  queueConfigWork([done] { done->set_value(); });
  return finished.wait_for(std::chrono::duration<double>(seconds)) == std::future_status::ready;
}
