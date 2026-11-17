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
#include <cstdlib>
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

namespace {

bool hasVideoExtension(const fs::path& p) {
  return hasExtension(p, {".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v", ".ts", ".m2ts", ".wmv",
                          ".flv", ".mpg", ".mpeg", ".iso"});
}

}  // namespace

bool isVideoFile(const fs::path& p) { return hasVideoExtension(p) || isPartialDownload(p); }

// Not a disc image: it plays only once it is complete.
bool isPartialDownload(const fs::path& p) {
  return hasExtension(p, {".part"}) && hasVideoExtension(p.stem()) && !hasExtension(p.stem(), {".iso"});
}

bool subtitleBelongsTo(const std::string& video, const std::string& subtitle) {
  const fs::path name = isPartialDownload(video) ? fs::path(video).stem() : fs::path(video);
  const std::string base = foldFileName(name.stem().string());
  const std::string other = foldFileName(fs::path(subtitle).stem().string());
  return other == base || (other.size() > base.size() + 1 && other[base.size()] == '.' &&
                           other.compare(0, base.size(), base) == 0);
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

std::string neighborIn(const std::vector<std::string>& list, const std::string& current, int step,
                       const PlayOrder& order) {
  if (!order.shuffle) {
    const long count = static_cast<long>(list.size());
    long i = std::find(list.begin(), list.end(), current) - list.begin();
    if (i == count) return {};
    // An entry listed again (two Stash scenes of one file) counts once, where it is first.
    for (long left = std::abs(step); left > 0;) {
      i += step < 0 ? -1 : 1;
      if (i < 0 || i >= count) return {};
      if (std::find(list.begin(), list.begin() + i, list[i]) == list.begin() + i) --left;
    }
    return list[i];
  }
  // Ordered by a hash of each entry with the seed: entries that come or go
  // leave the order of the others as it was. The start keeps its place when
  // it is gone from the list (deleted meanwhile), so the round still ends there.
  const bool startGone = std::find(list.begin(), list.end(), order.start) == list.end();
  if (startGone && current == order.start) return {};
  std::vector<std::pair<uint64_t, const std::string*>> shuffled;
  shuffled.reserve(list.size() + 1);
  auto add = [&](const std::string& entry) {
    uint64_t h = std::hash<std::string>{}(entry) ^ order.shuffle;  // mixed as splitmix64 does
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    shuffled.emplace_back(h ^ (h >> 31), &entry);
  };
  for (const std::string& entry : list) add(entry);
  add(order.start);
  std::sort(shuffled.begin(), shuffled.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : *a.second < *b.second;
  });
  // An entry listed again counts once.
  shuffled.erase(std::unique(shuffled.begin(), shuffled.end(),
                             [](const auto& a, const auto& b) { return *a.second == *b.second; }),
                 shuffled.end());
  const auto at = std::find_if(shuffled.begin(), shuffled.end(),
                               [&](const auto& e) { return *e.second == current; });
  if (at == shuffled.end()) return {};
  const long count = static_cast<long>(shuffled.size());
  long i = at - shuffled.begin();
  for (; step > 0; --step) {
    i = (i + 1) % count;
    if (*shuffled[i].second == order.start) return {};
  }
  for (; step < 0; ++step) {
    if (*shuffled[i].second == order.start) return {};
    i = (i + count - 1) % count;
  }
  const std::string& found = *shuffled[i].second;
  return found == current || (startGone && found == order.start) ? std::string() : found;
}

std::future<std::string> mediaNeighbor(const std::string& current, int step, bool wrap,
                                       PlayOrder order) {
  std::packaged_task<std::string()> task([current, step, wrap, order = std::move(order),
                                          cache = mediaCache]() mutable -> std::string {
    const fs::path path(current);
    const std::string folder = remote::isRemote(current) ? remote::parent(current)
                                                : path.parent_path().string();
    const std::string prefix = folder + (!folder.empty() && folder.back() != '/' ? "/" : "");
    // The listing has names: the start as one, if it is this folder's.
    order.start = order.start.compare(0, prefix.size(), prefix) == 0
                      ? order.start.substr(prefix.size())
                      : std::string();
    auto pick = [&](const std::vector<std::string>& names) {
      const std::string name = neighborIn(names, path.filename().string(), step, order);
      if (!name.empty()) return prefix + name;
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

namespace {

bool isLink(const std::string& s) {
  const std::string lower = lowerCase(s.substr(0, 8));
  return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

std::string percentDecoded(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i] == '+' ? ' ' : s[i];
    }
  }
  return out;
}

// The query's parameters, names lowercase, values still encoded.
std::vector<std::pair<std::string, std::string>> queryOf(const std::string& link) {
  std::vector<std::pair<std::string, std::string>> params;
  const size_t q = link.find('?');
  if (q == std::string::npos) return params;
  const std::string query = link.substr(q + 1, link.find('#', q) - q - 1);
  for (size_t start = 0; start <= query.size();) {
    const size_t end = std::min(query.find('&', start), query.size());
    const std::string part = query.substr(start, end - start);
    const size_t eq = part.find('=');
    params.emplace_back(lowerCase(part.substr(0, eq)), eq == std::string::npos ? "" : part.substr(eq + 1));
    start = end + 1;
  }
  return params;
}

// filename*=UTF-8''name or filename="name" of a Content-Disposition.
std::string dispositionName(const std::string& d) {
  const std::string lower = lowerCase(d);
  if (size_t at = lower.find("filename*="); at != std::string::npos) {
    std::string v = d.substr(at + 10, d.find(';', at) - at - 10);
    if (const size_t quotes = v.find("''"); quotes != std::string::npos) v = v.substr(quotes + 2);
    return percentDecoded(v);
  }
  if (size_t at = lower.find("filename="); at != std::string::npos) {
    std::string v = d.substr(at + 9);
    if (!v.empty() && v[0] == '"') return v.substr(1, v.find('"', 1) - 1);
    return v.substr(0, v.find(';'));
  }
  return {};
}

}  // namespace

std::string linkInText(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const std::string link = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  return isLink(link) && link.find_first_of(" \t") == std::string::npos ? link : std::string();
}

std::string linkFileName(const std::string& location) {
  if (!isLink(location)) return {};
  for (const auto& [name, value] : queryOf(location))
    if (name == "response-content-disposition")
      if (std::string file = dispositionName(percentDecoded(value)); !file.empty())
        return file.substr(file.find_last_of("/\\") + 1);
  const std::string path = location.substr(0, location.find_first_of("?#"));
  const size_t slash = path.find_last_of('/');
  return percentDecoded(slash == std::string::npos ? path : path.substr(slash + 1));
}

std::string linkIdentity(const std::string& location) {
  if (!isLink(location)) return location;
  for (const auto& [name, value] : queryOf(location))
    if (name == "signature" || name == "x-amz-signature" || name == "x-goog-signature")
      return location.substr(0, location.find_first_of("?#"));
  return location;
}
