#include "stash_write_back.h"

#include "media_client.h"
#include "stash_outbox.h"
#include "stash_store.h"

#include <algorithm>
#include <cmath>
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

// A rating or plays made in the app, for the outbox.
struct Event {
  std::string oshash;
  bool rating = false;
  float stars = 0;
  int64_t at = 0;
  int count = 0;
};

// Puts the events into the outbox in order. One it can't take stays in
// `events` with those after it (a later rating must stay the later one),
// tried again with the next events or before the next send.
bool keep(StashOutbox& outbox, std::vector<Event>& events) {
  size_t kept = 0;
  for (; kept < events.size(); ++kept) {
    const Event& e = events[kept];
    if (!(e.rating ? outbox.addRating(e.oshash, e.stars, true)
                   : outbox.addPlays(e.oshash, e.at, e.count)))
      break;
  }
  events.erase(events.begin(), events.begin() + kept);
  return events.empty();
}

// Sends what waits in the outbox: each rating as it is, each scene's plays
// but those its play_history has already (beyond the ones kept as sent);
// to a server without play times, one play of a scene it doesn't have as
// watched, the others being there with it.
// Stops at the first request that gets no answer, keeping the rest; a
// change Stash refuses waits too, the others go on; and between scenes once
// `paused` (write-back switched off). False with `error` if anything is
// left that could not be sent.
bool deliverWaiting(MediaClient& client, StashOutbox& outbox, int& sentRatings, int& sentPlays,
                    std::string& error, const std::function<bool()>& paused) {
  const char* name = client.kind().name();
  const bool playTimes = client.kind().abilities().playTimes;
  std::vector<StashOutbox::Rating> ratings;
  std::vector<StashOutbox::Play> plays;
  if (!outbox.waiting(ratings, plays)) {
    error = outbox.error();
    return false;
  }
  if (ratings.empty() && plays.empty()) return true;
  StashStore store(client.kind());
  std::unordered_map<std::string, int64_t> scenes;
  std::unordered_map<int64_t, std::string> names;
  if (!store.scenes(client.server(), client.kind().id(), scenes, names)) {
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
    MediaClient::Activity activity;
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
      const int there =
          playTimes ? countOf(activity.plays, p.at) - countOf(sentTimes, p.at) - taken[p.at]
                    : activity.watched || !times.empty();
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
    std::printf("%s: %zu ratings and %zu plays of videos %s doesn't have were not sent\n", name,
                notStash.size(), notStashPlays.size(), name);
  if (sentRatings > ratingsBefore || sentPlays > playsBefore)
    std::printf("%s: sent %d ratings and %d plays\n", name, sentRatings - ratingsBefore,
                sentPlays - playsBefore);
  return ok;
}

// The one-off send's plan: for each video of the library Stash has (by its
// oshash), its rating if Stash's differs (never a clearing one), and the
// plays neither sent before nor waiting. The app keeps the time of its last
// play only, so those go at that time; plays Stash already has at it (more
// than were sent at it) are left out, as delivery would. A server without
// play times: one play of a scene it doesn't have as watched.
bool makePlan(MediaClient& client, StashOutbox& outbox,
              const std::vector<StashLibraryEntry>& library, StashSendPlan& plan) {
  plan = StashSendPlan{};
  StashStore store(client.kind());
  std::unordered_map<std::string, int64_t> scenes;
  std::unordered_map<int64_t, std::string> names;
  if (!store.scenes(client.server(), client.kind().id(), scenes, names)) {
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
  const bool playTimes = client.kind().abilities().playTimes;
  std::unordered_map<int64_t, MediaClient::Activity> activity;
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
          playTimes ? std::max(0, countOf(now->second.plays, e.lastPlayedAt) -
                                      countOf(sentTimes, e.lastPlayedAt))
                    : missing - !now->second.watched;
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

struct StashWriteBack::Shared : StashWorkerShared {
  StashSettings settings;
  bool deliver = false;
  bool paused = false;  // write-back off: nothing is sent, what waits is kept
  enum class Ask { None, Plan, Send } ask = Ask::None;
  uint64_t ticket = 0;  // the one-off asked for; a closed one's result is dropped
  std::vector<StashLibraryEntry> library;
  std::vector<Event> events;
  struct Tags {
    bool pending = false;
    int64_t scene = 0;
    std::vector<int64_t> add, remove;
  } tags;
  // What the worker finished, for poll().
  bool changed = false;
  StashWriteBackStatus status;
  bool planChanged = false;
  StashSendPlan plan;
  bool tagsChanged = false;
  StashTagSave tagSave;
};

StashWriteBack::StashWriteBack(const StashSettings& settings) {
  shared_->settings = settings;
  shared_.start(work);
}

StashWriteBack::~StashWriteBack() = default;

void StashWriteBack::stop() { shared_.stop(); }

void StashWriteBack::rated(const std::string& oshash, float stars) {
  shared_.hand([&] {
    shared_->events.push_back({oshash, true, stars, 0, 0});
  });
}

void StashWriteBack::played(const std::string& oshash, int64_t at, int count) {
  if (count <= 0 || at <= 0) return;
  shared_.hand([&] {
    shared_->events.push_back({oshash, false, 0, at, count});
  });
}

void StashWriteBack::setEnabled(bool on) {
  shared_.hand([&] {
    shared_->paused = !on;
    shared_->deliver = on;
    if (!on) {
      shared_->ask = Shared::Ask::None;
      ++shared_->ticket;
      shared_->planChanged = false;
      shared_->tags.pending = false;  // never sent later, out of sight
      if (shared_->client) shared_->client->cancel();
    }
  });
  if (!on) plan_ = StashSendPlan{};
  if (!on && tagSave_.state == StashTagSave::State::Saving) tagSave_.state = StashTagSave::State::Failed;
}

void StashWriteBack::deliver() {
  shared_.hand([&] {
    shared_->deliver = true;
  });
}

void StashWriteBack::plan(std::vector<StashLibraryEntry> library) {
  shared_.hand([&] {
    shared_->library = std::move(library);
    shared_->ask = Shared::Ask::Plan;
    ++shared_->ticket;
  });
  plan_ = StashSendPlan{};
  plan_.state = StashSendPlan::State::Checking;
}

void StashWriteBack::send(std::vector<StashLibraryEntry> library) {
  shared_.hand([&] {
    shared_->library = std::move(library);
    shared_->ask = Shared::Ask::Send;
    ++shared_->ticket;
  });
  plan_.state = StashSendPlan::State::Sending;
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
  shared_.hand([&] {
    shared_->tags = {true, scene, std::move(add), std::move(remove)};
    shared_->tagsChanged = false;
  });
  tagSave_ = {StashTagSave::State::Saving, scene, 0};
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
  const MediaServer& kind = mediaServer(shared->settings.kind);
  const char* name = kind.name();
  StashOutbox outbox(serverIdentity(kind.kind(), StashAddress(shared->settings.url).server),
                     originalKind(kind.kind()));
  StashWriteBackStatus status;  // the worker's; published as shared->status
  std::vector<Event> unsaved;   // handed over, not in the outbox yet (keep())
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
      unsaved.insert(unsaved.end(), shared->events.begin(), shared->events.end());
      shared->events.clear();
      lock.unlock();
      status.error.clear();
      if (!keep(outbox, unsaved)) {
        status.error = outbox.error();
        std::fprintf(stderr, "%s: %s\n", name, status.error.c_str());
      }
      counted(outbox, status);
      lock.lock();
      shared->deliver = true;
      publish();
      continue;
    }
    if (shared->stop) {
      lock.unlock();
      if (!keep(outbox, unsaved)) std::fprintf(stderr, "%s: %s\n", name, outbox.error().c_str());
      return;
    }
    if (shared->paused) continue;

    const std::unique_ptr<MediaClient> made = kind.client(
        shared->settings.url, shared->settings.apiKey, shared->settings.unverified());
    MediaClient& client = *made;
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
        std::printf("%s: tags saved, %zu added and %zu removed\n", name, tags.add.size(),
                    tags.remove.size());
      else
        std::fprintf(stderr, "%s: tags not saved: %s\n", name, client.error().c_str());
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
      // What the app handed over goes in first, or the plan would count its
      // plays as missing and add them again.
      bool ok = keep(outbox, unsaved);
      if (!ok) plan.error = outbox.error();
      ok = ok && makePlan(client, outbox, library, plan);
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
    const bool kept = keep(outbox, unsaved);
    if (!kept) error = outbox.error();
    const bool ok = deliverWaiting(client, outbox, ratings, plays, error, paused) && kept;
    const bool halted = paused();  // switched off: what is left waits, no error
    status.sentRatings += ratings;
    status.sentPlays += plays;
    status.error = ok || halted ? std::string() : error;
    if (!ok && !halted) std::fprintf(stderr, "%s: write-back: %s\n", name, error.c_str());
    counted(outbox, status);
    lock.lock();
    shared->client = nullptr;
    publish();
  }
}
