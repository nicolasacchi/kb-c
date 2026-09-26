# Rust `kb` daemon — measured baseline

All numbers below were produced by one real run of `bench/bench-rust.sh` on
2026-09-26, 18:26–18:32 local time, on this machine. Nothing is estimated,
extrapolated, or copied from a previous run. Where a number is *not* a clean
measurement, that is stated in place.

Reproduce with:

    ./bench-rust.sh                                   # defaults below
    ./bench-rust.sh <corpus-dir> <port>

The script wipes its own sandbox, runs the whole measurement from a clean
state, prints `key=value` lines on stdout, and kills every process it started.

## Binary under test

| | |
|---|---|
| path | `/home/nik/project/kb/target/release/kb` |
| version | `kb 0.38-16-g9b0d3728` |
| size | 135,457,144 bytes |
| rebuilt for this run? | no — the pre-existing release binary was used as-is |

## Corpus

| | |
|---|---|
| path | `/home/nik/project/kb-c/bench/data/corpus` |
| files | 1594 (1147 `.md`, 447 `.html`) |
| bytes | 15,607,388 (14.9 MiB) |
| documents indexed | 1594 (1:1, no file dropped) |
| `open_errors` at end of ingest | 0 |

The corpus is the pre-staged snapshot; it was not regenerated.

## Configuration

The daemon ran fully isolated from the real `~/.config/kb` installation:
`HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and
`XDG_STATE_HOME` were all pointed at a private sandbox (`/tmp/kbbench-rust`).
`~/.config/kb` was neither read nor written (its mtime is unchanged,
`2026-09-26 02:14:04`). A separate port, 4318, was used and verified free
before and after the run.

The only config the daemon saw (`/tmp/kbbench-rust/.config/kb/kb.toml`):

```toml
[daemon]
name = "bench-rust"

[server]
addr = "127.0.0.1:4318"

[kb.bench]
path = "/home/nik/project/kb-c/bench/data/corpus"
```

No `embedding_model` was configured, so the daemon took its registry default
`bge-small-en-v1.5` (384-dim), loaded from the sandbox model cache
(`/tmp/kbbench-rust/.cache/kb/models`) into a `kb-embedder` subprocess. Both
the `keyword` (BM25 only) and the `hybrid` (BM25 + vector, RRF) lanes therefore
ran for real — see "what is and is not being compared" below.

## Command lines

    # daemon (inside the isolated env described above)
    kb daemon --config /tmp/kbbench-rust/.config/kb/kb.toml

    # end-to-end HTTP query sample (one = TCP connect + request + full JSON body)
    curl -fsS -o /dev/null -m 30 -w '%{time_total}' \
      "http://127.0.0.1:4318/api/search?q=<query>&mode=<keyword|hybrid>&kb=bench&limit=20"

    # in-process sample (no daemon, no HTTP)
    kb search --offline --kb bench --mode keyword --limit 20 --json <query>

    # index state
    curl -fsS http://127.0.0.1:4318/api/kb/bench/stats

## Query set

Eight fixed queries, terms sampled from the actual corpus text (occurrence
counts over the corpus: daemon 2488, artifact 1140, corpus 1129, markdown
1129, milestone 347, provenance 298, embedding 209, watcher 191, lancedb 124,
benchmark 124, tokenizer 105, throttle 41, telemetry 36, onboarding 27,
backpressure 4). 20 repetitions each = 160 samples per measurement.

    daemon | artifact provenance | markdown tokenizer | lancedb embedding
    benchmark milestone | watcher reconcile | throttle telemetry
    onboarding backpressure

Each query and each mode got one unrecorded warm-up request before its 20
recorded ones.

## Ingest and startup

| measurement | value | conditions |
|---|---|---|
| cold startup-to-ready | **3.681 s** | empty index, cold model cache; "ready" = daemon answers `GET /api/kb/bench/stats`. The embedder was still loading and `doc_count` was still 0 at that instant. |
| warm startup-to-ready | **0.596 s** | index already on disk (1594 docs), model already cached, embedder subprocess re-spawned. Verified: the responding pid differs from the stopped daemon's pid. |
| ingest wall clock | **252.180 s** | from daemon-ready to `doc_count == 1594`, 0 open errors, cold index |
| ingest throughput | 6.3 docs/s | 1594 / 252.18 s (derived, arithmetic only) |

Ingest wall clock across the three complete from-scratch runs of this script
on this machine: **295.217 s, 269.439 s, 252.180 s** (min-to-max spread
~17%). Only the last of those is the run whose latency numbers are reported
below; the two earlier runs were discarded because of two measurement bugs in
the script (sample-file aggregation and a stale-responder check), both since
fixed. Ingest timing was unaffected by those bugs.

## Idle RSS

RSS (`VmRSS`) of the daemon process and of its `kb-embedder` child, sampled
every second for 15 s and reported as the maximum seen.

| point | daemon | embedder | total |
|---|---|---|---|
| after ingest, in the process that did the indexing | 224,620 kB | 252,408 kB | **477,028 kB** |
| after warm restart + 160 HTTP queries + 160 in-process queries | 130,500 kB | 187,724 kB | **318,224 kB** |

The second row is a freshly started daemon that never ingested, so it is the
right number to compare a steady-state resident against. The embedder is a
separate process and is only resident once the corpus is opened / the vector
lane is used; a keyword-only deployment would not pay it.

## Query latency — end to end through the daemon's HTTP API

One sample = one complete HTTP round trip: TCP connect, request written, the
entire JSON response body read by `curl` (`curl -w time_total`). 160 samples
per row (8 queries × 20 reps), all in milliseconds.

| measurement | n | min | p50 | p95 | p99 | mean | max |
|---|---|---|---|---|---|---|---|
| HTTP, `mode=keyword` (BM25 only) | 160 | 15.501 | **19.910** | 27.602 | 30.749 | 20.512 | 40.621 |
| HTTP, `mode=hybrid` (BM25 + vector, RRF k=60) | 160 | 27.353 | **34.107** | 42.484 | 52.418 | 34.977 | 52.713 |

Per-query p50 (ms):

| query | HTTP keyword | HTTP hybrid |
|---|---|---|
| daemon | 18.630 | 34.949 |
| artifact provenance | 18.912 | 31.839 |
| markdown tokenizer | 17.687 | 35.538 |
| lancedb embedding | 23.363 | 39.687 |
| benchmark milestone | 18.376 | 35.185 |
| watcher reconcile | 19.704 | 33.542 |
| throttle telemetry | 19.762 | 30.699 |
| onboarding backpressure | 21.137 | 31.171 |

All HTTP numbers are warm: the index is fully built, the OS page cache is warm
from the ingest that just ran, and every query was issued at least once before
its recorded reps.

## Query latency — in-process (no HTTP)

There is **no** pure in-process latency benchmark in this CLI: `kb bench` is a
retrieval-quality bake-off (Recall@k / MRR / nDCG across embedding models) and
reports no timings. The closest available measurement is `kb search --offline`,
which opens the lance dataset directly with no daemon and no HTTP — but which
still pays a full CLI process start per sample. So this row is
"process start + dataset open + BM25 search + JSON render", **not** the query
cost alone, and it is not directly comparable to the HTTP rows.

| measurement | n | min | p50 | p95 | p99 | mean | max |
|---|---|---|---|---|---|---|---|
| `kb search --offline --mode keyword` | 160 | 24.691 | **29.436** | 37.277 | 48.797 | 52.200 | 3518.504 |

The mean exceeds p99 because of one 3518 ms outlier (a single CLI cold start
under machine load); the second-largest sample is 48.695 ms. The p50/p95/p99
columns are unaffected by it.

Per-query p50 (ms): daemon 32.377, artifact provenance 31.561, markdown
tokenizer 27.767, lancedb embedding 27.639, benchmark milestone 29.697, watcher
reconcile 29.711, throttle telemetry 28.159, onboarding backpressure 27.047.

## What is and is not being compared

* **HTTP rows vs. in-process row are not the same measurement.** The HTTP
  numbers include TCP connect, HTTP framing and JSON serialization; the
  in-process number excludes all of that but adds a CLI process launch. The
  in-process number being *larger* despite doing strictly less work per query
  is explained by the process launch, not by the search being slower. Do not
  subtract one from the other to estimate "HTTP overhead".
* **The vector lane really ran.** The daemon had a live
  `bge-small-en-v1.5` embedder; the `hybrid` rows are genuine BM25+vector RRF
  queries with the query embedded on every request. The `keyword` rows touch
  no embedder at query time, but the embedder process is still resident and is
  counted in the RSS totals above.
* **All latency numbers are warm-cache.** Nothing here measures a cold OS page
  cache or a cold lance dataset. A C implementation measured against these
  numbers would need the same warm-cache discipline, or the comparison is
  meaningless.
* **The corpus is this corpus.** 1594 markdown/HTML documents, ~15 MB, one
  directory, mixed sizes. No conclusion about scaling beyond that point is
  supported by this data.
* **The ingest number is wall clock for a single-threaded-ish watcher walk on
  a shared machine**, not a controlled throughput figure. Three runs gave
  252–295 s.
* **The daemon is the prebuilt release binary**, not a build from the current
  working tree, and the machine was running other work concurrently.

## Isolation and cleanup (verified)

* `~/.config/kb` untouched: directory mtime `2026-09-26 02:14:04`, before this
  session began.
* Port 4318 verified free before the run and free after it (`ss -ltn` shows no
  listener).
* The daemon and its `kb-embedder` child were terminated by the script's
  cleanup trap; only processes the script itself started were signalled (the
  real installation's own `kb` / `kb-embedder` processes, which use
  `/var/lib/kb/cache`, were left alone).
* Raw per-sample data from this run: `/tmp/kbbench-rust/samples/*.tsv`;
  machine-readable results: `/tmp/kbbench-rust/results.env`.
