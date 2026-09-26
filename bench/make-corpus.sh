#!/usr/bin/env bash
# Stage a real corpus for the benchmark: copy the Markdown and HTML that
# already exists in the kb checkout into a flat tree, so both the Rust daemon
# and kb-c index byte-identical input.
#
#   ./bench/make-corpus.sh [SOURCE_REPO] [DEST]
#
# Defaults: SOURCE_REPO=../kb, DEST=bench/data/corpus. Prints the file count
# and total bytes so a benchmark run can record exactly what it measured.
set -euo pipefail

src="${1:-$(cd "$(dirname "$0")/../.." && pwd)/kb}"
[ -d "$src" ] || src="$(cd "$(dirname "$0")/.." && pwd)/../kb"
dest="${2:-$(cd "$(dirname "$0")/.." && pwd)/bench/data/corpus}"

rm -rf "$dest"
mkdir -p "$dest"

count=0
bytes=0
while IFS= read -r -d '' f; do
  rel="${f#"$src"/}"
  # Keep a stable, readable path: docs/research/index.html stays under docs/.
  out="$dest/$rel"
  mkdir -p "$(dirname "$out")"
  cp "$f" "$out"
  count=$((count + 1))
  bytes=$((bytes + $(stat -c%s "$f")))
done < <(find "$src" \
  -path "$src/target" -prune -o \
  -path "$src/node_modules" -prune -o \
  -path "$src/.git" -prune -o \
  -path "$src/.grokclaude-worktrees" -prune -o \
  -type f \( -name '*.md' -o -name '*.html' -o -name '*.htm' \) -print0)

printf 'corpus: %s\n' "$dest"
printf 'files:  %d\n' "$count"
printf 'bytes:  %d\n' "$bytes"
