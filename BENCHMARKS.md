# BENCHMARKS

Measured numbers only. Every figure came from a run on this machine (8-core
x86-64, Linux 7.0, 62 GB) on **2026-09-26**. Nothing here is estimated or
extrapolated. Where a comparison is imperfect, the imperfection is stated next to
the number, not in a footnote.

Reproduce with:

```bash
./bench/make-corpus.sh                                  # stage the corpus
QUERY_FILE=bench/queries.txt ./bench/bench-rust.sh       # the Rust baseline
./bench/bench-kbc.sh                                    # kb-c, single + concurrent
./build/kbc --config <cfg> bench                        # kb-c, in-process
```

The raw Rust measurements are in [`RUST-BASELINE.md`](RUST-BASELINE.md); the
scripts are [`bench-rust.sh`](bench-rust.sh) and
[`bench-kbc.sh`](bench-kbc.sh). Both engines are driven from the **same query
file**, [`queries.txt`](queries.txt).

## The corpus

| | |
|---|---|
| files staged | 1,594 (1,147 `.md`, 447 `.html`) |
| bytes | 15,607,388 (22 MB on disk) |
| documents indexed by kb-c | **1,114** |
| documents indexed by Rust `kb` | **1,594** |

The 480-document difference is deliberate, not a truncation: kb-c's walk skips
dot-directories (`.claude/`, `.github/`, `.kb-memory/`, `.grok/`), and exactly
480 of the 1,594 staged files live under one. **The corpora are not matched** —
the Rust side indexed 43% more documents, which inflates its figures somewhat
and does not come close to explaining a 22x gap.

## Query latency — matched A/B

Same 8 queries from `queries.txt`, same corpus, same host, `mode=keyword` on
both sides so neither pays for an embedding model. Full `curl` round trip: TCP,
HTTP, query, serialised response. 160 samples each, one discarded warm-up per
query.

| percentile | Rust `kb` | kb-c | ratio |
|---|---|---|---|
| min | 16.456 ms | 0.711 ms | 23x |
| **p50** | **21.418 ms** | **0.958 ms** | **22x** |
| p95 | 35.755 ms | 1.243 ms | 29x |
| p99 | 63.625 ms | 1.378 ms | 46x |
| mean | 23.245 ms | — | — |

This is the run to quote. An earlier Rust run with a *different* query set
measured p50 19.910 ms, so run-to-run variance on this shared host is roughly
±8%; the gap is an order of magnitude larger than that.

### In-process query cost (kb-c only)

`kbc bench` measures the query itself — no HTTP, no process launch:

```
documents: 1114   terms: 34401   queries: 20 (repeats: 20, samples: 400)
min: 13 us   p50: 47 us   p95: 289 us   p99: 319 us
```

**This is not comparable to the table above** and must not be subtracted from it.
The Rust CLI has no in-process latency benchmark — its `kb bench` measures
retrieval *quality* (Recall@k, MRR, nDCG) and reports no timings.

## Ingest and reindex

| | Rust `kb` | kb-c |
|---|---|---|
| full corpus, from empty | 252–262 s (1,594 docs) | 1.79 s / 1.83 s (1,114 docs) |
| **one file changed** | full rebuild, same cost | **10.3 ms** (was 1,334.9 ms) |
| largest single file (2.4 MB) | — | 75.6 ms |

The single-file row is the one that matters for a daemon: a file save used to
re-parse and re-index the whole corpus, and now costs what the file costs.
**~130x**, and the cost is proportional to the file rather than the corpus.

The full-corpus gap is also partly "kb-c does less per document" — no links,
wikilinks, attachments, capability flags, frontmatter or provenance edges, and
no embedding model at ingest. Read it as a bound, not as "C is 140x faster".

## Memory and startup

| | Rust `kb` | kb-c |
|---|---|---|
| idle RSS, steady state | 321,580 kB | 18,816 kB |
| breakdown | 133,884 kB daemon + 187,696 kB `kb-embedder` | single process |
| startup to ready, warm | 429 ms | 108–123 ms (3 runs) |
| startup to ready, cold | 5.624 s | not measured |

Against the daemon process alone (133,884 kB) the ratio is ~7x; against the
total it is ~17x. The total includes a resident ONNX model kb-c does not load.

## Concurrency

`bench/bench-kbc.sh`, kb-c only — the Rust daemon's concurrency was **not**
measured, so there is no comparison to draw. 8 clients, REPS=30, after the
per-hit resolve fix:

| clients | rps | p50 | p99 |
|---|---|---|---|
| 1 | 1,385 | 0.82 ms | 1.49 ms |
| 2 | 2,286 | 0.92 ms | 2.27 ms |
| 4 | 2,648 | 1.62 ms | 7.67 ms |
| 8 | 2,336 | 2.43 ms | 7.20 ms |

Throughput does not scale past ~4 clients. That was diagnosed rather than
guessed, and the first diagnosis was only half right:

- **Ruled out by measurement:** the client (4 processes x 8 threads reach the
  same rps at 12% CPU each), the epoll worker count (2/4/8/16 workers plateau
  identically), CPU (~1.2 of 8 cores busy at the plateau), fd/connection limits
  (16 connections, 0 errors), and the scoring path itself — queries returning no
  hits reach **20.7k rps** at 8 clients.
- **Fixed:** the per-hit store resolve took the store's single mutex once per
  returned hit. Resolving a query's hits in one `kbc_store_get_artifacts_by_path`
  call took it once per query. Interleaved A/B at 8 clients: **1,832 → 2,336 rps
  median (+27%)**, p50 −20%, p99 halved. The `limit=1` vs `limit=50` spread —
  which isolates the per-hit cost — went from **10.0x to 6.0x**, and the
  marginal cost per hit fell from 33.1 µs to 18.5 µs.
- **Still the limit:** the residual ~18 µs is the **row read, not the lock**.
  `artifacts.source` averages 12.5 KB, so each resolve walks a multi-page
  overflow record. Fixing it needs a schema change (a slim covering table
  without `source`, or a search projection off the wide row). Not done.
- The literal prescription to build the batch as one
  `(?1 AND ?2) OR (?3 AND ?4) ...` statement was **slower**, because
  `sqlite3_prepare_v2` re-parses and re-plans the n-term OR on every query
  (193 µs at n=10 vs 168 µs for one statement reset and rebound per pair). The
  first implementation measured *worse than baseline* and was thrown away; what
  shipped is the reset-and-rebind form.

## What was not measured

- **The vector lane.** kb-c's embedder seam is implemented and tested against a
  fake sidecar, but no real `kb-embedder` was ever run against kb-c. The Rust
  hybrid row (p50 42.116 ms with a real `bge-small-en-v1.5`) therefore has no
  counterpart here.
- **The Rust daemon under concurrency.** kb-c's scaling curve stands alone.
- **A larger corpus.** 1,114 documents exercises the index but does not
  characterise it at 100k+, where the mmap'd postings start paging.
- **Cold page cache** for query latency; all query figures are warm.
- **kb-c startup, cold** (no index on disk).

## Why the query path is faster, and what is unproven

1. **No database on the query path.** The Rust build answers a keyword query
   through lancedb → arrow → datafusion. kb-c answers from its own inverted
   index; SQLite stores the records and is touched only to resolve the returned
   hits, in one batched call. Structural, not a tuning win.
2. **Contiguous per-term postings**, materialised at build time, so scoring is a
   sequential memory scan with no indirection.
3. **Bounded top-k selection** — cost tracks the page size, not the corpus size.
4. **Arena-only request allocation** — no per-object malloc/free in a query.
5. **Thread-local query scratch** — workers never serialise on the accumulator.
   65 ThreadSanitizer races before the fix, 0 after, no added allocation.
6. **Incremental reindex** — a file save costs that file, not the corpus.

Unproven: that any of this holds at a scale beyond what was measured, that it
holds for the vector lane, or that it holds under concurrent load. Those need
their own measurements.
