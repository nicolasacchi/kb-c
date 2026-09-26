# kb-c

kb-c is a C17 rewrite of the [kb](https://github.com/nicolasacchi/kb) daemon — the
self-hosted system of record for everything an AI agent writes. It targets lower
query latency and a much smaller dependency surface: the query path is a
memory-mapped inverted index, so it touches no database and copies no bytes it
does not need.

The relationship to the Rust original: same product, same corpus model, new
engine. A kb-c daemon and the Rust daemon use different default ports (4317 vs
4000) and different data directories, so they can run side by side against the
same corpus.

## Status

**As of 2026-09-26 — the port is in progress. This is a pre-1.0 codebase.**

Works today:

- `kbc` builds from source with CMake and passes its ctest suite.
- Config loading (a strict `kb.toml` subset), the SQLite store and its
  migrations, the block/anchor parser, the tokenizer, the mmap'd inverted index
  with BM25, RRF fusion over the keyword and vector lanes, the inotify watcher,
  and the subprocess embedder seam are specified in the frozen headers under
  `include/kbc/`. Which of them are wired end to end changes week to week —
  read the source, not this list, if you need today's truth.

Not done:

- No published performance numbers. The head-to-head harness is `kbc bench`;
  its results have not been written up.
- No web UI. The Rust repo's two React/Vite SPAs, the Claude Code plugins and
  the Playwright e2e suite are out of scope for the C port.
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
data_dir = "./data"
port = 4317

[[corpus]]
name = "docs"
path = "./corpus"
```

Index it, then search it:

```bash
./build/kbc reindex --config kb.toml
./build/kbc search "inverted index" --config kb.toml
```

Run the daemon and talk to it over HTTP:

```bash
./build/kbc daemon --config kb.toml &
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
        [ kbc CLI: reindex | search | daemon | status | bench | config ]
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

There are no published latency numbers for kb-c yet, so this section makes no
claims. The head-to-head harness is `kbc bench`; the numbers belong in
`BENCHMARKS.md`, which does not exist yet. When it does, it will state the
corpus, the machine, the query set and the Rust build it was compared against.

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
