# BENCHMARKS

Measured numbers only. Every figure below came from a run on this machine
(8-core x86-64, Linux 7.0, 62 GB RAM) on **2026-09-26**. Nothing here is
estimated, extrapolated or rounded away from what was observed. Where a
comparison is imperfect, the imperfection is stated in the same section as the
number.

The raw Rust measurements, with their conditions and the script that produces
them, are in [`RUST-BASELINE.md`](RUST-BASELINE.md). The script is
[`bench-rust.sh`](bench/bench-rust.sh).

## The corpus

Both engines indexed the same staged tree, produced by
[`bench/make-corpus.sh`](bench/make-corpus.sh) from the Markdown and HTML
already in the `kb` checkout:

| | |
|---|---|
| files staged | 1,594 (1,147 `.md`, 447 `.html`) |
| bytes | 15,607,388 (22 MB on disk) |
| documents indexed by kb-c | **1,114** |
| documents indexed by Rust `kb` | **1,594** |

The 480-document difference is a deliberate behaviour, not a truncation:
kb-c's walk skips dot-directories (`.claude/`, `.github/`, `.kb-memory/`,
`.grok/`), and exactly 480 of the 1,594 staged files live under one. The
corpus therefore is **not** matched between the two engines — the Rust side
indexed 43% more documents. That inflates the Rust figures somewhat; it does
not come close to explaining a 21x gap.

## Query latency — keyword mode, HTTP round trip

Same host, warm cache, `--mode keyword` on both sides, so neither is paying for
an embedding model. Latency is a full `curl` round trip: TCP, HTTP, query,
serialised response.

| percentile | Rust `kb` | kb-c | ratio |
|---|---|---|---|
| min | 15.501 ms | 0.708 ms | 22x |
| **p50** | **19.910 ms** | **0.954 ms** | **21x** |
| p95 | 27.602 ms | 1.300 ms | 21x |
| p99 | 30.749 ms | 1.536 ms | 20x |
| max | 40.621 ms | 1.578 ms | 26x |

kb-c: 8 corpus-derived queries x 20 repetitions = 160 samples, one discarded
warm-up per query. Rust: 8 queries x 20 repetitions = 160 samples per row, the
same protocol, on a different run of `bench-rust.sh`.

### The query sets differ — read this before quoting the ratio

Each side was given 8 queries drawn from the same corpus, but **not the same
8 queries**: the Rust baseline was measured first and its query set was not
handed to the kb-c run. Both sets are corpus-derived and of similar shape, and
both engines are asked for `limit=10`, but this is a real difference in method
and it is the weakest link in the comparison above. A matched-query-set run is
the obvious next measurement and is not something these numbers should be read
as having done.

### In-process query cost (kb-c only)

`kbc bench` measures the query itself, with no HTTP and no process launch:

```
corpus:    /home/nik/project/kb-c/bench/data/corpus
documents: 1114
terms:     34401
queries:   20 (repeats: 20, samples: 400)
min:       13 us    p50: 47 us    p95: 289 us    p99: 319 us
```

**This is not comparable to the Rust HTTP numbers above** and must not be
subtracted from them. It answers a different question. The Rust CLI has no
in-process latency benchmark of its own — `kb bench` there measures retrieval
*quality* (Recall@k, MRR, nDCG) and reports no timings — so the closest Rust
figure is `kb search --offline`, which skips HTTP but pays a full CLI process
start per sample and measured *slower* than the HTTP path for that reason.

## Ingest

Full scan of the corpus from an empty state, wall clock, both engines doing
their own parsing, tokenizing, indexing and persisting:

| | Rust `kb` | kb-c |
|---|---|---|
| documents | 1,594 | 1,114 |
| ingest | 252.180 s | 1.79 s |
| repeat runs | 295.217 / 269.439 / 252.180 s | 1.79 s / 1.83 s |

Roughly 140x, and the Rust figure varied ~17% across three runs on a shared
machine. kb-c's two runs are within 2% of each other.

The two engines are not doing identical work — kb-c does not extract links,
wikilinks, attachments, capability flags, frontmatter or provenance edges, and
does not run an embedding model at ingest. This is the honest reason the ingest
gap is much larger than the query gap, and it should be read as "kb-c does less
per document", not as "C is 140x faster".

## Memory

| | Rust `kb` | kb-c |
|---|---|---|
| idle RSS, steady state | 318,224 kB | 18,816 kB |
| breakdown | 130,500 kB daemon + 187,724 kB `kb-embedder` | single process |

The Rust total includes a resident ONNX embedding model. The comparison a
reader should draw is against the daemon process alone (130,500 kB), which is
still ~7x kb-c — consistent with kb-c holding one mmap'd inverted index and a
SQLite file rather than an Arrow/Lance columnar store plus a model.

## Startup

| | Rust `kb` | kb-c |
|---|---|---|
| startup to answering requests (warm, index on disk) | 596 ms | 108–123 ms (3 runs) |

## What was not measured

- **The vector lane.** kb-c's embedder seam is implemented and tested against a
  fake sidecar, but no real `kb-embedder` was run against kb-c during this
  benchmark, so there is no kb-c hybrid-semantic number. The Rust hybrid row
  (p50 34.107 ms, real `bge-small-en-v1.5`) therefore has no counterpart here
  and is reported in `RUST-BASELINE.md` for reference only.
- **Concurrent load.** Every figure is a single client, serialised. Neither
  engine was measured under concurrency, so no claim is made about throughput
  under parallel queries.
- **Cold page cache.** All query figures are warm.
- **A larger corpus.** 1,114 documents is enough to exercise the index but not
  enough to characterise its behaviour at 100k+ documents, where the mmap'd
  postings start paging.

## Why the query path is faster, and which parts are unproven

The mechanism is specific, and each part is either implemented and tested or
explicitly not claimed:

1. **No database on the query path.** The Rust build answers a keyword query
   through lancedb → arrow → datafusion. kb-c answers from its own inverted
   index; SQLite stores the records and is not touched during a search. This is
   the single largest contributor and it is structural, not a tuning win.
2. **Contiguous per-term postings.** A term's postings are materialised into one
   contiguous run at build time, so scoring is a sequential memory scan with no
   indirection. A stable counting sort keyed on a rehash-stable term id does
   that materialisation.
3. **Bounded top-k selection.** A search returns the best `limit` hits through a
   bounded heap rather than sorting every matching document, so cost tracks the
   page size, not the corpus size.
4. **Arena-only request allocation.** A request's JSON, rows and token buffers
   come from one arena that is reset at the end, so a query performs no
   per-object malloc/free pairs.
5. **Thread-local query scratch.** The scoring accumulator is per-thread with a
   destructor, so concurrent workers never serialise on it. Verified: 65
   ThreadSanitizer races before the fix, 0 after, with no added allocation per
   query.

What is **not** claimed: that any of this holds at scale beyond what was
measured, that it holds for the vector lane, or that it holds under concurrent
load. Those need their own measurements.
