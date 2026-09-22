#!/usr/bin/env bash
# Concurrent typed-layout x RLE replay using frozen numeric/UUID payloads.

set -u

PAYLOAD="${PAYLOAD:?set PAYLOAD to the frozen numeric command file}"
PAYLOAD_UUID="${PAYLOAD_UUID:-}"
OUTDIR="${OUTDIR:?set OUTDIR to a new, non-existent result directory}"
GOBLIN="${GOBLIN:?set GOBLIN to the frozen RLE-enabled goblin-core binary}"
PACKED_MERGE_EXPONENT="${PACKED_MERGE_EXPONENT:-0.5}"
REDIS_CLI="${REDIS_CLI:-$HOME/bench/redis-8.8.0/src/redis-cli}"
HEAD_LINES="${HEAD_LINES:-all}"
EXPECTED_FULL_COMMANDS="${EXPECTED_FULL_COMMANDS:-1483700913}"
SAMPLE_SECONDS="${SAMPLE_SECONDS:-300}"
HASH_INPUT="${HASH_INPUT:-1}"
CMP_KEY="${CMP_KEY:-key}"
VERIFY="${VERIFY:-1}"
DIGEST_PAGE_SIZE="${DIGEST_PAGE_SIZE:-0}"
default_servers=()
for member in int32 int64 uuid; do
  for score in float32 float64; do
    for rle in off on; do
      default_servers+=("goblin-packed-$member-$score-rle-$rle")
    done
  done
done
read -ra SERVERS <<< "${SERVERS:-${default_servers[*]}}"
for name in "${SERVERS[@]}"; do
  if ! [[ "$name" =~ ^goblin-packed-(int32|int64|uuid)-(float32|float64)-rle-(off|on)$ ]]; then
    echo "invalid matrix variant: $name" >&2
    exit 1
  fi
  if [[ "$name" == goblin-packed-uuid-* ]] && [ ! -f "$PAYLOAD_UUID" ]; then
    echo "UUID variants require PAYLOAD_UUID" >&2
    exit 1
  fi
done

if [ -e "$OUTDIR" ]; then
  echo "refusing to overwrite existing output directory: $OUTDIR" >&2
  exit 1
fi
if [ ! -f "$PAYLOAD" ]; then
  echo "payload not found: $PAYLOAD" >&2
  exit 1
fi
for executable in "$GOBLIN" "$REDIS_CLI"; do
  if [ ! -x "$executable" ]; then
    echo "executable not found: $executable" >&2
    exit 1
  fi
done
if [ "$HEAD_LINES" != all ] && ! [[ "$HEAD_LINES" =~ ^[1-9][0-9]*$ ]]; then
  echo "HEAD_LINES must be 'all' or a positive integer" >&2
  exit 1
fi
if ! [[ "$DIGEST_PAGE_SIZE" =~ ^(0|[1-9][0-9]*)$ ]] || \
   [ "${#DIGEST_PAGE_SIZE}" -gt 7 ] || [ "$DIGEST_PAGE_SIZE" -gt 1000000 ]; then
  echo "DIGEST_PAGE_SIZE must be 0 (one reply) or between 1 and 1000000" >&2
  exit 1
fi

mkdir -p "$OUTDIR/results" "$OUTDIR/logs" "$OUTDIR/samples" \
         "$OUTDIR/state" "$OUTDIR/digests"
touch "$OUTDIR/RUNNING"
RUN_TOKEN="wiki-$PPID-$$-$(date +%s)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cp "$0" "$OUTDIR/run.sh"
cp "$SCRIPT_DIR/zset_digest.py" "$SCRIPT_DIR/compare_digests.py" "$OUTDIR/"
# Goblin also materializes a loopback TCP listener. Reserve distinct unused
# ports as a group before startup; clients still use only the Unix sockets.
python3 - "$OUTDIR/tcp-ports.tsv" "${SERVERS[@]}" <<'PYPORTS'
import socket
import sys
sockets = []
try:
    with open(sys.argv[1], "x") as out:
        for name in sys.argv[2:]:
            sock = socket.socket()
            sockets.append(sock)
            sock.bind(("127.0.0.1", 0))
            out.write(f"{name}\t{sock.getsockname()[1]}\n")
finally:
    for sock in sockets:
        sock.close()
PYPORTS
[ "$?" = 0 ] || exit 1

launch_cmd() {
  local name="$1" sock="$2" implementation rle_flag port
  [[ "$name" =~ ^goblin-packed-(int32|int64|uuid)-(float32|float64)-rle-(off|on)$ ]] || return 1
  implementation="packed-${BASH_REMATCH[1]}-${BASH_REMATCH[2]}"
  rle_flag=--no-packed-zset-score-rle
  [ "${BASH_REMATCH[3]}" = on ] && rle_flag=--packed-zset-score-rle
  port=$(awk -v name="$name" '$1==name {print $2}' "$OUTDIR/tcp-ports.tsv")
  CMD=("$GOBLIN" --uds-listen "$sock" --port "$port" --zset-implementation "$implementation"
       --packed-zset-merge-exponent "$PACKED_MERGE_EXPONENT" "$rle_flag")
}

socket_for() {
  printf '/tmp/%s-%s.sock' "$RUN_TOKEN" "$1"
}

# Verification is untimed. Paging bounds the server reply and client buffering
# for full datasets; the verifier receives the same ordered member/score stream.
# Feeds have all finished before this runs, so ranks are stable across pages.
stream_digest() {
  local sock="$1" count start stop
  if [ "$DIGEST_PAGE_SIZE" = 0 ]; then
    "$REDIS_CLI" --raw -s "$sock" ZRANGE "$CMP_KEY" 0 -1 WITHSCORES
    return "$?"
  fi
  count=$("$REDIS_CLI" --raw -s "$sock" ZCARD "$CMP_KEY") || return "$?"
  if ! [[ "$count" =~ ^[0-9]+$ ]]; then
    echo "invalid ZCARD before paged verification: $count" >&2
    return 1
  fi
  for ((start=0; start<count; start+=DIGEST_PAGE_SIZE)); do
    stop=$((start + DIGEST_PAGE_SIZE - 1))
    "$REDIS_CLI" --raw -s "$sock" ZRANGE "$CMP_KEY" "$start" "$stop" WITHSCORES \
      || return "$?"
  done
}

sample_one() {
  local name="$1" pid="$2" sock="$3" started="$4"
  local now elapsed rss info used card
  now=$(date +%s)
  elapsed=$((now - started))
  rss=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
  info=$("$REDIS_CLI" --raw -s "$sock" INFO memory 2>/dev/null || true)
  used=$(printf '%s\n' "$info" | awk -F: '$1=="used_memory_rss" {gsub(/\r/,"",$2); print $2; exit}')
  card=$("$REDIS_CLI" --raw -s "$sock" ZCARD "$CMP_KEY" 2>/dev/null || true)
  printf '%s\t%s\t%s\t%s\t%s\n' "$elapsed" "${rss:-0}" \
    "${used:-0}" "${card:-0}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    >> "$OUTDIR/samples/$name.tsv"
}

run_one() {
  local name="$1" payload="$PAYLOAD" expected_rle actual_rle
  [[ "$name" == goblin-packed-uuid-* ]] && payload="$PAYLOAD_UUID"
  local sock log pid ready attempt started sampler sample_stop
  local start_ns end_ns elapsed rss_kb rss_mb info used card error_count status
  local feed_status
  local -a feed_pipeline_status

  sock=$(socket_for "$name")
  log="$OUTDIR/logs/$name.server.log"
  printf '%s\n' "$sock" > "$OUTDIR/state/$name.socket"
  printf '%s\n' "$payload" > "$OUTDIR/state/$name.payload"
  rm -f -- "$sock"
  if ! launch_cmd "$name" "$sock"; then
    printf '%s\tFAILED\t0\t0\t0\t0\t0\tunknown-server\n' "$name" \
      > "$OUTDIR/results/$name.tsv"
    return
  fi
  printf '%q ' "${CMD[@]}" > "$OUTDIR/state/$name.command"
  printf '\n' >> "$OUTDIR/state/$name.command"
  "${CMD[@]}" > "$log" 2>&1 &
  pid=$!
  printf '%s\n' "$pid" > "$OUTDIR/state/$name.server.pid"

  ready=0
  for attempt in $(seq 1 300); do
    if [ -S "$sock" ] && [ "$("$REDIS_CLI" --raw -s "$sock" PING 2>/dev/null)" = PONG ]; then
      ready=1
      break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if [ "$ready" != 1 ]; then
    printf '%s\tFAILED\t0\t0\t0\t0\t0\tstart-failed\n' "$name" \
      > "$OUTDIR/results/$name.tsv"
    return
  fi

  printf 'elapsed_seconds\tos_rss_kb\tused_memory_rss_bytes\tzcard\tutc\n' \
    > "$OUTDIR/samples/$name.tsv"
  started=$(date +%s)
  sample_stop="$OUTDIR/state/$name.sample.stop"
  sample_one "$name" "$pid" "$sock" "$started"
  (
    remaining="$SAMPLE_SECONDS"
    while [ ! -e "$sample_stop" ] && kill -0 "$pid" 2>/dev/null; do
      sleep 1
      remaining=$((remaining - 1))
      if [ "$remaining" -le 0 ]; then
        [ -e "$sample_stop" ] || sample_one "$name" "$pid" "$sock" "$started"
        remaining="$SAMPLE_SECONDS"
      fi
    done
  ) &
  sampler=$!
  printf '%s\n' "$sampler" > "$OUTDIR/state/$name.sampler.pid"

  start_ns=$(date +%s%N)
  if [ "$HEAD_LINES" = all ]; then
    "$REDIS_CLI" --raw -s "$sock" < "$payload" \
      2> "$OUTDIR/logs/$name.client.stderr" |
      LC_ALL=C grep -aE '^(ERR|WRONGTYPE|NOAUTH|BUSY|OOM|MISCONF|Error|Could not connect)' \
        > "$OUTDIR/logs/$name.command-errors"
    feed_pipeline_status=("${PIPESTATUS[@]}")
    feed_status="${feed_pipeline_status[0]}"
  else
    head -n "$HEAD_LINES" "$payload" |
      "$REDIS_CLI" --raw -s "$sock" 2> "$OUTDIR/logs/$name.client.stderr" |
      LC_ALL=C grep -aE '^(ERR|WRONGTYPE|NOAUTH|BUSY|OOM|MISCONF|Error|Could not connect)' \
        > "$OUTDIR/logs/$name.command-errors"
    feed_pipeline_status=("${PIPESTATUS[@]}")
    feed_status="${feed_pipeline_status[1]}"
  fi
  end_ns=$(date +%s%N)

  touch "$sample_stop"
  kill "$sampler" 2>/dev/null || true
  wait "$sampler" 2>/dev/null || true
  sample_one "$name" "$pid" "$sock" "$started"

  elapsed=$(awk "BEGIN {printf \"%.3f\", ($end_ns - $start_ns) / 1000000000}")
  rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
  rss_mb=$(awk "BEGIN {printf \"%.1f\", ${rss_kb:-0} / 1024}")
  info=$("$REDIS_CLI" --raw -s "$sock" INFO memory 2>/dev/null || true)
  printf '%s\n' "$info" > "$OUTDIR/results/$name.info-memory.txt"
  used=$(printf '%s\n' "$info" | awk -F: '$1=="used_memory_rss" {gsub(/\r/,"",$2); print $2; exit}')
  card=$("$REDIS_CLI" --raw -s "$sock" ZCARD "$CMP_KEY" 2>/dev/null || true)
  error_count=$(wc -l < "$OUTDIR/logs/$name.command-errors" | tr -d ' ')
  "$REDIS_CLI" --raw -s "$sock" GOBLIN.MEMORY "$CMP_KEY" \
    > "$OUTDIR/results/$name.goblin-memory.txt" 2>&1 || true
  expected_rle=0
  [[ "$name" == *-rle-on ]] && expected_rle=1
  actual_rle=$(awk '$0=="score_rle" {getline; print; exit}' "$OUTDIR/results/$name.goblin-memory.txt")
  status=PASS
  if [ "$actual_rle" != "$expected_rle" ] || [ "$feed_status" != 0 ] || [ "$error_count" != 0 ] || [ -s "$OUTDIR/logs/$name.client.stderr" ]; then
    status=FAILED
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$status" \
    "$elapsed" "$rss_mb" "${rss_kb:-0}" "${used:-0}" "${card:-0}" \
    "$error_count" > "$OUTDIR/results/$name.tsv"
  touch "$OUTDIR/state/$name.feed-complete"
}

cleanup() {
  local pid_file pid socket_file sock
  for pid_file in "$OUTDIR"/state/*.sampler.pid "$OUTDIR"/state/*.server.pid; do
    [ -f "$pid_file" ] || continue
    pid=$(<"$pid_file")
    kill "$pid" 2>/dev/null || true
  done
  for pid_file in "$OUTDIR"/state/*.server.pid; do
    [ -f "$pid_file" ] || continue
    pid=$(<"$pid_file")
    wait "$pid" 2>/dev/null || true
  done
  for socket_file in "$OUTDIR"/state/*.socket; do
    [ -f "$socket_file" ] || continue
    sock=$(<"$socket_file")
    rm -f -- "$sock"
  done
}

on_signal() {
  echo "benchmark interrupted" >&2
  exit 130
}

trap cleanup EXIT
trap on_signal INT TERM

{
  echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "hostname=$(hostname)"
  echo "payload=$PAYLOAD"
  echo "payload_bytes=$(stat -c %s "$PAYLOAD")"
  echo "payload_uuid=$PAYLOAD_UUID"
  if [ -n "$PAYLOAD_UUID" ]; then echo "payload_uuid_bytes=$(stat -c %s "$PAYLOAD_UUID")"; fi
  echo "merge_exponent=$PACKED_MERGE_EXPONENT"
  echo "head_lines=$HEAD_LINES"
  echo "sample_seconds=$SAMPLE_SECONDS"
  echo "digest_page_size=$DIGEST_PAGE_SIZE"
  echo "cmp_key=$CMP_KEY"
  echo "servers=${SERVERS[*]}"
  echo "goblin=$GOBLIN"
  "$REDIS_CLI" --version
  uname -a
  lscpu
  free -b
} > "$OUTDIR/metadata.txt"
sha256sum "$GOBLIN" "$REDIS_CLI" "$0" "$SCRIPT_DIR/zset_digest.py" \
  "$SCRIPT_DIR/compare_digests.py" > "$OUTDIR/software.sha256"

if [ "$HEAD_LINES" = all ]; then
  expected_commands="$EXPECTED_FULL_COMMANDS"
  actual_commands=$(wc -l < "$PAYLOAD" | tr -d ' ')
  if [ "$actual_commands" != "$expected_commands" ]; then
    echo "payload has $actual_commands lines, expected $expected_commands" >&2
    exit 1
  fi
else
  expected_commands="$HEAD_LINES"
fi
echo "expected_commands=$expected_commands" >> "$OUTDIR/metadata.txt"
if [ "$HASH_INPUT" = 1 ]; then
  echo "hashing input before the timed concurrent feeds ..." >&2
  sha256sum "$PAYLOAD" > "$OUTDIR/input.sha256"
  if [ -n "$PAYLOAD_UUID" ]; then sha256sum "$PAYLOAD_UUID" >> "$OUTDIR/input.sha256"; fi
fi

echo "starting ${#SERVERS[@]} concurrent feeds at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >&2
jobs=()
for name in "${SERVERS[@]}"; do
  run_one "$name" &
  jobs+=("$!")
done
for job in "${jobs[@]}"; do
  wait "$job" || true
done

summary="$OUTDIR/summary.tsv"
feed_status_overall=0
printf 'server\tstatus\tfeed_seconds\tos_rss_mb\tos_rss_kb\tused_memory_rss_bytes\tzcard\tcommand_errors\n' \
  > "$summary"
for name in "${SERVERS[@]}"; do
  if [ -f "$OUTDIR/results/$name.tsv" ]; then
    cat "$OUTDIR/results/$name.tsv" >> "$summary"
    [ "$(cut -f2 "$OUTDIR/results/$name.tsv")" = PASS ] || feed_status_overall=1
  else
    printf '%s\tFAILED\t0\t0\t0\t0\t0\tmissing-result\n' "$name" >> "$summary"
    feed_status_overall=1
  fi
done
cat "$summary"

verification_status="$feed_status_overall"
if [ "$VERIFY" = 1 ]; then
  echo "starting concurrent final-state digests at $(date -u +%Y-%m-%dT%H:%M:%SZ)" >&2
  digest_jobs=()
  for name in "${SERVERS[@]}"; do
    sock=$(socket_for "$name")
    tie_order=numeric
    member_format=integer
    [[ "$name" == goblin-packed-uuid-* ]] && member_format=uuid-page-id
    (
      stream_digest "$sock" \
        2> "$OUTDIR/logs/$name.digest-client.stderr" |
        python3 "$SCRIPT_DIR/zset_digest.py" --tie-order "$tie_order" --member-format "$member_format" \
          > "$OUTDIR/digests/$name.json" \
          2> "$OUTDIR/logs/$name.digest.stderr"
      digest_pipeline_status=("${PIPESTATUS[@]}")
      printf '%s\t%s\n' "${digest_pipeline_status[0]}" \
        "${digest_pipeline_status[1]}" > "$OUTDIR/digests/$name.status"
    ) &
    digest_jobs+=("$!")
  done
  for job in "${digest_jobs[@]}"; do
    wait "$job" || verification_status=1
  done
  for name in "${SERVERS[@]}"; do
    read -r client_status digest_status < "$OUTDIR/digests/$name.status"
    if [ "$client_status" != 0 ] || [ "$digest_status" != 0 ] ||
       [ -s "$OUTDIR/logs/$name.digest-client.stderr" ] ||
       [ -s "$OUTDIR/logs/$name.digest.stderr" ]; then
      verification_status=1
    fi
  done
  baseline_args=()
  if [ -n "${REFERENCE_DIGEST:-}" ]; then
    cp "$REFERENCE_DIGEST" "$OUTDIR/reference-digest.json"
    baseline_args=(--baseline-digest "$OUTDIR/reference-digest.json")
  fi
  python3 "$SCRIPT_DIR/compare_digests.py" "$OUTDIR/digests" \
    "$expected_commands" "${SERVERS[@]}" "${baseline_args[@]}" | tee "$OUTDIR/verification.txt"
  compare_status="${PIPESTATUS[0]}"
  [ "$compare_status" = 0 ] || verification_status=1
fi

echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUTDIR/metadata.txt"
echo "verification_status=$verification_status" >> "$OUTDIR/metadata.txt"
mv "$OUTDIR/RUNNING" "$OUTDIR/COMPLETE"
exit "$verification_status"
