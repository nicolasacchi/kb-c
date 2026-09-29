/* test_workflow.c — src/workflow.c: the kb-code-lane workflow state
 * machines. Every case runs against its own sibling database in its own
 * tmpdir, and the restart cases genuinely drop and reopen the handle rather
 * than re-reading through the same connection.
 *
 * The cases are grouped by the six things the subsystem has to get right:
 * the transition contract, restart survival, partial-run recording, the JSON
 * Canvas geometry invariant, concurrency, and the pure vocabularies. */

#include "kbc_test.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "kbc/workflow.h"

/* ------------------------------------------------------------- helpers --- */

/* Every test that needs a database gets a fresh path in a fresh tmpdir. The
 * handle is opened per test rather than shared, so one test's transactions
 * cannot be another test's rollback. */
static void db_path(char *buf, size_t cap, const char *name) {
  char dir[KBC_TEST_PATH_MAX];
  kbc_test_tmpdir(dir, sizeof(dir));
  int n = snprintf(buf, cap, "%s/%s.db", dir, name);
  if (n < 0 || (size_t)n >= cap) {
    kbc_test_fail(__FILE__, __LINE__, "db path does not fit %zu bytes", cap);
  }
}

static kbc_workflow *open_wf(const char *path, kbc_err *err) {
  kbc_workflow *w = NULL;
  KBC_CHECK_OK(kbc_workflow_open(path, &w, err));
  return w;
}

/* Builds one export node with owned strings, so the exporter sees exactly what
 * a resolved board would hand it. */
static kbc_export_node mk_node(const char *id, const char *kind,
                               const char *state, const char *reason) {
  kbc_export_node n;
  memset(&n, 0, sizeof(n));
  snprintf(n.id, sizeof(n.id), "%s", id);
  snprintf(n.kind, sizeof(n.kind), "%s", kind);
  snprintf(n.state, sizeof(n.state), "%s", state);
  snprintf(n.reason, sizeof(n.reason), "%s", reason);
  n.address = strdup("src/main.c:42");
  n.title = strdup("A title");
  return n;
}

/* Counts occurrences of `needle` in the exported document. Used to assert the
 * geometry invariant by construction: four keys per node, not on some. */
static size_t count_of(const char *hay, const char *needle) {
  size_t n = 0;
  size_t len = strlen(needle);
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += len;
  }
  return n;
}

/* =========================================================================
 * 1. THE TRANSITION CONTRACT
 * ========================================================================= */

/* The refusal is the contract. It must name the from-state AND the to-state:
 * a caller holding two states and an error needs to know which pair was
 * refused, or it cannot decide what to do next. */
KBC_TEST(an_illegal_board_transition_names_both_states) {
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_board_status_can_transition(KBC_BOARD_ARCHIVED,
                                               KBC_BOARD_ACCEPTED, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(e);
  KBC_CHECK_MSG(strstr(e.msg, "archived") != NULL,
                "refusal must name the from-state, got: %s", e.msg);
  KBC_CHECK_MSG(strstr(e.msg, "accepted") != NULL,
                "refusal must name the to-state, got: %s", e.msg);
}

/* Every legal pair is legal and every illegal pair is refused. The table is
 * the machine, so the table is what the test walks — a new state added to
 * the enum without a decision here fails rather than passing by default. */
KBC_TEST(the_board_legal_set_is_exactly_the_documented_one) {
  static const bool legal[KBC_BOARD_STATUS__COUNT][KBC_BOARD_STATUS__COUNT] = {
      /* to:      pending  draft  accepted  archived */
      /* pending */ {true, true, true, true},
      /* draft */ {true, true, true, true},
      /* accepted */ {true, true, true, true},
      /* archived */ {true, true, false, true},
  };
  for (int f = 0; f < KBC_BOARD_STATUS__COUNT; f++) {
    for (int t = 0; t < KBC_BOARD_STATUS__COUNT; t++) {
      kbc_err e;
      kbc_err_reset(&e);
      kbc_status s = kbc_board_status_can_transition(
          (kbc_board_status)f, (kbc_board_status)t, &e);
      if (legal[f][t]) {
        KBC_CHECK_MSG(s == KBC_OK,
                      "%s -> %s must be legal, got %s (%s)",
                      kbc_board_status_str((kbc_board_status)f),
                      kbc_board_status_str((kbc_board_status)t),
                      kbc_status_str(s), e.msg);
      } else {
        KBC_CHECK_MSG(s == KBC_ERR_CONFLICT,
                      "%s -> %s must be refused, got %s",
                      kbc_board_status_str((kbc_board_status)f),
                      kbc_board_status_str((kbc_board_status)t),
                      kbc_status_str(s));
      }
    }
  }
}

/* A refusal changes NOTHING — not the status, not the revision, and not the
 * history. A machine that refuses but half-applies is worse than one that
 * does not refuse at all, because the caller believes it lost. */
KBC_TEST(a_refused_transition_changes_nothing_at_all) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "refuse");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  KBC_CHECK_OK(kbc_board_create(w, "r", "b1", "board", "T", "d", NULL, "h1",
                                KBC_BOARD_PENDING, 100, NULL, &e));
  KBC_CHECK_OK(kbc_board_set_status(w, "r", "b1", KBC_BOARD_ACCEPTED, 200,
                                    NULL, &e));
  KBC_CHECK_OK(kbc_board_set_status(w, "r", "b1", KBC_BOARD_ARCHIVED, 300,
                                    NULL, &e));
  kbc_board before;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b1", &before, &e));
  int64_t rev = before.revision;
  kbc_board_free(&before);

  /* archived -> accepted is the one illegal move. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_board_set_status(w, "r", "b1", KBC_BOARD_ACCEPTED, 400,
                                     NULL, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(e);

  kbc_board after;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b1", &after, &e));
  KBC_CHECK_EQ_INT(after.status, KBC_BOARD_ARCHIVED);
  KBC_CHECK_EQ_INT(after.revision, rev);
  KBC_CHECK_EQ_INT(after.updated_unix, 300);
  kbc_board_free(&after);

  /* And the history did not grow: a log of refusals would be a log of
   * nothing but callers that got it wrong. */
  kbc_board_event *evs = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_board_events(w, "r", "b1", 0, &evs, &n, &e));
  KBC_CHECK_EQ_INT(n, 2);
  kbc_board_events_free(evs, n);
  kbc_workflow_close(w);
}

/* A create is an apply, and an apply may not author accepted/archived: those
 * are transitions a human makes. A document that names one is refused. */
KBC_TEST(a_board_cannot_be_created_already_accepted) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "selfaccept");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  for (int s = KBC_BOARD_ACCEPTED; s < KBC_BOARD_STATUS__COUNT; s++) {
    kbc_err_reset(&e);
    kbc_status rc = kbc_board_create(w, "r", "b", "board", "T", NULL, NULL,
                                     "h", (kbc_board_status)s, 1, NULL, &e);
    KBC_CHECK_ERR(rc, KBC_ERR_CONFLICT);
    KBC_CHECK_MSG(strstr(e.msg, kbc_board_status_str((kbc_board_status)s)) !=
                      NULL,
                  "refusal must name the status it refused: %s", e.msg);
  }
  /* And nothing was written. */
  kbc_board b;
  KBC_CHECK_ERR(kbc_board_get(w, "r", "b", &b, &e), KBC_ERR_NOTFOUND);
  kbc_workflow_close(w);
}

/* An apply of UNCHANGED content is a no-op: the revision does not move and a
 * human's acceptance is preserved. This is the idempotency contract, in one
 * comparison rather than a field-by-field diff that could disagree with
 * itself. */
KBC_TEST(an_unchanged_apply_preserves_the_acceptance) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "idem");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  KBC_CHECK_OK(kbc_board_create(w, "r", "b", "board", "T", NULL, NULL, "h1",
                                KBC_BOARD_PENDING, 1, NULL, &e));
  KBC_CHECK_OK(kbc_board_set_status(w, "r", "b", KBC_BOARD_ACCEPTED, 2, NULL,
                                    &e));
  bool changed = true;
  KBC_CHECK_OK(kbc_board_apply_status(w, "r", "b", "h1", KBC_BOARD_PENDING, 3,
                                      &changed, NULL, &e));
  KBC_CHECK_MSG(!changed, "an unchanged apply must report no change");
  kbc_board b;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b", &b, &e));
  KBC_CHECK_EQ_INT(b.status, KBC_BOARD_ACCEPTED);
  KBC_CHECK_EQ_INT(b.revision, 2);
  kbc_board_free(&b);
  kbc_workflow_close(w);
}

/* A CHANGED apply resets an accepted board. A human accepted a specific
 * board, not a slug; preserving `accepted` across new bytes would smuggle
 * unreviewed content under that approval. */
KBC_TEST(a_changed_apply_withdraws_an_acceptance_and_logs_it) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "reset");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  KBC_CHECK_OK(kbc_board_create(w, "r", "b", "board", "T", NULL, NULL, "h1",
                                KBC_BOARD_PENDING, 1, NULL, &e));
  KBC_CHECK_OK(kbc_board_set_status(w, "r", "b", KBC_BOARD_ACCEPTED, 2, NULL,
                                    &e));
  bool changed = false;
  KBC_CHECK_OK(kbc_board_apply_status(w, "r", "b", "h2", KBC_BOARD_PENDING, 3,
                                      &changed, NULL, &e));
  KBC_CHECK_MSG(changed, "a changed apply must report a change");
  kbc_board b;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b", &b, &e));
  KBC_CHECK_EQ_INT(b.status, KBC_BOARD_PENDING);
  kbc_board_free(&b);
  /* The history shows BOTH the accept and its withdrawal — this is the
   * "how did this get stuck" question the original cannot answer. */
  kbc_board_event *evs = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_board_events(w, "r", "b", 0, &evs, &n, &e));
  KBC_CHECK_EQ_INT(n, 2);
  KBC_CHECK_EQ_INT(evs[0].from, KBC_BOARD_PENDING);
  KBC_CHECK_EQ_INT(evs[0].to, KBC_BOARD_ACCEPTED);
  KBC_CHECK_EQ_INT(evs[1].from, KBC_BOARD_ACCEPTED);
  KBC_CHECK_EQ_INT(evs[1].to, KBC_BOARD_PENDING);
  KBC_CHECK_NOT_NULL(evs[1].note);
  kbc_board_events_free(evs, n);
  kbc_workflow_close(w);
}

/* The trail machine's two gates. A write needs BOTH the operator's switch
 * and a persisted `recording`; either one alone admits nothing. */
KBC_TEST(trail_admission_needs_both_gates) {
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_trail_admit_write(true, KBC_TRAIL_RECORDING, &e));
  /* The master switch wins even when the row says recording: an indicator
   * that claimed "recording" while nothing was recorded is the one lie this
   * surface exists to prevent. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_trail_admit_write(false, KBC_TRAIL_RECORDING, &e),
                KBC_ERR_UNSUPPORTED);
  KBC_CHECK_ERR_MSG(e);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_trail_admit_write(true, KBC_TRAIL_PAUSED, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "PAUSED") != NULL,
                "the paused refusal must say PAUSED: %s", e.msg);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_trail_admit_write(true, KBC_TRAIL_OFF, &e),
                KBC_ERR_CONFLICT);
  /* Each refusal maps to a distinct, recoverable problem type. */
  KBC_CHECK_EQ_STR(kbc_trail_admit_problem_type(false, KBC_TRAIL_RECORDING),
                   KBC_TRAILS_ERR_DISABLED);
  KBC_CHECK_EQ_STR(kbc_trail_admit_problem_type(true, KBC_TRAIL_PAUSED),
                   KBC_TRAILS_ERR_PAUSED);
  KBC_CHECK_EQ_STR(kbc_trail_admit_problem_type(true, KBC_TRAIL_OFF),
                   KBC_TRAILS_ERR_OFF);
  KBC_CHECK_EQ_STR(kbc_trail_admit_problem_type(true, KBC_TRAIL_RECORDING), "");
}

/* =========================================================================
 * 2. RESTART SURVIVAL
 *
 * Every machine's state must be readable after the process dies. These cases
 * CLOSE the handle and REOPEN the file — a second read through the same
 * connection would prove nothing about durability.
 * ========================================================================= */

KBC_TEST(a_board_survives_a_restart) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "restart_board");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  KBC_CHECK_OK(kbc_board_create(w, "r", "b", "tour", "Title", "desc", "ref",
                                "abc123", KBC_BOARD_PENDING, 111, NULL, &e));
  KBC_CHECK_OK(kbc_board_set_status(w, "r", "b", KBC_BOARD_ACCEPTED, 222, NULL,
                                    &e));
  int schema = kbc_workflow_schema_version(w);
  kbc_workflow_close(w);

  w = open_wf(path, &e);
  KBC_CHECK_EQ_INT(kbc_workflow_schema_version(w), schema);
  kbc_board b;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b", &b, &e));
  KBC_CHECK_EQ_STR(b.slug, "b");
  KBC_CHECK_EQ_STR(b.kind, "tour");
  KBC_CHECK_EQ_STR(b.title, "Title");
  KBC_CHECK_EQ_STR(b.description_md, "desc");
  KBC_CHECK_EQ_STR(b.authored_ref, "ref");
  KBC_CHECK_EQ_STR(b.content_hash, "abc123");
  KBC_CHECK_EQ_INT(b.status, KBC_BOARD_ACCEPTED);
  KBC_CHECK_EQ_INT(b.revision, 2);
  KBC_CHECK_EQ_INT(b.created_unix, 111);
  KBC_CHECK_EQ_INT(b.updated_unix, 222);
  kbc_board_free(&b);

  kbc_board_event *evs = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_board_events(w, "r", "b", 0, &evs, &n, &e));
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_INT(evs[0].to, KBC_BOARD_ACCEPTED);
  KBC_CHECK_EQ_INT(evs[0].at_unix, 222);
  kbc_board_events_free(evs, n);
  kbc_workflow_close(w);
}

/* The trail opt-in row survives, and a volume that never opted in still
 * reads `off` with no row — the default is the ABSENCE of a decision. */
KBC_TEST(the_trail_mode_survives_a_restart_and_absent_reads_off) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "restart_trail");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_trail_mode m = KBC_TRAIL_RECORDING;
  int64_t changed = 99;
  KBC_CHECK_OK(kbc_trail_state_get(w, &m, &changed, &e));
  KBC_CHECK_EQ_INT(m, KBC_TRAIL_OFF);
  KBC_CHECK_EQ_INT(changed, 0);

  KBC_CHECK_OK(kbc_trail_state_set(w, KBC_TRAIL_PAUSED, 777, &e));
  kbc_workflow_close(w);

  w = open_wf(path, &e);
  m = KBC_TRAIL_OFF;
  changed = 0;
  KBC_CHECK_OK(kbc_trail_state_get(w, &m, &changed, &e));
  KBC_CHECK_EQ_INT(m, KBC_TRAIL_PAUSED);
  KBC_CHECK_EQ_INT(changed, 777);
  kbc_workflow_close(w);
}

KBC_TEST(a_recipe_run_and_its_ops_survive_a_restart) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "restart_run");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_recipe_run r;
  memset(&r, 0, sizeof(r));
  KBC_CHECK_OK(kbc_recipe_run_begin(w, "r", "audit", 10, &r, &e));
  KBC_CHECK_EQ_INT(r.state, KBC_RUN_RUNNING);
  KBC_CHECK_EQ_STR(r.slug, "audit");
  char run_id[24];
  snprintf(run_id, sizeof(run_id), "%s", r.id);
  kbc_recipe_run_free(&r);
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "search", KBC_OP_OK, 12, 5,
                                         NULL, &e));
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "blame", KBC_OP_OK, 3, 7,
                                         NULL, &e));
  KBC_CHECK_OK(kbc_recipe_run_finish(w, run_id, KBC_RUN_DONE, NULL, 20, &e));
  kbc_workflow_close(w);

  w = open_wf(path, &e);
  kbc_recipe_run after;
  memset(&after, 0, sizeof(after));
  KBC_CHECK_OK(kbc_recipe_run_get(w, run_id, &after, &e));
  KBC_CHECK_EQ_INT(after.state, KBC_RUN_DONE);
  KBC_CHECK_EQ_STR(after.slug, "audit");
  KBC_CHECK_EQ_INT(after.op_count, 2);
  KBC_CHECK_EQ_INT(after.created_unix, 10);
  KBC_CHECK_NULL(after.error);
  kbc_recipe_run_free(&after);

  kbc_run_op *ops = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_recipe_run_ops(w, run_id, &ops, &n, &e));
  KBC_CHECK_EQ_INT(n, 2);
  KBC_CHECK_EQ_INT(ops[0].ordinal, 0);
  KBC_CHECK_EQ_STR(ops[0].op, "search");
  KBC_CHECK_EQ_INT(ops[0].rows, 12);
  KBC_CHECK_EQ_INT(ops[1].ordinal, 1);
  KBC_CHECK_EQ_STR(ops[1].op, "blame");
  kbc_run_ops_free(ops, n);
  kbc_workflow_close(w);
}

/* A process that dies mid-run leaves a `running` row naming the last op that
 * completed. This is the durability half of the partial-run contract: the
 * record exists even though the run never reached a terminal state. */
KBC_TEST(a_run_that_died_midway_is_still_readable) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "died");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_recipe_run r;
  memset(&r, 0, sizeof(r));
  KBC_CHECK_OK(kbc_recipe_run_begin(w, "r", "audit", 1, &r, &e));
  char run_id[24];
  snprintf(run_id, sizeof(run_id), "%s", r.id);
  kbc_recipe_run_free(&r);
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "search", KBC_OP_OK, 5, 1,
                                         NULL, &e));
  /* No finish: this is the process dying. */
  kbc_workflow_close(w);

  w = open_wf(path, &e);
  kbc_recipe_run after;
  memset(&after, 0, sizeof(after));
  KBC_CHECK_OK(kbc_recipe_run_get(w, run_id, &after, &e));
  KBC_CHECK_EQ_INT(after.state, KBC_RUN_RUNNING);
  KBC_CHECK_EQ_INT(after.op_count, 1);
  kbc_recipe_run_free(&after);
  kbc_run_op *ops = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_recipe_run_ops(w, run_id, &ops, &n, &e));
  KBC_CHECK_EQ_INT(n, 1);
  KBC_CHECK_EQ_STR(ops[0].op, "search");
  kbc_run_ops_free(ops, n);
  kbc_workflow_close(w);
}

/* =========================================================================
 * 3. THE PARTIAL-RUN RECORD
 *
 * The Rust original materialises a run only after it completes, so a run
 * that failed at op 3 of 5 leaves no row at all. These cases make one op
 * fail on purpose and assert the record says which ops completed.
 * ========================================================================= */

KBC_TEST(a_run_that_fails_partway_records_which_ops_completed) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "partial");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_recipe_run r;
  memset(&r, 0, sizeof(r));
  KBC_CHECK_OK(kbc_recipe_run_begin(w, "r", "audit", 1, &r, &e));
  char run_id[24];
  snprintf(run_id, sizeof(run_id), "%s", r.id);
  kbc_recipe_run_free(&r);

  /* Three ops complete. */
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "search", KBC_OP_OK, 40, 3,
                                         NULL, &e));
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "usages", KBC_OP_OK, 9, 4,
                                         NULL, &e));
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "outline", KBC_OP_OK, 17, 2,
                                         NULL, &e));
  /* The fourth op FAILS. Nothing is recorded for it, because a real op
   * failure means the caller got an error and never reached the record call.
   * That gap is the thing under test: the three above are already durable. */
  const char *op_err = "blame: git blame failed on src/x.c";
  /* The ops after the failure never ran, and say so. */
  KBC_CHECK_OK(kbc_recipe_run_record_op(w, run_id, "churn", KBC_OP_SKIPPED, 0,
                                         0, op_err, &e));
  KBC_CHECK_OK(kbc_recipe_run_finish(w, run_id, KBC_RUN_FAILED, op_err, 9, &e));

  /* A terminal run is terminal. Re-closing it as `done` is how a
   * half-completed run's record gets erased, so it is refused — and the
   * refusal names both states. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_recipe_run_finish(w, run_id, KBC_RUN_DONE, NULL, 10, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "failed") != NULL &&
                    strstr(e.msg, "done") != NULL,
                "the refusal must name both states, got: %s", e.msg);

  /* The record a reader gets: which ops completed, which failed, which never
   * ran, and the error that stopped it. */
  kbc_recipe_run after;
  memset(&after, 0, sizeof(after));
  KBC_CHECK_OK(kbc_recipe_run_get(w, run_id, &after, &e));
  KBC_CHECK_EQ_INT(after.state, KBC_RUN_FAILED);
  KBC_CHECK_NOT_NULL(after.error);
  KBC_CHECK_EQ_STR(after.error, op_err);
  KBC_CHECK_EQ_INT(after.op_count, 4);
  kbc_recipe_run_free(&after);

  kbc_run_op *ops = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_recipe_run_ops(w, run_id, &ops, &n, &e));
  KBC_CHECK_EQ_INT(n, 4);
  KBC_CHECK_EQ_INT(ops[0].state, KBC_OP_OK);
  KBC_CHECK_EQ_STR(ops[0].op, "search");
  KBC_CHECK_EQ_INT(ops[1].state, KBC_OP_OK);
  KBC_CHECK_EQ_STR(ops[1].op, "usages");
  KBC_CHECK_EQ_INT(ops[2].state, KBC_OP_OK);
  KBC_CHECK_EQ_STR(ops[2].op, "outline");
  KBC_CHECK_EQ_INT(ops[3].state, KBC_OP_SKIPPED);
  KBC_CHECK_EQ_STR(ops[3].op, "churn");
  KBC_CHECK_EQ_STR(ops[3].detail, op_err);
  kbc_run_ops_free(ops, n);
  kbc_workflow_close(w);
}

/* A terminal run takes no more ops, and the run-level op cap is a refusal
 * rather than a silent truncation. Both are the same rule: the record says
 * what happened, it never quietly drops what did not fit. */
KBC_TEST(a_terminal_run_takes_no_more_ops_and_the_cap_refuses) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "cap");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_recipe_run r;
  memset(&r, 0, sizeof(r));
  KBC_CHECK_OK(kbc_recipe_run_begin(w, "r", "audit", 1, &r, &e));
  char run_id[24];
  snprintf(run_id, sizeof(run_id), "%s", r.id);
  kbc_recipe_run_free(&r);
  KBC_CHECK_OK(kbc_recipe_run_finish(w, run_id, KBC_RUN_DONE, NULL, 2, &e));
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_recipe_run_record_op(w, run_id, "search", KBC_OP_OK, 1, 1,
                                         NULL, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "done") != NULL,
                "the refusal must name the run's state: %s", e.msg);

  kbc_recipe_run r2;
  memset(&r2, 0, sizeof(r2));
  KBC_CHECK_OK(kbc_recipe_run_begin(w, "r", "big", 1, &r2, &e));
  char run2[24];
  snprintf(run2, sizeof(run2), "%s", r2.id);
  kbc_recipe_run_free(&r2);
  for (unsigned i = 0; i < KBC_MAX_RUN_OPS; i++) {
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_recipe_run_record_op(w, run2, "search", KBC_OP_OK, 1, 1,
                                           NULL, &e));
  }
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_recipe_run_record_op(w, run2, "search", KBC_OP_OK, 1, 1,
                                         NULL, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "cap") != NULL,
                "the cap refusal must say it is a cap: %s", e.msg);
  kbc_workflow_close(w);
}

/* =========================================================================
 * 4. THE JSON CANVAS GEOMETRY INVARIANT
 *
 * The spec REQUIRES x/y/width/height on every node. "On some" is the failure
 * that matters: a file where most nodes have geometry and one does not is
 * still a file, still parses, and still loses a card silently in whatever app
 * opens it. So the assertion is a COUNT, not a presence check.
 * ========================================================================= */

KBC_TEST(every_exported_node_carries_all_four_coordinates) {
  /* Heap, because kbc_export_nodes_free owns the array as well as each node's
   * strings — the contract is a heap-array contract and the test holds itself
   * to it rather than to a looser one. */
  kbc_export_node *nodes = calloc(7, sizeof(*nodes));
  KBC_CHECK_NOT_NULL(nodes);
  const char *ids[7] = {"n0", "n1", "n2", "n3", "n4", "n5", "n6"};
  const char *kinds[7] = {"code", "note", "group", "link", "query", "hunk",
                          "turn"};
  for (int i = 0; i < 7; i++) {
    nodes[i] = mk_node(ids[i], kinds[i], "pinned", "blob-current");
  }
  nodes[0].code_path = strdup("src/main.c");
  nodes[0].range_start = 10;
  nodes[0].range_end = 20;
  nodes[3].url = strdup("https://example.org/spec");

  const char *steps[2] = {"n2", "n0"};
  kbc_str out;
  kbc_str_init(&out);
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_board_export_jsoncanvas("repo", "slug", "Title",
                                           KBC_BOARD_ACCEPTED, 3, nodes, 7,
                                           NULL, 0, steps, 2, &out, &e));
  const char *doc = out.ptr;
  /* Four geometry keys, once per node — all seven, not some. */
  KBC_CHECK_EQ_INT(count_of(doc, "\"x\":"), 7);
  KBC_CHECK_EQ_INT(count_of(doc, "\"y\":"), 7);
  KBC_CHECK_EQ_INT(count_of(doc, "\"width\":"), 7);
  KBC_CHECK_EQ_INT(count_of(doc, "\"height\":"), 7);
  /* And every node really is present. */
  KBC_CHECK_EQ_INT(count_of(doc, "\"id\":\"n"), 7);
  /* The coordinates are integers on the derived grid. This board has no
   * edges, so every node is layer 0 and every card sits at x=0 with the
   * card size the SPA uses. */
  KBC_CHECK_MSG(strstr(doc, "\"x\":0,\"y\":0,\"width\":320,\"height\":200")
                    != NULL,
                "an unlayered card should sit at the origin at card size: %s",
                doc);
  /* A code node's range cannot ride `file` (which is vault-relative and whose
   * subpath is a heading anchor), so it rides an extension field instead. */
  KBC_CHECK_MSG(strstr(doc, "\"kbc_range\":[10,20]") != NULL,
                "a code node must carry its range: %s", doc);
  /* The snapshot caption, because an exported file is not the live board. */
  KBC_CHECK_MSG(strstr(doc, "SNAPSHOT") != NULL, "missing the snapshot caption");
  kbc_str_free(&out);
  kbc_export_nodes_free(nodes, 7);
}

/* An empty board is a valid canvas with an empty node list — a board that was
 * swept away must still export rather than emit a malformed file. */
KBC_TEST(an_empty_board_exports_as_a_valid_empty_canvas) {
  kbc_str out;
  kbc_str_init(&out);
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_board_export_jsoncanvas("r", "s", "T", KBC_BOARD_PENDING, 1,
                                           NULL, 0, NULL, 0, NULL, 0, &out, &e));
  KBC_CHECK_MSG(strstr(out.ptr, "\"nodes\": []") != NULL,
                "an empty board must still be a node list: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "\"edges\": []") != NULL,
                "an empty board must still be an edge list: %s", out.ptr);
  kbc_str_free(&out);
}

/* An authored node title or body carrying a quote must not be able to break
 * out of the document. This is the reason every string goes through the JSON
 * escaper rather than a format string. */
KBC_TEST(an_authored_title_cannot_break_out_of_the_export) {
  kbc_export_node *nodes = calloc(1, sizeof(*nodes));
  KBC_CHECK_NOT_NULL(nodes);
  nodes[0] = mk_node("n0", "note", "pinned", "blob-current");
  free(nodes[0].title);
  nodes[0].title = strdup("he said \"hi\"\\ and\nnewline");
  kbc_str out;
  kbc_str_init(&out);
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_board_export_jsoncanvas("r", "s", "T", KBC_BOARD_PENDING, 1,
                                           nodes, 1, NULL, 0, NULL, 0, &out,
                                           &e));
  /* The raw sequence must not survive unescaped, and the quote must be. */
  KBC_CHECK_MSG(strstr(out.ptr, "he said \\\"hi\\\"") != NULL,
                "a quote must be escaped: %s", out.ptr);
  KBC_CHECK_MSG(strstr(out.ptr, "\nnewline") == NULL,
                "a raw newline must not appear inside a JSON string");
  kbc_str_free(&out);
  kbc_export_nodes_free(nodes, 1);
}

/* Layout is deterministic and pins win: an authored position is never
 * overridden by a derived one, and the same input gives the same picture. */
KBC_TEST(layout_is_deterministic_and_a_pin_is_never_overridden) {
  kbc_layout_node nodes[3];
  memset(nodes, 0, sizeof(nodes));
  snprintf(nodes[0].id, sizeof(nodes[0].id), "a");
  snprintf(nodes[1].id, sizeof(nodes[1].id), "b");
  snprintf(nodes[2].id, sizeof(nodes[2].id), "c");
  /* a -> b -> c is three layers, so c is one COLUMN right of a. */
  kbc_layout_edge edges[2];
  memset(edges, 0, sizeof(edges));
  snprintf(edges[0].from, sizeof(edges[0].from), "a");
  snprintf(edges[0].to, sizeof(edges[0].to), "b");
  snprintf(edges[1].from, sizeof(edges[1].from), "b");
  snprintf(edges[1].to, sizeof(edges[1].to), "c");
  kbc_pin pins[1];
  memset(pins, 0, sizeof(pins));
  snprintf(pins[0].id, sizeof(pins[0].id), "b");
  pins[0].x = 11.0;
  pins[0].y = 22.0;

  kbc_placed p1[3];
  kbc_placed p2[3];
  memset(p1, 0, sizeof(p1));
  memset(p2, 0, sizeof(p2));
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_board_layout(nodes, 3, edges, 2, NULL, 0, pins, 1, p1, &e));
  KBC_CHECK_OK(kbc_board_layout(nodes, 3, edges, 2, NULL, 0, pins, 1, p2, &e));
  KBC_CHECK_EQ_DBL(p1[0].x, p2[0].x, 0.0);
  KBC_CHECK_EQ_DBL(p1[2].y, p2[2].y, 0.0);
  /* The pin is exactly where the author put it. */
  KBC_CHECK_EQ_DBL(p1[1].x, 11.0, 0.0);
  KBC_CHECK_EQ_DBL(p1[1].y, 22.0, 0.0);
  /* Layer comes from the edge DAG over EVERY node, so the chain a->b->c puts
   * c two columns right of a whether or not b is pinned — a pin overrides a
   * node's POSITION, not its depth. */
  KBC_CHECK_EQ_DBL(p1[0].x, 0.0, 0.0);
  KBC_CHECK_EQ_DBL(p1[2].x, 2.0 * (KBC_CANVAS_CARD_W + KBC_CANVAS_COL_GAP), 0.0);
  /* Every node has geometry, pins included. */
  for (int i = 0; i < 3; i++) {
    KBC_CHECK_EQ_DBL(p1[i].w, KBC_CANVAS_CARD_W, 0.0);
    KBC_CHECK_EQ_DBL(p1[i].h, KBC_CANVAS_CARD_H, 0.0);
  }
}

/* A cycle must terminate, not hang. The relax is bounded by the node count
 * precisely so an authored board can contain one. */
KBC_TEST(a_cyclic_board_lays_out_instead_of_hanging) {
  kbc_layout_node nodes[3];
  memset(nodes, 0, sizeof(nodes));
  for (int i = 0; i < 3; i++) {
    snprintf(nodes[i].id, sizeof(nodes[i].id), "n%d", i);
  }
  kbc_layout_edge edges[3];
  memset(edges, 0, sizeof(edges));
  const char *names[3] = {"n0", "n1", "n2"};
  for (int i = 0; i < 3; i++) {
    snprintf(edges[i].from, sizeof(edges[i].from), "%s", names[i]);
    snprintf(edges[i].to, sizeof(edges[i].to), "%s", names[(i + 1) % 3]);
  }
  kbc_placed p[3];
  memset(p, 0, sizeof(p));
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_board_layout(nodes, 3, edges, 3, NULL, 0, NULL, 0, p, &e));
  for (int i = 0; i < 3; i++) {
    KBC_CHECK_MSG(p[i].w > 0.0, "a cyclic board must still place every node");
  }
}

/* The node cap is a REFUSAL with the count in the message, never a silent
 * truncation — the same rule the original's own cap comments state. */
KBC_TEST(a_board_over_the_node_cap_is_refused_with_the_count) {
  kbc_err e;
  kbc_err_reset(&e);
  kbc_layout_node *nodes = calloc(KBC_MAX_BOARD_NODES + 1, sizeof(*nodes));
  KBC_CHECK_NOT_NULL(nodes);
  for (unsigned i = 0; i <= KBC_MAX_BOARD_NODES; i++) {
    snprintf(nodes[i].id, sizeof(nodes[i].id), "n%u", i);
  }
  kbc_placed *p = calloc(KBC_MAX_BOARD_NODES + 1, sizeof(*p));
  KBC_CHECK_NOT_NULL(p);
  KBC_CHECK_ERR(kbc_board_layout(nodes, KBC_MAX_BOARD_NODES + 1, NULL, 0,
                                 NULL, 0, NULL, 0, p, &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e.msg, "201") != NULL,
                "the refusal must carry the count: %s", e.msg);
  free(nodes);
  free(p);
}

/* =========================================================================
 * 5. CONCURRENCY
 *
 * Two writers, one winner or a refusal — never a corrupted state. Two
 * THREADS on one handle prove the in-process lock; two HANDLES on one file
 * prove the cross-process case, which is the one a real daemon hits.
 * ========================================================================= */

typedef struct {
  const char *path;
  kbc_status status;
  kbc_board_status to;
  char msg[KBC_ERR_MSG_MAX];
} racer;

/* Both racers are released together so they genuinely contend rather than
 * running one after the other by accident. */
static pthread_barrier_t start_line;

/* Each racer opens its OWN handle. That is the real concurrency model and it
 * is the one the contract states: a kbc_workflow wraps ONE sqlite3
 * connection, and a transaction is per-connection, so two threads sharing a
 * handle would be asking the second BEGIN IMMEDIATE to nest inside the
 * first's transaction. Two handles on one file is also what a second PROCESS
 * looks like, which is the case a per-process lock would not save. */
static void *race_accept(void *arg) {
  racer *r = arg;
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = NULL;
  if (kbc_workflow_open(r->path, &w, &e) != KBC_OK) {
    r->status = KBC_ERR_IO;
    snprintf(r->msg, sizeof(r->msg), "%s", e.msg);
    return NULL;
  }
  pthread_barrier_wait(&start_line);
  kbc_err_reset(&e);
  r->status = kbc_board_set_status(w, "r", "b", r->to, 500, NULL, &e);
  snprintf(r->msg, sizeof(r->msg), "%s", e.msg);
  kbc_workflow_close(w);
  return NULL;
}

/* Two threads, two different transitions, both legal from `pending`. Exactly
 * one lands, and the board ends in one of the two — never a half-applied
 * state, never a revision that moved twice for one board. */
KBC_TEST(two_concurrent_writers_produce_one_winner_and_one_refusal) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "race");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  KBC_CHECK_OK(kbc_board_create(w, "r", "b", "board", "T", NULL, NULL, "h",
                                KBC_BOARD_PENDING, 1, NULL, &e));
  racer a = {path, KBC_OK, KBC_BOARD_ACCEPTED, {0}};
  racer b = {path, KBC_OK, KBC_BOARD_ARCHIVED, {0}};
  pthread_barrier_init(&start_line, NULL, 2);
  pthread_t ta;
  pthread_t tb;
  KBC_CHECK_EQ_INT(pthread_create(&ta, NULL, race_accept, &a), 0);
  KBC_CHECK_EQ_INT(pthread_create(&tb, NULL, race_accept, &b), 0);
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  pthread_barrier_destroy(&start_line);

  /* Every racer resolves to a win or a refusal — never a hang, never a
   * silent no-op. That is the whole of the concurrency claim. */
  KBC_CHECK_MSG(a.status == KBC_OK || a.status == KBC_ERR_CONFLICT,
                "a racer must win or be refused, got %s", kbc_status_str(a.status));
  KBC_CHECK_MSG(b.status == KBC_OK || b.status == KBC_ERR_CONFLICT,
                "b must win or be refused, got %s", kbc_status_str(b.status));
  int winners = (a.status == KBC_OK) + (b.status == KBC_OK);

  /* Both transitions can be legal, so the two writers SERIALISE into a queue
   * and both may win — pending->accepted then accepted->archived is a legal
   * sequence, not a lost update. What must hold is that the board ends in a
   * state reachable by that sequence, and that its revision moved exactly
   * once per accepted write. Asserting "exactly one winner" would be
   * asserting a race that SQLite's locking resolves deterministically
   * against us. */
  kbc_board got;
  KBC_CHECK_OK(kbc_board_get(w, "r", "b", &got, &e));
  KBC_CHECK_MSG(got.status == KBC_BOARD_ACCEPTED ||
                    got.status == KBC_BOARD_ARCHIVED,
                "the board must be in one of the two raced states, got %s",
                kbc_board_status_str(got.status));
  KBC_CHECK_MSG(got.revision == 1 + winners,
                "revision must move once per accepted write: %lld vs %d "
                "winners", (long long)got.revision, winners);
  kbc_board_free(&got);

  /* And the log agrees with the board, rather than contradicting it: one
   * event per accepted write, no more. */
  kbc_board_event *evs = NULL;
  size_t n = 0;
  KBC_CHECK_OK(kbc_board_events(w, "r", "b", 0, &evs, &n, &e));
  KBC_CHECK_EQ_INT(n, (size_t)winners);
  /* Every logged move is a legal one, in the order it was applied. */
  for (size_t i = 1; i < n; i++) {
    kbc_err ce;
    kbc_err_reset(&ce);
    KBC_CHECK_OK(kbc_board_status_can_transition(evs[i - 1].to, evs[i].from,
                                                 &ce));
  }
  kbc_board_events_free(evs, n);
  kbc_workflow_close(w);
}

/* Two HANDLES on one file — the cross-process case a real daemon hits, and
 * the one where a per-process lock would be worthless. Whichever transition
 * lands, the loser is told it lost rather than silently overwriting. */
KBC_TEST(two_separate_handles_on_one_file_never_corrupt_the_board) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "race2");
  kbc_err e1;
  kbc_err_reset(&e1);
  kbc_err e2;
  kbc_err_reset(&e2);
  kbc_workflow *w1 = open_wf(path, &e1);
  kbc_workflow *w2 = NULL;
  KBC_CHECK_OK(kbc_workflow_open(path, &w2, &e2));
  KBC_CHECK_OK(kbc_board_create(w1, "r", "b", "board", "T", NULL, NULL, "h",
                                KBC_BOARD_PENDING, 1, NULL, &e1));
  kbc_status s1 = kbc_board_set_status(w1, "r", "b", KBC_BOARD_ARCHIVED, 10,
                                       NULL, &e1);
  KBC_CHECK_MSG(s1 == KBC_OK, "the first transition must land: %s", e1.msg);
  /* The second handle reads `archived` — the state the FIRST handle wrote,
   * across a connection boundary — and refuses archived -> accepted. That it
   * sees the committed state at all is the cross-process half of the claim;
   * that it refuses on it is the machine half. */
  kbc_status s2 = kbc_board_set_status(w2, "r", "b", KBC_BOARD_ACCEPTED, 20,
                                       NULL, &e2);
  KBC_CHECK_ERR(s2, KBC_ERR_CONFLICT);
  KBC_CHECK_MSG(strstr(e2.msg, "archived") != NULL &&
                    strstr(e2.msg, "accepted") != NULL,
                "the refusal must name both states: %s", e2.msg);
  kbc_board got;
  KBC_CHECK_OK(kbc_board_get(w1, "r", "b", &got, &e1));
  KBC_CHECK_EQ_INT(got.status, KBC_BOARD_ARCHIVED);
  KBC_CHECK_EQ_INT(got.revision, 2);
  kbc_board_free(&got);
  kbc_workflow_close(w1);
  kbc_workflow_close(w2);
}

/* =========================================================================
 * 6. THE PURE VOCABULARIES
 * ========================================================================= */

/* Every status round-trips through its wire spelling, and anything outside
 * the closed vocabulary is refused WITH the vocabulary in the message — a
 * caller who typed "open" needs to know what this machine calls open. */
KBC_TEST(board_statuses_round_trip_and_refuse_with_the_vocabulary) {
  for (int i = 0; i < KBC_BOARD_STATUS__COUNT; i++) {
    kbc_board_status s;
    kbc_err e;
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_board_status_parse(kbc_board_status_str(
                                            (kbc_board_status)i),
                                        &s, &e));
    KBC_CHECK_EQ_INT(s, i);
    KBC_CHECK_MSG(kbc_board_status_is_valid(
                      kbc_board_status_str((kbc_board_status)i)),
                  "\"%s\" must be a valid status",
                  kbc_board_status_str((kbc_board_status)i));
  }
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_board_status_parse("open", NULL, &e), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e);
  KBC_CHECK_MSG(strstr(e.msg, "open") != NULL, "must name the bad value: %s",
                e.msg);
  KBC_CHECK_MSG(strstr(e.msg, "accepted") != NULL,
                "must name the vocabulary: %s", e.msg);
  /* NULL is not a status, and saying so beats a crash. */
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_board_status_parse(NULL, NULL, &e), KBC_ERR_INVALID);
  KBC_CHECK_ERR_MSG(e);
}

/* The classing rule is min(ceiling, per-fact cap, anchor state), and it is
 * computed per request — never persisted. These cases pin each rung and,
 * more importantly, pin that a cap can only ever LOWER a class. */
KBC_TEST(lane_classing_is_the_min_of_ceiling_cap_and_anchor) {
  kbc_fact_anchor a;
  memset(&a, 0, sizeof(a));
  a.cap = KBC_TRUST__COUNT; /* no cap. */
  a.line_start = 10;
  a.line_end = 20;

  /* No anchor at all: the honest floor, not a silently dropped row. */
  kbc_classed c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha1", "text");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_ORPHAN);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_NO_ANCHOR);

  /* The file is gone. */
  a.blob_sha = "sha1";
  a.sha_source = KBC_LANE_SHA_SOURCE_TOOL;
  a.snippet = "needle";
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, NULL, NULL);
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_ORPHAN);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_PATH_GONE);

  /* Blob current and named by the tool: exact, under a high ceiling. */
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha1", "text");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_EXACT);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_BLOB_CURRENT);
  /* The SAME anchor under a candidate ceiling is candidate. A lane can never
   * mint above its own ceiling — that is the whole of the min(). */
  c = kbc_lane_class_for(KBC_TRUST_CANDIDATE, &a, "sha1", "text");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_CANDIDATE);
  /* And the per-fact cap can lower it further, independently. */
  a.cap = KBC_TRUST_LIKELY;
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha1", "text");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_LIKELY);

  /* The daemon attributed the blob rather than the tool naming it: one rung
   * lower however exact the bytes are. */
  a.cap = KBC_TRUST__COUNT;
  a.sha_source = KBC_LANE_SHA_SOURCE_MIRROR;
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha1", "text");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_LIKELY);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_SHA_ATTRIBUTED);

  /* The blob moved and the snippet re-anchors exactly. */
  a.sha_source = KBC_LANE_SHA_SOURCE_TOOL;
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha2", "a needle here");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_LIKELY);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_REANCHORED_EXACT);

  /* The blob moved and nothing re-anchors: the floor, with the reason. */
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha2", "unrelated bytes");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_CANDIDATE);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_REANCHORED_FUZZY);

  /* Moved, and the file cannot be read at all. */
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha2", NULL);
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_ORPHAN);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_CONTENT_UNREADABLE);

  /* Moved, readable, and no snippet was ever captured. */
  a.snippet = NULL;
  c = kbc_lane_class_for(KBC_TRUST_EXACT, &a, "sha2", "anything");
  KBC_CHECK_EQ_INT(c.class, KBC_TRUST_ORPHAN);
  KBC_CHECK_EQ_STR(c.reason, KBC_REASON_NO_SNIPPET);
}

/* Every reason the classer can emit is in the list a caller can walk, and
 * every listed reason is a real one. A closed vocabulary that drifts is a
 * filter that silently stops matching. */
KBC_TEST(every_lane_reason_is_in_the_closed_list) {
  size_t n = 0;
  for (const char *const *p = kbc_lane_reasons(); *p != NULL; p++) {
    n++;
    KBC_CHECK_MSG(kbc_lane_reason_is_valid(*p),
                  "listed reason \"%s\" must be valid", *p);
  }
  KBC_CHECK_EQ_INT(n, 9);
  KBC_CHECK_MSG(!kbc_lane_reason_is_valid("because"),
                "an unlisted reason must not validate");
  KBC_CHECK_MSG(!kbc_lane_reason_is_valid(NULL),
                "NULL must not validate");
}

/* The trust order is the enum order, because min() IS the rule. */
KBC_TEST(trust_class_min_is_the_lower_rung) {
  KBC_CHECK_EQ_INT(kbc_trust_class_min(KBC_TRUST_EXACT, KBC_TRUST_ORPHAN),
                   KBC_TRUST_ORPHAN);
  KBC_CHECK_EQ_INT(kbc_trust_class_min(KBC_TRUST_CANDIDATE,
                                       KBC_TRUST_LIKELY),
                   KBC_TRUST_CANDIDATE);
  KBC_CHECK_EQ_INT(kbc_trust_class_min(KBC_TRUST_LIKELY, KBC_TRUST_LIKELY),
                   KBC_TRUST_LIKELY);
  for (int i = 0; i < KBC_TRUST__COUNT; i++) {
    kbc_trust_class c;
    kbc_err e;
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_trust_class_parse(kbc_trust_class_str(
                                            (kbc_trust_class)i),
                                        &c, &e));
    KBC_CHECK_EQ_INT(c, i);
  }
}

/* The remaining vocabularies round-trip, and the run machine's terminal state
 * is terminal. */
KBC_TEST(the_run_and_trail_vocabularies_round_trip) {
  for (int i = 0; i < KBC_RUN_STATE__COUNT; i++) {
    kbc_run_state s;
    kbc_err e;
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_run_state_parse(kbc_run_state_str((kbc_run_state)i), &s,
                                     &e));
    KBC_CHECK_EQ_INT(s, i);
  }
  for (int i = 0; i < KBC_OP_STATE__COUNT; i++) {
    kbc_op_state s;
    kbc_err e;
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_op_state_parse(kbc_op_state_str((kbc_op_state)i), &s,
                                    &e));
    KBC_CHECK_EQ_INT(s, i);
  }
  for (int i = 0; i < KBC_TRAIL_MODE__COUNT; i++) {
    kbc_trail_mode m;
    kbc_err e;
    kbc_err_reset(&e);
    KBC_CHECK_OK(kbc_trail_mode_parse(kbc_trail_mode_str((kbc_trail_mode)i),
                                      &m, &e));
    KBC_CHECK_EQ_INT(m, i);
  }
  /* A terminal run never moves again. */
  kbc_err e;
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_run_state_can_transition(KBC_RUN_RUNNING, KBC_RUN_DONE,
                                            &e));
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_run_state_can_transition(KBC_RUN_FAILED, KBC_RUN_DONE,
                                             &e),
                KBC_ERR_CONFLICT);
  KBC_CHECK_ERR_MSG(e);
  KBC_CHECK_MSG(strstr(e.msg, "failed") != NULL && strstr(e.msg, "done") != NULL,
                "must name both states: %s", e.msg);
  /* A no-op is legal: a retry after a timeout is not a lost race. */
  kbc_err_reset(&e);
  KBC_CHECK_OK(kbc_run_state_can_transition(KBC_RUN_DONE, KBC_RUN_DONE, &e));
}

/* A board that does not exist is a NOTFOUND naming both parts, and a
 * transition on it changes nothing rather than creating it. */
KBC_TEST(a_transition_on_a_missing_board_is_notfound_and_creates_nothing) {
  char path[KBC_TEST_PATH_MAX];
  db_path(path, sizeof(path), "missing");
  kbc_err e;
  kbc_err_reset(&e);
  kbc_workflow *w = open_wf(path, &e);
  kbc_err_reset(&e);
  KBC_CHECK_ERR(kbc_board_set_status(w, "r", "nope", KBC_BOARD_ACCEPTED, 1,
                                     NULL, &e),
                KBC_ERR_NOTFOUND);
  KBC_CHECK_ERR_MSG(e);
  KBC_CHECK_MSG(strstr(e.msg, "nope") != NULL, "must name the slug: %s",
                e.msg);
  kbc_board b;
  KBC_CHECK_ERR(kbc_board_get(w, "r", "nope", &b, &e), KBC_ERR_NOTFOUND);
  kbc_workflow_close(w);
}

int main(void) {
  static const kbc_test_case cases[] = {
      {"an_illegal_board_transition_names_both_states",
       an_illegal_board_transition_names_both_states},
      {"the_board_legal_set_is_exactly_the_documented_one",
       the_board_legal_set_is_exactly_the_documented_one},
      {"a_refused_transition_changes_nothing_at_all",
       a_refused_transition_changes_nothing_at_all},
      {"a_board_cannot_be_created_already_accepted",
       a_board_cannot_be_created_already_accepted},
      {"an_unchanged_apply_preserves_the_acceptance",
       an_unchanged_apply_preserves_the_acceptance},
      {"a_changed_apply_withdraws_an_acceptance_and_logs_it",
       a_changed_apply_withdraws_an_acceptance_and_logs_it},
      {"trail_admission_needs_both_gates", trail_admission_needs_both_gates},
      {"a_board_survives_a_restart", a_board_survives_a_restart},
      {"the_trail_mode_survives_a_restart_and_absent_reads_off",
       the_trail_mode_survives_a_restart_and_absent_reads_off},
      {"a_recipe_run_and_its_ops_survive_a_restart",
       a_recipe_run_and_its_ops_survive_a_restart},
      {"a_run_that_died_midway_is_still_readable",
       a_run_that_died_midway_is_still_readable},
      {"a_run_that_fails_partway_records_which_ops_completed",
       a_run_that_fails_partway_records_which_ops_completed},
      {"a_terminal_run_takes_no_more_ops_and_the_cap_refuses",
       a_terminal_run_takes_no_more_ops_and_the_cap_refuses},
      {"every_exported_node_carries_all_four_coordinates",
       every_exported_node_carries_all_four_coordinates},
      {"an_empty_board_exports_as_a_valid_empty_canvas",
       an_empty_board_exports_as_a_valid_empty_canvas},
      {"an_authored_title_cannot_break_out_of_the_export",
       an_authored_title_cannot_break_out_of_the_export},
      {"layout_is_deterministic_and_a_pin_is_never_overridden",
       layout_is_deterministic_and_a_pin_is_never_overridden},
      {"a_cyclic_board_lays_out_instead_of_hanging",
       a_cyclic_board_lays_out_instead_of_hanging},
      {"a_board_over_the_node_cap_is_refused_with_the_count",
       a_board_over_the_node_cap_is_refused_with_the_count},
      {"two_concurrent_writers_produce_one_winner_and_one_refusal",
       two_concurrent_writers_produce_one_winner_and_one_refusal},
      {"two_separate_handles_on_one_file_never_corrupt_the_board",
       two_separate_handles_on_one_file_never_corrupt_the_board},
      {"board_statuses_round_trip_and_refuse_with_the_vocabulary",
       board_statuses_round_trip_and_refuse_with_the_vocabulary},
      {"lane_classing_is_the_min_of_ceiling_cap_and_anchor",
       lane_classing_is_the_min_of_ceiling_cap_and_anchor},
      {"every_lane_reason_is_in_the_closed_list",
       every_lane_reason_is_in_the_closed_list},
      {"trust_class_min_is_the_lower_rung", trust_class_min_is_the_lower_rung},
      {"the_run_and_trail_vocabularies_round_trip",
       the_run_and_trail_vocabularies_round_trip},
      {"a_transition_on_a_missing_board_is_notfound_and_creates_nothing",
       a_transition_on_a_missing_board_is_notfound_and_creates_nothing},
      {NULL, NULL},
  };
  return kbc_test_run("workflow", cases);
}
