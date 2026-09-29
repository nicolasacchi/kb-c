# INVENTORY.md — module-by-module map of the Rust `kb` → C `kb-c`

Source of truth for LOC: `find /home/nik/project/kb/crates -name '*.rs' -not -path '*/tests/*' -not -path '*/benches/*' | xargs wc -l`.
Total: **510,455 LOC across 575 files in 8 crates.**

kb-c's side is the frozen contract in `include/kbc/*.h` (18 headers) and its
implementation, one file per subsystem (`src/<name>.c`, `cli/main.c`).
`src/` is the owner column below; where a responsibility moved rather than
ported, the reason says so. A kb-c module that owns no Rust row of its own —
`src/links.c`, `src/markdown.c`, the `chunk.h` contract — has its own section
near the end.

## Status legend

| Status | Meaning |
|---|---|
| **PORTED** | kb-c implements the behaviour; a C test exercises it. |
| **PARTIAL** | Implemented for the kb-c scope, deliberately narrower than the Rust original. |
| **PLANNED** | Named in a later stage of `PORT_PLAN.md`; not in kb-c yet. |
| **OUT-OF-SCOPE** | Not ported in this project at all, with the reason given. |

## Summary by status

In-scope crates — `kb-core`, `kb-server`, `kb-cli`, `kb-lip`, `kb-embedder`,
`kb-buildstamp` — are **231,379 LOC in 251 files** (plus 128 LOC of
`kb-core/benches/perf.rs`, listed as OUT-OF-SCOPE). Percentages are of that
231,507. Every figure below is the sum of the row statuses in this document,
recomputed from the rows on 2026-09-29.

| Status | Files | LOC | % |
|---|---:|---:|---:|
| PORTED | 33 | 11,357 | 4.91% |
| PARTIAL | 47 | 91,607 | 39.57% |
| PLANNED | 50 | 15,920 | 6.88% |
| OUT-OF-SCOPE | 122 | 112,623 | 48.65% |
| **total** | **252** | **231,507** | **100%** |

Plus, in the two `kb-code-*` crates: 34 module rows, 240,255 LOC, every one
OUT-OF-SCOPE (table below).

> These counts are the row statuses in this document, not a claim about what
> is finished. The port's live surface today is the PORTED + PARTIAL set:
> 102,964 LOC of Rust that kb-c claims, of which 11,357 is a row-for-row port
> and 91,607 is a deliberately narrower replacement. The bulk of the
> difference is `storage/sqlite.rs` and `storage/lance.rs`, whose C
> counterparts are `store.c` and `index.c` — a fifteen-data-table SQLite schema
> at v12 (plus the `schema_version` bookkeeping table) and a hand-rolled
> mmap'd index respectively, against 42 migrations and a columnar store with
> IVF-PQ.

## Out-of-scope crates: `kb-code-server`, `kb-code-cli`

34 module groups, 278 files, **240,255 LOC, all OUT-OF-SCOPE**, none of it
with a C owner. `module (files, LOC) — what it is`:

- `kb-code-cli/src/*` (11, 35,946) — the `kbc` CLI: lanes, boards, join,
  recipe, mirror, blame, security review
- `kb-code-server/src/*` (86, 84,324) — daemon core: state, dispatch, git
  plumbing, session diffing, provenance
- `…/store/*` (25, 18,223) — recipes, trails, tours, frameworks, entities,
  intel
- `…/doclens/*` (9, 8,954) — per-language document-length metrics
- `…/search/*` (9, 7,402) — code-lane search over a git checkout
- `…/review_doc/*` (7, 7,294) — review-document rendering
- `…/recipe/*` (8, 6,567) — recipe authoring/compilation
- `…/history/*` (12, 6,430) — repo history traversal
- `…/frameworks/rails/*` (11, 5,961) — Rails knowledge (tree-sitter)
- `…/boards/*` (6, 5,277) — boards/lanes data model
- `…/join/*` (7, 5,112) — multi-repo join
- `…/entities/*` (4, 5,071) — entity extraction (tree-sitter)
- `…/tree/*` (5, 3,759) — syntax tree walk
- `…/haml/*` (6, 3,281) — Haml parsing
- `…/comments/*` (5, 3,263) — review comments
- `…/lanes/*` (7, 2,907) and `…/lanes/adapters/*` (3, 734) — lane state
  machine and its adapters
- `…/agentview/*` (6, 2,500) — agent-facing read view
- `…/mirror/*` (4, 2,367) — remote mirror sync
- `…/behavioral/*` (3, 2,334) — behavioural test corpus
- `…/rails/*` (5, 2,237) — Rails heuristics
- `…/tours/*` (5, 2,130) — guided tours
- `…/trails/*` (3, 2,078) — trails
- `…/semantic/*` (5, 2,055) — code semantic search
- `…/git/*` (7, 1,930) — git object plumbing
- `…/provenance/*` (4, 1,846) — provenance graph
- `…/blame/*` (4, 1,820) — blame attribution
- `…/intel/*` (6, 1,796) — intel ingestion
- `…/transcripts/*` (4, 1,692) — transcript ingestion
- `…/security/*` (5, 1,597) — security review heuristics
- `…/sessiondiff/*` (3, 1,455) — session diffing
- `…/rekey/*` (2, 976) — re-key pass
- `…/prose_refs/*` (1, 532) — prose reference extraction
- `…/frameworks/*` (1, 405) — framework registry

**Why the whole subtree is out of scope for milestone 1** — the full decision, with alternatives and a revisit trigger, is [DECISIONS.md ADR-001](DECISIONS.md). In short:** `kb-code-server`
is a *different product* that happens to live in the same workspace: a daemon
over a git checkout whose entire value is language-aware structure —
tree-sitter grammars for 6+ languages, `frameworks/rails` heuristics, blame
and provenance, board/lane workflow. It is not a document index. `kb-c`
replaces the `kb` daemon (folders of HTML/Markdown → HTTP + CLI) and touches
neither a git object store nor a parser generator. The two share only a
workspace, not a data model: `kb` has artifacts, chunks and embeddings;
`kb-code` has repos, commits, recipes and lanes.

**What it would cost.** 240,255 LOC, of which the blockers are structural
rather than volumetric: tree-sitter has no C build that kb-c could link
without vendoring ~4 MB of generated parser C per language (Rust gets it from
a crate), the lane/board/recipe state machines are domain logic with no
testable analogue in kb-c, and the two daemons would need a shared IPC and a
shared auth model that neither has today. A faithful port is a second project
of roughly the same size as this one, not a stage of it. It stays in Rust.

## `kb-core` (87 files, 119,511 LOC) — the library both binaries sit on

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-core/src/storage/sqlite.rs` | 14,913 | SQLite open/pragmas, 42 embedded migrations, every side table's CRUD, `is_newest` collapse, cascade delete | PARTIAL | `src/store.c` — **twelve** migrations in refinery's shape (an ordered `(version, sql)` list, one transaction per version, the recorded version bumped INSIDE that transaction so a failure part-way leaves the last version that fully committed), and no version constant: `BINARY_EPOCH` is read off the last entry of the list so the two cannot drift. v1 (`schema_version`, `artifacts`, `chunks`, `comments`), v2 `edges`, v3 `pending_links`, v4 `doc_metas`, v5 `sources`+`index_runs`+`errors`, v6 `history`, v7 `corkboard`, v8 `pinned_memories`, v9 `excluded_files`, v10 `doc_first_seen`, v11 `moves`, v12 `moves.abandoned_at` — **fifteen data tables** at v12, plus the `schema_version` bookkeeping table. The Rust DDL is unchanged except for `source_slug`→`corpus`; the partial indexes are ported as partial (`idx_errors_open`, `idx_history_artifact_open`, `idx_history_search`), because each exists for exactly one query and restricting it to the rows that query can match is what turns a scan into a seek. **The epoch-ahead refusal is here** (`sibling.rs:119`): a volume whose `MAX(version)` is ahead of this binary's embedded set is refused BEFORE the migration run, so a refused boot never writes to a volume it cannot understand. **The move log and the rekey live on this row**, and a reader who goes looking for them on the `kb-core/src/relocate.rs` row — which still reads OUT-OF-SCOPE — will not find them: `kbc_store_rekey_artifact` rewrites every id-keyed row (`chunks`, `corkboard`, `pinned_memories`, `doc_first_seen`, `history`, `edges` and `pending_links` in both directions, then `comments`, then `artifacts` LAST) in ONE transaction under `PRAGMA defer_foreign_keys`, which is what breaks the circular deadlock the child FKs would otherwise cause. All three orderings are load-bearing, not tidy: deferring the FK check is the only way both updates apply, `artifacts` goes last because the delete-leftover step is an ON DELETE CASCADE parent delete that would take the children with it, and `comments` cannot follow the UPDATE-OR-IGNORE-then-delete pattern because the only public way to write one MINTS its `id` and `created_at` — an ordinary rekey would stamp every carried comment "now". `edges`/`pending_links` are rewritten with the row's OWN corpus as a predicate: unfiltered, a same-named path in a second corpus has its edge renamed onto a path that does not exist there, and the delete half then destroys the collision outright, both returning ok. `moves_lookup` is chain-walked (64 hops, cycles guarded) and `abandoned_at` is a third terminal state that is explicitly NOT a redirect. v12 is an ALTER rather than a fold into the unreleased v11 because the fold's safety condition is "no volume anywhere has recorded version 11", not "unreleased" — `migrate_locked` skips every version at or below what a volume has, so the fold's failure is a volume that claims to be current and then says "no such column". `identity_backfill_done` is deliberately NOT ported — it gates `Db::identity_backfill`, a per-user read/unread backfill over list entries, and kb-c has neither list entries nor per-user override state, so the marker would gate nothing: a table with no writer and no reader. The session/slate/atlas/recall/slo tables and the remaining 30 migrations stay out of scope |
| `kb-core/src/indexer.rs` | 9,028 | Watcher→queue→batch→parse→embed→write pipeline, dedup, quarantine, reconcile | PARTIAL | `src/app.c` — watch→queue→parse→id→upsert→index→embed, plus the **quarantine gate** (a document past `QUARANTINE_THRESHOLD = 3` stops being embedded and stays keyword-searchable, because the embedding column is nullable by design), the **reconcile delete pass** (a file that vanished since the last walk emits `watch.delete` even though its mtime never changed) and **delete-by-path**. The gate is keyed by `(path, CONTENT HASH)`, matching the original's `retry_count_for_path_hash`, and the error row is NOT cleared on a gated pass — clearing it un-gates the document, which then takes a real embed, fails, returns at count 1, and oscillates forever. The hash is load-bearing: editing a document is how an operator fixes one that failed. Quarantine is the `errors` row, not a `<state>/quarantine/<kb>/` directory. Still absent: the enrich tail, session substitution, and the batch drain (`reindex_one` still handles one file per event) |
| `kb-core/src/storage/actor.rs` | 7,205 | Tokio actor wrapping `Storage`, mpsc back-pressure for writers | PARTIAL | `src/store.c` — direct calls under a mutex, no actor; the batch boundary is the store transaction |
| `kb-core/src/storage/lance.rs` | 6,171 | Lance/Arrow columnar store: FTS indices, IVF-PQ, chunk kNN, compact, backup hook | PARTIAL | `src/index.c` + `src/embed.c` + `kbc_vecstore` — replaced, not ported; no columnar store, no IVF-PQ; the vector lane is a brute-force cosine scan over an mmap'd f32 matrix |
| `kb-core/src/sessions.rs` | 6,023 | Session transcript parsing, digest rendering, aggregation | OUT-OF-SCOPE | — |
| `kb-core/src/slate.rs` | 4,683 | Slate board model and engine | OUT-OF-SCOPE | — |
| `kb-core/src/config.rs` | 4,535 | Whole `kb.toml` schema, resolvers, `validate()`, save/preserve | PARTIAL | `src/config.c` — `[daemon] [search] [watcher] [embedder] [[corpus]]`, an unknown key is a parse error, and the non-loopback-without-token bind refusal lives here; `kbc_config_load_token` resolves the token once, from the literal value or the token file, and the daemon calls it before validate/bind — the token value never appears in `kbc config show`. No TOML re-emit |
| `kb-core/src/sessions/view.rs` | 4,441 | Session HTML rendering | OUT-OF-SCOPE | — |
| `kb-core/src/memory.rs` | 3,678 | Memory types, decay, salience, recall scoring | OUT-OF-SCOPE | — |
| `kb-core/src/share/mod.rs` | 3,268 | Share/deploy orchestration | OUT-OF-SCOPE | — |
| `kb-core/src/review.rs` | 3,169 | Review anchors, comments, verdicts, attachments | PARTIAL | `src/store.c` — comments and anchors; no verdicts, no attachments |
| `kb-core/src/enrich.rs` | 3,100 | 8 ordered post-index enrichment hooks | PARTIAL | `src/app.c` — `enrich_registry` (app.c:3285) registers **one** of the original's eight, `edge-record`, in the original's fifth slot; the other seven are named-and-absent with a reason each at app.c:3050-3072. What kb-c took whole is the DISCIPLINE, not the count: a hook is best-effort (`enrich.rs:14-24`) — its failure is logged by name and stepped over and must never fail the index — a cheap pre-filter runs first, run order IS registration order, and the store is the only writer (`include/kbc/app.h:243-266`). The seam `kbc_enrich_run` is in the frozen `app.h` and takes parallel arrays, so a caller never mirrors the private hook struct. NOT ported, and this is a narrowing rather than a gap: the seven absent hooks each serve a feature this document marks OUT-OF-SCOPE — `session-capture` (no sessions corpus), `memory-recall-ledger` and `memory-commit-ledger` (`memory.rs` is OUT-OF-SCOPE and no ledger table exists), `memory-link-seed` (`memory_links.rs`), `code-refs` (`coderefs.rs`), `snapshot-capture` (`versions.rs`), `list-anchor` (`lists.rs`). kb-c also enriches a BATCH, not a document, because it writes the link graph after the whole walk and an edge written per document would make the graph depend on walk order |
| `kb-core/src/atlas.rs` | 2,530 | UMAP/PCA embedding layout, clustering, snapshots | OUT-OF-SCOPE | — |
| `kb-core/src/parser.rs` | 2,029 | HTML/Markdown field extraction, frontmatter, capability flags | PARTIAL | `src/parse.c` — Markdown and HTML block extraction with stable element anchors, heading levels and a title fallback, and **frontmatter is read**: the `kb-*` keys of a LEADING `---` block become facets, in both the scalar (`kb-tags: a, b`) and block-list (`kb-tags:\n  - a`) forms. The fence predicate is `kbc_fm_fence` in the frozen `parse.h`, called by BOTH the facet extractor and the renderer — the second copy of that scan is what produced a facet-injection defect (a document opening with prose, mentioning `kb-tags:` anywhere and containing a later `---` had that facet **indexed** while the renderer treated the whole file as body, so `tag:x` matched a document that never declared the tag). No capability flags |
| `kb-core/src/lists.rs` | 1,927 | Lists and entries with dense positions | OUT-OF-SCOPE | — |
| `kb-core/src/coderefs.rs` | 1,791 | Code-reference extraction from doc prose | OUT-OF-SCOPE | — |
| `kb-core/src/capture.rs` | 1,585 | Capture pipeline: slug, stamp, atomic write, id | PARTIAL | `src/app.c` — write/stamp/id, no HTML sanitizer |
| `kb-core/src/relocate.rs` | 1,505 | Move log and path relocation | PARTIAL | `src/store.c` + `src/app.c` + the frozen `include/kbc/store.h`'s moves section — **the rekey half is ported, the file is not**. The `moves` table is migration 11 and `abandoned_at` is migration 12 (`ALTER`, not a fold into 11); `kbc_store_record_move` writes the intent BEFORE the rename and `kbc_store_complete_move` stamps it after, so an interrupted move is a listable row rather than a lost document. `kbc_store_rekey_artifact` re-keys every artifact-referencing row in ONE transaction, and `kbc_store_moves_lookup` is CHAIN-WALKED (bounded, cycle-guarded) so a stale id resolves to where the document is now; `kbc_store_list_incomplete_moves` feeds a bring-up pass in `kbc_app_open`, which warns and ABANDONS rather than re-running the rekey. `kbc_app_move_path` drives all of it. **Deliberately NOT ported: the delete-suppression guard.** The original holds an in-memory `PendingMoves` set plus a durable `moves_suppresses_delete` check so a rename cannot be read as a delete inside the watcher's debounce window; kb-c has no such table and does not need one, because the watcher's delete resolves against the STORE ROW for the path and the move holds `reindex_mu` across the rename and the rekey, so the window never opens — the argument is stated in `include/kbc/app.h`'s move contract. Its absence is a narrowing, not a missing capability, and it is why this row is PARTIAL rather than a whole-file port. Also not ported and not claimed: `relocate_folder` (a whole-folder rename) and `list_indexed_under_folder`, which have no counterpart in `src/` or `cli/`. The move has no verb and no route — see `PORT_PLAN.md` stage 4 |
| `kb-core/src/watcher.rs` | 1,390 | Filesystem watch, debounce, reconcile walk | PARTIAL | `src/watcher.c` (inotify) — per-path debounce/coalescing into one publish and a prompt, null-safe stop; no periodic reconcile walk |
| `kb-core/src/docs_query.rs` | 1,300 | In-memory filter evaluation over candidate rows | PARTIAL | `src/search.c` + `src/app.c` + `src/store.c` — the filter overlay is built. Facets come from the document's own markup (`<meta name="kb-tags" content="a, b">` and Markdown front matter), extracted into `doc_metas` (schema v4) and evaluated **once per query** as a membership set of index doc ids. `tag:`, `cap:`/`caps:` and `index:` are no longer HTTP 400s. A filter that matches nothing returns **zero** rows, never everything — that invariant is tested at the store, search and app layers. Value comparison is CASE-SENSITIVE, as in the original (`docs_query.rs:258` compares `String`s with `==`). `cap:` matches declared `kb-caps` metadata rather than the original's capability ANALYSIS (`svg_count`, `has_canvas`, `code_block_count`), which kb-c does not perform — a deliberate deviation |
| `kb-core/src/sessions/live.rs` | 1,256 | Live-transcript tailing | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/replay.rs` | 1,231 | Session replay export | OUT-OF-SCOPE | — |
| `kb-core/src/embed_ipc.rs` | 1,220 | NDJSON subprocess protocol to `kb-embedder`, handshake, timeouts, respawn | PORTED | `src/embed.c` — the protocol is now verified against the **production** `kb-embedder` with `bge-small-en-v1.5`, not a fake sidecar, and the header's earlier description of it was wrong in every respect: there is no health op, readiness is an unsolicited `{"kind":"ready"}` absorbed wherever it arrives, `req_id` is mandatory and must be echoed, replies are `kind=embed_ok` / `kind=error`, and the sidecar **exits on non-UTF-8 stdin** so no byte >= 0x80 may reach the wire (text is escaped to `\u00XX` locally) |
| `kb-core/src/session_render.rs` | 1,204 | Session digest renderer | OUT-OF-SCOPE | — |
| `kb-core/src/query.rs` | 1,188 | Query grammar: `AND`/`OR`/`NOT`, keys, DNF, `since:` | PARTIAL | `src/search.c` — the grammar as the index can answer it: `AND`/`OR`/`NOT`, groups, implicit AND, depth 64, the 64-conjunct DNF cap, `folder:` as a path-prefix facet, `text:`/`q:`, and the `tag:` / `cap:` / `caps:` / `index:` facet atoms, which are handed to the `docs_query.rs` overlay below rather than scored. **`since:` remains refused with HTTP 400** (its value grammar is parsed, `kbc_since_value_ns`, but the layer that would apply it does not exist) and **`scope:` remains refused**, because the original's `apply_atom` has an empty arm for it (`query.rs:403-406`) — the original evaluates it nowhere either. **There is no phrase search anywhere in the original either** — a quoted `"…"` in `query.rs` quotes an atom *value* (`folder:"deep notes"`), not a phrase, and the kb SEARCH path hands `?q=` **unparsed** to BM25, so the boolean structure never reached the lexical arm in Rust either |
| `kb-core/src/mentions.rs` | 1,108 | `@mention` resolution | OUT-OF-SCOPE | — |
| `kb-core/src/atlas_field.rs` | 1,097 | Atlas field values per artifact | OUT-OF-SCOPE | — |
| `kb-core/src/storage/schema.rs` | 988 | The 45-field `Doc` struct and its Arrow schema | PARTIAL | `src/types.h` — the 20 fields kb-c keeps |
| `kb-core/src/vcs.rs` | 956 | Git versions mode, snapshot capture | OUT-OF-SCOPE | — |
| `kb-core/src/embed.rs` | 935 | Model registry, batch caps, pending-query yield | PARTIAL | `src/embed.c` — registry names and the batch caps; no yield scheduler. The **query-embedding LRU** the original keeps in the server layer is here, with its contract in the frozen `include/kbc/embed.h`: the key is `(model, query)` and nothing else, and the look-up is a LINEAR SCAN because that is measured, not assumed — about 19 µs at capacity 1024, flat contention from 1 to 8 threads, against the 50–100 ms sidecar round trip the cache exists to avoid. If embedding ever gets a hundred times faster the trade flips, and the header says so so the decision gets revisited rather than inherited |
| `kb-core/src/slo.rs` | 895 | SLO indicator computation and snapshots | OUT-OF-SCOPE | — |
| `kb-core/src/markdown.rs` | 800 | Markdown → page HTML renderer | PARTIAL | `src/markdown.c` + the frozen `include/kbc/markdown.h` — **scoping this row by LOC hid a full Markdown engine**: the 800 lines here are a wrapper around comrak, and the grammar underneath is comrak's. The operator's decision (2026-09-28) is that kb-c takes **no third-party dependency**, so the renderer is hand-rolled and deliberately narrower: ATX/setext headings, nested lists, fenced and indented code, GFM tables, links, images, autolinks, emphasis/strong, strikithrough, task lists, hard breaks, HTML blocks, callouts and raw passthrough. NOT supported, each rendering as ordinary text: reference links and link reference definitions, footnotes, superscript, description lists, and entity references (`&copy;` passes through for the browser to resolve; numeric `&#169;` decodes). **The divergence is documented, not silent**: a five-lens campaign (differential against the real Rust, CommonMark/GFM conformance, security, robustness, mutation testing) took the supported-subset divergences from 2,940 to 8, and CommonMark semantic divergences from 257 to 120 of 652 spec examples, of which 75 are reference-link cases this row declares not supported. **It now HAS its production caller** — this row's earlier claim that nothing a user sees is rendered by it was stale: `render_markdown_page` is what both `GET /api/kb/{kb}/artifact/{id}` and the artifact subdomain serve for a `.md`, on both origins. A render FAILURE is a visible error and never a silent fall back to raw bytes, because a route that falls back passes every test whether or not the renderer works. `?download=1` is the deliberate exception and stays a download: the SOURCE, keeping the `.html` name rewrite as the original's wire contract rather than as a claim about the body |
| `kb-core/src/iframe.rs` | 724 | Artifact-subdomain host model | PORTED | `src/httpd.c` — one origin per artifact. A `Host:` that parses as an artifact subdomain routes to the artifact serve, anything else to the parent-origin static handler (`routes/dispatch.rs:46-50`), and both sit outside the token gate there and here: `Host:` is chosen by the client, so gating on it would be theatre. What protects the corpus is the bind guard and the token on `/api`; what the artifact surfaces carry instead is **origin isolation** — a sandbox CSP on the parent origin, a `frame-ancestors` CSP on the subdomain, and a different header set and order for each |
| `kb-core/src/headings.rs` | 706 | Heading tree extraction | PORTED | `src/parse.c` |
| `kb-core/src/meta_edit.rs` | 700 | `kb-*` frontmatter/meta rewriting | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/narrative.rs` | 674 | Session narrative assembly | OUT-OF-SCOPE | — |
| `kb-core/src/session_render_tests.rs` | 672 | Tests that live in `src/` | OUT-OF-SCOPE | — |
| `kb-core/src/links.rs` | 656 | Wikilink parsing and canonical-path resolution | PORTED | **Extraction and normalisation**: `src/parse.c` — the bare and the aliased wikilink forms go through the same normaliser as `[text](target)` and `<a href>`, so `[[y.md#part]]` and `[y](y.md#part)` produce the identical `y.md`; a bare `[[target]]` takes the target as its visible text, matching `links.rs`, which collapses label == url to `alias: None`; `![[embed]]` is an image, not a link. **The four-tier ladder**: `src/links.c` + the frozen `include/kbc/links.h`, a leaf with no I/O, no clock and no map iteration — the ingest path builds ONE `kbc_resolve_index` over the corpus's candidates and resolves every target against it, because building it per target is the O(docs) scan the original removed. Tiers in order: **id** (a 12-hex document id), **path** (exact, or with the final extension elided, case-sensitive), **title** (case-insensitive exact), **basename** (case-insensitive, with or without extension). Tiers 1–2 cannot be ambiguous; tiers 3–4 can, and then the answer is `KBC_RESOLVE_AMBIGUOUS` and never "the first one" — picking by index order would be nondeterministic and would flip on the next reindex. An ambiguous target is **counted, not written**: no edge, and not a pending row either, because the corpus did not say which document it meant. A document naming itself is counted as a self-link rather than a backlink. `tests/test_links.c` covers the ladder's 23 behaviours, including the extension-elided collision, the first-wins order contract and both ambiguity outcomes |
| `kb-core/src/events.rs` | 652 | Event bus: ring, broadcast, filters, replay | PARTIAL | `src/httpd.c` — a 256-entry ring, `Last-Event-ID` replay and an SSE stream; no per-subscriber filters. **The cold-replay gap probe is in**: a `Last-Event-ID` the ring cannot honour is answered with a synthetic `event: gap` frame carrying no `id:`, and the replay it cannot justify is suppressed — deterministically tested. The `lag` half (a queue overflow reported to the client as `{"skipped":N}`, coalesced to one frame ahead of the frame that caused the overflow) **ships with no test at any layer**, which is a decision and not an accident: DECISIONS.md ADR-007 records the declined option — bounding the SSE send buffer, which would make the path deterministically testable and is defensible on its own merits — and the measurement behind the decline: 4 MiB pushed at a client reading not one byte still failed 2 runs in 6, because the server's kernel send buffer absorbs everything past `sse_pump`'s 256 KiB high-water mark, so the overflow is a coin flip rather than a difficulty to tune away. A test that fails on a correct build is worse than no test |
| `kb-core/src/share/cloudflare.rs` | 651 | Cloudflare Pages/Access deploy | OUT-OF-SCOPE | — |
| `kb-core/src/paths.rs` | 630 | State/config/cache roots, rel-path derivation | PORTED | `src/mem.c` + `src/config.c` |
| `kb-core/src/triage.rs` | 619 | Memory triage heuristics | OUT-OF-SCOPE | — |
| `kb-core/src/storage/backup.rs` | 617 | `VACUUM INTO` snapshot + tar bundle | PORTED | `cli/main.c` — `kbc backup <kb> [--out PATH] [--all]`. **Why `VACUUM INTO` and not a file copy**: tar-ing the live database while the daemon writes captures a torn, half-applied transaction; `VACUUM INTO` reads a transactionally consistent view under WAL and writes a fresh standalone database with no `-wal`/`-shm` sidecars to reconcile. **Three deliberate divergences from the Rust, each with a reason.** (1) The destination is a BOUND PARAMETER, not a literal with doubled apostrophes: kb-c links sqlite 3.27+, where the argument is an ordinary expression that takes a bound parameter, and rule 9 says all SQL goes through bound parameters. (2) `--` always precedes the member list, because a corpus name beginning with `-` is otherwise read by tar as an option. (3) The source is opened **READONLY**: rusqlite's `Connection::open` is `SQLITE_OPEN_CREATE`, so a mistyped source path silently creates an empty database and the backup "succeeds" with nothing in it — read-only fails loudly instead. A failed backup leaves nothing behind: the staging tree and any partial tarball are removed on both paths, because a half-written tarball that looks like a backup is one artefact worse than no backup at all. **What the tarball carries is the store, not the index**: `<kb>/index.db` is the snapshot, and the `lance/`, `.review/` and `slates/` clauses fire only for directories the Rust daemon writes and kb-c does not. A restored kb-c therefore has no index, and `kbc reindex` rebuilds it |
| `kb-core/src/procrustes.rs` | 614 | Recall-fidelity measurement | OUT-OF-SCOPE | — |
| `kb-core/src/metrics.rs` | 610 | Counters, histograms, `/metrics` text | PORTED | `src/httpd.c` — six scalars, per-route series, and fixed-boundary latency histograms rendered as exact rational quantiles (`ceil` is not available without libm and 0.95 is not representable anyway), in the original's hand-written order, because a scrape diffs by line. Two families are honest zeros rather than placeholders: kb-c has no storage actor, so channel depth and capacity are 0, which is what the exposition format's own answer looks like for a family that has observed nothing |
| `kb-core/src/sessions/tail.rs` | 570 | Live transcript tail reader | OUT-OF-SCOPE | — |
| `kb-core/src/fusion.rs` | 567 | RRF fusion, title boost, graph boost | PARTIAL | `src/search.c` — RRF with `rrf_k` defaulting to 60 and dedup on id, plus the **title boost**: `TITLE_BOOST = 0.5` (`fusion.rs:145`, `apply_title_boost` at `:155`), terms >= 3 bytes, ASCII-lowercased, **substring** match not word-boundary (the original's own comment records that a word-boundary variant bench-measured worse), multiplied **once** per hit however many terms matched, applied to the **fused** RRF score over the full pool before filtering and truncation. The **graph boost is ported** (`fusion.rs:214` `apply_graph_boost`): in-degree is read from the `edges` table (schema v2, present since the graph work landed), and the boost adds `weight / rrf_k * sqrt(in)/sqrt(in_max)` to each row's fused score. It ships **disabled** — `graph_boost` defaults to `0.0` and only a weight in (0, 4] turns it on, because the weight is unmeasured; see DECISIONS.md ADR-004. The graph it reads now converges, which is what removed the first reason for shipping it off |
| `kb-core/src/versions.rs` | 545 | Version timeline reads | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/opencode.rs` | 545 | opencode live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/notes.rs` | 535 | Notes model | OUT-OF-SCOPE | — |
| `kb-core/src/history.rs` | 526 | Reading/search history rows | PARTIAL | `src/store.c` — the polymorphic `history` table (schema v6: `open`/`search`/`comment`, with the `CHECK` that makes "one of three kinds" an invariant of the table rather than of every caller) and `kbc_store_add_history`, tested at the store layer. **No surface records a visit**: `kbc cat` does not exist, and no route writes a row, so a kb-c daemon writes no history. The table is where stage 3's `cat` and history routes will write, and the columns are the original's, including the `''` "pre-multi-user row" marker upstream uses for the identity backfill |
| `kb-core/src/atlas_labels.rs` | 515 | c-TF-IDF cluster labels | OUT-OF-SCOPE | — |
| `kb-core/src/reading.rs` | 509 | Reading-progress sections | OUT-OF-SCOPE | — |
| `kb-core/src/session_bundle.rs` | 505 | Session bundle export/import | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/kimi.rs` | 503 | kimi live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/grok.rs` | 488 | grok live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/codex.rs` | 457 | codex live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/resurface.rs` | 455 | Resurface queue scoring | OUT-OF-SCOPE | — |
| `kb-core/src/attachments.rs` | 451 | Attachment blobs and manifest | PLANNED | `src/httpd.c` (stage 5) |
| `kb-core/src/webhook_url.rs` | 440 | Webhook URL validation incl. SSRF rules | PLANNED | `src/config.c` (stage 5) |
| `kb-core/src/session_scrub.rs` | 436 | Session scrubbing/anonymisation | OUT-OF-SCOPE | — |
| `kb-core/src/tracing_init.rs` | 423 | `tracing` subscriber setup | PARTIAL | `src/log.c` — levels and JSON lines, no spans |
| `kb-core/src/anchors.rs` | 390 | Anchor ids and stale/resolved detection | PORTED | `src/parse.c` |
| `kb-core/src/graph_report.rs` | 382 | Link graph report | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/projects.rs` | 336 | Project-key inference | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/mod.rs` | 336 | Live adapter registry | OUT-OF-SCOPE | — |
| `kb-core/src/cascade.rs` | 335 | Delete cascade across every side table | PARTIAL | `src/app.c`'s `store_forget_path` — one removal, reached by the watcher's delete event, the reconcile sweep and `kbc_app_delete_path` alike. **GOES, with the document:** `edges` (outbound deleted, inbound demoted to `pending_links`, so a document that comes back finds its backlinks), `doc_metas`, `artifacts`, and `chunks`/`comments` by the foreign key. **STAYS, deliberately:** `history`, `corkboard`, `pinned_memories`, `reading_sections`. A comment is a reply to something the document said; reading history is the fact that the user read it and a pin is a decision they made about it. None of those can be reconstructed from the bytes, and none becomes false because the bytes are gone — the day the document comes back, its history is still theirs. The cascade across the session/slate/atlas/recall tables is out of scope with them |
| `kb-core/src/share/github.rs` | 331 | GitHub Pages deploy | OUT-OF-SCOPE | — |
| `kb-core/src/exclusions.rs` | 302 | Excluded-path gate | PORTED | `src/config.c` |
| `kb-core/src/ids.rs` | 294 | `hash12`, `SourceSlug`, Crockford `r-`/`e-` ids | PARTIAL | `src/ids.c` — the 12-hex artifact id (FNV-1a over corpus ⨯ 0x1f ⨯ path) plus its validator; no `hash12`, `SourceSlug` or Crockford ids |
| `kb-core/src/extmap.rs` | 268 | Extension → pipeline map | PORTED | `src/config.c` |
| `kb-core/src/identity.rs` | 258 | Operator name normalisation and validation | PORTED | `src/httpd.c` — `GET /api/identity` reports the resolved identity, its source, the client address and `loopback`, and refuses a caller that tries to claim one (`?as=`, `?identity=`, `?user=`). There is one trust tier: identity is attribution, not authorization, and the response says so in its own body |
| `kb-core/src/scrub.rs` | 246 | Redaction pattern application | OUT-OF-SCOPE | — |
| `kb-core/src/share/host.rs` | 245 | Share host routing | PLANNED | `src/httpd.c` (stage 5) |
| `kb-core/src/sibling.rs` | 224 | Schema-epoch volume-ahead refusal | PORTED | `src/store.c` — the guard runs on a freshly-opened connection BEFORE the migration pass, so a refused boot never writes to a volume it cannot understand. The condition is `volume > binary`: a volume AHEAD has been forward-migrated by a newer binary, and one BEHIND is what the forward-only migration list exists to handle, so a refusal here would be refusing the ordinary case. Both epochs are named in the message, because an operator has to be able to tell which binary to run. A read failure against the migration-history table is NOT a refusal — it falls through to the migration run, which surfaces the real error on the same connection. (This row's reference in `PORT_PLAN.md` was `storage/sibling.rs:224`; the file is `kb-core/src/sibling.rs` and the guard is at line 119) |
| `kb-core/src/chunk.rs` | 184 | Deterministic word-window chunking | PORTED | `include/kbc/chunk.h` (frozen) + `src/app.c` — exact: whitespace-delimited WORDS, a 280-word window with 60-word overlap, chunk 0 the title passage rather than a window, and a 512-chunk cap that keeps the FIRST 512 and reports the ORIGINAL count, because a caller logging "N chunks" must report what the document produced. The step is `chunk_words - overlap_words` floored at 1, so a degenerate overlap still terminates. Chunk 0 is emitted only when the title/heading passage is non-empty, so a body-only document starts at its first body window with no empty leading chunk. All eight of the Rust's tests are ported — seven under the Rust's own name, and `is_deterministic` as `chunking_is_deterministic` — and the test no longer MIRRORS the structs with `sizeof` asserts: that pins layout rather than behaviour and can drift, so the mirror is a compile error instead |
| `kb-core/src/types.rs` | 179 | `KbName`, `ArtifactId`, `EventEnvelope`, enums | PARTIAL | `src/types.h` |
| `kb-core/src/sessions/constants.rs` | 171 | Session constants | OUT-OF-SCOPE | — |
| `kb-core/src/error.rs` | 167 | `Error` → status/URN-kind mapping | PORTED | `src/kbc.c` — `kbc_status` enum |
| `kb-core/src/test_support.rs` | 133 | Test fixtures | OUT-OF-SCOPE | — |
| `kb-core/src/share/assets.rs` | 125 | Share asset bundling | OUT-OF-SCOPE | — |
| `kb-core/src/timeparse.rs` | 103 | Date/time parsing | PLANNED | `src/mem.c` (stage 3) |
| `kb-core/src/strutil.rs` | 99 | String helpers | PORTED | `src/mem.c` |
| `kb-core/src/corkboard.rs` | 95 | Pin board | OUT-OF-SCOPE | — |
| `kb-core/src/fsx.rs` | 81 | Atomic write, mkdir -p | PORTED | `src/mem.c` |
| `kb-core/src/lib.rs` | 72 | Crate root and re-exports | PORTED | `include/kbc/kbc.h` |
| `kb-core/src/storage/mod.rs` | 11 | `Storage` enum | PORTED | `src/store.c` |
| `kb-core/benches/perf.rs` | 128 | Criterion benches (excluded from the LOC total above) | OUT-OF-SCOPE | — |

## `kb-server` (77 files, 58,636 LOC) — the axum daemon

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-server/src/routes/sessions.rs` | 8,070 | 30+ session routes, presence, live lane | OUT-OF-SCOPE | — |
| `kb-server/src/routes/memory.rs` | 4,832 | Memory write/read/recall routes | OUT-OF-SCOPE | — |
| `kb-server/src/lib.rs` | 3,048 | Listener bind, task spawn, startup refusal | PARTIAL | `src/httpd.c` — bind plus one SO_REUSEPORT socket per worker; the non-loopback-without-token refusal is in `config.c`; no task spawn, no startup check |
| `kb-server/src/routes/search.rs` | 2,633 | `/search`: pool sizing, arms, fusion site, `Hit` shape, rerank | PARTIAL | `src/httpd.c` + `src/search.c` — `GET /api/search` with the keyword and vector arms, the RRF fusion site and a `results[]` shape; no pool sizing, no rerank |
| `kb-server/src/routes/slates.rs` | 2,243 | Slate routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/comments.rs` | 2,231 | Comment CRUD, reanchor, upload | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/atlas.rs` | 2,213 | Atlas recompute/recluster/history | OUT-OF-SCOPE | — |
| `kb-server/src/routes/docs.rs` | 2,130 | `/docs` listing and artifact serving with CSP/nosniff | PARTIAL | `src/httpd.c` — **the artifact serve is in**: `GET /api/kb/{kb}/artifact/{id}` serves the bytes inline on the trusted app origin with `Content-Security-Policy: sandbox` (literally that, nothing else — a `default-src` or a `frame-ancestors` added to it would be a different policy), `X-Content-Type-Options: nosniff` and `X-Kb-Artifact-Id`, in the original's header order, plus `?download=1`. Without both headers a hostile artifact navigated to here would execute in the app origin: a stored-XSS and sandbox-bypass hole the moment a corpus can hold untrusted content. **A `.md` is served as the RENDERED PAGE, not its source** — this row used to say the opposite, and `src/markdown.c` has its production caller now. The title the two JSON surfaces report for a `.md` is therefore the RENDERER's, not the store's, and the fixture makes the two provably different (frontmatter `title:` above an `h1`) because a document where they agree proves nothing. `?download=1` is the deliberate exception: it serves the SOURCE and keeps the `.html` name rewrite as the original's wire contract. A `.md` that is not valid UTF-8 is 400 rather than raw source under an HTML label, and a failed render is a visible error rather than a raw serve — a route that falls back passes every test whether or not the renderer works. The `/docs` LISTING does not exist |
| `kb-server/src/routes/context.rs` | 1,707 | Context bundle assembly | OUT-OF-SCOPE | — |
| `kb-server/src/routes/daycard.rs` | 1,672 | Daycard HTML/JSON | OUT-OF-SCOPE | — |
| `kb-server/src/state.rs` | 1,599 | `KbHandles`, per-kb state, caches | PLANNED | `src/app.c` (stage 3) |
| `kb-server/src/middleware.rs` | 1,511 | Bearer auth, identity ladder, CORS, origin allowlist, rate limit, loopback | PARTIAL | `src/httpd.c` — constant-time `Authorization: Bearer` on every `/api` route except `/api/health`, the loopback bind refusal, and a `kbc_config_load_token` that resolves the token once from the literal value or the token file, so a non-loopback bind is possible at all (nothing set `cfg->token` before, so the bearer tier was dead code and a non-loopback bind was ALWAYS refused). **`X-Kb-Token` is the second carrier**, parsed by its own rules (a blank one is ABSENT and gets 401, not the 403 a wrong secret gets). The two are NOT required to agree and disagreement is NOT an error: the original takes candidates in the fixed order `[bearer, x_kb_token]` and returns the FIRST that matches, with no comparison anywhere, so `Authorization` silently outranks `X-Kb-Token` — inherited, not endorsed, and `/api/identity` now reports which carrier decided and declares `Vary: Authorization, X-Kb-Token`, because a silent preference is otherwise invisible and a shared cache would hand one caller's attribution to another. CORS is same-origin by default and reflects an `Origin` **only** on an exact allowlist match (`KBC_CORS_ORIGINS`, no wildcard anywhere); rate limiting is a per-connection fixed window, 120 req/s by default (`KBC_RATE_LIMIT_RPS`), 429 + `Retry-After`, and it exists to protect worker fairness rather than the corpus. No multi-tier identity ladder |
| `kb-server/src/routes/artifact.rs` | 1,367 | Artifact-subdomain serving with traversal guard | PARTIAL | `src/httpd.c` — the subdomain host model is in (see `kb-core/src/iframe.rs`), with a **different** header set from the parent origin: `frame-ancestors` instead of `sandbox`, and the order rearranged. **The traversal guard is the component-wise one**: the original's check is `Path::starts_with`, which is COMPONENT-WISE, so `/root/data` is a prefix of `/root/data/x` and NOT of `/root/data-secret`. A C port writing `strncmp(path, root, strlen(root)) == 0` would serve the sibling directory's bytes, and a `../` test does not catch that — `strncmp` rejects `../` too. `path_within()` walks both paths component by component and `realpath(3)` resolves `..` and symlinks BEFORE the comparison, so the guard runs on canonical paths only. `tests/test_httpd.c::no_path_outside_the_source_root_is_ever_served` pins the sibling directory and four other cases, with a control case that an in-root asset IS served, so a guard that refused everything could not pass |
| `kb-server/src/routes/lists.rs` | 1,203 | List routes | OUT-OF-SCOPE | — |
| `kb-server/src/router.rs` | 1,029 | 226 route registrations, layer order, fallbacks | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/echoes.rs` | 997 | Echo/notebook routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/lookup.rs` | 937 | Lookup by id/path/title | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/artifacts.rs` | 935 | Artifact list/paging | PARTIAL | `src/httpd.c` — `GET /api/artifacts` with `kb`, `kind`, `limit`, `offset`; no filter grammar |
| `kb-server/src/routes/history.rs` | 820 | History write/read | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/notes.rs` | 813 | Notes routes | PARTIAL | `src/httpd.c` — **one handler**, and it is the same handler the `routes/links.rs` row below credits: `GET /api/kb/{kb}/notes/{id}/links`, registered in the exported `KBC_ROUTES` table (httpd.c:6547) and implemented at httpd.c:2021 — one note's outgoing wikilinks carrying state `resolved`/`ambiguous`/`dangling`, plus its backlinks. **One deliberate divergence**: `note_row` asserts `notes::is_note` before it will answer (`notes.rs:305-317`), so an artifact id is a 404 there; kb-c cannot make that assertion, because its ingest assigns `KBC_KIND_ARTIFACT` to every document unconditionally (`app.c:789`) and asserting the stored kind would 404 the whole corpus. The row's `is_note` still reports the stored kind, so the assertion is a single `if` away the day ingest starts minting notes. Not ported: every other notes route — create, edit, delete, today, daycard — and none of them is in `KBC_ROUTES`. `kb-core/src/notes.rs` (the note model itself: `slugify`, `compose_note_source`, `split_note_source`, `checklist_counts`) is a separate row and is still OUT-OF-SCOPE; this row is the serving surface, not the model |
| `kb-server/src/routes/proposals.rs` | 747 | Proposals | OUT-OF-SCOPE | — |
| `kb-server/src/routes/links.rs` | 747 | Link routes | PARTIAL | `src/httpd.c` — three of the five handlers, wire shapes field for field: `GET /api/kb/{kb}/backlinks/{id}` (nothing linking here is an **empty array, not a 404**), `GET /api/kb/{kb}/wikilinks/suggest` (`?q=` required, a title prefix outranks a substring outranks a basename, `?limit=` clamped to 50) and the note-links surface at `GET /api/kb/{kb}/notes/{id}/links`, whose outgoing rows carry `state` `resolved`/`ambiguous`/`dangling`. **The ladder's AMBIGUOUS answer discards its candidate ids on the wire**, because the original does, and the test asserts the whole row so a build that smuggles them through any member fails. NOT ported: `links suggest` and `links apply` — both are thin handlers over `kb_core::mentions` (`suggest_unlinked` calls `find_mentions` and reads `MIN_MENTION_LEN` directly), and that module is OUT-OF-SCOPE below, so there is nothing to port them *from* |
| `kb-server/src/routes/metrics.rs` | 736 | Prometheus text endpoint | PORTED | `src/httpd.c` — `GET /metrics`, text exposition 0.0.4, always 200, always `no-store`. It sits outside the `/api` tree, so it is neither counted nor rate-limited as an API request, which is the placement the original has. Tested: the exposition shape, that served `/api` requests are counted, and that the detailed layer appears when enabled. **A client of it is in the tree now too** — see the `kb-cli/src/commands/metrics.rs` row |
| `kb-server/src/embed_cache.rs` | 681 | Query-embedding cache (`embed_ms`, `cache_hit`) | PORTED | `src/embed.c` + the frozen `include/kbc/embed.h` — an LRU keyed by `(model, query)`, capacity 1024 by default and never exceeded. `embed_ms` times the EMBEDDING STEP alone and is exactly 0 on a hit, because on a hit there is no embedding step to time and timing the whole call would make the field say nothing about whether inference happened. One instance per daemon reproduces the original's process-wide behaviour without a global (rule 5), and sharing it across embedders on different models is safe precisely because the model is in the key. Tested: a second identical query never reaches the sidecar, a dead sidecar keeps the cache and stores nothing new, a populated cache is released at close, and concurrent queries share one cache with counters that add up |
| `kb-server/src/routes/attachments.rs` | 670 | Attachment blob serving | PLANNED | `src/httpd.c` (stage 5) |
| `kb-server/src/routes/coderefs.rs` | 642 | Code-ref routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/share.rs` | 639 | Share routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/timeline.rs` | 637 | Timeline lanes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/schema.rs` | 606 | Event schema endpoint + 100-type catalogue + drift test | PLANNED | `src/httpd.c` (stage 3) — the endpoint does not exist; only the event envelope is defined in the frozen headers |
| `kb-server/src/routes/desk.rs` | 592 | Desk handoff routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/capture.rs` | 533 | Multipart capture, desk, content PUT | PARTIAL | `src/httpd.c` + `src/app.c` — `POST /api/kb/{kb}/capture`, multipart, **201** with the created ids and source-relative paths in request order (not a directory listing's order). Both caps are enforced and both REFUSE rather than truncate: at most 50 files, 10 MiB per file, 64 MiB for the request as received. Only `.md`/`.markdown` is accepted (415 otherwise) — the HTML pipeline stamps `<meta>` into `<head>` and runs ammonia, and kb-c has no sanitiser, so a head-splice without one is a way to persist attacker markup into a trusted origin. The **frontmatter key ORDER is a byte-compatibility contract**, not a style choice, and the order is the order of *first insertion*: the seven keys go through the original's own line-oriented setter, which edits a key the document already carries **in place** and appends only the ones it lacks, so a document that arrived with `kb-category` set keeps it first and grows the rest after it. A port implementing the list as a strict order passes every shape assertion and fails every byte-diff. `title` is never stamped — it steers the output filename and nothing else. The 60-char slug policy is ported. **The URL is stored and never dereferenced** (the project's SSRF ruling), and that is a test binding a real listener and asserting nothing connects, not a comment. The write direction has its own traversal test and it is the SIBLING case (`<root>/kb` vs `<root>/kb-secret`): a `../` test passes under both a `strncmp` guard and the component-wise one and separates nothing. NOT ported: the `POST /capture` Web Share Target (303 + `Location`), the desk surface and the content `PUT` |
| `kb-server/src/routes/spa.rs` | 530 | Static SPA asset serving | PARTIAL | `src/httpd.c` — the parent-origin static handler, for every path no route claimed, behind the same component-wise root guard as the artifact surfaces. `KB_SPA_DIST` is the root and it must hold an `index.html` or the daemon **refuses to start** and says why, rather than running with no root and 404ing everything. Cache-Control follows the original's policy (no-cache for a webmanifest, one year immutable under `/assets`, an hour otherwise), and a client-routed `/a/…` URL still gets the shell. kb-c has no SPA to serve and no per-artifact OpenGraph splice, so the shell is the whole of that branch |
| `kb-server/src/routes/turn.rs` | 488 | Turn routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/versions.rs` | 484 | Version routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/events.rs` | 429 | SSE endpoint, keep-alive, lag/gap frames, filters | PARTIAL | `src/httpd.c` — `GET /api/events` with keep-alive and `Last-Event-ID` replay. **The `gap` frame is in and deterministically tested**; the `lag` frame ships untested by decision (DECISIONS.md ADR-007, and the `kb-core/src/events.rs` row above). No per-subscriber filters |
| `kb-server/src/routes/resurface.rs` | 422 | Resurface queue | OUT-OF-SCOPE | — |
| `kb-server/src/routes/atlas_field.rs` | 383 | Atlas field routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/graph.rs` | 352 | Link graph | OUT-OF-SCOPE | — |
| `kb-server/src/live_registry.rs` | 327 | Live-session registry | OUT-OF-SCOPE | — |
| `kb-server/src/routes/config.rs` | 317 | Config read/validate | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/mod.rs` | 315 | Shared helpers, `is_safe_id`, fanout cap | PARTIAL | `src/httpd.c` — an id syntax guard on `/api/artifacts/{id}`; no `is_safe_id` table, no fanout cap |
| `kb-server/src/routes/boards.rs` | 291 | Boards | OUT-OF-SCOPE | — |
| `kb-server/src/routes/anchors.rs` | 289 | Anchor routes | PLANNED | `src/httpd.c` (stage 4) — **the events are in, the three routes are not.** `comment.anchor_stale` / `comment.anchor_resolved` fire from the re-index pass (`anchors_one_doc`) and fire on the TRANSITION and only on the transition: the stale set is re-checked and emits nothing, so event volume is bounded by transitions rather than by reindexes. Still missing: the cross-kb corkboard list, pin, unpin and `GET /anchors/stale`. The `corkboard` table and its store calls are in; the routes over them are not, and no entry in `KBC_ROUTES` names `anchors` |
| `kb-server/src/routes/memory_links.rs` | 270 | Memory link routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/inbox.rs` | 265 | Inbox | OUT-OF-SCOPE | — |
| `kb-server/src/routes/slo.rs` | 261 | SLO routes | OUT-OF-SCOPE | — |
| `kb-server/src/live_tail_cache.rs` | 250 | Live tail cache | OUT-OF-SCOPE | — |
| `kb-server/src/routes/relocate.rs` | 225 | Move routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/quarantine.rs` | 218 | Quarantine routes | PLANNED | `src/httpd.c` (stage 4) |
| `kb-server/src/routes/prompt.rs` | 215 | Outbound prompt template + redaction floor | OUT-OF-SCOPE | — |
| `kb-server/src/slate_registry.rs` | 214 | Slate registry | OUT-OF-SCOPE | — |
| `kb-server/src/routes/stats.rs` | 211 | Stats | PLANNED | `src/httpd.c` (stage 5) |
| `kb-server/src/routes/download.rs` | 207 | Download via the index's own path | PLANNED | `src/httpd.c` (stage 5) |
| `kb-server/src/replay_cache.rs` | 188 | SSE replay cache | PARTIAL | `src/httpd.c` — the 256-entry in-process ring `Last-Event-ID` replays from; nothing survives a restart |
| `kb-server/src/routes/exclusions.rs` | 179 | Exclusion routes | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/review.rs` | 173 | Review verdict routes | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/touches_cache.rs` | 164 | Touch-event cache | OUT-OF-SCOPE | — |
| `kb-server/src/scrub.rs` | 148 | Response scrubbing | OUT-OF-SCOPE | — |
| `kb-server/src/routes/reindex.rs` | 141 | Reindex trigger | PORTED | `src/app.c` |
| `kb-server/src/routes/sources.rs` | 130 | Source listing | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/errors.rs` | 122 | Error listing | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/dispatch.rs` | 118 | `/api` 404 problem+json, trailing-slash, host fallback | PARTIAL | `src/httpd.c` — errors are RFC 7807 `application/problem+json` with `urn:kb:errors:*` types across the whole status map, an unmatched path is a problem+json 404, and **the host fallback is in**: a `Host:` that parses as an artifact subdomain goes to the artifact serve and anything else to the static handler, both outside the token gate. No trailing-slash redirect |
| `kb-server/src/routes/saved_queries.rs` | 107 | Saved queries | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/users.rs` | 105 | User registry routes | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/facets.rs` | 105 | Facets | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/identity.rs` | 100 | `/api/identity` (the CLI's daemon probe) | PORTED | `src/httpd.c` — token-gated and in `KBC_ROUTES` |
| `kb-server/src/main.rs` | 79 | Binary entry | PORTED | `cli/main.c` |
| `kb-server/src/routes/folders.rs` | 78 | Folder routes | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/tags.rs` | 75 | Tag routes | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/kbs.rs` | 64 | `/api/kbs` (the CLI's default-kb probe) | PORTED | `src/httpd.c` — token-gated and in `KBC_ROUTES`; one row per configured corpus with a doc count, `?kb=` filters to one, and a corpus at the `KBC_MAX_HITS` clamp says so in `docs_truncated` rather than reporting a fake total |
| `kb-server/src/routes/compact.rs` | 64 | Lance compaction trigger | OUT-OF-SCOPE | — |
| `kb-server/src/routes/log_level.rs` | 62 | Log level control | PLANNED | `src/httpd.c` (stage 5) |
| `kb-server/src/routes/drop.rs` | 61 | kb drop | PLANNED | `src/app.c` (stage 4) |
| `kb-server/src/mdns.rs` | 59 | mDNS advertisement | OUT-OF-SCOPE | — |
| `kb-server/src/routes/settings.rs` | 55 | Settings | PLANNED | `src/httpd.c` (stage 3) |
| `kb-server/src/routes/health.rs` | 46 | `/healthz`, outside the auth nest | PORTED | `src/httpd.c` |
| `kb-server/src/routes/shutdown.rs` | 30 | Shutdown | PLANNED | `src/httpd.c` (stage 3) |

## `kb-cli` (73 files, 48,495 LOC) — the `kb` binary

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-cli/src/main.rs` | 7,521 | clap verb tree, dispatch, env setup, bearer read | PARTIAL | `cli/main.c` — a hand-rolled argv parser and **16 verbs** plus `help` (`daemon` (+ `daemon stop`), `add`, `search`, `get`, `reindex`, `list`, `status`, `comments` (4 subcommands), `bench` (+ `bench init`), `backup`, `restore`, `prune`, `metrics`, `config show`, `token generate`, `version`); global flags come before the verb. **Exit codes are this binary's own, not clap's** — every verb answers a user error the same way, and a test asserts that the same user error exits the same code from `search` and from `backup`. clap's default of 2 for a usage error was a Rust build artefact, not a contract anything in kb chose, and reproducing it needed a mutable file-scope static (rule 5) |
| `kb-cli/src/commands/memory.rs` | 3,025 | `remember`/`recall`/`forget`/`propose` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/doctor.rs` | 2,930 | `doctor`, `daemon doctor` | OUT-OF-SCOPE | — **This row was mis-scoped, and LOC hid it.** 2,930 lines was read as a large stage-5 unit; it is not a health check of the daemon at all. It is a check of the **Claude Code provenance chain** — the session marker files `kb-recall.sh` writes, the hook probes, and a recall-outcome check that reads the sessions census. Every one of those is a surface kb-c does not have: no sessions, no memory/recall, no hooks, no markers. Porting it would mean porting the subsystems it exists to police, and they are out of scope by ADR-001. A future reader should take the lesson as the general one: a row's cost is not its line count, it is what the lines are about |
| `kb-cli/src/commands/sessions.rs` | 2,700 | `sessions` verb family | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/slate.rs` | 2,629 | `slate` verb family | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/import.rs` | 2,379 | `import claude-history` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/comments.rs` | 1,683 | `comments` verb family | PARTIAL | `cli/main.c` — **4 of the original's 18 subcommands**: `list`, `add`, `resolve`, `unresolve`, each over `(artifact_id \| path)`. They read the store **directly** and take no `--daemon`, because kb-c's comments are rows in the `comments` table rather than entries in a per-kb `.review` file — there are no review routes to be a client of, and a verb that spoke HTTP to itself would be a second way to say the same SQL. The other 14 have no counterpart to port: `watch` is an SSE consumer over the review firehose (kb-c's SSE stream is there, but there is no review route to stream), and `inbox`/`export`/`apply`/`import`/`reply`/`edit`/`verdict`/`reanchor`/`keep`/`delete`/`upload`/`attach` are review-file and attachment surfaces this port does not have (`kb-core/src/review.rs` above: comments and anchors, no verdicts, no attachments) |
| `kb-cli/src/commands/session_read.rs` | 1,413 | `sessions read` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/bench.rs` | 1,337 | `bench init/discover/run` | PARTIAL | `cli/main.c` — the in-process `bench --queries N --repeat N --corpus DIR` plus **`bench init (--kb NAME \| --corpus DIR) --output PATH [--n N] [--seed S]`**, which scaffolds a `queries.jsonl` with one row per sampled artifact, pre-seeds each `relevant` array with a REAL artifact id so the operator extends rather than hunts, refuses to overwrite an existing file, and is byte-reproducible for a given seed. NOT ported: `discover` (it drives `kb search --json` and prints hits, which `kbc search --json` already does — a second spelling of one verb) and `run` (the labelled-set quality/speed bake-off, which needs the daemon's per-mode latency the in-process harness does not produce) |
| `kb-cli/src/commands/refs.rs` | 1,193 | `refs`, `backlinks` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/sessions_capture.rs` | 1,159 | `sessions capture` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/list.rs` | 1,115 | `list` verb family | PARTIAL | `cli/main.c` — `list --kb --limit`; no paging, no rest of the verb family |
| `kb-cli/src/commands/session_bundle.rs` | 939 | Session bundle | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/daemon.rs` | 827 | `daemon stop/doctor/log-level` | PARTIAL | `cli/main.c` — **`daemon stop`**, SIGTERM to the pid in `<data_dir>/kb-daemon.pid` with a 10 s poll and no SIGKILL escalation. The pid file is the whole contract and `kbc daemon` did not have one until this verb needed it: it is written after the listener binds, a start refuses when it names a live pid other than our own, prunes it when that pid is dead, and counts its OWN pid as stale — in a container the daemon is pid 1, so after a hard kill the next boot is also pid 1 and `kill -0 1` succeeds, which would make the daemon refuse to start forever. `daemon doctor` is OUT-OF-SCOPE above; `daemon log-level` needs `GET/PUT /api/log-level`, which is not in `KBC_ROUTES` |
| `kb-cli/src/commands/why_memory.rs` | 820 | `why` / `why-memory` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/versions.rs` | 790 | `versions`, `diff` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/fleet.rs` | 776 | `fleet` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/atlas.rs` | 760 | `atlas` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/comments_watch.rs` | 703 | `comments watch` (SSE NDJSON) | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/sessions_status.rs` | 632 | `sessions status` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/desk.rs` | 628 | `desk` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/notes.rs` | 607 | `notes` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/backup.rs` | 601 | `backup`, `restore` | PORTED | `cli/main.c` — `kbc backup <kb> [--out PATH] [--all]`. The mechanism, the tarball layout and the three deliberate divergences from the Rust are on the `kb-core/src/storage/backup.rs` row above. The corpus name is validated once, as `KbName` (`1..64` bytes of `[a-z0-9_-]`), because a corpus name is a filesystem path segment AND a tar member name AND a daemon name, so it is checked at the boundary rather than trusted at each use |
| `kb-cli/src/commands/share.rs` | 600 | `share` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/index_page.rs` | 583 | `index-page` HTML | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/context.rs` | 540 | `context` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/search.rs` | 480 | `search`, human + JSON output, exit codes | PORTED | `cli/main.c` — with the in-process fallback |
| `kb-cli/src/commands/model.rs` | 447 | `model` verbs | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/token.rs` | 396 | `token` verbs, `<config>/kb/token`, user registry | PARTIAL | `cli/main.c` — `token generate`; no user registry |
| `kb-cli/src/http.rs` | 390 | reqwest client, daemon probe, `send_json`, OAuth | PARTIAL | `cli/main.c` — a hand-rolled blocking HTTP/1.1 client, the daemon probe, and an in-process fallback for `reindex`/`search`/`get`/`list` when no daemon answers; no OAuth |
| `kb-cli/src/commands/new.rs` | 389 | `new` from template | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/status.rs` | 387 | `status` key/value block | PORTED | `cli/main.c` — daemon-backed, with a clear failure when no daemon answers |
| `kb-cli/src/sse.rs` | 341 | SSE frame parser, backoff, `Last-Event-ID` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/timeline.rs` | 323 | `timeline` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/capture.rs` | 321 | `capture` | PLANNED | `cli/main.c` (stage 5) |
| `kb-cli/src/commands/synth.rs` | 315 | `synth` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/add.rs` | 305 | `add PATH` | PORTED | `cli/main.c` — `add <dir> --kb NAME` |
| `kb-cli/src/commands/proposals.rs` | 303 | `proposals` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/reading.rs` | 269 | `reading` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/exclude.rs` | 251 | `exclude` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/queries.rs` | 248 | `queries` saved queries | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/daycard.rs` | 243 | `daycard` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/resurface.rs` | 223 | `resurface` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/mv.rs` | 217 | `mv` | PLANNED | `cli/main.c` (stage 4) |
| `kb-cli/src/commands/metrics.rs` | 211 | `metrics` | PORTED | `cli/main.c` — `kbc metrics [--out PATH]`, a **client** of the `/metrics` route kb-c already serves, not a second implementation of it. **No in-process fallback**, unlike `search`/`reindex`/`get`/`list`: every counter the endpoint renders lives in the running daemon's registry, so a fallback would print a healthy-looking exposition of zeroes; `kbc status` sets the precedent and its reason. `--out` is the one divergence and it is the one this section's word "export" asks for — the original's verb cannot save its scrape, so `kb metrics > file` is its only way to keep one, and a shell redirect is not something a verb can be tested through |
| `kb-cli/src/commands/board.rs` | 207 | `board` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/push.rs` | 200 | `push` (SSE consumer) | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/events.rs` | 198 | `events --follow` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/read.rs` | 193 | `read` (browser handoff) | PLANNED | `cli/main.c` (stage 5) |
| `kb-cli/src/commands/atlas_field.rs` | 189 | `atlas field` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/slo.rs` | 188 | `slo` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/cat.rs` | 180 | `cat`, offline read, records an open visit | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/download.rs` | 167 | `download` | PLANNED | `cli/main.c` (stage 5) |
| `kb-cli/src/session_marker.rs` | 153 | Session marker file | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/links.rs` | 153 | `links suggest/apply` | PLANNED | `cli/main.c` (stage 4) |
| `kb-cli/src/commands/tools.rs` | 143 | `tools` manifest | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/restore.rs` | 135 | `restore` | PORTED | `cli/main.c` — `kbc restore <tarball> --kb NAME [--force]`, extracting one member (`<kb>` or the daemon-wide `slates/`) into the kb-state parent. It refuses before extracting when the archive did not produce that corpus's `index.db`, so a restore cannot leave a state dir holding a partial one. The version guard stays deferred to the next open, exactly as in the original, which means a too-new tarball is not caught at restore time |
| `kb-cli/src/commands/graph.rs` | 131 | `graph` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/reset.rs` | 122 | `reset` | PLANNED | `cli/main.c` (stage 4) |
| `kb-cli/src/commands/history.rs` | 118 | `history` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/pull.rs` | 117 | `pull` (OIDC) | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/get.rs` | 116 | `get` | PORTED | `cli/main.c` — `get <id> [--source]`, daemon or in-process |
| `kb-cli/src/commands/config.rs` | 104 | `config show/validate/edit` | PARTIAL | `cli/main.c` — `config show` only; no `validate`, no `edit` |
| `kb-cli/src/commands/mod.rs` | 96 | Verb module registry | PARTIAL | `cli/main.c` |
| `kb-cli/src/commands/prompt.rs` | 90 | `prompt` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/compact.rs` | 84 | `compact` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/find.rs` | 80 | `find`, exit 2 on no match | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/users.rs` | 74 | `users` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/similar.rs` | 73 | `similar` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/related.rs` | 69 | `related` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/sources.rs` | 58 | `sources` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/reindex.rs` | 56 | `reindex` | PORTED | `cli/main.c` — with the in-process fallback |
| `kb-cli/src/commands/whoami.rs` | 52 | `whoami` | PLANNED | `cli/main.c` (stage 3) |

## `kb-lip` (11 files, 4,360 LOC) — live-transcript LSP sidecar

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-lip/src/http.rs` | 1,472 | LSP-over-HTTP bridge | OUT-OF-SCOPE | — |
| `kb-lip/src/lsp.rs` | 1,256 | LSP session handling, didOpen/didChange | OUT-OF-SCOPE | — |
| `kb-lip/src/bin/fake_lsp.rs` | 497 | Test double server | OUT-OF-SCOPE | — |
| `kb-lip/src/config.rs` | 285 | Lip config | OUT-OF-SCOPE | — |
| `kb-lip/src/supervisor.rs` | 245 | Child LSP supervision | OUT-OF-SCOPE | — |
| `kb-lip/src/rpc.rs` | 193 | JSON-RPC framing | OUT-OF-SCOPE | — |
| `kb-lip/src/position.rs` | 150 | Document position mapping | OUT-OF-SCOPE | — |
| `kb-lip/src/blob.rs` | 131 | Document blob store | OUT-OF-SCOPE | — |
| `kb-lip/src/main.rs` | 71 | Binary entry | OUT-OF-SCOPE | — |
| `kb-lip/src/server.rs` | 38 | Server handle | OUT-OF-SCOPE | — |
| `kb-lip/src/lib.rs` | 22 | Crate root | OUT-OF-SCOPE | — |

## `kb-embedder` (1 file, 262 LOC)

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-embedder/src/main.rs` | 262 | The NDJSON embed/rerank subprocess itself (ONNX, via `ort`) | OUT-OF-SCOPE | — — kb-c keeps it as an external process; only the parent half of the protocol is ported to `src/embed.c`, and porting the child would mean porting `ort` |

## `kb-buildstamp` (2 files, 115 LOC)

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-buildstamp/build.rs` | 91 | `git describe` at build time | OUT-OF-SCOPE | — |
| `kb-buildstamp/src/lib.rs` | 24 | `VERSION` constant | PARTIAL | `include/kbc/kbc.h` — `KBC_VERSION "0.1.0"`, a literal rather than a build stamp |

## kb-c modules that own no Rust row of their own

Three of kb-c's files have no 1:1 module in the original, so they are invisible
to a row-by-row reading of the tables above. They are listed here so that is not
where they stay.

| kb-c module | LOC | What it is | Why it has no Rust row |
|---|---:|---|---|
| `src/links.c` + `include/kbc/links.h` | 566 / 101 | The four-tier wikilink resolution ladder as a leaf: build one index over a candidate set, resolve many targets against it | It is the *resolution* half of `kb-core/src/links.rs`, which the table above credits to `src/parse.c`. Splitting extraction (a normaliser shared with every other link form) from resolution (an index over candidates) is what makes the ladder testable without a store |
| `src/markdown.c` + `include/kbc/markdown.h` | 3,654 / 118 | A hand-rolled Markdown → page HTML renderer, in place of comrak, serving a `.md` artifact as a page on both serving origins | `kb-core/src/markdown.rs` is 800 lines of *wrapper*; the engine it wraps is a third-party crate kb-c has decided not to take. The Rust row cannot express "we wrote our own, deliberately narrower" — so the divergence is recorded there and the artefact is recorded here |
| `include/kbc/chunk.h` (implemented in `src/app.c`) | 74 | The chunker's contract: 280-word window, 60 overlap, chunk 0 the title passage, a 512 cap that reports the original count | It is `kb-core/src/chunk.rs` given a frozen header because the constants are a contract and a test that mirrors a struct is a test of layout. The implementation lives in the ingest path because a chunk IS a storage-shaped write to the `chunks` table |

## Not in this inventory at all

Two React/Vite SPAs (`web/`, `web-code/`), the Claude Code plugins (`plugins/`),
`docs/`, and the Playwright e2e suite (`tests/e2e/`). None of it is C, and
kb-c still ships no UI of its own — but that is no longer the whole story. The
daemon answers a JSON API, `GET /` prints a route banner, and it now has the
three serving surfaces a reader UI needs to exist at all: the artifact bytes
on the parent origin, one artifact subdomain per artifact, and a static
handler for `KB_SPA_DIST` (which must hold an `index.html` or the daemon
refuses to start). Point any of them at a bundle and there is a page; kb-c
does not build one, and the Rust SPAs stay out of scope. `tests/e2e/` becomes
the parity harness kb-c's own acceptance gates run against, once a kb-c
daemon is behind the same API (stage 3).
