---
kb-category: note
kb-tags: ladder, parity
title: Ladder source
---

# Ladder source

One wikilink per rung of the resolution ladder the Rust original documents
(`crates/kb-core/src/links.rs:200-224`). Every rung that can be ambiguous
gets an ambiguous case here, because an ambiguous rung that is never exercised
is a rung nobody has measured.

- tier 2, exact source-relative path: [[ops/deploy.md]]
- tier 2, extension-elided source-relative path: [[ops/deploy]]
- tier 3, exact title, two documents share it: [[Runbook]]
- tier 4, bare basename, two documents share it: [[deploy]]
- tier 4, bare basename, exactly one candidate: [[only-here]]
- tier 4, no candidate at all: [[no-such-thing]]
- an alias, which is display text and never a second identity: [[only-here|renamed]]
- an anchor, which is stripped before the ladder runs: [[only-here#a-section]]
- the same target reached by a Markdown link, so the edge is written twice
  from one document: [ops/deploy.md](ops/deploy.md)
