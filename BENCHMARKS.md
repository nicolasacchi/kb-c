# BENCHMARKS

Measured numbers only. Every figure came from a run on this machine (8-core
x86-64, Linux 7.0, 62 GB) on **2026-09-26**, except the C-client concurrency
ladder, which is **2026-09-27**. Nothing here is estimated or extrapolated.
Where a comparison is imperfect, the imperfection is stated next to the number,
not in a footnote.

Reproduce with:

```bash
./bench/make-corpus.sh                                  # stage the corpus
QUERY_FILE=bench/queries.txt ./bench/bench-rust.sh       # the Rust baseline
CLIENT=c ./bench/bench-kbc.sh                           # kb-c, single + concurrent
CLIENT=python ./bench/bench-kbc.sh                      # the same, GIL-limited
./build/kbc --config <cfg> bench                        # kb-c, in-process
./bench/bench-rust-concurrency.sh bench/data/corpus/web-code  # matched-corpus A/B
./bench/bench-scale.sh 1000 && ./bench/bench-scale.sh 5000     # the scale ladder
./bench/bench-vector.sh                                         # the real sidecar
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
| **one file changed** | full rebuild, same cost | **33–36 ms at 1,000 and at 20,000 documents** — the cost no longer scales with the corpus |
| largest single file (2.4 MB) | — | 75.6 ms |

The single-file row is the one that matters for a daemon: a file save used to
re-parse and re-index the whole corpus, and now updates the index in place. The
9–14 ms figure at 1,114 documents is the end-to-end proof — an in-place update to
a live index, with the store row committed first, that survives a restart of
the daemon and reads back identical.

**And the cost is NOT proportional to the file.** That was a claim in an earlier
revision of this document and the scale ladder disproved it. Measured across
three sizes, a single-file save costs:

| documents | index size | save (median) | per MB of INDEX | per MB of CORPUS |
|---|---|---|---|---|
| 1,000 | 1.5 MB | 251 ms | 167 ms | 127 ms |
| 5,000 | 6.5 MB | 500 ms | 77 ms | 50 ms |
| 20,000 | 23.5 MB | 1.19 s | 186 ms | 110 ms |

It tracks INDEX SIZE, not corpus size and not file size: the daemon rewrites
the index arena and fsyncs it on every save, so the cost is a function of how
much index there is to rewrite. It is also violently variable — the three raw
samples at 20,000 were 1.19 s, 0.98 s and 10.95 s — so the MEDIAN is the number
to quote, not the mean, and a save at 20,000 documents can take ten seconds.

## Why the save path had to change

The table above says what was wrong. This is the fix and what it bought.

**What the cost actually was.** Instrumenting `kbc_index_save` and
`kbc_str_write_file_atomic` separately, at 20,000 documents on real storage
(`/dev/md3`, ext4 on a 3-disk RAID), 41 timed reps with 2 warmups discarded:

| phase | median | share |
|---|---:|---:|
| SERIALIZE (build the file in memory) | 103.3 ms | 13.1% |
| WRITE | 35.9 ms | 4.6% |
| fsync the file | 403.9 ms | 51.3% |
| fsync the directory | 104.2 ms | 13.2% |
| rename | 1.8 ms | 0.2% |
| `free()` of the 24.7 MB scratch buffer | ~137 ms | 17.4% |
| **total** | **786.5 ms** | |

The same save on tmpfs is **49.7 ms**. The difference is the device, not the
CPU, and `dd bs=1M count=25 conv=fsync` on this host is 0.34 s median against
0.06 s on tmpfs — the same shape.

**So the obvious fix was not enough.** SERIALIZE is 13.1%, and a *free*
serialiser still leaves 683 ms of a 786 ms save. Bulk-copying the sections that
are byte-identical in memory and on disk (`kbc_posting` and `kbc_doc_slot`,
pinned by `_Static_assert` on `sizeof` and `offsetof`; `kbc_term_slot` is 32
bytes with only 24 persisted and cannot be) took 1,437,761 per-row
`kbc_str_append` calls at 17.5 ns down to one `memcpy` — 25.1 ms to 1.33 ms for
the same bytes. That is worth having, and it is not the fix.

**The fix is to stop rewriting.** A single-document update now appends a record
to `<index>.journal` and fsyncs *that*; `kbc_index_open` replays the journal, so
the index file plus its journal replayed in order is the live index. The index
is rewritten whole only when the pending delta passes `KBC_INDEX_JOURNAL_MAX`
(4 MiB) or on a full rebuild, which drops the journal.

**What it bought**, measured as a matched A/B — both arms on the same data, in
the same process, interleaved, 2 warmups discarded, 10 samples each, persistence
timed separately from the mmap and from the index mutation. The 5,000-document
rung is the same corpus the ladder above uses (13,387 terms, reproduced):

| | index size | OLD (rewrite) | NEW (journal) | ratio | journal bytes |
|---|---:|---:|---:|---:|---:|
| 1,000 documents | 1.5 MB | 65.8 ms | **30.6 ms** | 2.15x | 1,881 |
| 5,000 documents | 6.5 MB | 110.5 ms | **33.3 ms** | 3.31x | 1,848 |
| 20,000 documents | 23.5 MB | 208.4 ms | **36.3 ms** | 5.74x | 1,892 |
| 20,000, worst sample | 23.5 MB | 358.3 ms | **70.7 ms** | 5.07x | — |

**The number to read is the NEW column: 30.6, 33.3, 36.3 ms across a 20x range
of corpus, and a journal that is the same ~1.9 KB at every rung.** The cost of
saving one changed file no longer has a term in it for the size of the index.

And the reason the ratio is not larger is the honest limit of the fix: the
journal appends **1,892 bytes after 10 appends** and still takes 36.3 ms,
because that is this device's fsync floor. Measured directly, fsync costs
33.9 ms for 90 bytes, 33.7 ms for 64 KiB, 98.2 ms for 1.5 MB and 140.3 ms for
24.7 MB. **The journal has removed the entire size-dependent part of the
barrier and is now sitting on the irreducible part.** On a device with cheap
sync the same change would read as a much larger ratio, because the floor
would be smaller — that is a property of the storage, not of kb-c.

The `open (mmap)` row is not a real difference and is not quoted: both arms
mmap the same file, and the arm that runs second pays for the first arm's
24.7 MB write. The rows to read are `PERSIST` and `persist max`.

**What is now true, and what is still not.** A file save's cost no longer
scales with the index: 1,000 and 20,000 documents persist in 33.0 ms and
36.4 ms, which is the same number to within the noise of the barrier. What is
still unproven is the *compaction* cost — the full rewrite is unchanged and
still costs what the table above says, and a long-lived daemon pays it every
4 MiB of accumulated delta.

The paragraph this section replaces said the fix was "unbuilt and the most
valuable thing left in this project". That was accurate when written and is not
now: the delta journal above is that fix, and the property the incremental
reindex was built for — a save costs what the file costs — now holds, because
the cost no longer scales with the index at all.

**An earlier number for this row was 10.3 ms, and a later measurement of the
same thing was 355 ms at 1,000 documents. The 355 ms is not a regression.** It
was taken while the watcher-path defect was live: the watcher published an
absolute path, `app.c` read it as corpus-relative, the file could not be
resolved, and a save was therefore recorded as a *removal* and swept the
document out of the index — a different code path from the incremental one,
measured under a different corpus size. Both defects are fixed and each has an
end-to-end proof; the incremental number to quote is 9–14 ms. The earlier
10.3 ms is not repeated in the table because it predates the fix and would
imply a clean measurement it never was.

The full-corpus gap is also partly "kb-c does less per document" — no
attachments, no capability analysis and no embedding model at ingest. It now
does extract links and wikilinks and reads Markdown front matter, but the
`cap:` facet it filters on is declared metadata rather than an analysis
kb-c does not perform. Read the gap as a bound, not as "C is 140x faster".

## Memory and startup

| | Rust `kb` | kb-c |
|---|---|---|
| idle RSS, steady state | 321,580 kB | 18,816 kB |
| breakdown | 133,884 kB daemon + 187,696 kB `kb-embedder` | single process |
| startup to ready, warm | 429 ms | 108–123 ms (3 runs) |
| startup to ready, cold | 5.624 s | not measured |

Against the daemon process alone (133,884 kB) the ratio is ~7x; against the
total it is ~17x. The total includes a resident ONNX model kb-c does not load.

### With a real embedder and a resident model

The table above is keyword-only. With the production
`/home/nik/.local/bin/kb-embedder` running `bge-small-en-v1.5` and the model
resident, the same 1,114-document corpus:

| | |
|---|---|
| ingest, with embeddings | **178 s** (1.79 s without — the model does that work) |
| documents embedded | 1,114 of 1,114, dim 384 |
| RSS, model resident | **299,948 kB** total: 22,844 kB daemon + 277,104 kB sidecar |
| the Rust baseline, same shape | 321,580 kB total |

So with the same model doing the same embedding work, kb-c holds ~7% *less*
resident memory than the Rust daemon. The daemon's own footprint barely moves
(18,816 kB → 22,844 kB) because the vector matrix is `mmap`ed, not copied.

## Concurrency — kb-c, with a client that is not the bottleneck

`bench/bench-kbc.sh` with `CLIENT=c` — the load client in
[`kbcbench-client.c`](kbcbench-client.c) — on the same corpus, the same query
file and the same `limit=10` the script defaults to, 4 workers, REPS=200,
2026-09-27. Both CPU columns are printed by the script at every level, so the
reader can see which side saturated.

| conc | rps | p50 | p95 | p99 | client CPU | daemon CPU |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3,387 | 0.26 ms | 0.55 ms | 0.63 ms | 0.054 | 0.889 |
| 2 | 4,985 | 0.35 ms | 0.89 ms | 1.10 ms | 0.081 | 1.433 |
| 4 | 6,948 | 0.47 ms | 1.04 ms | 1.43 ms | 0.118 | 2.019 |
| 8 | 8,113 | 0.78 ms | 1.98 ms | 2.68 ms | 0.137 | 2.497 |
| 16 | 8,747 | 1.39 ms | 3.65 ms | 5.61 ms | 0.150 | 2.617 |
| 32 | 9,233 | 2.84 ms | 7.23 ms | 9.64 ms | 0.160 | 2.824 |

**The client-vs-daemon CPU comparison is the acceptance evidence.** At 32
clients the client burns 0.160 cores against the daemon's 2.824 — about 17x
cheaper — and it is cheaper at every level in the table. That is the whole
point of writing the client in C: the python client's GIL saturated before
the daemon did, so the number it reported was a property of the harness.
These rps are the server's.

**The knee is at 16, not at 32.** 16→32 buys +6% rps while p50 more than
doubles and p99 grows 1.7x, so past 16 the extra connections buy queueing, not
throughput. Past 32 it goes backwards: **48 → 6,667 rps, 64 → 5,513 rps**, and
the daemon's CPU *falls* to 1.80 then 1.34 cores. Falling server CPU at rising
latency means the daemon is blocked, not searching.

Absolute values track this host's loadavg (about 40, from other users), and
are not portable; the shape is, and was reproduced across three runs.

### The same workload with the python client

Kept because the comparison is the interesting part — same corpus, same query
set, same `limit=10`, same percentile code, one GIL apart. On the same run the
python client reached **4,843 rps at 8 clients with 1.12 client cores against
the daemon's 1.55** — more client CPU than server CPU, and 60% less
throughput. Every kb-c concurrency figure in the sections below was taken
this way, and every one of them is a floor rather than a ceiling for the same
reason.

## Concurrency, Rust vs kb-c — MATCHED corpora

`bench/bench-rust-concurrency.sh` against `bench/bench-kbc.sh`, both engines on
the **same 360-document corpus** (`bench/data/corpus/web-code`, 360 documents
indexed by each), the same query file, `REPS=30`, `mode=keyword`, 1/2/4/8
clients, 2026-09-26. The corpora are matched here, which the latency table
above is not. **The kb-c columns are the python-client measurement** — see the
section above for the same workload with a client that is not the bottleneck.

| clients | Rust rps | kb-c rps | Rust p50 | kb-c p50 |
|---|---|---|---|---|
| 1 | 11.5 | 2,484 | 47.5 ms | 0.354 ms |
| 2 | 23.7 | 3,806 | 77.5 ms | 0.450 ms |
| 4 | 25.3 | 4,160 | 150.9 ms | 0.773 ms |
| 8 | 31.9 | 4,615 | 260.5 ms | 1.394 ms |

**Caveats, stated here rather than in a footnote.** 360 documents is a small
corpus, chosen because Rust ingest on this contended host ran at 1.2–2.5 docs/s
and a larger one did not fit in a reasonable run; it is matched, not
representative. kb-c's 4- and 8-client rows are **client-limited** — the Python
client burns 1.10 cores against the daemon's 0.91, so kb-c's throughput and
latency at those levels are a floor, not a ceiling. Both engines were driven
from the same file, so the query sets are identical.

The rps and p50 columns are throughput-shaped and therefore hostage to who is
the bottleneck. The number that is not is **CPU per query**:

| | Rust `kb` | kb-c | ratio |
|---|---|---|---|
| CPU per query | ~35 ms, flat at 1/2/4/8 clients | ~0.21 ms, flat | **~170x** |
| cores busy at the plateau | never more than 1.09 of 8 | — | — |

~170x of CPU per query is the comparison to quote. It is flat on both sides,
so it is a property of the work, not of the client count. Rust scales to 2.06x
at two clients and then flattens; kb-c is still climbing at 8 but the client,
not the daemon, is what stops it.

## Concurrency — the two resolve fixes (kb-c only)

`bench/bench-kbc.sh`, **python client**, 8 clients, REPS=30, the `limit=10`
the script uses by default. Before and after the resolve fix, run back to back
on the same corpus on 2026-09-26. These are the python-client figures the
resolve work was diagnosed against, kept as the record of that diagnosis; the
C-client ladder is above.

| clients | rps before | rps after | p50 before | p50 after | p99 before | p99 after |
|---|---|---|---|---|---|---|
| 1 | 1,372 | 2,452 | 0.68 ms | 0.38 ms | 1.48 ms | 0.62 ms |
| 2 | 2,497 | 4,525 | 0.74 ms | 0.41 ms | 1.55 ms | 0.90 ms |
| 4 | 2,325 | 5,762 | 1.21 ms | 0.61 ms | 3.57 ms | 1.75 ms |
| 8 | 2,533 | 5,682 | 3.02 ms | 1.17 ms | 5.64 ms | 3.99 ms |

The "before" column reproduces the earlier per-hit-resolve-fix run
(1,385 / 2,286 / 2,648 / 2,336 rps) to within host noise, and it shows the same
thing: **throughput did not scale past ~4 clients.** After the fix it still
flattens — 4 and 8 clients are within 2% of each other — but at 2.3x the
throughput, and the ceiling has moved off the per-hit resolve (see below).
Throughput was diagnosed rather than guessed, and the first diagnosis was only
half right:

- **Ruled out by measurement:** the client (4 processes x 8 threads reach the
  same rps at 12% CPU each), the epoll worker count (2/4/8/16 workers plateau
  identically), CPU (~1.2 of 8 cores busy at the plateau), fd/connection limits
  (16 connections, 0 errors), and the scoring path itself — queries returning no
  hits reach **20.7k rps** at 8 clients.
- **Fixed (1):** the per-hit store resolve took the store's single mutex once per
  returned hit. Resolving a query's hits in one `kbc_store_get_artifacts_by_path`
  call took it once per query. Interleaved A/B at 8 clients: **1,832 → 2,336 rps
  median (+27%)**, p50 −20%, p99 halved. The `limit=1` vs `limit=50` spread —
  which isolates the per-hit cost — went from **10.0x to 6.0x**, and the
  marginal cost per hit fell from 33.1 µs to 18.5 µs.
- **Fixed (2):** that residual was the **row read, not the lock** — the
  diagnosis was right, the prescription was not. `artifacts.source` averages
  12.5 KB of a ~12.8 KB record, so every resolve decoded an overflow record to
  get two small columns out of it. The resolve's statement simply stopped
  selecting `source` (no schema change, no migration: nothing on the search
  path reads it, and `kbc_store_get_artifact(..., with_source=true)` is a
  different call that still returns the full text). Interleaved A/B, 8 clients,
  five paired runs, REPS=30, `limit=50`:

  | | rps | p50 | p99 |
  |---|---|---|---|
  | before | 872 (median of 872/911/864/886/854) | 7.4 ms | 14.4–19.5 ms |
  | after | **2,967** (3102/3071/2687/2748/2967) | **2.0 ms** | 4.6–6.5 ms |

  **+240%**, and the same `limit=1` run — which resolves one hit and so is the
  control — did not move at all (5,643 → 5,671 rps median), which is what makes
  the limit=50 figure attributable to the per-hit read rather than to noise or
  to the daemon getting faster in general. The `limit=1` vs `limit=50` spread
  fell from **6.5x to 1.9x**.
- **What did NOT help, and why.** Both structural alternatives were built and
  measured before the one-line change was chosen, on the benchmark database:
  - a covering index on `(corpus, path)` including id/title/summary is
    **not used by the planner** — `EXPLAIN QUERY PLAN` still reports
    `SEARCH artifacts USING INDEX sqlite_autoindex_artifacts_2`, because the
    existing `UNIQUE(corpus, path)` is a smaller candidate and equally exact.
    Adding it changed nothing (3.6–4.2 µs per lookup either way);
  - moving `source` into its own `artifact_sources(doc_id, source)` table —
    i.e. making the hot row physically small — was built and timed too, and it
    measured **the same as simply not selecting the column** (min 3.57 µs,
    p50 4.31 µs against 3.50–3.64 / 4.11–4.51). Omitting the column already
    stops the overflow walk; physically removing the bytes buys nothing on top
    of that. So no migration was added, which is also why an existing database
    needs none: `CREATE TABLE` and the statement text are the only changes.
- The single-process microbenchmark is committed as `bench/resolve-mb.c`
  (`cc -O2 -o /tmp/resolve-mb bench/resolve-mb.c -lsqlite3`) so the per-lookup
  cost can be compared directly instead of inferred from the daemon. On the
  1,114-document benchmark database, 21 interleaved trials: at 50 pairs
  **5.75 µs wide vs 3.59 µs slim** (min), 6.59 vs 4.15 (p50); at 10 pairs
  4.96 vs 3.53 and 5.58 vs 4.24. This host is shared, so the trial spread is
  wide and the min/median are both reported; the end-to-end A/B above is the
  number to quote.
- The literal prescription to build the batch as one
  `(?1 AND ?2) OR (?3 AND ?4) ...` statement was **slower**, because
  `sqlite3_prepare_v2` re-parses and re-plans the n-term OR on every query
  (193 µs at n=10 vs 168 µs for one statement reset and rebound per pair). The
  first implementation measured *worse than baseline* and was thrown away; what
  shipped is the reset-and-rebind form.

## The vector lane

Verified against the **production** `/home/nik/.local/bin/kb-embedder` running
`bge-small-en-v1.5` — not a fake sidecar.

| | |
|---|---|
| ingest with embeddings, 1,114 docs | **178 s** (1.79 s without) |
| documents embedded | 1,114 / 1,114, dim 384 |
| RSS with the model resident | 299,948 kB (daemon 22,844 + sidecar 277,104) |
| Rust baseline, same shape | 321,580 kB |

After a streaming top-k fix, `mode=semantic` and `mode=hybrid` both work at
1,112 documents with `degraded:false` and real vector scores. A semantic
query's **peak arena is 71,406 bytes whether the store holds 1,114 rows or
20,000** — the vector scan is a single sequential pass, so the per-request
allocation does not track the corpus. That is the number that says the
brute-force scan is not the thing that will not scale; the 20,000-row cliff
where IVF-PQ would start to pay for itself is still ahead, and unmeasured.

**The wire protocol, corrected.** An earlier draft of `include/kbc/embed.h`
documented a protocol the real sidecar does not speak, in every respect. What
it actually is: there is **no health op**; readiness is an **unsolicited**
`{"kind":"ready",...}` line that must be absorbed wherever it arrives, including
mid-reply; **`req_id` is mandatory** and must be echoed, and a reply carrying
someone else's `req_id` is a protocol error; replies are `kind=embed_ok` and
`kind=error`, not what the draft said. And the sidecar reads stdin as UTF-8
and **exits** on a non-UTF-8 line, so no byte >= 0x80 may reach the wire — text
is escaped locally to `\u00XX` rather than passed through raw. A corpus with
8-bit bytes otherwise kills the sidecar mid-ingest and silently ends the vector
lane.

## The scale ladder

`bench/bench-scale.sh` is a byte-reproducible generator (`--seed 20260926`): the
same seed gives the same bytes, and the regeneration was verified against the run
it reproduces. The ladder was measured at three rungs; the fourth is covered in
"what was not measured".

| | 1,000 | 5,000 | 20,000 |
|---|---|---|---|
| corpus | 2.0 MB | 10.1 MB | 39.9 MB |
| index on disk | 1.5 MB | 6.5 MB | 23.5 MB |
| index/corpus | 0.75 | 0.64 | 0.59 |
| ingest | 8.3 s | 61.3 s | 213.7 s |
| ingest per document | 8.3 ms | 12.3 ms | 10.7 ms |
| RSS at rest | 9 MB | 16 MB | 36 MB |
| — file-backed (mmap of the index) | 5.6 MB | 9.6 MB | 19.7 MB |
| — anonymous (heap) | 3.5 MB | 6.6 MB | 16.0 MB |
| warm p50 | 0.340 ms | 0.462 ms | 0.495 ms |
| warm p99 | 0.524 ms | 0.794 ms | 1.108 ms |
| cold p50 | 0.574 ms | 0.415 ms | 0.446 ms |
| cold p99 | 1.323 ms | 0.529 ms | 15.994 ms |
| single-file save (median) | 30.6 ms | 33.3 ms | 36.3 ms † |
| terms | 6,835 | 13,387 | 19,828 |

† This row is the only one in the ladder re-measured after the delta journal
landed, and it was taken on a much quieter host than the rest of the table
(load average 3 against 38–59), so it is not comparable to the rows above it in
absolute terms — only to itself across the three rungs, which is the point.
The pre-journal figures for the same operation, measured in the same run as
these, are 65.8 / 110.5 / 208.4 ms; the full matched A/B is in "Why the save
path had to change" below. What the row shows is that the three rungs are now
within 6 ms of each other across a 20x range of corpus.

**The knee is not below 20,000 documents.** Both candidates for one dissolve on
measurement:

- *The falling index/corpus ratio* (0.75 → 0.64 → 0.59) is fixed per-index
  overhead being amortised over more documents, not a per-document cost drifting
  down. The term count is nearly flat against a 25,893-word vocabulary, so the
  ratio is heading for an asymptote near the postings-to-corpus ratio. 0.59 is a
  lower bound on that asymptote, not a turning point.
- *The rising warm p50* (0.340 → 0.495 ms) is the load client's own round-trip
  floor, not the index. At 20,000 documents a 56-posting query answers in 0.402 ms
  and a 7,445-posting query in 0.609 ms.

**What actually rises with scale is p99** (0.524 → 0.794 → 1.108 ms), which
tracks index size: the working set stops fitting in cache. First
size-sensitive thing measured is p99, not p50.

**A cold p99 penalty appears only at 20,000** (15.99 ms cold against 1.108 ms
warm) and is a first-touch fault cost, not a per-query one — cold p50 is 0.446 ms,
indistinguishable from warm. Only the first few queries after a restart pay.

**Per-query cost by posting-list length, df read out of the index file** (not
inferred from the generator, and not from the API, whose `candidates` field caps
at 200): at 20,000 documents the tail query `baz` has df 56 and answers in
0.402 ms; the head query `kb` has df 7,445 and answers in 0.609 ms. Across the
whole index, postings per term run min 1, p50 4, p90 72, p99 876, max 17,129 of
20,000. A **133x span in posting-list length costs 1.5x in p50** — the bounded
top-k is doing its job, and that is why p50 is nearly flat across the ladder.

**RSS is dominated by the index itself.** From 5,000 documents up, the
file-backed half of RSS tracks the index size within 1 MB, and the anonymous
half grows more slowly. At 20,000 documents the index is 23.5 MB against 36 MB
resident, of which 20 MB IS the index.

**Ingest is linear with a large constant**, ~10.7 ms per document, flat to within
13% from 5,000 to 20,000. Nothing degrades per document across 20x more
documents.

## What was still not measured

- **Rust at scale.** The Rust daemon was never run at the 5,000-document scale
  ladder, so the ladder is kb-c alone.
- **A 100,000-document ingest.** Attempted. The corpus generated (100,000
  documents, 209 MB, 24 s) and the store phase completed (100,000 artifacts and
  2,045,462 chunks in a 590 MB database — 20.45 chunks per document, linear
  against the 20,000-document rung). The INDEX build did not finish: it spent
  2h19m of wall clock, of which 1m32s was CPU, blocked in the block layer
  (`blk_mq_get_tag`, `folio_wait_bit_common`, state D) pulling 979 MB off a
  shared RAID at 0.2-0.4 MB/s. System-wide iowait was 5-19% and other tenants
  were getting I/O through, so it is this device's request queue, not a global
  stall. That 2h19m measures shared storage under contention, not kb-c, and is
  deliberately NOT reported as an ingest number or extrapolated. The 20,000
  rung was re-ingested an hour later into a different sandbox and produced a
  BYTE-IDENTICAL index (24,674,872 bytes, 19,828 terms) in 195.6 s against the
  ladder's 213.7 s, which is the reproducibility check: the smaller rungs are
  sound and only the 100k wall clock is unusable.
- **The 20,000-row cliff** where brute-force cosine stops beating IVF-PQ.
- **Cold page cache** for query latency; all query figures are warm.
- **kb-c startup, cold** (no index on disk).

## Three defects these measurements found

All three were found by running the thing, not by reading it, and all three are
fixed with end-to-end proofs.

1. **The incremental path wrote an index its own loader rejected.** Two whole
   sections were declared in the header and never written, because an opened
   index keeps its arenas in the `mmap` while `save` wrote the empty heap
   copies. `save` now has a self-check that refuses to write a file its own
   loader would reject.
2. **A file save in a running daemon deleted the document.** The watcher
   published an absolute path, `app.c` treated it as corpus-relative, the file
   could not be resolved, and the event was recorded as a *removal* — sweeping
   the document out of the index on every save. This is what the 355 ms
   "one file changed" number above was actually measuring.
3. **The daemon double-freed its connection table under concurrent load.** The
   daemon keeps ONE array of live connections that every worker appends to, and
   the append grew that array with `realloc` **without** the mutex the close
   path and the SSE fan-out already take. Two workers accepting at the same
   instant both realloc'd the same block; one buffer was freed while
   connections were still being written into it, and the process died with
   glibc's `double free or corruption (!prev)`. It needs enough connections
   arriving at once to reallocate the array while two workers are inside it:
   the daemon first died in the load harness at 24+ concurrent connections,
   and **reproducing it on demand took 5 attempts**, because below 64
   connections the race essentially never fires. That is why neither the test
   suite nor the old python client ever saw it.

   Reproduced under ASan, which named the two stacks: `httpd_track`
   (`src/httpd.c`) reallocating the same block in three different worker
   threads. One mutex around the growth — the `conns_mu` the close path and
   the SSE fan-out already take — is the whole fix.

   | | before | after |
   |---|---|---|
   | 64 conns, ASan daemon | **daemon dead** (`attempting double-free`, level aborted) | 3/3 runs clean, 0 errors |
   | 32 / 48 / 64 conns, release | 0 errors (survived by luck of the timing) | 3/3 runs, 0 errors at every level |
   | new `concurrent_connects_do_not_corrupt_the_conn_table` under ASan | 5 of 10 runs abort | 10 of 10 clean |
   | the same test under TSan | 3 of 3 report the data race | 3 of 3 clean |

   The regression test hammers the real accept path — 32 threads released at
   one barrier against a 4-worker daemon, 4 waves of 40 connections each — and
   asserts what a user can see (every connection answered, the daemon still
   serving afterwards). It is a real race, so a green run is not proof the old
   code always failed; the sanitizer lanes are what make it decisive, and TSan
   is deterministic about it.

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
6. **Incremental reindex** — and this one is now measured rather than asserted.
   It used to be claimed here as "a file save costs that file, not the corpus".
   That is **false above 1,114 documents**: the save rewrote the whole index
   and fsync'd it, so the cost tracked the INDEX, not the file. The breakdown
   that disproves it, at 20,000 documents on real storage, is in the scale
   ladder above — 786.5 ms of which 508.1 ms is the fsync pair. See "Why the
   save path had to change" below for the fix and its new numbers.

Concurrent load and the vector lane are now measured above, and neither is
where this breaks. What is still unproven is **scale**: the ladder stops at
20,000 documents and locates no knee, so nothing here is a claim about 100k+.
Also unproven: the 20,000-row point where brute-force cosine stops being the
right answer, and any Rust comparison above that 360-document matched corpus.
