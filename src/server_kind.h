#pragma once

// The kinds of media server the app browses and plays from, one server at a
// time (media_server.h has what each can do). Kept in settings.conf and
// stash.db by MediaServer::id(), never by number.
enum class ServerKind { Stash, Xbvr };
