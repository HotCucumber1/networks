#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
make -s
WORK="$(mktemp -d)"
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  rm -rf "$WORK"
}
trap cleanup EXIT

python3 - "$WORK/input.bin" <<'PY'
import os, sys
with open(sys.argv[1], "wb") as f:
    f.write(os.urandom(250000))
PY

run_case() {
  local port="$1" alg="$2" loss="$3" receiver_extra="$4" sender_extra="$5" output="$6"
  ./rdt_receiver "$port" "$WORK/$output" -alg "$alg" $receiver_extra >"$WORK/receiver.log" 2>&1 &
  local receiver_pid=$!
  sleep 0.2
  ./rdt_sender 127.0.0.1 "$port" "$WORK/input.bin" -alg "$alg" -loss "$loss" -window 24 -timeout 250 $sender_extra >"$WORK/sender.log" 2>&1
  wait "$receiver_pid"
  cmp "$WORK/input.bin" "$WORK/$output"
  echo "PASS: $alg, sender loss=$loss, receiver options='$receiver_extra'"
}

run_case 19101 SR 0 "" "" sr.bin
run_case 19102 GBN 0 "" "" gbn.bin
run_case 19103 SR 0.30 "" "" sr_loss30.bin

: > "$WORK/empty.bin"
./rdt_receiver 19104 "$WORK/empty_received.bin" -alg SR >"$WORK/receiver_empty.log" 2>&1 &
receiver_pid=$!
sleep 0.2
./rdt_sender 127.0.0.1 19104 "$WORK/empty.bin" -alg SR
wait "$receiver_pid"
cmp "$WORK/empty.bin" "$WORK/empty_received.bin"
echo "PASS: empty file"

echo "All automated tests passed."
