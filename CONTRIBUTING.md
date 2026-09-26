# Contributing to kb-c

## Sign-off

Every commit carries a DCO sign-off:

```bash
git commit -s -m "fix: bound the tokenizer's term length"
```

`git log` on any commit without a `Signed-off-by:` trailer is a rejected commit.
Squash or rebase with `-s` too — rebase drops trailers unless you tell it not
to.

## Build, test, sanitize

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKBC_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

cmake -B build-asan -DKBC_SANITIZE=ON
cmake --build build-asan -j
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure
```

The sanitizer lane must be green before any commit. Warnings are errors; a new
`-Wunused-result` is a defect in your code, not noise to be silenced.

## The headers are frozen

`include/kbc/*.h` is the contract. Do not edit a header to make your
implementation fit. If the contract is wrong or missing a piece you need, say
so in the PR description with the exact signature you require, and let the
orchestrator change the header. Two agents editing one header is how a port
rots. The rules that go with the headers — ownership tags, arena discipline,
checked return values, bounded arithmetic, lengths travelling with strings — are
in [AGENTS.md](AGENTS.md); they are not stylistic preferences.

## What a good test looks like

A good test fails when the behaviour is wrong. Concretely: assert the value,
the error code, the ordering, the boundary. If you can break the implementation
on purpose and watch the test go red, it is doing its job.

A bad test restates the code. A test that asserts a constant equals itself, that
a function returned, that a list is non-empty, that a row appears twice with the
same path, or that a call did not throw, is noise and will be deleted in review.
The harness (`tests/kbc_test.h`) gives you `KBC_TEST(name)`, the `KBC_CHECK*`
family and `kbc_test_tmpdir`.

Cover, at minimum: the error return and the message content; empty and
malformed input; both sides of every boundary constant in `kbc.h`; and any
invariant that must hold across a sequence of calls (index sealed then
searched, store and index swapped together, arena freed once). For a bug, add
the regression test that fails before the fix.

## Review bar

A reviewer is checking: does the change do one thing; does it keep to the frozen
headers; are all fallible returns checked; does the error message name the
offending value; is every `malloc`/`memcpy` length bounded or provably in
range; does the test fail when the behaviour breaks; and does the commit
description say why, not what. A change that alters a ranking constant, an id
format, the storage schema or an HTTP route is a behaviour change and needs to
be argued against the Rust original in the description, not just asserted.
