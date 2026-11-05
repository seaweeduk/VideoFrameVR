#include "places.h"

#include "file_util.h"
#include "removable_drives.h"
#include "settings.h"
#include "remote.h"

#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

bool isDirectory(const std::string& path) {
  if (remote::isRemote(path)) return true;
  std::error_code ec;
  return !path.empty() && fs::is_directory(path, ec);
}

// A folder's name, with its parent's when it is hidden ("raid/.p").
std::string labelFor(const fs::path& path) {
  const std::string name = path.filename().string();
  if (name.empty()) return path.string();
  if (name[0] == '.') return path.parent_path().filename().string() + "/" + name;
  return name;
}

// pinned_folders.conf: a pin is its path on a line, as older versions wrote
// and still read. Lines they skip keep the rest: "place\t<path>" a place
// other than a pin, at its position in the order; "hide\t<path>" and
// "name\t<name>\t<path>".
std::vector<Places::Entry> readEntries() {
  std::vector<Places::Entry> entries;
  std::vector<std::string> hidden;
  std::vector<std::pair<std::string, std::string>> names;  // path, name
  std::ifstream in(configDir() / "pinned_folders.conf");
  for (std::string line; std::getline(in, line);) {
    std::string path = line;
    bool pinned = true;
    if (line.rfind("place\t", 0) == 0) {
      path = line.substr(6);
      pinned = false;
    } else if (line.rfind("hide\t", 0) == 0) {
      hidden.push_back(line.substr(5));
      continue;
    } else if (line.rfind("name\t", 0) == 0) {
      const size_t tab = line.find('\t', 5);
      if (tab != std::string::npos) names.emplace_back(line.substr(tab + 1), line.substr(5, tab - 5));
      continue;
    }
    if (!remote::isRemote(path)) {
      if (!fs::path(path).is_absolute()) continue;
      path = fs::path(path).lexically_normal().string();
    }
    if (std::any_of(entries.begin(), entries.end(),
                    [&](const Places::Entry& e) { return e.path == path; }))
      continue;
    entries.push_back({path, pinned});
  }
  for (Places::Entry& e : entries) {
    e.hidden = std::find(hidden.begin(), hidden.end(), e.path) != hidden.end();
    for (const auto& [path, name] : names)
      if (path == e.path) e.name = name;
  }
  return entries;
}

// The places other than pins, in their default order.
std::vector<Places::Place> lookAround(const std::string& library, const Places::Names& names) {
  std::vector<Places::Place> places;
  auto add = [&](const std::string& label, const std::string& path) {
    if (!isDirectory(path)) return;
    if (std::any_of(places.begin(), places.end(),
                    [&](const Places::Place& p) { return p.path == path; }))
      return;
    Places::Place place;
    place.label = place.ownName = label;
    place.path = path;
    places.push_back(place);
  };
  const char* home = std::getenv("HOME");
  const std::string homeDir = home ? home : "";
  add(names.home, homeDir);
  const char* videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
  add(names.videos, videos ? videos : homeDir + "/Videos");
  add(labelFor(library), library);
  // microSD cards and USB drives, by their labels (else what they are).
  for (const RemovableDrives::Drive& d : mountedDrives()) {
    std::string label = d.label;
    if (label.empty() && d.kind == RemovableDrives::Kind::Card) label = names.card;
    if (label.empty() && d.kind == RemovableDrives::Kind::Usb) label = names.usbDrive;
    add(label.empty() ? names.drive : label, d.mountPoint);
  }
  // Other mounted drives: what is in the usual mount folders.
  const char* user = std::getenv("USER");
  std::vector<std::string> roots = {"/mnt"};
  if (user) roots.push_back(std::string("/media/") + user);
  for (const std::string& root : roots) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
      found.push_back(it->path());
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
      return fileNameLess(a.filename().string(), b.filename().string());
    });
    for (const fs::path& p : found) add(p.filename().string(), p.string());
  }
  add(names.fileSystem, "/");
  return places;
}

}  // namespace

struct Places::Pending {
  std::mutex mutex;
  bool done = false;
  bool read = false;  // the file, on the first look; after it entries_ is the one kept
  std::vector<Entry> entries;
  std::vector<Place> found;
  std::map<std::string, bool> available;
};

void Places::refresh(const std::string& library, const Names& names) {
  auto job = std::make_shared<Pending>();
  job->read = !loaded_;
  pending_ = job;
  std::vector<std::string> pins;
  for (const Entry& e : entries_)
    if (e.pinned) pins.push_back(e.path);
  std::thread([job, library, names, pins, saved = saved_]() mutable {
    if (saved.valid()) saved.wait();
    std::vector<Entry> entries;
    if (job->read) {
      entries = readEntries();
      for (const Entry& e : entries)
        if (e.pinned) pins.push_back(e.path);
    }
    std::map<std::string, bool> available;
    for (const std::string& pin : pins) available[pin] = isDirectory(pin);
    std::vector<Place> found = lookAround(library, names);
    std::lock_guard<std::mutex> lock(job->mutex);
    job->entries = std::move(entries);
    job->found = std::move(found);
    job->available = std::move(available);
    job->done = true;
  }).detach();
}

bool Places::poll() {
  if (!pending_) return false;
  const std::shared_ptr<Pending> job = pending_;
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (!job->done) return false;
    if (job->read) entries_ = std::move(job->entries);
    found_ = std::move(job->found);
    available_ = std::move(job->available);
    loaded_ = true;
  }
  pending_.reset();
  arrange();
  return true;
}

Places::~Places() {
  if (saved_.valid()) saved_.wait();
}

void Places::arrange() {
  auto at = [&](const std::string& path) {
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].path == path) return static_cast<int>(i);
    return -1;
  };
  for (size_t i = 0; i < found_.size(); ++i) {
    if (at(found_[i].path) >= 0) continue;
    size_t to = entries_.size();
    for (size_t j = i; j-- > 0;)
      if (const int k = at(found_[j].path); k >= 0) {
        to = k + 1;
        break;
      }
    entries_.insert(entries_.begin() + to, Entry{found_[i].path});
  }
  places_.clear();
  for (const Entry& e : entries_) {
    Place p;
    if (e.pinned) {
      p.path = e.path;
      p.pinned = true;
      p.ownName = labelFor(e.path);
      const auto known = available_.find(e.path);
      p.available = known == available_.end() || known->second;
    } else {
      const auto f = std::find_if(found_.begin(), found_.end(),
                                  [&](const Place& q) { return q.path == e.path; });
      if (f == found_.end()) continue;  // a drive not there now
      p = *f;
    }
    p.hidden = e.hidden;
    p.label = e.name.empty() ? p.ownName : e.name;
    places_.push_back(std::move(p));
  }
}

bool Places::pinned(const std::string& path) const {
  return std::any_of(places_.begin(), places_.end(),
                     [&](const Place& p) { return p.pinned && p.path == path; });
}

void Places::togglePin(const std::string& path) {
  if (!loaded_ || path.find('\n') != std::string::npos) return;
  const bool remove = pinned(path);
  // A place's name and whether it shows go with it.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.path == path; }),
                 entries_.end());
  if (!remove) entries_.insert(entries_.begin(), Entry{path, true});
  arrange();
  save();
}

void Places::move(const std::string& path, int by) {
  auto there = [&](const Entry& e) {
    return e.pinned || std::any_of(found_.begin(), found_.end(),
                                   [&](const Place& f) { return f.path == e.path; });
  };
  const int n = static_cast<int>(entries_.size());
  for (int i = 0; i < n; ++i) {
    if (entries_[i].path != path) continue;
    // Past the drives not there now, which keep their places.
    for (int j = i + by; j >= 0 && j < n; j += by)
      if (there(entries_[j])) {
        std::swap(entries_[i], entries_[j]);
        arrange();
        save();
        return;
      }
    return;
  }
}

void Places::setHidden(const std::string& path, bool hidden) {
  for (Entry& e : entries_)
    if (e.path == path) e.hidden = hidden;
  arrange();
  save();
}

void Places::rename(const std::string& path, const std::string& name) {
  std::string kept;
  for (const char c : name)
    if (c != '\t' && c != '\n') kept += c;
  kept.erase(0, kept.find_first_not_of(' '));
  kept.erase(kept.find_last_not_of(' ') + 1);
  for (const Place& p : places_)
    if (p.path == path && p.ownName == kept) kept.clear();
  for (Entry& e : entries_)
    if (e.path == path) e.name = kept;
  arrange();
  save();
}

void Places::resetOrder() {
  std::vector<Entry> pins, others;
  for (Entry e : entries_) {
    e.hidden = false;
    (e.pinned ? pins : others).push_back(e);
  }
  entries_ = pins;
  // The places found in the order found (arrange()), with their names; the
  // drives not there now after them.
  for (const Place& f : found_)
    for (const Entry& e : others)
      if (e.path == f.path) entries_.push_back(e);
  for (const Entry& e : others)
    if (std::none_of(found_.begin(), found_.end(), [&](const Place& f) { return f.path == e.path; }))
      entries_.push_back(e);
  arrange();
  save();
}

void Places::save() {
  std::string text;
  for (const Entry& e : entries_) {
    text += (e.pinned ? "" : "place\t") + e.path + '\n';
    if (e.hidden) text += "hide\t" + e.path + '\n';
    if (!e.name.empty()) text += "name\t" + e.name + '\t' + e.path + '\n';
  }
  // Serialize writes without waiting on disk in the render loop.
  auto done = std::make_shared<std::promise<void>>();
  const auto previous = saved_;
  saved_ = done->get_future().share();
  std::thread([text, previous, done] {
    if (previous.valid()) previous.wait();
    const fs::path file = configDir() / "pinned_folders.conf";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file);
    out << text;
    out.close();
    done->set_value();
  }).detach();
}
