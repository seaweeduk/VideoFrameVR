# Changelog

<!--
Shown in the app's What's new, so the format is strict (cmake/changelog.cmake
checks it in release builds). Newest release first:

## 0.2.0 · 2026-11-09
### New
- A line for both devices
- [Frame] A line for the Steam Frame only
### Improved
- [Desktop] A line for the desktop only

Sections, each only if it has lines, in this order: New, Improved, Fixed,
Removed. Every line starts "- ", then optionally "[Frame] " or "[Desktop] ".
-->

## 0.5.0 · 2026-11-14
### New
- The next video plays when one ends; a new button by Next has Shuffle and Repeat this video
- Stereo focus aimed with eye tracking learns where you really look from your menu clicks
- [Frame] Foveation uses the same learned eye tracking to stay sharp where you look
### Improved
- Stereo focus settings are simpler: one Show focus target switch and no State group
- Automatic focus, Remove background and videos from network shares use less power
- Controller beams fade out towards the pointer dot
### Fixed
- Automatic focus often read plain walls, ceilings and backgrounds as near
- [Frame] Match the room switched itself off in a dark room
- Cancelled downloads could leave files behind, and odd characters in names crashed the app
- In Stash, Scenes kept a pinned list's filters, and some expanded filter lists overlapped
- A Stash address you only tried in setup or Settings could appear in saved diagnostics

## 0.4.0 · 2026-11-12
### New
- Remove background handles solid-colour backgrounds too, found automatically or picked by you
- [Frame] Skip repeated frames saves power and keeps the headset cooler; it's on by default
- Connect to Stash servers that use a self-signed certificate
- [Desktop] Experimental hardware video decoding on AMD graphics cards
### Improved
- Automatic focus, Tone look and green screens use less power
- [Frame] 8-bit H.264 videos use less power
### Fixed
- [Frame] Some cropped or full-range 8K videos still would not play after waking from sleep
- The + and − buttons beside sliders changed values in steps too large for fine adjustments

## 0.3.0 · 2026-11-08
### New
- Support for a newer passthrough video format with two masked layers
- Turn off Mask's second layer to hide male genitalia in supported videos
### Improved
- Green screen videos are detected faster
### Fixed
- [Frame] Playback could freeze after several quick seeks
- [Frame] Some 8K HEVC and H.264 videos would not play after waking from sleep

## 0.2.0 · 2026-11-07
### New
- Play videos from SFTP servers: any computer or NAS you can reach over SSH
- Play Blu-ray disc images (.iso); 3D discs and their MKV rips play in full 3D
- [Frame] Opacity lets the room show through the video; hold a grip and twist to change it
- Warmth, Tint and Exposure in the Picture group
- [Frame] Match the room uses the headset's cameras to tint and light the video like your room
### Improved
- 180°, 360°, fisheye and cubemap videos play faster
- [Frame] 8-bit HEVC videos play faster still
### Fixed
- [Frame] The app could freeze when you switched away from some videos
- Brightness, Contrast and Saturation changed what Mask in the video or Green screen cut out
- Thumbnails of some .ts and .m2ts videos showed only their opening, often black
