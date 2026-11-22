#include "sqlite_util.h"

namespace sqlite {

bool exec(sqlite3* db, const char* sql) {
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

int userVersion(sqlite3* db) {
  Statement st(db, "PRAGMA user_version");
  return st.ok() && st.next() ? static_cast<int>(st.integer(0)) : -1;
}

}  // namespace sqlite
