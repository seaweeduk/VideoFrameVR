#pragma once

#include "video_format.h"
#include "video_memory.h"

#include <string>

// Whether the video brings its own packed mask: as chosen for it by hand,
// else as its file name says (whatever the format).
bool packedMask(const std::string& path, const VideoSettings& video);
// The one layout a packed mask comes in: side-by-side fisheye.
bool maskFormat(const VideoFormat& format);

// The colours a packed mask is read in: red and blue where its second layer
// was found and not hidden, else red.
MaskColours maskColours(const VideoSettings& video);

// What passthrough shows the room through, as the scene draws it: the
// packed alpha mask (side-by-side fisheye only, read in maskColours()),
// else the video's background key (VideoSettings::keyRemoves(), colourKeyed()).
// tvGamma: the picture is shown with TV gamma (VideoPlayer::tvGammaImage()),
// which the key's colour (the frame's own values, found or picked) is
// converted for. picking: the key's colour is being picked, so the key cuts
// nothing, to show what is pointed at.
SeeThrough seeThrough(bool passthrough, bool alphaPacked, const VideoFormat& format,
                      const VideoSettings& video, bool tvGamma, bool picking);

// Why passthrough switches itself on for a video made for it (Settings →
// Automatic passthrough), if its passthrough was never set by hand: it
// brings its own mask (packedMask(), and in the one layout a mask comes in: a
// name alone also matches titles such as "…-alpha-turbo"), or the fill
// check found a flat background, or the green screen check a green screen
// (video_check.h). A green fill is found by both: the fill's colour key wins.
enum class AutoPassthrough { None, Mask, Fill, GreenScreen };
AutoPassthrough autoPassthrough(const std::string& path, const VideoFormat& format,
                                const VideoSettings& video);
// The green screen check has yet to look at this video (or only an older one did).
bool greenCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video);
// ... and the fill check, which comes with it, where passthrough may switch
// itself on or is on (its colour is the key's Automatic).
bool fillCheckWanted(const std::string& path, const VideoFormat& format,
                     const VideoSettings& video);
// The mask layer check has yet to look at this video's packed mask (shown
// in this format), and no second layer was found in it yet.
bool layerCheckWanted(const std::string& path, const VideoFormat& format,
                      const VideoSettings& video);
