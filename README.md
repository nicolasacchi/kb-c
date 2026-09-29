# kb-c

kb-c is a C17 rewrite of the [kb](https://github.com/nicolasacchi/kb) daemon — the
self-hosted system of record for everything an AI agent writes. Its query path
is a memory-mapped inverted index, so a search touches no database and copies
no bytes it does not need, and it links four libraries instead of a runtime
stack. It is also **~170x cheaper in CPU per query** than the Rust daemon it
replaces, measured on a matched corpus: [Performance](#performance).

The relationship to the Rust original: same product, same corpus model, new
engine. A kb-c daemon and the Rust daemon use different default ports (4317 vs
4000) and different data directories, so they can run side by side against the
same corpus.

## Status

**As of 2026-09-29 — the port is in progress. This is a pre-1.0 codebase.**

Works today:

- `kbc` builds from source with CMake and passes its ctest suite: 14 suites,
  624 cases, green in the Release, `-DKBC_SANITIZE=ON` and TSan lanes (14/14 in
  each, from clean trees, at commit `c05a564`).
- Config loading (a strict `kb.toml` subset), the SQLite store and its
  **twelve** migrations — fifteen data tables at schema v12, plus the
  `schema_version` bookkeeping table — the Markdown/HTML block parser with
  stable anchors and `kb-*` front matter, the tokenizer and its stopword list,
  the mmap'd inverted index with BM25, RRF fusion over the keyword and vector
  lanes, the mmap'd vector store, the inotify watcher with debounce, the
  subprocess embedder client, the epoll + `SO_REUSEPORT` HTTP daemon with bearer
  auth and an SSE stream, and a CLI with sixteen verbs — all specified in the
  frozen headers under `include/kbc/` and all wired end to end.
- A storage volume that has been forward-migrated by a **newer** binary is
  refused at open, before the migration run touches it, so a kb-c that cannot
  read a volume never writes to it.
- A document is chunked as a **280-word sliding window with 60-word overlap**,
  chunk 0 being the title passage, rather than one row per parsed block — so a
  single long paragraph is no longer one oversized chunk the embedder truncates
  away. A document that fails to embed three times is **quarantined**: it stops
  being embedded and stays keyword-searchable, and editing it — which changes
  its content hash — is how an operator gets it out again.
- The daemon serves a route banner on `/` and **fourteen** JSON routes:
  `/api/health`, `/api/identity`, `/api/kbs`, `/api/stats`, `/api/search`,
  `/api/artifacts`, `/api/artifacts/{id}`, `/api/kb/{kb}/artifact/{id}`,
  `/api/kb/{kb}/notes/{id}/links`, `/api/kb/{kb}/backlinks/{id}`,
  `/api/kb/{kb}/wikilinks/suggest`, `POST /api/kb/{kb}/capture`,
  `POST /api/reindex` and `/api/events`, plus a Prometheus text endpoint at
  `/metrics`. `reindex`, `search`, `get` and `list` work with or without a
  running daemon.
- Artifact **serving**, on two origins: the page on the app origin with
  `Content-Security-Policy: sandbox`, `nosniff` and `X-Kb-Artifact-Id`, and one
  subdomain per artifact (`<id>.artifacts.localhost`) with a `frame-ancestors`
  CSP instead. **A `.md` is rendered to a page on both** rather than served as
  source, and `?download=1` is the deliberate exception that hands back the
  source. Both are behind a **component-wise** root guard — `strncmp`
  containment would serve a sibling directory, and the test that catches that
  is a sibling directory, not a `../` case. A static bundle in `KB_SPA_DIST` is
  served on the same guard.
- A hand-rolled Markdown renderer with **no third-party dependency** (see the
  caveat below), a query-embedding cache keyed by `(model, query)`, and
  `kbc backup` / `kbc restore` (`VACUUM INTO` + tar).
- **Capture**: `POST /api/kb/{kb}/capture` takes a multipart upload and answers
  201 with the created ids. The stamped frontmatter is **byte-comparable** with
  the original's, which is why the key order is a contract — but the order is
  the order of *first insertion*, so a document that already carries
  `kb-category` keeps it first and grows the rest after it. A shared `url` is
  **recorded and never fetched** (the SSRF ruling; a test binds a real listener
  and asserts nothing connects), and the write is checked against the corpus
  root with the same component-wise guard the serving surfaces use.
- **Anchor comments go stale, and say so.** After a re-index the daemon
  re-resolves every open comment's anchor and publishes
  `comment.anchor_stale` or `comment.anchor_resolved` — **on the transition
  only**, so the event volume tracks state changes rather than reindexes.
  `kbc comments list|add|resolve|unresolve` read and write the store directly.
- **A move carries the document's state with it.** A rename rewrites every
  id-keyed row in ONE store transaction — comments keep their ids and
  timestamps, and the corkboard entry, the pin, the first-indexed anchor and
  the reading history follow — records its intent before the rename and stamps
  it after, and a stale id resolves to its new home. A rename interrupted by a
  crash is converged at the next bring-up, which **warns and abandons** rather
  than re-running the rekey, because a rekey it cannot complete would report
  having carried state it did not carry. The move has no verb or route yet:
  the seam is the app and store API, reachable from the tests only.
- **Retention and metrics as operator verbs.** `kbc prune --days N [--apply]`
  removes reading-history rows older than the window and **never a document**,
  because the original never removes one by age — `edges` has no timestamp
  column to prune on. It is a dry run without `--apply`. `kbc metrics [--out
  PATH]` prints the daemon's Prometheus exposition; it has no in-process
  fallback, because a fallback would print a healthy-looking exposition of
  zeroes.
- A vector lane works when `kb-embedder` is configured and healthy, verified
  against the production sidecar with `bge-small-en-v1.5`. Without one,
  `hybrid` degrades to keyword and `semantic` returns nothing, and the
  response says `degraded: true`.
- A query grammar: `AND`/`OR`/`NOT`, groups, implicit AND, and `folder:` as a
  path-prefix facet. There is no phrase search, and there is none to port: a
  quoted `"…"` in the original quotes an atom *value*, not a phrase.
- A filter overlay over the results. `tag:`, `cap:`/`caps:` and `index:` read
  the document's own declared metadata (`<meta name="kb-tags">` and Markdown
  front matter) and are evaluated once per query; a filter matching nothing
  returns zero rows, never everything. Value comparison is case-sensitive, as
  in the original. `since:` and `scope:` are still refused with a 400 rather
  than searched as literal terms.
- Link extraction, including wikilinks: `[text](target)`, `<a href>`,
  `[[target]]` and `[[target|alias]]` all normalise to the same target, and
  `![[embed]]` is an image. The link graph **converges**: a document ingested
  before the documents it links to still ends up with the right in-degree, and
  the property is asserted by a test that ingests the same corpus in both
  orders and requires an identical edge set.
- Link **resolution** the way the original's does: a target goes up a
  four-tier ladder — id, path, title, basename — and when a title or a basename
  matches several documents the answer is *ambiguous*, recorded as no edge
  rather than an arbitrary one (picking by index order would be nondeterministic
  and would flip on the next reindex). A target that matches nothing yet stays a
  pending link and is drained when the document arrives, which is why the
  in-degrees above do not depend on ingest order.
- A backlink boost in ranking, read from the edge graph. It ships **disabled**:
  `graph_boost` defaults to `0.0` and the weight is unmeasured. See
  `DECISIONS.md` ADR-004 for why, and ADR-006 for the concurrency rule the
  double-free fix established.
- Incremental index update: a file save re-indexes that one file in place and
  persists through a delta journal, so the save costs what the file costs rather
  than what the index weighs — **30.6 / 33.3 / 36.3 ms at 1,000 / 5,000 / 20,000
  documents**, flat across a 20× range, which is this host's fsync floor rather
  than a kb-c cost. The update survives a restart. `BENCHMARKS.md` has the
  measurement and the breakdown that forced the change.
- Errors are RFC 7807 `application/problem+json`. CORS is same-origin by
  default, with an exact allowlist in `KBC_CORS_ORIGINS` and no wildcard
  anywhere. Rate limiting is a per-connection fixed window, 120 req/s by
  default (`KBC_RATE_LIMIT_RPS`), answering 429 with `Retry-After`.
- Bearer auth actually works: `kbc_config_load_token` resolves the token from
  the literal value or the token file, so a non-loopback bind is possible. A
  `0.0.0.0` bind with a token answers 401 with no header, 403 with the wrong
  token and 200 with the right one. The token value never appears in
  `kbc config show`. `X-Kb-Token` works as a second carrier beside
  `Authorization: Bearer`; the two do not have to agree, and disagreement is
  not an error, because the original takes the first candidate that matches and
  never compares. `/api/identity` reports which carrier decided.

Not done:

- **No date filters.** `since:` parses its value grammar but the layer that
  would apply it does not exist, so the atom is refused with a 400.
- **The Markdown renderer is deliberately narrower than the original's comrak.**
  It is the artifact route's production path — a `.md` is served as a rendered
  page on both origins — and it does not support reference links and link
  reference definitions, footnotes, superscript, description lists and entity
  references, which render as ordinary text. That is the cost of taking no
  third-party dependency, and it is recorded rather than silent.
- **The move is not user-facing yet.** The store and app halves are built and
  tested, and a crash-interrupted rename is converged at bring-up, but there is
  no `mv` verb and no relocate route: today only the tests can call it.
- **`links suggest` / `links apply` are not ported.** They are pure functions
  of `kb_core::mentions`, which is out of scope for this port, so there is
  nothing to port them from. The three routes that read the link graph —
  note links, backlinks and `[[` autocomplete — are in.
- **The anchor corkboard has no routes.** Stale/resolved detection and its two
  events are in; the cross-kb list, pin, unpin and `GET /anchors/stale` are not,
  so there is no HTTP surface for an SPA's stale-anchors dashboard to seed from.
- **Capture is Markdown-only.** `.html` is refused with a 415, because the
  original's HTML path splices `<meta>` into `<head>` and runs ammonia over the
  result, and kb-c takes no sanitiser — a head-splice without one is a way to
  persist attacker markup into a trusted origin. The Web Share Target
  (`POST /capture`) and the desk surface are not ported either.
- **A backup carries the store, not the index.** `kbc restore` gives back a
  working SQLite snapshot; the mmap'd index and `vectors.bin` are not in the
  tarball, so a restored corpus becomes searchable after `kbc reindex` and not
  before.
- **`cap:` is not capability analysis.** The original derives `svg_count`,
  `has_canvas` and `code_block_count` by inspecting the document. kb-c matches
  declared `kb-caps` metadata instead. A deliberate deviation, and the one
  place where a `cap:` query can legitimately return a different answer.
- No web UI of its own. The Rust repo's two React/Vite SPAs, the Claude Code
  plugins and the Playwright e2e suite are out of scope for the C port — though
  the daemon now has the three serving surfaces a UI needs (artifact bytes, the
  artifact subdomain, a `KB_SPA_DIST` static fallback).
- The scale ladder stops at 20,000 documents and **locates no knee**. Both
  candidate knees dissolved on measurement, and the 20,000 rung re-ingested an
  hour later into a different sandbox produced a byte-identical index. A
  100,000-document attempt is recorded in `BENCHMARKS.md` as an attempt, not an
  ingest figure: the corpus generated and the store phase completed, but the
  index build blocked on shared storage, so its wall clock measures the disk
  and not kb-c.
- Large parts of the Rust surface are deliberately not ported. `INVENTORY.md`
  maps every crate and says what happened to it; `PORT_PLAN.md` is the phased
  plan. Read both before assuming a feature exists here.

## Quickstart

```bash
git clone <this repo> && cd kb-c
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Make a corpus of two Markdown files:

```bash
mkdir -p corpus
cat > corpus/hello.md <<'EOF'
# Hello

kb-c indexes folders of Markdown and HTML and serves them over HTTP.
EOF
cat > corpus/design.md <<'EOF'
# Design

The keyword lane is a memory-mapped inverted index scored with BM25. The vector
lane runs out of process. The two are fused with reciprocal rank fusion.
EOF
```

Write `kb.toml` next to the corpus:

```toml
[daemon]
bind = "127.0.0.1"
port = 4317          # 4000 belongs to the Rust daemon; the two can coexist
data_dir = "./data"

[[corpus]]
name = "docs"
path = "./corpus"
```

`examples/kb.toml` is the annotated version. Keys live under a table header —
a bare `data_dir` on line 1 is a parse error, as is an unknown key.

Paths in a config are relative to **the config file's directory**, not to the
directory you run `kbc` from — the same rule `[[corpus]] path` uses. Setting
`data_dir` alone is enough to relocate the daemon: the database, the index and
the token file then resolve to `<data_dir>/kb.db`, `<data_dir>/index` and
`<data_dir>/token`, so `kbc token generate` and the daemon always agree on
which file the token lives in. `db_path`, `index_path` and `token_file` each
win on their own if you set them; the ones you omit still follow `data_dir`.
`kbc config show` prints the resolved values, so you never have to guess where
the daemon will look.

Index it, then search it. Global flags come **before** the verb, so this is
`kbc --config kb.toml reindex`, not `kbc reindex --config kb.toml`:

```bash
./build/kbc --config kb.toml reindex
./build/kbc --config kb.toml search "inverted index"
```

Both work with or without a running daemon: with no daemon answering they say
so on stderr (`reindex: no daemon at http://127.0.0.1:4317; running locally`)
and do the work in-process. `kbc status` has no such fallback and exits 2.

Run the daemon and talk to it over HTTP:

```bash
./build/kbc --config kb.toml daemon &
curl -s localhost:4317/api/health
curl -s 'localhost:4317/api/search?q=inverted+index&limit=5'
```

A non-loopback bind without a token is refused at startup; a token, when set,
is required on every `/api` route except `/api/health`.

## Architecture

```
        corpus dirs (.md/.html)
                  |
        [ watcher: inotify ]        debounce, hash, skip unchanged
                  |
                  v
      [ parser ] -> [ tokenizer ] -> [ inverted index (mmap, BM25) ]
                  |                         ^            ^
                  v                         |            |
            [ SQLite store ]                |            |
          artifacts / chunks / moves        |            |
                  ^                         |            |
                  |                    query tokens   query vector
                  |                         |            |
                  |                         v            v
                  |                 [ keyword lane ]  [ vector lane ]
                  |                         \            /
                  |                          \  RRF     /
                  |                    [ searcher ]  ---> [ embedder sidecar ]
                  |                         |              (subprocess, NDJSON)
                  v                         v
        [ HTTP daemon: epoll + SO_REUSEPORT ]  ---->  /api/*, /metrics, SSE
                  ^                                   and the artifact /
                  |                                   subdomain / static
                  |                                   serving surfaces
        [ kbc CLI: daemon | add | search | get | list | reindex | status
                    | comments | bench | backup | restore | prune | metrics
                    | config show | token generate | version ]
```

Request-scoped memory lives in an arena and dies with the request. The store,
the index, the config and the httpd are heap-allocated and outlive it. The
index and the store are swapped together under one write lock, so a reader can
never see an index that references documents the store has not committed.

## Build and test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# the memory-safety lane; this one must be green before any commit
cmake -B build-asan -DKBC_SANITIZE=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Options: `KBC_SANITIZE` (ASan + UBSan, no `-O3`), `KBC_WERROR` (on by default),
`KBC_NATIVE` (`-march=native`), `KBC_BUILD_TESTS` (on by default).
Warnings are errors: `-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual
-Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2`.

## Dependencies

libc, libsqlite3, pthreads, libm. Nothing else. No inference runtime is linked
into the daemon — embeddings come from a sidecar process over a line-delimited
JSON protocol, the same seam the Rust build uses.

## Performance

The measured numbers live in [`BENCHMARKS.md`](BENCHMARKS.md), written from
runs on this machine and nowhere else. The headline: **~170x less CPU per
query** than the Rust daemon on a matched 360-document corpus (kb-c ~0.21 ms
of CPU per query, Rust ~35 ms, both flat across 1/2/4/8 clients), and
**299,948 kB resident** with a real embedding model loaded against the Rust
daemon's 321,580 kB. Both comparisons state their own imperfection next to the
number rather than in a footnote — the concurrency corpus is small and
deliberately matched, and the latency comparison's corpora are *not* matched
(1,114 documents against 1,594).

On concurrency the claim is narrower and more specific: the rps figures are
the **server's**, because the load client is ~17x cheaper than the daemon it
measures — 0.160 client cores against 2.824 at 32 concurrent clients. The knee
is at 16, and past 32 the daemon's CPU *falls* while latency rises, which means
blocked rather than searching. The earlier figures in that section were taken
with a python client whose GIL saturated first; they are kept and labelled,
because the same workload one GIL apart is the interesting comparison.

On **writes** the headline is the save path, and it is the number that moved
most in the ingest work. A single-file save used to track the size of the index
— 251 ms / 500 ms / 1.19 s at 1,000 / 5,000 / 20,000 documents — because it
rewrote and fsynced the whole index. It now appends to a delta journal and
fsyncs that: **30.6 / 33.3 / 36.3 ms at the same three sizes**, flat across a
20× range of corpus. What is left is the storage's fsync floor, not kb-c: the
journal is ~1.9 KB after ten appends and still costs 36.3 ms, and fsync on this
device costs 33.9 ms for 90 bytes.

The harnesses are in `bench/` (`bench-kbc.sh`, `bench-rust.sh`,
`bench-rust-concurrency.sh`, `bench-scale.sh`, `bench-vector.sh`,
`kbcbench-client.c`) and the in-process one is
`kbc bench --queries N --repeat N --corpus DIR`.

## Layout

```
include/kbc/   the frozen contract — 18 headers, read these first
src/           the implementation, one file per subsystem
cli/main.c     the kbc binary
tests/         ctest-driven, one binary per test_*.c, harness in kbc_test.h
.github/       CI
```

## Contributing

Read [AGENTS.md](AGENTS.md) first — it holds the rules the headers and the
ownership model are built on. Then [CONTRIBUTING.md](CONTRIBUTING.md) for the
sign-off, the sanitizer lane and what a reviewable change looks like.
