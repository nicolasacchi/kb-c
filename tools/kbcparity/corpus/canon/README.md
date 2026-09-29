# Sample artifacts — reference canon

Five self-contained HTML artifacts from the Claude Design Web Gallery
handoff bundle `E4XcIWFBUnJJqWOWAaNBeA` (2026-05-11). They are the
canon the kb iframe sandbox (topic 06) and indexing pipeline (topic 07)
must support without modification.

## Files

| ID  | File                                | Pattern                       | Notes |
|-----|-------------------------------------|-------------------------------|-------|
| s01 | `fullscreen-viz.html`               | full-screen interactive       | Drag-to-resolve borrow checker. Single file, fills viewport. |
| s02 | `multi-page.html`                   | single-file w/ internal nav   | 4 in-page chapters, sticky TOC, no scroll-marathon. |
| s03 | `kitchen-sink.html`                 | component showcase            | `<details>`, `<dialog>`, native sliders, sortable tables, drag-to-reorder, image zoom, popovers, copy buttons. The iframe sandbox stress test. |
| s04 | `cost-of-abstraction.html`          | 4-page essay + internal index | Left-rail chapter index, drop-cap typography, pull quotes, inline backlink card to `s02` via `postMessage({type:'open-artifact', id:'s02'})`. |
| s05 | `pm/00-summary.html` + 3 siblings   | **multi-file**                | SEV-1 outage postmortem (Summary · Timeline · Root Cause · Action Items). Shared `pm/style.css`. Demonstrates the `pages` data shape and `pm:page` postMessage handshake. |

## Why these are canon

They extend an upstream 20-artifact reference set (cited in
`docs/research/surfaces/07-html-effectiveness.html`; the original
`kb-html-artifact-thesis.md` write-up is an internal planning document kept
outside this repository) with three patterns the upstream set didn't include:

1. **Multi-file artifacts** — `s05/pm/` is the only multi-file artifact
   in the combined canon. Introduces `pages: [{label, src, file}, …]`
   to the artifact data shape; introduces `pm:page` postMessage
   handshake for sub-page navigation.
2. **Full-screen interactive** — `s01` confirms the thesis that
   interactivity is a load-bearing HTML capability. Single file.
3. **Component-showcase stress test** — `s03` exercises every HTML
   primitive in one page. Useful as the v0.0.1 iframe sandbox smoke
   test ("does our sandbox break any of these?").

The other two (`s02`, `s04`) demonstrate single-file long-reads with
internal navigation and a cross-artifact backlink — both within the
existing canon's range.

## Sandbox notes

Per topic 06 (iframe sandbox), these artifacts should serve from a
per-artifact subdomain (`<id>.artifacts.localhost`) with
`sandbox="allow-scripts"` only — no `allow-same-origin`. The design
bundle's prototype used `sandbox="allow-scripts allow-popups allow-forms
allow-same-origin"`; that combination is storage-broken when the iframe
shares the parent's origin (the embedded script can simply remove the
sandbox attribute). See `10-web-gallery.html` §concerns #1.

## Provenance

Originally verbatim copies from `web-gallery/project/artifacts/` in the
design handoff bundle. **Post-review correction (2026-05-11):** the
Google Fonts `<link>` tags were stripped from `multi-page.html`,
`kitchen-sink.html`, `cost-of-abstraction.html`, and all four
`pm/*.html` so the canon matches topic 07's "0/20 external resources"
claim. The CSS variable fallback stacks (Georgia / system-ui /
ui-monospace) handle font rendering without external loads. The
prototype's Google Fonts intent is preserved as the *primary* font
name in each `--serif` / `--sans` / `--mono` variable — browsers
silently fall back when the named font isn't present.

To extend the canon for kb-specific tests, add new files alongside
these rather than editing. New artifacts should also avoid external
resources to keep the canon aligned with topic 07.

The full raw bundle (including the React/Babel prototype, alt layout
explorations, and the design canvas) lives at
`docs/research/claude-design-handoff/web-gallery/` (kept outside this repository).
The TUI handoff bundle is its sibling at
`docs/research/claude-design-handoff/tui-platform/` (kept outside this repository).
