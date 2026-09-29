# kbcparity — the cross-implementation evidence layer

kb-c is a C17 port of the Rust `kb` daemon. Every other gate in this repo
compares kb-c against **itself**: its own unit tests, its own benchmarks,
its own invariants. Nothing here compared kb-c against the thing it is a
port of. `PORT_PLAN.md`'s stage 3 and stage 4 acceptance both hinge on a
cross-implementation diff, and that diff had never been executed — so
"matches the original" was a claim, not a measurement.

This is the measurement.

```
./tools/kbcparity/kbcparity verify        # re-hash the pinned inputs
./tools/kbcparity/kbcparity run           # both suites, both daemons
ctest --test-dir build -R kbc_parity      # the same thing, as a gate
```

## The document count is enumerated, and the counters are not

Both daemons publish a "how many documents do I hold" number, and
comparing those two numbers is the obvious thing to do. It is wrong, and
it was the reason this gate was a coin flip.

`GET /api/kb/{kb}/stats` → `doc_count` on the rust side is
`storage.count_rows()` — every physical row of that kb's Lance
`artifacts` table (`routes/stats.rs:182`, `storage/lance.rs:974`).
kb-c's `GET /api/health` → `docs` is `kbc_index_doc_count()` over the
live inverted index (`httpd.c:847`, `app.c:6934`, `index.c:2299`) — a
count of **distinct documents**. Two numbers that answer the question
differently are not a comparison.

Worse, the rust's number is not a document count at all. Measured on
2026-09-29 over this 21-file corpus, with the rust daemon alone:

| what the harness did | `doc_count` | documents served | rows in `artifacts.lance` |
|---|---|---|---|
| nothing (the daemon's own startup ingest) | 21 | 21 | 21 |
| `POST /reindex` while that startup ingest was still running | **32** | **32** | **32** |
| `POST /reindex` after it had settled | 21 | 21 | 21 |

The 32 is 21 documents of which **11 are stored twice** — the same
`id` and the same absolute `path` appear twice in the table, counted
directly in the Lance data fragment. The daemon already indexes its
whole source at startup, so a reindex POSTed into that ingest starts a
second walk that merges the same documents into the same table
concurrently; the loser of the race appends its rows beside the
winner's instead of merging them. Every reconcile pass still logs
`files=21 deletes=0`, because the files are all there — it is the rows
that are doubled. Whether the race is lost is a coin flip, which is why
the gate alternated between 21 and 32.

So the harness now:

- **does not** ask the rust daemon to reindex while it is still ingesting.
  Its own startup ingest is waited for (`--startup-timeout`, default 240s)
  — asking earlier is what provokes the merge race above;
- **does not** compare the two store counters with each other. They are
  printed, under their real names, as what they are;
- compares the number of documents each daemon actually **lists**,
  enumerated once from the same routes the rest of the suite reads;
- compares the **list row count** as well as the document count. Equal
  documents with unequal rows is the signature of a daemon listing a
  document it has already listed, and it is reported as
  `ingest/listed_rows` rather than collapsed away by keying on path.

The document count settling is **not** evidence that the graph has. The
edge hook resolves a document's outbound targets with
`get_by_source_paths` against the rows that exist *at that moment*
(`enrich.rs:996`), so on a cold store an edge whose target is indexed
after its source is dropped, or recorded against the wrong target, and no
amount of waiting fixes it — a later pass over a fully-written store is
what settles it. On this corpus the edge that goes wrong is
`ladder.md -> only-here.md`: missing (6 rust edges where kb-c has 8) or
pointed at `ops/Runbook.md` instead, depending on how the ingest batches
interleave. That is a defect in the daemon under test, not a difference
between the two implementations.

So the harness settles the graph the way it settles the document count:
it asks **both** daemons for another pass while the edge set keeps
CHANGING (`--graph-passes`, default 4), and compares the graph once it
stops. The trigger is "the graph moved", never "the two sides disagree" —
a difference a further pass would close is a difference this harness must
report, not one it may retry into agreement.

## What it measures

| suite | compares | surface |
|---|---|---|
| `capture` | the stored artefact, **byte for byte** | `GET /api/kb/{kb}/artifact/{id}` on both |
| `capture` | the front matter, **field by field**, on the values both sides publish | kb-c's `kbc_parsed_metas()` vs the rust **document row** |
| `capture` | title and summary, as the two daemons each derived them | the doc/artifact row vs the rust document row |
| `links` | the resolved edge set, per document and whole-graph | `GET /api/kb/{kb}/backlinks/{id}` on both, transposed |
| `links` | backlink count per document | the same route |
| `links` | the **resolution ladder's outcome per case** | `GET /api/kb/{kb}/notes/{id}/links` on both |
| `ingest` | how many **documents** each daemon lists, and how many list **rows** it took to say so | the list routes on both, enumerated once |

The out-edge set is read by transposing backlinks on BOTH sides rather than
by reading the graph out of each daemon's own table. Neither daemon exposes
the out-edges of an arbitrary document over HTTP — the Rust one has
`/kb/{kb}/edges` in artifact ids, and kb-c has no edges route at all — and
deriving the same quantity the same way on both sides is the only version of
this comparison that is actually a comparison.

The front matter is read off `GET /api/kb/{kb}/docs/{id}` — the
**document** row — and not off the list rows. The rust publishes two row
shapes and they are not the same shape: `/docs` gives a `DocResponse`
with `title`, `kb_category`, `created_unix`, `tags`, `kb_status`,
`kb_severity` and `summary`; `/notes` gives a `NoteSummary` with `title`,
`tags` and `status` and nothing else. The two list namespaces are
disjoint, so a document the rust calls a note is not in `/docs` at all —
reading its front matter off the note row made a document that publishes
all six fields look like one that publishes none of them.

Not every published field is a front-matter comparison, and the harness
says which is which per document. A value the rust fills in when the
document declares nothing — a title that falls back to the first H1 and
then to `<title>`, tags that fall back to the file's path, a
`created_unix` that falls back to the file's birth time — is a fact about
the checkout rather than a declaration in the document, and putting it
next to a front-matter key would be a category error. Those are reported
as STRUCTURAL, by name, every run. A field neither side publishes is
compared as "the document declares none" on both sides and counted
separately from a value comparison, so a document with no front matter at
all cannot make the pass total look like breadth it does not have.

## The inputs are pinned, and they are pinned for different reasons

- `corpus/` — a **verbatim** copy of the Rust project's own `corpus/canon/`
  plus a pinned slice of its Markdown. Other people's bytes: a rewritten
  byte is a different claim.
- `corpus/UPSTREAM` — the upstream commit, the git blob sha1 of every
  vendored file, and the upstream path it came from.
- `fixtures/ladder/` — **authored by this harness**, and it exists because
  the upstream corpus contains no ambiguous wikilink at all. Without it the
  ladder comparison has nothing to compare and reports a vacuous pass.
- `SHA256SUMS` in each — a mismatch is a **HARD STOP** (exit 3). Not a
  failure, not a warning: if the bytes under test are not the pinned bytes,
  every number the run produces is about a corpus nobody else has.

**The upstream repository ships no `SHA256SUMS` of any kind.** The task
brief said it did. It does not — there is no `SHA256SUMS*` or `*.sha256`
anywhere in the kb checkout outside `target/`. The pin here was taken at
vendor time from the read-only upstream tree and is anchored to git
(`UPSTREAM`) so it stays re-checkable against the original rather than
against this copy of it.

## The allowlist is the design

`divergences.v1.conf` is not a suppression list. It is the reason this
harness will still be believed in a month:

- Every entry carries a **reason** (`accepted-trade-off`,
  `known-parser-difference`, `unported-original-behaviour`) and the **date**
  a human agreed to it. An entry with no reason or no date fails to parse.
- Accepted divergences print in a **bucket of their own**, with their entry
  id, and are never counted in the pass total.
- A divergence with **no entry** is a FAILURE.
- An entry that matches **nothing** is reported STALE and **fails the run**.
  A list that has quietly grown to cover everything is the failure mode this
  file exists to stop, and the only way to notice it is a tripwire. The
  report prints each stale entry with its id, the suite, field and subject
  globs it was written to match, and its reason — "what it used to match" —
  and the run continues to the end. A harness that dies on the one run that
  has something important to say is a harness that gets switched off.
- The entry count is compared against the previous run and the growth is
  printed, so a case enters the list only by a visible, explicit edit.

## A run that did not happen is not a pass

| exit | meaning |
|---|---|
| 0 | the comparison ran; everything matched or was agreed |
| 1 | an unlisted divergence, or a stale allowlist entry |
| 2 | usage error |
| 3 | **HARD STOP** — a pinned input does not verify |
| 77 | **SKIPPED-BLOCKED** — no comparison was performed |

Exit 77 is the automake skip convention. `CMakeLists.txt` registers it as
`SKIP_RETURN_CODE`, so `ctest` prints `***Skipped` with the harness's own
explanation above it. A skipped gate that reports green is worse than no
gate: it retires the question. `tests/kbc_parity.c` propagates 77 unchanged
and never softens it into a pass.

`NOT MEASURED` is a fourth, distinct category, printed as such, and it is
**two** categories with different consequences:

| | meaning | fails the run? |
|---|---|---|
| **GAP** | this run *could* have made the comparison and did not — a precondition was missing | **yes** |
| **STRUCTURAL** | no surface either daemon implements can answer the question, on any flag | no |

A GAP is a hole in the evidence: the run has nothing to say about it in
either direction, so it fails, and it prints **what would make it**. The
remedy is the gap's own, so a run that already passed `--front-matter` can
never be told to pass it again. `--no-require-coverage` downgrades gaps to
warnings; it does nothing to structural ones, which were never failures.

A STRUCTURAL gap is a permanent coverage hole, and it is printed every run
with its count, its subjects and the reason. It does not fail the run
because a design that produces unmeasured comparisons and also fails on
them can never go green, and a gate that is always red is a gate nobody
reads. What it is *not* is a pass: an unmeasured comparison is never
counted in `PASS`, and `FIELD COVERAGE` prints how many documents each
field was actually compared on, so a field compared on 2 of 21 cannot be
read as one that agreed on 21.

The structural classes this corpus produces are: the rust notes ladder for
the 19 documents it does not call notes (that route 404s them), and the
front-matter fields whose only published value on the far side is a
fallback rather than a declaration — the derived `title`, the
path-derived `tags`, the filesystem-birth-time `created_unix`.

A fourth class the harness can report — a `kb-*` key kb-c's reader
extracted that the rust parses and stores without surfacing on any row —
does not occur here, because no document in the corpus declares a `kb-*`
key outside `kb-category` and `kb-tags`. It is named in `EXTRA_KEY_WHY` so
that a corpus which does declare one reports a surface gap rather than a
mismatch.

## `make kbc` does not build the daemon

**`make kbc` builds the LIBRARY.** The executable is the **`kbc_cli`**
target, which sets `OUTPUT_NAME=kbc` — so the library and the program are
both called `kbc`, `make kbc` prints `Built target kbc`, and the binary
under `build/` is left exactly where it was.

```sh
make kbc        # builds libkbc.a. The daemon is NOT rebuilt.
make kbc_cli    # builds build/kbc — this is the one the harness runs.
```

A parity run against that stale binary measures yesterday's kb-c and
reports it as today's, which reads as "the fix did not work" or "the
divergence came back". So the harness checks it: `preflight` compares the
mtime of `--kbc` against the newest file under `src/`, `include/`, `cli/`
and `CMakeLists.txt`, and a binary that is older is **SKIPPED-BLOCKED**
(exit 77) with the offending file named and the fix in the message. The
check is one directory walk and a `getmtime` — it costs nothing next to a
200-second run, and it is the difference between measuring the port and
measuring the last build of it.

## The one job `tests/kbc_parity.c` does that the harness cannot

The kb-c daemon publishes **no front matter over HTTP**: the metas are store
rows and no route reads them back. So the C ctest links `libkbc`, parses
every vendored corpus file with `kbc_parse()`, and writes the metas out as
JSON for the harness to diff against the Rust daemon's own derivation. That
is the front-matter column of the capture suite, and without it the suite
reports a coverage gap instead of a result.

## Knobs

```
--suite capture|links|all     which suite to run
--corpus DIR                   the pinned upstream corpus
--fixtures DIR                 the authored ladder fixtures
--allowlist PATH               the divergence list
--kbc PATH                     the kb-c binary (default ../../build/kbc)
--rust-kb PATH                 the Rust binary (default the kb checkout's)
--front-matter PATH            kb-c's own front matter, as JSON
--json PATH                    write the full result as JSON
--keep                         keep the sandbox and print where
--no-require-coverage          do not fail on a GAP. Structural gaps were
                               never failures and this does not change that
--repo-root DIR                the checkout --kbc was built from; its
                               newest source decides whether --kbc is stale
--build-dir DIR                the cmake build dir, named in the
                               stale-binary error
--ready-timeout / --ingest-timeout   seconds to wait for each phase
--startup-timeout            seconds the rust daemon's OWN startup ingest is
                              given to settle before a further pass is asked for
--graph-passes                settle passes the harness will ask BOTH daemons
                              for while the edge set keeps changing
```

## Cost

The Rust daemon loads its embedding model into a private XDG cache on every
run, so a cold `run` takes minutes. That is the price of a real comparison;
it is not a benchmark and should not be run in a tight loop.
