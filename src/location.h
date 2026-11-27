#pragma once

#include <string>

// What a location is, told from its address alone (no file system or
// network). A file: a path, or a remote locator (remote.h; smb:// and
// sftp:// in any case). A stream: anything else with "://", a link (http
// and the like) or a Stash scene (stash_stream.h). The library keeps a
// stream under its address as given and a file under its normal path
// (a locator's scheme in lowercase), so isStream() is part of a video's
// identity. A link row kept for an SMB:// or SFTP:// address before it was
// a locator stays as it is.

// A link or a Stash scene.
bool isStream(const std::string& location);
// A link: a scene from Stash plays as its file does.
bool isLink(const std::string& location);

// A link's file name, for showing and guessing from, the first of: the one
// a filename or response-content-disposition parameter in its query names
// (as signed storage links do); its path's last part, if that is a media
// file's name (a video's, a photo's or a disc image's, by its extension);
// the first query value that is one (.../file?path=clip_LR_180.mp4&sig=...);
// its path's last part. Each percent-decoded, then only what follows the
// last / or \ in it, and none with a control character or not UTF-8. At
// most 255 bytes. Empty for anything but a link. Never a key: the library
// and the remote know a link by its address.
std::string linkFileName(const std::string& location);
// What a link is called where it plays: its file name, else its host
// (without a sign-in or port); never its query, which can hold a key.
// Empty for anything but a link, and for a host linkFileName() would refuse.
std::string linkTitle(const std::string& location);

// The folder a file or folder is in: smb://h/s/a -> smb://h/s -> smb://h
// -> "" (remote::parent()), /a/b -> /a -> /.
std::string parentFolder(const std::string& path);
