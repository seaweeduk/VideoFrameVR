#pragma once

#include <cstdint>
#include <string>

#include <sqlite3.h>

namespace sqlite {

// One prepared statement, bound and read by column number.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;
  }
  ~Statement() { sqlite3_finalize(st_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  bool ok() const { return st_ != nullptr; }

  Statement& bind(int i, const std::string& s) {
    if (s.empty()) sqlite3_bind_null(st_, i);
    else sqlite3_bind_text(st_, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Statement& bind(int i, int64_t v) {
    sqlite3_bind_int64(st_, i, v);
    return *this;
  }
  Statement& bind(int i, double v) {
    sqlite3_bind_double(st_, i, v);
    return *this;
  }
  // Runs a bound INSERT; false on an error.
  bool run() {
    const bool done = sqlite3_step(st_) == SQLITE_DONE;
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
    return done;
  }

  bool next() { return sqlite3_step(st_) == SQLITE_ROW; }
  std::string text(int i) const {
    const unsigned char* t = sqlite3_column_text(st_, i);
    return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(st_, i))
             : std::string();
  }
  int64_t integer(int i) const { return sqlite3_column_int64(st_, i); }
  double real(int i) const { return sqlite3_column_double(st_, i); }

 private:
  sqlite3_stmt* st_ = nullptr;
};

// Runs SQL without results; false on an error (sqlite3_errmsg says which).
bool exec(sqlite3* db, const char* sql);

// PRAGMA user_version; -1 if it can't be read.
int userVersion(sqlite3* db);

}  // namespace sqlite
