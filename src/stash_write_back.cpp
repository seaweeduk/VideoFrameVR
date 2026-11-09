#include "stash_write_back.h"

#include "stash_client.h"
#include "stash_store.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

// Stash's rating100 of a rating in stars: 0.5 .. 5 → 10 .. 100.
int rating100(float stars) { return static_cast<int>(std::lround(stars * 20)); }

int countOf(const std::vector<int64_t>& times, int64_t t) {
  return static_cast<int>(std::count(times.begin(), times.end(), t));
}

bool counted(StashOutbox& outbox, StashWriteBackStatus& status) {
  std::vector<StashOutbox::Rating> ratings;
  std::vector<StashOutbox::Play> plays;
  if (!outbox.waiting(ratings, plays)) {
    status.error = outbox.error();
    return false;
  }
  status.waitingRatings = static_cast<int>(ratings.size());
  status.waitingPlays = static_cast<int>(plays.size());
  return true;
}

// Sends what waits in the outbox: each rating as it is, each scene's plays
// but those its play_history has already (beyond the ones kept as sent).
// Stops at the first request that gets no answer, keeping the rest; a
// change Stash refuses waits too, the others go on; and between scenes once
// `paused` (write-back switched off). False with `error` if anything is
// left that could not be sent.
bool deliverWaiting(StashClient& client, StashOutbox& outbox, int& sentRatings, int& sentPlays,
                    std::string& error, const std::function<bool()>& paused) {
  std::vector<StashOutbox::Rating> ratings;
  std::vector<StashOutbox::Play> plays;
  if (!outbox.waiting(ratings, plays)) {
    error = outbox.error();
    return false;
  }
  if (ratings.empty() && plays.empty()) return true;
  StashStore store;
  std::unordered_map<std::string, int64_t> scenes;
  std::unordered_map<int64_t, std::string> names;
  if (!store.scenes(client.server(), scenes, names)) {
    error = "Waiting to send: " + store.error();
    return false;
  }
  std::vector<StashOutbox::Rating> notStash;
  std::vector<StashOutbox::Play> notStashPlays;
  bool ok = true;
  int ratingsBefore = sentRatings, playsBefore = sentPlays;
  for (const StashOutbox::Rating& r : ratings) {
    if (paused()) return false;
    const auto scene = scenes.find(r.oshash);
    if (scene == scenes.end()) {
      notStash.push_back(r);
      continue;
    }
    if (!client.setRating(scene->second, r.stars > 0 ? rating100(r.stars) : 0)) {
      error = client.error();
      ok = false;
      if (!client.reached()) return false;
      continue;
    }
    ++sentRatings;
    if (!outbox.ratingDone(r)) {
      error = outbox.error();
      return false;
    }
  }

  std::unordered_map<std::string, std::vector<int64_t>> sent;
  if (!outbox.sent(sent)) {
    error = outbox.error();
    return false;
  }
  // Each video's plays, in the order they were made.
  std::map<std::string, std::vector<StashOutbox::Play>> byVideo;
  for (const StashOutbox::Play& p : plays) byVideo[p.oshash].push_back(p);
  for (const auto& [oshash, list] : byVideo) {
    if (paused()) return false;
    const auto scene = scenes.find(oshash);
    if (scene == scenes.end()) {
      notStashPlays.insert(notStashPlays.end(), list.begin(), list.end());
      continue;
    }
    StashClient::Activity activity;
    bool exists = false;
    if (!client.fetchActivity(scene->second, activity, exists)) {
      error = client.error();
      ok = false;
      if (!client.reached()) return false;
      continue;
    }
    if (!exists) {  // gone from Stash since the copy was made
      notStashPlays.insert(notStashPlays.end(), list.begin(), list.end());
      continue;
    }
    // A play is there already when Stash has more plays at its time than
    // were sent at it: kept as found, not sent.
    const std::vector<int64_t>& sentTimes = sent[oshash];
    std::unordered_map<int64_t, int> taken;
    std::vector<bool> found;
    std::vector<int64_t> times;
    for (const StashOutbox::Play& p : list) {
      const int there = countOf(activity.plays, p.at) - countOf(sentTimes, p.at) - taken[p.at];
      found.push_back(there > 0);
      if (there > 0)
        ++taken[p.at];
      else
        times.push_back(p.at);
    }
    if (!client.addPlays(scene->second, times)) {
      error = client.error();
      ok = false;
      if (!client.reached()) return false;
      continue;
    }
    sentPlays += static_cast<int>(times.size());
    if (!outbox.playsDone(list, scene->second, found)) {
      error = outbox.error();
      return false;
    }
  }
  if ((!notStash.empty() || !notStashPlays.empty()) && !outbox.drop(notStash, notStashPlays)) {
    error = outbox.error();
    return false;
  }
  if (!notStash.empty() || !notStashPlays.empty())
    std::printf("Stash: %zu ratings and %zu plays of videos Stash doesn't have were not sent\n",
                notStash.size(), notStashPlays.size());
  if (sentRatings > ratingsBefore || sentPlays > playsBefore)
    std::printf("Stash: sent %d ratings and %d plays\n", sentRatings - ratingsBefore,
                sentPlays - playsBefore);
  return ok;
}

// The one-off send's plan: for each video of the library Stash has (by its
// oshash), its rating if Stash's differs (never a clearing one), and the
// plays neither sent before nor waiting. The app keeps the time of its last
// play only, so those go at that time; plays Stash already has at it (more
// than were sent at it) are left out, as delivery would.
bool makePlan(StashClient& client, StashOutbox& outbox,
              const std::vector<StashLibraryEntry>& library, StashSendPlan& plan) {
  plan = StashSendPlan{};
  StashStore store;
  std::unordered_map<std::string, int64_t> scenes;
  std::unordered_map<int64_t, std::string> names;
  if (!store.scenes(client.server(), scenes, names)) {
    plan.error = store.error() + ": sync first";
    return false;
  }
  std::vector<StashOutbox::Rating> waitingRatings;
  std::vector<StashOutbox::Play> waitingPlays;
  std::unordered_map<std::string, std::vector<int64_t>> sent;
  if (!outbox.waiting(waitingRatings, waitingPlays) || !outbox.sent(sent)) {
    plan.error = outbox.error();
    return false;
  }
  plan.waiting = static_cast<int>(waitingRatings.size() + waitingPlays.size());
  std::unordered_set<std::string> ratingWaits;
  for (const auto& r : waitingRatings) ratingWaits.insert(r.oshash);
  std::unordered_map<std::string, int> playsWait;
  for (const auto& p : waitingPlays) ++playsWait[p.oshash];
  std::unordered_map<int64_t, StashClient::Activity> activity;
  if (!client.fetchActivity(activity)) {
    plan.error = client.error();
    return false;
  }
  for (const StashLibraryEntry& e : library) {
    const auto scene = scenes.find(e.oshash);
    if (e.oshash.empty() || scene == scenes.end()) continue;
    const auto now = activity.find(scene->second);
    if (now == activity.end()) continue;  // gone from Stash since the copy
    StashSendPlan::Item item;
    item.oshash = e.oshash;
    item.scene = scene->second;
    item.name = names[scene->second];
    if (e.rating > 0 && !ratingWaits.count(e.oshash) &&
        now->second.rating100 != rating100(e.rating)) {
      item.stars = e.rating;
      item.stashRating100 = now->second.rating100;
    }
    const std::vector<int64_t>& sentTimes = sent[e.oshash];
    const int missing =
        e.plays - static_cast<int>(sentTimes.size()) - playsWait[e.oshash];
    if (missing > 0 && e.lastPlayedAt > 0) {
      const int there =
          std::max(0, countOf(now->second.plays, e.lastPlayedAt) - countOf(sentTimes, e.lastPlayedAt));
      item.found = std::min(missing, there);
      item.plays = missing - item.found;
      item.playedAt = e.lastPlayedAt;
    }
    if (item.stars <= 0 && item.plays == 0 && item.found == 0) continue;
    plan.ratings += item.stars > 0;
    plan.plays += item.plays;
    plan.found += item.found;
    plan.scenes += item.stars > 0 || item.plays > 0;
    plan.items.push_back(std::move(item));
  }
  std::sort(plan.items.begin(), plan.items.end(),
            [](const StashSendPlan::Item& a, const StashSendPlan::Item& b) { return a.name < b.name; });
  return true;
}

}  // namespace

struct StashWriteBack::Shared {
  StashSettings settings;
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false, deliver = false;
  bool paused = false;  // write-back off: nothing is sent, what waits is kept
  enum class Ask { None, Plan, Send } ask = Ask::None;
  uint64_t ticket = 0;  // the one-off asked for; a closed one's result is dropped
  std::vector<StashLibraryEntry> library;
  struct Event {
    std::string oshash;
    bool rating = false;
    float stars = 0;
    int64_t at = 0;
    int count = 0;
  };
  std::vector<Event> events;
  struct Tags {
    bool pending = false;
    int64_t scene = 0;
    std::vector<int64_t> add, remove;
  } tags;
  StashClient* client = nullptr;  // the request under way, to stop it
  // What the worker finished, for poll().
  bool changed = false;
  StashWriteBackStatus status;
  bool planChanged = false;
  StashSendPlan plan;
  bool tagsChanged = false;
  StashTagSave tagSave;
};

StashWriteBack::StashWriteBack(const StashSettings& settings) : shared_(std::make_shared<Shared>()) {
  shared_->settings = settings;
  worker_ = std::thread(work, shared_);
}

StashWriteBack::~StashWriteBack() {
  stop();
  if (worker_.joinable()) worker_.join();
}

void StashWriteBack::stop() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop = true;
    if (shared_->client) shared_->client->cancel();
  }
  shared_->wake.notify_one();
}

void StashWriteBack::rated(const std::string& oshash, float stars) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->events.push_back({oshash, true, stars, 0, 0});
  }
  shared_->wake.notify_one();
}

void StashWriteBack::played(const std::string& oshash, int64_t at, int count) {
  if (count <= 0 || at <= 0) return;
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->events.push_back({oshash, false, 0, at, count});
  }
  shared_->wake.notify_one();
}

void StashWriteBack::setEnabled(bool on) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->paused = !on;
    shared_->deliver = on;
    if (!on) {
      shared_->ask = Shared::Ask::None;
      ++shared_->ticket;
      shared_->planChanged = false;
      shared_->tags.pending = false;  // never sent later, out of sight
      if (shared_->client) shared_->client->cancel();
    }
  }
  if (!on) plan_ = StashSendPlan{};
  if (!on && tagSave_.state == StashTagSave::State::Saving) tagSave_.state = StashTagSave::State::Failed;
  shared_->wake.notify_one();
}

void StashWriteBack::deliver() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->deliver = true;
  }
  shared_->wake.notify_one();
}

void StashWriteBack::plan(std::vector<StashLibraryEntry> library) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->library = std::move(library);
    shared_->ask = Shared::Ask::Plan;
    ++shared_->ticket;
  }
  plan_ = StashSendPlan{};
  plan_.state = StashSendPlan::State::Checking;
  shared_->wake.notify_one();
}

void StashWriteBack::send(std::vector<StashLibraryEntry> library) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->library = std::move(library);
    shared_->ask = Shared::Ask::Send;
    ++shared_->ticket;
  }
  plan_.state = StashSendPlan::State::Sending;
  shared_->wake.notify_one();
}

void StashWriteBack::closePlan() {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    ++shared_->ticket;
    shared_->planChanged = false;
    // A send asked for and not begun is not begun.
    if (shared_->ask != Shared::Ask::None) shared_->ask = Shared::Ask::None;
  }
  plan_ = StashSendPlan{};
}

void StashWriteBack::saveTags(int64_t scene, std::vector<int64_t> add,
                              std::vector<int64_t> remove) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->tags = {true, scene, std::move(add), std::move(remove)};
    shared_->tagsChanged = false;
  }
  tagSave_ = {StashTagSave::State::Saving, scene, 0};
  shared_->wake.notify_one();
}

bool StashWriteBack::poll() {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  const bool changed = shared_->changed || shared_->planChanged || shared_->tagsChanged;
  if (shared_->changed) status_ = shared_->status;
  if (shared_->planChanged) plan_ = shared_->plan;
  if (shared_->tagsChanged) tagSave_ = shared_->tagSave;
  shared_->changed = shared_->planChanged = shared_->tagsChanged = false;
  return changed;
}

void StashWriteBack::work(std::shared_ptr<Shared> shared) {
  StashOutbox outbox(StashClient(shared->settings.url, shared->settings.apiKey).server());
  StashWriteBackStatus status;  // the worker's; published as shared->status
  std::unique_lock<std::mutex> lock(shared->mutex);
  auto publish = [&] {
    shared->status = status;
    shared->changed = true;
  };
  for (;;) {
    shared->wake.wait(lock, [&] {
      return shared->stop || !shared->events.empty() ||
             (!shared->paused &&
              (shared->deliver || shared->ask != Shared::Ask::None || shared->tags.pending));
    });
    // What the app handed over is kept first, even when stopping.
    if (!shared->events.empty()) {
      const std::vector<Shared::Event> events = std::move(shared->events);
      shared->events.clear();
      lock.unlock();
      status.error.clear();
      for (const Shared::Event& e : events) {
        const bool kept = e.rating ? outbox.addRating(e.oshash, e.stars, true)
                                   : outbox.addPlays(e.oshash, e.at, e.count);
        if (!kept) {
          status.error = outbox.error();
          std::fprintf(stderr, "Stash: %s\n", status.error.c_str());
        }
      }
      counted(outbox, status);
      lock.lock();
      shared->deliver = true;
      publish();
      continue;
    }
    if (shared->stop) return;
    if (shared->paused) continue;

    StashClient client(shared->settings.url, shared->settings.apiKey, shared->settings.unverified());
    shared->client = &client;
    const std::function<bool()> paused = [&shared] {
      std::lock_guard<std::mutex> hold(shared->mutex);
      return shared->paused || shared->stop;
    };
    if (shared->tags.pending) {
      const Shared::Tags tags = std::move(shared->tags);
      shared->tags = {};
      lock.unlock();
      const bool ok = client.editTags(tags.scene, tags.add, tags.remove);
      if (ok)
        std::printf("Stash: tags saved, %zu added and %zu removed\n", tags.add.size(),
                    tags.remove.size());
      else
        std::fprintf(stderr, "Stash: tags not saved: %s\n", client.error().c_str());
      lock.lock();
      shared->client = nullptr;
      if (!shared->tags.pending) {  // else a newer save says how it went
        shared->tagSave = {ok ? StashTagSave::State::Saved : StashTagSave::State::Failed,
                           tags.scene, static_cast<int64_t>(std::time(nullptr))};
        shared->tagsChanged = true;
      }
      continue;
    }
    if (shared->ask != Shared::Ask::None) {
      const bool send = shared->ask == Shared::Ask::Send;
      const uint64_t ticket = shared->ticket;
      const std::vector<StashLibraryEntry> library = std::move(shared->library);
      shared->library.clear();
      shared->ask = Shared::Ask::None;
      lock.unlock();
      StashSendPlan plan;
      bool ok = makePlan(client, outbox, library, plan);
      if (ok && send) {
        // Kept first, then sent with whatever else waits. Never a clearing
        // rating, and never over one made in the app meanwhile.
        for (const StashSendPlan::Item& item : plan.items) {
          if (item.stars > 0) ok = ok && outbox.addRating(item.oshash, item.stars, false);
          if (item.plays + item.found > 0)
            ok = ok && outbox.addPlays(item.oshash, item.playedAt, item.plays + item.found);
        }
        if (!ok) plan.error = outbox.error();
        if (ok) {
          int ratings = 0, plays = 0;
          ok = deliverWaiting(client, outbox, ratings, plays, plan.error, paused);
          status.sentRatings += ratings;
          status.sentPlays += plays;
          plan.sentRatings = ratings;
          plan.sentPlays = plays;
          status.error = ok || paused() ? std::string() : plan.error;
        }
        counted(outbox, status);
      }
      plan.state = !ok ? StashSendPlan::State::Failed
                   : send ? StashSendPlan::State::Done
                          : StashSendPlan::State::Ready;
      lock.lock();
      shared->client = nullptr;
      if (send) publish();
      if (ticket == shared->ticket) {
        shared->plan = std::move(plan);
        shared->planChanged = true;
      }
      continue;
    }
    shared->deliver = false;
    lock.unlock();
    int ratings = 0, plays = 0;
    std::string error;
    const bool ok = deliverWaiting(client, outbox, ratings, plays, error, paused);
    const bool halted = paused();  // switched off: what is left waits, no error
    status.sentRatings += ratings;
    status.sentPlays += plays;
    status.error = ok || halted ? std::string() : error;
    if (!ok && !halted) std::fprintf(stderr, "Stash: write-back: %s\n", error.c_str());
    counted(outbox, status);
    lock.lock();
    shared->client = nullptr;
    publish();
  }
}
