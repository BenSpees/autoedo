#!/usr/bin/env bash
# AutoEDO Live launcher: rebuild everything, restart the service, open the UI.
#
#   ./run.sh              build + (re)launch + open browser
#   ./run.sh --stop       stop a running instance
#   AUTOEDO_PORT=9000 ./run.sh    use a different port
set -euo pipefail
cd "$(dirname "$0")"

PORT="${AUTOEDO_PORT:-8017}"
PIDFILE="build/autoedo.pid"
LOGFILE="build/autoedo.log"
URL="http://127.0.0.1:${PORT}/"

stop_existing() {
  if [[ -f "$PIDFILE" ]]; then
    local pid
    pid="$(cat "$PIDFILE")"
    if kill -0 "$pid" 2>/dev/null; then
      echo "Stopping running instance (pid $pid)..."
      kill "$pid" 2>/dev/null || true
      for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PIDFILE"
  fi
  # Belt and braces: anything else serving from our build dir, and whatever
  # is still holding our port (e.g. an instance launched by hand).
  pkill -f "$(pwd)/build/autoedo" 2>/dev/null || true
  if command -v lsof >/dev/null 2>&1; then
    local holders
    holders="$(lsof -ti tcp:"$PORT" 2>/dev/null || true)"
    if [[ -n "$holders" ]]; then
      echo "Stopping process(es) on port $PORT: $holders"
      kill $holders 2>/dev/null || true
      sleep 0.3
      kill -9 $holders 2>/dev/null || true
    fi
  fi
}

if [[ "${1:-}" == "--stop" ]]; then
  stop_existing
  echo "Stopped."
  exit 0
fi

echo "== Building =="
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
make -j"$JOBS"

echo "== Restarting service =="
stop_existing
nohup ./build/autoedo --port "$PORT" > "$LOGFILE" 2>&1 &
echo $! > "$PIDFILE"
echo "Started pid $(cat "$PIDFILE") (log: $LOGFILE)"

# Wait for the web UI to answer (and make sure it's *our* process serving).
ok=""
for _ in $(seq 1 50); do
  if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then break; fi
  if curl -sf -o /dev/null "${URL}api/status"; then ok=1; break; fi
  sleep 0.2
done

if [[ -z "$ok" ]]; then
  echo "ERROR: service did not come up. Last log lines:" >&2
  tail -n 20 "$LOGFILE" >&2 || true
  exit 1
fi

echo "== AutoEDO Live is up: $URL =="
if command -v open >/dev/null 2>&1; then
  open "$URL"
else
  echo "Open $URL in your browser."
fi
echo "Logs: tail -f $LOGFILE   ·   Stop: ./run.sh --stop"
