#!/usr/bin/env bash
# bench-rust.sh — Rust `kb` daemon latency/resource baseline.
#
# Measures, from a completely clean state and in full isolation from the
# real ~/.config/kb installation:
#   * startup-to-ready (process start -> daemon answering HTTP)
#   * cold ingest wall clock + resulting document count
#   * end-to-end HTTP query latency (min/p50/p95/p99) over a fixed query set
#   * in-process (lance-direct, no HTTP) keyword query latency, same set
#   * idle RSS of the daemon (and its embedder subprocess)
#
# Isolation: every kb invocation runs with HOME/XDG_* pointed at a private
# sandbox dir. The real ~/.config/kb is never read or written. The port is
# a private one (default 4318), verified free before and after. Only
# processes this script itself started are ever signalled.
#
# Usage: bench-rust.sh [CORPUS_DIR] [PORT]
# Env:   REPS (default 20), KB_BIN, SANDBOX
# Output: progress log on stderr; machine-readable key=value on stdout.
set -uo pipefail

CORPUS="${1:-/home/nik/project/kb-c/bench/data/corpus}"
PORT="${2:-4318}"
KB_BIN="${KB_BIN:-/home/nik/project/kb/target/release/kb}"
SANDBOX="${SANDBOX:-/tmp/kbbench-rust}"
KB_NAME="bench"
REPS="${REPS:-20}"
URL="http://127.0.0.1:${PORT}"
LOG="${SANDBOX}/daemon.log"
SAMPLES="$SANDBOX/samples"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

# ---- query set -----------------------------------------------------------
# QUERY_FILE drives both engines from ONE list, so the head-to-head is a real
# A/B. Without it the two sides would each use their own queries and the
# comparison would be weaker than it looks. One query per line; blank lines and
# lines starting with # are ignored.
if [ -n "${QUERY_FILE:-}" ]; then
  [ -r "$QUERY_FILE" ] || { log "QUERY_FILE not readable: $QUERY_FILE"; exit 2; }
  QUERIES=()
  while IFS= read -r qline; do
    case "$qline" in ''|\#*) continue ;; esac
    QUERIES+=("$qline")
  done < "$QUERY_FILE"
  [ ${#QUERIES[@]} -gt 0 ] || { log "QUERY_FILE held no queries: $QUERY_FILE"; exit 2; }
  log "query set: QUERY_FILE=$QUERY_FILE (${#QUERIES[@]} queries)"
else
  QUERIES=(
    "daemon"
    "artifact provenance"
    "markdown tokenizer"
    "lancedb embedding"
    "benchmark milestone"
    "watcher reconcile"
    "throttle telemetry"
    "onboarding backpressure"
  )
fi
NQUERIES=${#QUERIES[@]}

DAEMON_PID=""
EMBEDDER_PIDS=""

collect_embedders() {
  # Only ever children of OUR sandbox daemon. Never a pattern-kill: the real
  # ~/.config/kb installation runs its own daemon and embedder of the same names.
  EMBEDDER_PIDS=$(pgrep -P "$DAEMON_PID" 2>/dev/null | tr '\n' ' ')
}

cleanup() {
  log "cleanup: stopping daemon"
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    collect_embedders
    # The kb daemon closes its listener promptly on SIGTERM but then spends
    # ~30 s finishing the lance/index shutdown, so "process still alive" is
    # not the acceptance test — "nothing is listening on our port" is.
    kill -TERM "$DAEMON_PID" 2>/dev/null
    for _ in $(seq 1 90); do
      port_busy || break
      kill -0 "$DAEMON_PID" 2>/dev/null || break
      sleep 0.5
    done
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
      log "cleanup: daemon still alive after 45 s, sending SIGKILL"
      kill -KILL "$DAEMON_PID" 2>/dev/null
    fi
  fi
  for p in $EMBEDDER_PIDS; do kill -TERM "$p" 2>/dev/null; done
  sleep 1
  for p in $EMBEDDER_PIDS; do kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null; done
  DAEMON_PID=""
  for _ in $(seq 1 20); do port_busy || break; sleep 0.5; done
  if port_busy; then log "WARNING: something is still listening on ${PORT}"; else log "cleanup: port ${PORT} free"; fi
}
trap cleanup EXIT INT TERM

port_busy() { ss -ltn 2>/dev/null | grep -q ":${PORT} "; }

# ---- preflight -----------------------------------------------------------
[ -x "$KB_BIN" ] || { echo "ERROR kb binary not found: $KB_BIN" >&2; exit 2; }
[ -d "$CORPUS" ] || { echo "ERROR corpus dir not found: $CORPUS" >&2; exit 2; }
if port_busy; then echo "ERROR port ${PORT} already in use; refusing to touch it" >&2; exit 2; fi

EXPECTED_DOCS=$(find "$CORPUS" -type f \( -name '*.md' -o -name '*.html' \) | wc -l | tr -d ' ')
CORPUS_BYTES=$(find "$CORPUS" -type f \( -name '*.md' -o -name '*.html' \) -printf '%s\n' | awk '{s+=$1} END{print s+0}')
log "corpus=${CORPUS} files=${EXPECTED_DOCS} bytes=${CORPUS_BYTES} port=${PORT} reps=${REPS} queries=${NQUERIES}"

# ---- clean state ---------------------------------------------------------
rm -rf "$SANDBOX"
mkdir -p "$SANDBOX/.config/kb" "$SANDBOX/.local/share" "$SANDBOX/.cache" "$SANDBOX/state" "$SAMPLES"
cat > "$SANDBOX/.config/kb/kb.toml" <<EOF
[daemon]
name = "bench-rust"

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

# ---- start daemon, measure startup-to-ready ------------------------------
# "ready" == the daemon answers GET /api/kb/<kb>/stats. This is deliberately
# NOT "the embedder has finished loading" nor "the index is populated".
# Sets the globals DAEMON_PID and READY_S. Deliberately NOT a command
# substitution: in a subshell the daemon would be reparented away from this
# shell's cleanup trap and leak, still holding the port.
start_daemon() {
  local t0 ready
  t0=$(date +%s.%N)
  kbenv "$KB_BIN" daemon --config "$SANDBOX/.config/kb/kb.toml" >"$LOG" 2>&1 &
  DAEMON_PID=$!
  ready=""
  for _ in $(seq 1 900); do
    if curl -fsS -m 2 "$URL/api/kb/${KB_NAME}/stats" >/dev/null 2>&1; then ready=$(date +%s.%N); break; fi
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
      echo "ERROR daemon exited during startup; log tail:" >&2; tail -20 "$LOG" >&2; exit 3
    fi
    sleep 0.2
  done
  [ -n "$ready" ] || { echo "ERROR daemon never became ready; log tail:" >&2; tail -20 "$LOG" >&2; exit 3; }
  READY_S=$(awk -v a="$t0" -v b="$ready" 'BEGIN{printf "%.3f", b-a}')
  READY_PID=$(ss -ltnp 2>/dev/null | grep ":${PORT} " | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  log "ready after ${READY_S}s: spawned_pid=${DAEMON_PID:-none} port_owner_pid=${READY_PID:-unknown}"
  if [ -n "${PREV_PID:-}" ] && [ "$READY_PID" = "$PREV_PID" ]; then
    log "WARNING: the responder on ${PORT} is the PREVIOUS daemon (${PREV_PID}); the restart did not take effect"
    READY_S=invalid_stale_responder
  fi
}

start_daemon
COLD_READY=$READY_S
log "cold startup-to-ready: ${COLD_READY}s"

# ---- ingest to completion ------------------------------------------------
T_INGEST_START=$(date +%s.%N)
LAST_DOCS=-1
STUCK=0
for _ in $(seq 1 4000); do
  S=$(curl -fsS -m 5 "$URL/api/kb/${KB_NAME}/stats" 2>/dev/null) || { sleep 2; continue; }
  DOCS=$(printf '%s' "$S" | sed -n 's/.*"doc_count":\([0-9]*\).*/\1/p')
  [ -z "$DOCS" ] && { sleep 2; continue; }
  if [ "$DOCS" = "$EXPECTED_DOCS" ]; then break; fi
  if [ "$DOCS" = "$LAST_DOCS" ]; then STUCK=$((STUCK+1)); else STUCK=0; fi
  # 150 consecutive 2 s polls (5 min) with no growth => the walk is finished
  [ "$STUCK" -ge 150 ] && { log "indexing stalled at ${DOCS}/${EXPECTED_DOCS}"; break; }
  LAST_DOCS=$DOCS
  if [ $((RANDOM % 40)) -eq 0 ]; then log "indexing: ${DOCS}/${EXPECTED_DOCS}"; fi
  sleep 2
done
T_INGEST_END=$(date +%s.%N)
STATS=$(curl -fsS -m 5 "$URL/api/kb/${KB_NAME}/stats")
DOC_COUNT=$(printf '%s' "$STATS" | sed -n 's/.*"doc_count":\([0-9]*\).*/\1/p')
OPEN_ERRORS=$(printf '%s' "$STATS" | sed -n 's/.*"open_errors":\([0-9]*\).*/\1/p')
INGEST_S=$(awk -v a="$T_INGEST_START" -v b="$T_INGEST_END" 'BEGIN{printf "%.3f", b-a}')
log "ingest: ${DOC_COUNT} docs (expected ${EXPECTED_DOCS}) in ${INGEST_S}s, open_errors=${OPEN_ERRORS}"

# ---- idle RSS (daemon + embedder helper) ---------------------------------
sleep 3
rss_kb_of() { awk '/^VmRSS:/ {print $2}' "/proc/$1/status" 2>/dev/null; }
# Every process descended from OUR daemon pid, however deep (the embedder
# helper may be reparented). Scoped by ancestry, never by name.
descendants_of() {
  local frontier="$1" out="" pid pp
  for _ in 1 2 3 4 5; do
    local next=""
    for pid in $frontier; do
      for pp in $(pgrep -P "$pid" 2>/dev/null); do
        case " $out $next " in *" $pp "*) continue ;; esac
        out="$out $pp"; next="$next $pp"
      done
    done
    [ -z "$next" ] && break
    frontier="$next"
  done
  echo $out
}

# Sample for up to 15 s and keep the max: the embedder helper is spawned
# lazily, so a single early sample can miss it.
probe_rss() {
  DAEMON_RSS_KB=0; EMBEDDER_RSS_KB=0
  # Prefer the pid we spawned; if /proc has no such pid, fall back to whoever
  # is holding OUR port (unambiguous: the port was verified free before start).
  local pid="$DAEMON_PID"
  if [ -z "$pid" ] || [ ! -r "/proc/$pid/status" ]; then
    pid=$(ss -ltnp 2>/dev/null | grep ":${PORT} " | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
    log "probe_rss: falling back to the port owner pid=${pid:-none}"
  fi
  [ -n "$pid" ] && DAEMON_PID="$pid"
  for _ in $(seq 1 15); do
    for p in $(descendants_of "$pid"); do
      r=$(rss_kb_of "$p"); [ -n "$r" ] || continue
      case "$(cat "/proc/$p/comm" 2>/dev/null)" in
        kb-embedder*) [ "$r" -gt "$EMBEDDER_RSS_KB" ] && EMBEDDER_RSS_KB=$r ;;
        kb*)          [ "$r" -gt "$DAEMON_RSS_KB" ] && DAEMON_RSS_KB=$r ;;
      esac
    done
    r=$(rss_kb_of "$pid"); [ -n "$r" ] && [ "$r" -gt "$DAEMON_RSS_KB" ] && DAEMON_RSS_KB=$r
    sleep 1
  done
  TOTAL_RSS_KB=$((DAEMON_RSS_KB + EMBEDDER_RSS_KB))
}

probe_rss
log "idle RSS (after ingest): daemon=${DAEMON_RSS_KB}kB embedder=${EMBEDDER_RSS_KB}kB total=${TOTAL_RSS_KB}kB"

# ---- warm restart: startup-to-ready with index on disk + model cached ----
log "stopping daemon for the warm-restart measurement"
OLD_PID=${READY_PID:-$DAEMON_PID}
collect_embedders
kill -TERM "$OLD_PID" 2>/dev/null
# SIGTERM frees the listener at once; the process then finishes the index
# shutdown, which takes tens of seconds. Wait for the PROCESS to be gone —
# a restarted daemon that cannot bind would leave the old one answering, and
# the "warm startup" number would silently be the old daemon's.
for _ in $(seq 1 120); do kill -0 "$OLD_PID" 2>/dev/null || break; sleep 0.5; done
if kill -0 "$OLD_PID" 2>/dev/null; then log "old daemon ${OLD_PID} survived SIGTERM 60 s; SIGKILL"; kill -KILL "$OLD_PID" 2>/dev/null; sleep 2; fi
for _ in $(seq 1 60); do port_busy || break; sleep 0.5; done
if port_busy; then log "ERROR port ${PORT} still busy after killing ${OLD_PID}; cannot measure a warm restart"; exit 4; fi
log "old daemon ${OLD_PID} gone, port ${PORT} free"
PREV_PID=$OLD_PID
DAEMON_PID=""
start_daemon
WARM_READY=$READY_S
DOC_COUNT_WARM=$(curl -fsS -m 5 "$URL/api/kb/${KB_NAME}/stats" | sed -n 's/.*"doc_count":\([0-9]*\).*/\1/p')
log "warm startup-to-ready: ${WARM_READY}s (doc_count ${DOC_COUNT_WARM})"

# ---- latency measurement -------------------------------------------------
# One sample = one complete HTTP round trip: TCP connect, request written,
# full JSON response body read by curl. curl -w time_total, in seconds.
http_lat() { # $1=query $2=mode -> prints ms
  local q; q=${1// /%20}
  curl -fsS -o /dev/null -m 30 -w '%{time_total}' \
    "$URL/api/search?q=${q}&mode=${2}&kb=${KB_NAME}&limit=20" 2>/dev/null | awk '{printf "%.3f", $1*1000}'
}

# In-process: `kb search --offline` opens the lance dataset directly and
# answers without the daemon. NOTE it still pays CLI process startup.
inproc_lat() { # $1=query -> prints ms
  local s e
  s=$(date +%s.%N)
  kbenv "$KB_BIN" search --offline --kb "$KB_NAME" --mode keyword --limit 20 --json "$1" >/dev/null 2>&1
  e=$(date +%s.%N)
  awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f\n", (b-a)*1000}'
}

pct() { # $1=file of one-sample-per-line ms, $2=percentile
  sort -g "$1" | awk -v p="$2" '{a[NR]=$1} END {
    if (NR==0) {print "NaN"; exit}
    i=int((p/100)*NR + 0.9999); if (i<1) i=1; if (i>NR) i=NR; printf "%.3f", a[i]}'
}

# probe the hybrid (vector) lane once; only claim it if it really answers
HYBRID_NOTE="hybrid_unavailable_mirrors_keyword"
MODES="keyword"
if [ -n "$(http_lat daemon hybrid)" ]; then MODES="keyword hybrid"; HYBRID_NOTE="hybrid_measured_with_bge-small-en-v1.5"; fi
log "modes measured over HTTP: $MODES"

# warm-up: one unrecorded request per query per mode
for mode in $MODES; do
  i=0
  while [ "$i" -lt "$NQUERIES" ]; do
    http_lat "${QUERIES[$i]}" "$mode" >/dev/null
    i=$((i+1))
  done
done

for mode in $MODES; do
  : > "$SAMPLES/http-${mode}.tsv"
  i=0
  while [ "$i" -lt "$NQUERIES" ]; do
    q="${QUERIES[$i]}"
    : > "$SAMPLES/http-${mode}-q${i}.tsv"
    r=0
    while [ "$r" -lt "$REPS" ]; do
      # One line per rep into BOTH the per-query file and the mode aggregate.
      # (Re-copying the per-query file per rep would weight late reps higher.)
      http_lat "$q" "$mode" | sed 's/$/\n/' | tee -a "$SAMPLES/http-${mode}-q${i}.tsv" >> "$SAMPLES/http-${mode}.tsv"
      r=$((r+1))
    done
    i=$((i+1))
    log "measured http mode=${mode} q='${q}'"
  done
done
if [ "$MODES" != "keyword hybrid" ]; then
  cp "$SAMPLES/http-keyword.tsv" "$SAMPLES/http-hybrid.tsv"
  i=0
  while [ "$i" -lt "$NQUERIES" ]; do cp "$SAMPLES/http-keyword-q${i}.tsv" "$SAMPLES/http-hybrid-q${i}.tsv"; i=$((i+1)); done
fi

: > "$SAMPLES/inproc-keyword.tsv"
i=0
while [ "$i" -lt "$NQUERIES" ]; do
  q="${QUERIES[$i]}"
  : > "$SAMPLES/inproc-keyword-q${i}.tsv"
  r=0
  while [ "$r" -lt "$REPS" ]; do
    inproc_lat "$q" | tee -a "$SAMPLES/inproc-keyword-q${i}.tsv" >> "$SAMPLES/inproc-keyword.tsv"
    r=$((r+1))
  done
  i=$((i+1))
  log "measured in-process q='${q}'"
done

# The embedder helper is only resident once a vector-lane query has actually
# run, so re-probe here and report both points.
probe_rss
RSS_D2=$DAEMON_RSS_KB; RSS_E2=$EMBEDDER_RSS_KB; RSS_T2=$TOTAL_RSS_KB
log "RSS (after queries, hybrid lane exercised): daemon=${RSS_D2}kB embedder=${RSS_E2}kB total=${RSS_T2}kB"

emit() { # $1=label $2=file
  echo "lat_${1}_n=$(wc -l < "$2" | tr -d ' ')"
  echo "lat_${1}_min_ms=$(pct "$2" 0)"
  echo "lat_${1}_p50_ms=$(pct "$2" 50)"
  echo "lat_${1}_p95_ms=$(pct "$2" 95)"
  echo "lat_${1}_p99_ms=$(pct "$2" 99)"
  echo "lat_${1}_mean_ms=$(awk '{s+=$1} END{if(NR) printf "%.3f", s/NR; else print "NaN"}' "$2")"
}

{
  echo "# rust-kb baseline"
  echo "kb_version=$(kbenv "$KB_BIN" --version 2>/dev/null)"
  echo "kb_binary=$KB_BIN"
  echo "config=$SANDBOX/.config/kb/kb.toml"
  echo "corpus_path=$CORPUS"
  echo "corpus_files=$EXPECTED_DOCS"
  echo "corpus_bytes=$CORPUS_BYTES"
  echo "port=$PORT"
  echo "reps_per_query=$REPS"
  echo "queries=$(printf '%s | ' "${QUERIES[@]}")"
  echo "startup_ready_cold_s=$COLD_READY"
  echo "startup_ready_warm_s=$WARM_READY"
  echo "ingest_wall_s=$INGEST_S"
  echo "doc_count=$DOC_COUNT"
  echo "doc_count_warm_restart=$DOC_COUNT_WARM"
  echo "open_errors=$OPEN_ERRORS"
  echo "rss_daemon_kb=$DAEMON_RSS_KB"
  echo "rss_embedder_kb=$EMBEDDER_RSS_KB"
  echo "rss_total_kb=$TOTAL_RSS_KB"
  echo "rss_after_queries_daemon_kb=$RSS_D2"
  echo "rss_after_queries_embedder_kb=$RSS_E2"
  echo "rss_after_queries_total_kb=$RSS_T2"
  echo "http_measure_scope=full_http_roundtrip_tcp_plus_json_body"
  echo "inproc_measure_scope=kb_search_offline_includes_cli_process_startup"
  echo "vector_lane=$HYBRID_NOTE"
  emit http_keyword "$SAMPLES/http-keyword.tsv"
  emit http_hybrid  "$SAMPLES/http-hybrid.tsv"
  emit inprocess_keyword "$SAMPLES/inproc-keyword.tsv"
  i=0
  while [ "$i" -lt "$NQUERIES" ]; do
    slug=$(printf '%s' "${QUERIES[$i]}" | tr ' ' '_')
    echo "pq_http_keyword_${slug}_p50_ms=$(pct "$SAMPLES/http-keyword-q${i}.tsv" 50)"
    echo "pq_http_hybrid_${slug}_p50_ms=$(pct "$SAMPLES/http-hybrid-q${i}.tsv" 50)"
    echo "pq_inprocess_keyword_${slug}_p50_ms=$(pct "$SAMPLES/inproc-keyword-q${i}.tsv" 50)"
    i=$((i+1))
  done
} > "$SANDBOX/results.env"
cat "$SANDBOX/results.env"
log "results: $SANDBOX/results.env ; raw samples: $SAMPLES/*.tsv"
