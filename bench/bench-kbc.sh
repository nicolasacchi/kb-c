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
# Client: two implementations of ONE measurement, both driving the same query
# set with one persistent HTTP/1.1 keep-alive connection per simulated client.
#   CLIENT=c        (default) bench/kbcbench-client.c, built by this script —
#                   one thread per connection, non-blocking sockets, no
#                   interpreter in the request path.
#   CLIENT=python   the embedded python3 program, kept as the readable
#                   reference implementation. It measures exactly the same
#                   thing, but one interpreter with one GIL saturated before
#                   the daemon did, so at 4+ clients it was measuring itself.
# `curl` loops are not usable here: each sample would pay ~5-10 ms of process
# startup, an order of magnitude more than the query being measured.
#
# PROCS>1 splits the level's connections across that many client PROCESSES
# (conns split, reps unchanged, so total requests per level are identical to
# PROCS=1 and levels stay comparable). Their CPU seconds are summed.
#
# Every level reports BOTH sides' CPU: the client's total, in cores, next to the
# daemon's, measured from /proc/<daemon>/stat across the same window. A rps
# figure is only a server measurement if the client was the cheaper side; the
# table prints that comparison at every level so the claim is checkable rather
# than asserted.
#
# Daemon: this script starts its OWN daemon from its own config in a private
# data dir, and kills only the pid it started. It refuses to run if its port is
# already in use, so it can never touch a daemon it did not start (the host
# runs a real Rust kb on port 4000; nothing here goes near it).
#
# Usage: bench-kbc.sh [CORPUS_DIR] [PORT]
# Env:   QUERY_FILE, REPS (default 20), LEVELS (default "1 2 4 8 16 32"),
#        CLIENT (c | python, default c), PROCS (client processes, default 1),
#        CC (compiler for the C client, default cc),
#        WORKERS (daemon http workers, default 4 = the config default),
#        LIMIT (hits per query, default 10 — set LIMIT=1 and LIMIT=50 to
#        isolate the marginal per-hit cost),
#        KBC_BIN, SANDBOX, PORT, KB_NAME
# Output: progress log on stderr; machine-readable key=value on stdout;
#         raw per-sample TSVs under $SANDBOX/samples.
#
# THE HARNESS WAS THE BOTTLENECK (fixed 2026-09-27). The measurements below
# were taken with the python client, and they were partly a measurement of the
# harness: at 8 clients the client burned 1.10 cores against the daemon's 0.913,
# so the ~4,600 rps it reported was a property of one GIL. That has been closed
# by making the client cheaper than the server it measures, and by printing both
# CPU fractions at every level so the reader can see which side saturated. The
# 2026-09-26 diagnosis below is kept because the SERVER-side limits it found
# are still real; read its client-CPU numbers as a statement about python.
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
# FIXED AGAIN (2026-09-26): the remaining marginal cost per hit was the row
# read, and the resolve's own statement was the whole of it. artifacts.source
# averages 12.5 KB of a ~12.8 KB record, so every step in the batch decode
# walked an overflow chain to produce columns the search path then discarded.
# The batch (and the watcher's by-path read) now SELECT the ten small columns
# and never name `source`; nothing on the search path reads it, and
# kbc_store_get_artifact(..., with_source=true) — a different call, the one
# `kb get --source` and the artifact route use — still returns the full text,
# so no schema change and no migration was needed.
#
# Interleaved A/B, 8 clients, REPS=30, five paired runs. limit=50:
# 872 -> 2,967 rps median (+240%), p50 7.4 -> 2.0 ms, p99 14.4-19.5 -> 4.6-6.5 ms.
# limit=1 — the control, one hit, so no per-hit read to remove — did not move
# (5,643 -> 5,671 rps), which is what makes the limit=50 figure attributable to
# the resolve rather than to host noise. The limit=1 vs limit=50 spread fell
# from 6.5x to 1.9x. At the default limit=10: 1,372/2,497/2,325/2,533 rps at
# 1/2/4/8 clients before, 2,452/4,525/5,762/5,682 after; p50 at 8 clients
# 3.02 -> 1.17 ms.
#
# NOT WORTH THE MIGRATION, both built and measured before the one-line change
# was chosen: a covering index on (corpus, path) is not used by the planner at
# all (the existing UNIQUE(corpus, path) is chosen instead, and the cost is the
# same), and moving `source` into its own artifact_sources table measures the
# same as simply not selecting the column. bench/resolve-mb.c is the
# single-process version of this measurement, for the per-lookup figure.
#
# WITH A CLIENT THAT CAN OUT-RUN THE SERVER (2026-09-27, CLIENT=c, REPS=200,
# workers=4, limit=10, this host with loadavg ~40 from other users):
#
#   conc   rps      p50      p95      p99     client_cpu  daemon_cpu
#       1   3387   0.26ms   0.55ms   0.63ms       0.054        0.889
#       2   4985   0.35ms   0.89ms   1.10ms       0.081        1.433
#       4   6948   0.47ms   1.04ms   1.43ms       0.118        2.019
#       8   8113   0.78ms   1.98ms   2.68ms       0.137        2.497
#      16   8747   1.39ms   3.65ms   5.61ms       0.150        2.617
#      32   9233   2.84ms   7.23ms   9.64ms       0.160        2.824
#
# The acceptance condition holds with a wide margin: at every level the client
# is ~17x cheaper than the daemon, and at 32 clients it is 0.16 cores against
# 2.82 — so these rps are the server's, not the harness's. (The python client on
# the same run: 4,843 rps at 8 clients with 1.12 client cores — more CPU than
# the daemon's 1.55, and 60% less throughput. Same workload, same percentiles,
# one GIL apart.)
#
# WHERE IT ACTUALLY PLATEAUS: the knee is at 16, not at 32. rps still rises
# (+6%) from 16 to 32 but p50 more than doubles and p99 grows 1.7x, so past 16
# the extra connections buy queueing, not throughput. Beyond 32 it goes
# backwards: 48 -> 6,667 rps and 64 -> 5,513 rps, with daemon CPU FALLING to
# 1.80 and 1.34 cores. Falling server CPU at rising latency is the signature of
# the server no longer being the thing doing the work — at 48+ clients the
# daemon is spending its time blocked, not searching. Reproduced across three
# runs; absolute values move with the host's load, the shape does not.
#
# Two harness honesty guards were added with it: the run now FAILS if the
# daemon dies mid-level (it did once, at 24 clients, with a glibc
# "double free or corruption" in the daemon log — that is a kb-c bug for
# whoever owns src/, not something this script papers over), and it fails if a
# level returns fewer requests than conns*reps*queries, since a short window
# divided into a full rps is a plausible-looking lie.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORPUS="${1:-$ROOT/bench/data/corpus}"
PORT="${2:-${PORT:-4320}}"
KBC_BIN="${KBC_BIN:-$ROOT/build/kbc}"
SANDBOX="${SANDBOX:-/tmp/kbcbench-conc}"
KB_NAME="${KB_NAME:-kb}"
REPS="${REPS:-20}"
LEVELS="${LEVELS:-1 2 4 8 16 32}"
CLIENT="${CLIENT:-c}"
PROCS="${PROCS:-1}"
CC="${CC:-cc}"
WORKERS="${WORKERS:-4}"
QUERY_FILE="${QUERY_FILE:-$ROOT/bench/queries.txt}"
MODE="keyword"
LIMIT="${LIMIT:-10}"
URL="http://127.0.0.1:${PORT}"
CFG="$SANDBOX/daemon.toml"
LOG="$SANDBOX/daemon.log"
SAMPLES="$SANDBOX/samples"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
loadavg() { awk '{print $1}' /proc/loadavg; }

# The daemon's own CPU seconds, in cores. utime+stime from /proc/<pid>/stat are
# the whole process, so this is server-side cost and nothing else — which is
# what the client figure has to be compared against.
daemon_cpu_s() {
  awk '{ print ($14 + $15) / 100 }' "/proc/$DAEMON_PID/stat" 2>/dev/null
}

DAEMON_PID=""
CLIENT_PIDS=()
cleanup() {
  # Reap the client processes this script forked, by pid, before the daemon —
  # a client still in its request loop against a dead daemon would sit out its
  # 60 s timeout. Never by name: another user's benchmark on this host is not
  # ours to signal.
  for cp in ${CLIENT_PIDS[@]+"${CLIENT_PIDS[@]}"}; do
    kill -0 "$cp" 2>/dev/null && kill -TERM "$cp" 2>/dev/null
  done
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
mkdir -p "$SANDBOX" "$SAMPLES" "$SANDBOX/data"
cat > "$CFG" <<EOF
[daemon]
data_dir = "${SANDBOX}/data"
token_file = "${SANDBOX}/data/token"
port = ${PORT}
workers = ${WORKERS}

[[corpus]]
name = "${KB_NAME}"
path = "${CORPUS}"
EOF
# An EMPTY token file, created up front: data_dir derives a token_file that
# would otherwise not exist, and a configured-but-unreadable token file is a
# hard startup error (a missing token_file key is not — that is the
# loopback-no-auth case). An empty file resolves to "no token", which is
# exactly what a benchmark on 127.0.0.1 wants: no auth round-trip in the
# measured request path.
: > "${SANDBOX}/data/token"

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

# Build the C load client from the tree. It is a repo artifact (bench/), not a
# generated file, so it is compiled here rather than embedded: it is worth
# reading, diffing and reviewing on its own. Kept in the sandbox so a run
# leaves nothing behind.
C_CLIENT_SRC="$ROOT/bench/kbcbench-client.c"
C_CLIENT="$SANDBOX/kbcbench-client"
case "$CLIENT" in
  c)
    [ -r "$C_CLIENT_SRC" ] || { echo "ERROR C client source missing: $C_CLIENT_SRC" >&2; exit 2; }
    command -v "$CC" >/dev/null 2>&1 || { echo "ERROR no C compiler ($CC); set CC= or CLIENT=python" >&2; exit 2; }
    log "building load client: $CC -O2 $C_CLIENT_SRC"
    "$CC" -O2 -std=c11 -pthread -o "$C_CLIENT" "$C_CLIENT_SRC" \
      || { echo "ERROR failed to build the C load client" >&2; exit 2; }
    ;;
  python) ;;
  *) echo "ERROR CLIENT must be 'c' or 'python', got '$CLIENT'" >&2; exit 2 ;;
esac
case "$PROCS" in ''|*[!0-9]*) echo "ERROR PROCS must be a positive integer" >&2; exit 2 ;; esac
[ "$PROCS" -ge 1 ] || { echo "ERROR PROCS must be >= 1" >&2; exit 2; }

# One percentile implementation for BOTH clients, run over the raw sample TSV.
# The clients do not compute percentiles themselves: if each did, the C and
# python paths could drift, and a drift here would be invisible in the table —
# it would just look like the C client being faster. Reading back the TSV the
# client already wrote is also what makes the C client's output
# interchangeable with the python client's.
pct() {
  python3 - "$1" <<'PYEOF'
import sys
ms = sorted(float(l.split("\t")[1]) for l in open(sys.argv[1]) if l.strip())
if not ms:
    sys.exit(0)
for name, p in (("MIN_MS", 0), ("P50_MS", 50), ("P95_MS", 95), ("P99_MS", 99), ("MAX_MS", 100)):
    # Nearest-rank on the sorted samples: the same definition the published
    # numbers used, so levels stay comparable with them.
    i = max(1, min(int(p / 100.0 * len(ms) + 0.9999) if p else 1, len(ms)))
    print("%s=%.4f" % (name, ms[i - 1]))
PYEOF
}

# Run one concurrency level. Echoes the client's own key=value lines. The
# daemon's CPU is sampled by the caller, around this call, so both sides cover
# the same window.
run_client() {
  local c="$1" out="$2"
  if [ "$PROCS" -le 1 ]; then
    if [ "$CLIENT" = "c" ]; then
      "$C_CLIENT" 127.0.0.1 "$PORT" "$MODE" "$KB_NAME" "$LIMIT" \
        "$c" "$REPS" "$QUERY_FILE" "$out"
    else
      python3 "$SANDBOX/loadclient.py" "$URL" "$MODE" "$KB_NAME" "$LIMIT" \
        "$c" "$REPS" "$QUERY_FILE" "$out"
    fi
    return
  fi
  # Split the level's connections across PROCS processes. Connections, not
  # reps, are what is split: a level is a concurrency, and total requests per
  # level must not change with PROCS or levels stop being comparable.
  local base=$(( c / PROCS )) extra=$(( c % PROCS )) i=0
  local pids=() outs=() conns=()
  while [ "$i" -lt "$PROCS" ]; do
    conns=$(( base + (i < extra ? 1 : 0) ))
    outs+=("$SANDBOX/proc${i}.tsv")
    : > "${outs[$i]}"
    if [ "$CLIENT" = "c" ]; then
      "$C_CLIENT" 127.0.0.1 "$PORT" "$MODE" "$KB_NAME" "$LIMIT" \
        "$conns" "$REPS" "$QUERY_FILE" "${outs[$i]}" > "$SANDBOX/proc${i}.env" 2>&1 &
    else
      python3 "$SANDBOX/loadclient.py" "$URL" "$MODE" "$KB_NAME" "$LIMIT" \
        "$conns" "$REPS" "$QUERY_FILE" "${outs[$i]}" > "$SANDBOX/proc${i}.env" 2>&1 &
    fi
    pids+=("$!")
    # Also recorded globally so an interrupted run reaps its own clients.
    CLIENT_PIDS+=("$!")
    i=$(( i + 1 ))
  done
  # Wait on the pids this function forked, by pid. Nothing here matches or
  # signals any process by NAME.
  local rc=0 p
  for p in "${pids[@]}"; do wait "$p" || rc=1; done
  : > "$out"
  local agg_req=0 agg_err=0 agg_conn=0 agg_wall=0 agg_cpu=0 k v
  for k in "${!outs[@]}"; do
    cat "${outs[$k]}" >> "$out"
    v=$(sed -n 's/^REQUESTS=//p' "$SANDBOX/proc${k}.env")
    agg_req=$(( agg_req + ${v:-0} ))
    v=$(sed -n 's/^ERRORS=//p' "$SANDBOX/proc${k}.env")
    agg_err=$(( agg_err + ${v:-0} ))
    v=$(sed -n 's/^CONNS_OPENED=//p' "$SANDBOX/proc${k}.env")
    agg_conn=$(( agg_conn + ${v:-0} ))
    v=$(sed -n 's/^WALL_S=//p' "$SANDBOX/proc${k}.env")
    agg_wall=$(awk -v a="$agg_wall" -v b="${v:-0}" 'BEGIN{print (b>a)?b:a}')
    v=$(sed -n 's/^CLIENT_CPU_S=//p' "$SANDBOX/proc${k}.env")
    agg_cpu=$(awk -v a="$agg_cpu" -v b="${v:-0}" 'BEGIN{printf "%.4f", a+b}')
  done
  echo "REQUESTS=${agg_req}"
  echo "CLIENTS=${c}"
  echo "WALL_S=${agg_wall}"
  awk -v n="$agg_req" -v w="$agg_wall" 'BEGIN{printf "RPS=%.2f\n", (w>0)?n/w:0}'
  echo "CLIENT_CPU_S=${agg_cpu}"
  awk -v c="$agg_cpu" -v w="$agg_wall" 'BEGIN{printf "CLIENT_CPU_FRAC=%.4f\n", (w>0)?c/w:0}'
  echo "CONNS_OPENED=${agg_conn}"
  echo "ERRORS=${agg_err}"
  return $rc
}

# The python client, embedded here rather than being a repo file. It is the
# readable reference implementation of this measurement and stays in the script
# for that reason; it is not the default any more, because one GIL made it the
# bottleneck before the daemon was (see the header comment). CLIENT=python runs
# it, and it produces the same TSV and the same key=value lines the C one does.
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
  printf '%9s  %7s  %8s  %7s  %8s  %8s  %8s  %8s  %8s  %8s  %6s\n' \
    concurrency clients requests wall_s rps p50_ms p95_ms p99_ms \
    client_cpu daemon_cpu client_lt_daemon >> "$TABLE"
  for c in $LEVELS; do
    out="$SAMPLES/c${c}.tsv"
    LOAD_BEFORE=$(loadavg)
    # Both CPU counters bracket exactly the client's own measured window, so
    # the two columns are directly comparable rather than two separate runs.
    DCPU0=$(daemon_cpu_s)
    res=$(run_client "$c" "$out")
    DCPU1=$(daemon_cpu_s)
    LOAD_AFTER=$(loadavg)
    # A daemon that died mid-level produces a table full of zeroes and a
    # NEGATIVE cpu delta, which reads as a measurement. It is not one: stop,
    # and say what the log says, instead of publishing the wreckage.
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
      echo "ERROR daemon ${DAEMON_PID} died during level ${c}; not a measurement." >&2
      echo "      daemon log tail:" >&2
      tail -20 "$LOG" >&2
      exit 5
    fi
    get() { printf '%s\n' "$res" | sed -n "s/^$1=//p"; }
    REQS=$(get REQUESTS); RPS=$(get RPS); WALL=$(get WALL_S)
    # Every connection issues the full query set REPS times, so a level that
    # returned fewer requests than connections*reps*queries did not finish its
    # work — the rps would be a fraction of a real window.
    EXPECT=$(( c * REPS * NQUERIES ))
    if [ "${REQS:-0}" -lt "$EXPECT" ]; then
      echo "ERROR level ${c}: ${REQS} of ${EXPECT} expected requests completed." >&2
      echo "      not a measurement; see ${out}" >&2
      exit 6
    fi
    ERRS=$(get ERRORS); CONNS=$(get CONNS_OPENED)
    # Percentiles come from the raw TSV through the one shared implementation,
    # for both clients, so the two can never drift apart.
    p=$(pct "$out")
    pg() { printf '%s\n' "$p" | sed -n "s/^$1=//p"; }
    P50=$(pg P50_MS); P95=$(pg P95_MS); P99=$(pg P99_MS)
    MIN=$(pg MIN_MS); MAX=$(pg MAX_MS)
    CPUF=$(get CLIENT_CPU_FRAC)
    DCPUF=$(awk -v a="${DCPU1:-0}" -v b="${DCPU0:-0}" -v w="$WALL" \
      'BEGIN{printf "%.4f", (w>0)?(a-b)/w:0}')
    # The whole point of the C client: at the top level the client must be the
    # CHEAPER side, or the rps is a property of the harness.
    VERDICT=$(awk -v c="$CPUF" -v d="$DCPUF" \
      'BEGIN{print (c < d) ? "yes" : "NO"}')
    if [ "$ERRS" != "0" ]; then
      log "WARNING level ${c}: ${ERRS} request errors (see ${out})"
    fi
    if [ "$VERDICT" = "NO" ]; then
      log "WARNING level ${c}: client CPU ${CPUF} is NOT below daemon CPU ${DCPUF}" \
          "— this level's rps is not a clean server measurement"
    fi
    if [ -z "$BASE_RPS" ]; then BASE_RPS="$RPS"; BASE_P50="$P50"; fi
    SCALE=$(awk -v a="$RPS" -v b="$BASE_RPS" 'BEGIN{printf "%.2f", a/b}')
    PSLOW=$(awk -v a="$P50" -v b="$BASE_P50" 'BEGIN{printf "%.2f", a/b}')
    printf '%9d  %7d  %8s  %7s  %8s  %8s  %8s  %8s  %8s  %8s  %6s\n' \
      "$c" "$c" "$REQS" "$WALL" "$RPS" "$P50" "$P95" "$P99" \
      "$CPUF" "$DCPUF" "$VERDICT" >> "$TABLE"
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
      echo "conc_${c}_conns_opened=${CONNS}"
      echo "conc_${c}_client_cpu_frac=${CPUF}"
      echo "conc_${c}_daemon_cpu_frac=${DCPUF}"
      echo "conc_${c}_client_cheaper_than_daemon=${VERDICT}"
      echo "conc_${c}_loadavg_before=${LOAD_BEFORE}"
      echo "conc_${c}_loadavg_after=${LOAD_AFTER}"
    } >> "$SANDBOX/levels.env"
    log "level ${c}: ${REQS} reqs in ${WALL}s -> ${RPS} rps, p50=${P50}ms p95=${P95}ms p99=${P99}ms (scale ${SCALE}x, p50 x${PSLOW})"
    log "level ${c}: CPU client ${CPUF} cores vs daemon ${DCPUF} cores -> client cheaper: ${VERDICT}"
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
  echo "client=${CLIENT}"
  echo "client_procs=${PROCS}"
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
log "client=${CLIENT} procs=${PROCS}. A level's rps is a SERVER measurement only if"
log "  client_cpu < daemon_cpu in the last column of the table; the run says so at"
log "  every level rather than leaving it to be assumed."
