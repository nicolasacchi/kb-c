---
title: kb-reflect — implementation plan (near + mid term, Claude-Code-driven dream)
kb-category: note
kb-tags: memory, kb-reflect, sessions, recall, plan, build-brief
---

# kb-reflect — implementation plan

**Build brief for the near-term + mid-term of the memory-evolution roadmap**
([evolving-the-kb-memory-system.html](evolving-the-kb-memory-system.html)),
with one architectural change the operator locked in this session:

> **The distiller is Claude Code itself.** `kb reflect` is **not** a new Rust
> CLI verb or a daemon worker with a hosted model. It is a Claude Code
> **command** (`/kb-reflect`) whose body drives the *existing* kb verbs
> (`kb sessions list/show` → `kb recall` → `kb remember`/`kb forget`). The LLM
> that distills sessions into facts is the same Claude Code session that runs
> the command — same model, same tokens, already authorized to drive every
> corpus. This **dissolves the single open question** that gated the original
> D2 ("will the project host a generative model?") — there is no model to host.

This plan is the deliverable for `/deep-review`; corrections fold in below the
line before implementation.

## Context

The research (31-agent workflow, real-data-grounded) found the binding
constraint is the **empty write side** (13 curated facts vs 196 captured
sessions; `memory_count = 0` on recent sessions; salience stuck at the 0.5
default), **not** the deterministic ranker (invariant #10), which is over-built
for 13 facts. The recommended path: a handful of cheap correctness fixes, then
build the deferred **R6 distiller** as an offline, model-isolated, human-gated
session→fact consolidator. The operator's refinement makes the distiller a
Claude Code command, so "offline + model-isolated" becomes "runs inside the
agent that already exists."

**Scope of THIS plan:** the near-term Rust correctness fixes (Track A) + the
mid-term `/kb-reflect` command (Track B). Explicitly **out**: the far-term graph
/ telemetry / eval-harness work (deferred, and two of those directions had fatal
mechanism bugs as designed — see the research doc §4).

**Invariants held intact:** #10 (per-turn recall stays deterministic, LLM-free —
this plan touches the WRITE side and a digest input, never `rerank`), #11
(episodic sessions stay pull-only and digest-indexed — the command READS digests
via `kb sessions show`, never auto-injects them), #26 (no model linked into the
daemon — the "model" is the live Claude Code session, entirely outside the
daemon), #5 kb-core EnrichmentHook order, #27 path canonicalization.

## Architecture

### Track B — `/kb-reflect`, the Claude-Code-driven dream (no new Rust)

A new plugin `plugins/kb-reflect/` with a single **command**
`commands/kb-reflect.md`. A command (not a skill) because the operator
*triggers* the dream deliberately, exactly like `/kb-comments` and `/kb-tools`
(user-invoked), not like `/kb-artifact` (model-elected). The command body is a
prompt that walks Claude through the loop, using only confirmed CLI surface:

| Step | Verb (confirmed exists) | Purpose |
|---|---|---|
| 1. Discover | `kb sessions list --limit N --json` | newest sessions; filter client-side to `memory_count == 0` (the **idempotency** gate) and, if the operator passed a folder/project hint, to that `folder`/`cwd` |
| 2. Read | `kb sessions show <id> --json` | per-session digest, decisions, touched files, first_user_prompt, git_branch — the distiller input (deterministic digest, #11 R1) |
| 3. Distill | *(Claude reasons)* | extract 0..N durable candidate facts per session; discard ephemera |
| 4. Reconcile | `kb recall "<candidate>" --scope all --json` | find nearest existing memories → classify **ADD** (novel) / **SUPERSEDE** (updates one) / **MERGE** (folds several) / **NOOP** (already covered) |
| 5. Propose | *(Claude prints a table)* | **dry-run by default**: proposed fact, classification, salience (per rubric), neighbors compared, source session — operator approves |
| 6. Write | `kb remember "<fact>" --title "<label>" --salience <g> --link <kb> --tags … [--supersedes <id>] --session-id <src>` | accepted facts only; `--session-id` stamps provenance AND lifts `memory_count` so step 1 skips the session next run |
| 7. Merge tail | `kb forget <other_id> [--kb …]` | for MERGE of N: SUPERSEDE the primary via `remember --supersedes`, then `forget` the remaining sources (drop-at-recall) |

**Idempotency** rides an existing signal — `kb sessions list --json` already
returns `memory_count` per session; a reflected session has
`memory_count > 0` because every `kb remember --session-id <src>` stamps
`kb-session`, which the daemon counts. No new state table, no watermark file.
(Edge case noted in Risks: a manually-remembered fact also lifts the count; v1
treats "has any memory" as "reflected" and the operator can force re-reflection
by naming a session.)

**Provenance** = `--session-id <src>` (links the fact to its origin session and
is queryable via the sessions API) + naming additional source sessions in the
body text for multi-source MERGE (one `--session-id` per `remember`; the body is
BM25-indexed so "(sessions X, Y)" stays searchable).

**The salience rubric** (baked into the command prompt — this is the research's
key guardrail against the LLM rater collapsing to all-0.8):

- user identity / preference / explicit correction → **0.8–0.9**
- project decision / architecture / shipped milestone → **0.6–0.7**
- operational fact (a deploy command, a path, a gotcha) → **0.5–0.6**
- one-off / already-in-the-repo / ephemeral → **NOOP** (don't remember)

Plus a **distribution check**: "if >60% of proposed facts land in one salience
bucket, re-grade — a flat distribution is the failure mode that recreates the
0.5 monoculture." And the **dedup discipline**: always `kb recall` a candidate
before writing; NOOP if a near-identical fact exists; SUPERSEDE if it's a newer
version of an existing fact.

**Why a command, not a hook:** the dream is deliberate and reviewable, not
automatic. (A future Stop-hook that *suggests* "you did durable work — run
/kb-reflect?" is a possible follow-up, explicitly out of scope here.)

### Track A — near-term Rust correctness fixes (enable + sharpen the write side)

Four small, independently green-CI fixes. Three are real correctness bugs the
research + recon verified; one (A4-summary) is a display nicety flagged optional.

**A1 — kill the duplicated salience default.** `crates/kb-core/src/memory.rs:22`
`DEFAULT_SALIENCE` is private; `crates/kb-server/src/routes/memory.rs:340`
hardcodes the literal `0.5` in the decay-floor filter with a code comment that
*admits* the duplication. Make the const `pub`, use it in the route, add the
missing recall-route test for the inline floor (only `rerank`'s own floor is
unit-tested today).

**A2 — clean the distiller input (digest-noise filter).**
`crates/kb-core/src/sessions.rs` `classify_research` (~:829) currently counts
`kb`'s own verbs (`kb search/recall/find/why/remember`, surfaced as
`kind="kb_search"`) and *all* `mcp__*` tools (incl. Playwright browser
automation, surfaced as `kind="skill"`) as "research". Filter both out so the
digest's `researched:` line — the distiller's input and the activity funnel —
carries real research signal only. Update the pinned test
`parse_extracts_research_signals` (~:1494) and add a negative test. (Self-
referential because the reflect loop itself runs `kb recall`/`kb remember`,
which must never pollute the next digest.)

**A3 — decay on write-time, not file mtime.** `routes/memory.rs:357-369` sources
`RecallHit.mtime_unix` from `ds.mtime_unix` (file mtime), so a reindex/rewrite
resets a memory's age to 0 and silently un-decays it. **Do not** use the
existing `created_unix` column — it is filesystem btime (resets on rewrite; NULL
on btime-less mounts → silent fallback to the bug). Instead stamp a write-time
`<meta name="kb-created" content="<unix>">` at creation (lives in the source,
stable across reindex by construction), parse it, store it in a new nullable
`kb_created_unix` lance column, and source the recall age from
`ds.kb_created_unix.or(ds.mtime_unix)` (fallback only for pre-migration rows).

**A4 — title/summary split (OPTIONAL — defer if deep-review objects).** Clean
titles are *already* achievable: `kb remember --title` exists, and the
/kb-reflect command always passes it, so the mid-sentence truncation never
appears on distilled facts. The remaining nice-to-have is a one-line **summary**
distinct from both the short title and the full body, surfaced in recall/SPA. If
kept: add `<meta name="kb-summary">` + a nullable `kb_summary` column (same
additive recipe as A3), a `--summary` flag on `kb remember`, and a `summary`
field on the recall response. Marked last and severable.

### The additive-column recipe (A3 `kb_created_unix`, and A4 `kb_summary` if kept)

Mirrors the v0.14 `kb_session` column end-to-end (verified file:line in recon):

1. `parser.rs` — add the field to the extracted `Fields` struct, a cached
   `Selector` for `meta[name="kb-created"]`, the extraction, the return-struct
   population, and the markdown-frontmatter override.
2. `storage/schema.rs` — add the `Doc` field, the Arrow `Field::new(_, _, true)`
   (nullable), the `placeholder` default, the `docs_to_batches` array builder,
   and bump the `schema_has_N_fields_in_order` count + name assertions.
3. `storage/lance.rs` — add to `SEARCH_PROJECTION`, the `DocSummary` field, the
   `batches_to_summaries` decoder, the `get_by_id`/`get_by_source_path`/
   `list_docs`/`list_docs_with_kb_session` projections, and a new idempotent
   `ensure_v18_*` migration (`add_columns(SqlExpressions[(col, "CAST(NULL AS …)")])`)
   called from `Storage::open`.
4. `indexer.rs` — populate the new `Doc` field from `fields.*` (no
   `doc_already_exists` clock games — the value comes from the parsed meta, so it
   is stable across reindex for free).
5. `memory.rs::render_artifact` — emit the meta (`kb-created` always when the
   caller passes `created: Some(now)`; `kb-summary` when `summary: Some(_)`).
6. `routes/artifacts.rs` — add `IngestBody.created_unix`/`.summary`, pass to
   `render_artifact`; the route computes `now` for `created_unix` so every
   memory written through the daemon is stamped.
7. `routes/memory.rs` — A3: source age from `kb_created_unix`; A4: add `summary`
   to `RecallResult` + populate from `ds.kb_summary`.
8. `kb-cli` — A4 only: `--summary` flag on `Remember` + payload wiring.

## Phasing (each commit green-CI; cadence `feat(crate): summary (PhaseID)`)

Tracks: **RA** near-term Rust · **RB** the command · **D** docs.

1. **RA1** `feat(kb-core)` — `pub DEFAULT_SALIENCE` + route uses it + inline-floor
   test. Pure hygiene; no schema; unblocks honest salience defaults.
2. **RA2** `feat(kb-core)` — `classify_research` digest-noise filter + test update
   + negative test. Cleans the distiller input. No schema.
3. **RA3** `feat(kb-core,kb-server)` — `kb-created` meta + `kb_created_unix`
   nullable column (v18 migration) + `render_artifact` stamp + ingest-route
   `now` + recall age re-source + rerank decay test (reindexed row still decays).
4. **RB1** `feat: kb-reflect command` — `plugins/kb-reflect/.claude-plugin/
   plugin.json` + `commands/kb-reflect.md` + marketplace entry +
   `docs/extending.md` row. **Works on the RA1–RA3 surface** (clean titles via
   existing `--title`, graded salience via the rubric, clean digests via RA2).
   Manually exercised end-to-end against the live daemon.
5. **RA4** `feat(kb-core,kb-server,kb-cli)` — *optional* `kb_summary` column +
   `--summary` + recall `summary` field. Ship only if deep-review keeps it;
   `/kb-reflect` does not depend on it.
6. **D1** `docs:` — note the command in `docs/extending.md`; if RA3 changes the
   decay-basis contract, add a one-liner to invariant #10's entry in
   `docs/architecture-invariants.md` (decay basis = `kb-created` meta, not mtime)
   and the kb-core CLAUDE.md if the column is kb-core-internal.

RB1 is the operator's headline deliverable and can ship after RA3 (it does not
need RA4). RA1/RA2 are independent quick wins; RA3 before RB1 (so distilled
facts age correctly from day one).

## Decisions

- **Command, not Rust verb, not skill.** The dream runs inside Claude Code via a
  user-triggered `/kb-reflect`. No `kb reflect` binary, no daemon worker, no
  `KB_REFLECT_MODEL`, no hosted model. (Corrects the original D2 framing.)
- **Dry-run/propose by default; human approves before any write.** No `--apply`
  auto-mode in v1. The operator sees the table, then writes happen.
- **Idempotency via `memory_count`, not new state.** Reuses an existing per-
  session field; `remember --session-id` lifts it.
- **Decay basis is a `kb-created` META, not a btime column and not a clock-at-
  index column.** The meta lives in the source → immune to reindex by
  construction → sidesteps the btime-less-FS silent-revert the research flagged.
- **`kb_summary` (A4) is severable.** Clean titles already work via `--title`;
  the summary column is a display upgrade, shipped last or dropped.
- **No new search/recall behavior.** `rerank` is untouched (#10). A3 only changes
  which timestamp feeds the *existing* age term; A2 only changes what counts as
  "research" in the *digest* (an #11 input), not recall.

## Risks

- **`/kb-reflect` writes bad facts.** Mitigated by dry-run-by-default + the
  salience rubric + the dedup `kb recall` pass + provenance (`--session-id` makes
  every fact traceable and `kb forget`-able). The operator is the gate.
- **Idempotency false-positive.** A session with a hand-written memory is treated
  as "reflected" (skipped). Acceptable for v1; the command accepts an explicit
  session id to force re-reflection. Document it.
- **Salience inflation (all-0.8).** The rubric + the >60%-one-bucket distribution
  check are mandatory parts of the command prompt, not optional advice.
- **MERGE correctness.** `--supersedes` takes one id; MERGE of N = supersede the
  primary + `kb forget` the rest. The command must spell this out, or it leaves
  orphan duplicates (the exact failure the distiller exists to prevent).
- **RA3 schema migration.** The additive-nullable `add_columns` is idempotent and
  matches four prior `ensure_vNN` migrations; the field-count test must bump or
  CI fails loudly (good). Pre-v18 rows fall back to `mtime` for decay (no worse
  than today).
- **A2 test churn.** `parse_extracts_research_signals` asserts `kb_search` is
  present; the filter inverts that — update it in the SAME commit + add the
  negative test, or CI red.
- **Scope creep.** Five Rust touch-points in RA3/RA4 across kb-core/server/cli;
  each phase is independently green-CI and the milestone can pause at any commit.
  The far-term graph/telemetry/eval work stays out.

## Verification

- Per Rust phase: `cargo test --workspace --exclude kb-embedder --no-fail-fast`
  + `cargo clippy --workspace --exclude kb-embedder --all-targets -- -D warnings`
  + `cargo fmt --all`. RA3/RA4 also: the schema field-count + round-trip tests.
- RA2: the updated + new `classify_research` tests; confirm the funnel/digest no
  longer count `kb recall`/Playwright as research (spot-check a real session via
  `kb sessions show <id> --json`).
- RB1 (manual, against the live daemon): run `/kb-reflect` over the real 196
  sessions; confirm (a) it skips `memory_count>0` sessions, (b) dry-run proposes
  a sane ADD/SUPERSEDE/MERGE/NOOP table with a non-flat salience spread, (c) on
  approval `kb remember --title …` writes clean-titled, graded, provenance-
  stamped facts, (d) re-running skips the just-reflected sessions. Verify the
  written facts surface in `kb recall` and the SPA `/memory` view.
- Regression: `kb recall` latency unchanged (#10 untouched); idle CPU unchanged.

## Critical files

- **A1:** `crates/kb-core/src/memory.rs:22` (const), `crates/kb-server/src/routes/memory.rs:335-341` (floor + comment + test).
- **A2:** `crates/kb-core/src/sessions.rs:829-886` (`classify_research`), `:634-640` (call site), `:1494-1517` (pinned test), `:892-934` (`kb_cli_query` VERBS).
- **A3:** `crates/kb-core/src/parser.rs` (Fields + selector + extract), `crates/kb-core/src/storage/schema.rs` (Doc + Arrow + count test + builder), `crates/kb-core/src/storage/lance.rs` (`SEARCH_PROJECTION` ~:589, `DocSummary` ~:130, decoder ~:1557, projections, new `ensure_v18_*` + `Storage::open` call ~:359), `crates/kb-core/src/indexer.rs:1220-1286` (Doc populate), `crates/kb-core/src/memory.rs:229` (`render_artifact` stamp), `crates/kb-server/src/routes/artifacts.rs:25-128` (IngestBody + call), `crates/kb-server/src/routes/memory.rs:357-369` (age source).
- **A4 (optional):** the same A3 sites for `kb_summary` + `crates/kb-cli/src/main.rs:82-135` (`--summary` arg) + `crates/kb-cli/src/commands/memory.rs:57-157` (payload) + `routes/memory.rs:53-112` (`RecallResult.summary`).
- **RB1:** `plugins/kb-reflect/.claude-plugin/plugin.json` (new), `plugins/kb-reflect/commands/kb-reflect.md` (new), `.claude-plugin/marketplace.json` (one entry), `docs/extending.md` (one row).
- **Reference:** `docs/research/evolving-the-kb-memory-system.html` (the design authority), `plugins/kb-comments/commands/kb-comments.md` (command template), `plugins/kb-memory/` (memory plugin sibling).

## Deep-review outcome (folded 2026-06-29)

3 parallel Opus reviewers (Rust-integration · command-design · invariants/scope), all
**GO WITH FIXES**. Corrections below **override the prose above** where they conflict.

**Re-sequenced to the reviewers' unanimous MVV — ship the dream first:**
`RA1 → RA2 → RA-recall → RB1` is the minimum viable "runnable dream". `RA3`/`RA4`
follow (the operator asked for all near-term items, so they ship after RB1, not
before it — RB1 does **not** depend on them; decay is negligible while facts are
days old).

**CRITICAL — canonical session id (RB1).** `kb sessions list/show` surface a
TRUNCATED `session_id` (e.g. `e47fef1c-…-ac90-`, the old `cut -c1-24` capture-hook
artifact) while a memory's `kb_session` carries the FULL canonical UUID. Two
consequences: (a) `count_memories_for_session('<truncated>')` won't match a
full-id memory → `memory_count` stays 0 → re-reflect forever; (b) stamping the
truncated id re-breaks the `claude -r` link (#11's named hazard). **Fix:** RB1's
step 0 verifies ids are canonical — if any listed `session_id` is not a 36-char
UUID (trailing dash / wrong length), run `kb reindex --kb sessions` once (the
JSONL-recovery in `parse_session_html_full`, sessions.rs:273-278, restores the
ground-truth id), then re-list. The command stamps `--session-id` ONLY with a
validated full UUID; otherwise it omits `--session-id` and records provenance via
a body `<a href="/a/sessions/<source_relative>">` permalink (rides the #29 edge
graph, queryable as a backlink) — never a truncated stamp.

**CRITICAL — RA2 must be NARROW.** Filter ONLY the self-referential memory verbs
(`recall`/`remember`/`recollect`/`why`/`forget`) and Playwright/browser UI tools
(`mcp__plugin_playwright_*`). **KEEP `search`/`find`/`related` as research** — they
are genuine signal when an agent searches the corpus during real work, and the
design authority filters only the write/recall verbs. Implement by splitting the
`kb_cli_query` `VERBS` list into RESEARCH_VERBS (search/find/related → keep
`kb_search`) vs SELF_VERBS (recall/remember/recollect/why/forget → return `None`).
This keeps the two e2e tests that assert `kb_search` for `kb search "…"`
GREEN (`end_to_end.rs` `session_research_extracted_and_served` ~:9719,
`research_rollup_and_funnel_aggregate_sessions` ~:9934) and preserves the funnel
`searched` stage. Verify `parse_extracts_research_signals` (~:1494) uses
`kb search` (kept) and add a negative test for a `recall`/Playwright block.

**HIGH — dedup oracle is floor-blind (RA-recall + RB1).** `kb recall` drops
salience≤floor non-pinned memories (`routes/memory.rs:340`), so the dedup pass is
blind to exactly the messy low-salience facts (moot today — all 13 sit ≥0.5 > the
0.15 floor — but a hard deliverable per the design authority). **Fix:** add a tiny
`kb recall --no-floor` flag (a `?floor=loose` query param → `DecayPolicy::Loose`
in the route's pre-filter; `rerank` untouched, #10-clean, ~10 lines) and have RB1
use `--no-floor --limit 50` for the dedup pass; instruct conservative bias
(prefer NOOP/SUPERSEDE over ADD when uncertain). New phase **RA-recall**.

**HIGH — `sessions show --json` carries STRUCTURED fields, not digest prose
(RB1 + A2 claim).** `SessionDetailResponse` returns `session`
(first_user_prompt capped 200 chars, git_branch, …), `decisions`, `commits`,
`files`, `touches`, `memories`, `readings` — NOT the `researched:` line or the
digest text. So: (a) the command's distiller input is this structured JSON (rich
enough: prompt + decisions + commits + files), optionally augmented by
`kb cat <session-artifact>` for the full digest; (b) **drop the claim that A2
"cleans the distiller input"** — re-justify A2 on funnel/rollup + recollect-digest
correctness (still valuable), and note it only feeds the command if the command
reads the digest via `cat`.

**HIGH — `--link` is visibility, not the corpus target (RB1).** Step 6's
`--link <kb>` is wrong: `--link` scopes VISIBILITY (and makes the memory
non-global). **Fix:** choose the write corpus via `--kb`/`--scope`, and map the
salience tier to visibility — user-identity/preference → `--global` (the default,
omit `--link`); project decision → `--scope project --kb <projectkb>` (or
`--link <projectkb>`).

**MEDIUM — MERGE = hard `kb forget`, not drop-at-recall (RB1).** `--supersedes`
takes ONE id (a recall-time tombstone); `kb forget` DELETES the file
(irreversible). Precise recipe: SUPERSEDE (1:1) = `kb remember --supersedes <id>`;
MERGE (N:1) = write the merged fact, then `kb forget` ALL N source ids (hard
delete — the dry-run MUST list exactly which ids will be deleted). Remove the
"(drop-at-recall)" mislabel on the `forget` path.

**MEDIUM — within-run dedup + dual-mode (RB1).** Two candidates distilled in one
run aren't deduped against each other (recall can't see a memory written
milliseconds earlier; the index lags). The command must dedup the CANDIDATE SET
in-memory before any write. And spell out two modes: `/kb-reflect` (no args →
filtered sweep over `memory_count==0`) vs `/kb-reflect <session-id>` (forced,
ignores the gate). `memory_count` means "has ≥1 curated fact," not "fully
distilled" — document the caveat. `--folder` wants the FULL cwd path (or filter
client-side on the `folder` basename field).

**MEDIUM — RA3/RA4 column recipe corrections.** (i) Field-count is **40**, with
TWO assertions to bump: `schema.rs:543` AND `schema.rs:691`. (ii) `render_artifact`
has test callers that pin its 10-arg signature (`memory.rs:454/486/601/628`,
`meta_edit.rs:655/668`) — adding params updates ALL of them; prefer trailing
`Option` params. (iii) RA4's `summary` on `RecallResult` flows to ts-rs
(`web/src/api/generated/RecallHit.ts`) — RA4 must run `just types` + commit, and
the field name must not clash with the existing `DocSummary.summary` body-excerpt.
(iv) RA3 is forward-only (existing 13 facts keep the `mtime` fallback until
rewritten) — pair it with a one-shot stamp during the no-LLM cleanup, or accept
the legacy fallback explicitly. Decay-basis options at implementation: a new
`kb_created_unix` column (semantically clean, ~8 touch-points) OR override the
already-plumbed `created_unix` column from the meta (fewer touch-points but
overloads the gallery "created" sort) — pick the new column for clarity.

**Salience distribution check is advisory, not enforceable.** Reframe as a
dry-run HISTOGRAM the operator reads (catches a flat spread); the human gate is
the real enforcement.

**Verified correct by all three reviewers:** #10 untouched on the hot path (A3 is
a pure age-input swap; existing order unchanged on day one via NULL→mtime
fallback); #11 pull-only intact (reads digests, writes to the memory corpus, R0
holds); #26 intact (no model in the daemon — the live Claude session is the
distiller); `memory_count` is a live recount, multi-capture newest-superset-safe;
`kb sessions show --json` is bounded (~23 KB even for a 2.6M-token session); the
RA1 duplication, the A3 btime trap avoidance, the additive-column recipe sites,
and the plugin/command scaffold are all accurate against HEAD.

### Final phase order

`RA1` (pub DEFAULT_SALIENCE + route test) → `RA2` (NARROW digest filter) →
`RA-recall` (`kb recall --no-floor`) → **`RB1` (the `/kb-reflect` command — the
dream, ships here)** → `RA3` (kb-created decay basis) → `RA4` (optional kb_summary)
→ `D1` (docs). Each green-CI; RB1 is the operator's headline and the MVV boundary.

## Shipped (2026-06-29)

All near-term + mid-term work landed on `main`, each green-CI:

- **RA1** `aa611591` — `pub DEFAULT_SALIENCE` re-exported to the recall floor + e2e inline-floor test.
- **RA2** `349042d7` — narrow digest filter: drop self-referential memory verbs (recall/recollect/why/remember) + Playwright MCP from `classify_research`; keep search/find/related (both research e2e tests stay green).
- **RA-recall** `5b5e9b4c` — `kb recall --no-floor` (`?no_floor=true`) dedup oracle; `rerank` untouched.
- **RB1** `1d5ea83d` — `plugins/kb-reflect/` + `/kb-reflect` command + marketplace + docs/extending row. Read+dedup pipeline verified end-to-end against the live daemon (list → reindex-detect → show → recall --no-floor).
- **RA3** `0800a370` — decay basis = write-time `kb-created` meta (parser + indexer prefer-meta + render_artifact stamp + ingest route `now` + recall age source), reusing the existing `created_unix` column (no new column/migration — the deep-review's minimal path).
- **D1** — invariant #10 entry updated (decay basis, `--no-floor`, DEFAULT_SALIENCE).

**RA4 (kb_summary column) — SHIPPED on operator request (2026-06-30), after an
initial deferral.** The review flagged it as the lowest-value item; the operator
chose to ship it anyway plus the SPA surfacing.
- **RA4** `0fdbf07f` — `kb-summary` meta → nullable `kb_summary` lance column (v18
  migration, full additive recipe mirroring `kb_session`: parser, schema +
  field-count 40→41 both asserts, lance projection/DocSummary/decoder/4 projections/
  migration, indexer, `render_artifact` param + emit, ingest body, `RecallResult.
  summary`, `RecallHit→Scored→RecallResult` passthrough, `kb remember --summary`,
  ts-rs regen of `RecallHit.ts`).
- **RA4-SPA** `6693e8b5` — render the summary as a muted one-line gloss beneath the
  title in the `/memory` table and the PreviewInspector "Related memories" popover.
- `/kb-reflect` Step 8 passes `--summary` so distilled facts carry a clean gloss.

**Deployed (2026-06-30).** Full rebuild + redeploy: the deploy script
(SPA + daemon docker image + container restart, sha-verified) AND the local
`~/.local/bin/kb` release binary, so `/kb-reflect`'s `kb recall --no-floor`, the
`kb_summary` column, and the RA3 decay basis are all live on the 127.0.0.1:4000
backend.
