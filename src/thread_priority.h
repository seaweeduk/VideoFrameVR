#pragma once

// How far a worker thread steps back behind playback and the menu.
enum class Background {
  Cpu,       // nice 19
  CpuAndIo,  // ... and the idle I/O class
  // ... and on the Frame only on kFrameBackgroundCores, off the compositor's
  // (the decoder's threads, started after, follow).
  Decoding,
};

// Sets it for the calling thread.
void runInBackground(Background how);
