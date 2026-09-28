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

**As of 2026-09-27 — the port is in progress. This is a pre-1.0 codebase.**

Works today:

- `kbc` builds from source with CMake and passes its ctest suite: 11 suites,
  339 cases, green in the Release and `-DKBC_SANITIZE=ON` lanes.
- Config loading (a strict `kb.toml` subset), the SQLite store and its four
  migrations (schema v4: v1 tables, v2 `edges`, v3 `pending_links`, v4
  `doc_metas`), the Markdown/HTML block parser with stable anchors, the
  tokenizer and its stopword list, the mmap'd inverted index with BM25, RRF
  fusion over the keyword and vector lanes, the mmap'd vector store, the
  inotify watcher with debounce, the subprocess embedder client, the epoll +
  `SO_REUSEPORT` HTTP daemon with bearer auth and an SSE stream, and a CLI
  with eleven verbs — all specified in the frozen headers under
  `include/kbc/` and all wired end to end.
- The daemon serves a route banner on `/` and nine JSON routes: `/api/health`,
  `/api/identity`, `/api/kbs`, `/api/stats`, `/api/search`, `/api/artifacts`,
  `/api/artifacts/{id}`, `POST /api/reindex` and `/api/events`. `reindex`,
  `search`, `get` and `list` work with or without a running daemon.
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
- A backlink boost in ranking, read from the edge graph. It ships **disabled**:
  `graph_boost` defaults to `0.0` and the weight is unmeasured. See
  `DECISIONS.md` ADR-004 for why, and ADR-006 for the concurrency rule the
  double-free fix established.
- Incremental index update: a file save re-indexes that one file in place and
  persists through a delta journal, so the save costs what the file costs rather
  than what the index weighs — 33 ms at 1,000 documents and 36 ms at 20,000,
  which is this host's fsync floor rather than a kb-c cost. The update survives a
  restart. `BENCHMARKS.md` has the measurement and the breakdown that forced the
  change.
- Errors are RFC 7807 `application/problem+json`. CORS is same-origin by
  default, with an exact allowlist in `KBC_CORS_ORIGINS` and no wildcard
  anywhere. Rate limiting is a per-connection fixed window, 120 req/s by
  default (`KBC_RATE_LIMIT_RPS`), answering 429 with `Retry-After`.
- Bearer auth actually works: `kbc_config_load_token` resolves the token from
  the literal value or the token file, so a non-loopback bind is possible. A
  `0.0.0.0` bind with a token answers 401 with no header, 403 with the wrong
  token and 200 with the right one. The token value never appears in
  `kbc config show`.

Not done:

- **No date filters.** `since:` parses its value grammar but the layer that
  would apply it does not exist, so the atom is refused with a 400.
- **The link resolution ladder is not ported.** Extraction and normalisation
  are; the four-tier id/path/title/basename resolution in the original's
  `links.rs` — and its *Ambiguous* outcome — is not, so a bare `[[name]]` is
  normalised but not yet resolved to a document id.
- **`cap:` is not capability analysis.** The original derives `svg_count`,
  `has_canvas` and `code_block_count` by inspecting the document. kb-c matches
  declared `kb-caps` metadata instead. A deliberate deviation, and the one
  place where a `cap:` query can legitimately return a different answer.
- No web UI. The Rust repo's two React/Vite SPAs, the Claude Code plugins and
  the Playwright e2e suite are out of scope for the C port.
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
           artifacts / chunks               |            |
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
        [ HTTP daemon: epoll + SO_REUSEPORT ]  ---->  /api/*  and  SSE
                  ^
                  |
        [ kbc CLI: daemon | add | search | get | list | reindex | status
                    | bench | config show | token generate | version ]
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

The harnesses are in `bench/` (`bench-kbc.sh`, `bench-rust.sh`,
`bench-rust-concurrency.sh`, `bench-scale.sh`, `bench-vector.sh`,
`kbcbench-client.c`) and the in-process one is
`kbc bench --queries N --repeat N --corpus DIR`.

## Layout

```
include/kbc/   the frozen contract — 15 headers, read these first
src/           the implementation, one file per subsystem
cli/main.c     the kbc binary
tests/         ctest-driven, one binary per test_*.c, harness in kbc_test.h
.github/       CI
```

## Contributing

Read [AGENTS.md](AGENTS.md) first — it holds the rules the headers and the
ownership model are built on. Then [CONTRIBUTING.md](CONTRIBUTING.md) for the
sign-off, the sanitizer lane and what a reviewable change looks like.
