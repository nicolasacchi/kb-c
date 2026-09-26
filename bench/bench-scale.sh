#!/usr/bin/env bash
# bench-scale.sh — kb-c at a corpus size where the index has to page.
#
# Every other kb-c number in this repo is measured at 1,114 documents. That
# exercises the index; it does not characterise it, because at 1,114 documents
# the whole index is a few megabytes and the entire working set lives in page
# cache for free. The central claim of the port is that the query path reads
# its own mmap'd inverted index instead of going through a database. That
# claim is only interesting where the posting lists are long enough and the
# index is big enough that the pages have to be faulted in, and this script is
# the one that goes there.
#
# What it does, for each corpus size given:
#   1. generates a synthetic corpus of that many documents (the generator is
#      embedded below; see "the corpus" in the header of the python program for
#      why it is built the way it is);
#   2. measures the full ingest wall clock from an empty data dir, plus the
#      peak RSS of the process that did it;
#   3. measures index file size, db size and idle RSS at rest;
#   4. measures per-query latency percentiles warm, and again with the index
#      file evicted from the page cache (posix_fadvise DONTNEED — no root, no
#      /proc/sys/vm/drop_caches, and it cannot touch another user's pages);
#   5. measures the incremental reindex cost of ONE file change at that size,
#      which is the path that was optimised at 1,114 documents and whose cost
#      model has never been checked at a scale where the index is big.
#
# Sizes form a ladder: the largest corpus is generated once and the smaller
# runs are hardlink views of its first N documents, so every rung sees the same
# generator, the same seed and the same term distribution, and the ladder
# measures document count and nothing else. The views cost no disk.
#
# On disk, deliberately NOT in /tmp: the sandbox lives under bench/data on the
# repo's real filesystem. /tmp is tmpfs here, and a 300 MB index on tmpfs is
# page cache by construction — the "does the working set still fit" question
# this script exists to answer would be unanswerable if the index were never
# on a real disk. Both paths are under /bench/data/, which is gitignored.
#
# Auth: the daemon is started WITH a per-run bearer token and every request
# presents it, because the deployed daemon requires one and a benchmark that
# quietly measured the no-auth path would be measuring a configuration nobody
# runs. It is one constant-time compare of a 48-byte token per request, and it
# is in every sample reported here rather than assumed away.
#
# Order within one size: ingest, warm, cold (evict + restart), incremental.
# The incremental step is last because it makes the daemon rewrite its index
# file, and a rewritten index the next start cannot reopen would take the cold
# numbers down with it.
#
#
# Usage: bench-scale.sh [DOCS_LIST] [PORT]
#        DOCS_LIST is one or more document counts, default "100000".
#        Pass several ("1000 5000 20000 100000") for the scaling ladder.
# Env:   QUERY_FILE (default bench/queries.txt), REPS (default 20, warm
#        samples per query), COLD_REPS (default 3, post-eviction samples),
#        INCREMENTAL_REPS (default 3), WORKERS (default 4), LIMIT (default 10),
#        KBC_BIN, SEED (default 20260926), WORD_SOURCE (default bench/data/
#        corpus, else the repo's own *.md), SHARD (default 500 docs/dir),
#        KEEP_CORPUS=1 to leave the generated corpus on disk, COLD=0 to skip
#        the page-cache eviction run, RUN_KBC_BENCH=1 to also run
#        `kbc bench --corpus` (in-process, own tmpdir) at each size.
# Output: per-size key=value on stdout, a summary table, progress on stderr,
#         raw per-sample TSVs under $SANDBOX/samples.
#
# The daemon: this script starts its own, from its own config, in its own data
# dir, and kills only the pid it started. It refuses to run if its port is
# already in use, so it can never touch a daemon it did not start.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCS_LIST="${1:-100000}"
PORT="${2:-${PORT:-4361}}"
KBC_BIN="${KBC_BIN:-$ROOT/build/kbc}"
SANDBOX="${SANDBOX:-$ROOT/bench/data/scale-sandbox}"
CORPUS_ROOT="${CORPUS_ROOT:-$ROOT/bench/data/scale-corpus}"
KB_NAME="scale"
REPS="${REPS:-20}"
COLD_REPS="${COLD_REPS:-3}"
INCREMENTAL_REPS="${INCREMENTAL_REPS:-3}"
WORKERS="${WORKERS:-4}"
LIMIT="${LIMIT:-10}"
SEED="${SEED:-20260926}"
SHARD="${SHARD:-500}"
COLD="${COLD:-1}"
RUN_KBC_BENCH="${RUN_KBC_BENCH:-0}"
KEEP_CORPUS="${KEEP_CORPUS:-0}"
QUERY_FILE="${QUERY_FILE:-$ROOT/bench/queries.txt}"
MODE="keyword"
URL="http://127.0.0.1:${PORT}"
SAMPLES="$SANDBOX/samples"
RESULTS="$SANDBOX/results.env"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
loadavg() { awk '{print $1}' /proc/loadavg; }

# The two query sets. bench/queries.txt is the one every other number in this
# repo was measured with, and it is run here too, unchanged, so the scale
# numbers join that table instead of starting a private one. Its terms are
# drawn from this repository's own vocabulary, which the generator's word
# source is, so they resolve against the synthetic corpus with a realistic
# document frequency. queries-scale.txt is generated alongside the corpus and
# holds queries chosen by TERM RANK, so posting-list length varies over four
# orders of magnitude between them; a query set of eight head terms measures
# one point on the curve, not the curve.

DAEMON_PID=""
cleanup() {
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    log "stopping daemon ${DAEMON_PID} (the one this script started)"
    kill -TERM "$DAEMON_PID" 2>/dev/null
    for _ in $(seq 1 240); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.25; done
    kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID" 2>/dev/null
  fi
  for _ in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ":${PORT} " || break
    sleep 0.25
  done
  if [ -n "${CORPUS_ROOT:-}" ] && [ "$KEEP_CORPUS" != "1" ]; then
    log "removing the generated corpus ${CORPUS_ROOT} (KEEP_CORPUS=1 to keep it)"
    rm -rf "$CORPUS_ROOT"
  fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

port_busy() { ss -ltn 2>/dev/null | grep -q ":${PORT} "; }

# ---------------------------------------------------------------- preflight
[ -x "$KBC_BIN" ] || { echo "ERROR kbc binary not found: $KBC_BIN" >&2; exit 2; }
[ -r "$QUERY_FILE" ] || { echo "ERROR QUERY_FILE not readable: $QUERY_FILE" >&2; exit 2; }
if port_busy; then
  echo "ERROR port ${PORT} already in use; refusing to touch a daemon this script did not start" >&2
  exit 2
fi
for t in python3 curl ss; do
  command -v "$t" >/dev/null || { echo "ERROR ${t} not found" >&2; exit 2; }
done

# The biggest size on the ladder: the corpus is generated once at that size
# and every smaller rung is a view of it.
MAX_DOCS=0
for d in $DOCS_LIST; do
  case "$d" in ''|*[!0-9]*) echo "ERROR DOCS_LIST must be document counts, got: $d" >&2; exit 2;; esac
  [ "$d" -gt "$MAX_DOCS" ] && MAX_DOCS=$d
done
[ "$MAX_DOCS" -gt 0 ] || { echo "ERROR empty DOCS_LIST" >&2; exit 2; }

# The word source: prose the generator draws its real term frequencies from.
# bench/data/corpus is the staged benchmark corpus; the repo's own Markdown is
# the fallback so the script works in a fresh checkout.
WORD_SOURCE="${WORD_SOURCE:-$ROOT/bench/data/corpus}"
if [ ! -d "$WORD_SOURCE" ] || [ -z "$(find "$WORD_SOURCE" -type f -name '*.md' -print -quit 2>/dev/null)" ]; then
  WORD_SOURCE="$ROOT"
fi

# Free space, checked before generating rather than after: a 100k-document
# corpus is ~200 MB of documents and the index that describes them is larger.
AVAIL_KB=$(df -Pk "$(dirname "$CORPUS_ROOT")" | awk 'NR==2 {print $4}')
log "corpus_root=${CORPUS_ROOT} max_docs=${MAX_DOCS} free_on_that_fs=$((AVAIL_KB / 1024))MB loadavg=$(loadavg)"
if [ "$AVAIL_KB" -lt 4194304 ]; then
  log "WARNING less than 4 GB free where the corpus goes; the run will probably fail on ENOSPC"
fi

rm -rf "$SANDBOX"
mkdir -p "$SAMPLES"
: > "$RESULTS"
: > "$SANDBOX/table.txt"

# ------------------------------------------------------------- the generator
# Embedded rather than a separate file for the reason bench-kbc.sh gives its
# load client: a harness others cannot read is worth less than one that lives
# in the script that uses it.
mkdir -p "$SANDBOX"
cat > "$SANDBOX/gen_corpus.py" <<'PYEOF'
#!/usr/bin/env python3
"""Synthetic corpus generator for kb-c's scale benchmark.

Realism is the whole point: a corpus of identical documents makes any inverted
index look fast for reasons that have nothing to do with the index. So the term
frequencies here are REAL -- extracted from prose in a word source -- and the
documents are topically coherent, variable in length, and mixed
Markdown/HTML with headings, lists, tables and code blocks.

Vocabulary
  * every prose token in the word source (bench/data/corpus by default),
    counted, with markup lines and kb-c's own stopword list removed, so the
    weight distribution is the Zipf curve real text has;
  * plus a fixed domain seed list (systems vocabulary) at plausible weights, so
    the generator does not depend on the staged corpus and so terms like
    epoll/inotify exist at all.

Shape
  * 70% of a document's content words come from a topic set drawn from the
    global distribution, 30% from the global distribution: that is what gives a
    document repeated terms (tf > 1) and topical coherence instead of a flat
    word salad;
  * content words are laid out in fixed-arity sentence frames, so a document
    reads as prose and not as a bag of tokens;
  * length is log-normal, clamped well under the 16 MiB artifact ceiling.

queries-scale.txt is emitted next to the corpus: one query per document-
frequency band, so the latency sample covers posting lists from tens of entries
to the whole corpus instead of one point on that curve.

Deterministic: same seed, same bytes.
"""
import argparse
import os
import random
import re
import sys
from collections import Counter

# kb-c's KBC_STOPWORDS (src/parse.c). A word on this list never survives
# kbc_tokenize, so it must never be a content word or a query term.
STOPWORDS = frozenset("""
a an the and or but if then else of to in on at by for with from as is are
was were be been being it its this that these those i you he she they we not
no nor so than too very can will just do does did done have has had having my
our your their his her them us me mine what which who whom when where why how
all any both each few more most other some such only own same s t don now
""".split())

# Domain seed: (word, weight) in the same units as the source counts
# (occurrences per ~1M prose tokens), so a seeded term lands in the same
# document-frequency band a real term of that frequency would.
DOMAIN_SEED = [
    ("epoll", 130), ("inotify", 90), ("mmap", 260), ("madvise", 40),
    ("varint", 55), ("posting", 70), ("postings", 60), ("tokenizer", 420),
    ("tokenize", 300), ("tokenized", 180), ("shard", 150), ("sharding", 70),
    ("corpus", 900), ("benchmark", 380), ("benchmarks", 150),
    ("throughput", 320), ("latency", 700), ("percentile", 210),
    ("percentiles", 80), ("concurrency", 260), ("concurrent", 340),
    ("sanitizer", 210), ("sanitizers", 60), ("asan", 120), ("ubsan", 80),
    ("tsan", 45), ("valgrind", 70), ("profiler", 90), ("profile", 300),
    ("sqlite", 620), ("vacuum", 110), ("checkpoint", 120),
    ("pager", 95), ("pagecache", 60), ("resident", 130),
    ("working", 500), ("workset", 70), ("readahead", 45), ("filesystem", 340),
    ("syscall", 180), ("scheduling", 150), ("scheduler", 160),
    ("threaded", 120), ("threading", 140), ("threadpool", 70),
    ("keepalive", 90), ("listener", 180), ("backlog", 90),
    ("sidecar", 60), ("inverted", 240), ("lexicon", 55), ("dictionary", 200),
    ("idempotent", 60), ("durable", 110), ("atomic", 220),
    ("regression", 300), ("benchmarking", 45), ("microbenchmark", 40),
    ("hypertext", 50), ("markdown", 620), ("frontmatter", 70),
    ("namespace", 260), ("namespaces", 60), ("iterator", 180),
    ("allocation", 210), ("allocator", 120), ("deallocation", 35),
]

# Sentence frames. Each consumes exactly ARITY content words; the function
# words around them are stopwords the tokenizer drops, so they cost bytes and
# nothing else -- which is what a real sentence is.
FRAMES = [
    ("The {0} of the {1} is {2} to the {3}.", 4),
    ("When the {0} {1} changes, the {2} {3} is {4} again.", 5),
    ("Every {0} that touches the {1} must also carry the {2} and the {3}.", 4),
    ("A {0} without the {1} leaves the {2} and the {3} out of range.", 4),
    ("The {0} is built once, then the {1} is read from the {2} and the {3}.", 4),
    ("Because the {0} is smaller than the {1}, the {2} costs less than the {3}.", 4),
    ("Under load the {0} and the {1} serialise on the {2}, so the {3} pays {4}.", 5),
    ("This is why the {0} is not the {1}: the {2} is the {3}, and the {4} is not.", 5),
    ("The {0} of the {1} is measured, not estimated; the {2} and the {3} are not.", 4),
    ("Once the {0} has been written the {1} is {2}, and the {3} is {4} only.", 5),
    ("Note that the {0} does not see the {1}, so the {2} stays and the {3} is stale.", 4),
    ("The {0} and the {1} answer the {2} from the {3}, never from the {4}.", 5),
    ("Between two {0} the {1} is rebuilt, and the {2} of the {3} is dropped.", 4),
    ("For a {0} the {1} is the {2}; for the {3} it is the {4}.", 5),
    ("The {0} holds the {1} and the {2} in one file, so the {3} and the {4} are adjacent.", 5),
    ("A query over the {0} touches the {1} and the {2} but never the {3} or the {4}.", 5),
]
# Bytes per sentence frame, measured off this file, and bytes per content word;
# together they turn a target document size into a content-word count.
FRAME_BYTES = 78
CONTENT_WORD_BYTES = 7
# Target mean document size, in bytes. Calibrated against the generator's own
# output: sentence frames add bytes on top of the content words, so the
# emitted mean is ~1.3x this. 1500 lands a 100,000-document corpus at ~200 MB.
MEAN_BYTES = 1500

MARKUP_LINE = re.compile(r"<|https?://|\]\(|www\.|&[a-z]+;|^\s*[-*+]\s*\[|^\s*\d+\.\s*\[")
TAG = re.compile(r"<[^>]*>")
TOKEN = re.compile(r"[A-Za-z][A-Za-z']{1,}")


def build_vocabulary(word_sources):
    counts = Counter()
    for root in word_sources:
        for dirpath, _dirs, files in os.walk(root):
            for name in files:
                if not name.endswith((".md", ".html", ".htm")):
                    continue
                try:
                    with open(os.path.join(dirpath, name), "rb") as fh:
                        raw = fh.read()
                except OSError:
                    continue
                text = raw.decode("utf-8", "ignore")
                if name.endswith(".html"):
                    text = TAG.sub(" ", text)
                for line in text.splitlines():
                    if MARKUP_LINE.search(line):
                        continue
                    for m in TOKEN.finditer(line.lower()):
                        w = m.group(0)
                        if len(w) > 1 and w not in STOPWORDS:
                            counts[w] += 1
    for w, weight in DOMAIN_SEED:
        if counts.get(w, 0) < weight:
            counts[w] = weight
    if len(counts) < 2000:
        sys.exit("ERROR: only %d vocabulary words extracted from %s; a corpus "
                 "built from this would be a word salad, not a corpus"
                 % (len(counts), ", ".join(word_sources)))
    return counts


def fill(frame, arity, chunk):
    if not chunk:
        return ""
    return frame.format(*[chunk[i] if i < len(chunk) else chunk[-1]
                          for i in range(arity)])


def code_block(topic, rnd):
    lang = rnd.choice(["c", "c", "c", "rust", "sh", "python", "toml"])
    if lang == "c":
        body = [
            "static kbc_status reindex_one(const kbc_index *ix,",
            "                            const kbc_tokens *toks) {",
            "  kbc_err e; kbc_err_reset(&e);",
            "  return kbc_index_bm25(ix, toks, 1.2, 0.75, 10, out, &e);",
            "}",
        ]
    elif lang == "rust":
        body = [
            "pub fn reindex(&mut self, path: &Path) -> Result<()> {",
            "    let text = std::fs::read_to_string(path)?;",
            "    self.index.upsert(path, &tokenize(&text));",
            "    Ok(())",
            "}",
        ]
    elif lang == "sh":
        body = ["#!/usr/bin/env bash", "set -euo pipefail",
                "./build/kbc reindex --config \"$CFG\""]
    elif lang == "python":
        body = ["def words(text):",
                "    return [w for w in text.lower().split() if w.isalpha()]"]
    else:
        body = ["[daemon]", "port = 4321", "workers = 4"]
    body.insert(0, "/* %s: one record per posting */"
                % (topic[0] if topic else "index"))
    return "```" + lang + "\n" + "\n".join(body) + "\n```"


def gen_doc(words, cumw, rnd):
    """One document. Returns (filename_with_extension, text)."""
    topic = rnd.choices(words, cum_weights=cumw, k=rnd.randint(5, 18))
    # Log-normal length: most documents are small, a few are large. Clamped so
    # no document approaches the 16 MiB artifact ceiling.
    size = int(rnd.lognormvariate(0.0, 0.85) * MEAN_BYTES)
    size = max(500, min(size, 90000))
    nwords = max(40, int(size / (FRAME_BYTES / 4.5 + CONTENT_WORD_BYTES)))
    n_topic_words = int(nwords * 0.7)
    body = (rnd.choices(topic, k=n_topic_words)
            + rnd.choices(words, cum_weights=cumw, k=nwords - n_topic_words))
    rnd.shuffle(body)

    title = " ".join(topic[:rnd.randint(2, 4)]).capitalize()
    blocks = []
    pos = 0
    n = len(body)
    while pos < n:
        frame, arity = FRAMES[rnd.randrange(len(FRAMES))]
        chunk = body[pos:pos + arity]
        pos += arity
        if len(chunk) < 3:
            break
        blocks.append(("p", fill(frame, arity, chunk)))
        r = rnd.random()
        if r < 0.28 and pos + 4 * arity < n:                 # bullet list
            items = []
            for _ in range(rnd.randint(2, 4)):
                items.append(fill(frame, arity, body[pos:pos + arity]))
                pos += arity
            blocks.append(("ul", items))
        elif r < 0.36 and pos + arity + 4 <= n:              # table
            head = fill(frame, arity, body[pos:pos + arity])
            pos += arity
            cells = [body[pos:pos + 2], body[pos + 2:pos + 4]]
            pos += 4
            blocks.append(("table", (head, cells)))
        elif r < 0.44:                                       # code block
            blocks.append(("pre", code_block(topic, rnd)))
        elif r < 0.52 and pos + 10 < n:                      # subheading
            blocks.append(("h2", " ".join(body[pos:pos + 2]).capitalize()))
            pos += 2

    slug = "-".join(topic[:3])
    out = []
    if rnd.random() < 0.35:                                  # HTML document
        out += ["<!doctype html>", '<html lang="en">', "<head>",
                '<meta charset="utf-8">', "<title>%s</title>" % title,
                "</head>", "<body>", "<h1>%s</h1>" % title]
        for kind, payload in blocks:
            if kind == "p":
                out.append("<p>%s</p>" % payload)
            elif kind == "ul":
                out.append("<ul>" + "".join("<li>%s</li>" % i
                                            for i in payload) + "</ul>")
            elif kind == "table":
                head, cells = payload
                out.append("<table><tr><th>%s</th></tr>" % head
                           + "".join("<tr><td>%s</td></tr>" % " ".join(c)
                                     for c in cells if c) + "</table>")
            elif kind == "pre":
                out.append("<pre><code>%s</code></pre>"
                           % payload.strip("`").split("\n", 1)[-1]
                           .replace("<", "&lt;"))
            else:
                out.append("<h2>%s</h2>" % payload)
        out += ["</body>", "</html>"]
        return slug + ".html", "\n".join(out) + "\n"

    out = ["# %s" % title, ""]
    for kind, payload in blocks:
        if kind == "p":
            out += [payload, ""]
        elif kind == "ul":
            out += [""] + ["- %s" % i for i in payload] + [""]
        elif kind == "table":
            head, cells = payload
            out += ["| %s |" % head, "| --- | --- |"]
            out += ["| %s |" % " ".join(c) for c in cells if c] + [""]
        elif kind == "pre":
            out += [payload, ""]
        else:
            out += ["## %s" % payload, ""]
    return slug + ".md", "\n".join(out) + "\n"


def write_scale_queries(path, counts, seed):
    """One query per document-frequency band, cheapest posting list first.

    A keyword query is the union of its terms' posting lists, so the cost is
    set by the LONGEST list in the query, not the shortest. The bands are
    picked by source weight: a term of weight w is expected in roughly
    w/total_tokens of the corpus's tokens, so a query of two head terms is a
    whole-corpus scan and a query of two tail terms is a few dozen postings.
    """
    words = sorted(counts)
    total = float(sum(counts.values()))
    ordered = sorted(words, key=lambda w: -counts[w])

    def band(lo, hi, k):
        picked = [w for w in ordered if lo <= counts[w] < hi][:k]
        while len(picked) < k:
            picked.append(picked[-1])
        return picked

    rows = []
    tail = band(2, 40, 2)
    rare = band(40, 400, 2)
    mid = band(400, 4000, 2)
    head = band(4000, 10 ** 9, 2)
    rows.append(("tail+tail", " ".join(tail)))
    rows.append(("rare+rare", " ".join(rare)))
    rows.append(("mid+mid", " ".join(mid)))
    rows.append(("rare+mid", "%s %s" % (rare[0], mid[0])))
    rows.append(("mid+head", "%s %s" % (mid[0], head[0])))
    rows.append(("head alone", head[0]))
    rows.append(("head+head", " ".join(head)))
    rows.append(("head+rare", "%s %s" % (head[1], rare[1])))
    with open(path, "w") as fh:
        fh.write("# Generated by bench-scale.sh (seed %s): one query per\n"
                 "# document-frequency band, so the latency sample spans\n"
                 "# posting lists from tens of entries to the whole corpus.\n"
                 "# Format: band <TAB> query\n" % seed)
        for label, q in rows:
            fh.write("%s\t%s\n" % (label, q))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--docs", type=int, required=True)
    ap.add_argument("--seed", type=int, default=20260926)
    ap.add_argument("--shard", type=int, default=500,
                    help="documents per directory")
    ap.add_argument("--mean-bytes", type=int, default=MEAN_BYTES)
    ap.add_argument("--words", default="",
                    help="comma-separated word-source directories")
    ap.add_argument("--queries-out", default="")
    args = ap.parse_args()

    sources = [s for s in args.words.split(",") if s and os.path.isdir(s)]
    if not sources:
        sys.exit("ERROR: no readable word source; pass --words DIR[,DIR]")

    counts = build_vocabulary(sources)
    words = sorted(counts)
    cumw = []
    total = 0
    for w in words:
        total += counts[w]
        cumw.append(total)

    rnd = random.Random(args.seed)
    subs = sorted(set("d%03d" % (i // args.shard) for i in range(args.docs)))
    for sub in subs:
        os.makedirs(os.path.join(args.outdir, sub), exist_ok=True)
    nbytes = 0
    for i in range(args.docs):
        stem, text = gen_doc(words, cumw, rnd)
        sub = os.path.join(args.outdir, "d%03d" % (i // args.shard))
        blob = text.encode("utf-8")
        with open(os.path.join(sub, "%06d-%s" % (i, stem)), "wb") as fh:
            fh.write(blob)
        nbytes += len(blob)
    if args.queries_out:
        write_scale_queries(args.queries_out, counts, args.seed)
    sys.stderr.write("gen: %d docs, %d bytes, mean %d B, vocab %d\n"
                     % (args.docs, nbytes, nbytes // max(1, args.docs),
                        len(words)))
    print("DOCS=%d" % args.docs)
    print("BYTES=%d" % nbytes)
    print("MEAN_BYTES=%d" % (nbytes // max(1, args.docs)))
    print("VOCAB=%d" % len(words))


if __name__ == "__main__":
    main()
PYEOF

# ------------------------------------------------------------ the load client
# Serial, one persistent keep-alive connection, one python process. A curl
# loop is not usable here: each sample would pay milliseconds of process
# startup against a query measured in tenths of a millisecond.
cat > "$SANDBOX/qclient.py" <<'PYEOF'
"""Serial query latency for one connection: per-query percentiles, plus the
total number of documents each query matches (limit=1000, i.e. capped).

Client CPU is reported as a fraction of wall time so a reader can see the
client was not the thing being measured.
"""
import http.client, json, sys, time, urllib.parse

url, mode, kb, limit = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
reps = int(sys.argv[5])
qfile = sys.argv[6]
cold = sys.argv[7] == "1"
out_path = sys.argv[8]
token = sys.argv[9] if len(sys.argv) > 9 else ""
AUTH = {"Authorization": "Bearer " + token} if token else {}

# A line may be "label<TAB>query" (the generated scale set labels each query
# with the document-frequency band it was drawn from); bench/queries.txt has
# no tabs and is used verbatim.
queries = []
for line in open(qfile):
    line = line.rstrip("\n")
    if not line.strip() or line.startswith("#"):
        continue
    queries.append(line.split("\t", 1)[-1].strip())
host = url.split("//", 1)[1]
conn = http.client.HTTPConnection(host, timeout=300)
samples, matched, errors = [], [], 0
cpu0 = time.process_time()
t_start = time.perf_counter()
for qi, q in enumerate(queries):
    url_q = "/api/search?q={}&mode={}&kb={}&limit={}".format(
        urllib.parse.quote(q), mode, kb, limit)
    for attempt in (0, 1):
        try:
            conn.request("GET", url_q, headers=AUTH)
            r = conn.getresponse()
            r.read()
            if r.status != 200:
                raise RuntimeError("status %d" % r.status)
            break
        except Exception:
            conn.close()
            conn = http.client.HTTPConnection(host, timeout=300)
            if attempt == 1:
                errors += 1
    if not cold:                      # one discarded warm-up per query
        try:
            conn.request("GET", url_q, headers=AUTH)
            r = conn.getresponse()
            r.read()
        except Exception:
            pass
    row = []
    for _ in range(reps):
        t0 = time.perf_counter()
        try:
            conn.request("GET", url_q, headers=AUTH)
            r = conn.getresponse()
            r.read()
        except Exception:
            errors += 1
            continue
        row.append((time.perf_counter() - t0) * 1000.0)
    samples.append((qi, row))
    # Total matching documents, capped by limit and by KBC_MAX_HITS (1000).
    try:
        conn.request("GET", "/api/search?q={}&mode={}&kb={}&limit=1000".format(
            urllib.parse.quote(q), mode, kb), headers=AUTH)
        r = conn.getresponse()
        matched.append(len(json.loads(r.read()).get("results", [])))
    except Exception:
        matched.append(-1)
cpu = time.process_time() - cpu0
wall = time.perf_counter() - t_start
conn.close()


def pct(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    i = max(1, min(len(s), int((p / 100.0) * len(s) + 0.9999)))
    return s[i - 1]


with open(out_path, "w") as fh:
    for qi, row in samples:
        for ms in row:
            fh.write("%d\t%.4f\n" % (qi, ms))
flat = [ms for _qi, row in samples for ms in row]
print("ERRORS=%d" % errors)
print("CLIENT_CPU_FRAC=%.4f" % (cpu / wall if wall else 0.0))
print("MIN_MS=%.4f" % pct(flat, 0))
print("P50_MS=%.4f" % pct(flat, 50))
print("P95_MS=%.4f" % pct(flat, 95))
print("P99_MS=%.4f" % pct(flat, 99))
print("MAX_MS=%.4f" % pct(flat, 100))
# samples is a list of (query_index, [latencies]), so enumerate yields the
# tuple: unpack it, or row is the tuple and pct sorts a list against an int.
for qi, (_q, row) in enumerate(samples):
    print("Q%d_MEDIAN_MS=%.4f" % (qi, pct(row, 50)))
    print("Q%d_P99_MS=%.4f" % (qi, pct(row, 99)))
    print("Q%d_MATCHED=%d" % (qi, matched[qi]))
PYEOF

# --------------------------------------------------------- page-cache evict
# posix_fadvise(POSIX_FADV_DONTNEED) drops this file's clean page-cache pages.
# It needs no root and cannot touch another process's pages, which
# /proc/sys/vm/drop_caches would. The daemon is stopped while it runs, so
# nothing can fault the pages straight back in.
cat > "$SANDBOX/evict.py" <<'PYEOF'
import os, sys
total = 0
for path in sys.argv[1:]:
    if not os.path.exists(path):
        continue
    fd = os.open(path, os.O_RDONLY)
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        total += os.fstat(fd).st_size
    finally:
        os.close(fd)
print("EVICTED_BYTES=%d" % total)
PYEOF

# ------------------------------------------------------------------ helpers
start_daemon() { # $1=config path, $2=log path. Deliberately not a
                 # substitution: in a subshell the daemon would be
                 # reparented away from this shell's trap and leak, still
                 # holding the port.
  local cfg="$1" logf="$2"
  "$KBC_BIN" daemon --config "$cfg" --foreground >"$logf" 2>&1 &
  DAEMON_PID=$!
  local i
  for i in $(seq 1 3000); do
    kcurl -fsS -m 2 "$URL/api/health" >/dev/null 2>&1 && return 0
    kill -0 "$DAEMON_PID" 2>/dev/null || return 1
    sleep 0.2
  done
  return 1
}

stop_daemon() {
  [ -n "$DAEMON_PID" ] || return 0
  kill -0 "$DAEMON_PID" 2>/dev/null || { DAEMON_PID=""; return 0; }
  log "  stopping daemon ${DAEMON_PID}"
  kill -TERM "$DAEMON_PID" 2>/dev/null
  local i
  for i in $(seq 1 1200); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.25; done
  kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID" 2>/dev/null
  # SIGTERM frees the listener at once but the process then finishes its index
  # shutdown, which takes seconds. A restart that begins before the port is
  # released would measure the OLD daemon, or fail to open the store.
  local j
  for j in $(seq 1 240); do
    port_busy || break
    sleep 0.25
  done
  DAEMON_PID=""
}

# The daemon is started WITH a token, exactly as an operator would run it, and
# every request presents it: a benchmark that quietly ran the no-auth path
# would be measuring a configuration nobody deploys. One constant-time compare
# of a 48-byte token is in every sample, which is the point.
kcurl() { curl -H "Authorization: Bearer ${TOKEN:-}" "$@"; }

rss_kb() { awk -v k="$1" '/^VmRSS:/ {print $2}' "/proc/$1/status" 2>/dev/null; }
rss_field() { awk -v k="$2" '$1 == k":" {print $2}' "/proc/$1/status" 2>/dev/null; }

# Hardlink view of the first N documents of the master corpus. Costs no disk
# and no regeneration; the shard layout (SHARD documents per directory) is
# what makes it cheap -- whole shards are one cp -al, only the last one is
# linked file by file.
make_view() { # $1=view dir  $2=N
  # Separate `local` statements: a single `local a=$1 b=$((a/2))` expands
  # $((a/2)) before `local` has assigned a, so the arithmetic silently reads
  # whatever `n` happens to be in the enclosing scope.
  local view="$1" ndocs="$2" full part k f src dst linked
  full=$((ndocs / SHARD))
  part=$((ndocs % SHARD))
  rm -rf "$view"
  mkdir -p "$view"
  for ((k = 0; k < full; k++)); do
    cp -al "$(printf '%s/d%03d' "$CORPUS_ROOT" "$k")" "$view/"
  done
  if [ "$part" -gt 0 ]; then
    src="$(printf '%s/d%03d' "$CORPUS_ROOT" "$full")"
    dst="$(printf '%s/d%03d' "$view" "$full")"
    mkdir -p "$dst"
    linked=0
    for f in "$src"/*; do
      [ "$linked" -ge "$part" ] && break
      ln "$f" "$dst/${f##*/}"
      linked=$((linked + 1))
    done
  fi
}

jnum() { sed -n "s/.*\"$1\":\([0-9-]*\).*/\1/p"; }

# ------------------------------------------------------------------ generate
log "generating ${MAX_DOCS} documents into ${CORPUS_ROOT} (seed ${SEED}, word source ${WORD_SOURCE})"
GEN_START=$(date +%s.%N)
GEN_OUT=$(python3 "$SANDBOX/gen_corpus.py" "$CORPUS_ROOT" --docs "$MAX_DOCS" \
            --seed "$SEED" --shard "$SHARD" --words "$WORD_SOURCE" \
            --queries-out "$SANDBOX/queries-scale.txt")
GEN_S=$(awk -v a="$GEN_START" -v b="$(date +%s.%N)" 'BEGIN{printf "%.2f", b-a}')
GEN_BYTES=$(printf '%s\n' "$GEN_OUT" | sed -n 's/^BYTES=//p')
GEN_VOCAB=$(printf '%s\n' "$GEN_OUT" | sed -n 's/^VOCAB=//p')
log "generated ${MAX_DOCS} docs, ${GEN_BYTES} bytes, vocab ${GEN_VOCAB}, in ${GEN_S}s"
echo "gen_docs=${MAX_DOCS} gen_bytes=${GEN_BYTES} gen_seconds=${GEN_S} gen_vocab=${GEN_VOCAB}" >> "$RESULTS"
echo "corpus_root=${CORPUS_ROOT}" >> "$RESULTS"
echo "seed=${SEED}" >> "$RESULTS"
echo "loadavg_at_start=$(loadavg)" >> "$RESULTS"
echo "host_cores=$(nproc)" >> "$RESULTS"
echo "mem_available_kb_at_start=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)" >> "$RESULTS"

# ------------------------------------------------------------------- the run
{
  printf '%9s %12s %10s %9s %11s %11s %9s %9s %9s %9s %8s %8s\n' \
    "docs" "corpus_MB" "index_MB" "index/cor" "ingest_s" "peakRSS_MB" \
    "rssRest_MB" "p50_ms" "p99_ms" "cold_p50" "incr_ms" "terms"
} >> "$SANDBOX/table.txt"

for N in $DOCS_LIST; do
  log "=== ${N} documents ==="
  if [ "$N" = "$MAX_DOCS" ]; then
    CORPUS="$CORPUS_ROOT"
  else
    CORPUS="$SANDBOX/corpus-$N"
    log "building a hardlink view of the first ${N} documents"
    make_view "$CORPUS" "$N"
  fi
  DATA="$SANDBOX/data-$N"
  CFG="$SANDBOX/daemon-$N.toml"
  LOG="$SANDBOX/daemon-$N.log"
  rm -rf "$DATA"
  mkdir -p "$DATA"
  # A per-run bearer token, written before the config so the daemon can read
  # it at startup. The CLI ingest below does not need it: it is local.
  TOKEN="$(od -An -tx1 -N24 /dev/urandom | tr -d ' \n')"
  printf '%s' "$TOKEN" > "$DATA/token"
  cat > "$CFG" <<EOF
[daemon]
data_dir = "${DATA}"
port = ${PORT}
workers = ${WORKERS}
token_file = "${DATA}/token"

[[corpus]]
name = "${KB_NAME}"
path = "${CORPUS}"
EOF

  CORPUS_BYTES=$(find "$CORPUS" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')
  CORPUS_FILES=$(find "$CORPUS" -type f \( -name '*.md' -o -name '*.html' \) 2>/dev/null | wc -l)
  log "corpus: ${CORPUS_FILES} files, ${CORPUS_BYTES} bytes"

  # ---- full ingest, from an empty data dir, with peak RSS of the ingester
  TIMEFILE="$SANDBOX/time-$N.txt"
  T0=$(date +%s.%N)
  /usr/bin/time -f "%e %M" -o "$TIMEFILE" \
    "$KBC_BIN" reindex --config "$CFG" > "$SANDBOX/reindex-$N.log" 2>&1
  RC=$?
  INGEST_S=$(awk -v a="$T0" -v b="$(date +%s.%N)" 'BEGIN{printf "%.3f", b-a}')
  PEAK_KB=$(awk 'NR==1 {print $2}' "$TIMEFILE" 2>/dev/null)
  [ "${PEAK_KB:-0}" -gt 0 ] 2>/dev/null || PEAK_KB=0
  if [ "$RC" -ne 0 ]; then
    log "ERROR reindex failed (rc=${RC}); tail:"; tail -5 "$SANDBOX/reindex-$N.log" >&2
  fi
  INDEX_BYTES=$(stat -c%s "$DATA/index" 2>/dev/null || echo 0)
  DB_BYTES=$(stat -c%s "$DATA/kb.db" 2>/dev/null || echo 0)
  WAL_BYTES=$(stat -c%s "$DATA/kb.db-wal" 2>/dev/null || echo 0)
  log "ingest ${INGEST_S}s, peak RSS ${PEAK_KB}kB, index ${INDEX_BYTES}B, db ${DB_BYTES}B"

  # ---- daemon up
  if ! start_daemon "$CFG" "$LOG"; then
    log "ERROR daemon failed to start; log tail:"; tail -20 "$LOG" >&2
    stop_daemon
    continue
  fi
  log "daemon ${DAEMON_PID} ready on ${URL}"
  STATS=$(kcurl -fsS -m 30 "$URL/api/stats")
  DOCS=$(printf '%s' "$STATS" | jnum index_docs)
  TERMS=$(printf '%s' "$STATS" | jnum index_terms)
  RSS_REST=$(rss_kb "$DAEMON_PID")
  RSS_FILE=$(rss_field "$DAEMON_PID" RssFile)
  RSS_ANON=$(rss_field "$DAEMON_PID" RssAnon)
  log "index: ${DOCS} docs, ${TERMS} terms; RSS at rest ${RSS_REST}kB (file ${RSS_FILE}kB, anon ${RSS_ANON}kB)"
  if [ "${DOCS:-0}" -lt 100 ]; then
    log "index has ${DOCS:-0} docs after the CLI ingest; asking the daemon to scan"
    kcurl -fsS -m 3600 -X POST "$URL/api/reindex" >/dev/null 2>&1
    for _ in $(seq 1 7200); do
      DOCS=$(kcurl -fsS -m 30 "$URL/api/stats" | jnum index_docs)
      [ "${DOCS:-0}" -ge 100 ] && break
      sleep 0.5
    done
    TERMS=$(kcurl -fsS -m 30 "$URL/api/stats" | jnum index_terms)
  fi
  [ "${DOCS:-0}" -ge 1 ] || { log "ERROR no documents indexed at ${N}; skipping this size"; stop_daemon; continue; }

  # ---- warm latency, both query sets
  measure_set() { # $1=label $2=query file $3=reps $4=cold(0|1)
    local label="$1" qf="$2" reps="$3" cold="$4"
    local out="$SAMPLES/${N}-${label}.tsv"
    python3 "$SANDBOX/qclient.py" "$URL" "$MODE" "$KB_NAME" "$LIMIT" \
            "$reps" "$qf" "$cold" "$out" "$TOKEN" > "$SANDBOX/q-$N-$label.env" 2>"$SANDBOX/q-$N-$label.err"
    grep -E '^(P50_MS|P95_MS|P99_MS|MIN_MS|MAX_MS|ERRORS|CLIENT_CPU_FRAC)=' \
        "$SANDBOX/q-$N-$label.env" | sed "s/^/${label}_/"
    grep -E '^Q[0-9]+_(MEDIAN_MS|P99_MS|MATCHED)=' "$SANDBOX/q-$N-$label.env" \
        | sed "s/^/${label}_/"
  }
  WARM_ENV=$(measure_set warm_q "$QUERY_FILE" "$REPS" 0)
  WARM_SCALE_ENV=$(measure_set warm_scale "$SANDBOX/queries-scale.txt" "$REPS" 0)
  if ! printf '%s\n' "$WARM_ENV" | grep -q '^warm_q_P50_MS='; then
    log "ERROR the warm query client produced no percentiles; see ${SANDBOX}/q-${N}-warm_q.err"
    cat "$SANDBOX/q-${N}-warm_q.err" >&2
    WARM_ENV=""; WARM_SCALE_ENV=""
  fi
  WARM_P50=$(printf '%s\n' "$WARM_ENV" | sed -n 's/^warm_q_P50_MS=//p')
  WARM_P99=$(printf '%s\n' "$WARM_ENV" | sed -n 's/^warm_q_P99_MS=//p')
  SCALE_P50=$(printf '%s\n' "$WARM_SCALE_ENV" | sed -n 's/^warm_scale_P50_MS=//p')
  SCALE_P99=$(printf '%s\n' "$WARM_SCALE_ENV" | sed -n 's/^warm_scale_P99_MS=//p')
  RSS_AFTER=$(rss_kb "$DAEMON_PID")
  RSS_AFTER_FILE=$(rss_field "$DAEMON_PID" RssFile)
  log "warm queries.txt: p50 ${WARM_P50}ms p99 ${WARM_P99}ms | warm scale set: p50 ${SCALE_P50}ms p99 ${SCALE_P99}ms"
  log "RSS after warm queries ${RSS_AFTER}kB (file ${RSS_AFTER_FILE}kB)"

  # ---- cold: the index evicted from page cache, then the daemon restarted so
  # SQLite's own page cache is empty too. Without the restart only the index
  # would be cold and the hit-resolve half of the query would still be warm.
  COLD_P50=""; COLD_P99=""; EVICTED=0
  if [ "$COLD" = "1" ]; then
    stop_daemon
    EVICTED=$(python3 "$SANDBOX/evict.py" "$DATA/index" "$DATA/kb.db" \
                "$DATA/kb.db-wal" 2>/dev/null | sed -n 's/^EVICTED_BYTES=//p')
    if start_daemon "$CFG" "$LOG"; then
      log "cold: evicted ${EVICTED}B, daemon restarted (pid ${DAEMON_PID})"
      COLD_ENV=$(measure_set cold_scale "$SANDBOX/queries-scale.txt" "$COLD_REPS" 1)
      COLD_ENV2=$(measure_set cold_q "$QUERY_FILE" "$COLD_REPS" 1)
      COLD_P50=$(printf '%s\n' "$COLD_ENV" | sed -n 's/^cold_scale_P50_MS=//p')
      COLD_P99=$(printf '%s\n' "$COLD_ENV" | sed -n 's/^cold_scale_P99_MS=//p')
      log "cold scale set: p50 ${COLD_P50}ms p99 ${COLD_P99}ms"
      printf '%s\n' "$COLD_ENV" "$COLD_ENV2" >> "$RESULTS"
    else
      log "ERROR daemon failed to restart for the cold run"
    fi
  fi
  # ---- incremental: ONE file changed, at this size
  #
  # LAST on purpose, and not for tidiness: this step makes the daemon rewrite
  # its index file, and a rewritten index the next start cannot reopen would
  # take the cold measurement down with it. Measured after the cold run, a
  # failure here costs one number instead of two.
  # Touched by write-to-temp + rename, which is what an editor does and what
  # the watcher reports as IN_MOVED_TO. A direct rewrite would edit the shared
  # inode and, in a hardlink view, the master corpus and every other view.
  INCR_US=""
  if [ "$INCREMENTAL_REPS" -gt 0 ]; then
    VICTIM=$(find "$CORPUS" -type f -name '*.md' | sort | head -1)
    for r in $(seq 1 "$INCREMENTAL_REPS"); do
      BEFORE_NS=$(kcurl -fsS -m 30 "$URL/api/stats" | jnum last_reindex_ns)
      # A copy, then the comment, then the rename. A direct append would edit
      # the shared inode, and in a hardlink view that inode belongs to the
      # master corpus and to every other view of it. cp+mv is also what an
      # editor does, and it keeps the document the same size on every rep.
      cp "$VICTIM" "$VICTIM.tmp" \
        && printf '\n<!-- save %s -->\n' "$r" >> "$VICTIM.tmp" \
        && mv -f "$VICTIM.tmp" "$VICTIM"
      GOT=""
      for _ in $(seq 1 2400); do
        ST=$(kcurl -fsS -m 30 "$URL/api/stats")
        NS=$(printf '%s' "$ST" | jnum last_reindex_ns)
        if [ -n "$NS" ] && [ "$NS" != "$BEFORE_NS" ] && [ "$NS" != "0" ]; then
          GOT=$(printf '%s' "$ST" | jnum last_reindex_us); break
        fi
        sleep 0.25
      done
      if [ -n "$GOT" ]; then
        INCR_US="${INCR_US}${INCR_US:+,}${GOT}"
        log "  incremental save ${r}: ${GOT} us (daemon-side, excludes the ${DEBOUNCE:-250}ms watcher debounce)"
      else
        log "  incremental save ${r}: NOT OBSERVED within 600 s"
        INCR_US="${INCR_US}${INCR_US:+,}timeout"
      fi
    done
  fi
  INCR_MS=$(printf '%s' "$INCR_US" | awk -F, '{
    s=0; n=0; for (i=1;i<=NF;i++) { if ($i ~ /^[0-9]+$/) { s+=$i; n++ } }
    if (n) printf "%.3f", s/n/1000; else printf "nan" }')

  RSS_FINAL=$(rss_kb "$DAEMON_PID")
  stop_daemon
  sleep 1

  # ---- optional in-process bench (its own tmpdir; not comparable to the
  # HTTP numbers, and not run by default at large N because it is a second
  # full ingest)
  if [ "$RUN_KBC_BENCH" = "1" ]; then
    log "running in-process kbc bench at ${N} documents"
    "$KBC_BIN" bench --corpus "$CORPUS" --queries 20 --repeat 20 \
        > "$SANDBOX/kbcbench-$N.txt" 2>&1
    tail -4 "$SANDBOX/kbcbench-$N.txt" >&2
    cp "$SANDBOX/kbcbench-$N.txt" "$RESULTS.kbcbench-$N.txt" 2>/dev/null
  fi

  RATIO=$(awk -v a="$INDEX_BYTES" -v b="$CORPUS_BYTES" 'BEGIN{printf "%.2f", b? a/b : 0}')
  {
    echo "N=$N"
    echo "corpus_files=$CORPUS_FILES"
    echo "corpus_bytes=$CORPUS_BYTES"
    echo "index_docs=$DOCS"
    echo "index_terms=$TERMS"
    echo "index_bytes=$INDEX_BYTES"
    echo "db_bytes=$DB_BYTES"
    echo "db_wal_bytes=$WAL_BYTES"
    echo "index_over_corpus=$RATIO"
    echo "ingest_s=$INGEST_S"
    echo "ingest_peak_rss_kb=$PEAK_KB"
    echo "rss_at_rest_kb=$RSS_REST"
    echo "rss_at_rest_file_kb=$RSS_FILE"
    echo "rss_at_rest_anon_kb=$RSS_ANON"
    echo "rss_after_queries_kb=$RSS_AFTER"
    echo "rss_after_queries_file_kb=$RSS_AFTER_FILE"
    echo "rss_final_kb=$RSS_FINAL"
    echo "incremental_us=$INCR_US"
    echo "incremental_mean_ms=$INCR_MS"
    echo "evicted_bytes=$EVICTED"
    echo "cold_p50_ms=$COLD_P50"
    echo "cold_p99_ms=$COLD_P99"
    printf '%s\n' "$WARM_ENV" "$WARM_SCALE_ENV"
  } >> "$RESULTS"

  printf '%9d %12.1f %10.1f %9s %11s %11d %9d %9s %9s %9s %8s %8s\n' \
    "$N" "$(awk -v b="$CORPUS_BYTES" 'BEGIN{printf "%.1f", b/1048576}')" \
    "$(awk -v b="$INDEX_BYTES" 'BEGIN{printf "%.1f", b/1048576}')" \
    "$RATIO" "$INGEST_S" "$((PEAK_KB / 1024))" "$((RSS_REST / 1024))" \
    "$WARM_P50" "$WARM_P99" "${COLD_P50:-n/a}" "$INCR_MS" "$TERMS" \
    >> "$SANDBOX/table.txt"
  cat "$SANDBOX/table.txt" >&2

  # The data dir is the biggest thing this script writes; drop it unless the
  # caller wants to poke at the index by hand.
  [ "${KEEP_DATA:-0}" = "1" ] || rm -rf "$DATA"
  [ "$N" = "$MAX_DOCS" ] || rm -rf "$CORPUS"
done

echo
cat "$SANDBOX/table.txt"
echo
cat "$RESULTS"
log "results: $RESULTS ; raw per-sample TSVs: $SAMPLES/*.tsv ; table: $SANDBOX/table.txt"
log "NOTE: this host is shared (8 cores, loadavg printed above, other users' "
log "      processes). Absolute latencies are not portable; the SHAPE across "
log "      the ladder is."
log "NOTE: the ingest number is 'kbc reindex' into an empty data dir, which "
log "      is the full-corpus cost. The incremental number is the daemon's "
log "      own last_reindex_us for a single file change, so it excludes the "
log "      watcher debounce that precedes it."
