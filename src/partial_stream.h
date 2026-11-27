#pragma once

#include <string>

struct mpv_handle;

// A partial download on this device (isPartialDownload()), read while its
// browser may still be writing it. At the end of its data a read waits for
// more, as for a slow stream, up to 30 s without the file growing; it ends
// at once when mpv cancels it, and once the download is renamed (finished)
// and read to its end. A read from past the end answers at once: a demuxer
// looking for an index there gets no wait.
namespace partial_stream {

// Registers the "partialfile" protocol with this mpv.
void addTo(mpv_handle* mpv);
// mpv's URL for a local partial download.
std::string mpvUrl(const std::string& path);

}  // namespace partial_stream
