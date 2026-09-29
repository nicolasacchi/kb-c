# Notes as connective tissue — wikilinks + backlinks (design + review spec)

Status: IMPLEMENTED on `main` (track W, commits 036530e2 → f0a8bcf3). This spec
records the design and serves as the deep-review input: every claim below cites
the code so a reviewer can verify it matches.

## Goal

Evolve kb notes from isolated todo-lists into the **connective tissue** of the
corpus: a note's `[[target]]` / `[[target|alias]]` links to any other artifact,
note, or session; backlinks ("Linked from" / "Referenced in") surface the
inverse. Research-backed (Obsidian-competitive analysis: backlinks/wikilinks is
the one high-value, on-brand new capability; graph view / daily-notes / plugins
are deliberately refused). Built entirely on the **existing** `edges` table — no
new entity, no new storage.

## Architecture (invariant #29)

1. **Parsing + resolution: `kb_core::links` (pure, LLM-free).**
   - `parse_wikilinks(body)` — `crates/kb-core/src/links.rs`. Uses comrak's
     wikilink AST (`wikilinks_title_after_pipe = true`) so code spans/fences are
     skipped for free (same approach as `notes::scan_tasks`). Returns
     `WikiLink { target, alias }` in document order, duplicates kept.
   - `resolve(target, &[DocLite])` — deterministic first-hit ladder: id (12-hex)
     → source-relative path (with/without extension) → exact title (ci) → unique
     basename (ci). Returns `One(id)` / `Ambiguous(ids)` / `None`. Pure; the
     caller supplies candidates.
   - `normalize_target` strips `#fragment`, leading `/`, backslashes.
   - 13 unit tests in `links.rs`.

2. **Edge recording: `EdgeRecordHook` (`crates/kb-core/src/enrich.rs`).**
   For a Markdown source containing `[[`, parses wikilinks from `ctx.raw_source`,
   resolves each distinct target against `list_docs(u32::MAX)`, and pushes
   `kind="link"` edges (deduped with the existing HTML-link pass, self-edges
   dropped). `EnrichCtx` gained `source_root` (set once in
   `indexer.rs::index_file`) to map candidate abs paths → rel.

3. **Storage: `backlinks_of(id)`** — reverse-edge query, sqlite
   (`storage/sqlite.rs`) + actor message + `StorageHandle` method
   (`storage/actor.rs`). `SELECT src_artifact, kind FROM edges WHERE
   dst_artifact = ?1 AND src_artifact != ?1`.

4. **HTTP (`crates/kb-server/src/routes/links.rs` + `notes.rs`):**
   - `GET /api/kb/{kb}/notes/{id}/links` → `{ outgoing: ResolvedLink[],
     backlinks: BacklinkRef[] }`.
   - `GET /api/kb/{kb}/backlinks/{id}` → `{ backlinks }` for any artifact.
   - `GET /api/kb/{kb}/wikilinks/suggest?q=&limit=` → `{ suggestions }`.
   - `NoteDetail.links` (outgoing, resolved) added — computed in `get_one` +
     `update`, gated on a literal `[[` (`outgoing_links` helper) so plain notes
     pay nothing.
   - `resolve_outgoing` reuses the SAME `kb_core::links::resolve`, so a rendered
     link and the recorded edge agree.

5. **SPA:**
   - `web/src/lib/wikilink.ts` — mirror grammar + `rehypeWikilinks(resolve)` hast
     pass + `makeResolver(NoteDetail.links)`. Golden-pinned by `wikilink.test.ts`
     (9 cases). Skips code/pre/a. Resolved → internal `<a>` (SPA navigate);
     unresolved → muted span.
   - `NoteMarkdown` consumes `links`; `NoteEditor` passes `note.links` +
     `wikilinkKb={kb}`.
   - `BacklinksSection` ("Linked from") under a note via `useBacklinks` →
     `/backlinks/{id}`; hidden when empty. Artifacts get "Referenced in" FREE —
     `PreviewInspector`'s existing Backlinks list rides the same `kind="link"`
     edges.
   - `[[` autocomplete: a second source on the existing slash `autocompletion`
     (`editor/slash.ts`), threaded as optional `wikilinkKb` through
     `MarkdownEditor → CodeMirrorInput → buildExtensions`.

6. **CLI (`crates/kb-cli/src/commands/notes.rs` + `main.rs`):**
   `kb notes links <target> [--json]`, `kb backlinks <target> [--json]`,
   link-count surfaced in `kb notes show`. Over the same HTTP API as the SPA.

## Render-path invariant (load-bearing)

`markdown::kb_options` does NOT enable the wikilink extension. Resolving a target
to a permalink needs storage; the pure render path lacks it. So served HTML keeps
`[[…]]` literal and the SPA renders links from the resolution map. A dedicated
`wikilink_parse_options()` enables the extension for the parse pass only.

## Tests

- `kb-core`: 13 `links::` unit + `backlinks_of` sqlite test.
- `kb-server` e2e: `note_wikilinks_resolve_edges_and_backlinks` (resolve at index
  time → edge → backlink; dangling stays dangling; suggest).
- SPA: 9 `wikilink.test.ts` cases (169 vitest total green).
- `kb-cli`: `notes_cli_links_and_backlinks`.

## Risks / things a reviewer should check

- Grammar drift between `kb_core::links` (comrak) and `web/src/lib/wikilink.ts`
  (regex) — they must agree on the common forms so the SPA map key matches the
  server's `target`.
- `list_docs(u32::MAX)` on every note GET/index with `[[` — acceptable on a
  personal kb? Any pathological corpus size?
- Ambiguous title resolution drops the edge silently — intended, but is it
  surfaced anywhere the user would want?
- The `[[` autocomplete's interaction with `closeBrackets` auto-pairing.
- Cross-kb: resolution + backlinks are corpus-local by design — confirm no path
  assumes cross-kb wikilinks.
