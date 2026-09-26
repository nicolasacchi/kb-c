# AGENTS.md — working on kb-c

kb-c is a C17 port of [kb](https://github.com/nicolasacchi/kb), a self-hosted
system of record for everything an AI agent writes: a daemon that watches
folders of HTML and Markdown, indexes them, and serves them over an HTTP API
and a CLI. See `PORT_PLAN.md` for the port's scope and `INVENTORY.md` for the
module-by-module map of the Rust original.

## What this repo is

- `include/kbc/*.h` — **the frozen contract.** 14 headers, no implementation
  details beyond the ones a caller must know. Read them before writing code.
- `src/*.c` — the implementation, one file per subsystem.
- `cli/main.c` — the `kbc` binary.
- `tests/` — ctest-driven, one binary per file, using the harness in
  `kbc_test.h`.

## The rules that make C survive contact with a real codebase

1. **The headers are frozen.** Do not edit `include/kbc/*.h` to make your
   implementation fit. If the contract is genuinely wrong or missing, say so
   in your final report with the exact signature you need — the orchestrator
   changes the header, not you. Two agents changing one header is how a port
   rots.
2. **Every fallible function returns `kbc_status` and takes `kbc_err *`.** Fill
   `err` with `kbc_err_set` and a human message that names the offending value
   ("open /x/y: No such file or directory"), never "error". A caller that
   passes `err == NULL` must be safe.
3. **Three ownership tags, no fourth.** `KBC_OWN` (caller frees, exactly once),
   `KBC_ARENA` (dies with its arena — never free it individually, never let it
   outlive the arena), `BORROWED` (valid only as long as the input; never
   mutate, never free). If a value has no tag, that is a bug in the header:
   report it.
4. **Request-scoped memory goes in an arena.** Parsed JSON, search rows,
   artifact records, tokenizer output: one `kbc_arena` per request, reset or
   freed at the end. `malloc` is for state that outlives a request (store,
   index, config, httpd).
5. **No global mutable state** except the logger, which is already
   thread-safe. No static caches, no singletons, no lazy globals.
6. **Check every return value.** `kbc_str_append`, `sqlite3_*`, `write`,
   `fread`, `mmap` — all of them. A `-Wunused-result` warning is a defect, not
   noise; the build uses `-Werror`.
7. **Bounds before arithmetic.** Every `malloc`/`realloc`/`memcpy` length is
   either bounded by a `KBC_MAX_*` constant or provably in range. No
   `int` where a size belongs; multiply with an overflow check where the
   operands are not both compile-time constants.
8. **Lengths travel with strings.** A `const char *` that may contain an
   embedded NUL must travel with its `size_t len`. Do not assume NUL-termination
   on anything that came from a file.
9. **Input is hostile.** Corpus files, query strings, HTTP bodies and config
   files are attacker-influenced in the threat model. Validate lengths, reject
   `..` in any path that reaches the filesystem, and never format a path or a
   query fragment into a shell command, an SQL string or a log line without
   escaping it. All SQL goes through bound parameters.
10. **Fail loudly, once.** A function that fails does not half-apply. Either
    the artifact is stored and indexed, or it is not; never "stored but the
    index is stale" left silent — `kbc_app` owns that transaction boundary,
    individual units report the failure upward.

## Style

- 2-space indent, 80-column soft limit, `snake_case`, `KBC_`-prefixed public
  names, file-static helpers named `static` with no prefix.
- One declaration block per idea; initializers at declaration where it reads
  better than in the body.
- Comments explain *why*, and cite the invariant. A comment that restates the
  code is deleted, not written.
- No `goto` except in error-unwind paths, where it is the clearest form.
- Prefer `ssize_t`/`size_t` discipline from the `*_read`/`*_write` helpers over
  raw `read`/`write` loops.

## Build and test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# the memory-safety lane — this is the one that must be green before any commit
cmake -B build-asan -DKBC_SANITIZE=ON -DKBC_BUILD_TESTS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Warnings are errors: `-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual
-Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2`.

**Subagents do not run these.** Build, test and format are the orchestrator's
gate, run once over the union of the changes. Edit files; do not run
`cmake`, `ctest`, `clang-format` or `git commit`.

## Tests

`tests/kbc_test.h` is the framework: `KBC_TEST(name)`, the `KBC_CHECK*`
family, `kbc_test_tmpdir` for a scratch directory, and a `main` that runs the
cases and returns non-zero on failure. Every behaviour a caller can observe
needs a test that fails when the behaviour is wrong — boundaries, error
returns, empty and malformed input, and invariants that must hold across a
sequence of calls. Tests that assert a constant equals itself, or that a
function returned, are noise; do not write them.
