#!/usr/bin/env bash
# bench-kbc.sh — kb-c query latency and throughput UNDER CONCURRENCY.
#
# Every other kb-c benchmark is single-client and serialised, so none of them
# says anything about what the epoll worker pool buys. This one does: it drives
# the same query set (QUERY_FILE, default bench/queries.txt — the same file
# bench-rust.sh uses) at several client concurrency levels, and reports latency
# percentiles AND aggregate requests/second for each level, plus the scaling
# factor relative to the 1-client level.
#
# Latency and throughput are both reported on purpose. A system whose p99
# explodes under load is not faster, and a system that only reports its best
# number is hiding the queue.
#
# Client: a purpose-built python3 program (embedded below) using one thread per
# simulated client, each holding a persistent HTTP/1.1 keep-alive connection.
# A `curl` shell loop is not usable here: each sample would pay ~5-10 ms of
# process startup, which is an order of magnitude larger than the 1 ms query
# being measured, and it cannot saturate 8 clients without the client becoming
# the thing under test. The python client spends microseconds per request
# outside the socket call and the GIL is released for every I/O; the script
# reports client-side CPU seconds so you can see how much headroom it had.
#
# Daemon: this script starts its OWN daemon from its own config in a private
# data dir, and kills only the pid it started. It refuses to run if its port is
# already in use, so it can never touch a daemon it did not start (the host
# runs a real Rust kb on port 4000; nothing here goes near it).
#
# Usage: bench-kbc.sh [CORPUS_DIR] [PORT]
# Env:   QUERY_FILE, REPS (default 20), LEVELS (default "1 2 4 8"),
#        WORKERS (daemon http workers, default 4 = the config default),
#        KBC_BIN, SANDBOX, PORT, KB_NAME
# Output: progress log on stderr; machine-readable key=value on stdout;
#         raw per-sample TSVs under $SANDBOX/samples.
#
# What the numbers showed on 2026-09-26 (this host, loadavg 19-34 from other
# users' processes, so the absolute values are not portable — the shape is).
# Ruled out with measurements, not guessed: not the client (4 client processes
# reach the same rps at 12% CPU each), not the epoll worker count (2, 4, 8 and
# 16 workers plateau identically), not CPU (~1.2 of 8 cores busy at the plateau)
# and not fd/connection limits (16 connections, 0 errors). Queries returning no
# hits — so that no row is resolved — reach 20.7k rps at 8 clients, so the
# scoring path itself scales and is not the limit; throughput tracks the hit
# count instead (8 clients: limit=1 -> 5.6k rps, limit=50 -> 0.9k).
#
# FIXED: the first real limit was the per-hit store resolve. app.c called
# kbc_store_get_artifact_by_path() once per returned hit and kbc_store holds one
# pthread_mutex around one SQLite connection, so every hit of every query
# serialised on that mutex; a wchan census under load showed 6 of 8 workers
# ~72% of the time in futex_do_wait. Resolving all of a query's hits in one
# kbc_store_get_artifacts_by_path() call took the lock once per query instead of
# once per hit. Interleaved A/B at 8 clients, REPS=30: 1832 -> 2336 rps median
# (+27%), p50 -20%, p99 halved. The limit=1 vs limit=50 spread — the cleanest
# evidence, since it isolates the per-hit cost — went 10.0x -> 6.0x.
#
# STILL THE LIMIT (not fixed): the marginal cost per returned hit is ~18 us, and
# it is the row read, not the lock. artifacts.source averages 12.5 KB, so every
# resolve walks a multi-page overflow record. Fixing it needs a schema change —
# a slim covering table without `source`, or a search projection that does not
# read the wide row. Until then, expect throughput to keep tracking 1/hits.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${1:-$ROOT/bench/data/corpus}"
PORT="${2:-${PORT:-4320}}"
KBC_BIN="${KBC_BIN:-$ROOT/build/kbc}"
SANDBOX="${SANDBOX:-/tmp/kbcbench-conc}"
KB_NAME="${KB_NAME:-kb}"
REPS="${REPS:-20}"
LEVELS="${LEVELS:-1 2 4 8}"
WORKERS="${WORKERS:-4}"
QUERY_FILE="${QUERY_FILE:-$ROOT/bench/queries.txt}"
MODE="keyword"
LIMIT=10
URL="http://127.0.0.1:${PORT}"
CFG="$SANDBOX/daemon.toml"
LOG="$SANDBOX/daemon.log"
SAMPLES="$SANDBOX/samples"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
loadavg() { awk '{print $1}' /proc/loadavg; }

DAEMON_PID=""
cleanup() {
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    log "stopping daemon ${DAEMON_PID} (the one this script started)"
    kill -TERM "$DAEMON_PID" 2>/dev/null
    for _ in $(seq 1 120); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.25; done
    kill -0 "$DAEMON_PID" 2>/dev/null && kill -KILL "$DAEMON_PID" 2>/dev/null
  fi
  # SIGTERM frees the listener at once; wait for the port to be free so this
  # script never leaves a listener behind.
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
if port_busy; then
  echo "ERROR port ${PORT} already in use; refusing to touch a daemon this script did not start" >&2
  exit 2
fi

# ---- query set (same file bench-rust.sh is driven from) ------------------
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
cat > "$CFG" <<EOF
[daemon]
data_dir = "${SANDBOX}/data"
port = ${PORT}
workers = ${WORKERS}

[[corpus]]
name = "${KB_NAME}"
path = "${CORPUS}"
EOF

log "corpus=${CORPUS} port=${PORT} workers=${WORKERS} reps=${REPS} levels='${LEVELS}' queries=${NQUERIES} loadavg=$(loadavg)"

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

# ---- make sure the index is populated ------------------------------------
DOCS=$(curl -fsS -m 10 "$URL/api/stats" | sed -n 's/.*"index_docs":\([0-9]*\).*/\1/p')
if [ "${DOCS:-0}" -lt 100 ]; then
  log "index has ${DOCS:-0} docs; running a full reindex"
  curl -fsS -m 600 -X POST "$URL/api/reindex" >/dev/null || true
  for _ in $(seq 1 2400); do
    DOCS=$(curl -fsS -m 10 "$URL/api/stats" | sed -n 's/.*"index_docs":\([0-9]*\).*/\1/p')
    [ "${DOCS:-0}" -ge 100 ] && break
    sleep 0.5
  done
fi
[ "${DOCS:-0}" -ge 100 ] || { echo "ERROR index never populated (docs=${DOCS:-0})" >&2; exit 4; }
log "index populated: ${DOCS} docs"

# ---- the load client -----------------------------------------------------
# One embedded python program. It is not a separate repo file: it is part of
# this benchmark's harness, and a throwaway client that others cannot read is
# worth less than one that lives in the script that uses it.
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
barrier = threading.Barrier(level + 1, timeout=60)


def worker(wid):
    conn = http.client.HTTPConnection(HOST, timeout=60)
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
                    conn = http.client.HTTPConnection(HOST, timeout=60)
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

# ---- the measurement loop ------------------------------------------------
TABLE="$SANDBOX/table.txt"
: > "$SANDBOX/levels.env"
: > "$TABLE"
BASE_RPS=""
BASE_P50=""
echo "host: $(uname -srm), $(nproc) cores; loadavg at start $(loadavg)" >&2
{
  echo "concurrency  clients  requests   wall_s     rps      p50_ms    p95_ms    p99_ms    loadavg  conns" >> "$TABLE"
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

{
  cat "$SANDBOX/levels.env"
  echo "workers=${WORKERS}"
  echo "reps=${REPS}"
  echo "queries=${NQUERIES}"
  echo "mode=${MODE}"
  echo "limit=${LIMIT}"
  echo "index_docs=${DOCS_FINAL}"
  echo "searches_served=${SERVED}"
  echo "searches_degraded=${DEGRADED}"
  echo "daemon_rss_kb=${RSS}"
  echo "daemon_threads=${THREADS}"
  echo "nproc=$(nproc)"
  echo "loadavg_start=$(loadavg)"
}

log "raw per-sample TSVs: ${SAMPLES}/c*.tsv ; table: ${SANDBOX}/table.txt"
log "NOTE: absolute numbers are host-specific (shared 8-core box, loadavg above ~30 during the run from OTHER users' processes: kb-embedder, chrome, omp). The SCALING SHAPE is the portable result."
log "NOTE: no comparison against the Rust daemon's concurrency behaviour is made here — that was not measured. This run says nothing about how the two engines compare under load."
log "NOTE: keyword mode only; the vector lane was not exercised."
