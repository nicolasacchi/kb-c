# PORT_PLAN.md — the engineering plan for kb-c

kb-c is a C17 reimplementation of the `kb` daemon and CLI. The module-by-module
map of what is and is not being ported is `INVENTORY.md`; this document is why
the port exists, what is being built in what order, how the performance claim
is measured, and what would have to be true for kb-c to be a drop-in.

Scope in one line: 231,379 LOC of Rust across six crates, of which kb-c today
claims 74,024 (32%) and a further 44,357 (19%) is scheduled below. The
remaining 48.9% is out of scope and stays in Rust.

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
| `lance` + `arrow` + `datafusion` | Columnar store, FTS indices, IVF-PQ vector index, compaction | `src/kbc/index.c`: hand-rolled mmap'd inverted index, BM25, brute-force cosine over an mmap'd float matrix | **Replaced.** This is the point of the port. The vector lane loses IVF-PQ above 20,000 rows and gains predictable latency below it. |
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
   │  watcher.c  inotify per source root, debounce 400 ms, reconcile walk
   │             every 60 s for what inotify missed; extension gate,
   │             skip patterns, exclusions
   ▼  WatchWork{kind, path}
   │  app.c      batch drain ≤32 docs or ≤8 MiB; gate → read → identity
   │             → dedup → stat → UTF-8 check
   ├──────────────▶ parse.c   Html | Markdown arms, blocks + anchors,
   │                         title / headings / tags / capabilities
   └──────────────▶ embed.c   NDJSON to `kb-embedder`:
                             {"kind":"embed","req_id":N,"texts":[…]}
                             300 s handshake, 60 s/request, respawn-once
                             on transport death
   ▼
   write boundary — one transaction per batch (AGENTS.md rule 10)
     store.c   artifacts + chunks + comments, bound parameters
     index.c   postings, doc meta, avg doc length
     vecstore  f32 × dim matrix at row id

   ── query ──

   q ─▶ search.c   grammar (AND/OR/NOT, key:value) → DNF, ≤64 disjuncts
        ├▶ index.c   BM25 over mmap'd postings, k1=1.2, b=0.75,
        │            IDF = ln(1 + (N−n+0.5)/(n+0.5)); fields
        │            title/body/headings/code/prompt; fetch max(limit,150)
        ├▶ embed.c   query vector (cached: embed_ms, cache_hit)
        │            └▶ cosine top-k over the mmap'd matrix;
        │               score = 1/(1+distance), ties by doc id ASC
        └▶ RRF k=60, dedup on id, score = Σ 1/(60+rank)
             sort: score DESC, then first-appearance ordinal ASC
             title boost ×1.5 (substring, terms ≥3 chars)
             graph boost + w/60·√in/√in_max   [opt-in; staged]
             sort DESC, take(limit ≤ 200)
   ▼
   httpd.c  epoll accept → HTTP/1.1 → router → handler → JSON;
            bearer auth, loopback determination, problem+json errors
   cli/main.c  the same requests over a blocking socket
```

### 2.2 Memory ownership

Three tags, no fourth (`AGENTS.md` rule 3). Request-scoped work — parsed
query, token arrays, candidate rows, response JSON — allocates from one
`kbc_arena` per request, reset at the end. The index's postings, term
dictionary, doc-meta table and vector matrix are `mmap`ed from
`KBCIDX\x01` files and are read-only for the life of the process: a rebuild
writes a new file and the open one is replaced by pointer swap, never
mutated in place. The store, the config and the httpd are the only `malloc`
states.

### 2.3 Concurrency

- `httpd.c`: epoll, one `SO_REUSEPORT` listener per worker. N workers by
  default, fixed at startup. No locks on the read path — the index is
  immutable once open.
- `watcher.c`: one inotify thread per app; it owns the queue and hands
  batches to the app's indexing thread.
- `embed.c`: the sidecar's stdin/stdout are the only shared mutable state;
  one request in flight at a time, guarded by a mutex, because the protocol
  is request/response over a single pipe pair.
- `store.c`: SQLite in WAL mode, one writer. `busy_timeout = 5s`.

---

## 3. Staged migration

Stage 0 is built. Every stage after it is **PLANNED** and none of it exists in
the tree yet.

### Stage 0 — the core (BUILT)

Units: config subset, `store.c` + migrations, `parse.c`, tokenizer, `index.c`
(inverted index + BM25), `search.c` (RRF + boosts), `httpd.c` (epoll,
SO_REUSEPORT), `watcher.c` (inotify), `embed.c` (sidecar seam), `json.c`,
`mem.c`, `log.c`, `types.c`, `kbc.c`, `cli/main.c`, the 14 frozen headers.

Acceptance gate (met, per this repo's test suite): Release and
`-DKBC_SANITIZE=ON` builds clean under
`-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wstrict-prototypes
-Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Werror`; `ctest`
green in both.

Behaviour it must match, and does, per the contracts extracted from the Rust
source: the 12-hex `sha256(rel_path)[0..6]` artifact id; title fallback
`fields.title or h1 or file_stem or "untitled"`; 280-word chunk windows with
60-word overlap and chunk 0 as `title\nheadings`; BM25 at tantivy's defaults;
RRF at `k = 60`; the embedder's envelope names and `req_id` discipline.

### Stage 1 — storage completeness — PLANNED

Units:
1. `store.c`: the remaining tables kb-c claims — `sources`, `index_runs`,
   `errors`, `edges`, `excluded_files`, `doc_first_seen`, `history`,
   `corkboard`, `pinned_memories`, `identity_backfill_done`. Migration list
   versioned the way refinery's is; the epoch-ahead refusal
   (`sibling.rs:224`) already exists in stage 0 and extends.
2. `app.c`: the reconcile delete pass — files that vanished since the last
   walk emit `watch.delete` even when their mtime never changed.
3. `app.c`: quarantine. `retry_count_for_path_hash >= 3` sets
   `embed_gated`; the document still indexes and stays BM25-searchable
   because the embedding column is nullable by design. The error row is
   *not* cleared on the gated success path — clearing it would un-gate the
   document on the next pass.
4. `app.c`: `cascade.c` — delete by path, cascade to chunks, leave
   `history`, `corkboard`, pins and `reading_sections` untouched
   (invariant 8).
5. `app.c`: the eight `enrich.rs` hooks, in Rust's registration order.

Dependency order: 1 → 2 → 3 → 4 → 5. Every unit is independent of the HTTP
layer.

Acceptance gate: a corpus of N files can be indexed, one deleted from disk,
reconciled, and re-indexed with no orphan rows in `artifacts` or
`artifact_chunks`; a file that fails to embed three times lands in
`<state>/quarantine/<kb>/` and remains findable by keyword.

Behaviour it must match: the four ingest constants
(`INGEST_QUEUE_CAPACITY = 1024`, `INGEST_BATCH_MAX_DOCS = 32`,
`INGEST_BATCH_MAX_BYTES = 8 MiB`, `QUARANTINE_THRESHOLD = 3`) and the
enrich hook ordering, which is run order.

### Stage 2 — query grammar and filters — PLANNED

Units: the `query.rs` grammar, `docs_query.rs` filter evaluation, prefix
expansion (`index.c` already has `kbc_index_expand_prefix`).

Dependency order: grammar → DNF lowering → filter evaluation → prefix
expansion wired into the BM25 arm.

Acceptance gate: the port of `query.rs`'s own unit tests passes — n-ary
`AND`/`OR` flattening, De Morgan over groups, `since:Nd|Nh|<unix>`,
`since:all` as an error, the 64-conjunct cap with its exact warning string,
depth 64, and the bareword-becomes-`Key::Text` rule. Each of those is a
separate test because each is a separate user-visible behaviour.

Behaviour it must match: `MAX_DNF_CONJUNCTS = 64`,
`MAX_PARSE_DEPTH = 64`, `Text` collection in document order space-joined
across the whole expression, `NOT since:` / `NOT text:` / `NOT scope:` as
warnings rather than silent misfilters.

### Stage 3 — HTTP surface and CLI parity — PLANNED

Units: the router (the Rust route table is generated by
`crates/kb-server/tests/api_docs.rs`, which regex-scans `router.rs`;
kb-c reproduces the extractor rather than hand-transcribing), bearer auth and
the identity ladder, CORS, the origin allowlist, fixed-window rate limiting,
problem+json errors, SSE with the gap/lag probe, the CLI's HTTP client, SSE
client, and the verbs that depend on the above.

Dependency order: problem+json error mapping → `/healthz`, `/api/identity`,
`/api/kbs` (the CLI's two probes — nothing else can be tested until these
exist) → auth + identity → the route table → SSE → CLI client → CLI verbs.

Acceptance gate: the parity contract in §5, invariants P1–P9, each with a
failing test that fails when the behaviour is wrong. In particular: an
unmatched `/api/*` returns 404 `application/problem+json` with
`type = "urn:kb:errors:no-such-route"`; a non-loopback bind with no token
and no `KB_ALLOW_NO_AUTH=1` refuses at startup, after the listener is bound
and before any task is spawned; `X-Kb-Token` and `Authorization: Bearer` are
both accepted carriers.

The 226 route registrations are the scope, not 226 units. The kb-c subset is
the `PORTED` + `PLANNED` rows in `INVENTORY.md`; the rest stay in Rust
forever.

The routes `cli/main.c` depends on in stage 0, which `httpd.c` therefore has
to serve in that form: `GET /api/search` (`q`, `kb`, `mode`, `limit`; an
object whose array is `hits[]` or `results[]`), `GET /api/artifacts`
(`kb`, `limit`), `GET /api/artifacts/{id}` (`source=1`),
`POST /api/reindex` (`{}` or `{"kb":"…"}`), `GET /api/stats`. There is no
`/api/metrics` in the frozen contract; the Prometheus text endpoint is a
separate top-level surface in stage 5.

`POST /api/reindex` may answer **202** — accepted, indexing asynchronously —
in which case the CLI prints `reindex accepted` instead of a document count.
202 is a success path, not a case to fold into the 2xx-with-a-count branch.
The CLI accepts `hits`, `results`, `artifacts` or `items` and falls back to
the raw body, so the array name is not yet a parity constraint; the shape
above is the first one both sides agree on. A 401 makes the CLI print
``run `kbc token generate` ``.

### Stage 4 — capture, links, anchors — PLANNED

Units: multipart capture with Rust's exact frontmatter write order
(`kb-category`, `kb-tags`, `kb-capture-original`, `kb-capture-url`,
`kb-capture-at`, `kb-session`, `kb-expires-at` — title is never touched);
the 60-char slug policy; URL stubs as inert text (the daemon never fetches a
URL — that is the SSRF ruling, not an omission); wikilink edges; anchor
re-resolution and the `comment.anchor_stale` /
`comment.anchor_resolved` pair; `mv` and the `moves` rename-race log.

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

### Stage 6 — measurement, backup, maintenance — PLANNED

Units: `bench init/discover/run` in the CLI (the harness §4 needs), the
`VACUUM INTO` + tar backup, retention pruning, metrics export.

Acceptance gate: the benchmark suite in §4 runs end to end and writes its
report. This stage is where the performance claims in this document stop
being hypotheses.

---

## 4. The performance thesis, and how each claim is measured

**There is no `BENCHMARKS.md` in this repo and no measured number in this
document. Every figure below is a mechanism, not a result.** The claims
become results when stage 6's harness exists, and until then the correct
statement is that the mechanism is in place and unmeasured.

### M1 — mmap'd postings

Mechanism: the term dictionary, the postings array, the doc-meta table and
the vector matrix are four `mmap`ed regions with a documented layout under
the `KBCIDX\x01` magic (`kbc.h:33-34`). A keyword query resolves terms in the
dictionary (a hash-probe over a sorted term table), then walks postings as
`{doc_id:u32, tf:u16, field_mask:u16}` records and accumulates a score array.
The pages the query actually touches are faulted in; the rest of the corpus
is not in the process's resident set. There is no decompression, no
deserialisation, and no `Vec<DocSummary>` for 45 columns — the candidate row
is `{doc_id, score}` and the doc-meta lookup happens after ranking.

Measure: wall-clock p50/p95/p99 for `/api/search?mode=keyword&limit=20` over
a fixed corpus, with `perf stat` reporting `minor-faults` and
`rss`. The claim under test is that resident set stays flat as corpus size
grows 10× while p99 latency grows sub-linearly.

### M2 — bounded top-k heap instead of a full sort

Mechanism: the BM25 arm does not sort. It keeps a min-heap of size
`max(limit, 150)` keyed on score, and the pop/replace cost is `O(log k)` per
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
lowercase hex of the first 6 bytes of `sha256(source-relative path)`, 12
chars. Editing a document keeps its id, its URL, its comments and its anchors;
renaming it changes all of them. Test: index, edit the bytes, reindex, assert
the id is unchanged and the content hash moved.

**P2 — `KBC_MAX_QUERY_LEN` (4096), `KBC_MAX_ARTIFACT_BYTES` (16 MiB),
`KBC_MAX_HITS` (1000) and the other `KBC_MAX_*` bounds are hard limits**,
enforced before any allocation that depends on the input. Test: each limit at
the boundary and one past it, on a request and on a file.

**P3 — Ranking constants are fixed, not configurable.** BM25 `k1 = 1.2`,
`b = 0.75`; RRF `k = 60`; title boost factor `0.5`; per-arm fetch
`max(limit, 150)`; `SEARCH_MAX_LIMIT = 200`. Test: a fixture whose expected
top-20 is computed by hand from the formula fails if any constant moves.

**P4 — The tie-break chain is exactly** (1) arm-internal score DESC then id
ASC, (2) fusion score DESC then first-appearance ordinal ASC, (3) title boost
stable sort, (4) graph boost stable sort, (5) filters, (6) `take(limit)`.
Test: a fixture with deliberate exact ties at every step, asserting the
document order the chain produces.

**P5 — The embedder protocol is a contract, not an implementation detail.**
`{"kind":"ready","model":…,"dim":…}` is written before any request is read; a
`req_id` that does not match kills the transport; a mid-stream `ready` is an
error; an `error` envelope is *not* a desync; transport death is retried once
on respawn, an application error is returned as-is. Test: a fake sidecar that
replays each of those five, asserting the daemon's exact reaction.

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
Test: a valid token from a non-loopback peer is admitted and attributed; an
invalid token from a non-loopback peer is 401 even with a valid
`X-Kb-Token` registry entry for a different user.

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
  `"…"`, `+`/`-` must/should-not, wildcards. kb-c implements the kb grammar.
- No `corkboard`/lists/notes/slate/session/memory/atlas/share surface. Those
  are 48.9% of the in-scope LOC and they stay in Rust.
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
- Being a general-purpose search engine: no language analysers, no stemming
  beyond what the tokenizer does, no stopword list, no field-weighted BM25F.
  Match tantivy's defaults, because matching tantivy's defaults is what makes
  kb-c a drop-in rather than a different product.
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

---

## 7. Risks and mitigations

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | The BM25 reimplementation ranks differently from tantivy at the margin (tokenization, field weighting, phrase handling). | kb-c is not a drop-in; every result a user sees changes. | The highest-priority gate is Recall@k parity, not latency (§4.1 step 4). `bench run` compares both implementations on the same corpus and queries. Ship the quality gate before the speed claims. |
| R2 | The frozen headers turn out to be wrong for a case the port meets — an untagged ownership, a missing length. | Blocked work. | `AGENTS.md` rule 1: the implementing agent reports the exact signature it needs and the orchestrator changes the header. Two agents never change one header. |
| R3 | Memory safety under ASan is not the same as memory safety in a long-running daemon (the sanitizer only sees what the tests run). | Corruption after days of uptime. | ASan lane is mandatory before any commit. Bounds are checked before arithmetic, not after (§`AGENTS.md` rule 7). The index's mmap'd structures are the sharp edge: they are parsed from a file, so a truncated or corrupt index must be detected and rejected, never trusted. |
| R4 | Hand-rolling 226 routes and 7 middleware layers reproduces the behaviour but not the ergonomics; a subtle layer-order bug changes who can read what. | An auth or CORS regression. | P6 and P7 are testable invariants, tested directly. The route table is generated by the same extractor technique as the Rust one rather than transcribed. |
| R5 | The vector lane's loss of IVF-PQ makes large corpora slower. | kb-c loses to Rust above ~20k rows. | Stated in §4 M5 as a non-claim. The crossover point is a measurement, and the honest answer may be "kb-c is a keyword-first tool". |
| R6 | Scope creep: 48.9% of the in-scope LOC is untaken, and the pressure to "just add slate" or "just add sessions" is real. | The port never finishes and the delivered thing is neither a drop-in nor fast. | `INVENTORY.md` is the scope contract, with the reason on every OUT-OF-SCOPE row. Adding a subsystem means changing this document first, with a cost, not slipping into a stage. |
| R7 | The two daemons (`kb`, `kb-code`) drift further apart as kb-c diverges from kb. | A workspace with two incompatible `kb`s. | Out of scope by decision and stated as such, in both documents. When kb-c becomes production-replacement, the migration story is "run the new daemon, keep the old one for the code lane" — they never shared a store. |
| R8 | The `kb-code-*` subtree is 240,255 LOC of Rust that nobody maintains after the port lands. | Silent bit-rot in a subsystem kb-c does not replace. | Explicitly not this project's problem, and said so rather than left implicit. |
| R9 | Performance work is done before correctness work, because the performance claim is the reason the port exists. | A fast, wrong index ships. | The stage order in §3 puts storage completeness, grammar and HTTP parity ahead of measurement (stage 6). §4 opens by saying there are no measured numbers. |
| R10 | The 400 ms debounce, 60 s reconcile and batching constants are tuned for the Rust daemon's latency. | kb-c either re-indexes too eagerly or too slowly. | P3-style treatment: these are constants in one place, listed in stage 1's gate, and revisited once kb-c has a corpus to tune against. |
