#!/usr/bin/env bash
# bench-rust-concurrency.sh — the Rust `kb` daemon under concurrent load.
#
# BENCHMARKS.md reports kb-c's scaling curve at 1/2/4/8 clients and says, in
# the same breath, that the Rust daemon's concurrency was never measured, so
# there is no comparison to draw. This is that comparison. It drives the same
# query set (bench/queries.txt, the file every other number in this repo was
# measured with), at the same client levels, with the same client design as
# bench-kbc.sh, and reports the same percentiles plus the two numbers that say
# whether a system scaled or merely queued:
#
#   * SERVER_CORES_BUSY — server CPU seconds per wall second during the level.
#     Below 1.0 the server had idle capacity and the limit is elsewhere.
#   * SERVER_CPU_US_PER_REQ — server CPU per query. This is the number that
#     compares the two engines, because it is per unit of work done and does
#     not care how many clients were asking at once.
#
# and the one that says the client was not the thing under test:
#
#   * CLIENT_CPU_FRAC — client CPU as a fraction of wall time.
#
# ENGINE=kbc runs the identical client, the identical queries and the identical
# accounting against a kb-c daemon this script starts itself. That is what makes
# the per-query CPU comparison an A/B rather than a comparison of two different
# harnesses measured on different days.
#
# Isolation, following bench-rust.sh: every kb invocation runs with HOME and
# XDG_* pointed at a private sandbox under /tmp, so the real ~/.config/kb is
# never read or written. The port is a private one, verified free before the
# run. Only processes this script started are ever signalled.
#
# The real Rust daemon on this host (ports 4000/4001) belongs to someone else.
# This script never binds, signals, reconfigures or writes to it. It does take
# one read-only liveness snapshot of it before the run and one after: a GET of
# / (which changes nothing) plus a TCP connect on each of its two ports. If the
# answer changed, the run is reported as having damaged someone else's daemon.
# A benchmark that breaks the machine it runs on is not a benchmark.
#
# Usage: bench-rust-concurrency.sh [CORPUS_DIR] [PORT]   (default port 4362;
#        ENGINE=kbc wants a different one, e.g. 4363, since both engines must
#        be measurable at the same time for the per-query CPU A/B)
# Env:   REPS (default 30), LEVELS (default "1 2 4 8"), LIMIT (default 10),
#        QUERY_FILE, KB_BIN, KBC_BIN, SANDBOX, KB_NAME, ENGINE (rust|kbc),
#        WORKERS (kb-c http workers, default 4), INGEST_TIMEOUT (default 3600 s)
# Output: progress on stderr; key=value and a table on stdout; raw per-sample
#         TSVs under $SANDBOX/samples.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${1:-$ROOT/bench/data/corpus}"
PORT="${2:-4362}"
ENGINE="${ENGINE:-rust}"
KB_BIN="${KB_BIN:-/home/nik/project/kb/target/release/kb}"
KBC_BIN="${KBC_BIN:-$ROOT/build/kbc}"
SANDBOX="${SANDBOX:-/tmp/kbbench-rust-conc}"
KB_NAME="${KB_NAME:-bench}"
REPS="${REPS:-30}"
LEVELS="${LEVELS:-1 2 4 8}"
LIMIT="${LIMIT:-10}"
WORKERS="${WORKERS:-4}"
QUERY_FILE="${QUERY_FILE:-$ROOT/bench/queries.txt}"
MODE="keyword"
TOKEN=""
INGEST_TIMEOUT="${INGEST_TIMEOUT:-3600}"
URL="http://127.0.0.1:${PORT}"
LOG="$SANDBOX/daemon.log"
SAMPLES="$SANDBOX/samples"
CLK_TCK=$(getconf CLK_TCK)

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
loadavg() { awk '{print $1}' /proc/loadavg; }
port_busy() { ss -ltn 2>/dev/null | grep -q ":${PORT} "; }

# Only kb-c needs this: its daemon is started WITH a token, exactly as an
# operator would run it, so every request presents it and the measured path is
# the deployed one. The Rust daemon takes no token in this configuration.
ACURL() {
  if [ -n "${TOKEN:-}" ]; then
    curl -H "Authorization: Bearer ${TOKEN}" "$@"
  else
    curl "$@"
  fi
}

# ---- the other daemon on this host, read-only -----------------------------
# One GET of / (200 = serving) plus a TCP connect on each of its ports. No
# state is changed by either. Compared before and after the run.
other_daemon_snapshot() {
  for p in 4000 4001; do
    code=$(curl -s -o /dev/null -m 5 -w '%{http_code}' "http://127.0.0.1:${p}/" 2>/dev/null)
    tcp=$(timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/${p}" 2>/dev/null && echo up || echo down)
    printf '%s:http=%s,tcp=%s ' "$p" "${code:-none}" "$tcp"
  done
}

# ---- server CPU -----------------------------------------------------------
# utime+stime from /proc/PID/stat, in clock ticks. Parsed after the comm field
# so a process name containing a space cannot shift the columns.
server_cpu_ticks() {
  # utime+stime for $1 and every descendant, in clock ticks. The Rust daemon
  # forks a listener child and spawns kb-embedder beside it; counting only the
  # spawned pid would report the server as idle while it worked. Descendants
  # are found by ppid, never by process name.
  local total=0 p kids
  for p in "$1" $(kids_of "$1"); do
    [ -r "/proc/$p/stat" ] || continue
    t=$(awk '{ n = index($0, ")"); split(substr($0, n + 2), f, " ");
                  printf "%d", f[12] + f[13] }' "/proc/$p/stat" 2>/dev/null)
    total=$((total + ${t:-0}))
  done
  printf "%d" "$total"
}
kids_of() {
  ps -eo pid=,ppid= 2>/dev/null | awk -v want="$1" '
    { parent[$1] = $2 }
    END { for (p in parent) if (parent[p] == want) print p }'
}
port_owner_pid() {
  ss -ltnp 2>/dev/null | grep ":${PORT} " | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1
}
rss_kb() { awk '/^VmRSS:/ {print $2}' "/proc/$1/status" 2>/dev/null; }

DAEMON_PID=""
cleanup() {
  # Children first (the embedder helper), then the daemon, then the spawner.
  local c
  for c in $(kids_of "$DAEMON_PID") "$DAEMON_PID" "${SPAWNED_PID:-}"; do
    [ -n "$c" ] || continue
    kill -0 "$c" 2>/dev/null || continue
    log "stopping pid ${c} (started by this script: ${DAEMON_PID}/${SPAWNED_PID:-})"
    kill -TERM "$c" 2>/dev/null
  done
  for _ in $(seq 1 240); do
    port_busy || break
    sleep 0.25
  done
  for c in $(kids_of "$DAEMON_PID") "$DAEMON_PID" "${SPAWNED_PID:-}"; do
    [ -n "$c" ] || continue
    kill -0 "$c" 2>/dev/null && kill -KILL "$c" 2>/dev/null
  done
  for _ in $(seq 1 40); do
    port_busy || break
    sleep 0.25
  done
  if port_busy; then log "ERROR port ${PORT} still listening after cleanup"; fi
}
trap cleanup EXIT INT TERM

# ---- preflight ------------------------------------------------------------
case "$ENGINE" in
  rust)
    [ -x "$KB_BIN" ] || { echo "ERROR kb binary not found: $KB_BIN" >&2; exit 2; }
    ;;
  kbc)
    [ -x "$KBC_BIN" ] || { echo "ERROR kbc binary not found: $KBC_BIN" >&2; exit 2; }
    ;;
  *) echo "ERROR ENGINE must be rust or kbc, got: $ENGINE" >&2; exit 2;;
esac
[ -d "$CORPUS" ] || { echo "ERROR corpus dir not found: $CORPUS" >&2; exit 2; }
CORPUS="$(cd "$CORPUS" && pwd)"
[ -r "$QUERY_FILE" ] || { echo "ERROR QUERY_FILE not readable: $QUERY_FILE" >&2; exit 2; }
if port_busy; then
  echo "ERROR port ${PORT} already in use; refusing to touch a daemon this script did not start" >&2
  exit 2
fi

QUERIES=()
while IFS= read -r qline; do
  case "$qline" in ''|\#*) continue ;; esac
  QUERIES+=("$qline")
done < "$QUERY_FILE"
NQUERIES=${#QUERIES[@]}
[ "$NQUERIES" -gt 0 ] || { echo "ERROR QUERY_FILE held no queries: $QUERY_FILE" >&2; exit 2; }

EXPECTED_DOCS=$(find "$CORPUS" -type f \( -name '*.md' -o -name '*.html' -o -name '*.htm' \) 2>/dev/null | wc -l | tr -d ' ')
CORPUS_BYTES=$(find "$CORPUS" -type f \( -name '*.md' -o -name '*.html' -o -name '*.htm' \) -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')

OTHER_BEFORE=$(other_daemon_snapshot)
log "engine=${ENGINE} corpus=${CORPUS} files=${EXPECTED_DOCS} bytes=${CORPUS_BYTES} port=${PORT} reps=${REPS} levels='${LEVELS}' limit=${LIMIT} queries=${NQUERIES} loadavg=$(loadavg)"
log "other daemon (4000/4001) before: ${OTHER_BEFORE}"

# ---- clean state + private config ----------------------------------------
rm -rf "$SANDBOX"
mkdir -p "$SAMPLES" "$SANDBOX/.config/kb" "$SANDBOX/.local/share" \
         "$SANDBOX/.cache" "$SANDBOX/state" "$SANDBOX/data"
if [ "$ENGINE" = "rust" ]; then
  CFG="$SANDBOX/.config/kb/kb.toml"
  cat > "$CFG" <<EOF
[daemon]
name = "bench-rust-conc"

[server]
addr = "127.0.0.1:${PORT}"

[kb.${KB_NAME}]
path = "${CORPUS}"
EOF
  kbenv() {
    env HOME="$SANDBOX" \
        XDG_CONFIG_HOME="$SANDBOX/.config" \
        XDG_DATA_HOME="$SANDBOX/.local/share" \
        XDG_CACHE_HOME="$SANDBOX/.cache" \
        XDG_STATE_HOME="$SANDBOX/state" \
        "$@"
  }
  STATS_URL="$URL/api/kb/${KB_NAME}/stats"
  start_cmd=(kbenv "$KB_BIN" daemon --config "$CFG")
else
  CFG="$SANDBOX/daemon.toml"
  TOKEN="$(od -An -tx1 -N24 /dev/urandom | tr -d ' \n')"
  printf '%s' "$TOKEN" > "$SANDBOX/data/token"
  cat > "$CFG" <<EOF
[daemon]
data_dir = "${SANDBOX}/data"
port = ${PORT}
workers = ${WORKERS}
token_file = "${SANDBOX}/data/token"

[[corpus]]
name = "${KB_NAME}"
path = "${CORPUS}"
EOF
  kbenv() { "$@"; }
  STATS_URL="$URL/api/stats"
  start_cmd=("$KBC_BIN" daemon --config "$CFG" --foreground)
fi

# ---- start, wait for ready, wait for ingest -------------------------------
"${start_cmd[@]}" >"$LOG" 2>&1 &
SPAWNED_PID=$!
DAEMON_PID="$SPAWNED_PID"
ready=""
for _ in $(seq 1 1500); do
  if ACURL -fsS -m 2 "$STATS_URL" >/dev/null 2>&1; then ready=1; break; fi
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "ERROR daemon exited during startup; log tail:" >&2; tail -20 "$LOG" >&2; exit 3
  fi
  sleep 0.2
done
[ -n "$ready" ] || { echo "ERROR daemon never became ready; log tail:" >&2; tail -20 "$LOG" >&2; exit 3; }
# The Rust daemon forks, so the pid that spawned is not the pid that listens.
# Measure and stop the one that owns the port, or every number here describes a
# process that was already gone.
OWNER_PID=$(port_owner_pid)
DAEMON_PID="${OWNER_PID:-$SPAWNED_PID}"
log "daemon ready on ${URL}: spawned=${SPAWNED_PID} port_owner=${OWNER_PID:-unknown} measuring=${DAEMON_PID}"

# The Rust daemon ingests on its own schedule, and on a corpus much larger
# than the 1,594-document one that took 252-262 s it takes proportionally
# longer. Waiting here rather than timing a short window is the whole point:
# measuring a daemon that is still ingesting measures ingest, not queries.
doc_count() {
  ACURL -fsS -m 10 "$STATS_URL" 2>/dev/null |
    sed -n 's/.*"\(doc_count\|index_docs\)":\([0-9]*\).*/\2/p'
}

# The Rust daemon scans the corpus by itself when it starts. kb-c does not: it
# serves whatever index is on disk, and scans only when asked. So under
# ENGINE=kbc the scan is requested here, once, and the same wait applies to it.
if [ "$ENGINE" = "kbc" ] && [ "$(doc_count)" -lt 1 ]; then
  log "kb-c does not scan at startup; POSTing /api/reindex"
  ACURL -fsS -m "$INGEST_TIMEOUT" -X POST "$URL/api/reindex" >/dev/null 2>&1
fi
T_INGEST_START=$(date +%s.%N)
LAST=-1; STUCK=0; DOCS=0
for _ in $(seq 1 $((INGEST_TIMEOUT * 2))); do
  DOCS=$(doc_count)
  [ -n "$DOCS" ] || DOCS=0
  # Only two ways out: the corpus is fully indexed, or the count has not moved
  # for 120 s. An earlier version also broke on "the count repeated once",
  # which the Rust daemon triggers while it is still ingesting — its stats
  # endpoint is slow under load and two polls in a row can return the same
  # number. That measured a daemon mid-ingest: 119 of 1,594 documents, and its
  # ingest CPU counted as query CPU.
  if [ "$DOCS" -ge "$EXPECTED_DOCS" ]; then
    break
  fi
  if [ "$DOCS" != "$LAST" ]; then STUCK=0; else STUCK=$((STUCK + 1)); fi
  [ "$STUCK" -lt 240 ] || break       # 120 s with no movement: call it done
  LAST=$DOCS
  if [ $((STUCK % 40)) -eq 0 ] && [ "$STUCK" -gt 0 ]; then
    log "  ingest at ${DOCS}/${EXPECTED_DOCS} docs, no movement for $((STUCK / 2))s"
  fi
  sleep 0.5
done
INGEST_S=$(awk -v a="$T_INGEST_START" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}')
if [ "${STUCK:-0}" -ge 240 ] && [ "${DOCS:-0}" -lt "$EXPECTED_DOCS" ]; then
  if [ "$ENGINE" = "rust" ]; then
    log "WARNING ingest stalled at ${DOCS} of ${EXPECTED_DOCS} docs for 120 s; measuring anyway"
  else
    log "ingest settled at ${DOCS} docs; kb-c's walk skips dot-directories, so it indexes fewer than the ${EXPECTED_DOCS} staged files"
  fi
fi
log "ingest: ${DOCS} docs indexed (expected ${EXPECTED_DOCS}) in ${INGEST_S}s"
[ "${DOCS:-0}" -ge 1 ] || { echo "ERROR index empty; not measuring" >&2; exit 4; }

# ---- the load client ------------------------------------------------------
# One thread per simulated client, each holding a persistent HTTP/1.1
# keep-alive connection, all released from a barrier so the measured window
# contains no ramp-up. A curl shell loop is not usable here: each sample would
# pay milliseconds of process startup, which is larger than the query being
# measured, and it cannot saturate 8 clients without the client becoming the
# thing under test.
cat > "$SANDBOX/loadclient.py" <<'PYEOF'
"""Closed-loop HTTP load client: one thread per client, one keep-alive
connection each, a barrier so the window has no ramp-up, and every client
issuing the full query set the same number of times in the same order.

Client CPU is reported as a fraction of wall time: a reader must be able to
see that the client was not the thing being measured.
"""
import http.client, sys, threading, time, urllib.parse

url, mode, kb, limit = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
level, reps = int(sys.argv[5]), int(sys.argv[6])
qfile, out = sys.argv[7], sys.argv[8]
token = sys.argv[9] if len(sys.argv) > 9 else ""
AUTH = {"Authorization": "Bearer " + token} if token else {}

queries = []
for line in open(qfile):
    line = line.rstrip("\n")
    if line.strip() and not line.startswith("#"):
        queries.append(line.split("\t", 1)[-1].strip())

HOST = url.split("//", 1)[1]
samples = []
lock = threading.Lock()
errors = [0]
conns_opened = [0]
# level + 1: the workers plus this thread. With `level` parties the main
# thread would satisfy the barrier alongside the FIRST worker and release it
# early, stranding the rest.
barrier = threading.Barrier(level + 1, timeout=120)


def worker(_wid):
    conn = http.client.HTTPConnection(HOST, timeout=120)
    with lock:
        conns_opened[0] += 1
    local = []
    barrier.wait()
    for _ in range(reps):
        for qi, q in enumerate(queries):
            req = "/api/search?q={}&mode={}&kb={}&limit={}".format(
                urllib.parse.quote(q), mode, kb, limit)
            t0 = time.perf_counter()
            for attempt in (0, 1):
                try:
                    conn.request("GET", req, headers=AUTH)
                    r = conn.getresponse()
                    r.read()
                    if r.status != 200:
                        raise RuntimeError("status %d" % r.status)
                    break
                except Exception:
                    try:
                        conn.close()
                    except Exception:
                        pass
                    conn = http.client.HTTPConnection(HOST, timeout=120)
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
for i in range(level):
    t = threading.Thread(target=worker, args=(i,))
    t.start()
    threads.append(t)
barrier.wait()
wall0 = time.perf_counter()
cpu1 = time.process_time()
for t in threads:
    t.join()
wall1 = time.perf_counter()
cpu_total = time.process_time() - cpu0

with open(out, "w") as fh:
    for qi, ms in samples:
        fh.write("%d\t%.4f\n" % (qi, ms))
ms_sorted = sorted(ms for _, ms in samples)


def pct(p):
    if not ms_sorted:
        return float("nan")
    i = max(1, min(len(ms_sorted), int((p / 100.0) * len(ms_sorted) + 0.9999)))
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

# ---- the measurement loop -------------------------------------------------
: > "$SANDBOX/levels.env"
{
  printf '%8s %9s %9s %8s %9s %9s %9s %9s %9s %9s %11s %11s %8s\n' \
    "clients" "requests" "wall_s" "rps" "scale" "p50_ms" "p95_ms" "p99_ms" \
    "srv_cpu_s" "cores_busy" "cpu_us/req" "client_cpu%" "loadavg"
} > "$SANDBOX/table.txt"

BASE_RPS=""
{
  echo "engine=$ENGINE"
  echo "corpus=$CORPUS"
  echo "corpus_files=$EXPECTED_DOCS"
  echo "corpus_bytes=$CORPUS_BYTES"
  echo "index_docs=$DOCS"
  echo "ingest_s=$INGEST_S"
  echo "port=$PORT"
  echo "reps=$REPS"
  echo "limit=$LIMIT"
  echo "queries=$NQUERIES"
  echo "loadavg_at_start=$(loadavg)"
  echo "host_cores=$(nproc)"
  echo "clk_tck=$CLK_TCK"
} > "$SANDBOX/levels.env"

for c in $LEVELS; do
  out="$SAMPLES/c${c}.tsv"
  LOAD_BEFORE=$(loadavg)
  TICKS0=$(server_cpu_ticks "$DAEMON_PID")
  res=$(python3 "$SANDBOX/loadclient.py" "$URL" "$MODE" "$KB_NAME" "$LIMIT" \
          "$c" "$REPS" "$QUERY_FILE" "$out" "$TOKEN")
  TICKS1=$(server_cpu_ticks "$DAEMON_PID")
  LOAD_AFTER=$(loadavg)
  get() { printf '%s\n' "$res" | sed -n "s/^$1=//p"; }
  REQS=$(get REQUESTS); RPS=$(get RPS); WALL=$(get WALL_S)
  P50=$(get P50_MS); P95=$(get P95_MS); P99=$(get P99_MS)
  MIN=$(get MIN_MS); MAX=$(get MAX_MS); ERRS=$(get ERRORS)
  CONNS=$(get CONNS_OPENED); CPUF=$(get CLIENT_CPU_FRAC)
  SRV_S=$(awk -v t0="$TICKS0" -v t1="$TICKS1" -v h="$CLK_TCK" \
          'BEGIN{printf "%.4f", (t1-t0)/h}')
  CORES=$(awk -v s="$SRV_S" -v w="$WALL" 'BEGIN{printf "%.3f", (w>0)? s/w : 0}')
  CPUUS=$(awk -v s="$SRV_S" -v n="$REQS" 'BEGIN{printf "%.1f", (n>0)? s*1e6/n : 0}')
  if [ "$ERRS" != "0" ]; then log "WARNING level ${c}: ${ERRS} request errors (${out})"; fi
  [ -n "$BASE_RPS" ] || BASE_RPS="$RPS"
  SCALE=$(awk -v a="$RPS" -v b="$BASE_RPS" 'BEGIN{printf "%.2f", (b>0)? a/b : 0}')
  printf '%8s %9s %9s %8s %9s %9s %9s %9s %9s %9s %11s %11s %8s\n' \
    "$c" "$REQS" "$WALL" "$RPS" "${SCALE}x" "$P50" "$P95" "$P99" \
    "$SRV_S" "$CORES" "$CPUUS" "$CPUF" "$LOAD_AFTER" >> "$SANDBOX/table.txt"
  {
    echo "c${c}_requests=$REQS"
    echo "c${c}_wall_s=$WALL"
    echo "c${c}_rps=$RPS"
    echo "c${c}_scale_vs_1=$SCALE"
    echo "c${c}_p50_ms=$P50"
    echo "c${c}_p95_ms=$P95"
    echo "c${c}_p99_ms=$P99"
    echo "c${c}_min_ms=$MIN"
    echo "c${c}_max_ms=$MAX"
    echo "c${c}_server_cpu_s=$SRV_S"
    echo "c${c}_server_cores_busy=$CORES"
    echo "c${c}_server_cpu_us_per_req=$CPUUS"
    echo "c${c}_client_cpu_frac=$CPUF"
    echo "c${c}_errors=$ERRS"
    echo "c${c}_conns=$CONNS"
    echo "c${c}_loadavg_after=$LOAD_AFTER"
  } >> "$SANDBOX/levels.env"
  log "level ${c}: ${REQS} reqs in ${WALL}s -> ${RPS} rps (${SCALE}x), p50=${P50}ms p95=${P95}ms p99=${P99}ms, server ${SRV_S}s cpu = ${CORES} cores busy, ${CPUUS} us cpu/req, client ${CPUF}"
done

cat "$SANDBOX/table.txt"
RSS=$(rss_kb "$DAEMON_PID")
THREADS=$(awk '/^Threads:/ {print $2}' "/proc/$DAEMON_PID/status" 2>/dev/null)
{
  echo "daemon_rss_kb=$RSS"
  echo "daemon_threads=$THREADS"
  echo "loadavg_at_end=$(loadavg)"
} >> "$SANDBOX/levels.env"

# ---- the other daemon, again ----------------------------------------------
OTHER_AFTER=$(other_daemon_snapshot)
log "other daemon (4000/4001) after:  ${OTHER_AFTER}"
if [ "$OTHER_BEFORE" != "$OTHER_AFTER" ]; then
  echo "ERROR the other daemon's liveness changed across this run:" >&2
  echo "  before: ${OTHER_BEFORE}" >&2
  echo "  after:  ${OTHER_AFTER}" >&2
  OTHER_OK=0
else
  OTHER_OK=1
fi
{
  echo "other_daemon_before=$OTHER_BEFORE"
  echo "other_daemon_after=$OTHER_AFTER"
  echo "other_daemon_untouched=$OTHER_OK"
} >> "$SANDBOX/levels.env"

echo
cat "$SANDBOX/levels.env"
log "results: $SANDBOX/levels.env ; raw per-sample TSVs: $SAMPLES/*.tsv"
log "NOTE: keyword mode only, limit=${LIMIT}, REPS=${REPS}, same query file as every other benchmark in this repo."
log "NOTE: this host is shared; absolute numbers are not portable, the SCALING SHAPE is."
exit 0
