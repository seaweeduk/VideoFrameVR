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
