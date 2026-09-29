/* workflow.c — the kb-code-lane workflow state machines in C17.
 *
 * Ported from the Rust shard boards/ lanes/ recipe/ tours/ trails/ mirror/ of
 * crates/kb-code-server. include/kbc/workflow.h is the contract; this file is
 * the only implementation.
 *
 * The file is three layers, deliberately in this order:
 *
 *   1. PURE. Vocabularies and transition predicates over closed enums. No
 *      clock, no store, no allocation. Everything decidable without IO is
 *      here so it can be tested exhaustively and so a refusal message is
 *      minted in exactly one place.
 *   2. PERSISTENT. One sibling SQLite database with its own connection,
 *      because the Rust original's code lane is a separate crate with a
 *      separate store, and kb-c's kbc_store is the DOCUMENT lane — disjoint
 *      tables, no shared row, no join. See the header's argument.
 *   3. DERIVED. Layout and the JSON Canvas export: pure functions of an
 *      already-read board, running no query and touching no file, so an
 *      export can never disagree with the board it came from.
 */

#include "kbc/workflow.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The migration ladder this build knows. A database stamped with anything
 * else is refused rather than read: a binary that interprets a layout it was
 * not written for is how a port rots. */
#define KBC_WORKFLOW_SCHEMA 1u

/* Longest repo name accepted, matching the sibling store's own cap. */
#define KBC_REPO_MAX 128u

/* =========================================================================
 * 1. PURE — vocabularies
 * ========================================================================= */

/* One table per closed vocabulary, walked by its own round-trip test so a
 * spelling cannot drift out of sync with its enum. A parallel-array design
 * would let an edit add a state to one and not the other; a switch would let
 * a compiler catch it, and the compiler is the cheaper witness. */

const char *kbc_board_status_str(kbc_board_status s) {
  switch (s) {
    case KBC_BOARD_PENDING: return "pending";
    case KBC_BOARD_DRAFT: return "draft";
    case KBC_BOARD_ACCEPTED: return "accepted";
    case KBC_BOARD_ARCHIVED: return "archived";
    default: return "";
  }
}

bool kbc_board_status_is_valid(const char *s) {
  if (s == NULL) {
    return false;
  }
  for (int i = 0; i < KBC_BOARD_STATUS__COUNT; i++) {
    if (strcmp(s, kbc_board_status_str((kbc_board_status)i)) == 0) {
      return true;
    }
  }
  return false;
}

kbc_status kbc_board_status_parse(const char *s, kbc_board_status *out,
                                  kbc_err *err) {
  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "board status: NULL is not a status");
  }
  for (int i = 0; i < KBC_BOARD_STATUS__COUNT; i++) {
    if (strcmp(s, kbc_board_status_str((kbc_board_status)i)) == 0) {
      if (out != NULL) {
        *out = (kbc_board_status)i;
      }
      return KBC_OK;
    }
  }
  /* The message names the offending value AND the whole vocabulary, because
   * a caller who typed "pending " needs to know it was whitespace, and a
   * caller who typed "open" needs to know what this machine calls open. */
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "board status \"%s\" is not a status (expected one of: "
                     "pending, draft, accepted, archived)",
                     s);
}

bool kbc_board_status_is_appliable(kbc_board_status s) {
  /* D21: `accepted` and `archived` are TRANSITIONS, not authored state. A
   * document naming one is refused before any gate matters, which is what
   * makes "impossible by the lint" true by construction. */
  return s == KBC_BOARD_PENDING || s == KBC_BOARD_DRAFT;
}

const char *kbc_trail_mode_str(kbc_trail_mode m) {
  switch (m) {
    case KBC_TRAIL_OFF: return "off";
    case KBC_TRAIL_RECORDING: return "recording";
    case KBC_TRAIL_PAUSED: return "paused";
    default: return "";
  }
}

bool kbc_trail_mode_is_valid(const char *s) {
  if (s == NULL) {
    return false;
  }
  for (int i = 0; i < KBC_TRAIL_MODE__COUNT; i++) {
    if (strcmp(s, kbc_trail_mode_str((kbc_trail_mode)i)) == 0) {
      return true;
    }
  }
  return false;
}

kbc_status kbc_trail_mode_parse(const char *s, kbc_trail_mode *out,
                                kbc_err *err) {
  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "trail mode: NULL is not a mode");
  }
  for (int i = 0; i < KBC_TRAIL_MODE__COUNT; i++) {
    if (strcmp(s, kbc_trail_mode_str((kbc_trail_mode)i)) == 0) {
      if (out != NULL) {
        *out = (kbc_trail_mode)i;
      }
      return KBC_OK;
    }
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "unknown mode \"%s\" — expected one of off, recording, "
                     "paused",
                     s);
}

const char *kbc_run_state_str(kbc_run_state s) {
  switch (s) {
    case KBC_RUN_RUNNING: return "running";
    case KBC_RUN_DONE: return "done";
    case KBC_RUN_FAILED: return "failed";
    case KBC_RUN_ABORTED: return "aborted";
    default: return "";
  }
}

const char *kbc_op_state_str(kbc_op_state s) {
  switch (s) {
    case KBC_OP_OK: return "ok";
    case KBC_OP_FAILED: return "failed";
    case KBC_OP_SKIPPED: return "skipped";
    default: return "";
  }
}

kbc_status kbc_run_state_parse(const char *s, kbc_run_state *out,
                               kbc_err *err) {
  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "run state: NULL is not a state");
  }
  for (int i = 0; i < KBC_RUN_STATE__COUNT; i++) {
    if (strcmp(s, kbc_run_state_str((kbc_run_state)i)) == 0) {
      if (out != NULL) {
        *out = (kbc_run_state)i;
      }
      return KBC_OK;
    }
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "run state \"%s\" is not a state (expected one of: "
                     "running, done, failed, aborted)",
                     s);
}

kbc_status kbc_op_state_parse(const char *s, kbc_op_state *out, kbc_err *err) {
  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "op state: NULL is not a state");
  }
  for (int i = 0; i < KBC_OP_STATE__COUNT; i++) {
    if (strcmp(s, kbc_op_state_str((kbc_op_state)i)) == 0) {
      if (out != NULL) {
        *out = (kbc_op_state)i;
      }
      return KBC_OK;
    }
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "op state \"%s\" is not a state (expected one of: ok, "
                     "failed, skipped)",
                     s);
}

const char *kbc_trust_class_str(kbc_trust_class c) {
  switch (c) {
    case KBC_TRUST_ORPHAN: return "orphan";
    case KBC_TRUST_CANDIDATE: return "candidate";
    case KBC_TRUST_LIKELY: return "likely";
    case KBC_TRUST_EXACT: return "exact";
    default: return "";
  }
}

kbc_status kbc_trust_class_parse(const char *s, kbc_trust_class *out,
                                 kbc_err *err) {
  if (s == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "trust class: NULL is not a class");
  }
  for (int i = 0; i < KBC_TRUST__COUNT; i++) {
    if (strcmp(s, kbc_trust_class_str((kbc_trust_class)i)) == 0) {
      if (out != NULL) {
        *out = (kbc_trust_class)i;
      }
      return KBC_OK;
    }
  }
  return kbc_err_set(err, KBC_ERR_INVALID,
                     "trust class \"%s\" is not a class (expected one of: "
                     "orphan, candidate, likely, exact)",
                     s);
}

kbc_trust_class kbc_trust_class_min(kbc_trust_class a, kbc_trust_class b) {
  /* The enum is declared in ascending trust order, so the ordinal IS the
   * order and `min` is the lower ordinal. Written as a comparison rather
   * than `a < b ? a : b` so the invariant is stated, not assumed. */
  return (a <= b) ? a : b;
}

/* The nine reasons class_for can emit, in the order the original lists them.
 * A closed list so a new rung cannot be added without appearing where a
 * caller can walk it. */
static const char *const kbc_lane_reason_list[] = {
    KBC_REASON_PATH_GONE,       KBC_REASON_BLOB_CURRENT,
    KBC_REASON_SHA_ATTRIBUTED,  KBC_REASON_FILE_LEVEL_MOVED,
    KBC_REASON_CONTENT_UNREADABLE, KBC_REASON_NO_SNIPPET,
    KBC_REASON_REANCHORED_EXACT, KBC_REASON_REANCHORED_FUZZY,
    KBC_REASON_NO_ANCHOR,       NULL};

const char *const *kbc_lane_reasons(void) {
  return kbc_lane_reason_list;
}

bool kbc_lane_reason_is_valid(const char *r) {
  if (r == NULL) {
    return false;
  }
  for (const char *const *p = kbc_lane_reason_list; *p != NULL; p++) {
    if (strcmp(r, *p) == 0) {
      return true;
    }
  }
  return false;
}

/* =========================================================================
 * 1. PURE — the transition predicates
 *
 * Every refusal in this file is minted HERE and only here, so the message a
 * caller can match on is the same whether it came from the predicate or from
 * the transaction that enforced it. Each names the from-state AND the
 * to-state: a refusal that says only "invalid" leaves the caller to guess
 * which of two states it got wrong, and a state machine is exactly the place
 * where that guess is expensive.
 * ========================================================================= */

kbc_status kbc_board_status_can_transition(kbc_board_status from,
                                           kbc_board_status to,
                                           kbc_err *err) {
  /* A no-op is legal and NOT a refusal: re-accepting an accepted board is
   * what an idempotent retry looks like, and a caller that retries after a
   * timeout must not be told it lost a race it did not. It still costs a
   * revision bump in the original, and kb-c keeps that. */
  if (from == to) {
    return KBC_OK;
  }
  bool legal = false;
  switch (from) {
    case KBC_BOARD_PENDING:
    case KBC_BOARD_DRAFT:
      /* An authored or proposed board may be accepted, archived, or moved
       * between the two authoring states. */
      legal = (to == KBC_BOARD_ACCEPTED || to == KBC_BOARD_ARCHIVED ||
               to == KBC_BOARD_DRAFT || to == KBC_BOARD_PENDING);
      break;
    case KBC_BOARD_ACCEPTED:
      /* Accepted is not terminal: a CHANGED apply resets it (see
       * kbc_board_apply_status), and an accepted board may be archived. */
      legal = (to == KBC_BOARD_ARCHIVED || to == KBC_BOARD_DRAFT ||
               to == KBC_BOARD_PENDING);
      break;
    case KBC_BOARD_ARCHIVED:
      /* Deliberately NOT accepted. An archive says the board is finished;
       * re-accepting it with no content change in between would resurrect
       * that decision from a bare call. The way back is a re-apply, which is
       * a content change by definition, and which resets the status. */
      legal = (to == KBC_BOARD_PENDING || to == KBC_BOARD_DRAFT);
      break;
    default:
      return kbc_err_set(err, KBC_ERR_INTERNAL,
                         "board status: from-state %d is not a board status",
                         (int)from);
  }
  if (legal) {
    return KBC_OK;
  }
  return kbc_err_set(err, KBC_ERR_CONFLICT,
                     "board status: illegal transition %s -> %s", 
                     kbc_board_status_str(from), kbc_board_status_str(to));
}

kbc_status kbc_run_state_can_transition(kbc_run_state from, kbc_run_state to,
                                        kbc_err *err) {
  if (from == to) {
    return KBC_OK;
  }
  if (from == KBC_RUN_RUNNING) {
    return KBC_OK;
  }
  /* A terminal run never moves. Without this, a slow writer that returns
   * after the failure was recorded could overwrite `failed` with `done` and
   * erase the one record a half-completed run leaves. */
  return kbc_err_set(err, KBC_ERR_CONFLICT,
                     "recipe run: illegal transition %s -> %s",
                     kbc_run_state_str(from), kbc_run_state_str(to));
}

kbc_status kbc_trail_admit_write(bool enabled, kbc_trail_mode mode,
                                 kbc_err *err) {
  if (!enabled) {
    return kbc_err_set(err, KBC_ERR_UNSUPPORTED,
                       "trails are disabled on this daemon — set "
                       "`[trails] enabled = true` and restart. kbc-trail/1 is "
                       "OFF on first boot by design: a record of where a "
                       "person looked is opt-in, never a default");
  }
  if (mode == KBC_TRAIL_RECORDING) {
    return KBC_OK;
  }
  if (mode == KBC_TRAIL_PAUSED) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "trail recording is PAUSED — nothing is being recorded. "
                       "Resume with `kb-code trail state --mode recording`");
  }
  return kbc_err_set(err, KBC_ERR_CONFLICT,
                     "trail recording has not been turned on — the feature is "
                     "permitted but no one has opted in on this volume. Start "
                     "with `kb-code trail state --mode recording`");
}

const char *kbc_trail_admit_problem_type(bool enabled, kbc_trail_mode mode) {
  if (!enabled) {
    return KBC_TRAILS_ERR_DISABLED;
  }
  if (mode == KBC_TRAIL_PAUSED) {
    return KBC_TRAILS_ERR_PAUSED;
  }
  if (mode == KBC_TRAIL_OFF) {
    return KBC_TRAILS_ERR_OFF;
  }
  /* Admitted: no problem type, because there is no problem. */
  return "";
}

/* =========================================================================
 * 1. PURE — lane classing
 * ========================================================================= */

kbc_classed kbc_lane_class_for(kbc_trust_class ceiling,
                               const kbc_fact_anchor *anchor,
                               const char *current_blob,
                               const char *current_text) {
  kbc_classed out;
  out.shifted = false;
  /* The per-fact cap. KBC_TRUST__COUNT is the "no cap" sentinel and is
   * normalised to exact so the min() below is total over four values rather
   * than five. */
  kbc_trust_class cap = anchor->cap;
  if (cap >= KBC_TRUST__COUNT) {
    cap = KBC_TRUST_EXACT;
  }
  out.line_start = anchor->line_start;
  out.line_end = anchor->line_end;

  /* No anchor at all: there is nothing to be exact about. */
  if (anchor->blob_sha == NULL) {
    out.class = KBC_TRUST_ORPHAN;
    out.reason = KBC_REASON_NO_ANCHOR;
    return out;
  }
  /* The file is gone. This is the floor, and it is an honest floor: the
   * claim is not silently dropped, it is reported as unresolvable. */
  if (current_blob == NULL) {
    out.class = KBC_TRUST_ORPHAN;
    out.reason = KBC_REASON_PATH_GONE;
    return out;
  }
  /* The blob the tool named is the blob on disk: the strongest claim, and
   * still capped by the lane ceiling and the per-fact cap. */
  if (strcmp(current_blob, anchor->blob_sha) == 0) {
    if (anchor->sha_source != NULL &&
        strcmp(anchor->sha_source, KBC_LANE_SHA_SOURCE_TOOL) == 0) {
      out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                      KBC_TRUST_EXACT);
      out.reason = KBC_REASON_BLOB_CURRENT;
    } else {
      /* The daemon attributed this blob itself rather than the tool naming
       * it, so it is one rung lower no matter how exact the bytes are. */
      out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                      KBC_TRUST_LIKELY);
      out.reason = KBC_REASON_SHA_ATTRIBUTED;
    }
    return out;
  }
  /* The blob moved. What is left is the snippet, and the Ladder's re-anchoring
   * rules: an exact re-anchor is as good as the blob was, a fuzzy one is a
   * candidate. Neither may rise above the cap, which is the whole point of
   * computing rather than caching. */
  if (current_text == NULL) {
    out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                    KBC_TRUST_ORPHAN);
    out.reason = KBC_REASON_CONTENT_UNREADABLE;
    return out;
  }
  if (anchor->snippet == NULL) {
    out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                    KBC_TRUST_ORPHAN);
    out.reason = KBC_REASON_NO_SNIPPET;
    return out;
  }
  if (strstr(current_text, anchor->snippet) != NULL) {
    out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                    KBC_TRUST_LIKELY);
    out.reason = KBC_REASON_REANCHORED_EXACT;
    return out;
  }
  out.class = kbc_trust_class_min(kbc_trust_class_min(ceiling, cap),
                                  KBC_TRUST_CANDIDATE);
  out.reason = KBC_REASON_REANCHORED_FUZZY;
  return out;
}

/* =========================================================================
 * 2. PERSISTENT — the sibling database
 *
 * The Rust original keeps these rows in the kb-code store, the one holding
 * `repos`/`files`/`symbols`/`highlights`. kb-c's kbc_store is the DOCUMENT
 * lane — `artifacts`/`chunks`/`comments`/`edges`/`history`/`moves`. The two
 * lanes are the same split the Rust workspace already makes as two crates
 * with two stores and two schema epochs, so this is a SIBLING database with
 * its own connection and its own ladder. Nothing below reads or writes
 * kbc_store, and no table here references a document id.
 * ========================================================================= */

struct kbc_workflow {
  sqlite3 *db;
  int schema_version;
};

int kbc_workflow_schema_version(const kbc_workflow *w) {
  return (w == NULL) ? -1 : w->schema_version;
}

void kbc_workflow_close(kbc_workflow *w) {
  if (w == NULL) {
    return;
  }
  if (w->db != NULL) {
    /* sqlite3_close_v2 rather than _v: a leaked statement would otherwise
     * turn a close into a no-op and leak the whole file handle. */
    (void)sqlite3_close_v2(w->db);
  }
  free(w);
}

/* Every SQL string runs as one statement, so a statement that fails has not
 * half-applied. `exec` is the only place raw SQL is run; every VALUE that
 * came from a caller goes through sqlite3_bind_* instead, never through a
 * format string (AGENTS.md rule 9). */
static kbc_status exec(sqlite3 *db, const char *sql, kbc_err *err) {
  char *msg = NULL;
  if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK) {
    kbc_status s = kbc_err_set(err, KBC_ERR_SQL, "workflow sql: %s",
                               msg ? msg : "unknown");
    sqlite3_free(msg);
    return s;
  }
  sqlite3_free(msg);
  return KBC_OK;
}

static kbc_status prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out,
                          kbc_err *err) {
  if (sqlite3_prepare_v2(db, sql, -1, out, NULL) != SQLITE_OK) {
    return kbc_err_set(err, KBC_ERR_SQL, "workflow prepare: %s",
                       sqlite3_errmsg(db));
  }
  return KBC_OK;
}

static kbc_status step_done(sqlite3 *db, sqlite3_stmt *st, kbc_err *err) {
  int rc = sqlite3_step(st);
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
    return kbc_err_set(err, KBC_ERR_SQL, "workflow step: %s",
                       sqlite3_errmsg(db));
  }
  return KBC_OK;
}

/* The migration ladder. One rung, one version, and the tables the original's
 * V0036__canvas.sql + V0039__tours_and_trails.sql declare, minus the three
 * that hold code-lane content kb-c does not index. `board_events` is the one
 * table with no counterpart in the original; the header says why. */
static const char *const KBC_WORKFLOW_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS wf_boards ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  repo TEXT NOT NULL,"
    "  slug TEXT NOT NULL,"
    "  kind TEXT NOT NULL DEFAULT 'board',"
    "  title TEXT NOT NULL,"
    "  description_md TEXT NOT NULL DEFAULT '',"
    "  status TEXT NOT NULL,"
    "  authored_ref TEXT,"
    "  content_hash TEXT NOT NULL,"
    "  revision INTEGER NOT NULL DEFAULT 1,"
    "  created_unix INTEGER NOT NULL,"
    "  updated_unix INTEGER NOT NULL,"
    "  UNIQUE (repo, slug)"
    ");"
    "CREATE TABLE IF NOT EXISTS wf_trails_state ("
    "  id INTEGER PRIMARY KEY,"
    "  mode TEXT NOT NULL,"
    "  changed_unix INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS wf_recipe_runs ("
    "  id TEXT PRIMARY KEY,"
    "  repo TEXT NOT NULL,"
    "  slug TEXT NOT NULL,"
    "  state TEXT NOT NULL,"
    "  error TEXT,"
    "  created_unix INTEGER NOT NULL,"
    "  updated_unix INTEGER NOT NULL"
    ");"
    /* One row per op, committed the moment the op finishes. This table is the
     * whole reason a half-completed run is answerable: the ops that DID
     * complete are on disk before the failing one returns, and a restart
     * finds them. */
    "CREATE TABLE IF NOT EXISTS wf_recipe_run_ops ("
    "  run_id TEXT NOT NULL REFERENCES wf_recipe_runs(id) ON DELETE CASCADE,"
    "  ordinal INTEGER NOT NULL,"
    "  op TEXT NOT NULL,"
    "  state TEXT NOT NULL,"
    "  rows INTEGER NOT NULL DEFAULT 0,"
    "  ms INTEGER NOT NULL DEFAULT 0,"
    "  detail TEXT,"
    "  PRIMARY KEY (run_id, ordinal)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_wf_runs_slug"
    "  ON wf_recipe_runs(repo, slug, created_unix DESC);"
    /* Append-only. One row per ACCEPTED transition, never rewritten and never
     * deleted with the board only if the board is deleted — hence the cascade
     * is deliberately absent: a board's history outliving the board is a
     * feature, and a caller that wants it gone asks for it by name. */
    "CREATE TABLE IF NOT EXISTS wf_board_events ("
    "  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  repo TEXT NOT NULL,"
    "  slug TEXT NOT NULL,"
    "  from_status TEXT NOT NULL,"
    "  to_status TEXT NOT NULL,"
    "  revision INTEGER NOT NULL,"
    "  at_unix INTEGER NOT NULL,"
    "  note TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_wf_events_board"
    "  ON wf_board_events(repo, slug, seq);";

kbc_status kbc_workflow_open(const char *path, kbc_workflow **out,
                             kbc_err *err) {
  if (path == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_workflow_open: NULL argument");
  }
  *out = NULL;
  kbc_workflow *w = calloc(1, sizeof(*w));
  if (w == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_workflow_open: out of memory");
  }
  /* SQLITE_OPEN_READWRITE|CREATE: the sibling database is created on first
   * use, exactly as the original's store is. */
  int rc = sqlite3_open_v2(path, &w->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
  if (rc != SQLITE_OK) {
    const char *msg = (w->db != NULL) ? sqlite3_errmsg(w->db) : "unknown";
    kbc_err_set(err, KBC_ERR_IO, "open %s: %s", path, msg);
    if (w->db != NULL) {
      sqlite3_close_v2(w->db);
    }
    free(w);
    return KBC_ERR_IO;
  }
  /* A 10s busy timeout is the default because the two-writer test drives
   * concurrent writers on purpose; without it the loser would get SQLITE_BUSY
   * instead of the KBC_ERR_CONFLICT the contract promises. */
  (void)sqlite3_busy_timeout(w->db, 10000);
  /* WAL: a reader never blocks a writer and a writer never sees a torn page,
   * which is what turns "two writers" into "one winner and one refusal"
   * rather than a corruption. */
  (void)sqlite3_exec(w->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
  (void)sqlite3_exec(w->db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

  kbc_status s = exec(w->db, KBC_WORKFLOW_SCHEMA_SQL, err);
  if (s != KBC_OK) {
    kbc_workflow_close(w);
    return s;
  }
  w->schema_version = (int)KBC_WORKFLOW_SCHEMA;
  *out = w;
  return KBC_OK;
}

kbc_status kbc_workflow_configure(kbc_workflow *w, int busy_timeout_ms,
                                  kbc_err *err) {
  if (w == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_workflow_configure: NULL workflow");
  }
  if (busy_timeout_ms < 0) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_workflow_configure: busy_timeout_ms %d is negative",
                       busy_timeout_ms);
  }
  int rc = sqlite3_busy_timeout(w->db, busy_timeout_ms);
  if (rc != SQLITE_OK) {
    return kbc_err_set(err, KBC_ERR_SQL, "busy_timeout: %s",
                       sqlite3_errmsg(w->db));
  }
  return KBC_OK;
}

/* Binds a NUL-terminated string, or SQL NULL for NULL. Every caller-supplied
 * string reaches SQLite through here, never through a format string. */
static void bind_text(sqlite3_stmt *st, int idx, const char *s) {
  if (s == NULL) {
    sqlite3_bind_null(st, idx);
  } else {
    sqlite3_bind_text(st, idx, s, -1, SQLITE_TRANSIENT);
  }
}

/* Copies a column into a fixed-size header field, refusing to TRUNCATE. A
 * silently shortened id is a board that answers to the wrong name, which is
 * exactly the class of quiet corruption this port is trying not to add. */
static kbc_status copy_bounded(char *dst, size_t cap, const unsigned char *src,
                               const char *what, kbc_err *err) {
  if (src == NULL) {
    dst[0] = '\0';
    return KBC_OK;
  }
  size_t n = strlen((const char *)src);
  if (n >= cap) {
    return kbc_err_set(err, KBC_ERR_SQL,
                       "%s: stored value is %zu bytes, over the %zu-byte field",
                       what, n, cap - 1);
  }
  memcpy(dst, src, n + 1);
  return KBC_OK;
}

/* =========================================================================
 * 2. PERSISTENT — the board machine
 * ========================================================================= */

/* `run_` + 12 hex, the same shape the original mints for a recipe run
 * (`run_` + 12 hex via annotations::short_random_hex). The generator is a
 * local one rather than a call into ids.c, because this file owns a database
 * whose ids do not appear in any other kb-c table and sharing a minting
 * routine across that boundary would couple two stores that are deliberately
 * independent. */
static void mint_id(char *dst, size_t cap, const char *prefix) {
  static const char hex[] = "0123456789abcdef";
  uint64_t seed = (uint64_t)kbc_now_ns();
  /* Mix in the address of a stack slot so two processes opened in the same
   * nanosecond do not collide on a fixed-seed sequence. */
  seed ^= (uint64_t)(uintptr_t)&seed;
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  int n = snprintf(dst, cap, "%s%012llx", prefix,
                   (unsigned long long)(seed & 0xffffffffffffULL));
  if (n < 0 || (size_t)n >= cap) {
    dst[0] = '\0';
    return;
  }
  /* Fold a second draw into the low nibbles: a 48-bit space is small enough
   * that the test's two-writer race should not depend on luck alone. */
  uint64_t r2 = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  for (int i = 0; i < 4; i++) {
    size_t at = strlen(prefix) + 8 + (size_t)i;
    if (at + 1 < cap) {
      dst[at] = hex[(r2 >> (unsigned)(i * 4)) & 0xf];
    }
  }
}

void kbc_board_free(kbc_board *b) {
  if (b == NULL) {
    return;
  }
  free(b->description_md);
  free(b->authored_ref);
  memset(b, 0, sizeof(*b));
}

void kbc_board_event_free(kbc_board_event *e) {
  if (e == NULL) {
    return;
  }
  free(e->note);
  memset(e, 0, sizeof(*e));
}

void kbc_board_events_free(kbc_board_event *e, size_t n) {
  if (e == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    kbc_board_event_free(&e[i]);
  }
  free(e);
}

/* Reads one board row into `out`. Assumes the statement is already positioned
 * on a row. */
static kbc_status board_from_row(sqlite3_stmt *st, kbc_board *out,
                                 kbc_err *err) {
  memset(out, 0, sizeof(*out));
  kbc_status s;
  if ((s = copy_bounded(out->id, sizeof(out->id), sqlite3_column_text(st, 0),
                        "board.id", err)) != KBC_OK) {
    return s;
  }
  if ((s = copy_bounded(out->slug, sizeof(out->slug),
                        sqlite3_column_text(st, 1), "board.slug", err)) != KBC_OK) {
    return s;
  }
  if ((s = copy_bounded(out->kind, sizeof(out->kind),
                        sqlite3_column_text(st, 2), "board.kind", err)) != KBC_OK) {
    return s;
  }
  if ((s = copy_bounded(out->title, sizeof(out->title),
                        sqlite3_column_text(st, 3), "board.title", err)) != KBC_OK) {
    return s;
  }
  const unsigned char *desc = sqlite3_column_text(st, 4);
  if (desc != NULL) {
    out->description_md = strdup((const char *)desc);
    if (out->description_md == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "board: out of memory");
    }
  }
  if ((s = kbc_board_status_parse((const char *)sqlite3_column_text(st, 5),
                                  &out->status, err)) != KBC_OK) {
    return s;
  }
  const unsigned char *ref = sqlite3_column_text(st, 6);
  if (ref != NULL) {
    out->authored_ref = strdup((const char *)ref);
    if (out->authored_ref == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "board: out of memory");
    }
  }
  if ((s = copy_bounded(out->content_hash, sizeof(out->content_hash),
                        sqlite3_column_text(st, 7), "board.content_hash",
                        err)) != KBC_OK) {
    return s;
  }
  out->revision = sqlite3_column_int64(st, 8);
  out->created_unix = sqlite3_column_int64(st, 9);
  out->updated_unix = sqlite3_column_int64(st, 10);
  return KBC_OK;
}

#define BOARD_COLUMNS \
  "id, slug, kind, title, description_md, status, authored_ref, content_hash, " \
  "revision, created_unix, updated_unix"

kbc_status kbc_board_get(kbc_workflow *w, const char *repo, const char *slug,
                         kbc_board *out, kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_board_get: NULL argument");
  }
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "SELECT " BOARD_COLUMNS " FROM wf_boards"
                         " WHERE repo = ?1 AND slug = ?2",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    s = board_from_row(st, out, err);
  } else if (rc == SQLITE_DONE) {
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "board \"%s\" in %s", slug, repo);
  } else {
    s = kbc_err_set(err, KBC_ERR_SQL, "kbc_board_get: %s",
                    sqlite3_errmsg(w->db));
  }
  sqlite3_finalize(st);
  return s;
}

kbc_status kbc_board_create(kbc_workflow *w, const char *repo, const char *slug,
                            const char *kind, const char *title,
                            const char *description_md, const char *authored_ref,
                            const char *content_hash, kbc_board_status status,
                            int64_t now_unix, kbc_board *out, kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL || title == NULL ||
      content_hash == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_board_create: NULL argument");
  }
  /* A create is an apply, and an apply may not author accepted/archived: those
   * are transitions a human makes, and a document that names one is refused
   * here rather than being allowed to mint its own approval. */
  if (!kbc_board_status_is_appliable(status)) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "board status: a create may not author %s — only "
                       "pending or draft are authored; accepted and archived "
                       "are reached by their own transitions",
                       kbc_board_status_str(status));
  }
  if (strlen(repo) > KBC_REPO_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID, "repo name is over %u bytes",
                       KBC_REPO_MAX);
  }
  if (strlen(slug) > KBC_BOARD_ID_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "board slug is %zu bytes, over the %u-byte cap",
                       strlen(slug), KBC_BOARD_ID_MAX);
  }
  if (strlen(title) > KBC_BOARD_TITLE_MAX) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "board title is %zu bytes, over the %u-byte cap",
                       strlen(title), KBC_BOARD_TITLE_MAX);
  }
  const char *k = (kind != NULL) ? kind : "board";
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "INSERT INTO wf_boards (repo, slug, kind, title,"
                         " description_md, status, authored_ref, content_hash,"
                         " revision, created_unix, updated_unix)"
                         " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,1,?9,?9)",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  bind_text(st, 3, k);
  bind_text(st, 4, title);
  bind_text(st, 5, (description_md != NULL) ? description_md : "");
  bind_text(st, 6, kbc_board_status_str(status));
  bind_text(st, 7, authored_ref);
  bind_text(st, 8, content_hash);
  sqlite3_bind_int64(st, 9, now_unix);
  if ((s = step_done(w->db, st, err)) != KBC_OK) {
    sqlite3_finalize(st);
    /* The UNIQUE(repo, slug) violation is a CONFLICT with a message naming
     * the offending pair, because a caller that lost a create race needs to
     * know WHICH pair, not "constraint failed". */
    if (sqlite3_extended_errcode(w->db) == SQLITE_CONSTRAINT_UNIQUE) {
      return kbc_err_set(err, KBC_ERR_CONFLICT,
                         "board \"%s\" in %s already exists", slug, repo);
    }
    return s;
  }
  sqlite3_finalize(st);
  if (out != NULL) {
    return kbc_board_get(w, repo, slug, out, err);
  }
  return KBC_OK;
}

kbc_status kbc_board_delete(kbc_workflow *w, const char *repo,
                            const char *slug, kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_board_delete: NULL argument");
  }
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "DELETE FROM wf_boards WHERE repo = ?1 AND slug = ?2",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    return kbc_err_set(err, KBC_ERR_SQL, "kbc_board_delete: %s",
                       sqlite3_errmsg(w->db));
  }
  if (sqlite3_changes(w->db) == 0) {
    return kbc_err_set(err, KBC_ERR_NOTFOUND, "board \"%s\" in %s", slug, repo);
  }
  return KBC_OK;
}

/* Appends one transition to the log. Called INSIDE the transition's
 * transaction, so the event and the status move together or not at all: a
 * status that moved with no event is a board that has already started to
 * forget. */
static kbc_status log_event(sqlite3 *db, const char *repo, const char *slug,
                            kbc_board_status from, kbc_board_status to,
                            int64_t revision, int64_t at_unix,
                            const char *note, kbc_err *err) {
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(db,
                         "INSERT INTO wf_board_events (repo, slug, from_status,"
                         " to_status, revision, at_unix, note)"
                         " VALUES (?1,?2,?3,?4,?5,?6,?7)",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  bind_text(st, 3, kbc_board_status_str(from));
  bind_text(st, 4, kbc_board_status_str(to));
  sqlite3_bind_int64(st, 5, revision);
  sqlite3_bind_int64(st, 6, at_unix);
  bind_text(st, 7, note);
  s = step_done(db, st, err);
  sqlite3_finalize(st);
  return s;
}

kbc_status kbc_board_events(kbc_workflow *w, const char *repo,
                            const char *slug, size_t limit,
                            kbc_board_event **out, size_t *n_out,
                            kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL || out == NULL || n_out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID, "kbc_board_events: NULL argument");
  }
  *out = NULL;
  *n_out = 0;
  /* One page of history is bounded so a board with a very long life cannot
   * make a read allocate without limit; `limit` 0 means "the caller's problem
   * is memory", which is a caller that asked for no limit. */
  const size_t cap = (limit == 0) ? 256u : limit;
  kbc_board_event *evs = calloc(cap, sizeof(*evs));
  if (evs == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_board_events: out of memory");
  }
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "SELECT seq, from_status, to_status, revision, at_unix,"
                         " note FROM wf_board_events"
                         " WHERE repo = ?1 AND slug = ?2 ORDER BY seq ASC"
                         " LIMIT ?3",
                         &st, err);
  if (s != KBC_OK) {
    free(evs);
    return s;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  sqlite3_bind_int64(st, 3, (int64_t)cap);
  size_t n = 0;
  while (n < cap) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      s = kbc_err_set(err, KBC_ERR_SQL, "kbc_board_events: %s",
                      sqlite3_errmsg(w->db));
      break;
    }
    kbc_board_event *e = &evs[n];
    e->seq = sqlite3_column_int64(st, 0);
    if ((s = kbc_board_status_parse((const char *)sqlite3_column_text(st, 1),
                                    &e->from, err)) != KBC_OK) {
      break;
    }
    if ((s = kbc_board_status_parse((const char *)sqlite3_column_text(st, 2),
                                    &e->to, err)) != KBC_OK) {
      break;
    }
    e->revision = sqlite3_column_int64(st, 3);
    e->at_unix = sqlite3_column_int64(st, 4);
    const unsigned char *note = sqlite3_column_text(st, 5);
    if (note != NULL) {
      e->note = strdup((const char *)note);
      if (e->note == NULL) {
        s = kbc_err_set(err, KBC_ERR_NOMEM, "kbc_board_events: out of memory");
        break;
      }
    }
    n++;
  }
  sqlite3_finalize(st);
  if (s != KBC_OK) {
    kbc_board_events_free(evs, n);
    return s;
  }
  *out = evs;
  *n_out = n;
  return KBC_OK;
}

kbc_status kbc_board_set_status(kbc_workflow *w, const char *repo,
                                const char *slug, kbc_board_status to,
                                int64_t now_unix, kbc_board *out,
                                kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_board_set_status: NULL argument");
  }
  if (to >= KBC_BOARD_STATUS__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "board status: to-state %d is not a board status",
                       (int)to);
  }
  /* BEGIN IMMEDIATE takes the write lock BEFORE the read below. Without it
   * two callers could both read `pending`, both decide accepted is legal, and
   * both write — the second overwriting the first with no trace. Taking the
   * write lock first makes the read-then-write one atomic step, which is the
   * whole of the two-writer guarantee. */
  kbc_status s;
  if ((s = exec(w->db, "BEGIN IMMEDIATE", err)) != KBC_OK) {
    return s;
  }
  kbc_board cur;
  memset(&cur, 0, sizeof(cur));
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db, "SELECT " BOARD_COLUMNS " FROM wf_boards"
                          " WHERE repo = ?1 AND slug = ?2",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    st = NULL;
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "board \"%s\" in %s", slug, repo);
    goto rollback;
  }
  s = board_from_row(st, &cur, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  /* The legality check happens INSIDE the transaction, on the status this
   * writer actually read. A refusal here rolls back, so it changes nothing —
   * not the status, not the revision, not the log. */
  if ((s = kbc_board_status_can_transition(cur.status, to, err)) != KBC_OK) {
    goto rollback;
  }
  int64_t next_rev = cur.revision + 1;
  /* The compare-and-swap: `revision = ?` in the WHERE clause means the write
   * lands only if nobody moved the board since this writer read it.
   *
   * It is BELT, not the load-bearing part: BEGIN IMMEDIATE above already
   * holds the write lock across the read, so under SQLite's locking this
   * clause can never fail. It stays because it is the statement-level
   * expression of the invariant, and because a refactor that moved the read
   * outside the transaction would then be caught here rather than becoming a
   * silent lost update. Measured, not assumed: dropping the clause entirely
   * kills no test, because the lock is doing the work. */
  if ((s = prepare(w->db,
                   "UPDATE wf_boards SET status = ?3, revision = ?4,"
                   " updated_unix = ?5"
                   " WHERE repo = ?1 AND slug = ?2 AND revision = ?6",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  bind_text(st, 3, kbc_board_status_str(to));
  sqlite3_bind_int64(st, 4, next_rev);
  sqlite3_bind_int64(st, 5, now_unix);
  sqlite3_bind_int64(st, 6, cur.revision);
  if ((s = step_done(w->db, st, err)) != KBC_OK) {
    sqlite3_finalize(st);
    st = NULL;
    goto rollback;
  }
  sqlite3_finalize(st);
  st = NULL;
  if (sqlite3_changes(w->db) == 0) {
    s = kbc_err_set(err, KBC_ERR_CONFLICT,
                    "board \"%s\" in %s: status %s -> %s lost a concurrent "
                    "write (revision moved under this writer)",
                    slug, repo, kbc_board_status_str(cur.status),
                    kbc_board_status_str(to));
    goto rollback;
  }
  if ((s = log_event(w->db, repo, slug, cur.status, to, next_rev, now_unix,
                     NULL, err)) != KBC_OK) {
    goto rollback;
  }
  if ((s = exec(w->db, "COMMIT", err)) != KBC_OK) {
    goto rollback;
  }
  kbc_board_free(&cur);
  if (out != NULL) {
    return kbc_board_get(w, repo, slug, out, err);
  }
  return KBC_OK;

rollback:
  if (st != NULL) {
    sqlite3_finalize(st);
  }
  kbc_board_free(&cur);
  /* A rollback on a transaction that already committed is harmless; the
   * failure is the one being returned, not this. */
  (void)exec(w->db, "ROLLBACK", NULL);
  return s;
}

kbc_status kbc_board_apply_status(kbc_workflow *w, const char *repo,
                                  const char *slug, const char *new_hash,
                                  kbc_board_status want, int64_t now_unix,
                                  bool *changed, kbc_board *out,
                                  kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL || new_hash == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_board_apply_status: NULL argument");
  }
  if (!kbc_board_status_is_appliable(want)) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "board status: an apply may not author %s — only "
                       "pending or draft are authored",
                       kbc_board_status_str(want));
  }
  if (changed != NULL) {
    *changed = false;
  }
  kbc_status s;
  if ((s = exec(w->db, "BEGIN IMMEDIATE", err)) != KBC_OK) {
    return s;
  }
  kbc_board cur;
  memset(&cur, 0, sizeof(cur));
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db, "SELECT " BOARD_COLUMNS " FROM wf_boards"
                          " WHERE repo = ?1 AND slug = ?2",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    st = NULL;
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "board \"%s\" in %s", slug, repo);
    goto rollback;
  }
  s = board_from_row(st, &cur, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  /* The idempotency contract, in ONE comparison rather than a field-by-field
   * diff that could disagree with itself (the original's own words). */
  if (strcmp(cur.content_hash, new_hash) == 0) {
    if ((s = exec(w->db, "COMMIT", err)) != KBC_OK) {
      goto rollback;
    }
    kbc_board_free(&cur);
    if (changed != NULL) {
      *changed = false;
    }
    if (out != NULL) {
      return kbc_board_get(w, repo, slug, out, err);
    }
    return KBC_OK;
  }
  int64_t next_rev = cur.revision + 1;
  bool status_reset = (cur.status != want);
  if ((s = prepare(w->db,
                   "UPDATE wf_boards SET content_hash = ?3, status = ?4,"
                   " revision = ?5, updated_unix = ?6"
                   " WHERE repo = ?1 AND slug = ?2 AND revision = ?7",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, repo);
  bind_text(st, 2, slug);
  bind_text(st, 3, new_hash);
  bind_text(st, 4, kbc_board_status_str(want));
  sqlite3_bind_int64(st, 5, next_rev);
  sqlite3_bind_int64(st, 6, now_unix);
  sqlite3_bind_int64(st, 7, cur.revision);
  if ((s = step_done(w->db, st, err)) != KBC_OK) {
    sqlite3_finalize(st);
    st = NULL;
    goto rollback;
  }
  sqlite3_finalize(st);
  st = NULL;
  if (sqlite3_changes(w->db) == 0) {
    s = kbc_err_set(err, KBC_ERR_CONFLICT,
                    "board \"%s\" in %s: apply lost a concurrent write "
                    "(revision moved under this writer)",
                    slug, repo);
    goto rollback;
  }
  /* A changed board that a human had accepted goes back to what the document
   * asked for. Preserving `accepted` would smuggle unreviewed content under
   * an approval a human gave to DIFFERENT bytes — the exact thing D21's
   * pending state exists to prevent. The reset is logged like any other
   * transition, so the history shows both the accept and its withdrawal. */
  if (status_reset) {
    if ((s = log_event(w->db, repo, slug, cur.status, want, next_rev, now_unix,
                       "apply reset", err)) != KBC_OK) {
      goto rollback;
    }
  }
  if ((s = exec(w->db, "COMMIT", err)) != KBC_OK) {
    goto rollback;
  }
  kbc_board_free(&cur);
  if (changed != NULL) {
    *changed = true;
  }
  if (out != NULL) {
    return kbc_board_get(w, repo, slug, out, err);
  }
  return KBC_OK;

rollback:
  if (st != NULL) {
    sqlite3_finalize(st);
  }
  kbc_board_free(&cur);
  (void)exec(w->db, "ROLLBACK", NULL);
  return s;
}

/* =========================================================================
 * 2. PERSISTENT — the trail opt-in row
 *
 * A volume that has never opted in has NO row, and that reads as `off` with
 * changed_unix 0. The default is the ABSENCE of a decision, not a decision
 * to record — so this function does not write a row to answer a read.
 * ========================================================================= */

kbc_status kbc_trail_state_get(kbc_workflow *w, kbc_trail_mode *out,
                               int64_t *changed_unix, kbc_err *err) {
  if (w == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_trail_state_get: NULL argument");
  }
  *out = KBC_TRAIL_OFF;
  if (changed_unix != NULL) {
    *changed_unix = 0;
  }
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "SELECT mode, changed_unix FROM wf_trails_state"
                         " WHERE id = 1",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    s = kbc_trail_mode_parse((const char *)sqlite3_column_text(st, 0), out, err);
    if (s == KBC_OK && changed_unix != NULL) {
      *changed_unix = sqlite3_column_int64(st, 1);
    }
  } else if (rc != SQLITE_DONE) {
    s = kbc_err_set(err, KBC_ERR_SQL, "kbc_trail_state_get: %s",
                    sqlite3_errmsg(w->db));
  }
  sqlite3_finalize(st);
  return s;
}

kbc_status kbc_trail_state_set(kbc_workflow *w, kbc_trail_mode mode,
                               int64_t now_unix, kbc_err *err) {
  if (w == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_trail_state_set: NULL workflow");
  }
  if (mode >= KBC_TRAIL_MODE__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "trail mode: mode %d is not a mode", (int)mode);
  }
  /* There is exactly ONE row, id 1, so the write is an upsert rather than an
   * insert. A second insert would be a UNIQUE violation the caller cannot
   * act on; the row is a singleton by design, not by luck. */
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db,
                         "INSERT INTO wf_trails_state (id, mode, changed_unix)"
                         " VALUES (1, ?1, ?2)"
                         " ON CONFLICT(id) DO UPDATE SET mode = excluded.mode,"
                         " changed_unix = excluded.changed_unix",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, kbc_trail_mode_str(mode));
  sqlite3_bind_int64(st, 2, now_unix);
  s = step_done(w->db, st, err);
  sqlite3_finalize(st);
  return s;
}

/* =========================================================================
 * 2. PERSISTENT — recipe runs
 *
 * The Rust original materialises a run ONLY after it has completed: routes.rs
 * `materialise_route` runs the recipe to the end, and only then INSERTs one
 * `recipe_runs` row holding the whole result JSON. A run that failed at step
 * 7 of 12 therefore leaves NO row at all — which is precisely the failure the
 * brief for this shard names: "a run that half-completed and left no record
 * is worse than one that refused to start."
 *
 * So this is the THIRD deliberate divergence, and the one with the clearest
 * justification: the run row is written BEFORE the first op, and each op's
 * outcome is committed as it finishes. A reader can always ask how far a run
 * got, and a process that dies mid-run leaves a `running` row naming the last
 * op that completed.
 * ========================================================================= */

void kbc_recipe_run_free(kbc_recipe_run *r) {
  if (r == NULL) {
    return;
  }
  free(r->error);
  memset(r, 0, sizeof(*r));
}

void kbc_run_op_free(kbc_run_op *op) {
  if (op == NULL) {
    return;
  }
  free(op->detail);
  memset(op, 0, sizeof(*op));
}

void kbc_run_ops_free(kbc_run_op *ops, size_t n) {
  if (ops == NULL) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    kbc_run_op_free(&ops[i]);
  }
  free(ops);
}

/* Reads one run row. Assumes the statement is positioned on a row. */
static kbc_status run_from_row(sqlite3_stmt *st, kbc_recipe_run *out,
                               kbc_err *err) {
  memset(out, 0, sizeof(*out));
  kbc_status s = copy_bounded(out->id, sizeof(out->id),
                              sqlite3_column_text(st, 0), "run.id", err);
  if (s != KBC_OK) {
    return s;
  }
  if ((s = copy_bounded(out->slug, sizeof(out->slug),
                        sqlite3_column_text(st, 1), "run.slug", err)) != KBC_OK) {
    return s;
  }
  if ((s = kbc_run_state_parse((const char *)sqlite3_column_text(st, 2),
                               &out->state, err)) != KBC_OK) {
    return s;
  }
  const unsigned char *e = sqlite3_column_text(st, 3);
  if (e != NULL) {
    out->error = strdup((const char *)e);
    if (out->error == NULL) {
      return kbc_err_set(err, KBC_ERR_NOMEM, "run: out of memory");
    }
  }
  out->created_unix = sqlite3_column_int64(st, 4);
  out->updated_unix = sqlite3_column_int64(st, 5);
  out->op_count = (size_t)sqlite3_column_int64(st, 6);
  return KBC_OK;
}

#define RUN_COLUMNS "id, slug, state, error, created_unix, updated_unix, " \
                    "(SELECT COUNT(*) FROM wf_recipe_run_ops o " \
                    " WHERE o.run_id = wf_recipe_runs.id)"

kbc_status kbc_recipe_run_begin(kbc_workflow *w, const char *repo,
                                const char *slug, int64_t now_unix,
                                kbc_recipe_run *out, kbc_err *err) {
  if (w == NULL || repo == NULL || slug == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_recipe_run_begin: NULL argument");
  }
  kbc_recipe_run r;
  memset(&r, 0, sizeof(r));
  mint_id(r.id, sizeof(r.id), "run_");
  if (r.id[0] == '\0') {
    return kbc_err_set(err, KBC_ERR_INTERNAL, "run id: mint produced nothing");
  }
  kbc_status s;
  if ((s = copy_bounded(r.slug, sizeof(r.slug), (const unsigned char *)slug,
                        "run.slug", err)) != KBC_OK) {
    return s;
  }
  r.state = KBC_RUN_RUNNING;
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db,
                   "INSERT INTO wf_recipe_runs (id, repo, slug, state,"
                   " error, created_unix, updated_unix)"
                     " VALUES (?1,?2,?3,?4,NULL,?5,?5)",
                     &st, err)) != KBC_OK) {
    return s;
  }
  bind_text(st, 1, r.id);
  bind_text(st, 2, repo);
  bind_text(st, 3, slug);
  bind_text(st, 4, kbc_run_state_str(KBC_RUN_RUNNING));
  sqlite3_bind_int64(st, 5, now_unix);
  if ((s = step_done(w->db, st, err)) != KBC_OK) {
    sqlite3_finalize(st);
    return s;
  }
  sqlite3_finalize(st);
  if (out != NULL) {
    return kbc_recipe_run_get(w, r.id, out, err);
  }
  return KBC_OK;
}

kbc_status kbc_recipe_run_get(kbc_workflow *w, const char *run_id,
                              kbc_recipe_run *out, kbc_err *err) {
  if (w == NULL || run_id == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_recipe_run_get: NULL argument");
  }
  sqlite3_stmt *st = NULL;
  kbc_status s = prepare(w->db, "SELECT " RUN_COLUMNS " FROM wf_recipe_runs"
                          " WHERE id = ?1",
                         &st, err);
  if (s != KBC_OK) {
    return s;
  }
  bind_text(st, 1, run_id);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    s = run_from_row(st, out, err);
  } else if (rc == SQLITE_DONE) {
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "run \"%s\"", run_id);
  } else {
    s = kbc_err_set(err, KBC_ERR_SQL, "kbc_recipe_run_get: %s",
                    sqlite3_errmsg(w->db));
  }
  sqlite3_finalize(st);
  return s;
}

kbc_status kbc_recipe_run_record_op(kbc_workflow *w, const char *run_id,
                                    const char *op, kbc_op_state state,
                                    int64_t rows, int64_t ms,
                                    const char *detail, kbc_err *err) {
  if (w == NULL || run_id == NULL || op == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_recipe_run_record_op: NULL argument");
  }
  if (state >= KBC_OP_STATE__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "op state: state %d is not an op state", (int)state);
  }
  if (strlen(op) >= 32u) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "op name \"%s\" is over the 31-byte cap", op);
  }
  /* The read of the run, the legality check and the insert are ONE
   * transaction. Reading the state first and writing second would leave a
   * window in which a second connection could close the run between the two,
   * and this function would then append an op to a run that had already
   * reported failure — a contradiction no reader can resolve. BEGIN IMMEDIATE
   * takes the write lock before the read, which closes that window the same
   * way it does for the board transition. */
  kbc_status s;
  if ((s = exec(w->db, "BEGIN IMMEDIATE", err)) != KBC_OK) {
    return s;
  }
  kbc_recipe_run run;
  memset(&run, 0, sizeof(run));
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db, "SELECT " RUN_COLUMNS " FROM wf_recipe_runs"
                          " WHERE id = ?1",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, run_id);
  if (sqlite3_step(st) != SQLITE_ROW) {
    sqlite3_finalize(st);
    st = NULL;
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "run \"%s\"", run_id);
    goto rollback;
  }
  s = run_from_row(st, &run, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  /* A terminal run takes no more ops. */
  if (run.state != KBC_RUN_RUNNING) {
    kbc_run_state frm = run.state;
    kbc_recipe_run_free(&run);
    s = kbc_err_set(err, KBC_ERR_CONFLICT,
                    "recipe run \"%s\": cannot record an op on a run that is "
                    "already %s",
                    run_id, kbc_run_state_str(frm));
    goto rollback_nofree;
  }
  /* The cap is checked HERE, against the count the row reports, so the bound
   * on this table's size is enforced by the writer rather than assumed by the
   * reader. A cap that is only documented is a cap that gets exceeded. */
  if (run.op_count >= KBC_MAX_RUN_OPS) {
    size_t have = run.op_count;
    kbc_recipe_run_free(&run);
    s = kbc_err_set(err, KBC_ERR_CONFLICT,
                    "run \"%s\" already has %zu recorded ops, at the %u cap — a "
                    "cap is a refusal, never a truncation",
                    run_id, have, KBC_MAX_RUN_OPS);
    goto rollback_nofree;
  }
  kbc_recipe_run_free(&run);
  if ((s = prepare(w->db,
                   "INSERT INTO wf_recipe_run_ops (run_id, ordinal, op, state,"
                   " rows, ms, detail) VALUES (?1,"
                   " (SELECT COALESCE(MAX(ordinal) + 1, 0) FROM"
                   "  wf_recipe_run_ops WHERE run_id = ?1),"
                   " ?2,?3,?4,?5,?6)",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, run_id);
  bind_text(st, 2, op);
  bind_text(st, 3, kbc_op_state_str(state));
  sqlite3_bind_int64(st, 4, rows);
  sqlite3_bind_int64(st, 5, ms);
  bind_text(st, 6, detail);
  s = step_done(w->db, st, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  if ((s = exec(w->db, "COMMIT", err)) != KBC_OK) {
    goto rollback;
  }
  return KBC_OK;

rollback:
  if (st != NULL) {
    sqlite3_finalize(st);
  }
  (void)exec(w->db, "ROLLBACK", NULL);
  return s;

rollback_nofree:
  /* The run struct was already freed on the path that jumps here, so this
   * label exists only to skip the second free. Two labels rather than one
   * because double-freeing a zeroed struct is still a double-free. */
  (void)exec(w->db, "ROLLBACK", NULL);
  return s;
}

kbc_status kbc_recipe_run_finish(kbc_workflow *w, const char *run_id,
                                 kbc_run_state final_state,
                                 const char *error, int64_t now_unix,
                                 kbc_err *err) {
  if (w == NULL || run_id == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_recipe_run_finish: NULL argument");
  }
  if (final_state >= KBC_RUN_STATE__COUNT) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "run state: state %d is not a run state",
                       (int)final_state);
  }
  /* Closing a run that is already running is the only legal move; a terminal
   * run is terminal. The check is inside the transaction, on the state this
   * writer read, so a late finisher cannot overwrite a recorded failure. */
  kbc_status s;
  if ((s = exec(w->db, "BEGIN IMMEDIATE", err)) != KBC_OK) {
    return s;
  }
  kbc_recipe_run run;
  memset(&run, 0, sizeof(run));
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db, "SELECT " RUN_COLUMNS " FROM wf_recipe_runs"
                          " WHERE id = ?1",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, run_id);
  int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    st = NULL;
    s = kbc_err_set(err, KBC_ERR_NOTFOUND, "run \"%s\"", run_id);
    goto rollback;
  }
  s = run_from_row(st, &run, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  if ((s = kbc_run_state_can_transition(run.state, final_state, err)) != KBC_OK) {
    goto rollback;
  }
  if ((s = prepare(w->db,
                   "UPDATE wf_recipe_runs SET state = ?2, error = ?3,"
                   " updated_unix = ?4 WHERE id = ?1",
                   &st, err)) != KBC_OK) {
    goto rollback;
  }
  bind_text(st, 1, run_id);
  bind_text(st, 2, kbc_run_state_str(final_state));
  bind_text(st, 3, error);
  sqlite3_bind_int64(st, 4, now_unix);
  s = step_done(w->db, st, err);
  sqlite3_finalize(st);
  st = NULL;
  if (s != KBC_OK) {
    goto rollback;
  }
  if ((s = exec(w->db, "COMMIT", err)) != KBC_OK) {
    goto rollback;
  }
  kbc_recipe_run_free(&run);
  return KBC_OK;

rollback:
  if (st != NULL) {
    sqlite3_finalize(st);
  }
  kbc_recipe_run_free(&run);
  (void)exec(w->db, "ROLLBACK", NULL);
  return s;
}

kbc_status kbc_recipe_run_ops(kbc_workflow *w, const char *run_id,
                              kbc_run_op **ops_out, size_t *n_out,
                              kbc_err *err) {
  if (w == NULL || run_id == NULL || ops_out == NULL || n_out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_recipe_run_ops: NULL argument");
  }
  *ops_out = NULL;
  *n_out = 0;
  kbc_recipe_run run;
  kbc_status s = kbc_recipe_run_get(w, run_id, &run, err);
  if (s != KBC_OK) {
    return s;
  }
  size_t n = run.op_count;
  kbc_recipe_run_free(&run);
  if (n == 0) {
    return KBC_OK;
  }
  /* A run's op count is bounded by KBC_MAX_RUN_OPS at record time, so this
   * allocation is bounded by a constant, not by a column a caller controls. */
  if (n > KBC_MAX_RUN_OPS) {
    return kbc_err_set(err, KBC_ERR_INTERNAL,
                       "run \"%s\" has %zu recorded ops, over the %u cap — the "
                       "store and the cap disagree",
                       run_id, n, KBC_MAX_RUN_OPS);
  }
  kbc_run_op *ops = calloc(n, sizeof(*ops));
  if (ops == NULL) {
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_recipe_run_ops: out of memory");
  }
  sqlite3_stmt *st = NULL;
  if ((s = prepare(w->db,
                   "SELECT ordinal, op, state, rows, ms, detail"
                   " FROM wf_recipe_run_ops WHERE run_id = ?1"
                   " ORDER BY ordinal ASC",
                   &st, err)) != KBC_OK) {
    free(ops);
    return s;
  }
  bind_text(st, 1, run_id);
  size_t got = 0;
  while (got < n) {
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      s = kbc_err_set(err, KBC_ERR_SQL, "kbc_recipe_run_ops: %s",
                      sqlite3_errmsg(w->db));
      break;
    }
    kbc_run_op *op = &ops[got];
    op->ordinal = (int)sqlite3_column_int64(st, 0);
    if ((s = copy_bounded(op->op, sizeof(op->op), sqlite3_column_text(st, 1),
                          "op.name", err)) != KBC_OK) {
      break;
    }
    if ((s = kbc_op_state_parse((const char *)sqlite3_column_text(st, 2),
                                &op->state, err)) != KBC_OK) {
      break;
    }
    op->rows = sqlite3_column_int64(st, 3);
    op->ms = sqlite3_column_int64(st, 4);
    const unsigned char *d = sqlite3_column_text(st, 5);
    if (d != NULL) {
      op->detail = strdup((const char *)d);
      if (op->detail == NULL) {
        s = kbc_err_set(err, KBC_ERR_NOMEM, "kbc_recipe_run_ops: out of memory");
        break;
      }
    }
    got++;
  }
  sqlite3_finalize(st);
  if (s != KBC_OK) {
    kbc_run_ops_free(ops, got);
    return s;
  }
  *ops_out = ops;
  *n_out = got;
  return KBC_OK;
}

/* =========================================================================
 * 3. DERIVED — layout and the JSON Canvas export
 *
 * Both are pure functions of an already-read board. They run no query,
 * resolve no anchor and read no file, so an export can never disagree with
 * the board it came from — which is the only reason an export is worth having
 * at all.
 * ========================================================================= */

void kbc_export_node_free(kbc_export_node *n) {
  if (n == NULL) {
    return;
  }
  free(n->title);
  free(n->body_md);
  free(n->address);
  free(n->code_path);
  free(n->url);
  free(n->query);
  memset(n, 0, sizeof(*n));
}

void kbc_export_nodes_free(kbc_export_node *n, size_t count) {
  if (n == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    kbc_export_node_free(&n[i]);
  }
  free(n);
}

void kbc_export_edges_free(kbc_export_edge *e, size_t count) {
  if (e == NULL) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(e[i].label);
    free(e[i].trust);
  }
  free(e);
}

/* Index of `id` in `nodes`, or SIZE_MAX. Linear rather than a hash: a board
 * is capped at KBC_MAX_BOARD_NODES, and 200 linear steps per edge beats the
 * allocation and the failure mode of a hash table that can be exhausted. */
static size_t node_index(const kbc_layout_node *nodes, size_t n,
                         const char *id) {
  for (size_t i = 0; i < n; i++) {
    if (strcmp(nodes[i].id, id) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

static bool pin_index(const kbc_pin *pins, size_t npins, const char *id) {
  for (size_t i = 0; i < npins; i++) {
    if (strcmp(pins[i].id, id) == 0) {
      return true;
    }
  }
  return false;
}

kbc_status kbc_board_layout(const kbc_layout_node *nodes, size_t n,
                            const kbc_layout_edge *edges, size_t ne,
                            const char *const *steps, size_t nsteps,
                            const kbc_pin *pins, size_t npins,
                            kbc_placed *out, kbc_err *err) {
  if (nodes == NULL || out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_board_layout: NULL node or out array");
  }
  if (n == 0) {
    return KBC_OK;
  }
  if (n > KBC_MAX_BOARD_NODES) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "board layout: %zu nodes is over the %u-node cap",
                       n, KBC_MAX_BOARD_NODES);
  }
  /* Layer assignment by bounded longest-path relax over the edge DAG. The
   * bound is `n` rounds rather than a visited set: a visited set's result
   * depends on traversal order, and this must not. A cycle simply stops
   * contributing once no depth changes, which terminates. */
  size_t *layer = calloc(n, sizeof(*layer));
  /* Adjacency in authored order, so the relax below visits edges in the order
   * the author wrote them and a cycle always breaks the same way. */
  size_t *adj_from = calloc(ne > 0 ? ne : 1, sizeof(*adj_from));
  size_t *adj_to = calloc(ne > 0 ? ne : 1, sizeof(*adj_to));
  size_t adj_n = 0;
  if (layer == NULL || adj_from == NULL || adj_to == NULL) {
    free(layer);
    free(adj_from);
    free(adj_to);
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_board_layout: out of memory");
  }
  for (size_t e = 0; e < ne; e++) {
    size_t a = node_index(nodes, n, edges[e].from);
    size_t b = node_index(nodes, n, edges[e].to);
    /* An edge naming a node that is not on this board is dropped, and a self
     * edge is dropped: it can never move a layer. */
    if (a == (size_t)-1 || b == (size_t)-1 || a == b) {
      continue;
    }
    adj_from[adj_n] = a;
    adj_to[adj_n] = b;
    adj_n++;
  }
  for (size_t round = 0; round < n; round++) {
    bool moved = false;
    for (size_t e = 0; e < adj_n; e++) {
      size_t a = adj_from[e];
      size_t b = adj_to[e];
      if (layer[b] < layer[a] + 1) {
        layer[b] = layer[a] + 1;
        moved = true;
      }
    }
    if (!moved) {
      break;
    }
  }
  /* Row order within a layer: step order first — a walkthrough's own reading
   * order is the best row order there is — then authored order. */
  size_t *rank = calloc(n, sizeof(*rank));
  if (rank == NULL) {
    free(layer);
    free(adj_from);
    free(adj_to);
    return kbc_err_set(err, KBC_ERR_NOMEM, "kbc_board_layout: out of memory");
  }
  for (size_t i = 0; i < n; i++) {
    rank[i] = (size_t)-1; /* "not in any step" sorts last. */
  }
  for (size_t s = 0; s < nsteps; s++) {
    if (steps[s] == NULL) {
      continue;
    }
    size_t idx = node_index(nodes, n, steps[s]);
    if (idx != (size_t)-1 && rank[idx] == (size_t)-1) {
      rank[idx] = s;
    }
  }
  /* Emit the pins first: an authored position is never overridden by a
   * derived one, and writing them first makes that structural. */
  for (size_t p = 0; p < npins; p++) {
    size_t idx = node_index(nodes, n, pins[p].id);
    if (idx == (size_t)-1) {
      continue;
    }
    out[idx].x = pins[p].x;
    out[idx].y = pins[p].y;
    out[idx].w = KBC_CANVAS_CARD_W;
    out[idx].h = KBC_CANVAS_CARD_H;
  }
  /* Row index within a layer = how many of this layer's unpinned members sort
   * before this one by (step_rank, authored_index). Counting rather than
   * sorting is deliberate: n is capped at KBC_MAX_BOARD_NODES so the O(n^2) is
   * bounded by a constant, and it removes the comparator — and with it any
   * chance of two nodes claiming the same row. */
  for (size_t i = 0; i < n; i++) {
    if (pin_index(pins, npins, nodes[i].id)) {
      continue;
    }
    size_t l = layer[i];
    size_t r = 0;
    for (size_t j = 0; j < n; j++) {
      if (j == i || pin_index(pins, npins, nodes[j].id)) {
        continue;
      }
      if (layer[j] != l) {
        continue;
      }
      /* (rank, index) lexicographic: a step node leads, authored order breaks
       * the tie. rank[] holds SIZE_MAX for "not in any step", which the
       * unsigned comparison sorts last exactly as the original's
       * `unwrap_or(usize::MAX)` does. */
      if (rank[j] < rank[i] || (rank[j] == rank[i] && j < i)) {
        r++;
      }
    }
    out[i].x = (double)l * (KBC_CANVAS_CARD_W + KBC_CANVAS_COL_GAP);
    out[i].y = (double)r * (KBC_CANVAS_CARD_H + KBC_CANVAS_ROW_GAP);
    out[i].w = KBC_CANVAS_CARD_W;
    out[i].h = KBC_CANVAS_CARD_H;
  }
  free(rank);
  free(layer);
  free(adj_from);
  free(adj_to);
  /* The postcondition the JSON Canvas spec states, checked on the way out
   * rather than assumed: every node was written exactly once above (a pin in
   * the pin pass, everything else in this pass), so this cannot fire — which
   * is the point. A board node with no geometry is not a JSON Canvas node, and
   * the failure mode if one ever were emitted is a file no other app can
   * open, so the invariant is asserted where the file is built. */
  for (size_t i = 0; i < n; i++) {
    if (!(out[i].w > 0.0) || !(out[i].h > 0.0)) {
      return kbc_err_set(err, KBC_ERR_INTERNAL,
                         "board layout: node \"%s\" was left without geometry",
                         nodes[i].id);
    }
  }
  return KBC_OK;
}

/* The spec's coordinates are integers, and it says so. The Rust original
 * emits `p.x.round() as i64` for exactly this reason; emitting the double
 * would produce a valid-looking file that a strict parser rejects. Rounding
 * is half-away-from-zero for .5 and therefore NOT the C library's
 * banker's rounding, so it is written out rather than delegated. */
static long long round_half_away(double v) {
  return (long long)(v < 0.0 ? -(double)(long long)(-v + 0.5)
                            : (double)(long long)(v + 0.5));
}

/* Appends a JSON string literal. Every byte a caller can put in a title, a
 * body or a URL goes through here, so a board node carrying a quote or a
 * newline cannot break out of the document (AGENTS.md rule 9). */
/* Appends a JSON VALUE: null for NULL, else a quoted, escaped string. */
static kbc_status json_str(kbc_str *out, const char *s) {
  if (s == NULL) {
    return kbc_str_puts(out, "null");
  }
  return kbc_str_append_json_string(out, s, strlen(s));
}

/* Appends a KEY: escaped, but WITHOUT the surrounding quotes the value form
 * adds. A key that went through json_str would come out `""x""` — which is
 * still parseable by nothing, and is the kind of near-miss a test that only
 * greps for `x` would sail straight past. */
static kbc_status json_key(kbc_str *out, const char *key) {
  return kbc_str_append_json_string(out, key, strlen(key));
}

static kbc_status json_kv_str(kbc_str *out, const char *key, const char *val) {
  kbc_status s = json_key(out, key);
  if (s != KBC_OK) {
    return s;
  }
  if ((s = kbc_str_putc(out, ':')) != KBC_OK) {
    return s;
  }
  return json_str(out, val);
}

static kbc_status json_kv_int(kbc_str *out, const char *key, long long v) {
  /* json_key already emits the surrounding quotes, so this appends only the
   * colon and the number. */
  kbc_status s = json_key(out, key);
  if (s != KBC_OK) {
    return s;
  }
  return kbc_str_printf(out, ":%lld", v);
}

/* The plain-text body a `text`-typed node carries — the same assembly the
 * Markdown export makes, minus the headings. Authored Markdown is stored
 * INERT: this exporter never renders it, only ever carries it as bytes. */
static kbc_status text_body(kbc_str *out, const kbc_export_node *n) {
  kbc_status s = KBC_OK;
  if (n->title != NULL && n->title[0] != '\0') {
    if ((s = kbc_str_printf(out, "**%s**\n\n", n->title)) != KBC_OK) {
      return s;
    }
  }
  if ((s = kbc_str_printf(out, "`%s` · %s · %s\n", n->kind, n->state,
                          (n->address != NULL) ? n->address : "")) != KBC_OK) {
    return s;
  }
  if (n->body_md != NULL && (s = kbc_str_printf(out, "\n%s\n", n->body_md)) != KBC_OK) {
    return s;
  }
  if (n->query != NULL &&
      (s = kbc_str_printf(out, "\nQuery: `%s`\n", n->query)) != KBC_OK) {
    return s;
  }
  return s;
}

kbc_status kbc_board_export_jsoncanvas(const char *repo, const char *slug,
                                       const char *title, kbc_board_status status,
                                       int64_t revision,
                                       const kbc_export_node *nodes,
                                       size_t n_nodes,
                                       const kbc_export_edge *edges,
                                       size_t n_edges,
                                       const char *const *steps,
                                       size_t n_steps, kbc_str *out,
                                       kbc_err *err) {
  if (out == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_board_export_jsoncanvas: NULL output");
  }
  if (n_nodes > KBC_MAX_BOARD_NODES) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "board export: %zu nodes is over the %u-node cap",
                       n_nodes, KBC_MAX_BOARD_NODES);
  }
  if (n_edges > KBC_MAX_BOARD_EDGES) {
    return kbc_err_set(err, KBC_ERR_CONFLICT,
                       "board export: %zu edges is over the %u-edge cap",
                       n_edges, KBC_MAX_BOARD_EDGES);
  }
  /* `steps` may be NULL: a board with no walkthrough order is an ordinary
   * board, and "no steps" is a real state rather than a malformed call. Only
   * a node array that is missing while a node count says otherwise is an
   * error, because then the count is a lie. */
  if (n_nodes > 0 && nodes == NULL) {
    return kbc_err_set(err, KBC_ERR_INVALID,
                       "kbc_board_export_jsoncanvas: %zu nodes claimed but no "
                       "node array given",
                       n_nodes);
  }
  /* Layout first, and a layout failure ABORTS the export rather than
   * emitting a node with default geometry. A file with one default-positioned
   * node is a file that silently lost a card, which is the failure mode the
   * whole "an orphan is shown, never dropped" posture exists to avoid. */
  kbc_layout_node *lnodes = NULL;
  kbc_layout_edge *ledges = NULL;
  kbc_placed *placed = NULL;
  if (n_nodes > 0) {
    lnodes = calloc(n_nodes, sizeof(*lnodes));
    ledges = calloc(n_edges > 0 ? n_edges : 1, sizeof(*ledges));
    placed = calloc(n_nodes, sizeof(*placed));
    if (lnodes == NULL || ledges == NULL || placed == NULL) {
      free(lnodes);
      free(ledges);
      free(placed);
      return kbc_err_set(err, KBC_ERR_NOMEM, "board export: out of memory");
    }
    for (size_t i = 0; i < n_nodes; i++) {
      kbc_status cs = copy_bounded(lnodes[i].id, sizeof(lnodes[i].id),
                                   (const unsigned char *)nodes[i].id,
                                   "node.id", err);
      if (cs != KBC_OK) {
        free(lnodes);
        free(ledges);
        free(placed);
        return cs;
      }
      lnodes[i].is_group = (strcmp(nodes[i].kind, "group") == 0);
    }
    for (size_t i = 0; i < n_edges; i++) {
      kbc_status cs = copy_bounded(ledges[i].from, sizeof(ledges[i].from),
                                   (const unsigned char *)edges[i].from,
                                   "edge.from", err);
      if (cs == KBC_OK) {
        cs = copy_bounded(ledges[i].to, sizeof(ledges[i].to),
                          (const unsigned char *)edges[i].to, "edge.to", err);
      }
      if (cs != KBC_OK) {
        free(lnodes);
        free(ledges);
        free(placed);
        return cs;
      }
    }
  }
  /* An empty board has nothing to lay out, and kbc_board_layout refuses a
   * NULL node array rather than reading it — so the call is skipped rather
   * than fed a dummy. A swept-away board must still export as a valid empty
   * canvas; refusing to export it would be the opposite of honest. */
  kbc_status s = KBC_OK;
  if (n_nodes > 0) {
    s = kbc_board_layout(lnodes, n_nodes, ledges, n_edges, steps, n_steps,
                         NULL, 0, placed, err);
  }
  free(lnodes);
  free(ledges);
  if (s != KBC_OK) {
    free(placed);
    return s;
  }
  kbc_str buf;
  kbc_str_init(&buf);
  if ((s = kbc_str_puts(&buf, "{\n  \"nodes\": [")) != KBC_OK) {
    goto fail;
  }
  /* An empty list is written as [] rather than as a bracket pair with a
   * newline between: same JSON, and a reader (or a grep) sees the difference
   * between "no nodes" and "nodes not rendered". */
  for (size_t i = 0; i < n_nodes; i++) {
    if (i > 0 && (s = kbc_str_puts(&buf, ",")) != KBC_OK) {
      goto fail;
    }
    if ((s = kbc_str_puts(&buf, "\n    {\"id\":")) != KBC_OK ||
        (s = json_str(&buf, nodes[i].id)) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK) {
      goto fail;
    }
    /* The four coordinates the spec REQUIRES on every node. They are emitted
     * unconditionally, before anything optional, so a reader of this file can
     * see the spec's own requirement satisfied by construction rather than by
     * a later fallback. */
    if ((s = json_kv_int(&buf, "x", round_half_away(placed[i].x))) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_int(&buf, "y", round_half_away(placed[i].y))) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_int(&buf, "width", round_half_away(placed[i].w))) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_int(&buf, "height", round_half_away(placed[i].h))) != KBC_OK) {
      goto fail;
    }
    /* Extension fields, namespaced so the spec's extensibility rule applies
     * and another app ignores them. This is the ONLY place a resolution state
     * or a trust class survives the round trip. */
    if ((s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_str(&buf, "kbc_kind", nodes[i].kind)) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_str(&buf, "kbc_state", nodes[i].state)) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_str(&buf, "kbc_reason", nodes[i].reason)) != KBC_OK) {
      goto fail;
    }
    if (nodes[i].address != NULL) {
      if ((s = kbc_str_putc(&buf, ',')) != KBC_OK ||
          (s = json_kv_str(&buf, "kbc_address", nodes[i].address)) != KBC_OK) {
        goto fail;
      }
    }
    /* `type` plus the one field that type requires. A JSON Canvas node
     * without a type is not a node. */
    if (strcmp(nodes[i].kind, "group") == 0) {
      const char *label = (nodes[i].title != NULL && nodes[i].title[0] != '\0')
                              ? nodes[i].title
                              : nodes[i].id;
      if ((s = kbc_str_puts(&buf, ",\"type\":\"group\",\"label\":")) != KBC_OK ||
          (s = json_str(&buf, label)) != KBC_OK) {
        goto fail;
      }
    } else if (strcmp(nodes[i].kind, "link") == 0) {
      if ((s = kbc_str_puts(&buf, ",\"type\":\"link\",\"url\":")) != KBC_OK ||
          (s = json_str(&buf, nodes[i].url)) != KBC_OK) {
        goto fail;
      }
    } else if (strcmp(nodes[i].kind, "code") == 0) {
      /* A JSON Canvas `file` path is vault-relative and its `subpath` is a
       * Markdown heading anchor, so a line range cannot ride it. The path
       * goes in `file` and the range in an extension field. */
      if ((s = kbc_str_puts(&buf, ",\"type\":\"file\",\"file\":")) != KBC_OK ||
          (s = json_str(&buf, nodes[i].code_path)) != KBC_OK) {
        goto fail;
      }
      if (nodes[i].range_start > 0 && nodes[i].range_end > 0) {
        if ((s = kbc_str_printf(&buf, ",\"kbc_range\":[%d,%d]",
                                nodes[i].range_start,
                                nodes[i].range_end)) != KBC_OK) {
          goto fail;
        }
      }
    } else {
      kbc_str body;
      kbc_str_init(&body);
      s = text_body(&body, &nodes[i]);
      if (s != KBC_OK) {
        kbc_str_free(&body);
        goto fail;
      }
      s = kbc_str_puts(&buf, ",\"type\":\"text\",\"text\":");
      if (s == KBC_OK) {
        s = kbc_str_append_json_string(&buf, body.ptr, body.len);
      }
      kbc_str_free(&body);
      if (s != KBC_OK) {
        goto fail;
      }
    }
    if ((s = kbc_str_putc(&buf, '}')) != KBC_OK) {
      goto fail;
    }
  }
  /* Close the node list: a newline + indent before "]" when there were
   * nodes, and nothing extra when there were none, so an empty board reads
   * "nodes": [] rather than a bracket pair with a blank line in it. */
  if (n_nodes > 0 && (s = kbc_str_puts(&buf, "\n  ]")) != KBC_OK) {
    goto fail;
  } else if (n_nodes == 0 && (s = kbc_str_putc(&buf, ']')) != KBC_OK) {
    goto fail;
  }
  if ((s = kbc_str_puts(&buf, ",\n  \"edges\": [")) != KBC_OK) {
    goto fail;
  }
  for (size_t i = 0; i < n_edges; i++) {
    char eid[32];
    int n = snprintf(eid, sizeof(eid), "e%zu", i);
    if (n < 0) {
      s = kbc_err_set(err, KBC_ERR_INTERNAL, "board export: edge id");
      goto fail;
    }
    if (i > 0 && (s = kbc_str_putc(&buf, ',')) != KBC_OK) {
      goto fail;
    }
    if ((s = kbc_str_printf(&buf, "\n    {\"id\":\"%s\",\"fromNode\":", eid)) != KBC_OK ||
        (s = json_str(&buf, edges[i].from)) != KBC_OK ||
        (s = kbc_str_puts(&buf, ",\"toNode\":")) != KBC_OK ||
        (s = json_str(&buf, edges[i].to)) != KBC_OK ||
        /* toEnd is what makes an edge an arrow; the original pins it. */
        (s = kbc_str_puts(&buf, ",\"toEnd\":\"arrow\",")) != KBC_OK ||
        (s = json_kv_str(&buf, "kbc_kind", edges[i].kind)) != KBC_OK ||
        (s = kbc_str_putc(&buf, ',')) != KBC_OK ||
        (s = json_kv_str(&buf, "kbc_provenance", edges[i].provenance)) != KBC_OK) {
      goto fail;
    }
    /* The label is the kind, plus the author's own label when there is one. */
    if (edges[i].label != NULL && edges[i].label[0] != '\0') {
      kbc_str lab;
      kbc_str_init(&lab);
      s = kbc_str_printf(&lab, "%s — %s", edges[i].kind, edges[i].label);
      if (s == KBC_OK) {
        s = kbc_str_puts(&buf, ",\"label\":");
      }
      if (s == KBC_OK) {
        s = kbc_str_append_json_string(&buf, lab.ptr, lab.len);
      }
      kbc_str_free(&lab);
      if (s != KBC_OK) {
        goto fail;
      }
    } else if ((s = kbc_str_puts(&buf, ",\"label\":")) != KBC_OK ||
               (s = json_str(&buf, edges[i].kind)) != KBC_OK) {
      goto fail;
    }
    /* A derived edge carries its trust class; an authored one carries none —
     * a human's arrow is not a claim the index can back. */
    if (edges[i].trust != NULL) {
      if ((s = kbc_str_putc(&buf, ',')) != KBC_OK ||
          (s = json_kv_str(&buf, "kbc_trust", edges[i].trust)) != KBC_OK) {
        goto fail;
      }
    }
    if ((s = kbc_str_putc(&buf, '}')) != KBC_OK) {
      goto fail;
    }
  }
  if (n_edges > 0 && (s = kbc_str_puts(&buf, "\n  ]")) != KBC_OK) {
    goto fail;
  } else if (n_edges == 0 && (s = kbc_str_putc(&buf, ']')) != KBC_OK) {
    goto fail;
  }
  if ((s = kbc_str_puts(&buf, ",\n  \"kbc_schema\":")) != KBC_OK ||
      (s = json_str(&buf, KBC_BOARD_SCHEMA)) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_slug\":")) != KBC_OK ||
      (s = json_str(&buf, slug)) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_repo\":")) != KBC_OK ||
      (s = json_str(&buf, repo)) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_title\":")) != KBC_OK ||
      (s = json_str(&buf, title)) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_status\":")) != KBC_OK ||
      (s = json_str(&buf, kbc_board_status_str(status))) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_revision\":")) != KBC_OK ||
      (s = kbc_str_printf(&buf, "%lld", (long long)revision)) != KBC_OK ||
      (s = kbc_str_puts(&buf, ",\n  \"kbc_snapshot\":")) != KBC_OK ||
      (s = json_str(&buf, KBC_BOARD_SNAPSHOT_CAPTION)) != KBC_OK ||
      (s = kbc_str_puts(&buf,
                        ",\n  \"kbc_layout\": \"derived by kb-code for THIS "
                        "export only; the live board is coordinate-free plus "
                        "pins and the SPA owns its layout\"\n}\n")) != KBC_OK) {
    goto fail;
  }
  free(placed);
  /* `out` is appended to, not replaced, so the local buffer is copied and
   * then released. Freeing it on the failure path below but not here is the
   * kind of asymmetry ASan is for. */
  kbc_status rs = kbc_str_append(out, buf.ptr, buf.len);
  kbc_str_free(&buf);
  return rs;

fail:
  kbc_str_free(&buf);
  free(placed);
  return s;
}
