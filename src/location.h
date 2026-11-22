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

// The folder a file or folder is in: smb://h/s/a -> smb://h/s -> smb://h
// -> "" (remote::parent()), /a/b -> /a -> /.
std::string parentFolder(const std::string& path);
