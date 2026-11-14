# The Frame's V4L2 decoder

The Steam Frame build decodes H.264 and HEVC on the headset's Iris decoder
through FFmpeg's V4L2 decoders (`h264_v4l2m2m`, `hevc_v4l2m2m`). FFmpeg's own
version of them copies every frame to the CPU and has no working seek, so the
Frame build replaces it with the files in `libavcodec/` here. `vp9_v4l2m2m` is
built too, but the app decodes VP9 in software.

The release recipe (`packaging/release/build-sdk.sh`) copies them over the files
of the same name in official FFmpeg 9.0.2, then applies `../ffmpeg-v4l2.patch`.
That patch adds the extra files to FFmpeg's build and refuses to map Qualcomm
compressed frames to the CPU. Nothing else in FFmpeg is changed, and the desktop
build uses FFmpeg unmodified.

## Where the code comes from

- FFmpeg's V4L2 decoder, LGPL-2.1-or-later.
- John Cox's rewrite of it in [rpi-ffmpeg](https://github.com/jc-kynesim/rpi-ffmpeg),
  commit `d0fe46c27ff128e6263ceb3251cbc9d0ed3d3932` (July 2024, FFmpeg 7.0), also
  LGPL-2.1-or-later. It hands decoded frames out as DMA-BUFs (DRM PRIME), tracks
  timestamps through the decoder and handles flushes and resolution changes.
  `weak_link.[ch]`, `v4l2_req_dmabufs.[ch]` and `v4l2_req_utils.h` are his under
  the MIT licence in each file.
- This project's changes for the Frame, GPL-3.0-or-later, listed below. With
  them the Frame's FFmpeg is GPL-3.0-or-later as a whole.

Until November 2026 the build used the rpi-ffmpeg source tree with those changes
as seven patches. The files here are that code, updated for FFmpeg 9.0.2.

## What this project changed

All of it works around the Frame's `iris_driver` or fits the decoder to this app.

- **Iris fixes.** The known coded size is set before input buffers are
  allocated. An empty buffer marked as an error is a damaged frame, not the end
  of the stream. A missing end-of-stream event after the last buffer is the end
  of the stream. A failed allocation is reported as such.
- **Compressed capture.** `capture_ubwc` asks for Qualcomm's compressed layouts,
  Q08C for 8-bit and Q10C for Main 10, and describes them as NV12 or P010 with
  the Qualcomm DRM modifier. Main 10 always uses Q10C. Linear NV12 is used
  instead of Q08C when an SPS crop would change the compressed layout.
- **Capture pool.** `capture_spare` sizes the pool as the driver's own minimum
  plus the caller's spare frames, instead of a fixed count.
- **Seeks.** For HEVC a seek keeps the session. It stops the decoder, discards
  old pictures up to the last one and starts it again, without restarting either
  queue. `drain_seek_status` tells the caller whether that worked or the decoder
  must be reopened.
- **Staged start.** When Iris refuses a new session for a large stream, the
  decoder programs the full-size compressed capture format first and
  provisional, smaller input sizes, so the stream's own headers bring the real
  size at its first source change. Every step has a deadline.
- **Interrupt.** `av_v4l2_m2m_set_interrupted()` lets another thread wake a
  decoder that waits for its caller to return a capture buffer.

## Moving to a newer FFmpeg

1. Change the `ffmpeg` entry in `packaging/release/sources.json` and the source
   directory in `build-sdk.sh`, then build. Fix what no longer compiles. So far
   that has been renamed fields and headers.
2. Check what FFmpeg changed around the decoder, since its behaviour can change
   without these files changing. For 7.0 to 9.0.2 that was:
   - the MP4 reader adds a multilayer stream's other-layer parameter sets
     (MV-HEVC's second view) to the codec header, and FFmpeg's header parser
     reads them. `hevc_base_extradata()` is that parser reading the base layer
     only, so the size, crop and buffer checks see what they saw before. The
     header itself still goes to Iris with those sets in it;
   - `hevc_mp4toannexb` puts the header's parameter sets before a keyframe's own
     instead of after them.
3. Compare FFmpeg's own `v4l2_*` files with the previous release for fixes worth
   taking.
4. Play the hardware checks on a headset before release. A build that compiles
   and links has not shown that seeks, large streams or Main 10 still work.
