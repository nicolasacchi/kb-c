---
name: kb-distill
description: Distill one captured Claude Code session (or a --since window of them) into 1–3 durable kb memories — what was decided, what shipped, what failed and why. Use when a finished session had commits but produced no curated memory (the Stop-hook nudge names this skill), when the user asks to distill/keep what a session learned, or after substantial work worth remembering. Dedups against existing memory, quality-gates every write, stamps session provenance. Supports --dry-run.
---

# /kb-distill — episodic → semantic memory

One session's raw record in (digest: decisions, commits, files); **0–3 durable,
provenance-stamped facts** out, via `kb remember`. **You** are the distiller —
the daemon stays deterministic and LLM-free (#10/#26); the `kb` CLI is the only
path.

**Contract — the autonomous-safe half of consolidation** (`/kb-reflect` is the
corpus-wide, human-gated other half):

- **Write vocabulary = ADD · SUPERSEDE · NOOP.** `--supersedes` is a recall-time
  drop (file kept, reversible). **NEVER run `kb forget` on a pre-existing
  memory** — when a many-into-one MERGE looks right, report it and defer to
  `/kb-reflect`. One carve-out: an id you created **in this same run** that
  turns out to be a duplicate (Step 8 verify surfaces a pre-existing superset)
  may be `kb forget`-ed — that is undoing your own write, not consolidation.
- **Quality over quantity.** ≤3 memories per session; **zero is a valid,
  reportable outcome**. Never write a vague memory (Step 4 gate).
- **Idempotent.** Every write carries `--session-id`; a stamped session is
  skipped by the next window run, and recall-dedup catches the rest. Running
  the same input twice must produce zero new writes.
- Recall stays untouched; sessions stay pull-only (#11). You only WRITE facts.

## Arguments

- `<session-id>` — distill ONE session, even if it already has memories
  (forced; recall-dedup still guards against duplicates).
- `--since <7d | YYYY-MM-DD>` [`--folder <name>`] — window mode: every captured
  session started in the window; sessions that already produced a memory are
  skipped (the idempotency gate).
- `--dry-run` — run the full pipeline, print the Step 7 plan table, write
  **nothing**.
- No arguments → `--since 7d --folder <basename of cwd>`.

## Step 1 — resolve candidate sessions

```bash
kb sessions list --limit 60 --json     # add --folder <full cwd path> if known
```

Filter the `sessions` array client-side: `started_at` (epoch) in the window,
`folder` match. In window mode drop rows with `memory_count > 0` (already
distilled — list them as SKIP in the report). Don't auto-drop zero-edit
sessions: research sessions whose whole outcome is a published artifact still
yield a SHIPPED memory — judge from Step 2 evidence.

**Id hygiene (do not skip):** each candidate's stored `session_id` must be a
clean 36-char UUID (`8-4-4-4-12` hex). If any is truncated or carries a
trailing `-`, refresh from transcript ground truth — `kb reindex --kb
<the "kb" field from the row>`, wait ~3–5 s, re-list — then use the stored id
**verbatim**. Never hand-repair an id: `memory_count` and `kb sessions show`
key on the *stored* id, so a stamped-clean/stored-dirty mismatch silently
breaks idempotency and the session→memory link.

## Step 2 — bounded evidence per session

```bash
kb sessions show <session-id> --json
```

Use `session.*` (title, `first_user_prompt`, `cwd`, `git_branch`), `decisions`
(AskUserQuestion rulings: prompt + chosen answer), `commits`, `files`,
`research`, and `memories` (what's already stamped — read them so you don't
restate them).

- **Known digest defect:** `commits[].subject` may be a placeholder
  (`"commit"`/`"tag"`). The shas are real — recover subjects when
  `session.cwd` is a local repo:
  `git -C <cwd> show -s --format='%h %s' <sha>`.
- **Workflow results (post-M2):** if the payload carries them (a `workflows`
  array, or a `kb workflows` verb exists), fold their return values into the
  SHIPPED/FAILED evidence. Absent → skip silently; never fail on it.
- **Escape hatch (rare):** the raw transcript is multi-MB — never read it
  whole. Targeted, bounded greps only, e.g. the session's shell commands:
  `kb get <artifact_id> --kb <sessions-kb> --format html | grep -oE '"command":"([^"\\]|\\\\.){0,160}' | head -50`.
- **Not replaceable by `kb context` (CT-D1), on purpose:** the pack's
  `sessions` lane is POINTERS — ids, names, one-line digest excerpts —
  because invariant #11's R0/R1/R3 keep episodic material pull-only. This
  step is the pull. `kb sessions show` is one call and stays one call; folding
  it into the pack would put transcript-derived bodies into a surface the
  turn-1 hook reads.

## Step 3 — extract candidates (three lenses, ≤3 total)

- **DECIDED** — a ruling that constrains future work: option chosen (and why),
  convention adopted, refusal recorded, direction locked.
- **SHIPPED** — what durably landed, with its end state: commits on main, a
  deploy, a published artifact ("live on X" vs "on main, NOT deployed").
- **FAILED** — what didn't work *and why*: a reverted approach, a root-caused
  incident, a dead end a cold agent would otherwise re-walk. Write these with
  `kb remember --failed` (Step 7) so recall renders them with an explicit
  "✗ didn't work:" warning instead of reading like a positive fact.

Most sessions yield **one** memory (a SHIPPED with the decision folded in).
Merge lenses that tell one story; split only when the facts would be recalled
by different future queries.

## Step 4 — the quality gate (all four, or the candidate dies)

1. **Outcome as fact** — past tense, falsifiable.
2. **The why** — rationale or failure cause, in the body.
3. **≥1 concrete citation** — commit sha, artifact id/path, file path, exact
   command, or URL.
4. **Cold-agent test** — three months from now an agent with zero context sees
   this as a three-line recall hit: does it change what they do?

Rejects, by example: *"worked on improving session memory"* (no outcome) ·
*"fixed a bug in the indexer"* (no citation, no why) · *"discussed CI options"*
(no decision). Form: absolute dates only (write `2026-07-06`, never "today");
title = outcome-first and COMPLETE — a full sentence, never hard-chopped
mid-word (the old "80-char cutoff" premise was false: nothing in the
recall/render path truncates; 231 of 245 live memories were needlessly
chopped mid-word by this rule); aim for concise but never cut a title to hit
a number; `--summary` is MANDATORY on every write — a one-line,
self-contained gloss distinct from the title, because the recall/wake hooks
inject the summary under the title and it is what a future session actually
reads.

## Step 5 — dedup (two layers, two arms)

1. **Batch self-dedup** — merge near-identical candidates across sessions
   *before* consulting the oracle (it can't see a fact you wrote seconds ago).
2. **The oracle, per survivor — BOTH arms, union the neighbours:**
   ```bash
   # arm 1 — the neighbourhood, in ONE call (CT-D1)
   kb context "<candidate text>" --cwd <session cwd> --session <session-id> \
     --no-floor --json
   # arm 2 — the content-ranked half; still its own call, see below
   kb search "<candidate key tokens>" --kb <memory-corpus> --limit 5 --json
   ```
   **Arm 1 is `kb context`, not a hand-chained assembly.** The pack's
   `memories` lane IS the recall arm (`--no-floor` is a straight passthrough
   to `kb recall --no-floor`; identical ranking math, floor bypassed) and it
   arrives with the full `score = rel × salience × decay` decomposition, each
   hit's `linked_kbs`/`global` (which Step 6's ladder needs), and its
   `flagged`/`warns`/`drift_open` marks. In the SAME call the pack also hands
   you `sessions` pointers, `comments` (open comments on artifacts this
   candidate matches — a reviewer objecting to the very claim you're about to
   write) and `code_hints`, none of which the old three-call chain could see.
   Read `scent` first: it is the counts line, so an all-zero pack means
   "nothing to dedup against" without reading four lanes.

   **Arm 2 stays a separate call, deliberately.** The two arms have disjoint
   blind spots: recall is **visibility-ranked** (rank × salience × decay — in
   a corpus of 0.85–0.9-salience memories it buries a mid-salience true
   duplicate even with `--no-floor`, which lifts the floor but not the
   ranking), while corpus search is **content-ranked** and salience-free.
   `kb context` does not and must not fold arm 2 in: a pack that silently
   mixed content-ranked hits into a visibility-ranked lane would be lying
   about its own ordering, and the decomposition it prints would stop
   explaining the rows. Run the search arm against every memory-scope corpus
   (typically the project one + `memory`). Skipping the search arm is how a
   verbatim duplicate slips through.

   Classify the union: **NOOP** (already said → drop, report why) ·
   **SUPERSEDE** (an existing memory is now outdated/subsumed → note its
   `id`) · **ADD** (genuinely new). Uncertain → prefer NOOP/SUPERSEDE over
   ADD. Several old ids collapsing into one → **defer to `/kb-reflect`**,
   never delete here.

   Also read any `memories` already stamped on the session (Step 2) — but
   don't trust the Step 1 gate as dedup: `memory_count == 0` only means no
   memory is *stamped to this session id*; the same fact may have been
   curated from a different session. The oracle is the real guard.

## Step 6 — grade + scope

**Salience** (same rubric as `/kb-reflect` — don't flatten to one bucket):
project decision / milestone **0.6–0.7** · operational fact / gotcha
**0.5–0.6** · user preference / correction **0.8–0.9** (rare from distill).

**Visibility ladder** — first rung that holds wins:
1. Dedup neighbours exist → inherit their `linked_kbs` (already on each
   Step 5 arm-1 pack hit — no second recall call for this field).
2. `--link` the kb(s) that serve that project's artifacts (session `folder` →
   kb-name prefix match, e.g. folder `kb` → `kb-docs`).
3. No plausible kb → global (omit `--link`).

`--link` validates server-side; an unknown-kb 400 writes **nothing** — on 400,
retry with the next rung. Corpus: plain `kb remember` resolves the
project-scope memory corpus; pass `--kb` only to target a different one.

## Step 7 — the plan table (always) → write (unless --dry-run)

| session | lens | title | class | salience | link | citation | supersedes |

…plus one SKIP row per skipped session with the reason (`already has memory
<id>` / `no durable outcome` / `zero-signal`). **`--dry-run` stops here.**

Otherwise write each ADD/SUPERSEDE:

```bash
kb remember "<body>" --title "<title>" --summary "<gloss>" \
  --salience <g> --tags <a,b> [--link <kb,kb>] [--failed] \
  [--supersedes <old-id>] --session-id <clean-uuid>
```

**FAILED-lens candidates get `--failed`** — it marks the memory as a
tried-and-did-NOT-work outcome (`kb-outcome: failed` + the `outcome:failed`
tag), so recall injects it with a "✗ didn't work:" warning. Only for
memories whose *point* is the failure (a dead end, a reverted approach);
a SHIPPED memory that merely mentions a fixed bug is not `--failed`.

`--session-id` is **mandatory** — provenance plus the idempotency stamp (it
lifts the session's `memory_count`, so the next window run skips it). It takes
one id; when several sessions contributed, name the others in the body.

## Step 8 — verify + report

Verify each write on both axes, mirroring Step 5's two arms exactly:
**content** — `kb search "<its key tokens>" --kb <memory-corpus> --limit 3`
must rank it in the top hits (this is also your last duplicate check: a
pre-existing superset ranking beside it means you just wrote a dup — undo it,
see the carve-out above); **visibility** —
`kb context "<a natural future query>" --json`, whose `memories` lane is the
recall arm a real future turn will actually see (a fresh mid-salience memory
may legitimately rank below older 0.9-salience giants on vague queries;
near-topic queries must surface it). Note the ABSENT `--no-floor` here: this
check must run against the floored population a normal turn gets, not the
oracle's. Report counts:
written · superseded · NOOPed · skipped(+why) · deferred-to-reflect. State
explicitly that a re-run over the same input now reports **zero writes**.
