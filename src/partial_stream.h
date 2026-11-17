#pragma once

#include <string>

struct mpv_handle;

// Local .part files, read while their download may still be writing them. At
// the end of the data a read waits for more, so mpv buffers as it does for a
// slow stream, instead of ending the video where the download happens to be.
namespace partial_stream {

// Registers the "partialfile" protocol with this mpv.
void addTo(mpv_handle* mpv);
// mpv's URL for this file; the file itself if it is not a local .part file.
std::string mpvUrl(const std::string& path);

}  // namespace partial_stream
