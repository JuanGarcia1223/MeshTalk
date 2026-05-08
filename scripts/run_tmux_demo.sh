#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_BIN="${APP_BIN:-$ROOT_DIR/build/meshtalk_demo}"
SESSION_NAME="${1:-meshtalk-demo}"
TMUX_COLS="$(tput cols 2>/dev/null || echo 120)"
TMUX_ROWS="$(tput lines 2>/dev/null || echo 30)"

if ! command -v tmux >/dev/null 2>&1; then
  echo "error: tmux is not installed" >&2
  exit 1
fi

if [[ ! -x "$APP_BIN" ]]; then
  echo "error: binary not found or not executable: $APP_BIN" >&2
  echo "build first: cmake --build build -j\$(nproc)" >&2
  exit 1
fi

if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "session '$SESSION_NAME' already exists, attaching..."
  if [[ -n "${TMUX:-}" ]]; then
    exec tmux switch-client -t "$SESSION_NAME"
  else
    exec tmux attach -t "$SESSION_NAME"
  fi
fi

start_window() {
  local win_name="$1"
  local user_name="$2"
  tmux new-window -t "$SESSION_NAME" -n "$win_name" \
    "cd '$ROOT_DIR' && '$APP_BIN' --name '$user_name'"
}

# Create first window via new-session, then add two more.
tmux new-session -d -s "$SESSION_NAME" -n "alice" \
  -x "$TMUX_COLS" -y "$TMUX_ROWS" \
  "cd '$ROOT_DIR' && '$APP_BIN' --name 'alice'"
start_window "bob" "bob"
start_window "charlie" "charlie"

# Focus first window.
tmux select-window -t "$SESSION_NAME":0

echo "Started tmux session: $SESSION_NAME"
echo "Stop all peers: tmux kill-session -t $SESSION_NAME"

if [[ -n "${TMUX:-}" ]]; then
  exec tmux switch-client -t "$SESSION_NAME"
else
  exec tmux attach -t "$SESSION_NAME"
fi
