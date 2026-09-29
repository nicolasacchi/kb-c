/* workflow.h — the kb-code-lane WORKFLOW STATE MACHINES in C.
 *
 * Ported from the Rust shard boards/ lanes/ recipe/ tours/ trails/ mirror/ of
 * crates/kb-code-server. This header is the whole contract; src/workflow.c is
 * the only implementation and tests/test_workflow.c the only consumer.
 *
 * ── WHY A SIBLING DATABASE AND NOT TABLES IN kbc_store ──────────────────────
 *
 * The Rust original keeps these rows in the kb-code store, the one that holds
 * `repos`/`files`/`symbols`/`highlights` (V0001__initial.sql) — a CODE lane.
 * kb-c's `kbc_store` holds a DISJOINT set: `artifacts`/`chunks`/`comments`/
 * `edges`/`doc_metas`/`history`/`moves` — a DOCUMENT lane. Not one table of
 * this subsystem is reachable from a document id, and not one table of
 * kbc_store is reachable from a board slug. The two lanes are the same split
 * the Rust workspace already makes: kb_core (documents) and kb-code-server
 * (code) are SEPARATE CRATES with separate stores, separate migrations and
 * separate schema epochs. Putting boards in the document store would make
 * kb-c one store where the original has two, and would buy nothing: the
 * workflow tables reference no artifact.
 *
 * So: a SIBLING database, opened here with its own sqlite3 handle, exactly as
 * the original does. The migration ladder is this file's, not store.c's.
 *
 * ── WHAT THESE MACHINES ARE ──────────────────────────────────────────────────
 *
 * Four machines, in the order they matter:
 *
 *   1. BOARD STATUS   pending | draft | accepted | archived  — D21's human
 *      gate: an agent-proposed board is PENDING until a human accepts it.
 *   2. TRAIL MODE     off | recording | paused              — D17's opt-in:
 *      the default is the ABSENCE of a decision, never a decision to record.
 *   3. LANE TRUST     orphan < candidate < likely < exact   — computed per
 *      request, never persisted, so a fact whose blob has moved can never
 *      read back as fresh.
 *   4. RECIPE RUN     running -> done | failed | aborted     — with a row per
 *      op, so a run that half-completed leaves a record of what completed.
 *
 * ── THE RULE THE WHOLE FILE EXISTS TO KEEP ───────────────────────────────────
 *
 * A machine either REFUSES an illegal transition and changes nothing, or it
 * is not a state machine. Every refusal here names BOTH the from-state and the
 * to-state, because a message that says only "invalid" is not actionable by
 * the caller that has to decide what to do next. And every machine's state is
 * on disk, because a board that exists only in memory is a demo.
 */
#ifndef KBC_WORKFLOW_H
#define KBC_WORKFLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbc/kbc.h"
#include "kbc/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The wire schema strings the Rust original carries. kb-c does not serve
 * these routes yet, but a port that renames the wire vocabulary is a port
 * that has already diverged, and the strings cost nothing to keep exact. */
#define KBC_BOARD_SCHEMA "kbc-canvas/1"
#define KBC_TOUR_SCHEMA "kbc-tour/1"
#define KBC_TRAIL_SCHEMA "kbc-trail/1"
#define KBC_LANE_SCHEMA "aug-lane/1"
#define KBC_RECIPE_RUN_SCHEMA "kbc-recipe-run/1"

/* Every export carries this sentence. An export is a SNAPSHOT: a board
 * re-resolves every node against the working tree on each read, and a file
 * does not. Saying so is the difference between a snapshot and a lie. */
#define KBC_BOARD_SNAPSHOT_CAPTION                                          \
  "This is a SNAPSHOT. A kb-code board re-resolves every node against the " \
  "working tree on each read; this file does not. Its states were true "    \
  "when it was exported."

/* ── 1. BOARD STATUS ───────────────────────────────────────────────────────── */

/* The CLOSED status vocabulary, in the order the original declares it. */
typedef enum {
  KBC_BOARD_PENDING = 0, /* D21: an agent-proposed board awaits a human. */
  KBC_BOARD_DRAFT,       /* authored and explicitly not proposing. */
  KBC_BOARD_ACCEPTED,    /* written ONLY by the accept route. */
  KBC_BOARD_ARCHIVED,    /* written ONLY by the archive route. */
  KBC_BOARD_STATUS__COUNT
} kbc_board_status;

const char *kbc_board_status_str(kbc_board_status s);
/* BORROWED wire spelling -> enum. KBC_ERR_INVALID on anything outside the
 * closed vocabulary, and the message names the bad value AND the list. */
kbc_status kbc_board_status_parse(const char *s, kbc_board_status *out,
                                  kbc_err *err);
bool kbc_board_status_is_valid(const char *s);

/* The two statuses an APPLY may write. `accepted`/`archived` are transitions,
 * not authored state: a document that names one is refused by the lint before
 * any gate matters, which is what makes "impossible by the lint" true by
 * construction rather than by convention. */
bool kbc_board_status_is_appliable(kbc_board_status s);

/* The transition predicate. KBC_OK when `from -> to` is legal;
 * KBC_ERR_CONFLICT with a message naming BOTH states when it is not.
 *
 * This is STRICTLY STRONGER than the Rust original, which is deliberate and is
 * the one behavioural divergence in this file. `set_canvas_board_status`
 * (store/canvas.rs:449) executes
 *     UPDATE canvas_boards SET status = ?3, revision = revision + 1 ...
 * unconditionally — the route layer constrains WHICH status may be requested
 * (only accept_board/archive_board call it, and the lint refuses an apply
 * naming accepted/archived), so the invariant holds by the accident of every
 * call site agreeing. A port that keeps that accident has no way to TEST it:
 * a second caller, or a new route, silently widens the machine. The legal set
 * below is the invariant the original's call sites imply, and archived ->
 * accepted is the one transition they imply but never exercise.
 *
 *   pending  -> draft, accepted, archived
 *   draft    -> pending, accepted, archived
 *   accepted -> pending, draft, archived
 *   archived -> pending, draft
 *
 * `archived -> accepted` is REFUSED. An archive is a human's decision that a
 * board is finished; re-accepting it with no content change in between would
 * resurrect that decision from a bare call. Bring it back by re-applying
 * (a changed apply resets the status, which is legal), then accept. */
kbc_status kbc_board_status_can_transition(kbc_board_status from,
                                           kbc_board_status to, kbc_err *err);

/* ── 2. TRAIL MODE ─────────────────────────────────────────────────────────── */

typedef enum {
  KBC_TRAIL_OFF = 0,     /* the ABSENCE of a decision, not a decision. */
  KBC_TRAIL_RECORDING,
  KBC_TRAIL_PAUSED,
  KBC_TRAIL_MODE__COUNT
} kbc_trail_mode;

const char *kbc_trail_mode_str(kbc_trail_mode m);
kbc_status kbc_trail_mode_parse(const char *s, kbc_trail_mode *out, kbc_err *err);
bool kbc_trail_mode_is_valid(const char *s);

/* The two-gate write admission, verbatim in behaviour from
 * trails::routes::admit_write. `enabled` is the operator's `[trails] enabled`
 * master switch; `mode` is the PERSISTED opt-in row. A write is admitted only
 * when both agree. KBC_OK / KBC_ERR_UNSUPPORTED (feature off) /
 * KBC_ERR_CONFLICT (paused) / KBC_ERR_CONFLICT (never turned on). */
kbc_status kbc_trail_admit_write(bool enabled, kbc_trail_mode mode,
                                 kbc_err *err);

/* The error URNs the original hangs on its refusals. kb-c has no HTTP layer
 * for these routes yet, but a caller building one needs the same problem
 * types, and a refusal whose type cannot be recovered is a string match. */
#define KBC_TRAILS_ERR_DISABLED "urn:kb:errors:trails-disabled"
#define KBC_TRAILS_ERR_OFF "urn:kb:errors:trails-off"
#define KBC_TRAILS_ERR_PAUSED "urn:kb:errors:trails-paused"
#define KBC_TRAILS_ERR_FULL "urn:kb:errors:trail-full"

/* The problem type a refusal maps to, for the same reason. */
const char *kbc_trail_admit_problem_type(bool enabled, kbc_trail_mode mode);

/* ── 3. LANE TRUST CLASS ───────────────────────────────────────────────────── */

/* Ordered so `min` is meaningful. The SAME vocabulary resolve.rs and
 * entities::class_for mint in, with `orphan` — the honest "nothing matched"
 * — as its own floor rather than a silently dropped row. */
typedef enum {
  KBC_TRUST_ORPHAN = 0,
  KBC_TRUST_CANDIDATE,
  KBC_TRUST_LIKELY,
  KBC_TRUST_EXACT,
  KBC_TRUST__COUNT
} kbc_trust_class;

const char *kbc_trust_class_str(kbc_trust_class c);
kbc_status kbc_trust_class_parse(const char *s, kbc_trust_class *out,
                                 kbc_err *err);
/* The min() the whole classing rule is built on. */
kbc_trust_class kbc_trust_class_min(kbc_trust_class a, kbc_trust_class b);

/* The anchoring inputs of one fact. All BORROWED; the struct outlives nothing.
 * `cap` is the per-fact ceiling (KBC_TRUST__COUNT = "capped above exact"),
 * `blob_sha` is the blob the producing tool named, and `sha_source` is
 * "tool" when the tool named it and "mirror_at_ingest" when the daemon
 * attributed it. */
typedef struct {
  const char *blob_sha;   /* NULL = the fact has no anchor at all. */
  const char *sha_source; /* NULL = unattributed. */
  const char *snippet;    /* NULL = none captured. */
  kbc_trust_class cap;    /* KBC_TRUST__COUNT means "no cap". */
  int line_start;
  int line_end;
} kbc_fact_anchor;

/* What classing computed, and WHY. The reason is a closed vocabulary so a new
 * rung cannot be added without appearing in a list a caller can walk. */
typedef struct {
  kbc_trust_class class;
  const char *reason;
  int line_start;
  int line_end;
  bool shifted;
} kbc_classed;

#define KBC_LANE_SHA_SOURCE_TOOL "tool"
#define KBC_LANE_SHA_SOURCE_MIRROR "mirror_at_ingest"

#define KBC_REASON_PATH_GONE "path-gone"
#define KBC_REASON_BLOB_CURRENT "blob-current"
#define KBC_REASON_SHA_ATTRIBUTED "blob-current-sha-attributed"
#define KBC_REASON_FILE_LEVEL_MOVED "file-level-blob-moved"
#define KBC_REASON_CONTENT_UNREADABLE "content-unreadable"
#define KBC_REASON_NO_SNIPPET "no-snippet"
#define KBC_REASON_REANCHORED_EXACT "reanchored-exact"
#define KBC_REASON_REANCHORED_FUZZY "reanchored-fuzzy"
#define KBC_REASON_NO_ANCHOR "no-anchor"

/* Every reason class_for can emit, NULL-terminated. */
const char *const *kbc_lane_reasons(void);
bool kbc_lane_reason_is_valid(const char *r);

/* The ONE function that turns a stored claim into a class:
 * min(lane ceiling, per-fact cap, anchor state). Pure — no store, no clock —
 * which is what makes the whole table testable and is why the class is never
 * persisted: a fact whose blob has moved must not read back as fresh. */
kbc_classed kbc_lane_class_for(kbc_trust_class ceiling,
                               const kbc_fact_anchor *anchor,
                               const char *current_blob,
                               const char *current_text);

/* ── 4. RECIPE RUN ─────────────────────────────────────────────────────────── */

typedef enum {
  KBC_RUN_RUNNING = 0,
  KBC_RUN_DONE,
  KBC_RUN_FAILED,
  KBC_RUN_ABORTED, /* the wall-clock budget ran out. */
  KBC_RUN_STATE__COUNT
} kbc_run_state;

typedef enum {
  KBC_OP_OK = 0,
  KBC_OP_FAILED,
  KBC_OP_SKIPPED, /* a dependency failed, so this op never ran. */
  KBC_OP_STATE__COUNT
} kbc_op_state;

const char *kbc_run_state_str(kbc_run_state s);
const char *kbc_op_state_str(kbc_op_state s);
kbc_status kbc_run_state_parse(const char *s, kbc_run_state *out, kbc_err *err);
kbc_status kbc_op_state_parse(const char *s, kbc_op_state *out, kbc_err *err);

/* The transition predicate for a run. running -> done|failed|aborted is the
 * ONLY legal move; a terminal run never moves again, so a late writer cannot
 * resurrect a run that already reported failure. KBC_ERR_CONFLICT names both
 * the from-state and the to-state, as every refusal in this file does. */
kbc_status kbc_run_state_can_transition(kbc_run_state from, kbc_run_state to,
                                        kbc_err *err);

/* One recorded op. `error` is BORROWED and may be NULL for KBC_OP_OK. */
typedef struct {
  int ordinal;
  char op[32]; /* the Rust `Op` wire spelling, e.g. "git_log" */
  kbc_op_state state;
  int64_t rows;
  int64_t ms;
  char *detail; /* KBC_OWN, the failure text; NULL when there was none. */
} kbc_run_op;

void kbc_run_op_free(kbc_run_op *op);
void kbc_run_ops_free(kbc_run_op *ops, size_t n);

/* A run row. `id` is "run_" + 12 hex. */
typedef struct {
  char id[24]; /* "run_" + 12 hex = 16 bytes, + NUL. */
  char slug[64];
  kbc_run_state state;
  int64_t created_unix;
  int64_t updated_unix;
  size_t op_count; /* how many op rows this run recorded. */
  char *error;     /* KBC_OWN; NULL unless the run failed or aborted. */
} kbc_recipe_run;

void kbc_recipe_run_free(kbc_recipe_run *r);

/* ── THE DATABASE ──────────────────────────────────────────────────────────── */

/* One handle, one sibling database, one migration ladder owned by this file. */
typedef struct kbc_workflow kbc_workflow;

/* Opens (creating if absent) the workflow database at `path` and brings it to
 * KBC_WORKFLOW_SCHEMA. Owns its own sqlite3 connection; it does NOT touch
 * kbc_store and does NOT share its connection. */
kbc_status kbc_workflow_open(const char *path, kbc_workflow **out,
                             kbc_err *err);
/* Closes the handle. Every value a call returned is freed by the caller and
 * stays valid; nothing here borrows from `w` after this returns.
 *
 * THREADING: a handle is NOT safe to share between threads. It wraps exactly
 * one sqlite3 connection and a transaction is per-connection, so two threads
 * calling in on one handle would have the second BEGIN IMMEDIATE nest inside
 * the first's. Concurrent writers each open their own handle on the same
 * file — which is also what a second process looks like, and the case a
 * per-process lock would not save. Two handles on one path are coordinated
 * by SQLite's own locking, not by anything here. */
void kbc_workflow_close(kbc_workflow *w);

/* The migration ladder this build knows. A database at a different version is
 * REFUSED rather than read, so a binary can never interpret a layout it was
 * not written for. */
int kbc_workflow_schema_version(const kbc_workflow *w);

/* Sets `busy_timeout_ms` on the connection and turns WAL on. WAL is what
 * makes the two-writer case below a WINNER rather than a corruption: a
 * reader never blocks a writer and a writer never sees a torn page. */
kbc_status kbc_workflow_configure(kbc_workflow *w, int busy_timeout_ms,
                                  kbc_err *err);

/* ── BOARDS ────────────────────────────────────────────────────────────────── */

#define KBC_MAX_BOARD_NODES 200u   /* D10's node cap. A cap is a REFUSAL. */
#define KBC_MAX_BOARD_EDGES 600u
#define KBC_BOARD_ID_MAX 64u
#define KBC_BOARD_TITLE_MAX 200u

typedef struct {
  char id[KBC_BOARD_ID_MAX + 1];
  char slug[KBC_BOARD_ID_MAX + 1];
  char kind[8]; /* "board" or "tour" — they SHARE the tables (V0039). */
  char title[KBC_BOARD_TITLE_MAX + 1];
  char *description_md; /* KBC_OWN; may be NULL, treated as "". */
  kbc_board_status status;
  char *authored_ref; /* KBC_OWN; may be NULL. */
  char content_hash[65];
  int64_t revision;
  int64_t created_unix;
  int64_t updated_unix;
} kbc_board;

void kbc_board_free(kbc_board *b);

/* Creates a board in `status`, which MUST be appliable (pending or draft) —
 * a create is an apply, and an apply may not author accepted/archived. */
kbc_status kbc_board_create(kbc_workflow *w, const char *repo, const char *slug,
                            const char *kind, const char *title,
                            const char *description_md, const char *authored_ref,
                            const char *content_hash, kbc_board_status status,
                            int64_t now_unix, kbc_board *out, kbc_err *err);

kbc_status kbc_board_get(kbc_workflow *w, const char *repo, const char *slug,
                         kbc_board *out, kbc_err *err);
kbc_status kbc_board_delete(kbc_workflow *w, const char *repo,
                            const char *slug, kbc_err *err);

/* THE TRANSITION. Reads the current status, checks it against
 * kbc_board_status_can_transition, and only then writes — all inside ONE
 * BEGIN IMMEDIATE transaction, so the read and the write cannot be separated
 * by another writer. That is the whole of the concurrency guarantee, and it
 * means two concurrent callers SERIALISE rather than race: if both asked for
 * legal transitions, both succeed in order, and if the second is illegal
 * against the first's result it is refused with KBC_ERR_CONFLICT and changes
 * nothing at all. A refusal never half-applies. On success `out` (if non-NULL)
 * receives the new row. */
kbc_status kbc_board_set_status(kbc_workflow *w, const char *repo,
                                const char *slug, kbc_board_status to,
                                int64_t now_unix, kbc_board *out,
                                kbc_err *err);

/* An apply of unchanged content is a NO-OP: nothing is written, `revision`
 * does not move, and this returns KBC_OK with `*changed` false. A CHANGED
 * apply resets an accepted/archived board to the status the document asked
 * for — a human accepted a specific board, not a slug, and preserving
 * `accepted` would smuggle unreviewed content under that approval. */
kbc_status kbc_board_apply_status(kbc_workflow *w, const char *repo,
                                  const char *slug, const char *new_hash,
                                  kbc_board_status want, int64_t now_unix,
                                  bool *changed, kbc_board *out, kbc_err *err);

/* ── THE TRANSITION LOG ────────────────────────────────────────────────────── */

/* The original keeps NO board history: `canvas_boards` holds the current
 * status and nothing else, so a board that reached `archived` from
 * `accepted` cannot say it was ever accepted, and a board sitting in
 * `pending` cannot say how many times it has been proposed. The original's
 * only append-only record is the crate-wide `mutations` ledger (V0027),
 * which stores route/method/admission/outcome and no status values at all.
 *
 * This port ADDS `board_events`, an append-only log of every accepted
 * transition, and this is the second deliberate divergence from the original
 * in this file. A board that forgets cannot answer "how did this get stuck".
 * A REFUSAL is not logged: a log of refused transitions would be a log of
 * nothing but the callers that got it wrong. */

typedef struct {
  int64_t seq;      /* monotonic per board. */
  kbc_board_status from;
  kbc_board_status to;
  int64_t revision; /* the revision this transition produced. */
  int64_t at_unix;
  char *note;       /* KBC_OWN, may be NULL. */
} kbc_board_event;

void kbc_board_event_free(kbc_board_event *e);
void kbc_board_events_free(kbc_board_event *e, size_t n);

/* The board's transitions, oldest first. Caller frees with
 * kbc_board_events_free. `limit` 0 means no limit. */
kbc_status kbc_board_events(kbc_workflow *w, const char *repo,
                            const char *slug, size_t limit,
                            kbc_board_event **out, size_t *n_out,
                            kbc_err *err);

/* ── TRAILS STATE ──────────────────────────────────────────────────────────── */

/* Reads the persisted opt-in row. A volume that has never opted in has NO
 * row, and that reads as `off` with `changed_unix` 0 — the default is the
 * absence of a decision, not a decision to record. */
kbc_status kbc_trail_state_get(kbc_workflow *w, kbc_trail_mode *out,
                               int64_t *changed_unix, kbc_err *err);
/* The ONLY writer of that row. */
kbc_status kbc_trail_state_set(kbc_workflow *w, kbc_trail_mode mode,
                               int64_t now_unix, kbc_err *err);

/* ── RECIPE RUNS ───────────────────────────────────────────────────────────── */

#define KBC_MAX_RUN_OPS 256u /* one run's step cap; over it is a refusal. */

/* Opens a run in `running`. The row is written BEFORE the first op runs, so a
 * process that dies mid-run still leaves a run to find. */
kbc_status kbc_recipe_run_begin(kbc_workflow *w, const char *repo,
                                const char *slug, int64_t now_unix,
                                kbc_recipe_run *out, kbc_err *err);

/* Records ONE op's outcome, committed immediately and independently. This is
 * what acceptance requires of a half-completed run: the ops that DID complete
 * are on disk before the failing one returns, so a reader can always say how
 * far the run got. */
kbc_status kbc_recipe_run_record_op(kbc_workflow *w, const char *run_id,
                                    const char *op, kbc_op_state state,
                                    int64_t rows, int64_t ms,
                                    const char *detail, kbc_err *err);

/* Closes a run. `error` may be NULL for KBC_RUN_DONE. The transition is
 * checked, so closing an already-terminal run is refused and changes
 * nothing. */
kbc_status kbc_recipe_run_finish(kbc_workflow *w, const char *run_id,
                                 kbc_run_state final_state,
                                 const char *error, int64_t now_unix,
                                 kbc_err *err);

kbc_status kbc_recipe_run_get(kbc_workflow *w, const char *run_id,
                              kbc_recipe_run *out, kbc_err *err);
/* The recorded ops, in ordinal order. Caller frees with kbc_run_ops_free. */
kbc_status kbc_recipe_run_ops(kbc_workflow *w, const char *run_id,
                              kbc_run_op **ops_out, size_t *n_out,
                              kbc_err *err);

/* ── JSON CANVAS 1.0 ───────────────────────────────────────────────────────── */

/* The layout constants, from web-code's canvasPlacement.ts / egoGraph.ts. The
 * export reuses the SPA's grid so cards land on the same pitch. */
#define KBC_CANVAS_CARD_W 320.0
#define KBC_CANVAS_CARD_H 200.0
#define KBC_CANVAS_COL_GAP 180.0
#define KBC_CANVAS_ROW_GAP 48.0

typedef struct {
  double x, y, w, h;
} kbc_placed;

typedef struct {
  char id[KBC_BOARD_ID_MAX + 1];
  bool is_group;
} kbc_layout_node;

typedef struct {
  char from[KBC_BOARD_ID_MAX + 1];
  char to[KBC_BOARD_ID_MAX + 1];
} kbc_layout_edge;

/* The one authored geometry: a hand-placed node. Absent = layout decides. */
typedef struct {
  char id[KBC_BOARD_ID_MAX + 1];
  double x, y;
} kbc_pin;

/* Layered DAG placement, for the JSON Canvas export ONLY — the live board is
 * coordinate-free plus pins and the SPA owns its layout. Deterministic: no
 * clock, no randomness, no floating-point accumulation, cycles broken by a
 * bounded relax so the result cannot depend on traversal order.
 *
 * `out` is indexed parallel to `nodes`; every node in `nodes` MUST get an
 * entry, because a JSON Canvas node without geometry is not a JSON Canvas
 * node. KBC_ERR_INVALID names the first node left unplaced rather than
 * emitting a default. */
kbc_status kbc_board_layout(const kbc_layout_node *nodes, size_t n,
                            const kbc_layout_edge *edges, size_t ne,
                            const char *const *steps, size_t nsteps,
                            const kbc_pin *pins, size_t npins,
                            kbc_placed *out, kbc_err *err);

/* One node as the exporter sees it. `state` and `reason` ride along as
 * `kbc_`-prefixed extension fields, which the spec's own extensibility rule
 * says other apps ignore. */
typedef struct {
  char id[KBC_BOARD_ID_MAX + 1];
  char kind[16];   /* code|note|query|hunk|finding|annotation|turn|... */
  char *title;     /* KBC_OWN, may be NULL. */
  char *body_md;   /* KBC_OWN, may be NULL. */
  char *address;   /* KBC_OWN, may be NULL. */
  char state[8];   /* pinned | carried | present | inert | orphan. */
  char reason[40]; /* one of the eleven resolution reasons. */
  char *code_path; /* KBC_OWN for kind=code; else NULL. */
  int range_start; /* 0 when absent. */
  int range_end;
  char *url;       /* KBC_OWN for kind=link; else NULL. */
  char *query;     /* KBC_OWN for kind=query; else NULL. */
} kbc_export_node;

typedef struct {
  char from[KBC_BOARD_ID_MAX + 1];
  char to[KBC_BOARD_ID_MAX + 1];
  char kind[16];
  char *label; /* KBC_OWN, may be NULL. */
  char provenance[8]; /* authored | derived. */
  char *trust;        /* KBC_OWN for a derived edge; NULL for authored. */
} kbc_export_edge;

void kbc_export_node_free(kbc_export_node *n);
void kbc_export_nodes_free(kbc_export_node *n, size_t count);
void kbc_export_edges_free(kbc_export_edge *e, size_t count);

/* Emits a JSON Canvas 1.0 document.
 *
 * The spec REQUIRES x/y/width/height on every node and has no concept of a
 * code range, a blob or a trust class, so the export is lossy in a stated
 * way: geometry is derived here, and everything the spec cannot express rides
 * along in `kbc_`-prefixed extension fields. Round-tripping through another
 * canvas app WILL lose them. kbc_board_export_markdown is the lossless one.
 *
 * Returns KBC_ERR_INVALID if any node would be emitted without geometry —
 * the invariant the spec states and this exporter exists to satisfy. */
kbc_status kbc_board_export_jsoncanvas(const char *repo, const char *slug,
                                       const char *title, kbc_board_status status,
                                       int64_t revision,
                                       const kbc_export_node *nodes,
                                       size_t n_nodes,
                                       const kbc_export_edge *edges,
                                       size_t n_edges,
                                       const char *const *steps,
                                       size_t n_steps, kbc_str *out,
                                       kbc_err *err);

#ifdef __cplusplus
}
#endif

#endif /* KBC_WORKFLOW_H */
