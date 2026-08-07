#!/usr/bin/env bash
set -euo pipefail

mkdir -p /data
cd /data

core_pid=""
ws_pid=""
fix_pid=""

shutdown() {
    trap - INT TERM EXIT

    for pid in "$fix_pid" "$ws_pid" "$core_pid"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    for pid in "$fix_pid" "$ws_pid" "$core_pid"; do
        if [[ -n "$pid" ]]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
}

trap shutdown INT TERM EXIT

/app/bin/exchange_lab &
core_pid=$!

/app/bin/exchange_websocket_gateway &
ws_pid=$!

/app/bin/exchange_fix_gateway &
fix_pid=$!

# If any backend process exits, stop the other two so Docker can restart
# the service as one coherent ExchangeLab backend unit.
set +e
wait -n "$core_pid" "$ws_pid" "$fix_pid"
status=$?
set -e

shutdown
exit "$status"
