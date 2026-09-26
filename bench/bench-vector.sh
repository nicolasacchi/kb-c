#!/usr/bin/env bash
# bench-vector.sh — kb-c's VECTOR lane, measured against the REAL sidecar.
#
# bench/bench-kbc.sh measures mode=keyword, so it never pays for an embedding
# model and says nothing about the lane that fuses with it. This script is the
# same harness pointed at mode=semantic with a real `kb-embedder` as the
# subprocess, so the two are directly comparable: same corpus, same
# bench/queries.txt, same client, same percentile code, same concurrency
# levels. What it adds on top of a latency sweep is the evidence that the lane
# is actually doing something — the vector store's dimension, the ingest cost
# of embedding the corpus, the daemon's RSS with a model resident (the sidecar
# is a separate process, so both numbers are reported), and a per-query
# keyword-vs-semantic ranking diff, because a lane that silently returns the
# keyword ordering would look identical to a lane that works.
#
# The sidecar is the production binary, run the way production runs it:
#     kb-embedder --model bge-small-en-v1.5 --cache <dir>
# It is spawned by this script's daemon, which is killed with that daemon. Any
# kb-embedder this host was already running is left alone.
#
# Usage:
#   bench/bench-vector.sh [CORPUS_DIR] [PORT]
# Options (env or flags; flags win):
#   --sidecar PATH   embedder binary   (default: /usr/local/bin/kb-embedder,
#                                      else ~/.local/bin/kb-embedder)
#   --model NAME     model name        (default: bge-small-en-v1.5)
#   --cache DIR      model cache       (default: ${XDG_CACHE_HOME:-$HOME/.cache}/kb/models)
#   --mode MODE      search mode       (default: semantic; "hybrid" also accepted)
# Env:  QUERY_FILE, REPS (20), LEVELS ("1 2 4 8"), LIMIT (10), WORKERS (4),
#       KBC_BIN, SANDBOX, KB_NAME, SKIP_INGEST=1
# Output: progress on stderr, the table and key=value on stdout, raw samples
#         under $SANDBOX/samples.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${1:-$ROOT/bench/data/corpus}"
PORT="${2:-${PORT:-4331}}"
KBC_BIN="${KBC_BIN:-$ROOT/build/kbc}"
SANDBOX="${SANDBOX:-/tmp/kbcbench-vector}"
KB_NAME="${KB_NAME:-kb}"
REPS="${REPS:-20}"
LEVELS="${LEVELS:-1 2 4 8}"
WORKERS="${WORKERS:-4}"
QUERY_FILE="${QUERY_FILE:-$ROOT/bench/queries.txt}"
MODE="${MODE:-semantic}"
LIMIT="${LIMIT:-10}"
SKIP_INGEST="${SKIP_INGEST:-0}"

# Defaults are the production invocation on this host. /usr/local/bin is where
# the running sidecars come from; the per-user install is the same binary and is
# used when the system one is absent.
SIDECAR="${SIDECAR:-/usr/local/bin/kb-embedder}"
[ -x "$SIDECAR" ] || SIDECAR="$HOME/.local/bin/kb-embedder"
MODEL="${MODEL:-bge-small-en-v1.5}"
CACHE="${CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/kb/models}"

while [ $# -gt 0 ]; do
  case "$1" in
    --sidecar) SIDECAR="$2"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --cache) CACHE="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) break ;;   # a positional (CORPUS_DIR); the case block ends here
  esac
done

URL="http://127.0.0.1:${PORT}"
CFG="$SANDBOX/daemon.toml"
LOG="$SANDBOX/daemon.log"
SAMPLES="$SANDBOX/samples"
VECFILE="$SANDBOX/data/vectors.bin"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
loadavg() { awk '{print $1}' /proc/loadavg; }

DAEMON_PID=""

cleanup() {
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    log "stopping daemon ${DAEMON_PID} (the one this script started)"
    # The sidecar is the daemon's child and dies with its stdin; give it a
    # moment, then make sure nothing of ours is left holding the port.
    kill -TERM "$DAEMON_PID" 2>/dev/null
    for _ in $(seq 1 120); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.25; done
    kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID" 2>/dev/null
  fi
  for _ in $(seq 1 40); do
    ss -ltn 2>/dev/null | grep -q ":${PORT} " || break
    sleep 0.25
  done
  if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
    log "ERROR port ${PORT} still listening after cleanup"
  fi
}
trap cleanup EXIT INT TERM

port_busy() { ss -ltn 2>/dev/null | grep -q ":${PORT} "; }

# ---- preflight -----------------------------------------------------------
[ -x "$KBC_BIN" ] || { echo "ERROR kbc binary not found: $KBC_BIN" >&2; exit 2; }
[ -d "$CORPUS" ] || { echo "ERROR corpus dir not found: $CORPUS" >&2; exit 2; }
[ -r "$QUERY_FILE" ] || { echo "ERROR QUERY_FILE not readable: $QUERY_FILE" >&2; exit 2; }
if [ ! -x "$SIDECAR" ]; then
  echo "ERROR sidecar not executable: $SIDECAR (pass --sidecar PATH)" >&2
  exit 2
fi
case "$MODE" in
  semantic|hybrid) ;;
  *) echo "ERROR MODE must be semantic or hybrid (got '$MODE')" >&2; exit 2 ;;
esac
if port_busy; then
  echo "ERROR port ${PORT} already in use; refusing to touch a daemon this script did not start" >&2
  exit 2
fi

# ---- query set ------------------------------------------------------------
QUERIES=()
while IFS= read -r qline; do
  case "$qline" in ''|\#*) continue ;; esac
  QUERIES+=("$qline")
done < "$QUERY_FILE"
NQUERIES=${#QUERIES[@]}
[ "$NQUERIES" -gt 0 ] || { echo "ERROR QUERY_FILE held no queries: $QUERY_FILE" >&2; exit 2; }

# ---- clean state + own config -------------------------------------------
rm -rf "$SANDBOX"
mkdir -p "$SANDBOX" "$SAMPLES"
# The command is a command LINE, not a path: the sidecar is told which model
# and which cache to load, and there is no other way to say it. Quoted paths
# keep a cache dir with a space in it working.
{
  echo "[daemon]"
  echo "data_dir = \"${SANDBOX}/data\""
  echo "port = ${PORT}"
  echo "workers = ${WORKERS}"
  # The daemon binds 127.0.0.1 only and this client talks to it directly, so
  # auth is switched off rather than satisfied with a generated token: a
  # benchmark that failed on a fresh sandbox for want of a token file would
  # say nothing about the vector lane.
  echo "token_file = \"\""
  echo
  echo "[[corpus]]"
  echo "name = \"${KB_NAME}\""
  echo "path = \"${CORPUS}\""
  echo
  echo "[embedder]"
  echo "command = \"${SIDECAR} --model ${MODEL} --cache ${CACHE}\""
} > "$CFG"

log "corpus=${CORPUS} port=${PORT} mode=${MODE} sidecar=${SIDECAR} model=${MODEL} cache=${CACHE}"
log "reps=${REPS} levels='${LEVELS}' limit=${LIMIT} queries=${NQUERIES} loadavg=$(loadavg)"

# Run from the sandbox: db_path, index_path and token_path default to paths
# relative to the CURRENT DIRECTORY, so a daemon started from the repo root
# drops a data/ directory there. Every artifact this run creates belongs
# inside the sandbox it also deletes.
cd "$SANDBOX" || exit 2

# ---- start the daemon ----------------------------------------------------
"$KBC_BIN" daemon --config "$CFG" --foreground >"$LOG" 2>&1 &
DAEMON_PID=$!
ready=""
for _ in $(seq 1 1200); do
  if curl -fsS -m 2 "$URL/api/health" >/dev/null 2>&1; then ready=1; break; fi
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "ERROR daemon exited during startup; log tail:" >&2; tail -20 "$LOG" >&2; exit 3
  fi
  sleep 0.2
done
[ -n "$ready" ] || { echo "ERROR daemon never became ready; log tail:" >&2; tail -20 "$LOG" >&2; exit 3; }
log "daemon ${DAEMON_PID} ready on ${URL} (workers=${WORKERS})"

# ---- index the corpus, timed, with embeddings live ----------------------
DOCS=$(curl -fsS -m 10 "$URL/api/stats" | sed -n 's/.*"index_docs":\([0-9]*\).*/\1/p')
INGEST_S=""
if [ "${SKIP_INGEST}" = "1" ] && [ "${DOCS:-0}" -ge 100 ]; then
  log "reusing the existing index (${DOCS} docs, SKIP_INGEST=1)"
else
  log "reindexing with embeddings; this is the ingest-with-embeddings figure"
  t0=$(date +%s.%N)
  curl -fsS -m 3600 -X POST "$URL/api/reindex" >/dev/null || {
    echo "ERROR reindex failed; log tail:" >&2; tail -20 "$LOG" >&2; exit 4; }
  t1=$(date +%s.%N)
  INGEST_S=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
  log "reindex done in ${INGEST_S}s"
fi
DOCS=$(curl -fsS -m 10 "$URL/api/stats" | sed -n 's/.*"index_docs":\([0-9]*\).*/\1/p')
[ "${DOCS:-0}" -ge 100 ] || { echo "ERROR index never populated (docs=${DOCS:-0})" >&2; exit 4; }
log "index populated: ${DOCS} docs"

# ---- is the vector lane real? -------------------------------------------
# The store's on-disk header is magic[8], version, dim, capacity, count. Its
# dimension is the sidecar's own, and a store that exists at all means the
# embed answered during ingest rather than degrading to keyword-only.
DIM="0"
VEC_ROWS="0"
if [ -f "$VECFILE" ]; then
  DIM=$(od -An -tu4 -j12 -N4 "$VECFILE" | tr -d ' ')
  VEC_ROWS=$(od -An -tu4 -j20 -N4 "$VECFILE" | tr -d ' ')
else
  log "WARNING no vector store at ${VECFILE}: the lane never produced vectors"
fi
log "vector store: dim=${DIM} rows=${VEC_ROWS} (${VECFILE})"

# ---- the load client -----------------------------------------------------
# Byte-for-byte the client in bench/bench-kbc.sh, so the two sweeps differ only
# in the mode they ask for. It is not a separate repo file on purpose: a
# throwaway client nobody can read is worth less than one that lives in the
# script that uses it.
cat > "$SANDBOX/loadclient.py" <<'PYEOF'
"""Closed-loop HTTP load client: C threads, one persistent connection each.

Each thread issues the FULL query set REPS times, in the same order, so every
query is sampled equally often at every concurrency level. A barrier releases
all threads at once so the measured window contains no ramp-up skew.
"""
import http.client, sys, threading, time, urllib.parse

url, mode, kb, limit = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
level, reps = int(sys.argv[5]), int(sys.argv[6])
queries = [l.rstrip("\n") for l in open(sys.argv[7]) if l.strip() and not l.startswith("#")]

samples = []          # (query_index, milliseconds)
lock = threading.Lock()
errors = [0]
conns_opened = [0]

HOST = url.split("//", 1)[1]
QUERIES = queries
# level + 1: the workers plus this thread. With `level` parties the main
# thread would satisfy the barrier alongside the FIRST worker and release it
# early, stranding the rest — every level above 1 would then hang forever.
# The timeout turns that class of mistake into a loud failure.
barrier = threading.Barrier(level + 1, timeout=300)


def worker(wid):
    conn = http.client.HTTPConnection(HOST, timeout=300)
    with lock:
        conns_opened[0] += 1
    local = []
    # Release every client at once: the measured window then contains no
    # ramp-up, so rps is a steady-state figure and not a startup average.
    barrier.wait()
    for _ in range(reps):
        for qi, q in enumerate(QUERIES):
            req = "/api/search?q={}&mode={}&kb={}&limit={}".format(
                urllib.parse.quote(q), mode, kb, limit)
            t0 = time.perf_counter()
            for attempt in (0, 1):
                try:
                    conn.request("GET", req)
                    r = conn.getresponse()
                    r.read()
                    if r.status != 200:
                        raise RuntimeError("status %d" % r.status)
                    break
                except Exception:
                    # server closed the keep-alive connection: reconnect once
                    try:
                        conn.close()
                    except Exception:
                        pass
                    conn = http.client.HTTPConnection(HOST, timeout=300)
                    with lock:
                        conns_opened[0] += 1
                    if attempt == 1:
                        with lock:
                            errors[0] += 1
            local.append((qi, (time.perf_counter() - t0) * 1000.0))
    try:
        conn.close()
    except Exception:
        pass
    with lock:
        samples.extend(local)


threads = []
cpu0 = time.process_time()
t0 = time.perf_counter()
for i in range(level):
    t = threading.Thread(target=worker, args=(i,))
    t.start()
    threads.append(t)
barrier.wait()          # every client is connected and blocked on the barrier
wall0 = time.perf_counter()
cpu1 = time.process_time()
for t in threads:
    t.join()
wall1 = time.perf_counter()
cpu_total = time.process_time() - cpu0

out = sys.argv[8]
with open(out, "w") as fh:
    for qi, ms in samples:
        fh.write("%d\t%.4f\n" % (qi, ms))
ms_sorted = sorted(ms for _, ms in samples)


def pct(p):
    if not ms_sorted:
        return float("nan")
    i = int((p / 100.0) * len(ms_sorted) + 0.9999)
    i = max(1, min(i, len(ms_sorted)))
    return ms_sorted[i - 1]


n = len(samples)
dur = wall1 - wall0
print("REQUESTS=%d" % n)
print("CLIENTS=%d" % level)
print("WALL_S=%.4f" % dur)
print("RPS=%.2f" % (n / dur))
print("CLIENT_CPU_S=%.4f" % cpu_total)
print("CLIENT_CPU_FRAC=%.4f" % (cpu_total / dur if dur else 0.0))
print("CONNS_OPENED=%d" % conns_opened[0])
print("ERRORS=%d" % errors[0])
print("MIN_MS=%.4f" % (ms_sorted[0] if ms_sorted else float("nan")))
print("P50_MS=%.4f" % pct(50))
print("P95_MS=%.4f" % pct(95))
print("P99_MS=%.4f" % pct(99))
print("MAX_MS=%.4f" % (ms_sorted[-1] if ms_sorted else float("nan")))
PYEOF

# ---- does the vector lane rank differently from the keyword lane? -------
# Identical orderings across every query would mean the vector half is not
# contributing, whatever the latency says. Both modes are asked the same
# question, one at a time, and the returned ids are compared position by
# position.
cat > "$SANDBOX/rankdiff.py" <<'PYEOF'
"""keyword vs semantic ranking diff, per query. Prints key=value on stdout."""
import http.client, json, sys, urllib.parse

url, kb, limit, qfile = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
HOST = url.split("//", 1)[1]
queries = [l.rstrip("\n") for l in open(qfile) if l.strip() and not l.startswith("#")]


def run(mode, q):
    conn = http.client.HTTPConnection(HOST, timeout=300)
    conn.request("GET", "/api/search?q={}&mode={}&kb={}&limit={}".format(
        urllib.parse.quote(q), mode, kb, limit))
    r = conn.getresponse()
    body = r.read()
    conn.close()
    if r.status != 200:
        raise RuntimeError("%s %r -> %d %s" % (mode, q, r.status, body[:200]))
    doc = json.loads(body)
    rows = doc.get("results") or doc.get("hits") or doc.get("rows") or []
    return [row.get("id") or row.get("path") for row in rows]


same = 0
overlap_sum = 0
first_diff = None
for q in queries:
    kw = run("keyword", q)
    sem = run("semantic", q)
    if kw == sem:
        same += 1
    ov = len(set(kw) & set(sem))
    overlap_sum += ov
    if first_diff is None and kw != sem:
        first_diff = (q, kw, sem)

n = len(queries)
print("queries=%d" % n)
print("identical_orderings=%d" % same)
print("mean_overlap_at_%s=%.2f" % (limit, overlap_sum / n if n else 0.0))
if first_diff is not None:
    print("first_differing_query=%s" % first_diff[0])
    print("keyword_order=%s" % ",".join(str(x) for x in first_diff[1]))
    print("semantic_order=%s" % ",".join(str(x) for x in first_diff[2]))
else:
    print("first_differing_query=")
PYEOF

log "comparing keyword vs semantic rankings"
RANKDIFF=$(python3 "$SANDBOX/rankdiff.py" "$URL" "$KB_NAME" "$LIMIT" "$QUERY_FILE" 2>&1) || {
  echo "ERROR ranking diff failed: $RANKDIFF" >&2; exit 5; }
printf '%s\n' "$RANKDIFF"
log "ranking diff: $(printf '%s' "$RANKDIFF" | sed -n 's/^identical_orderings=//p') of ${NQUERIES} queries returned an identical order"

# ---- the measurement loop ------------------------------------------------
TABLE="$SANDBOX/table.txt"
: > "$SANDBOX/levels.env"
: > "$TABLE"
BASE_RPS=""
BASE_P50=""
echo "host: $(uname -srm), $(nproc) cores; loadavg at start $(loadavg)" >&2
{
  echo "concurrency  clients  requests   wall_s     rps      p50_ms    p95_ms    p99_ms    loadavg  conns"
  for c in $LEVELS; do
    out="$SAMPLES/c${c}.tsv"
    LOAD_BEFORE=$(loadavg)
    res=$(python3 "$SANDBOX/loadclient.py" "$URL" "$MODE" "$KB_NAME" "$LIMIT" \
            "$c" "$REPS" "$QUERY_FILE" "$out")
    LOAD_AFTER=$(loadavg)
    get() { printf '%s\n' "$res" | sed -n "s/^$1=//p"; }
    REQS=$(get REQUESTS); RPS=$(get RPS); P50=$(get P50_MS)
    P95=$(get P95_MS); P99=$(get P99_MS); WALL=$(get WALL_S)
    MIN=$(get MIN_MS); MAX=$(get MAX_MS); ERRS=$(get ERRORS)
    CONNS=$(get CONNS_OPENED); CPUF=$(get CLIENT_CPU_FRAC)
    if [ "$ERRS" != "0" ]; then
      log "WARNING level ${c}: ${ERRS} request errors (see ${out})"
    fi
    if [ -z "$BASE_RPS" ]; then BASE_RPS="$RPS"; BASE_P50="$P50"; fi
    SCALE=$(awk -v a="$RPS" -v b="$BASE_RPS" 'BEGIN{printf "%.2f", a/b}')
    PSLOW=$(awk -v a="$P50" -v b="$BASE_P50" 'BEGIN{printf "%.2f", a/b}')
    printf '%9d  %7d  %8d  %7.2f  %8.1f  %8.3f  %8.3f  %8.3f  %7s  %5d\n' \
      "$c" "$c" "$REQS" "$WALL" "$RPS" "$P50" "$P95" "$P99" "$LOAD_AFTER" "$CONNS" \
      >> "$TABLE"
    {
      echo "conc_${c}_requests=${REQS}"
      echo "conc_${c}_wall_s=${WALL}"
      echo "conc_${c}_rps=${RPS}"
      echo "conc_${c}_p50_ms=${P50}"
      echo "conc_${c}_p95_ms=${P95}"
      echo "conc_${c}_p99_ms=${P99}"
      echo "conc_${c}_min_ms=${MIN}"
      echo "conc_${c}_max_ms=${MAX}"
      echo "conc_${c}_scale_vs_1=${SCALE}"
      echo "conc_${c}_p50_slowdown_vs_1=${PSLOW}"
      echo "conc_${c}_errors=${ERRS}"
      echo "conc_${c}_client_cpu_frac=${CPUF}"
      echo "conc_${c}_loadavg_after=${LOAD_AFTER}"
    } >> "$SANDBOX/levels.env"
    log "level ${c}: ${REQS} reqs in ${WALL}s -> ${RPS} rps, p50=${P50}ms p95=${P95}ms p99=${P99}ms (scale ${SCALE}x, p50 x${PSLOW}), loadavg ${LOAD_BEFORE}->${LOAD_AFTER}"
  done
}
cat "$TABLE"

STATS=$(curl -fsS -m 10 "$URL/api/stats")
DOCS_FINAL=$(printf '%s' "$STATS" | sed -n 's/.*"index_docs":\([0-9]*\).*/\1/p')
SERVED=$(printf '%s' "$STATS" | sed -n 's/.*"searches_served":\([0-9]*\).*/\1/p')
DEGRADED=$(printf '%s' "$STATS" | sed -n 's/.*"searches_degraded":\([0-9]*\).*/\1/p')
RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$DAEMON_PID/status" 2>/dev/null)
THREADS=$(awk '/^Threads:/ {print $2}' "/proc/$DAEMON_PID/status" 2>/dev/null)
# The model lives in the child, so the daemon's own RSS is only half the
# story; the Rust baseline's 321,580 kB is a TOTAL (133,884 daemon + 187,696
# embedder), and only a total is comparable to it.
EMB_PID=$(pgrep -P "$DAEMON_PID" -f kb-embedder 2>/dev/null | head -1)
EMB_RSS=$(awk '/^VmRSS:/ {print $2}' "/proc/$EMB_PID/status" 2>/dev/null)
TOTAL_RSS=$(awk -v a="${RSS:-0}" -v b="${EMB_RSS:-0}" 'BEGIN{print a+b}')

{
  cat "$SANDBOX/levels.env"
  printf '%s\n' "$RANKDIFF"
  echo "mode=${MODE}"
  echo "limit=${LIMIT}"
  echo "workers=${WORKERS}"
  echo "reps=${REPS}"
  echo "queries=${NQUERIES}"
  echo "sidecar=${SIDECAR}"
  echo "model=${MODEL}"
  echo "cache=${CACHE}"
  echo "vector_dim=${DIM}"
  echo "vector_rows=${VEC_ROWS}"
  echo "ingest_with_embeddings_s=${INGEST_S}"
  echo "index_docs=${DOCS_FINAL}"
  echo "searches_served=${SERVED}"
  echo "searches_degraded=${DEGRADED}"
  echo "daemon_rss_kb=${RSS}"
  echo "embedder_pid=${EMB_PID}"
  echo "embedder_rss_kb=${EMB_RSS}"
  echo "total_rss_kb=${TOTAL_RSS}"
  echo "daemon_threads=${THREADS}"
  echo "nproc=$(nproc)"
  echo "loadavg_start=$(loadavg)"
}

log "raw per-sample TSVs: ${SAMPLES}/c*.tsv ; table: ${TABLE}"
log "NOTE: absolute numbers are host-specific (shared 8-core box, loadavg well above 1 from OTHER users' processes: other kb daemons, chrome, omp). The SHAPE is the portable result."
log "NOTE: every query here embeds the query text through the sidecar, so this lane is bounded by the sidecar's throughput, not by kb-c's index."
log "NOTE: searches_degraded is the count of queries whose vector lane could not run. A non-zero value at the end means the lane broke mid-run; grep ${LOG} for 'embedder:'."
log "WARNING: if ${LOG} contains a line matching 'embedder: .*vector lane degraded', the lane was NOT running for the whole sweep and the numbers above are keyword latency with an embedding tax — check it before quoting them."
