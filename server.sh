#!/usr/bin/env bash
#
# Manually start/stop the light-control server node.
# This is NOT a system service — it does not start on boot. Run it yourself:
#
#   ./server.sh start      # start in the background
#   ./server.sh stop       # stop it
#   ./server.sh restart
#   ./server.sh status
#   ./server.sh logs       # tail the log
#
# Config via env vars: PORT (default 8080), NODE_NAME (default mac-server),
# PEER_TTL_MS. Example:  PORT=8090 NODE_NAME=lab ./server.sh start
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$ROOT/server"
PID_FILE="$SERVER_DIR/.server.pid"
LOG_FILE="$SERVER_DIR/.server.log"

PORT="${PORT:-8080}"
NODE_NAME="${NODE_NAME:-mac-server}"

is_running() {
  [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

# Kill a process and all of its descendants (tsx spawns a child node process).
kill_tree() {
  local pid="$1"
  local child
  for child in $(pgrep -P "$pid" 2>/dev/null || true); do
    kill_tree "$child"
  done
  kill "$pid" 2>/dev/null || true
}

start() {
  if is_running; then
    echo "already running (pid $(cat "$PID_FILE")) on port $PORT"
    return 0
  fi
  if [[ ! -x "$SERVER_DIR/node_modules/.bin/tsx" ]]; then
    echo "installing server dependencies..."
    (cd "$SERVER_DIR" && npm install)
  fi
  echo "starting server (name=$NODE_NAME, port=$PORT)..."
  (
    cd "$SERVER_DIR"
    NODE_NAME="$NODE_NAME" PORT="$PORT" PEER_TTL_MS="${PEER_TTL_MS:-}" \
      nohup node_modules/.bin/tsx src/index.ts >"$LOG_FILE" 2>&1 &
    echo $! >"$PID_FILE"
  )
  sleep 1
  if is_running; then
    echo "started (pid $(cat "$PID_FILE"))  ->  http://localhost:$PORT"
    echo "logs: $LOG_FILE  (./server.sh logs)"
  else
    echo "failed to start; last log lines:" >&2
    tail -n 20 "$LOG_FILE" >&2 || true
    rm -f "$PID_FILE"
    exit 1
  fi
}

stop() {
  if ! is_running; then
    echo "not running"
    rm -f "$PID_FILE"
    return 0
  fi
  local pid
  pid="$(cat "$PID_FILE")"
  echo "stopping (pid $pid)..."
  kill_tree "$pid"
  for _ in $(seq 1 25); do
    is_running || break
    sleep 0.2
  done
  rm -f "$PID_FILE"
  echo "stopped"
}

status() {
  if is_running; then
    echo "running (pid $(cat "$PID_FILE")) on port $PORT"
  else
    echo "not running"
  fi
}

case "${1:-}" in
  start)   start ;;
  stop)    stop ;;
  restart) stop; start ;;
  status)  status ;;
  logs)    tail -f "$LOG_FILE" ;;
  *)
    echo "usage: $0 {start|stop|restart|status|logs}" >&2
    exit 2
    ;;
esac
