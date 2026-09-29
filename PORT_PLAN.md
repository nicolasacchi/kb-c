# PORT_PLAN.md — the engineering plan for kb-c

kb-c is a C17 reimplementation of the `kb` daemon and CLI. The module-by-module
map of what is and is not being ported is `INVENTORY.md`; this document is why
the port exists, what is being built in what order, how the performance claim
is measured, and what would have to be true for kb-c to be a drop-in.

Scope in one line: 231,507 LOC of Rust across six crates, of which kb-c today
claims 84,712 (36.6%) and a further 34,784 (15.0%) is scheduled below. The
remaining 48.4% is out of scope and stays in Rust. Those three figures are the
PORTED+PARTIAL, PLANNED and OUT-OF-SCOPE row sums in `INVENTORY.md`,
recomputed from the rows on 2026-09-27.

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
   │             No periodic reconcile walk yet (stage 1).
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
   │                         heading levels, title fallback
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
        ├▶ embed.c   query vector (no cache yet — stage 3)
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
            ring with Last-Event-ID
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
   document would not have.** The test suite is 11 ctest binaries with 339
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

---

## 3. Staged migration

Stage 0 is built. Stage 2 is **mostly** built: the query grammar and prefix
expansion landed, in the shape the index can answer, the `docs_query.rs` filter
overlay is in (`tag:`, `cap:`/`caps:`, `index:`; `scope:` and `since:` stay
refused), and link extraction — including wikilinks — is in. Stage 3 is
**mostly** built: `problem+json`, `/api/identity`, `/api/kbs`, CORS and rate
limiting are in, so what remains there is the route table, the identity ladder,
the SSE gap probe and the query-embedding cache. Stages 4, 5 and 6 are
**PLANNED** apart from stage 6's measurement half, and nothing else in them is
in the tree. The stages that are partly in already are therefore *gaps in what
exists*, not greenfield.

### Stage 0 — the core (BUILT)

Units, as they are in the tree: `config.c`, `store.c` + four migrations,
`parse.c` + the tokenizer and its stopword table, `ids.c`, `index.c`
(inverted index + BM25), `search.c` (RRF, the query grammar and the filter
overlay), `embed.c` (sidecar client) and `kbc_vecstore`, `httpd.c` (epoll,
SO_REUSEPORT, 9 JSON routes plus the `/` banner, bearer, SSE), `watcher.c`
(inotify), `json.c`, `mem.c`, `log.c`, `kbc.c`, `cli/main.c`, the frozen
headers.

20,838 lines of C in `src/`, 1,585 of frozen header, 2,516 in the CLI.

Acceptance gate (met): Release and `-DKBC_SANITIZE=ON` builds clean under
`-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wstrict-prototypes
-Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Werror`; `ctest` green
in both — 11 suites, 339 case functions.

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
- chunking is **one chunk per parsed block**, not a 280-word window with
  60-word overlap, and chunk 0 is not `title\nheadings`;
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

### Stage 1 — storage completeness — MOSTLY PLANNED

Two units' substance have landed since this was written, and they are the ones
the acceptance gate cares about most: **the incremental in-place index update**
and, after it, **the delta journal that makes it cheap**.
`app.c` re-indexes a single file rather than rebuilding the corpus
(`reindex_one`), with the store row committed before the index so a reader
never sees an index naming an uncommitted document. That work also produced
the first two defects in §2.4.5 and their fixes.

The in-place update alone did not deliver the property this stage is for. A
single-file save still rewrote the whole index and fsynced it, so its cost
tracked the INDEX: 251 ms / 500 ms / 1.19 s at 1,000 / 5,000 / 20,000
documents. Measurement then showed the barrier was 64.6% of that and the
serializer only 13.1%, so a cheaper serializer could recover at most the 13% it
was responsible for. The fix was a delta journal — a single-document update
appends a record and fsyncs that, and `kbc_index_open` replays it — which puts
the save at **33 ms at 1,000 documents and 36 ms at 20,000**, this host's
fsync floor. The cost no longer scales with the corpus. `BENCHMARKS.md` has the
breakdown and the before/after.

Unit 1 has since landed **in part**: the schema is now at **v4**, adding
`edges` (v2), `pending_links` (v3) and `doc_metas` (v4) to the v1 tables. Those
three are the link graph and the query overlay, and they were the reason the
edge table exists at all. The rest of unit 1 — `sources`, `index_runs`,
`errors`, `excluded_files`, `doc_first_seen`, `history`, `corkboard`,
`pinned_memories`, `identity_backfill_done` — and units 2–6 are untouched: no
reconcile pass, no quarantine, no enrich hooks, no `chunk.c`.

Units:
1. `store.c`: the remaining tables kb-c claims — `sources`, `index_runs`,
   `errors`, `excluded_files`, `doc_first_seen`, `history`, `corkboard`,
   `pinned_memories`, `identity_backfill_done`. `edges`, `pending_links` and
   `doc_metas` are already there (v2–v4). Migration list versioned the way
   refinery's is, one transaction per version. **The epoch-ahead refusal
   (`sibling.rs:224`) is not in stage 0** — the store records
   `kbc_store_schema_version` and nothing more, so the refusal is built here
   from scratch.
2. `app.c`: the reconcile delete pass — files that vanished since the last
   walk emit `watch.delete` even when their mtime never changed.
3. `app.c`: quarantine. `retry_count_for_path_hash >= 3` sets
   `embed_gated`; the document still indexes and stays BM25-searchable
   because the embedding column is nullable by design. The error row is
   *not* cleared on the gated success path — clearing it would un-gate the
   document on the next pass.
4. `app.c`: delete-by-path, cascading to chunks and comments, leaving
   `history`, `corkboard`, pins and `reading_sections` untouched
   (invariant 8). `kbc_store_delete_artifact` already does the artifacts →
   chunks → comments half of this; what is missing is the by-path entry point
   and the explicit "leave these alone" list.
5. `app.c`: the eight `enrich.rs` hooks, in Rust's registration order.
6. `app.c`: `chunk.c` — the 280-word window with 60-word overlap and chunk 0
   as `title\nheadings`, replacing today's one-chunk-per-parsed-block. It
   belongs here because it is a storage-shaped change to the `chunks` table.

Dependency order: 1 → 2 → 3 → 4 → 5 → 6. Every unit is independent of the HTTP
layer.

Acceptance gate: a corpus of N files can be indexed, one deleted from disk,
reconciled, and re-indexed with no orphan rows in `artifacts` or
`artifact_chunks`; a file that fails to embed three times lands in
`<state>/quarantine/<kb>/` and remains findable by keyword.

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

Not done: the four-tier id/path/title/basename **resolution ladder** in
`links.rs` `resolve` / `ResolveIndex`, including its `Ambiguous` outcome.

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

Still to do: the router that reproduces the Rust route table (generated by
`crates/kb-server/tests/api_docs.rs`, which regex-scans `router.rs`; kb-c
reproduces the extractor rather than hand-transcribing), the multi-tier
identity ladder and `X-Kb-Token` as a second carrier beside
`Authorization: Bearer`, the SSE gap/lag probe, the query-embedding cache, and
the verbs that depend on the above.

The 226 route registrations are the scope, not 226 units. The kb-c subset is
the `PORTED` + `PLANNED` rows in `INVENTORY.md`; the rest stay in Rust
forever.

The routes `cli/main.c` depends on, which `httpd.c` serves today, verified
against the running binary and exported in `KBC_ROUTES`: `GET /api/health`,
`GET /api/identity`, `GET /api/kbs`, `GET /api/stats`, `GET /api/search`
(`q`, `kb`, `kind`, `mode`, `limit`, `offset`; an object whose array is
`hits[]` or `results[]`), `GET /api/artifacts` (`kb`, `kind`, `limit`,
`offset`), `GET /api/artifacts/{id}` (`source=1`), `POST /api/reindex`
(`{}` or `{"kb":"…"}`), and `GET /api/events` (SSE). `GET /` returns a
plain-text route banner. There is no `/api/metrics` in the frozen contract;
the Prometheus text endpoint is a separate top-level surface in stage 5.

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

### Stage 4 — capture, links, anchors — PARTLY BUILT

Done since this section was written: **link extraction, including
wikilinks**, and the edge graph behind it. `[text](target)`, `<a href>`,
`[[target]]` and `[[target|alias]]` all go through the same normaliser; the
graph converges via `pending_links` (see §3 stage 2).

Units that remain: multipart capture with Rust's exact frontmatter write order
(`kb-category`, `kb-tags`, `kb-capture-original`, `kb-capture-url`,
`kb-capture-at`, `kb-session`, `kb-expires-at` — title is never touched);
the 60-char slug policy; URL stubs as inert text (the daemon never fetches a
URL — that is the SSRF ruling, not an omission); the `links.rs`
id/path/title/basename **resolution ladder** and its `Ambiguous` outcome; the
`/api/links` routes and `links suggest/apply`; anchor re-resolution and the
`comment.anchor_stale` / `comment.anchor_resolved` pair; `mv` and the `moves`
rename-race log.

Acceptance gate: capture the same bytes through both implementations and
diff the resulting file, tag list and id.

### Stage 5 — serving, SPA, metrics, outbound — PLANNED

Units: `/docs` listing, artifact serving with `Content-Type: text/html`,
`X-Content-Type-Options: nosniff`, `Content-Security-Policy: sandbox` and
`X-Kb-Artifact-Id` always set; the artifact subdomain with the
`resolved.starts_with(source_root)` traversal guard; the SPA static handler
with the same guard; the Prometheus text endpoint; backup/restore; the
Markdown renderer; the remaining CLI verbs.

Acceptance gate: the traversal suite — `..` in a segment, an absolute path, a
symlink out of the source root, a percent-encoded `..` — each returns 404 or
400 and never a byte outside the root.

### Stage 6 — measurement, backup, maintenance — PARTLY BUILT

Done: the measurement half. `BENCHMARKS.md` exists and is written from runs on
this host, and it covers the head-to-head latency comparison, a matched-corpus
concurrency A/B against Rust, a kb-c concurrency ladder measured with a load
client (`bench/kbcbench-client.c`) that is cheap enough not to be the
bottleneck, the vector lane against the production sidecar, and a scale ladder
that stops at 20,000 documents and **locates no knee**. `bench/` holds the
harnesses (`bench-kbc.sh`, `bench-rust.sh`, `bench-rust-concurrency.sh`,
`bench-scale.sh`, `bench-vector.sh`) and `kbc bench` is the in-process one.

Not done: `bench init`/`discover` as CLI subcommands, the `VACUUM INTO` + tar
backup, retention pruning, metrics export.

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

**P9 — No path reaches the filesystem without the guard.** Every id that
becomes a path is checked (`..` rejected, `[A-Za-z0-9._-]` only, no leading or
trailing dot); every resolved file must start with its root. Test: the
traversal suite in stage 5.

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
  are 48.4% of the in-scope LOC and they stay in Rust.
- Chunking is one row per parsed block, not the 280-word window with 60-word
  overlap. The rows are stored and listed correctly; the windowing is stage 1.
- There is no add-to-open on a frozen index, so a single changed file is
  re-indexed by rebuilding the corpus — but the *result* is then updated in
  place (`reindex_one`) and persisted through the delta journal, so a save
  costs what the file costs: 33 ms at 1,000 documents and 36 ms at 20,000,
  flat, because both numbers are this host's fsync floor rather than a kb-c
  cost. There is still no batch drain: N files touched in one debounce window
  means N single-file updates rather than one transaction (R11).
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
| R4 | Hand-rolling 226 routes and 7 middleware layers reproduces the behaviour but not the ergonomics; a subtle layer-order bug changes who can read what. | An auth or CORS regression. | P6 and P7 are testable invariants, tested directly. The route table is generated by the same extractor technique as the Rust one rather than transcribed. |
| R5 | The vector lane's loss of IVF-PQ makes large corpora slower. | kb-c loses to Rust above ~20k rows. | Stated in §4 M5 as a non-claim. The crossover point is a measurement, and the honest answer may be "kb-c is a keyword-first tool". |
| R6 | Scope creep: 48.4% of the in-scope LOC is untaken, and the pressure to "just add slate" or "just add sessions" is real. | The port never finishes and the delivered thing is neither a drop-in nor fast. | `INVENTORY.md` is the scope contract, with the reason on every OUT-OF-SCOPE row. Adding a subsystem means changing this document first, with a cost, not slipping into a stage. |
| R7 | The two daemons (`kb`, `kb-code`) drift further apart as kb-c diverges from kb. | A workspace with two incompatible `kb`s. | Out of scope by decision and stated as such, in both documents. When kb-c becomes production-replacement, the migration story is "run the new daemon, keep the old one for the code lane" — they never shared a store. |
| R8 | The `kb-code-*` subtree is 240,255 LOC of Rust that nobody maintains after the port lands. | Silent bit-rot in a subsystem kb-c does not replace. | Explicitly not this project's problem, and said so rather than left implicit. |
| R9 | Performance work is done before correctness work, because the performance claim is the reason the port exists. | A fast, wrong index ships. | The stage order in §3 puts storage completeness, grammar and HTTP parity ahead of measurement (stage 6), and the measurement stage found two severe correctness defects in the ingest path (§2.4.5) — which is the argument for the order, made by the order itself. |
| R10 | The debounce and batching constants are tuned for the Rust daemon's latency. | kb-c either re-indexes too eagerly or too slowly. | P3-style treatment: `watcher_debounce_ms` (250 ms today, configurable) and the batch constants are in one place and listed in stage 1's gate, and are revisited once kb-c has a corpus to tune against. There is no 60 s reconcile in the tree yet — stage 1's unit 2 adds it, and the constant arrives with it. |
| R11 | A watcher event re-indexes **that one file** in place (`reindex_one`), because the frozen index has no add-to-open operation. | Ingestion cost is a function of change size rather than corpus size, which is the point, but there is still no batch drain: N files touched in a debounce window means N single-file updates rather than one transaction. | Deliberate and measured, and the measurement corrected the design twice: the in-place update alone did NOT make the cost proportional to the file, because the save rewrote and fsynced the whole index (251 ms / 500 ms / 1.19 s at 1,000 / 5,000 / 20,000 documents). A delta journal fixed that, and a save is now 33 ms at 1,000 documents and 36 ms at 20,000 — flat, because both are the storage's fsync floor. The remaining work is stage 1's batch drain (≤32 docs / ≤8 MiB), which is now a throughput improvement rather than a correctness one. The remaining work is stage 1's batch drain (≤32 docs / ≤8 MiB). Until it lands, many small changes in one window cost more than they should — a known weakness, not a surprise. |
| R12 | The artifact id is a pure function of `(corpus, path)`, so a **rename** is a delete plus an insert, not an update. | A renamed file loses its id, its comment threads and its chunk history unless the move is observed. | Deliberate: the alternative mints a new id on every save, which is worse. The mitigation is stage 4's `mv` and the `moves` rename-race log, plus stage 1's reconcile delete pass, which is what makes an unobserved rename disappear rather than linger. Until both land, a rename orphans a row. |
| R13 | A design document can describe a behaviour convincingly enough that nobody tests it. | Stage 0 shipped claiming a `sha256(rel_path)` id, 280-word chunk windows and plain JSON errors — and later drafts of this plan described an embedder wire protocol the real sidecar does not speak, and quoted a 10.3 ms incremental update that was never measured clean. | The test suite is the arbiter, and it earns that position: 11 suites, 339 cases, 24 defects found during the build, including two use-after-frees and a cross-thread data race (§2.4.4). A number that no run on the current code produced does not belong in this document, even as a placeholder. Any behaviour added to §3 without a test that fails when it is wrong is not done. |
