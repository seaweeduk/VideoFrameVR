#include "keyframe_grabber.h"

#include "cairo_canvas.h"
#include "keyframe_reader.h"
#include "playback_state.h"
#include "remote.h"
#include "thread_priority.h"

#include <cairo.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace {
// A file's opening or a picture's reading is given up after this long (a
// share that stopped answering).
constexpr std::chrono::seconds kPatience{30};
// A video on a network share or a stream that playback hasn't read ahead of
// by then is opened all the same.
constexpr std::chrono::seconds kAheadWait{30};
// Not pointed at for this long, the pointer comes anew: how it moves isn't
// known yet.
constexpr std::chrono::milliseconds kAway{100};
}  // namespace

struct KeyframeGrabber::Shared {
  int maxWidth = 0, maxHeight = 0;
  std::mutex mutex;
  std::condition_variable wake;
  bool quit = false;
  bool stopped = false;  // the worker has ended
  // What is asked for.
  std::string path;                // empty: no video
  std::atomic<uint64_t> video{0};  // changes with `path`, which stops the reads of the one before
  double target = 0;               // seconds: the latest request's
  uint64_t asked = 0;              // counts this video's requests
  bool rest = false;               // free the decoder
  // The worker's answer.
  bool opened = false;    // it has tried to open `path`
  uint64_t answered = 0;  // the request it finished last
  bool fresh = false;     // `picture` is new
  bool failed = false;    // the video can't be opened, or gave no picture at all
  KeyframeReader::Picture picture;

  void work();
  void serve();
};

void KeyframeGrabber::Shared::work() {
  runInBackground(Background::Decoding);
  serve();
  // Only now, with the file closed: that can take as long as a read.
  std::lock_guard<std::mutex> lock(mutex);
  stopped = true;
  wake.notify_all();
}

void KeyframeGrabber::Shared::serve() {
  uint64_t mine = 0, served = 0;
  std::string current;
  bool shown = false;  // this video gave a picture
  Interrupt interrupt(kPatience, video, mine);
  KeyframeReader reader(interrupt);
  KeyframeReader::Picture made;
  std::unique_lock<std::mutex> lock(mutex);
  for (;;) {
    wake.wait(lock, [&] { return quit || video != mine || asked != served || rest; });
    if (quit) return;
    if (video != mine) {
      mine = video;
      current = path;
      served = 0;
      shown = false;
      rest = false;
      lock.unlock();  // closing a remote file's connection can take a moment, opening one more
      reader.close();
      interrupt = Interrupt(kPatience, video, mine);
      const bool ok = current.empty() || reader.open(current);
      lock.lock();
      if (video != mine) continue;  // closed meanwhile
      opened = true;
      failed = !ok;
      // One that can't be opened has said so, and is asked nothing more.
      if (!ok) current.clear();
      continue;
    }
    if (rest && asked == served) {
      rest = false;
      lock.unlock();
      reader.rest();
      lock.lock();
      continue;
    }
    rest = false;
    if (current.empty() || asked == served) {
      served = asked;
      continue;
    }
    const uint64_t request = asked;
    const double seconds = target;
    lock.unlock();
    using Read = KeyframeReader::Read;
    interrupt = Interrupt(kPatience, video, mine);
    const bool wasOpen = reader.isOpen();
    Read got = wasOpen || reader.open(current) ? reader.read(seconds, maxWidth, maxHeight, made)
                                               : Read::Failed;
    // A read that fails in a file opened a while ago (a connection that
    // dropped since): once more, opened anew.
    if (got == Read::Failed && wasOpen && !Interrupt::check(&interrupt)) {
      interrupt = Interrupt(kPatience, video, mine);
      got = reader.open(current) ? reader.read(seconds, maxWidth, maxHeight, made) : Read::Failed;
    }
    lock.lock();
    if (video != mine) continue;  // closed meanwhile
    served = answered = request;
    if (got == Read::New) {
      picture.pixels.swap(made.pixels);
      picture.width = made.width;
      picture.height = made.height;
      fresh = shown = true;
    } else if (got == Read::Failed && !shown) {
      failed = true;
    }
  }
}

KeyframeGrabber::KeyframeGrabber(Uploader& uploader, int maxWidth, int maxHeight, double steady)
    : shared_(std::make_shared<Shared>()), uploader_(uploader), steady_(steady) {
  shared_->maxWidth = maxWidth;
  shared_->maxHeight = maxHeight;
}

KeyframeGrabber::~KeyframeGrabber() {
  if (!worker_.joinable()) return;
  Shared& s = *shared_;
  std::unique_lock<std::mutex> lock(s.mutex);
  s.quit = true;
  ++s.video;
  s.wake.notify_all();
  // A read under way on a share or from a server need not end when told to.
  // The worker owns what it uses, so it can outlive this.
  const bool stopped =
      s.wake.wait_for(lock, std::chrono::milliseconds(500), [&s] { return s.stopped; });
  lock.unlock();
  if (stopped) worker_.join();
  else worker_.detach();
}

void KeyframeGrabber::setVideo(const std::string& location, bool stream) {
  video_ = location;
  far_ = stream || remote::isRemote(location);
  videoAt_ = lastUse_ = Clock::now();
  unavailable_ = false;
  open(far_ ? std::string() : location);
}

void KeyframeGrabber::point(double seconds, double fraction) {
  const Clock::time_point now = Clock::now();
  if (now - lastUse_ > kAway) pace_.reset();
  lastUse_ = now;
  if (video_.empty() || unavailable_) return;
  const bool worth = pace_.update(
      {std::chrono::duration<double>(now.time_since_epoch()).count(), fraction, steady_, busy()});
  // Pointed at before playback let it open: now.
  if (path_ != video_) open(video_);
  // The video's first picture at once, whatever the pointer does: there is
  // none to show meanwhile.
  if (asked_ != 0 && !(worth && std::abs(seconds - target_) > 0.5)) return;
  ask(seconds);
  pace_.asked(fraction);
}

void KeyframeGrabber::update(const PlaybackState& playback) {
  const Clock::time_point now = Clock::now();
  // Not a live stream (no duration): its bar shows no preview.
  if (far_ && !video_.empty() && !unavailable_ && path_ != video_ && playback.duration > 0 &&
      (playback.readAhead || now - videoAt_ > kAheadWait))
    open(video_);
  take();
  // One that can't show other places (a stream that can't seek) lets its
  // connection go.
  if (failed_) {
    open({});
    unavailable_ = true;
  }
  if (now - lastUse_ > kIdle) rest();
}

void KeyframeGrabber::open(const std::string& path) {
  if (path.empty() && path_.empty()) return;
  Shared& s = *shared_;
  path_ = path;
  target_ = -1;
  asked_ = answered_ = 0;
  opened_ = hasFrame_ = failed_ = resting_ = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.path = path;
    ++s.video;
    s.asked = s.answered = 0;
    s.opened = s.fresh = s.failed = s.rest = false;
  }
  if (!path.empty() && !worker_.joinable())
    worker_ = std::thread([shared = shared_] { shared->work(); });
  s.wake.notify_all();
}

void KeyframeGrabber::ask(double seconds) {
  Shared& s = *shared_;
  target_ = seconds;
  resting_ = false;
  ++asked_;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.target = seconds;
    s.asked = asked_;
    s.rest = false;
  }
  s.wake.notify_all();
}

// Frees the decoder and its frames; the file stays open.
void KeyframeGrabber::rest() {
  if (resting_ || path_.empty() || busy()) return;
  resting_ = true;
  Shared& s = *shared_;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rest = true;
  }
  s.wake.notify_all();
}

// The worker's answer, if it has one.
void KeyframeGrabber::take() {
  // Its uploads were dropped (no frames drawn for long): the picture again.
  if (canvas_ && canvas_->takeLost()) canvas_->upload();
  if (path_.empty() || (opened_ && answered_ == asked_)) return;
  Shared& s = *shared_;
  KeyframeReader::Picture picture;
  {
    // Never waits for the worker: this thread draws the headset's frames.
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock) return;
    opened_ = s.opened;
    answered_ = s.answered;
    failed_ = s.failed;
    if (!s.fresh) return;
    s.fresh = false;
    picture.pixels.swap(s.picture.pixels);
    picture.width = s.picture.width;
    picture.height = s.picture.height;
  }
  if (!canvas_ || canvas_->width() != picture.width || canvas_->height() != picture.height) {
    auto canvas = std::make_unique<CairoCanvas>();
    if (!canvas->init(uploader_, picture.width, picture.height)) return;
    canvas_ = std::move(canvas);
  }
  cairo_surface_t* surface = canvas_->surface();
  cairo_surface_flush(surface);
  unsigned char* rows = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const size_t rowBytes = static_cast<size_t>(picture.width) * 4;
  for (int y = 0; y < picture.height; ++y)
    std::memcpy(rows + static_cast<size_t>(y) * stride,
                &picture.pixels[static_cast<size_t>(y) * picture.width], rowBytes);
  cairo_surface_mark_dirty(surface);
  canvas_->upload();
  hasFrame_ = true;
}

const Texture* KeyframeGrabber::image() const {
  return hasFrame_ && canvas_ ? canvas_->texture() : nullptr;
}

int KeyframeGrabber::width() const { return canvas_ ? canvas_->width() : 0; }
int KeyframeGrabber::height() const { return canvas_ ? canvas_->height() : 0; }
