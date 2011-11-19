/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#include "ncdc.h"
#include <sqlite3.h>
#include <stdarg.h>

// TODO: The conf_* stuff in util.c should eventually be merged into this file as well.

// All of the db_* functions can be used from multiple threads, the database is
// protected by one master lock. The only exception to this is db_init().
// Note that, even though sqlite may do locking internally in SERIALIZED mode,
// ncdc still performs manual locks around all database calls, in order to get
// reliable values from sqlite3_errmsg(). Prepared statements are not visible
// outside of a single transaction, since I don't know how well those work in a
// multithreaded environment.

static sqlite3 *db = NULL;

static void db_queue_init();
static void db_queue_close();


void db_init() {
  char *dbfn = g_build_filename(conf_dir, "db.sqlite3", NULL);

  if(!sqlite3_threadsafe())
    g_error("sqlite3 has not been compiled with threading support. Please recompile sqlite3 with SQLITE_THREADSAFE enabled.");

  // TODO: should look at the version file instead. This check is simply for debugging purposes.
  gboolean newdb = !g_file_test(dbfn, G_FILE_TEST_EXISTS);
  if(newdb)
    g_error("No db.sqlite3 file present yet. Please run ncdc-db-upgrade.");

  if(sqlite3_open(dbfn, &db))
    g_error("Couldn't open `%s': %s", dbfn, sqlite3_errmsg(db));
  g_free(dbfn);

  sqlite3_busy_timeout(db, 10);

  sqlite3_exec(db, "PRAGMA foreign_keys = FALSE", NULL, NULL, NULL);

  // TODO: create SQL schema, if it doesn't exist yet
  db_queue_init();
}


// Note: db may be NULL when db_lock() is called, this happens when ncdc is shutting down.
#define db_lock(x) do {\
    if(!db) {\
      g_warning("%s:%d: Attempting to use the database after it has been closed.", __FILE__, __LINE__);\
      return x;\
    }\
    sqlite3_mutex_enter(sqlite3_db_mutex(db));\
  } while(0)

#define db_unlock() sqlite3_mutex_leave(sqlite3_db_mutex(db))

// Rolls a transaction back and unlocks the database. Does not check for errors.
#define db_rollback(x) do {\
    char *db_err = NULL;\
    if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &db_err) && db_err)\
      sqlite3_free(db_err);\
    db_unlock();\
  } while(0)

// Convenience function. msg, if not NULL, is assumed to come from sqlite3
// itself, and will be sqlite3_free()'d. A rollback is also automatically
// issued to attempt to create a clean state again.
#define db_err(msg, x) do {\
    g_critical("%s:%d: SQLite3 error: %s", __FILE__, __LINE__, (msg)?(msg):sqlite3_errmsg(db));\
    if(msg)\
      sqlite3_free(msg);\
    db_rollback();\
    return x;\
  } while(0)

// Locks the database and starts a transaction. Must be followed by either db_commit(), db_err() or db_rollback().
#define db_begin(x) do {\
    db_lock(x);\
    char *db_err = NULL;\
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &db_err))\
      db_err(db_err, x);\
  } while(0)

// Performs a step() and handles BUSY and error result codes. If this
// completes, r is set to either SQLITE_ROW or SQLITE_DONE. Otherwise the
// statement is finalized and x is returned.
// Don't use this function within a transaction! A BUSY result code is an error
// in that case, with the single exception of the COMMIT.
#define db_step(s, r, x) do {\
    while((r = sqlite3_step(s)) == SQLITE_BUSY) \
      ;\
    if(r != SQLITE_ROW && r != SQLITE_DONE) {\
      sqlite3_finalize(s);\
      db_err(NULL, x);\
    }\
  } while(0)

// Commits a transaction and unlocks the database.
#define db_commit(x) do {\
    sqlite3_stmt *db_st;\
    int db_r;\
    if(sqlite3_prepare_v2(db, "COMMIT", -1, &db_st, NULL))\
      db_err(NULL, x);\
    db_step(db_st, db_r, x);\
    g_warn_if_fail(db_r == SQLITE_DONE);\
    sqlite3_finalize(db_st);\
    db_unlock();\
  } while(0)


void db_close() {
  db_queue_close();
  sqlite3 *d = db;
  db_lock();
  db = NULL;
  sqlite3_mutex_leave(sqlite3_db_mutex(d));
  sqlite3_close(d);
}





// Asynchronous queue for background processing of queries.
// This facility allows for queueing multiple UPDATE/INSERT/DELETE statements
// and batch-executing them in a single transaction in a background thread.
// WARNING: This facility should *ONLY* be used when no SELECT queries are
// performed afterward that rely on the previously queued queries being
// processed. No guarantees are made about when the queries are executed, only
// that they are. Under the assumption that ncdc does not crash and there is no
// power failure. Queries are always processed in the same order as they are
// queued.

GAsyncQueue *db_queue = NULL;
GThreadPool *db_queue_pool = NULL;
int db_queue_needflush = 0;


// Column types
#define DBQ_END    0
#define DBQ_NULL   1 // No arguments
#define DBQ_INT    2 // GINT_TO_POINTER() argument
#define DBQ_INT64  3 // pointer to a slice-allocated gint64 as argument
#define DBQ_TEXT   4 // pointer to a g_malloc()'ed UTF-8 string
#define DBQ_BLOB   5 // first argument: GINT_TO_POINER(length), second: g_malloc()'d blob


// Processes the queue in a background thread.
static void db_queue_process(gpointer dat, gpointer udat) {
  db_begin();
  sqlite3_stmt *s;
  void **q, **a;
  int i, t;

  while((q = (void **)g_async_queue_try_pop(db_queue)) != NULL) {
    if(sqlite3_prepare_v2(db, *q, -1, &s, NULL))
      db_err(NULL,);
    a = q+1;
    i = 1;
    while((t = GPOINTER_TO_INT(*(a++))) != DBQ_END) {
      switch(t) {
      case DBQ_NULL:
        sqlite3_bind_null(s, i);
        break;
      case DBQ_INT:
        sqlite3_bind_int(s, i, GPOINTER_TO_INT(*(a++)));
        break;
      case DBQ_INT64:
        sqlite3_bind_int64(s, i, *((gint64 *)*a));
        g_slice_free(gint64, *(a++));
        break;
      case DBQ_TEXT:
        sqlite3_bind_text(s, i, (char *)*(a++), -1, g_free);
        break;
      case DBQ_BLOB:
        sqlite3_bind_blob(s, i, (char *)*(a+1), GPOINTER_TO_INT(*a), g_free);
        a += 2;
        break;
      }
      i++;
    }
    if(sqlite3_step(s) != SQLITE_DONE)
      db_err(NULL,);
    sqlite3_finalize(s);

    g_free(q);
  }

  db_commit();
}


static void db_queue_init() {
  db_queue = g_async_queue_new();
  // Only allow a single processing thread - it requires a lock on the database
  // anyway.
  db_queue_pool = g_thread_pool_new(db_queue_process, NULL, 1, FALSE, NULL);
}


// Start a processing thread to flush the queue in the background. Must be
// called from the main thread. (Idle function / timer preferred)
static gboolean db_queue_doflush(gpointer dat) {
  // g_thread_pool_push() may not be called with data = NULL, for some odd reason.
  if(g_async_queue_length(db_queue) && !g_thread_pool_unprocessed(db_queue_pool)) {
    g_thread_pool_push(db_queue_pool, (void *)1, NULL);
    g_atomic_int_set(&db_queue_needflush, 0);
  }
  return FALSE;
}


// Flushes the queue, blocks until all queries are processed and then performs
// a little cleanup.
static void db_queue_close() {
  db_queue_doflush(NULL);
  g_thread_pool_free(db_queue_pool, FALSE, TRUE);
  g_async_queue_unref(db_queue);
  db_queue = NULL;
}


// Queue a flush after a short delay.
#define db_queue_flush() do {\
    if(g_atomic_int_compare_and_exchange(&db_queue_needflush, 0, 1))\
      g_timeout_add(1000, db_queue_doflush, NULL);\
  } while(0)


// The query is assumed to be a static string that is not freed or modified.
// Any BLOB or TEXT arguments will be passed directly to the processing thread
// and will be g_free()'d there.
static void *db_queue_create(const char *q, ...) {
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, (void *)q);

  int t;
  gint64 *n;
  va_list va;
  va_start(va, q);
  while((t = va_arg(va, int)) != DBQ_END) {
    g_ptr_array_add(a, GINT_TO_POINTER(t));
    switch(t) {
    case DBQ_NULL: break;
    case DBQ_INT:
      g_ptr_array_add(a, GINT_TO_POINTER(va_arg(va, int)));
      break;
    case DBQ_INT64:
      n = g_slice_new(gint64);
      *n = va_arg(va, gint64);
      g_ptr_array_add(a, n);
      break;
    case DBQ_TEXT:
      g_ptr_array_add(a, va_arg(va, char *));
      break;
    case DBQ_BLOB:
      g_ptr_array_add(a, GINT_TO_POINTER(va_arg(va, int)));
      g_ptr_array_add(a, va_arg(va, char *));
      break;
    default:
      g_return_val_if_reached(NULL);
    }
  }
  va_end(va);
  g_ptr_array_add(a, GINT_TO_POINTER(DBQ_END));

  return g_ptr_array_free(a, FALSE);
}


#define db_queue_lock() g_async_queue_lock(db_queue)

#define db_queue_unlock() do {\
    g_async_queue_unlock(db_queue);\
    db_queue_flush();\
  } while(0)

#define db_queue_push(...) do {\
    g_async_queue_push(db_queue, db_queue_create(__VA_ARGS__));\
    db_queue_flush();\
  } while(0)

#define db_queue_push_unlocked(...) \
  g_async_queue_push_unlocked(db_queue, db_queue_create(__VA_ARGS__))






// hashdata and hashfiles

#if INTERFACE

// TODO!
#define db_fl_getdone() TRUE
#define db_fl_setdone(v) {}

#endif


// Adds a file to hashfiles and, if not present yet, hashdata. Returns the new hashfiles.id.
gint64 db_fl_addhash(const char *path, guint64 size, time_t lastmod, const char *root, const char *tthl, int tthl_len) {
  db_begin(0);

  char hash[40] = {};
  base32_encode(root, hash);
  sqlite3_stmt *s;

  // hashdata
  if(sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO hashdata (root, size, tthl) VALUES(?, ?, ?)", -1, &s, NULL))
    db_err(NULL, 0);
  sqlite3_bind_text(s, 1, hash, -1, SQLITE_STATIC);
  sqlite3_bind_int64(s, 2, size);
  sqlite3_bind_blob(s, 3, tthl, tthl_len, SQLITE_STATIC);
  if(sqlite3_step(s) != SQLITE_DONE)
    db_err(NULL, 0);
  sqlite3_finalize(s);

  // hashfiles.
  // Note that it in certain situations it may happen that a row with the same
  // filename is already present. This happens when two files in the share have
  // the same realpath() (e.g. one is a symlink). In such a case it is safe to
  // just do a REPLACE.
  if(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO hashfiles (tth, lastmod, filename) VALUES(?, ?, ?)", -1, &s, NULL))
    db_err(NULL, 0);
  sqlite3_bind_text(s, 1, hash, -1, SQLITE_STATIC);
  sqlite3_bind_int64(s, 2, lastmod);
  sqlite3_bind_text(s, 3, path, -1, SQLITE_STATIC);
  if(sqlite3_step(s) != SQLITE_DONE)
    db_err(NULL, 0);
  sqlite3_finalize(s);

  gint64 id = sqlite3_last_insert_rowid(db);
  db_commit(0);
  return id;
}


// Fetch the tthl data associated with a TTH root. Return value must be
// g_free()'d. Returns NULL on error or when it's not in the DB.
char *db_fl_gettthl(const char *root, int *len) {
  db_lock(NULL);

  char hash[40] = {};
  base32_encode(root, hash);
  sqlite3_stmt *s;
  int r;
  int l = 0;
  char *res = NULL;

  if(sqlite3_prepare_v2(db, "SELECT tthl FROM hashfiles WHERE root = ?", -1, &s, NULL))
    db_err(NULL, NULL);
  sqlite3_bind_text(s, 1, hash, -1, SQLITE_STATIC);

  db_step(s, r, NULL);
  if(r == SQLITE_ROW) {
    res = (char *)sqlite3_column_blob(s, 0);
    if(res) {
      l = sqlite3_column_bytes(s, 0);
      res = g_memdup(res, l);
    }
    if(len)
      *len = l;
  }

  sqlite3_finalize(s);
  db_unlock();
  return res;
}


// Get information for a file. Returns 0 if not found or error.
gint64 db_fl_getfile(const char *path, time_t *lastmod, guint64 *size, char *tth) {
  db_lock(0);

  sqlite3_stmt *s;
  int r;
  gint64 id = 0;

  if(sqlite3_prepare_v2(db,
       "SELECT f.id, f.lastmod, f.tth, d.size FROM hashfiles f JOIN hashdata d ON d.root = f.tth WHERE f.filename = ?",
       -1, &s, NULL))
    db_err(NULL, 0);
  sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);

  db_step(s, r, 0);
  if(r == SQLITE_ROW) {
    id = sqlite3_column_int64(s, 0);
    if(lastmod)
      *lastmod = sqlite3_column_int64(s, 1);
    if(tth) {
      const char *hash = (const char *)sqlite3_column_text(s, 2);
      base32_decode(hash, tth);
    }
    if(size)
      *size = sqlite3_column_int64(s, 3);
  }

  sqlite3_finalize(s);
  db_unlock();
  return id;
}


// Batch-remove rows from hashfiles.
// TODO: how/when to remove rows from hashdata for which no entry in hashfiles
// exist? A /gc will do this by calling db_fl_purgedata(), but ideally this
// would be done as soon as the hashdata row has become obsolete.
void db_fl_rmfiles(gint64 *ids, int num) {
  db_begin();

  sqlite3_stmt *s;
  int i;

  if(sqlite3_prepare_v2(db, "DELETE FROM hashfiles WHERE id = ?", -1, &s, NULL))
    db_err(NULL,);

  for(i=0; i<num; i++) {
    sqlite3_bind_int64(s, 1, ids[i]);
    if(sqlite3_step(s) != SQLITE_DONE || sqlite3_reset(s))
      db_err(NULL,);
  }

  sqlite3_finalize(s);

  db_commit();
}


// Gets the full list of all ids in the hashfiles table, in ascending order.
// *callback is called for every row.
void db_fl_getids(void (*callback)(gint64)) {
  db_lock();
  sqlite3_stmt *s;
  int r;

  // This query is fast: `id' is the SQLite rowid, and has an index that is
  // already ordered.
  if(sqlite3_prepare_v2(db, "SELECT id FROM hashfiles ORDER BY id ASC", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    callback(sqlite3_column_int64(s, 0));
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// Remove rows from the hashdata table that are not referenced from the
// hashfiles table.
void db_fl_purgedata() {
  db_lock();
  // Since there is no index on hashfiles(tth), one might expect this query to
  // be extremely slow. Luckily sqlite is clever enough to create a temporary
  // index for this query.
  char *err;
  if(sqlite3_exec(db,
      "DELETE FROM hashdata WHERE NOT EXISTS(SELECT 1 FROM hashfiles WHERE tth = root)",
      NULL, NULL, &err))
    db_err(err,);
  db_unlock();
}





// dl and dl_users


// Fetches everything (except the raw TTHL data) from the dl table in no
// particular order, calls the callback for each row.
void db_dl_getdls(
  void (*callback)(const char *tth, guint64 size, const char *dest, char prio, char error, const char *error_msg, int tthllen)
) {
  db_lock();
  sqlite3_stmt *s;
  char hash[24];
  int r;

  if(sqlite3_prepare_v2(db, "SELECT tth, size, dest, priority, error, error_msg, length(tthl) FROM dl", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    base32_decode((const char *)sqlite3_column_text(s, 0), hash);
    callback(
      hash,
      (guint64)sqlite3_column_int64(s, 1),
      (const char *)sqlite3_column_text(s, 2),
      sqlite3_column_int(s, 3),
      sqlite3_column_int(s, 4),
      (const char *)sqlite3_column_text(s, 5),
      sqlite3_column_int(s, 6)
    );
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// Fetches everything from the dl_users table in no particular order, calls the
// callback for each row.
void db_dl_getdlus(void (*callback)(const char *tth, guint64 uid, char error, const char *error_msg)) {
  db_lock();
  sqlite3_stmt *s;
  char hash[24];
  int r;

  if(sqlite3_prepare_v2(db, "SELECT tth, uid, error, error_msg FROM dl_users", -1, &s, NULL))
    db_err(NULL,);

  db_step(s, r,);
  while(r == SQLITE_ROW) {
    base32_decode((const char *)sqlite3_column_text(s, 0), hash);
    callback(
      hash,
      (guint64)sqlite3_column_int64(s, 1),
      sqlite3_column_int(s, 2),
      (const char *)sqlite3_column_text(s, 3)
    );
    db_step(s, r,);
  }

  sqlite3_finalize(s);
  db_unlock();
}


// (queued) Delete a row from dl and any rows from dl_users that reference the row.
void db_dl_rm(const char *tth) {
  char hash[40] = {};
  base32_encode(tth, hash);

  db_queue_lock();
  db_queue_push_unlocked("DELETE FROM dl_users WHERE tth = ?", DBQ_TEXT, g_strdup(hash), DBQ_END);
  db_queue_push_unlocked("DELETE FROM dl WHERE tth = ?", DBQ_TEXT, g_strdup(hash), DBQ_END);
  db_queue_unlock();
}





// Executes a VACUUM
void db_vacuum() {
  db_lock();
  char *err;
  if(sqlite3_exec(db, "VACUUM", NULL, NULL, &err))
    db_err(err,);
  db_unlock();
}
