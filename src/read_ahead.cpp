#include "read_ahead.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace {
// How many blocks window `w` lacks to be read two ahead of where it was last
// read, up to `length`: what queue() adds.
std::ptrdiff_t lacking(const ReadAhead& r, const ReadAhead::Window& w, uint64_t length) {
  if (w.next >= length) return 0;
  const auto ahead = std::count_if(r.blocks.begin(), r.blocks.end(), [&](auto& b) {
    return b.window == w.id && b.generation == r.generation && b.offset + b.bytes.size() > w.at;
  });
  return ahead < 2 ? 2 - ahead : 0;
}
}  // namespace

void ReadAhead::seek(uint64_t offset) {
  position = offset;
  auto covers = [&](const Window& w) {
    if (offset >= w.at && offset <= w.next + kBlock) return true;
    return std::any_of(blocks.begin(), blocks.end(), [&](const Block& b) {
      return b.window == w.id && b.generation == generation && offset >= b.offset &&
             offset < b.offset + b.bytes.size();
    });
  };
  auto it = std::find_if(windows.begin(), windows.end(), covers);
  Window w = it != windows.end() ? *it : Window{++windowIds, offset, offset};
  if (it != windows.end()) windows.erase(it);
  else if (windows.size() == kWindows) windows.erase(windows.begin());
  w.at = std::min(w.at, offset);
  if (offset > w.next) w.at = w.next = offset;  // skipped past what it read ahead
  windows.push_back(w);
}

void ReadAhead::retire() {
  blocks.remove_if([&](auto& b) {
    if (b.pending) return false;
    const auto w = std::find_if(windows.begin(), windows.end(),
                                [&](auto& x) { return x.id == b.window; });
    const bool unneeded = b.generation != generation || w == windows.end() ||
                          b.offset + b.bytes.size() + kBlock <= w->at;
    if (unneeded && b.bytes.capacity() > spare.capacity()) spare = std::move(b.bytes);
    return unneeded;
  });
}

void ReadAhead::queue(uint64_t chunk, uint64_t length) {
  for (auto w = windows.rbegin(); w != windows.rend(); ++w) {
    for (auto lacks = lacking(*this, *w, length); lacks > 0 && w->next < length; --lacks) {
      const size_t n = std::min(chunk, length - w->next);
      std::vector<uint8_t> bytes = std::exchange(spare, {});
      bytes.resize(n);
      blocks.push_back({this, generation, w->id, w->next, std::move(bytes)});
      w->next += n;
    }
  }
}

size_t ReadAhead::take(void* out, uint64_t count) {
  for (auto& b : blocks) if (b.generation == generation && b.done &&
      position >= b.offset && position < b.offset + b.filled) {
    const size_t at = position - b.offset, n = std::min<uint64_t>(count, b.filled - at);
    memcpy(out, b.bytes.data() + at, n);
    position += n;
    if (!windows.empty()) windows.back().at = position;
    return n;
  }
  return 0;
}

bool ReadAhead::wantsBlocks(uint64_t length) const {
  return std::any_of(windows.begin(), windows.end(),
                     [&](const Window& w) { return lacking(*this, w, length) > 0; });
}
