# INVENTORY.md — module-by-module map of the Rust `kb` → C `kb-c`

Source of truth for LOC: `find /home/nik/project/kb/crates -name '*.rs' -not -path '*/tests/*' -not -path '*/benches/*' | xargs wc -l`.
Total: **471,634 LOC across 540 files in 8 crates.**

kb-c's side is the frozen contract in `include/kbc/*.h` (14 headers) and its
implementation, one file per subsystem (`src/kbc/<name>.c`, `cli/main.c`).
`src/` is the owner column below; where a responsibility moved rather than
ported, the reason says so.

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
231,507.

| Status | Files | LOC | % |
|---|---:|---:|---:|
| PORTED | 25 | 10,091 | 4.4% |
| PARTIAL | 23 | 63,933 | 27.6% |
| PLANNED | 80 | 44,357 | 19.2% |
| OUT-OF-SCOPE | 124 | 113,126 | 48.9% |
| **total** | **252** | **231,507** | **100%** |

Plus, in the two `kb-code-*` crates: 34 module rows, 240,255 LOC, every one
OUT-OF-SCOPE (table below).

> These counts are the row statuses in this document, not a claim about what
> is finished. The port's live surface today is the PORTED + PARTIAL set:
> 74,024 LOC of Rust that kb-c claims, of which 10,091 is a row-for-row port
> and 63,933 is a deliberately narrower replacement. The difference is
> almost entirely `storage/sqlite.rs` and `storage/lance.rs`, whose C
> counterparts are `store.c` and `index.c`.

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

**Why the whole subtree is out of scope for milestone 1.** `kb-code-server`
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
| `kb-core/src/storage/sqlite.rs` | 14,913 | SQLite open/pragmas, 42 embedded migrations, every side table's CRUD, `is_newest` collapse, cascade delete | PARTIAL | `src/kbc/store.c` — artifacts, chunks, comments only; the session/slate/atlas/recall/slo tables are out of scope |
| `kb-core/src/indexer.rs` | 9,028 | Watcher→queue→batch→parse→embed→write pipeline, dedup, quarantine, reconcile | PARTIAL | `src/kbc/app.c` — no enrich tail, no session substitution, no sidecar embedding reuse |
| `kb-core/src/storage/actor.rs` | 7,205 | Tokio actor wrapping `Storage`, mpsc back-pressure for writers | PARTIAL | `src/kbc/store.c` — direct calls, no actor; the batch boundary is the store transaction |
| `kb-core/src/storage/lance.rs` | 6,171 | Lance/Arrow columnar store: FTS indices, IVF-PQ, chunk kNN, compact, backup hook | PARTIAL | `src/kbc/index.c` + `src/kbc/embed.c` — replaced, not ported; no columnar store, no IVF-PQ |
| `kb-core/src/sessions.rs` | 6,023 | Session transcript parsing, digest rendering, aggregation | OUT-OF-SCOPE | — |
| `kb-core/src/slate.rs` | 4,683 | Slate board model and engine | OUT-OF-SCOPE | — |
| `kb-core/src/config.rs` | 4,535 | Whole `kb.toml` schema, resolvers, `validate()`, save/preserve | PARTIAL | `src/kbc/config.c` — `[daemon] [server] [indexer] [kb.*]` subset, no TOML re-emit |
| `kb-core/src/sessions/view.rs` | 4,441 | Session HTML rendering | OUT-OF-SCOPE | — |
| `kb-core/src/memory.rs` | 3,678 | Memory types, decay, salience, recall scoring | OUT-OF-SCOPE | — |
| `kb-core/src/share/mod.rs` | 3,268 | Share/deploy orchestration | OUT-OF-SCOPE | — |
| `kb-core/src/review.rs` | 3,169 | Review anchors, comments, verdicts, attachments | PARTIAL | `src/kbc/store.c` — comments and anchors; no verdicts, no attachments |
| `kb-core/src/enrich.rs` | 3,100 | 8 ordered post-index enrichment hooks | PLANNED | `src/kbc/app.c` (stage 4) |
| `kb-core/src/atlas.rs` | 2,530 | UMAP/PCA embedding layout, clustering, snapshots | OUT-OF-SCOPE | — |
| `kb-core/src/parser.rs` | 2,029 | HTML/Markdown field extraction, frontmatter, capability flags | PORTED | `src/kbc/parse.c` — Html and Markdown arms only, as in Rust |
| `kb-core/src/lists.rs` | 1,927 | Lists and entries with dense positions | OUT-OF-SCOPE | — |
| `kb-core/src/coderefs.rs` | 1,791 | Code-reference extraction from doc prose | OUT-OF-SCOPE | — |
| `kb-core/src/capture.rs` | 1,585 | Capture pipeline: slug, stamp, atomic write, id | PARTIAL | `src/kbc/app.c` — write/stamp/id, no HTML sanitizer |
| `kb-core/src/relocate.rs` | 1,505 | Move log and path relocation | OUT-OF-SCOPE | — |
| `kb-core/src/watcher.rs` | 1,390 | Filesystem watch, debounce, reconcile walk | PORTED | `src/kbc/watcher.c` (inotify) |
| `kb-core/src/docs_query.rs` | 1,300 | In-memory filter evaluation over candidate rows | PLANNED | `src/kbc/search.c` (stage 3) |
| `kb-core/src/sessions/live.rs` | 1,256 | Live-transcript tailing | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/replay.rs` | 1,231 | Session replay export | OUT-OF-SCOPE | — |
| `kb-core/src/embed_ipc.rs` | 1,220 | NDJSON subprocess protocol to `kb-embedder`, handshake, timeouts, respawn | PORTED | `src/kbc/embed.c` — same envelope names, same `req_id` rules |
| `kb-core/src/session_render.rs` | 1,204 | Session digest renderer | OUT-OF-SCOPE | — |
| `kb-core/src/query.rs` | 1,188 | Query grammar: `AND`/`OR`/`NOT`, keys, DNF, `since:` | PLANNED | `src/kbc/search.c` (stage 3) |
| `kb-core/src/mentions.rs` | 1,108 | `@mention` resolution | OUT-OF-SCOPE | — |
| `kb-core/src/atlas_field.rs` | 1,097 | Atlas field values per artifact | OUT-OF-SCOPE | — |
| `kb-core/src/storage/schema.rs` | 988 | The 45-field `Doc` struct and its Arrow schema | PARTIAL | `src/kbc/types.h` — the 20 fields kb-c keeps |
| `kb-core/src/vcs.rs` | 956 | Git versions mode, snapshot capture | OUT-OF-SCOPE | — |
| `kb-core/src/embed.rs` | 935 | Model registry, batch caps, pending-query yield | PARTIAL | `src/kbc/embed.c` — registry names, no yield scheduler |
| `kb-core/src/slo.rs` | 895 | SLO indicator computation and snapshots | OUT-OF-SCOPE | — |
| `kb-core/src/markdown.rs` | 800 | Markdown → page HTML renderer | PLANNED | `src/kbc/parse.c` (stage 5) |
| `kb-core/src/iframe.rs` | 724 | Artifact-subdomain host model | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-core/src/headings.rs` | 706 | Heading tree extraction | PORTED | `src/kbc/parse.c` |
| `kb-core/src/meta_edit.rs` | 700 | `kb-*` frontmatter/meta rewriting | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/narrative.rs` | 674 | Session narrative assembly | OUT-OF-SCOPE | — |
| `kb-core/src/session_render_tests.rs` | 672 | Tests that live in `src/` | OUT-OF-SCOPE | — |
| `kb-core/src/links.rs` | 656 | Wikilink parsing and canonical-path resolution | PLANNED | `src/kbc/parse.c` (stage 4) |
| `kb-core/src/events.rs` | 652 | Event bus: ring, broadcast, filters, replay | PARTIAL | `src/kbc/httpd.c` — ring + SSE, no cold-replay gap probe yet |
| `kb-core/src/share/cloudflare.rs` | 651 | Cloudflare Pages/Access deploy | OUT-OF-SCOPE | — |
| `kb-core/src/paths.rs` | 630 | State/config/cache roots, rel-path derivation | PORTED | `src/kbc/mem.c` + `src/kbc/config.c` |
| `kb-core/src/triage.rs` | 619 | Memory triage heuristics | OUT-OF-SCOPE | — |
| `kb-core/src/storage/backup.rs` | 617 | `VACUUM INTO` snapshot + tar bundle | PLANNED | `src/kbc/store.c` (stage 6) |
| `kb-core/src/procrustes.rs` | 614 | Recall-fidelity measurement | OUT-OF-SCOPE | — |
| `kb-core/src/metrics.rs` | 610 | Counters, histograms, `/metrics` text | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-core/src/sessions/tail.rs` | 570 | Live transcript tail reader | OUT-OF-SCOPE | — |
| `kb-core/src/fusion.rs` | 567 | RRF fusion, title boost, graph boost | PORTED | `src/kbc/search.c` — `RRF_K=60`, `TITLE_BOOST=0.5` |
| `kb-core/src/versions.rs` | 545 | Version timeline reads | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/opencode.rs` | 545 | opencode live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/notes.rs` | 535 | Notes model | OUT-OF-SCOPE | — |
| `kb-core/src/history.rs` | 526 | Reading/search history rows | PLANNED | `src/kbc/store.c` (stage 3) |
| `kb-core/src/atlas_labels.rs` | 515 | c-TF-IDF cluster labels | OUT-OF-SCOPE | — |
| `kb-core/src/reading.rs` | 509 | Reading-progress sections | OUT-OF-SCOPE | — |
| `kb-core/src/session_bundle.rs` | 505 | Session bundle export/import | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/kimi.rs` | 503 | kimi live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/grok.rs` | 488 | grok live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/codex.rs` | 457 | codex live adapter | OUT-OF-SCOPE | — |
| `kb-core/src/resurface.rs` | 455 | Resurface queue scoring | OUT-OF-SCOPE | — |
| `kb-core/src/attachments.rs` | 451 | Attachment blobs and manifest | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-core/src/webhook_url.rs` | 440 | Webhook URL validation incl. SSRF rules | PLANNED | `src/kbc/config.c` (stage 5) |
| `kb-core/src/session_scrub.rs` | 436 | Session scrubbing/anonymisation | OUT-OF-SCOPE | — |
| `kb-core/src/tracing_init.rs` | 423 | `tracing` subscriber setup | PARTIAL | `src/kbc/log.c` — levels and JSON lines, no spans |
| `kb-core/src/anchors.rs` | 390 | Anchor ids and stale/resolved detection | PORTED | `src/kbc/parse.c` |
| `kb-core/src/graph_report.rs` | 382 | Link graph report | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/projects.rs` | 336 | Project-key inference | OUT-OF-SCOPE | — |
| `kb-core/src/sessions/live_adapters/mod.rs` | 336 | Live adapter registry | OUT-OF-SCOPE | — |
| `kb-core/src/cascade.rs` | 335 | Delete cascade across every side table | PLANNED | `src/kbc/store.c` (stage 4) |
| `kb-core/src/share/github.rs` | 331 | GitHub Pages deploy | OUT-OF-SCOPE | — |
| `kb-core/src/exclusions.rs` | 302 | Excluded-path gate | PORTED | `src/kbc/config.c` |
| `kb-core/src/ids.rs` | 294 | `hash12`, `SourceSlug`, Crockford `r-`/`e-` ids | PARTIAL | `src/kbc/types.c` — `hash12` only |
| `kb-core/src/extmap.rs` | 268 | Extension → pipeline map | PORTED | `src/kbc/config.c` |
| `kb-core/src/identity.rs` | 258 | Operator name normalisation and validation | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-core/src/scrub.rs` | 246 | Redaction pattern application | OUT-OF-SCOPE | — |
| `kb-core/src/share/host.rs` | 245 | Share host routing | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-core/src/sibling.rs` | 224 | Schema-epoch volume-ahead refusal | PORTED | `src/kbc/store.c` |
| `kb-core/src/chunk.rs` | 184 | Deterministic word-window chunking | PORTED | `src/kbc/search.c` — 280-word window, 60 overlap |
| `kb-core/src/types.rs` | 179 | `KbName`, `ArtifactId`, `EventEnvelope`, enums | PARTIAL | `src/kbc/types.h` |
| `kb-core/src/sessions/constants.rs` | 171 | Session constants | OUT-OF-SCOPE | — |
| `kb-core/src/error.rs` | 167 | `Error` → status/URN-kind mapping | PORTED | `src/kbc/kbc.c` — `kbc_status` enum |
| `kb-core/src/test_support.rs` | 133 | Test fixtures | OUT-OF-SCOPE | — |
| `kb-core/src/share/assets.rs` | 125 | Share asset bundling | OUT-OF-SCOPE | — |
| `kb-core/src/timeparse.rs` | 103 | Date/time parsing | PLANNED | `src/kbc/mem.c` (stage 3) |
| `kb-core/src/strutil.rs` | 99 | String helpers | PORTED | `src/kbc/mem.c` |
| `kb-core/src/corkboard.rs` | 95 | Pin board | OUT-OF-SCOPE | — |
| `kb-core/src/fsx.rs` | 81 | Atomic write, mkdir -p | PORTED | `src/kbc/mem.c` |
| `kb-core/src/lib.rs` | 72 | Crate root and re-exports | PORTED | `include/kbc/kbc.h` |
| `kb-core/src/storage/mod.rs` | 11 | `Storage` enum | PORTED | `src/kbc/store.c` |
| `kb-core/benches/perf.rs` | 128 | Criterion benches (excluded from the LOC total above) | OUT-OF-SCOPE | — |

## `kb-server` (77 files, 58,636 LOC) — the axum daemon

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-server/src/routes/sessions.rs` | 8,070 | 30+ session routes, presence, live lane | OUT-OF-SCOPE | — |
| `kb-server/src/routes/memory.rs` | 4,832 | Memory write/read/recall routes | OUT-OF-SCOPE | — |
| `kb-server/src/lib.rs` | 3,048 | Listener bind, task spawn, startup refusal | PARTIAL | `src/kbc/httpd.c` — bind + SO_REUSEPORT; no public-bind-without-token refusal yet |
| `kb-server/src/routes/search.rs` | 2,633 | `/search`: pool sizing, arms, fusion site, `Hit` shape, rerank | PLANNED | `src/kbc/httpd.c` + `src/kbc/search.c` (stage 3) |
| `kb-server/src/routes/slates.rs` | 2,243 | Slate routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/comments.rs` | 2,231 | Comment CRUD, reanchor, upload | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/atlas.rs` | 2,213 | Atlas recompute/recluster/history | OUT-OF-SCOPE | — |
| `kb-server/src/routes/docs.rs` | 2,130 | `/docs` listing and artifact serving with CSP/nosniff | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/context.rs` | 1,707 | Context bundle assembly | OUT-OF-SCOPE | — |
| `kb-server/src/routes/daycard.rs` | 1,672 | Daycard HTML/JSON | OUT-OF-SCOPE | — |
| `kb-server/src/state.rs` | 1,599 | `KbHandles`, per-kb state, caches | PLANNED | `src/kbc/app.c` (stage 3) |
| `kb-server/src/middleware.rs` | 1,511 | Bearer auth, identity ladder, CORS, origin allowlist, rate limit, loopback | PARTIAL | `src/kbc/httpd.c` — loopback + bearer; origin/CORS/rate-limit in stage 3 |
| `kb-server/src/routes/artifact.rs` | 1,367 | Artifact-subdomain serving with traversal guard | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/lists.rs` | 1,203 | List routes | OUT-OF-SCOPE | — |
| `kb-server/src/router.rs` | 1,029 | 226 route registrations, layer order, fallbacks | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/echoes.rs` | 997 | Echo/notebook routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/lookup.rs` | 937 | Lookup by id/path/title | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/artifacts.rs` | 935 | Artifact list/paging | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/history.rs` | 820 | History write/read | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/notes.rs` | 813 | Notes routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/proposals.rs` | 747 | Proposals | OUT-OF-SCOPE | — |
| `kb-server/src/routes/links.rs` | 747 | Link routes | PLANNED | `src/kbc/httpd.c` (stage 4) |
| `kb-server/src/routes/metrics.rs` | 736 | Prometheus text endpoint | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/embed_cache.rs` | 681 | Query-embedding cache (`embed_ms`, `cache_hit`) | PORTED | `src/kbc/embed.c` |
| `kb-server/src/routes/attachments.rs` | 670 | Attachment blob serving | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/coderefs.rs` | 642 | Code-ref routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/share.rs` | 639 | Share routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/timeline.rs` | 637 | Timeline lanes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/schema.rs` | 606 | Event schema endpoint + 100-type catalogue + drift test | PARTIAL | `src/kbc/httpd.c` — envelope only |
| `kb-server/src/routes/desk.rs` | 592 | Desk handoff routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/capture.rs` | 533 | Multipart capture, desk, content PUT | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/spa.rs` | 530 | Static SPA asset serving | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/turn.rs` | 488 | Turn routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/versions.rs` | 484 | Version routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/events.rs` | 429 | SSE endpoint, keep-alive, lag/gap frames, filters | PARTIAL | `src/kbc/httpd.c` |
| `kb-server/src/routes/resurface.rs` | 422 | Resurface queue | OUT-OF-SCOPE | — |
| `kb-server/src/routes/atlas_field.rs` | 383 | Atlas field routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/graph.rs` | 352 | Link graph | OUT-OF-SCOPE | — |
| `kb-server/src/live_registry.rs` | 327 | Live-session registry | OUT-OF-SCOPE | — |
| `kb-server/src/routes/config.rs` | 317 | Config read/validate | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/mod.rs` | 315 | Shared helpers, `is_safe_id`, fanout cap | PARTIAL | `src/kbc/httpd.c` — `is_safe_id` equivalent in the path guard |
| `kb-server/src/routes/boards.rs` | 291 | Boards | OUT-OF-SCOPE | — |
| `kb-server/src/routes/anchors.rs` | 289 | Anchor routes | PLANNED | `src/kbc/httpd.c` (stage 4) |
| `kb-server/src/routes/memory_links.rs` | 270 | Memory link routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/inbox.rs` | 265 | Inbox | OUT-OF-SCOPE | — |
| `kb-server/src/routes/slo.rs` | 261 | SLO routes | OUT-OF-SCOPE | — |
| `kb-server/src/live_tail_cache.rs` | 250 | Live tail cache | OUT-OF-SCOPE | — |
| `kb-server/src/routes/relocate.rs` | 225 | Move routes | OUT-OF-SCOPE | — |
| `kb-server/src/routes/quarantine.rs` | 218 | Quarantine routes | PLANNED | `src/kbc/httpd.c` (stage 4) |
| `kb-server/src/routes/prompt.rs` | 215 | Outbound prompt template + redaction floor | OUT-OF-SCOPE | — |
| `kb-server/src/slate_registry.rs` | 214 | Slate registry | OUT-OF-SCOPE | — |
| `kb-server/src/routes/stats.rs` | 211 | Stats | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/download.rs` | 207 | Download via the index's own path | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/replay_cache.rs` | 188 | SSE replay cache | PARTIAL | `src/kbc/httpd.c` |
| `kb-server/src/routes/exclusions.rs` | 179 | Exclusion routes | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/review.rs` | 173 | Review verdict routes | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/touches_cache.rs` | 164 | Touch-event cache | OUT-OF-SCOPE | — |
| `kb-server/src/scrub.rs` | 148 | Response scrubbing | OUT-OF-SCOPE | — |
| `kb-server/src/routes/reindex.rs` | 141 | Reindex trigger | PORTED | `src/kbc/app.c` |
| `kb-server/src/routes/sources.rs` | 130 | Source listing | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/errors.rs` | 122 | Error listing | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/dispatch.rs` | 118 | `/api` 404 problem+json, trailing-slash, host fallback | PARTIAL | `src/kbc/httpd.c` |
| `kb-server/src/routes/saved_queries.rs` | 107 | Saved queries | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/users.rs` | 105 | User registry routes | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/facets.rs` | 105 | Facets | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/identity.rs` | 100 | `/api/identity` (the CLI's daemon probe) | PORTED | `src/kbc/httpd.c` |
| `kb-server/src/main.rs` | 79 | Binary entry | PORTED | `cli/main.c` |
| `kb-server/src/routes/folders.rs` | 78 | Folder routes | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/tags.rs` | 75 | Tag routes | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/kbs.rs` | 64 | `/api/kbs` (the CLI's default-kb probe) | PORTED | `src/kbc/httpd.c` |
| `kb-server/src/routes/compact.rs` | 64 | Lance compaction trigger | OUT-OF-SCOPE | — |
| `kb-server/src/routes/log_level.rs` | 62 | Log level control | PLANNED | `src/kbc/httpd.c` (stage 5) |
| `kb-server/src/routes/drop.rs` | 61 | kb drop | PLANNED | `src/kbc/app.c` (stage 4) |
| `kb-server/src/mdns.rs` | 59 | mDNS advertisement | OUT-OF-SCOPE | — |
| `kb-server/src/routes/settings.rs` | 55 | Settings | PLANNED | `src/kbc/httpd.c` (stage 3) |
| `kb-server/src/routes/health.rs` | 46 | `/healthz`, outside the auth nest | PORTED | `src/kbc/httpd.c` |
| `kb-server/src/routes/shutdown.rs` | 30 | Shutdown | PLANNED | `src/kbc/httpd.c` (stage 3) |

## `kb-cli` (73 files, 48,495 LOC) — the `kb` binary

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-cli/src/main.rs` | 7,521 | clap verb tree, dispatch, env setup, bearer read | PARTIAL | `cli/main.c` — hand-rolled argv parser, the shipped verb subset |
| `kb-cli/src/commands/memory.rs` | 3,025 | `remember`/`recall`/`forget`/`propose` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/doctor.rs` | 2,930 | `doctor`, `daemon doctor` | PLANNED | `cli/main.c` (stage 5) |
| `kb-cli/src/commands/sessions.rs` | 2,700 | `sessions` verb family | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/slate.rs` | 2,629 | `slate` verb family | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/import.rs` | 2,379 | `import claude-history` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/comments.rs` | 1,683 | `comments` verb family | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/session_read.rs` | 1,413 | `sessions read` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/bench.rs` | 1,337 | `bench init/discover/run` | PLANNED | `cli/main.c` (stage 6) — the harness the perf thesis is measured with |
| `kb-cli/src/commands/refs.rs` | 1,193 | `refs`, `backlinks` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/sessions_capture.rs` | 1,159 | `sessions capture` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/list.rs` | 1,115 | `list` verb family | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/session_bundle.rs` | 939 | Session bundle | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/daemon.rs` | 827 | `daemon stop/doctor/log-level` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/why_memory.rs` | 820 | `why` / `why-memory` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/versions.rs` | 790 | `versions`, `diff` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/fleet.rs` | 776 | `fleet` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/atlas.rs` | 760 | `atlas` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/comments_watch.rs` | 703 | `comments watch` (SSE NDJSON) | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/sessions_status.rs` | 632 | `sessions status` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/desk.rs` | 628 | `desk` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/notes.rs` | 607 | `notes` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/backup.rs` | 601 | `backup`, `restore` | PLANNED | `cli/main.c` (stage 6) |
| `kb-cli/src/commands/share.rs` | 600 | `share` verbs | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/index_page.rs` | 583 | `index-page` HTML | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/context.rs` | 540 | `context` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/search.rs` | 480 | `search`, human + JSON output, exit codes | PORTED | `cli/main.c` |
| `kb-cli/src/commands/model.rs` | 447 | `model` verbs | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/token.rs` | 396 | `token` verbs, `<config>/kb/token`, user registry | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/http.rs` | 390 | reqwest client, daemon probe, `send_json`, OAuth | PLANNED | `cli/main.c` (stage 3) — hand-rolled HTTP/1.1 |
| `kb-cli/src/commands/new.rs` | 389 | `new` from template | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/status.rs` | 387 | `status` key/value block | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/sse.rs` | 341 | SSE frame parser, backoff, `Last-Event-ID` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/timeline.rs` | 323 | `timeline` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/capture.rs` | 321 | `capture` | PLANNED | `cli/main.c` (stage 5) |
| `kb-cli/src/commands/synth.rs` | 315 | `synth` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/add.rs` | 305 | `add PATH` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/proposals.rs` | 303 | `proposals` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/reading.rs` | 269 | `reading` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/exclude.rs` | 251 | `exclude` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/queries.rs` | 248 | `queries` saved queries | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/daycard.rs` | 243 | `daycard` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/resurface.rs` | 223 | `resurface` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/mv.rs` | 217 | `mv` | PLANNED | `cli/main.c` (stage 4) |
| `kb-cli/src/commands/metrics.rs` | 211 | `metrics` | PLANNED | `cli/main.c` (stage 5) |
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
| `kb-cli/src/commands/restore.rs` | 135 | `restore` | PLANNED | `cli/main.c` (stage 6) |
| `kb-cli/src/commands/graph.rs` | 131 | `graph` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/reset.rs` | 122 | `reset` | PLANNED | `cli/main.c` (stage 4) |
| `kb-cli/src/commands/history.rs` | 118 | `history` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/pull.rs` | 117 | `pull` (OIDC) | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/get.rs` | 116 | `get` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/config.rs` | 104 | `config show/validate/edit` | PORTED | `cli/main.c` — `show` and `validate` |
| `kb-cli/src/commands/mod.rs` | 96 | Verb module registry | PARTIAL | `cli/main.c` |
| `kb-cli/src/commands/prompt.rs` | 90 | `prompt` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/compact.rs` | 84 | `compact` | OUT-OF-SCOPE | — |
| `kb-cli/src/commands/find.rs` | 80 | `find`, exit 2 on no match | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/users.rs` | 74 | `users` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/similar.rs` | 73 | `similar` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/related.rs` | 69 | `related` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/sources.rs` | 58 | `sources` | PLANNED | `cli/main.c` (stage 3) |
| `kb-cli/src/commands/reindex.rs` | 56 | `reindex` | PORTED | `cli/main.c` |
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
| `kb-embedder/src/main.rs` | 262 | The NDJSON embed/rerank subprocess itself (ONNX, via `ort`) | OUT-OF-SCOPE | — — kb-c keeps it as an external process; only the parent half of the protocol is ported to `src/kbc/embed.c`, and porting the child would mean porting `ort` |

## `kb-buildstamp` (2 files, 115 LOC)

| Module | LOC | What it does | Status | kb-c owner |
|---|---:|---|---|---|
| `kb-buildstamp/build.rs` | 91 | `git describe` at build time | OUT-OF-SCOPE | — |
| `kb-buildstamp/src/lib.rs` | 24 | `VERSION` constant | PARTIAL | `include/kbc/kbc.h` — `KBC_VERSION "0.1.0"`, a literal rather than a build stamp |

## Not in this inventory at all

Two React/Vite SPAs (`web/`, `web-code/`), the Claude Code plugins (`plugins/`),
`docs/`, and the Playwright e2e suite (`tests/e2e/`). None of it is C. The
port's UI story is unchanged: kb-c serves a static bundle on the same routes,
and the SPAs keep being built by the same toolchain. `tests/e2e/` becomes
the parity harness kb-c's own acceptance gates run against, once a kb-c
daemon is behind the same API (stage 3).
