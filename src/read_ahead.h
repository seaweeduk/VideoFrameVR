#pragma once

#include <cstdint>
#include <list>
#include <vector>

// Playback's read-ahead over the network (SMB, SFTP, Stash's stream): up to three
// windows per stream, each reading two blocks ahead of where it was last read
// and keeping the block behind it. A file whose audio is stored apart from its
// video is read from two places in turn, about once a frame, each time again
// from a little before where the last read there stopped.
//
// Only the bookkeeping: the owner locks it, reads the blocks queue() adds
// (pending while a read is under way, done once filled) and wakes readers.
struct ReadAhead {
  static constexpr uint64_t kBlock = 1024 * 1024;
  static constexpr size_t kWindows = 3;
  struct Block {
    ReadAhead* owner;
    uint64_t generation, window, offset;
    std::vector<uint8_t> bytes;
    size_t filled = 0;
    bool pending = false, done = false;
  };
  // A window reads ahead from where it was last read (at) to next.
  struct Window {
    uint64_t id, at, next;
  };
  uint64_t position = 0, generation = 0, windowIds = 0;
  std::list<Block> blocks;
  std::vector<Window> windows;  // least recently read first
  // A retired block's bytes, kept for the next block: every byte of a block
  // is written before it is read, so a fresh buffer, mapped and cleared, is
  // wasted work.
  std::vector<uint8_t> spare;

  // Moves to offset, in the window that already covers it if there is one.
  void seek(uint64_t offset);
  // Drops blocks no window needs (other generations', behind their window),
  // but not those being read.
  void retire();
  // Adds the blocks each window lacks to be two ahead, `chunk` bytes each,
  // up to `length`; the window being read first.
  void queue(uint64_t chunk, uint64_t length);
  // Copies up to `count` bytes at position from a done block and moves past
  // them; 0 if none has them yet.
  size_t take(void* out, uint64_t count);
  // Whether queue() would add a block now.
  bool wantsBlocks(uint64_t length) const;
};
