# DECISIONS

Architecture decisions for kb-c that are not derivable from the code, with the
reasoning and the evidence behind each. A decision that is not written down
becomes a bug report six months later; a decision that is written down and
reversed is a one-line diff.

Format: what was decided, why, what the alternatives were, and what would make
us revisit it. Newest first.

---

## ADR-007 — the SSE `lag` probe ships untested; bounding its send buffer is declined

**Status:** accepted, 2026-09-29

**Decision.** The `lag` half of the SSE gap/lag probe ships, and ships with no
test at any layer. The obvious way to make it testable — bound the send buffer
of an SSE connection so a stalled client provably falls behind — is declined,
and this entry exists so the next session does not rediscover the mechanism
from scratch to learn that the option exists and was considered.

**The mechanism, which is the expensive part to rediscover.** A client that
stops reading its socket does *not* make the daemon's per-connection queue
overflow. `sse_pump` buffers up to `KBC_SSE_OUT_HIGH_WATER` (256 KiB) into the
connection's write buffer, and the kernel then absorbs everything past that
into the **server's** send buffer. The worker's `write` never returns
`EWOULDBLOCK`, so the worker never falls behind and the 64-slot queue never
sheds. Shrinking the *client's* `SO_RCVBUF` does not help, because the buffer
doing the absorbing is on the other end of the connection and a test cannot
reach it. Measured: a burst of 60,000 events (~4 MiB) with the client not
reading a single byte still left the queue intact in 2 runs of 6. The overflow
is a coin flip, not a difficulty to be tuned away.

**Reasoning.** The probe is a faithful 20-line port of the original's
`EventFrame::Lag { skipped }` (`kb-core/src/events.rs:272`): coalesce
`c->q_dropped` into one `event: lag` frame carrying the true count, emit it
ahead of the frame that caused the overflow, with no `id:` line. It is correct
as far as it can be checked by reading, it compiles clean under the full
`-Werror` set, and an untested-but-correct degradation notice is strictly
better than a silent stream with a hole in it. What we cannot rule out is that
it is untested-and-wrong, which is why the gap is recorded in `src/httpd.c` and
`tests/test_httpd.c` rather than left to be discovered. The `gap` half of the
same feature is deterministically tested, including the absent `id:` line and
the suppression of a replay the cursor cannot justify.

A test that fails on a correct build is worse than no test: it trains everyone
who reads CI to ignore red. The case was deleted rather than tuned — the
provisional version that failed 10 runs in 20 was replaced by a
publish-and-check loop that still failed 5 in 15 under TSan, and raising the
loop bound would only have bought a slower red.

**Alternatives.** *Bound the send buffer (SO_SNDBUF) on SSE connections.*
Declined, and this is the option worth recording. It is defensible on its own
merits — a daemon that lets one stalled client buffer unboundedly in the
kernel is a mild resource wart, and `c->out` is already bounded, so the kernel
is the only unbounded buffer left in the path. It is declined *here* for three
reasons, and the third is the one that decides it: it is a production
behaviour change nobody asked for; it disconnects stalled clients sooner, which
is a policy choice about what a client is entitled to buffer; and it surfaced
while fixing a flaky test, which is the context in which a scope decision is
least trustworthy. Making it in that context would be scope creep wearing the
cost of a test. *Export the connection ring through `httpd.h` so a test can
drive the overflow directly.* Rejected: the parts with teeth — the coalescing,
the non-zero count, the absent `id:` line — all live downstream of the
overflow, so the only thing reachable without it is the frame formatter, which
is a `printf`; a printf test plus a widened published API is a bad trade in
both directions. *Drop the `lag` probe entirely.* Rejected: the reconnect case
is the common one and is covered by `gap`; a client that cannot detect that it
fell behind mid-stream is a real limitation, and a correct 20-line notice is
cheap to keep.

**What would change this.** If anyone picks the send buffer up, it is not a
one-line change and it wants its own ADR: a knob (which bounds, and what the
default is), its own test, and an explicit decision about what a client is
entitled to buffer before being cut off. With the send buffer bounded the
deleted case comes straight back and becomes deterministic. Until then the
honest position is the one above: the probe ships, and the gap is written
down.

---

## ADR-001 — `kb-code-*` stays in Rust; kb-c does not replace it

**Status:** accepted, 2026-09-26

**Decision.** The two `kb-code-*` crates — `kb-code-server` (204,309 LOC) and
`kb-code-cli` (35,946 LOC), 240,255 LOC in total — are out of scope for this
port. They remain Rust. kb-c replaces the `kb` daemon only.

**Reasoning.** `kb-code-server` is a different product that happens to share a
workspace. It is a daemon over a *git checkout* whose entire value is
language-aware structure: tree-sitter grammars for six or more languages,
framework heuristics (`frameworks/rails`), blame, provenance, and a
lane/board/recipe state machine. It is not a document index.

The two do not share a data model. `kb` has artifacts, chunks and embeddings.
`kb-code` has repositories, commits, recipes, boards and lanes. The overlap is
the workspace, not the domain.

The cost is also structural, not volumetric:

- **tree-sitter has no C distribution kb-c could use.** Rust takes the grammar
  from a crate; C would mean vendoring generated parser C for each language —
  megabytes of generated code per grammar, and a rebuild for every grammar bump.
- The lane/board/recipe state machines are domain logic with no analogue in
  kb-c's model to test against.
- The two daemons would need a shared IPC contract and a shared auth model.
  Neither exists today, and inventing one is a project in itself.

A faithful port is a second project roughly the size of this one, not a stage of
it.

**Alternatives considered.**

- *Port `kb-code-server`'s search only.* Rejected: its value is the structure
  around the search, and a search-only port would answer a different question
  than the one the tool is for.
- *Vendor the tree-sitter grammars.* Rejected: it moves the maintenance burden
  from `cargo` into a vendoring step, and buys nothing kb-c needs.
- *One daemon serving both.* Rejected: it would couple two data models that
  have no reason to be coupled, and it would put a document index's threat
  model (loopback, one operator) behind a code-search daemon's.

**What would change this.** If the two products ever need to answer a question
together — "which session produced this code, and what did the docs say about
it" — the right shape is a shared *store* and an IPC between the two daemons,
both in their own languages, not one daemon in C. That is a different project
with a different justification than "port it to C for speed", and it would need
its own evidence about where the Rust daemon actually spends its time.

**Where this is recorded elsewhere:** `INVENTORY.md` carries the per-module rows;
this record is the decision.

---

## ADR-002 — an artifact's id is derived from `(corpus, path)` only

**Status:** accepted, 2026-09-26 (corrected a day after first shipping)

**Decision.** `kbc_id_for_artifact` hashes the corpus name and the
corpus-relative path. It deliberately excludes mtime and size.

**Reasoning.** The store holds `UNIQUE(corpus, path)`. When the id was derived
from the revision, an edit changed the id, the insert collided with the document's
own previous row, and the whole reindex failed with `KBC_ERR_CONFLICT`. A file
could be ingested once and never again.

Identity is the *document*, not its current revision. mtime and size are the
change-detection fast path — they answer "has this changed?" — and they were
being used as the document's *name*, which they are not.

**Alternatives.** Minting a new id per revision and keeping a revision history
table; rejected as a feature nobody asked for, implemented under a bug fix.

---

## ADR-003 — the query path's scoring scratch is thread-local, not mutex-guarded

**Status:** accepted, 2026-09-26

**Decision.** `kbc_index_bm25`'s accumulator, generation stamps and top-k heap
live in thread-local storage with a `pthread_key` destructor.

**Reasoning.** The daemon runs four HTTP workers, each serving searches
concurrently against one index. A shared accumulator behind the index's `const`
pointer was a live data race — measured, not theorised: 65 ThreadSanitizer races
before the fix, 0 after, with no added allocation per query.

A mutex would have been correct and would have serialised the one thing the port
exists to make fast. The per-thread destructor means a short-lived query thread
returns its scratch instead of leaking it: 2,000 short-lived threads left the
live heap byte-for-byte unchanged.

**Alternatives.** A mutex (correct, serialising); per-call allocation on the hot
path (correct, and the exact cost the cache existed to avoid).

---

## ADR-004 — the graph boost ships disabled, and a control binary is the proof

**Status:** accepted, 2026-09-26

**Decision.** `graph_boost` defaults to `0.0`. The backlink boost runs only when
an operator configures a weight.

**Reasoning.** One reason is left, and it is the weaker of the two this record
started with.

The first reason is **gone**. It was that the graph did not converge: edges are
owned by the source, so a document ingested after a document that links to it
stayed at in-degree 0, and neither a daemon start nor a full reindex repaired
it, because an unchanged document contributed no edge writes. That is fixed.
Unresolved targets are recorded in a `pending_links` table (schema v3) and
drained when the target arrives; a full reindex drains for *every* document the
walk saw, including unchanged ones. The property is asserted, not assumed:
`tests/test_app.c::the_link_graph_does_not_depend_on_the_ingest_order` ingests
the same two-document corpus in both orders and requires an identical edge set
and identical in-degrees.

What remains is simply that **the weight is unmeasured**. The default has to
be *provably* inert rather than probably inert, and a backlink boost changes
ranking for every user, so it should not ship on taste. A control binary was
built with the boost compiled out, and its output compared byte-for-byte
against the shipping binary across four different queries: identical with the
boost unset, different with a weight set. "Off means off" is a test.

**Revisit when** there is a measurement of what a non-zero weight does to
retrieval quality — Recall@k / MRR on the Rust CLI's own `kb bench`, which is
the tool that reports quality and no timings. It should still stay opt-in even
then: a ranking change that improves the average and demotes a specific
document is a policy decision, not a tuning one.

## ADR-005 — the score the index computes is the score that ships

**Status:** accepted, 2026-09-26

**Decision.** The index file is a private format. `KBC_INDEX_FORMAT` gates it;
`kbc_index_open` rejects a mismatch by name rather than reading it.

**Reasoning.** Two bugs in this project's short life were the same bug wearing
different clothes: a header declaring two sections the writer never wrote, and a
document arena rebuilt in one order and read back in another. Both produced a
file that was *almost* right, and the bounds check in `kbc_index_open` caught
both. The writer now refuses to write a file its own loader would reject, so the
failure is loud at write time rather than silent at read time.

Keeping a stable on-disk format across releases would mean giving up the ability
to change the layout when the layout is the thing that needs to change. The
format is version 1 and the cost is nil while there is one reader and one writer
in the same binary.

---

## ADR-006 — one array, one writer rule

**Status:** accepted, 2026-09-27

**Decision.** The daemon's shared array of live connections, `h->all_conns`, is
appended to under `h->conns_mu` — the same lock `conn_close` and the SSE
fan-out already take. There is exactly one discipline for that array, and it
is the lock; `httpd_track` is the only place that grows it.

**Reasoning.** `httpd_track` grew the array with `realloc` and did not take the
mutex. That is not a subtle omission, it is the ordinary thing to write when
you are adding another writer to a structure that two others already lock, and
it is invisible in review because every *other* writer in the file locks
correctly — the bug is the one line that does not look like its neighbours.

It was a real double free. Two workers accepting at the same instant both
`realloc`'d the same block; the loser's buffer was freed while connections
were still being written into it, and glibc killed the daemon with
`double free or corruption (!prev)`. The load harness hit it at 24+ concurrent
connections, and reproducing it on demand took 5 attempts — below 64
connections the race essentially never fires, which is why neither the test
suite nor the old python client ever saw it.

Evidence: ASan 5 of 10 runs aborted before, 10 of 10 clean after; TSan 3 of 3
reported the data race before, 3 of 3 clean after. The regression test drives
32 threads from one barrier against a 4-worker daemon over real sockets.

**Alternatives.** *One lock-free or RCU connection table.* Rejected: the array
is appended to on accept and scanned on close and on fan-out, it is bounded by
`KBC_HTTP_MAX_CONNECTIONS`, and a mutex held for a `realloc` and a `memcpy` of
a few hundred pointers is not a contention problem. A lock-free discipline
would buy complexity and a new class of bug in exactly the place that already
had one. *Per-worker connection lists, merged on demand.* Rejected: the SSE
fan-out has to enumerate every worker's list anyway, and the merge would need
the same lock under a different name.

**What would change this.** Nothing short of removing the shared array. The
rule is worth writing down rather than enforcing: when a writer is added to a
structure that other writers already lock, the review question is not "does
this line look right" but "is this the discipline the others follow".
