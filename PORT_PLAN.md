# PORT_PLAN.md — the engineering plan for kb-c

kb-c is a C17 reimplementation of the `kb` daemon and CLI. The module-by-module
map of what is and is not being ported is `INVENTORY.md`; this document is why
the port exists, what is being built in what order, how the performance claim
is measured, and what would have to be true for kb-c to be a drop-in.

Scope in one line: 231,507 LOC of Rust across six crates, of which kb-c today
claims 97,546 (42.1%) and a further 19,020 (8.2%) is scheduled below. The
remaining 49.6% is out of scope and stays in Rust. Those three figures are the
PORTED+PARTIAL, PLANNED and OUT-OF-SCOPE row sums in `INVENTORY.md`,
recomputed from the rows on 2026-09-29.

---

## 1. Why a C port is worth it, and where it is not

### 1.1 The case

**The query path is a database round-trip for work that is a memory read.**
A keyword search in the Rust build issues
`FullTextSearchQuery::new(q).select(SEARCH_PROJECTION).limit(limit)` against a
Lance dataset (`storage/lance.rs:1097-1105`). That is: open a columnar file
set, resolve a field projection, run a tantivy inverted index inside a
DataFusion plan, and materialise `DocSummary` rows — 45 fields per row — only
to read four of them and score against `k1 = 1.2, b = 0.75` with the default
Robertson/Sparck-Jones IDF. The scan is a general-purpose columnar engine
answering a question that has a four-line answer. kb-c answers it with
`mmap` + a postings walk + a bounded heap, and touches no database at all on
the read path.

**There is no Rust-side ranking code to tune.** k1 and b are *never set
anywhere in the Rust repository*; the index is built with
`Index::FTS(FtsIndexBuilder::default())` (`lance.rs:816-817`) and tantivy's
defaults apply. Fusion is 567 lines in one file (`fusion.rs`) and is already
the only ranking logic the project owns. Porting the fusion and the query
grammar without the columnar engine is the whole thesis: the parts worth
keeping are small, and the parts worth deleting are the ones that were never
written in Rust either.

**The daemon is a file watcher, a socket, and a query loop.** inotify, epoll
and `SO_REUSEPORT` are the C substrate the operating system already provides.
The Rust daemon pays for `tokio` to multiplex three file descriptors and an
inotify queue. The parts of the system that benefit from an async runtime —
long-lived external processes, the LSP sidecar, unbounded fan-out across
corpora — are precisely the parts this port does not claim.

**The deployment is a single static binary.** kb-c links libsqlite3 and
libm and nothing else. No toolchain, no `cargo build` on the target, no
`ORT_DYLIB_PATH` in the daemon's environment (the embedder stays a separate
process, which is where the model runtime already lives).

### 1.2 Where a C port is the wrong answer

Named explicitly, because each of these is a place where Rust's ecosystem
carries something C does not:

| Rust dependency | What it does | C replacement | Verdict |
|---|---|---|---|
| `lance` + `arrow` + `datafusion` | Columnar store, FTS indices, IVF-PQ vector index, compaction | `src/index.c`: hand-rolled mmap'd inverted index, BM25, brute-force cosine over an mmap'd float matrix | **Replaced.** This is the point of the port. The vector lane loses IVF-PQ above 20,000 rows and gains predictable latency below it. |
| `tantivy` (inside lance) | Query parser with phrases, `+`/`-`, `OR`, wildcards | `kbc_query` in `search.c`: the kb-level grammar from `query.rs` only | **Narrowed.** kb-c supports the kb grammar (`AND`/`OR`/`NOT`, `key:value`) and drops tantivy's own syntax. This is a behaviour difference and is listed in the parity contract. |
| `ort` (ONNX Runtime) | Runs the embedding and reranking models in-process | `kb-embedder` stays a separate process; kb-c speaks its NDJSON protocol | **Kept external on purpose.** Porting the child would mean porting ONNX Runtime. |
| `tree-sitter` | Grammar-generated parsers, used by `kb-code-server` | none | **Out of scope.** Not needed by the `kb` daemon at all; this is one of the reasons the `kb-code-*` subtree stays in Rust. |
| `tokio` | Async runtime: the storage actor, mpsc back-pressure, `spawn_blocking` for rerank | epoll event loop (`httpd.c`), a synchronous request path, inotify (`watcher.c`) | **Replaced where it is cheap, dropped where it is not.** See below. |
| `axum` + `tower` | Router, extractors, layers, CORS, rate limiting | Hand-rolled router over epoll in `httpd.c`; the 226-route table is a C switch over `(method, path-template)` | **Replaced, at real cost.** Layer ordering is behaviour (§5, invariant P6) and must be reproduced by hand. |
| `reqwest` | Async HTTP client in the CLI | Blocking sockets in `cli/main.c` | **Replaced.** The CLI is a request/response client; async buys it nothing. |
| `tracing` | Structured spans, subscriber | `log.c`: levels + JSON lines, no spans | **Narrowed.** No span tree means no trace export. |
| `refinery` | Embedded forward-only SQL migrations | A hand-maintained migration list in `store.c` | **Replaced.** Only the tables kb-c owns are migrated. |
| `ammonia` | HTML sanitiser for captures | none | **Not yet.** kb-c writes captures un-sanitised; this is a gap, listed in §7. |
| `git2` | Versions mode, blame | none | **Out of scope** for the `kb` daemon's kb-c scope. |

The two places where dropping the runtime costs real work, and where the plan
pays it in stage 3:

- **Cross-kb fan-out.** `FANOUT_CAP = 8` concurrent reads across corpora are
  `tokio::JoinSet` parallelism in Rust. kb-c will issue them on a small fixed
  worker pool with a bounded result set, not on an event loop with unbounded
  fan-out.
- **Reranking.** The Rust route runs the cross-encoder in `spawn_blocking`
  and degrades to input order on any error. kb-c calls the same sidecar over
  the same NDJSON protocol but the call is on the request thread; the request
  holds its connection open for the duration. A 200-result page with a
  50-candidate rerank window is the only place a user can observe this.

---

## 2. Architecture of the C core

### 2.1 Data flow, file on disk → search result

```
 corpus file (HTML / Markdown on disk)
   │  watcher.c  inotify per source root, debounce cfg.watcher_debounce_ms
   │             (250 ms default), per-path coalescing into one publish;
   │             extension gate, dotfile/ignore gate, exclusions.
   │             A full reindex reconciles: a file that vanished since the
   │             last walk emits watch.delete even if its mtime never moved.
   ▼  WatchWork{kind, path}
   │  app.c      a watcher event re-indexes THAT file: read → identity →
   │             parse → store row → in-place index update. There is no
   │             add-to-open, but there is an incremental in-place path, and
   │             it is guarded at both ends: the watcher publishes a
   │             corpus-relative path, and reindex_one treats an unresolvable
   │             one as a removal rather than as an edit.
   │             (kbc_index_save refuses to write a file its own loader would
   │             reject — see §2.4.5.)
   ├──────────────▶ parse.c   Markdown | HTML blocks, stable element anchors,
   │                         heading levels, title fallback, kb-* front matter
   └──────────────▶ embed.c   NDJSON to `kb-embedder`:
                             {"kind":"embed","req_id":N,"texts":[…]}
                             30 s per exchange, respawn-once on transport
                             death; no vector lane without a healthy sidecar
   ▼
   write boundary — one transaction per batch (AGENTS.md rule 10)
     store.c   artifacts + chunks + comments, bound parameters
     index.c   postings, doc meta, avg doc length
     vecstore  f32 × dim matrix at row id

   ── query ──

   q ─▶ search.c   tokenize the query (stopwords, ≤KBC_MAX_TERM_LEN);
        │         the kb query grammar (AND/OR/NOT, folder:, text:) — done,
        │         in the shape the index can answer; keys it cannot evaluate
        │         are refused with a 400 rather than searched as literals
        ├▶ index.c   BM25 over mmap'd postings, k1/b from config
        │            (1.2 / 0.75 defaults), IDF = ln(1 + (N−n+0.5)/(n+0.5));
        │            the caller supplies the searchable text, title counted
        │            twice by convention; candidate depth = max(limit, 200)
        ├▶ embed.c   query vector, through the (model, query) LRU
        │            └▶ cosine top-k over the mmap'd matrix;
        │               score = 1/(1+distance), ties by doc id ASC
        └▶ RRF k=60, dedup on id, score = Σ 1/(60+rank)
             sort DESC, take(limit, ceiling KBC_MAX_HITS)
             title boost, +0.5 × the fused score  [ported: fusion.rs:145]
             graph boost + w/60·√in/√in_max   [ported, ships DISABLED: w=0.0]
   ▼
   httpd.c  epoll accept → HTTP/1.1 → dispatch → handler → JSON;
            bearer auth on every /api route but /api/health, RFC 7807
            application/problem+json errors, same-origin-only CORS with an
            exact allowlist, a per-connection request cap, a 256-entry SSE
            ring with Last-Event-ID and its gap probe;
            beside the JSON API: artifact bytes on the parent origin, one
            artifact subdomain per artifact, a KB_SPA_DIST static fallback and
            a Prometheus /metrics text endpoint
   cli/main.c  the same requests over a blocking socket, falling back to an
            in-process app for reindex/search/get/list when no daemon answers
```

### 2.2 Memory ownership

Three tags, no fourth (`AGENTS.md` rule 3). Request-scoped work — parsed
query, token arrays, candidate rows, response JSON — allocates from one
`kbc_arena` per request, reset at the end. The index's postings, term
dictionary and doc-meta table are `mmap`ed from files carrying
`KBC_INDEX_MAGIC`, and the vector matrix from `vectors.bin`; all are
read-only for the life of the process. A rebuild walks → ingests → builds →
saves → promotes → swaps, and on any failure before the swap the old index
keeps serving. The store, the config and the httpd are the only `malloc`
states.

### 2.3 Concurrency

- `httpd.c`: epoll, one `SO_REUSEPORT` listener per worker; `http_workers`
  defaults to 4, fixed at startup. The index is immutable once open, so the
  read path takes no lock.
- `index.c`: query scratch is **thread-local**, not mutex-guarded — a
  `pthread_key_t` accumulator, created once, destroyed with the thread. A
  mutex here would serialise every concurrent HTTP worker on one shared
  candidate buffer, which is the exact serialisation the SO_REUSEPORT pool
  exists to avoid. If `pthread_key_create` fails the accessor hands back a
  borrowed buffer the caller must free itself.
- `watcher.c`: one inotify thread per app; it owns the queue and hands
  work to the app.
- `embed.c`: the sidecar's stdin/stdout are the only shared mutable state;
  one request in flight at a time, guarded by a mutex, because the protocol
  is request/response over a single pipe pair.
- `store.c`: SQLite in WAL mode behind one mutex, so a rebuild's writes and
  every reader agree on the same connection state.
- `httpd.c`'s connection array: `h->all_conns` is ONE array that every worker
  appends to, so it has exactly **one writer discipline** — `h->conns_mu`, the
  same lock `conn_close` and the SSE fan-out take. Growing it with `realloc`
  outside that lock was a real double free under concurrent load; the rule is
  DECISIONS.md ADR-006, and lock order is `conns_mu` then `mu`.

### 2.4 Decisions the build made that the design did not

Ten of them, recorded here because each changed a contract this document
originally stated:

1. **The artifact id is derived from `(corpus, path)` only** — FNV-1a over
   the two joined by `0x1f`, first 48 bits, lowercase hex. mtime and size are
   deliberately *excluded*: identity has to survive an edit, or every save
   would mint a new id and collide with the `UNIQUE(corpus, path)` row that
   already holds the slot. mtime and size remain the change-detection fast
   path, and nothing else.
2. **Postings are materialised into contiguous per-term runs at the end of a
   build** (`end_build`). During the build each term's list is a growable
   side vector; the query path cannot afford a per-term indirection through
   it, so the sorted runs are laid out once and the query becomes a
   sequential scan.
3. **Query scratch is thread-local**, not mutex-guarded — see §2.3.
4. **The JSON writer and the daemon's own test suite found defects a design
   document would not have.** The test suite is 14 ctest binaries with 531
   case functions, and it found 24 defects during the build, among them two
   use-after-frees, a data race across query threads, and the CLI's offline
   path. Treat a stage's acceptance gate as unproven until a test exists that
   fails when the behaviour is wrong.
5. **Three more defects, found by measuring the running daemon rather than by
   reading it.** The incremental path wrote an index its own loader rejected:
   two whole sections were declared in the header and never written, because
   an opened index keeps its arenas in the `mmap` while `save` wrote the empty
   heap copies. `save` now runs a self-check and refuses to write a file
   `kbc_index_open` would reject — the failure is now impossible to produce
   rather than merely unlikely. Separately, the watcher published an
   **absolute** path which `app.c` read as corpus-relative, so a file save in a
   running daemon resolved to nothing, was recorded as a *removal*, and swept
   the document out of the index. That is why a "one file changed"
   measurement taken while it was live came out at 355 ms rather than 9–14 ms:
   it was measuring the remove-and-sweep path, not the incremental one.

   The third is not in the ingest path at all: `httpd_track` grew the shared
   `h->all_conns` array with `realloc` **without** the `conns_mu` that
   `conn_close` and the SSE fan-out already take, so two workers accepting at
   once both realloc'd the same block and the loser's buffer was freed while
   connections were still being written into it — glibc's `double free or
   corruption (!prev)`. It took 5 attempts to reproduce because below 64
   connections the race essentially never fires, which is why neither the test
   suite nor the python client ever saw it. One mutex around the growth is the
   whole fix; the rule it establishes is recorded in DECISIONS.md ADR-006. All
   three have end-to-end proofs.
6. **Five more, found by a review pass over the three largest files plus
   repeated runs of the suite.** They are recorded because each one is a
   contract this document stated wrongly, and because two of them had no test
   and no symptom anyone would have looked for:

   - **A dangling symlink in a corpus made the entire reindex fail.**
     `walk_dir` stats each entry with `fstatat(..., 0)`, which *follows* the
     link, so a broken one fails with `ENOENT`; the code correctly treats that
     as "vanished, not an error" and continues. But `readdir` signals
     end-of-directory by returning `NULL` and leaving `errno` **unchanged**,
     and the loop cleared `errno` only once, before it started — so the
     `ENOENT` that `fstatat` left behind was read afterwards as a failure of
     the walk itself. `kbc reindex` returned `readdir <path>: No such file or
     directory` and indexed nothing. This is not exotic: a documents folder
     with one broken link in it could not be indexed at all. Clearing `errno`
     immediately before every `readdir` is the whole fix, and
     `a_dangling_symlink_does_not_fail_the_reindex` is deterministic rather
     than a race.
   - **`fork()` before `fflush`.** `embed.c`'s `spawn` forked without flushing
     stdio, and the child's fd 1 is about to become the sidecar's *protocol*
     pipe. Any flush in the child — a libc exit path, a sanitizer runtime, a
     library warning — injected the daemon's own buffered output into the
     reply stream. It surfaced as the TSan lane failing, with the test suite's
     own `== embed ==` banner arriving as a sidecar reply.
   - **The two index writers shared one temp file.** Both built
     `<index>.build`, and the atomic write names its temp after the *process*,
     so both opened the same file with `O_TRUNC` and wrote from offset zero.
     The promoted index was a splice `kbc_index_open` rejects, and the daemon
     refused to start until an operator deleted it.
   - **The full rebuild ran outside the write lock**, so it could promote a
     stale snapshot over a newer single-file update, with every status
     endpoint reporting success. Fixed with a `reindex_mu` held across the
     whole pass and across the single-file path's store *and* index writes —
     the store half was initially still outside it, and a `UNIQUE(doc_id, ord)`
     violation is what exposed that.
   - **An SSE connection was published to the cross-thread event fan-out one
     line before the frame ring it writes into existed**, so a publisher
     landing in that window computed `% 0` and dereferenced `NULL`. The
     connection entered the shared array at accept time, and `c->sse` is the
     only thing that makes it visible.

### 2.5 What the 2026-09-28/29 work found — and the lesson worth more

Four bodies of work — stage 1 storage, quarantine/chunking/backup/renderer, the
query cache and the serving surfaces — and four defects they surfaced that are
worth writing down because each one contradicts something this plan or the
code believed.

**A facet-injection defect, and the second copy of a scan that caused it.**
`parse.c`'s `front_matter` skipped the first line unconditionally and then
looked for a closing `---` *anywhere after it*. So a document that opens with
prose, mentions `kb-tags:` anywhere, and contains a later `---` had that facet
**indexed**, while the renderer — which had its own, correct frontmatter scan —
treated the whole document as body. `tag:x` matched a document that never
declared the tag. The fix is not a patched condition; it is **one predicate**,
`kbc_fm_fence` in the frozen `parse.h`, called by both parsers. The lesson is
the general one: a second implementation of a scan is a second bug, and the
cheapest time to notice is when the two disagree.

**A false diagnosis, chased for a session, and then disproved.** A
`chunk <id>/0: duplicate ord` error was chased as an `ord` collision until
48,000 attempts showed the collision is **unreachable** even between two apps
sharing one data directory. What actually fires is a FOREIGN KEY violation
wearing that message: the connection leaves sqlite's extended result codes off,
so a failed step hands back the primary code 19 whatever broke — the unique
index, the foreign key, a NOT NULL, a CHECK. `store.c` now reads the extended
code and names the constraint. A message that is the same for four different
causes is not a diagnosis.

**A lost sidecar reply, a handshake that never ran, and an fd leak — the last
one found while measuring the other two.** `read_line` read a whole chunk,
appended all of it, then truncated to the first newline, destroying any
complete line that arrived in the same read: 13 of 200 embeds timed out
against a sidecar whose own log showed it HAD answered. `kbc_embedder_start`
spawned and returned without handshaking, so the model was unknown on every
fresh start, and the query lane was dead until some exchange absorbed the ready
line — which is exactly the reply-losing path above. The two composed into the
~10% flake the cache's tests kept hitting. Separately, `spawn` did `F_DUPFD` and
never closed the original, so the daemon held a second write end on the child's
stdin for life: closing `in_fd` never delivered EOF, the child never exited on
its own, every stop burned the 2 s grace, and every restart leaked two
descriptors. The embed suite goes 50 s to 3.4 s with that one fixed.

**And the one that is a testing lesson, not a bug list entry: an uninitialised
struct flag that survived a green release lane AND a green ASan lane.** In
`markdown.c`, `sink.plain` was read before it was ever written. The only
reason anyone noticed is that the **TSan build rendered differently from the
release build** — 17 check failures under TSan, 0 under release, 0 under ASan.
None of the three sanitizers reports uninitialised reads; the release suite was
green by accident of that binary's stack layout. Two mistakes compounded: a
hand-rolled sink declaration that set two of three fields and bypassed the one
initializer that set the rest, and a lost line in that initializer when a field
was added by editing it. It was read because a paragraph is buffered into a
sink of its own and then rendered against the **caller's** sink, so the
top-level body sink is what the inline renderer reads at three sites — the soft
break, the hard break and the raw-tag passthrough, which are precisely the
three symptoms. Proven with `gcc -ftrivial-auto-var-init=pattern` against
`=zero` on the same `-O3` build: different pages before the fix, byte-identical
after.

So: **a sanitizer lane is evidence, not a certificate.** ASan proved memory
safety and said nothing about this; the finding came from *two lanes of the
same code disagreeing*, which is a signal a single-lane gate structurally
cannot produce. The cheap generalisation is to run the suite under a different
stack discipline and diff the output — pattern-init against zero-init is a
one-flag version of it that needs no second toolchain.

---

## 3. Staged migration

Stage 0 is built. Stage 1 is **MOSTLY BUILT**: of its six units, five are in
and the enrich hooks are not. Stage 2 is **mostly** built: the query grammar and
prefix expansion landed, in the shape the index can answer, the `docs_query.rs`
filter overlay is in (`tag:`, `cap:`/`caps:`, `index:`; `scope:` and `since:`
stay refused), and link extraction — including wikilinks — is in. Stage 3 is
**mostly** built: `problem+json`, `/api/identity`, `/api/kbs`, CORS, rate
limiting, `X-Kb-Token` and the query-embedding cache are in, so what remains
there is the route-table check and the verbs that depend on the routes and the
auth ladder.
Stage 4 is **mostly** built: multipart capture, the three link/anchor routes
and the move are in, and what remains there is `links suggest`/`apply` (pure
functions of the out-of-scope `kb_core::mentions`), the anchor corkboard and
stale-list routes, and a user-facing `mv`.
Stage 5's **serving** half is in — artifact bytes, the artifact
subdomain, the static fallback, the Prometheus endpoint and the renderer —
with the listing, the desk surface and the outbound verbs still PLANNED.
Stage 6 is **mostly** built: the measurement and the `VACUUM INTO` + tar
backup are in, `kbc prune` and `kbc metrics` landed with them, and what is
left is `bench discover`/`run`.
The stages that are partly in already are therefore *gaps in what exists*, not
greenfield.

**Two units are mis-scoped rather than merely unfinished, and the reason is the
same in both cases: the LOC column was read as the cost.** `markdown.rs` is 800
lines of *wrapper* around comrak, so scoping it by size hid an entire Markdown
engine underneath; taking the engine is a decision (the operator's: kb-c takes
no third-party dependency), and the port that followed is a hand-rolled
renderer with a stated narrower feature set — see §3 stage 5 and
`INVENTORY.md`. `doctor.rs` is 2,930 lines about the **Claude Code provenance
chain** — session marker files, hook probes, a recall-outcome check — and kb-c
has no sessions, no memory/recall and no hooks to police, so it is now
OUT-OF-SCOPE rather than stage-5 work. Neither was caught by a stage review;
both were caught by reading what the lines are *about*.

### Stage 0 — the core (BUILT)

Units, as they are in the tree: `config.c`, `store.c` + the migration ladder,
`parse.c` + the tokenizer and its stopword table, `ids.c`, `index.c`
(inverted index + BM25), `search.c` (RRF, the query grammar and the filter
overlay), `embed.c` (sidecar client) and `kbc_vecstore`, `httpd.c` (epoll,
SO_REUSEPORT, the nine JSON routes stage 0 had plus the `/` banner, bearer and
SSE — stages 3 and 5 added more, and the exported `KBC_ROUTES` table is the
authority), `watcher.c` (inotify), `json.c`, `mem.c`, `log.c`, `kbc.c`,
`cli/main.c`, the frozen headers.

The tree today, which is more than stage 0: 31,392 lines of C in `src/`, 2,261
of frozen header (18 headers), 3,648 in the CLI.

Acceptance gate (met): Release, `-DKBC_SANITIZE=ON` and TSan builds clean under
`-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wstrict-prototypes
-Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Werror`; `ctest` green in
all three — 14 suites, 531 case functions, last recorded at `8fc54e3` with
14/14 in each lane from clean trees.

Behaviour it matches, per the contracts extracted from the Rust source: BM25
at tantivy's defaults (`k1 = 1.2`, `b = 0.75`, configurable); RRF at
`k = 60` plus the title boost at `fusion.rs`'s own constants; the embedder's
envelope names and `req_id` discipline, verified against the **production**
`kb-embedder` rather than a fake one; constant-time bearer comparison; delete
cascades from `artifacts` to `chunks` and `comments`.

Behaviour it does **not** match, which an earlier draft of this plan claimed
it did — each is a later stage, not a stage-0 unit:

- the artifact id is **FNV-1a over `(corpus, path)`**, not
  `sha256(rel_path)[0..6]` (§2.4.1);
- title fallback is first `h1`, else first non-empty block, else the file
  stem — there is no `"untitled"` literal;
- chunking was one chunk per parsed block, not a 280-word window with 60-word
  overlap, and chunk 0 was not the title passage. **That is now stage 1 and it
  is built** — see below;
- no `since:` dates, and no phrase search — the original has neither: a quoted
  `"…"` there quotes an atom *value* (`folder:"deep notes"`), not a phrase;
- the **title boost** is ported (`TITLE_BOOST = 0.5`, `fusion.rs:145`, applied
  to the fused score before filtering and truncation), and the **graph boost**
  is ported too (`fusion.rs:214`) but **ships disabled** — `graph_boost`
  defaults to `0.0` and the weight is unmeasured. The graph it reads converges
  (see stage 2's pending-links drain);
  a `cap:` filter matches declared `kb-caps` metadata, not the original's
  capability analysis;
- the daemon serves RFC 7807 `application/problem+json` errors, and
  `/api/identity` and `/api/kbs` exist, both token-gated.

### Stage 1 — storage completeness — MOSTLY BUILT

Two units' substance landed before the rest, and they are the ones the
acceptance gate cares about most: **the incremental in-place index update**
and, after it, **the delta journal that makes it cheap**. `app.c` re-indexes a
single file rather than rebuilding the corpus (`reindex_one`), with the store
row committed before the index so a reader never sees an index naming an
uncommitted document. That work also produced the first two defects in §2.4.5
and their fixes.

The in-place update alone did not deliver the property this stage is for. A
single-file save still rewrote the whole index and fsynced it, so its cost
tracked the INDEX: 251 ms / 500 ms / 1.19 s at 1,000 / 5,000 / 20,000
documents. Measurement then showed the barrier was 64.6% of that and the
serializer only 13.1%, so a cheaper serializer could recover at most the 13% it
was responsible for. The fix was a delta journal — a single-document update
appends a record and fsyncs that, and `kbc_index_open` replays it — which puts
the save at **30.6 / 33.3 / 36.3 ms at 1,000 / 5,000 / 20,000 documents** in a
matched A/B, flat across a 20× range of corpus. That is this host's fsync
floor: the journal appends ~1.9 KB after ten appends and still costs 36.3 ms,
and fsync on this device costs 33.9 ms for 90 bytes. The cost no longer scales
with the corpus. `BENCHMARKS.md` has the breakdown and the before/after.

**Units 1–4 and 6 are in; unit 5, the enrich hooks, is not.** They landed
together in `5b17573` (storage, the resolution ladder, the root guard) and
`8dc36bb` (quarantine, chunking, backup, the renderer).

Units:
1. `store.c`: **BUILT.** The flat CREATE-TABLE array is now a versioned
   migration list in refinery's shape — an ordered `(version, sql)` array, one
   transaction per version, with the recorded version bumped **inside** that
   transaction so a failure part-way leaves the last version that fully
   committed. There is deliberately no `KBC_SCHEMA_VERSION` constant to fall
   out of step with the last entry; `BINARY_EPOCH` is read off the list itself.
   Eight tables landed across v5–v10 — `sources`, `index_runs`, `errors`,
   `history`, `corkboard`, `pinned_memories`, `excluded_files`,
   `doc_first_seen` — on top of `edges` (v2), `pending_links` (v3) and
   `doc_metas` (v4), and v11–v12 added `moves` and its `abandoned_at` column,
   which is where stage 4's move keeps its state (fifteen data tables in all).
   The partial indexes are ported as partial, because each
   exists for exactly one query and restricting it to the rows that query can
   match is what turns a scan into a seek. **The epoch-ahead refusal is in**
   (`kb-core/src/sibling.rs:119` — this section used to cite
   `storage/sibling.rs:224`, a path that does not exist and a line that is
   wrong): a volume whose `MAX(version)` is ahead of this binary's embedded set
   is refused before the migration run, so a refused boot never writes to a
   volume it cannot understand. `identity_backfill_done` is deliberately NOT
   ported — it gates a per-user read/unread backfill over list entries, and
   kb-c has neither, so a marker with no writer and no reader is dead weight.
2. `app.c`: the reconcile delete pass — **BUILT.** Files that vanished since
   the last walk emit `watch.delete` even when their mtime never changed. The
   sweep deliberately refuses to run it for a file that is merely unwalked, so
   an unreadable file is not a deleted one.
3. `app.c`: quarantine — **BUILT.** `retry_count_for_path_hash >= 3` sets
   `embed_gated`; the document still indexes and stays BM25-searchable because
   the embedding column is nullable by design. The error row is *not* cleared
   on the gated success path — clearing it would un-gate the document on the
   next pass, which then takes a real embed, fails, returns at count 1, and
   oscillates forever. The gate is keyed by `(path, CONTENT HASH)`, matching
   the original: editing a document is how an operator fixes one that failed,
   so without the hash an edited document never leaves quarantine. Two
   corrections to the wording above: the gate is the `errors` row, **not** a
   `<state>/quarantine/<kb>/` directory, and the earlier `app.c` fallback of
   embedding anyway when the gate cannot be read now logs and does exactly
   that rather than failing the pass.
4. `app.c`: delete-by-path — **BUILT.** `kbc_app_delete_path` cascades to
   chunks and comments and removes `edges` and `doc_metas`, and **leaves**
   `history`, `corkboard`, `pinned_memories` and `reading_sections` untouched
   (invariant 8). Inbound edges are demoted to `pending_links` rather than
   dropped, so a document that comes back finds its backlinks.
5. `app.c`: the eight `enrich.rs` hooks, in Rust's registration order. **NOT
   BUILT** — the only unit of this stage with nothing in the tree.
6. `app.c` + `include/kbc/chunk.h`: **BUILT.** The 280-word window with 60-word
   overlap and chunk 0 as the title passage, replacing one-chunk-per-parsed-
   block. The contract is frozen because the constants are a contract; the
   implementation lives in the ingest path because a chunk *is* a
   storage-shaped write to the `chunks` table. The 512 cap keeps the first 512
   and reports the ORIGINAL count, so a caller logging "N chunks" reports what
   the document produced.

Dependency order: 1 → 2 → 3 → 4 → 5 → 6. Every unit is independent of the HTTP
layer.

Acceptance gate, and how much of it is met: a corpus of N files can be indexed,
one deleted from disk, reconciled, and re-indexed with no orphan rows in
`artifacts` or `chunks` — **met**; a file that fails to embed three times
stops being embedded and remains findable by keyword — **met, and the thing
that is gated is the EMBED, not the file**: `embed_gated` skips the sidecar
call and returns, the artifact row, its chunks and its edges are written as
usual, and the embedding column is nullable by design, so the document stays
indexed and BM25-searchable. The gate is the `errors` row keyed by
`(path, content hash)`, **not** a `<state>/quarantine/<kb>/` directory — which
is what this section used to promise, and what a reader checking the tree
will not find.

Behaviour it must match: the four ingest constants
(`INGEST_QUEUE_CAPACITY = 1024`, `INGEST_BATCH_MAX_DOCS = 32`,
`INGEST_BATCH_MAX_BYTES = 8 MiB`, `QUARANTINE_THRESHOLD = 3`) and the
enrich hook ordering, which is run order. Note that there is still no batch
drain: a watcher event re-indexes that one file (`reindex_one` in `app.c`), so
these constants govern the first implementation of batching, not the current
one.

### Stage 2 — query grammar and filters — MOSTLY BUILT

Done: the `query.rs` grammar in the shape the index can answer — `AND`/`OR`/
`NOT` in both spellings plus `&&`/`||`/`!`, `(…)` groups, implicit AND, a
bareword with no colon becoming free text, depth 64, the 64-conjunct DNF cap
with the original's warning text, and De Morgan (in DNF, exactly "flip every
literal"). `folder:` works as a path-prefix facet and `text:`/`q:` collect
free text in document order, space-joined across the whole expression, exactly
as `query.rs:419` does. Prefix expansion is wired into the BM25 arm.

**The `docs_query.rs` filter overlay is built.** `tag:`, `cap:`/`caps:` and
`index:` are no longer HTTP 400s. Facets come from the document's own markup —
`<meta name="kb-tags" content="a, b">` and Markdown front matter — extracted
into `doc_metas` (schema v4) and evaluated **once per query** as a membership
set of index doc ids. Two properties are invariants rather than features, and
both are tested at the store, search and app layers:

- **A filter that matches nothing returns ZERO rows, never everything.** This
  is the whole reason the refusal design existed; the overlay is only allowed
  to remove rows, never to add them, and "no rows scored" must stay
  distinguishable from "every row was filtered out".
- **Value comparison is CASE-SENSITIVE**, as in the original
  (`docs_query.rs:258` compares `String`s with `==`).

**The gotcha, which a user will trip over and which is inherited, not
invented: within one DNF conjunct, tags are ANY-OF.** So `tag:a AND tag:b`
means "has a **or** has b", because the original ANDs *predicates* rather than
literals (`query.rs:246`: a row matches if it satisfies any conjunct). Verified
against the store on the benchmark corpus: 2 documents carry `research`, 18
carry `guide`, 0 carry both, and the union is 20 — which is exactly what both
`tag:research AND tag:guide` and `tag:research OR tag:guide` return.

`cap:` matches declared `kb-caps` metadata, **not** the original's capability
ANALYSIS (`svg_count`, `has_canvas`, `code_block_count`), which kb-c does not
perform. That is a deliberate deviation and is stated as one.

`scope:` **remains refused**, because the original's `apply_atom` has an empty
arm for it (`query.rs:403-406`) — the original evaluates it nowhere either, so
inventing a meaning here would be a filter the Rust daemon cannot reproduce.
`since:` remains refused too; its value grammar is parsed
(`kbc_since_value_ns`) but the layer that would apply it does not exist.
Refusing rather than mis-searching is a deliberate departure: a query that
quietly means something else is worse than an error.

There is no phrase search, and there is none to port: in the original a quoted
`"…"` quotes an atom *value* (`folder:"deep notes"`), not a phrase, and the kb
SEARCH path hands `?q=` **unparsed** to BM25 (`routes/search.rs:1163`), so its
boolean structure never reached the lexical arm either. A quoted phrase is two
terms on both sides, and a test pins that reading.

**The link graph converges.** Edges are owned by the source, so a document
ingested after a document that links to it used to stay at in-degree 0 — and
neither a daemon start nor a full reindex repaired it, because an unchanged
document contributed no edge writes. Unresolved targets are now recorded in a
`pending_links` table (schema v3) and drained when the target arrives; a full
reindex drains for **every** document the walk saw, including unchanged ones.
`kbc_store_forget_document` demotes a removed document's inbound edges to
pending rows rather than dropping them, so a document that comes back finds
its backlinks. The property is asserted, not assumed:
`tests/test_app.c::the_link_graph_does_not_depend_on_the_ingest_order` ingests
the same two-document corpus in **both** orders and requires an identical edge
set and identical in-degrees.

**Wikilink extraction is in.** `[[target]]` and `[[target|alias]]` go through
the **same** normaliser as `[text](target)` and `<a href>`, so `[[y.md#part]]`
and `[y](y.md#part)` produce the identical `y.md`. A bare `[[target]]` takes
the target as its visible text, matching `links.rs`, which collapses
label == url to `alias: None`. `![[embed]]` is an image, not a link.

**The resolution ladder is built and it is on the ingest path.** The four-tier
id/path/title/basename ladder in `links.rs` `resolve` / `ResolveIndex` is
`src/links.c` behind a frozen `include/kbc/links.h`, including the `Ambiguous`
outcome: tiers 1–2 (id, path) cannot be ambiguous, and when a title or a
basename matches several documents the answer is AMBIGUOUS and never "the first
one", because picking by index order would be nondeterministic and would flip
on the next reindex. `app.c` builds ONE `kbc_resolve_index` over the corpus's
candidates and resolves every raw target through it, rather than looking each
one up by exact path — which closed the gap this section used to name: before
it, `[[Some Title]]` and a bare `[[deploy]]` found nothing even when exactly
one document answered. An ambiguous target is **counted, not written**: no
edge, and no pending row either, because the corpus did not say which document
it meant and a pending row would resolve to whichever arrived first.

Acceptance gate: the port of `query.rs`'s own unit tests passes — n-ary
`AND`/`OR` flattening, De Morgan over groups, the 64-conjunct cap with its
exact warning string, depth 64, and the bareword-becomes-`Key::Text` rule. Each
of those is a separate test because each is a separate user-visible behaviour.
`since:Nd|Nh|<unix>` and `since:all`-as-an-error are **not** in the gate while
`since:` is refused, so they are deferred with the rest of the key.

Behaviour it must match: `MAX_DNF_CONJUNCTS = 64`,
`MAX_PARSE_DEPTH = 64`, `Text` collection in document order space-joined
across the whole expression, `NOT text:` / `NOT scope:` as warnings rather than
silent misfilters, and the case-sensitive facet comparison.

### Stage 3 — HTTP surface and CLI parity — MOSTLY BUILT

Done since this section was written: **RFC 7807 `application/problem+json`
errors** across the whole status map with `urn:kb:errors:*` types (an unmatched
`/api/*` is a problem+json 404); **`GET /api/identity`** and **`GET
/api/kbs`**, both token-gated and both in the exported `KBC_ROUTES` table,
which unblocks the CLI's two probes; **CORS**, same-origin by default,
reflecting an `Origin` only on an exact `KBC_CORS_ORIGINS` allowlist match
with no wildcard anywhere; and **fixed-window rate limiting**, per connection,
120 req/s by default via `KBC_RATE_LIMIT_RPS`, answering 429 with
`Retry-After`. The rate limit exists to protect **worker fairness, not the
corpus** — a per-connection window cannot tell one busy client from many, so
it is a politeness bound and nothing more.

Also worth recording, because it made a whole tier of this design dead code:
nothing ever set `cfg->token`, so the bearer tier could never be satisfied and
a non-loopback bind was **always** refused. `kbc_config_load_token` now
resolves the token once, from the literal value or the token file, and the
daemon calls it before validate/bind, so a `0.0.0.0` bind with a token works —
no header 401, wrong token 403, correct token 200. The token value never
appears in `kbc config show`.

Also in since this section was written, on the token and event side:
**`X-Kb-Token` as a second carrier** beside `Authorization: Bearer` (see P8 —
the two are not required to agree, and disagreement is not an error, because
the original returns the first candidate that matches and never compares);
**the SSE gap probe** (`kb-core/src/events.rs`, not a kb-server file as this
section used to imply), whose gap half is deterministically tested and whose
lag half ships untested by decision (DECISIONS.md ADR-007); and **the
query-embedding cache**, an LRU keyed by `(model, query)` whose linear scan is a
measured choice — about 19 µs at capacity 1024 against a 50–100 ms sidecar
round trip.

Still to do: the route-table check, and the verbs that depend on the auth
ladder and the routes above them.

The auth ladder is likewise built, at three tiers rather than the original's
four: open (no token configured), loopback, and token. The two that are not are
**ruled out, not pending** — the multi-user `Token` registry (`auth.tokens`)
and the trusted-proxy `Header` tier have no `kbc_config` field to carry them,
and honouring a fixed `Remote-User` nobody can switch off would impose a
proxy-trust decision on every operator instead of letting them make one.

**The route-table check, and what it is worth.** An earlier draft of this
section asked kb-c to reproduce `crates/kb-server/tests/api_docs.rs` — the
single regex plus byte scan over `router.rs` that keeps the original's
`docs/api-routes.md` in step with its Rust route table, so a hand-added route
without a docs update fails CI. That was misconceived and is struck. The
extractor parses **Rust source**; kb-c has C source, so a port would find
nothing. And the property it guarantees — no route is undiscoverable — is a
property of a doc file, and kb-c has no doc file to keep in step. What kb-c can
honestly do is two things it already has the raw material for:

- generate the route table from kb-c's own dispatch and check it against the
  exported `KBC_ROUTES` in `src/httpd.c`, both directions: no route exists
  without being listed, and no listed route lacks one;
- diff the kb-c subset against the committed `docs/api-routes.md` in the Rust
  tree, which is a data file and therefore readable.

What the first proves: no route is undiscoverable, and no route's
`needs_auth` goes unchecked. What it does **not** prove: anything about route
**behaviour** — no parity check over a table of path/method/auth triples says
what a handler returns. Stated plainly, because a check that sounds stronger
than it is worse than none. Separately, the running daemon already satisfies
the discoverability half in a stronger form than a docs file staying in step:
`GET /` answers a plain-text route banner listing what it serves.

The 226 route registrations are the scope, not 226 units. The kb-c subset is
the `PORTED` + `PLANNED` rows in `INVENTORY.md`; the rest stay in Rust
forever.

The routes `cli/main.c` depends on, which `httpd.c` serves today, verified
against the running binary and exported in `KBC_ROUTES`: `GET /api/health`,
`GET /api/identity`, `GET /api/kbs`, `GET /api/stats`, `GET /api/search`
(`q`, `kb`, `kind`, `mode`, `limit`, `offset`; an object whose array is
`hits[]` or `results[]`), `GET /api/artifacts` (`kb`, `kind`, `limit`,
`offset`), `GET /api/artifacts/{id}` (`source=1`), `GET
/api/kb/{kb}/artifact/{id}` (the artifact's bytes, sandboxed), `POST
/api/reindex` (`{}` or `{"kb":"…"}`), `GET /api/events` (SSE) and `GET
/metrics`. `GET /` returns a plain-text route banner, `GET *` falls back to
the `KB_SPA_DIST` static handler, and `<id>.artifacts.localhost/*` is the
artifact subdomain. The Prometheus text endpoint is a **top-level** route, not
under `/api`, which is the placement the original has and the reason it is
neither counted nor rate-limited as an API request.

`POST /api/reindex` answers **202** with `{"docs":N,"took_us":N}` after a
synchronous rescan — the count is there today and the CLI prints it. If a
future build makes it asynchronous the CLI prints `reindex accepted` instead
of a document count; 202 is a success path, not a case to fold into the
2xx-with-a-count branch. The CLI accepts `hits`, `results`, `artifacts` or
`items` and falls back to the raw body, so the array name is not yet a parity
constraint. A 401 makes the CLI print ``run `kbc token generate` ``.

The CLI half of stage 3 is no longer greenfield: `reindex`, `search`, `get`
and `list` run against a daemon when one answers and fall back to an
in-process app when none does, announcing the fallback on stderr; `status`
has no fallback and exits 2 when no daemon answers.

### Stage 4 — capture, links, anchors — MOSTLY BUILT

**Capture is in.** `POST /api/kb/{kb}/capture` in `src/httpd.c`, multipart,
answering 201 with the created ids and source-relative paths in request
order. Both caps are checked and both refuse rather than truncate: at most 50
files, 10 MiB per file, 64 MiB for the request as received, and a body past
the file-count cap is a 400 that writes nothing. Only `.md`/`.markdown` is
accepted (415 otherwise), because the capture engine is Markdown-only — the
HTML pipeline stamps `<meta>` into `<head>` and runs ammonia, and a
head-splice with no sanitiser is a way to persist attacker markup into a
trusted origin.

**The frontmatter key ORDER is the contract, because the acceptance gate is a
byte-diff against the original** — and the subtlety is that the order is the
order of *first insertion*, not a layout. `kb-category`, `kb-tags`,
`kb-capture-original`, `kb-capture-url`, `kb-capture-at`, `kb-session`,
`kb-expires-at` (title is never stamped: the caller's title steers the output
FILENAME and nothing else), each applied through the original's own
line-oriented setter, which edits a key the document already carries **in
place** and appends only the ones it lacks. A port that treats the list as a
strict order passes every test and fails every diff. The 60-char slug policy
is ported with it. The URL is stored and **never dereferenced** — the SSRF
ruling, and the test binds a real listener and asserts nothing connects.

**The write direction has its own traversal test, and it is the SIBLING
case**: root `<root>/kb`, decoy `<root>/kb-secret`. A `../` test passes under
both a `strncmp` guard and the component-wise `path_within()` one and
separates nothing; only the sibling does.

**Three of the link routes are in**, wire shapes field for field:
`GET /api/kb/{kb}/notes/{id}/links` (outgoing wikilinks with state
`resolved`/`ambiguous`/`dangling`, plus the backlinks), `GET
/api/kb/{kb}/backlinks/{id}` (empty array, never a 404) and `GET
/api/kb/{kb}/wikilinks/suggest` (`?q=` required, limit clamped to 50). The
ladder's AMBIGUOUS answer **discards its candidate ids on the wire** because
the original does, and the test asserts the whole row so a build that
smuggles them through any member fails.

**The anchor events are in** and fire on the TRANSITION and only on the
transition: the pass re-checks the stale set and emits nothing for a document
already known stale, so event volume is bounded by transitions rather than by
reindexes.

**The move is one store call rather than seven.** `kbc_store_rekey_artifact`
rewrites every id-keyed row in ONE transaction — `chunks`, `corkboard`,
`pinned_memories`, `doc_first_seen`, `history`, `edges` and `pending_links`
in both directions, then `comments`, then `artifacts` LAST — with the intent
recorded in `moves` BEFORE the rename and stamped after, so an interrupted
move is a listable row rather than a lost document, and a stale id resolves
to its new home through a chain-walked lookup. Three store facts made the
transaction possible at all: `PRAGMA defer_foreign_keys` (the child FKs have
no ON UPDATE CASCADE and the updates otherwise deadlock against each other);
`artifacts` rekeyed last (the delete-leftover step is an ON DELETE CASCADE
parent delete that eats children still naming the old id); and `comments`
cannot follow the UPDATE-OR-IGNORE-then-delete pattern, because the only
public way to write a comment MINTS its id and `created_at`, so an ordinary
rekey would stamp every carried comment "now".

**And a bring-up pass converges an interrupted rename**, in `kbc_app_open`,
deciding from the FILESYSTEM because the corpus on disk is the authority. It
takes no lock, triggers no reindex, and a store read that FAILS makes the
daemon refuse to start — an empty list and an unreadable table are different
answers. It **warns and abandons** rather than re-running the rekey: the
function is handed `old_id` and `old_rel` and cannot recover `new_rel`,
because `new_id` is a hash OF `new_rel`; and it cannot know the carry
survived, since `comments` is ON DELETE CASCADE from `artifacts(id)`. That is
the hole `abandoned_at` (migration 12) fills: a third terminal state,
explicitly **not** a redirect, because stamping an abandoned move "completed"
makes `moves_lookup` send a stale id to a destination that does not exist —
a wrong answer rather than a missing one, and reachable because
`kbc_app_get_artifact` follows the chain. It is migration 12 rather than a
fold into 11 because the fold's safety condition is "no volume anywhere has
recorded version 11", not "unreleased": `migrate_locked` skips every version
at or below what a volume has, so the fold's failure is a volume that claims
to be current and then says "no such column" — the silent drift the epoch
guard exists to make loud. The one branch that ends nothing is the one that
REFUSES: a row whose path is not corpus-relative stays in flight, because a
pass that publishes a decision built out of a corrupt record is worse than a
row that keeps warning.

Units that remain: `links suggest` / `links apply`, which are pure functions
of `kb_core::mentions` (`INVENTORY.md` marks it OUT-OF-SCOPE, so there is
nothing to port them from); the anchor corkboard and `GET /anchors/stale`
routes — the store table and the stale set are in, the three routes are not;
and a user-facing `mv`. **The move has no verb and no route**: the seam is
`kbc_app_move_path` and its bring-up pass, and both are reachable only from
the tests today.

Acceptance gate: capture the same bytes through both implementations and
diff the resulting file, tag list and id. **Met on the C side** — the written
bytes are asserted literally, key by key and in order, two captures of the
same input are byte-identical, and an edited-in-place key is asserted
separately from an appended one. The cross-implementation diff itself needs
both binaries and is not yet run.

### Stage 5 — serving, SPA, metrics, outbound — MOSTLY BUILT

**The serving half is built.** `GET /api/kb/{kb}/artifact/{id}` serves the
artifact's bytes inline on the trusted app origin with
`X-Content-Type-Options: nosniff`, `Content-Security-Policy: sandbox` and
`X-Kb-Artifact-Id` in the original's order, plus `?download=1`. The artifact
**subdomain** is the same serve on its own origin, chosen by `Host:`, with a
**different** header set — a `frame-ancestors` CSP instead of `sandbox`, and a
different order — because the point of the subdomain is origin isolation, not
a second copy of the same page. The **static fallback** serves `KB_SPA_DIST`
behind the same root guard, and a `KB_SPA_DIST` with no `index.html` refuses
the daemon at startup rather than 404ing everything. The **Prometheus text
endpoint** is in, at `/metrics`, text exposition 0.0.4.

**The traversal guard is the security item in this stage, and the part a future
porter gets wrong.** The original's check is `Path::starts_with`, which is
**component-wise**: `/root/data` is a prefix of `/root/data` and of
`/root/data/x`, and is *not* a prefix of `/root/data-secret`. A C port that
writes `strncmp(path, root, strlen(root)) == 0` accepts the sibling directory
and serves its bytes. `path_within()` walks both paths component by component,
and `realpath(3)` — the same all-or-nothing operation as Rust's
`Path::canonicalize` — resolves `..` and symlinks BEFORE the comparison, so the
guard never runs on a raw path.

**And the test that catches it is a SIBLING DIRECTORY, not a `../` case**,
because `strncmp` rejects `../` too: a traversal suite made only of `..` cases
passes on the buggy implementation. `tests/test_httpd.c::
no_path_outside_the_source_root_is_ever_served` pins a sibling directory that
shares every byte of the root's name, a symlink escape, an absolute path, and
`..` both plain and percent-encoded — and opens with a **control** case that an
in-root sibling asset IS served, so a guard that refused everything cannot
pass either. Replacing `path_within` with `strncmp` was run as a mutation:
two of those cases fail with the sibling answering 200 and leaking the secret.

Also in: the **Markdown renderer**, and it is the one unit in this plan that
changed shape. The 800-line `markdown.rs` is a wrapper around comrak, so
scoping it by LOC hid an entire engine; the operator's decision is that kb-c
takes no third-party dependency, so `src/markdown.c` is hand-rolled and
deliberately narrower — reference links, footnotes, superscript, description
lists and entity references render as ordinary text. A five-lens review
campaign took the supported-subset divergences from 2,940 to 8 across a
4,409-document differential corpus, and CommonMark semantic divergences from
257 to 120 of 652 spec examples, of which 75 are reference-link cases the
header declares not supported. The divergence is written down rather than left
for a reader to discover. **The renderer is now the artifact route's
production path**: a `.md` is served as the rendered page on BOTH the parent
origin and the artifact subdomain, and a render failure is a visible error
never a silent fall back to raw bytes — a route that serves raw source when
the renderer fails passes every test whether or not the renderer works.
`?download=1` is the exception and stays a download: the SOURCE, with the
`.html` name rewrite kept as the original's wire contract rather than as a
claim that the body is HTML.

Still PLANNED here: the `/docs` LISTING, the desk surface, attachment blobs,
the outbound webhook URL validation, `download` as its own verb, log-level
control, the remaining CLI verbs, and a SPA to point `KB_SPA_DIST` at.

Acceptance gate (met): the traversal suite — sibling directory, `..` in a
segment, an absolute path, a symlink out of the source root, a
percent-encoded `..` — each answers 400 or 404 and never a byte outside the
root.

### Stage 6 — measurement, backup, maintenance — MOSTLY BUILT

Done: the measurement half. `BENCHMARKS.md` exists and is written from runs on
this host, and it covers the head-to-head latency comparison, a matched-corpus
concurrency A/B against Rust, a kb-c concurrency ladder measured with a load
client (`bench/kbcbench-client.c`) that is cheap enough not to be the
bottleneck, the vector lane against the production sidecar, and a scale ladder
that stops at 20,000 documents and **locates no knee**. `bench/` holds the
harnesses (`bench-kbc.sh`, `bench-rust.sh`, `bench-rust-concurrency.sh`,
`bench-scale.sh`, `bench-vector.sh`) and `kbc bench` is the in-process one.

Also done: the **backup**. `kbc backup <kb> [--out PATH] [--all]` stages a
`VACUUM INTO` snapshot and tars it; `kbc restore <tarball> --kb NAME [--force]`
extracts one member. `VACUUM INTO` rather than a file copy because tar-ing the
live database while the daemon writes captures a torn, half-applied
transaction. Three deliberate divergences from the Rust, each with a reason: the
destination is a **bound parameter** rather than a literal with doubled
apostrophes (kb-c links sqlite 3.27+, and rule 9 says all SQL is bound);
`--` always precedes the member list (a corpus name beginning with `-` is
otherwise read by tar as an option); and the source is opened **READONLY**,
because rusqlite's `Connection::open` is `SQLITE_OPEN_CREATE` and a mistyped
source silently creates an empty database — the backup then "succeeds" with
nothing in it, which is the failure mode a backup exists to prevent. A failed
backup leaves nothing behind. **Exit codes follow this binary's convention, not
clap's**, so a bad flag exits the same from `search` and from `backup`.

One thing a reader should know about the tarball, because it is not what the
Rust's is: **it carries the store, not the index.** The Rust's `lance/` clause
is its index; kb-c's index is the mmap'd postings file and `vectors.bin` beside
it, and neither is in the archive. A restored kb-c therefore has a working
store and no index, and `kbc reindex` rebuilds it.

**Retention is in, and the premise it was scoped under was wrong.** `kbc prune
--days N [--apply]` removes reading-history rows older than the window, and
nothing else. The question this stage originally asked — does pruning touch
the store only, or the store AND the index AND the graph? — does not arise,
because the original never removes a document by age: `edges` has no
timestamp column, so it CANNOT be time-pruned, and the R2-cascade tables are
pruned per-artifact on delete. Wiring `kbc_app_delete_path` into retention
would have invented a corpus-deleting verb the original does not have, so the
test is the inverse of the obvious one: after a prune the document must still
be in the store AND still be findable. The delete goes through the store
under its own mutex (a second sqlite connection would be a second writer
outside it), an unset `--days` is a user error rather than a silent success,
and a WAL `TRUNCATE` checkpoint follows an applied prune on a best-effort
basis — a failed reclaim costs disk, and rolling the delete back to make it
succeed would cost the operator rows. **The dry-run default is kb-c's own and
is deliberate**: the original arms a daily background task from
`[retention]` rather than offering an operator verb, and `--apply` is the
opt-in.

**Metrics export is in as a verb.** `kbc metrics [--out PATH]` is a **client**
of the `/metrics` route the daemon already serves, not a second
implementation of it. There is deliberately **no in-process fallback**,
unlike `search`/`reindex`/`get`/`list`: every counter the endpoint renders
lives in the running daemon's registry, so a fallback would print a
healthy-looking exposition of zeroes. `kbc status` sets the precedent and its
reason — no fallback, exit 2 when no daemon answers. `--out` is the one
divergence from the Rust's verb, which cannot save its scrape: `kb metrics >
file` is the only way to keep one there, and a shell redirect is not something
a verb can be tested through.

Not done: `bench discover`/`run` as CLI subcommands, and the log-level
control.

---

## 4. The performance thesis, and how each claim is measured

**`BENCHMARKS.md` exists and holds the measured numbers**, and it is the
authority for anything in this document that sounds like a result. What
follows is the thesis: a mechanism per claim and the measurement that turns it
into a result. Where a claim has been measured, the number is in
`BENCHMARKS.md` and not repeated here; where it has not, this is a mechanism
and an unmeasured claim, and is labelled as one.

### M1 — mmap'd postings

Mechanism: the term dictionary, the postings array and the doc-meta table are
`mmap`ed regions with a documented layout, written and validated against
`KBC_INDEX_MAGIC`; the vector matrix is `vectors.bin`. A keyword query
resolves terms in the dictionary (a hash-probe over a sorted term table), then
walks postings as `{doc_id:u32, tf:u16, field_mask:u16}` records and
accumulates a score array. The pages the query actually touches are faulted
in; the rest of the corpus is not in the process's resident set. There is no
decompression, no deserialisation, and no `Vec<DocSummary>` for 45 columns —
the candidate row is `{doc_id, score}` and the doc-meta lookup happens after
ranking.

Measure: wall-clock p50/p95/p99 for `/api/search?mode=keyword&limit=20` over
a fixed corpus, with `perf stat` reporting `minor-faults` and
`rss`. The claim under test is that resident set stays flat as corpus size
grows 10× while p99 latency grows sub-linearly.

### M2 — bounded top-k heap instead of a full sort

Mechanism: the BM25 arm does not sort. It keeps a min-heap of size `limit`
(clamped to `KBC_MAX_HITS`), and the pop/replace cost is `O(log k)` per
surviving candidate instead of `O(n log n)` over every posting touched. The
Rust side sorts the arm (`rank_sort`, `lance.rs:3223-3231`) before fusing,
because the columnar engine hands back a materialized result set.

Tie-break is exact, and this is where a heap is easy to get wrong: the
comparator is `(score DESC, doc_id ASC)` with NaN comparing Equal, and the
final fuse sorts by `(score DESC, first-appearance ordinal ASC)`. The heap
must be a *total* order on `(score, doc_id)` or a heap-pop will return a
different set of ties than a stable sort would.

Measure: a microbenchmark on synthetic corpora at 10k / 100k / 1M postings
where the candidate set is larger than `k` by 100×, reported as time and
allocations. The claim under test is that the arm cost tracks the candidate
set, not the posting count.

### M3 — no SQLite on the query path

Mechanism: `store.c` is opened for writes and for artifact retrieval by id.
Ranking, filtering, fusion and rendering read only the mmap'd index. SQLite
is never opened, locked or queried by a `/api/search` request. This is the
difference from the Rust build, where a keyword search is a query against the
Lance dataset.

Measure: `strace -f -e trace=openat` on a search request, asserting zero
opens of the `.db` or its `-wal` after startup; plus p99 latency with the
store deliberately made read-only (chmod) to prove the query path does not
depend on it.

### M4 — no allocation on the ranking path

Mechanism: request-scoped arenas mean token arrays, candidate rows and the
response buffer come from one `kbc_arena` reset per request, not from
`malloc` per candidate. `malloc` remains for state that outlives a request.

Measure: an allocation-counting build (a `malloc` interposer) reporting
`malloc` calls per search request. The claim under test is that the count is
bounded by the number of query terms, not by the number of candidates.

### M5 — what is *not* claimed

- No claim about write throughput. The ingest path is batched but the Rust
  path is batched too, and the Rust path is the one that has been measured
  in production.
- No claim about the vector lane above 20,000 rows. Rust builds an IVF-PQ
  index at that threshold (`VECTOR_INDEX_MIN_ROWS = 20_000`,
  `num_partitions = round(sqrt(rows))`); kb-c does brute-force cosine over
  the mmap'd matrix. Below the threshold Rust does not use the index either.
  kb-c is expected to win below and lose above, and the crossover point is
  exactly the thing to measure.
- No claim about ranking quality. BM25 constants match tantivy's defaults
  and RRF matches `fusion.rs` exactly, but the tokenizer, the field split and
  the FTS field set are a reimplementation, and the recall of that
  reimplementation is an open question until `bench run --k 1,5,10` exists.
  Quality is the gate that matters more than speed: a fast index that ranks
  worse is not a port.

### 4.1 The measurement plan, concretely

1. Same corpus, same queries, both implementations. The corpus and query
   list come from `bench init` (deterministic: `--n 40 --seed 0xb33f`).
2. Metrics: p50/p95/p99 latency, peak RSS, page faults, allocations, and
   Recall@k / MRR for k ∈ {1, 5, 10} in each of `keyword`, `semantic`,
   `hybrid`.
3. Runs on the same machine, alternating, five repetitions, median reported.
4. Quality regression is a failure, not a caveat: kb-c's BM25 `nDCG`/Recall@k
   must be within the run-to-run noise of the Rust daemon's on the same
   corpus before any speed number is quoted anywhere.

---

## 5. The parity contract

Each invariant is stated so that a test can fail when it is violated. These
are the conditions under which kb-c is a drop-in; a violation is a bug, not
a documented difference.

**P1 — Identity follows the path, not the content.** The artifact id is the
lowercase hex of the first 48 bits of FNV-1a over `corpus ⨯ 0x1f ⨯ path` —
12 chars. (This supersedes the plan's original `sha256(source-relative path)`
specification; mtime and size were excluded on purpose so that identity
survives an edit, see §2.4.1.) Editing a document keeps its id, its URL, its
comments and its anchors; renaming it changes all of them. Test: index, edit
the bytes, reindex, assert the id is unchanged and the content hash moved.

**P2 — `KBC_MAX_QUERY_LEN` (4096), `KBC_MAX_ARTIFACT_BYTES` (16 MiB),
`KBC_MAX_HITS` (1000) and the other `KBC_MAX_*` bounds are hard limits**,
enforced before any allocation that depends on the input. Test: each limit at
the boundary and one past it, on a request and on a file.

**P3 — Ranking constants have fixed defaults.** BM25 `k1 = 1.2`,
`b = 0.75`; RRF `k = 60`; per-arm fetch `max(limit, 200)`;
`KBC_MAX_HITS = 1000` as the ceiling. The defaults are contract; the
`[search]` table in `kb.toml` may override `bm25_k1`, `bm25_b`, `rrf_k` and
`max_hits` — a deliberate departure from the Rust build, where nothing is
settable. Test: a fixture whose expected top-20 is computed by hand from the
formula fails if any default moves.

**P4 — The tie-break chain is exactly** (1) arm-internal score DESC then id
ASC, (2) fusion score DESC then first-appearance ordinal ASC, (3) title boost
stable sort, (4) graph boost stable sort, (5) filters, (6) `take(limit)`.
Every step is implemented; step 4 **ships with `w = 0.0`**, which makes it
provably inert (a control binary built with the boost compiled out produces
byte-identical output — DECISIONS.md ADR-004). With a weight set it reads
in-degree from the `edges` table, which now converges. Test: a fixture with
deliberate exact ties at every implemented step, asserting the document order
the chain produces.

**P5 — The embedder protocol is a contract, not an implementation detail.**
Read against the real sidecar, not a draft: there is **no health op**;
readiness is an **unsolicited** `{"kind":"ready","model":…,"dim":…}` that
`kbc_embedder` must absorb wherever it arrives, including while waiting for a
reply; a `req_id` that does not match the request kills the transport; the
replies are `kind=embed_ok` and `kind=error`; an `error` envelope is *not* a
desync; transport death is retried once on respawn, an application error is
returned as-is. And because the sidecar reads stdin as UTF-8 and exits on a
line that is not, **no byte >= 0x80 may reach the wire** — text is escaped
locally to `\u00XX`. Test: a fake sidecar that replays each of those cases,
including a non-UTF-8 line, asserting the daemon's exact reaction.

**P6 — Layer order is behaviour.** For a matched `/api` request, outermost to
innermost: trace → origin allowlist → CORS → request counter → bearer auth →
(body limit → rate limit) → handler. The `/api` 404 is registered *after* the
auth layer and is therefore not auth-gated; `count_requests` and CORS are
registered after it and therefore do wrap it. Test: a 404 with no token
returns the problem+json body, not a 401.

**P7 — Loopback determination fails closed.** No peer address → not loopback.
A peer that is not itself loopback and not in `trusted_proxies` → not
loopback, and `X-Forwarded-For` is never consulted. `XFF` is walked
right-to-left, skipping loopback and trusted entries, and the first untrusted
address is the client. Test: a spoofed `XFF` from a non-trusted peer does not
grant loopback.

**P8 — Loopback is an authentication tier, not a role.** Admission order is
loopback → registry token → configured token (no fallback on mismatch) →
`KB_ALLOW_NO_AUTH=1`. Identity is attribution only, resolved
`header|token|legacy|loopback`, and it never changes what a caller may do.
Test: a valid token from a non-loopback peer is admitted and attributed.

The `X-Kb-Token` clause this paragraph used to carry — "an invalid token from a
non-loopback peer is 401 even with a valid `X-Kb-Token` registry entry for a
different user" — is **false against the original**, and the port follows the
original instead. `registry_match` (kb-server/src/middleware.rs:379-391) takes
candidates in the fixed order `[bearer, x_kb_token]` and returns the FIRST that
matches; there is no comparison between the two anywhere, and a bearer that
fails followed by an `X-Kb-Token` that matches **admits the request**
(middleware.rs:290-293). So the two carriers are not required to agree,
disagreement is not an error, and `Authorization` silently outranks
`X-Kb-Token`.

That is inherited rather than endorsed, and the consequence is written down
where the ladder is implemented: in the proxy deployment the second carrier
exists for, the proxy overwrites `Authorization` with the shared daemon token,
so a request carrying both is attributed to the operator and the per-user secret
on `X-Kb-Token` is discarded without a word. `/api/identity` now reports which
carrier decided, because a silent preference is otherwise invisible, and that
response declares `Vary: Authorization, X-Kb-Token` so a shared cache cannot hand
one caller's attribution to another. If kb-c ever gains per-user tokens, this is
the line to revisit.

**P9 — No path reaches the filesystem without the guard, and the guard is
component-wise.** Every id that becomes a path is checked (`..` rejected,
`[A-Za-z0-9._-]` only, no leading or trailing dot); every resolved file must be
**under** its root, component by component. `starts_with` in the original means
the same thing and `strncmp(path, root, strlen(root))` does not: `/root/data`
is a prefix of `/root/data-secret`. Both arguments are canonical before the
comparison (`realpath(3)` ≡ `Path::canonicalize`), so `..` and symlinks are the
filesystem's job and the component walk is the only thing the guard does. Test:
the traversal suite in stage 5 — and it must include a **sibling directory**,
because `..` cases pass on the `strncmp` implementation and a suite made only
of them would certify the bug.

**P10 — All SQL is bound, never formatted.** Corpus files, query strings,
config values and HTTP bodies are attacker-influenced. Test: a corpus file
named `a'; DROP TABLE artifacts;--.md` indexes and searches without error.

**P11 — Fail loudly, once.** Either the artifact is stored and indexed or it
is not. "Stored but the index is stale" is a defect, not a state. Test: an
injected embedder failure leaves no artifact row and no index posting.

### 5.1 Known, accepted differences

These are differences, stated rather than hidden. Each is a deliberate
narrowing recorded in `INVENTORY.md`:

- The lexical arm does not accept tantivy's own query syntax — phrases via
  `"…"`, `+`/`-` must/should-not, wildcards. It **does** accept the kb grammar
  (`AND`/`OR`/`NOT`, groups, implicit AND, `folder:`, `text:`/`q:`, and the
  `tag:`/`cap:`/`index:` facet atoms), which tantivy never had; the narrowing
  is the other way round. `since:` and `scope:` are refused with a 400.
- No `corkboard`/lists/notes/slate/session/memory/atlas/share surface. Those
  are 49.6% of the in-scope LOC and they stay in Rust. (The `corkboard`,
  `pinned_memories` and `excluded_files` TABLES exist, because a removal must
  leave the user's rows alone and the port has to be able to say so; no surface
  writes or reads them yet.)
- **The Markdown renderer is narrower than comrak, by decision.** kb-c takes
  no third-party dependency, so `src/markdown.c` is hand-rolled and does not
  support reference links and link reference definitions, footnotes,
  superscript, description lists or entity references. Measured against the real
  Rust renderer over a 4,409-document differential corpus, 8 supported-subset
  documents still render differently, and 120 of the 652 CommonMark spec
  examples — 75 of them reference-link cases this row declares not supported.
  A document relying on reference links will look wrong, and that is the
  documented cost of the dependency rule rather than a defect. (It is also not
  on the serving path yet: the artifact route serves a `.md` document's source.)
- There is no add-to-open on a frozen index, so a single changed file is
  re-indexed by rebuilding the corpus — but the *result* is then updated in
  place (`reindex_one`) and persisted through the delta journal, so a save
  costs what the file costs: **30.6 / 33.3 / 36.3 ms at 1,000 / 5,000 / 20,000
  documents**, flat across a 20× range, because all three are this host's
  fsync floor rather than a kb-c cost. There is still no batch drain: N files
  touched in one debounce window means N single-file updates rather than one
  transaction (R11).
- **A backup carries the store, not the index.** `kbc backup` tars a
  `VACUUM INTO` snapshot; the mmap'd postings file and `vectors.bin` are not in
  the archive, so a restored corpus is searchable after `kbc reindex` and not
  before. Three smaller divergences from the Rust, all with reasons, are on the
  `storage/backup.rs` row in `INVENTORY.md`.
- Within one DNF conjunct, `tag:` atoms are ANY-OF, so `tag:a AND tag:b` means
  "has a **or** has b". This is inherited from the original, which ANDs
  predicates rather than literals, and it is a genuine trap for a user. See
  §3 stage 2 for the store-level verification.
- No SQL sanitizer on capture. Staged.
- No RRF rerank stage on the request thread. The sidecar protocol is
  implemented; the parallel call is not.
- `KBC_VERSION` is a literal, not a `git describe` stamp.

---

## 6. Non-goals and rejected alternatives

### Non-goals

- Replacing `kb-code-server` or `kb-code-cli`. 240,255 LOC, tree-sitter, a
  different data model. See `INVENTORY.md`.
- Porting the ONNX runtime. `kb-embedder` stays a subprocess.
- Beating Rust on ingestion throughput.
- Being a general-purpose search engine: no language analysers, no stemming,
  no field-weighted BM25F. kb-c does ship a 91-word English stopword list
  (`KBC_STOPWORDS` in `parse.c`), which tantivy's defaults do not apply;
  everything else is left at tantivy's defaults, because matching tantivy's
  defaults is what makes kb-c a drop-in rather than a different product.
- A C++ or Rust-rewrite detour. See below.

### Rejected: rewrite only the hot path as a native addon

Keep the Rust daemon, extract the indexer and the ranker into a crate, and
call it through PyO3/`cdylib`.

Rejected because it does not remove the cost it targets. The expensive part
is not the ranking arithmetic — it is that the ranking input arrives as
Arrow columnar batches from a DataFusion plan. An addon boundary that still
consumes Arrow batches has kept every allocation the thesis is trying to
remove, and added a second language's build, FFI-safety surface and ABI
story. It also keeps tokio, keeps axum, and keeps the Lance dependency, so
the deployment story gets worse, not better. The addon approach is right
when the hot path is CPU-bound inside a computation the host language already
has the data for. Here the data movement is the cost.

### Rejected: keep Rust, add a C index

Keep the Rust daemon and have it call a C library for the inverted index.

Rejected because it splits one invariant across two languages. The parity
contract in §5 — BM25 constants, the tie-break chain, the query grammar, the
DNF cap, the fusion order — is five places where a disagreement between the
C index and the Rust fuser produces a plausible, wrong, hard-to-attribute
answer. FFI also puts the failure mode in the least debuggable place: a
segmentation fault in the index is a C backtrace through Rust frames. Porting
the whole path means one language owns the ranking end to end.

### Rejected: rewrite in C++ or Zig

**C++** — rejected for the build and the dependency story. kb-c's value is a
single static binary with no toolchain on the target; libstdc++ is a runtime
dependency unless statically linked, the ABI is not something you want across
a plugin boundary, and the language buys nothing for an mmap'd posting walk.
`std::string` allocation would also work against M4.

**Zig** — a serious candidate and the closest runner-up. Zig has a real
argument for a rewrite: `arena_allocator` as a first-class type is M4 for
free, comptime is a better `KBC_MAX_*` story than macros, and the
cross-compilation story is better. It was rejected on one specific point: Zig
has no mature, portable, C-ABI-stable embedding model for a *frozen header
contract* like `include/kbc/*.h` across the exact toolchain matrix this
project targets, and because `@cImport` of one's own headers is a weaker
guarantee than a compiler reading the same C headers. A port whose central
discipline is "the headers are frozen and C" is better served by the language
whose compiler reads those headers natively. If kb-c's C contract is ever
softened, Zig is the first alternative to re-evaluate.

### Also considered and dropped

- **Keeping SQLite as the index.** Rejected: a B-tree walk per term is the
  database round-trip the port exists to remove (M3).
- **A library dependency on a C search engine** (Xapian, Meilisearch-style).
  Rejected: the parity contract requires tantivy's exact BM25 constants and
  the fusion chain is project-specific, so any library would be wrapped in
  enough code to reimplement what it does.
- **mmap-only, no pwrite.** Rejected: the index is rebuilt, and a build has
  to write somewhere. Rebuild writes a new file and swaps.
- **Add-to-open on a live index.** Rejected during the build, not on paper:
  `index.h` freezes an index as immutable once built and offers no
  incremental add, so a single changed file cannot be cheaper than a full
  rebuild. `reindex_one` rebuilds the whole corpus instead of lying about an
  update it cannot make. The cost is real and is R11.
- **Deriving the artifact id from `(path, mtime)` or `(path, size)`.**
  Rejected during the build: an id has to survive an edit, or every save mints
  a new id and collides with the `UNIQUE(corpus, path)` row that already holds
  the slot (§2.4.1). The cost is R12.

---

## 7. Risks and mitigations

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | The BM25 reimplementation ranks differently from tantivy at the margin (tokenization, field weighting, phrase handling). | kb-c is not a drop-in; every result a user sees changes. | The highest-priority gate is Recall@k parity, not latency (§4.1 step 4). `bench run` compares both implementations on the same corpus and queries. Ship the quality gate before the speed claims. |
| R2 | The frozen headers turn out to be wrong for a case the port meets — an untagged ownership, a missing length. | Blocked work. | `AGENTS.md` rule 1: the implementing agent reports the exact signature it needs and the orchestrator changes the header. Two agents never change one header. |
| R3 | Memory safety under ASan is not the same as memory safety in a long-running daemon (the sanitizer only sees what the tests run). | Corruption after days of uptime. | ASan lane is mandatory before any commit. Bounds are checked before arithmetic, not after (§`AGENTS.md` rule 7). The index's mmap'd structures are the sharp edge: they are parsed from a file, so a truncated or corrupt index must be detected and rejected, never trusted. |
| R4 | Hand-rolling 226 routes and 7 middleware layers reproduces the behaviour but not the ergonomics; a subtle layer-order bug changes who can read what. | An auth or CORS regression. | P6 and P7 are testable invariants, tested directly. The route table is generated from kb-c's own dispatch and checked against the exported `KBC_ROUTES` in both directions, so no route is undiscoverable and no listed route lacks one — a table check, not a behaviour check. |
| R5 | The vector lane's loss of IVF-PQ makes large corpora slower. | kb-c loses to Rust above ~20k rows. | Stated in §4 M5 as a non-claim. The crossover point is a measurement, and the honest answer may be "kb-c is a keyword-first tool". |
| R6 | Scope creep: 49.6% of the in-scope LOC is untaken, and the pressure to "just add slate" or "just add sessions" is real. | The port never finishes and the delivered thing is neither a drop-in nor fast. | `INVENTORY.md` is the scope contract, with the reason on every OUT-OF-SCOPE row. Adding a subsystem means changing this document first, with a cost, not slipping into a stage. **And the cost is not the LOC column**: `doctor.rs` read as 2,930 lines of stage-5 work and turned out to be a check of the Claude Code provenance chain, which kb-c has no concept of; `markdown.rs` read as 800 lines of a renderer and hid an entire engine. A row's size is not its scope, which is the same reason R13 is a risk. |
| R7 | The two daemons (`kb`, `kb-code`) drift further apart as kb-c diverges from kb. | A workspace with two incompatible `kb`s. | Out of scope by decision and stated as such, in both documents. When kb-c becomes production-replacement, the migration story is "run the new daemon, keep the old one for the code lane" — they never shared a store. |
| R8 | The `kb-code-*` subtree is 240,255 LOC of Rust that nobody maintains after the port lands. | Silent bit-rot in a subsystem kb-c does not replace. | Explicitly not this project's problem, and said so rather than left implicit. |
| R9 | Performance work is done before correctness work, because the performance claim is the reason the port exists. | A fast, wrong index ships. | The stage order in §3 puts storage completeness, grammar and HTTP parity ahead of measurement (stage 6), and the measurement stage found two severe correctness defects in the ingest path (§2.4.5) — which is the argument for the order, made by the order itself. |
| R10 | The debounce and batching constants are tuned for the Rust daemon's latency. | kb-c either re-indexes too eagerly or too slowly. | P3-style treatment: `watcher_debounce_ms` (250 ms today, configurable) and the batch constants are in one place and listed in stage 1's gate, and are revisited once kb-c has a corpus to tune against. The reconcile pass has landed — a full reindex sweeps and removes what vanished — but it runs **on a reindex**, not on a 60-second timer, so an unobserved removal is still only noticed when something asks for a reindex. |
| R11 | A watcher event re-indexes **that one file** in place (`reindex_one`), because the frozen index has no add-to-open operation. | Ingestion cost is a function of change size rather than corpus size, which is the point, but there is still no batch drain: N files touched in a debounce window means N single-file updates rather than one transaction. | Deliberate and measured, and the measurement corrected the design twice: the in-place update alone did NOT make the cost proportional to the file, because the save rewrote and fsynced the whole index (251 ms / 500 ms / 1.19 s at 1,000 / 5,000 / 20,000 documents). A delta journal fixed that, and the matched A/B puts a save at 30.6 / 33.3 / 36.3 ms at 1,000 / 5,000 / 20,000 documents — flat across a 20× range, because all three are the storage's fsync floor (`BENCHMARKS.md`). The remaining work is stage 1's batch drain (≤32 docs / ≤8 MiB), which is now a throughput improvement rather than a correctness one. Until it lands, many small changes in one window cost more than they should — a known weakness, not a surprise. |
| R12 | The artifact id is a pure function of `(corpus, path)`, so a **rename** is a delete plus an insert, not an update. | A renamed file loses its id, its comment threads and its chunk history unless the move is observed. | **Mitigated in the store, and not yet reachable by an operator.** `kbc_store_rekey_artifact` rewrites every id-keyed row in ONE transaction, so a rename carries the id, the comments with their ids and timestamps, the corkboard entry, the pin, the first-indexed anchor and the reading history across rather than dropping them; `moves` records the intent before the rename and stamps it after, a stale id resolves through a chain-walked lookup, and a bring-up pass converges an interrupted one by warning and ABANDONING (a rekey it cannot complete would report having carried state it did not carry). The alternative the row used to weigh — minting a new id on every save — remains worse. What is missing is the last step: there is no `mv` verb and no relocate route, so the seam is reachable from the tests only. A rename the daemon does NOT observe still disappears through stage 1's reconcile pass, which keeps the user's rows and demotes inbound edges to `pending_links` |
| R13 | A design document can describe a behaviour convincingly enough that nobody tests it. | Stage 0 shipped claiming a `sha256(rel_path)` id, 280-word chunk windows and plain JSON errors — and later drafts of this plan described an embedder wire protocol the real sidecar does not speak, and quoted a 10.3 ms incremental update that was never measured clean. | The test suite is the arbiter, and it earns that position: 14 suites, 624 cases, 24 defects found during the build, including two use-after-frees and a cross-thread data race (§2.4.4). A number that no run on the current code produced does not belong in this document, even as a placeholder. Any behaviour added to §3 without a test that fails when it is wrong is not done. **The corollary, learned the hard way in §2.5: a green lane is not the same as a correct one.** An uninitialised struct flag passed a green release lane AND a green ASan lane, because no sanitizer reports uninitialised reads; it was caught only because the TSan build rendered differently from the release build. A test that fails intermittently is worse than no test — it trains everyone who reads CI to ignore red — so the one such case was deleted rather than tuned, with the declined alternative recorded in DECISIONS.md ADR-007. |
