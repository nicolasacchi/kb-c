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

**As of 2026-09-26 — the port is in progress. This is a pre-1.0 codebase.**

Works today:

- `kbc` builds from source with CMake and passes its ctest suite: 11 suites,
  271 cases, green in the Release and `-DKBC_SANITIZE=ON` lanes.
- Config loading (a strict `kb.toml` subset), the SQLite store and its single
  migration, the Markdown/HTML block parser with stable anchors, the tokenizer
  and its stopword list, the mmap'd inverted index with BM25, RRF fusion over
  the keyword and vector lanes, the mmap'd vector store, the inotify watcher
  with debounce, the subprocess embedder client, the epoll + `SO_REUSEPORT`
  HTTP daemon with bearer auth and an SSE stream, and a CLI with eleven verbs
  — all specified in the frozen headers under `include/kbc/` and all wired end
  to end.
- The daemon serves a route banner on `/` and nine JSON routes: `/api/health`,
  `/api/identity`, `/api/kbs`, `/api/stats`, `/api/search`, `/api/artifacts`,
  `/api/artifacts/{id}`, `POST /api/reindex` and `/api/events`. `reindex`,
  `search`, `get` and `list` work with or without a running daemon.
- A vector lane works when `kb-embedder` is configured and healthy, verified
  against the production sidecar with `bge-small-en-v1.5`. Without one,
  `hybrid` degrades to keyword and `semantic` returns nothing, and the
  response says `degraded: true`.
- A query grammar: `AND`/`OR`/`NOT`, groups, implicit AND, and `folder:` as a
  path-prefix facet. The keys the index cannot evaluate — `tag:`, `cap:`,
  `since:`, `index:`, `scope:` — are refused with a 400 rather than searched
  as literal terms. There is no phrase search, and there is none to port: a
  quoted `"…"` in the original quotes an atom *value*, not a phrase.
- Incremental index update: a file save re-indexes that one file in place,
  9–14 ms at 1,114 documents, and the update survives a restart.
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

- No filter overlay over the search results and no date filters — the
  `docs_query.rs` half of the grammar, and the keys kb-c refuses rather than
  mis-searches.
- No graph boost in ranking. It needs an in-degree edge graph kb-c does not
  have, and a fake version would be a different ranking function wearing the
  original's name.
- No web UI. The Rust repo's two React/Vite SPAs, the Claude Code plugins and
  the Playwright e2e suite are out of scope for the C port.
- The scale ladder stops at 5,000 documents and **locates no knee**. The
  generator is proven at 100,000 documents; the ingest was not run there.
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

The harnesses are in `bench/` (`bench-kbc.sh`, `bench-rust.sh`,
`bench-rust-concurrency.sh`, `bench-scale.sh`, `bench-vector.sh`) and the
in-process one is `kbc bench --queries N --repeat N --corpus DIR`.

## Layout

```
include/kbc/   the frozen contract — 14 headers, read these first
src/           the implementation, one file per subsystem
cli/main.c     the kbc binary
tests/         ctest-driven, one binary per test_*.c, harness in kbc_test.h
.github/       CI
```

## Contributing

Read [AGENTS.md](AGENTS.md) first — it holds the rules the headers and the
ownership model are built on. Then [CONTRIBUTING.md](CONTRIBUTING.md) for the
sign-off, the sanitizer lane and what a reviewable change looks like.
