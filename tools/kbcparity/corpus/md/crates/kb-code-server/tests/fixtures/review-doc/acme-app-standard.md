---
schema: kbc-review/1
summary_md: |
  Adds a retry path to checkout. The dedup guard in [[code:app/models/order.rb:12]]
  is the load-bearing part; everything else is plumbing.
risk:
  level: medium
  why: touches the money path, but behind an existing feature flag
reading_order:
  - chapter: The guard
    stops:
      - ref: code:app/models/order.rb:12-14
        why: the new dedup key
      - code:app/models/order.rb:3
  - chapter: Plumbing
    stops:
      - ref: code:lib/retry.rb:1
        why: read after the guard
blocks:
  context: |
    Retries were double-charging. See [[finding:f-1]].
  tests: |
    One new spec covers the duplicate-submit path.
flows:
  - name: happy path
    steps:
      - code:app/models/order.rb:12
      - code:lib/retry.rb:1
questions:
  - to: to_author
    ask: Is the flag on in staging already?
    ref: code:app/models/order.rb:12
findings:
  - slug: f-1
    act: issue
    severity: concern
    category: correctness
    blocking: true
    title: Dedup key ignores the retry attempt
    rationale: |
      Two submits within the same second produce the same key, so the second
      charge is silently dropped rather than retried.
    recommendation: Include the attempt counter in the key.
    location:
      path: app/models/order.rb
      kind: single
      lines: [12]
    cites:
      - code:lib/retry.rb:1
      - sym:Order#checkout!
  - act: praise
    severity: ok
    category: tests
    title: The duplicate-submit spec is exactly the right shape
    rationale: It fails without the guard and passes with it.
    location:
      path: spec/order_spec.rb
      kind: whole_file
---

# Retry on checkout

The guard lives at [[code:app/models/order.rb:12-14]] and is exercised by
[[sym:Order#checkout!]]. The entity is [[ent:Order]]; the upstream discussion
is [[gh:comment/12345]] and the design note is [[kb:research/abc123]].

A bare [[Order]] is a kb wikilink, not a kbc ref.

```
[[code:not-a-ref.rb:1]]
```

See also [[hunk:app/models/order.rb@1#0]].
