#!/bin/bash
# AutoEDO one-stop launcher. Double-click for a Terminal window; the Dock app
# (tools/AutoEDO.app) runs it with --headless (no window: notification banners
# and a dialog with Show Log on failure). One brain, two faces — see
# tools/AutoEDO.app/Contents/MacOS/launcher for the shim.
set -uo pipefail

# Finder/Dock-launched processes get a bare PATH (no login shell!), so tools
# installed by Homebrew "don't exist" unless you put them back yourself.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
REPO="$(cd "$(dirname "$SELF")/.." && pwd)"   # tools/ -> live/
cd "$REPO" || exit 1

# ═══ EDIT ME ═════════════════════════════════════════════════════════════════
APP_NAME="AutoEDO"        # dialog/notification title
PROC_NAMES=(autoedo)      # process names this launcher manages (per-port match)
PORT="${AUTOEDO_PORT:-8017}"  # localhost web UI port; health check and tab match
CONFIG_FILE="${AUTOEDO_CONFIG:-}" # per-instance settings file ("" = ~/.autoedo.json)
needs_build() {           # exit 0 if a build is required (else skip straight to run)
    ! make -q >/dev/null 2>&1
}
do_build() { make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"; }
run_cmd()  { echo "build/autoedo"; }               # binary to exec
run_args=(--port "$PORT")                          # its arguments
[ -n "$CONFIG_FILE" ] && run_args+=(--config "$CONFIG_FILE")
# ═════════════════════════════════════════════════════════════════════════════

HEADLESS=0
OPEN_UI=1
STOP_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --headless) HEADLESS=1 ;;
        --no-ui)    OPEN_UI=0 ;;
        --stop)     STOP_ONLY=1 ;;
    esac
    shift
done

LAUNCH_LOG="$REPO/logs/launcher.log"
RUN_LOG="$REPO/logs/run-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$REPO/logs"
# One run, one log: the dialog's "Show Log" opens this attempt, not history.
[ "$HEADLESS" = 1 ] && : > "$LAUNCH_LOG"

notify() {  # headless progress: a banner, not a window
    command -v osascript >/dev/null 2>&1 \
        && osascript -e "display notification \"$1\" with title \"$APP_NAME\"" \
               >/dev/null 2>&1
}

say() {
    if [ "$HEADLESS" = 1 ]; then
        printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LAUNCH_LOG"
    else
        printf '▍ %s\n' "$*"
    fi
}

die() {
    if [ "$HEADLESS" = 1 ]; then
        printf '%s FAILED: %s\n' "$(date +%H:%M:%S)" "$*" >> "$LAUNCH_LOG"
        if command -v osascript >/dev/null 2>&1; then
            BTN=$(osascript -e "display dialog \"$APP_NAME did not start:\n\n$*\" \
                  with title \"$APP_NAME\" buttons {\"Show Log\", \"OK\"} \
                  default button \"OK\" with icon caution" 2>/dev/null)
            case "$BTN" in *"Show Log"*) open "$LAUNCH_LOG" ;; esac
        fi
    else
        printf '▍ FAILED: %s\n' "$*"
    fi
    exit 1
}

# Focus an existing tab instead of opening a duplicate. Scriptable browsers
# only; anything else falls through to a plain `open`.
#
# WHY THIS IS A BASH LOOP, NOT ONE APPLESCRIPT: an AppleScript `tell
# application "Google Chrome"` block needs Chrome's scripting dictionary AT
# COMPILE TIME — on a machine without Chrome the whole script fails to
# compile, including the branches that never run. So each browser gets its
# own osascript invocation, attempted only after bash has confirmed that
# browser is running (running ⇒ installed ⇒ dictionary available).
focus_or_open_ui() {
    URL="http://localhost:$1"; MATCH=":$1"   # matches localhost and 127.0.0.1 tabs
    if command -v osascript >/dev/null 2>&1; then
        for B in "Google Chrome" "Brave Browser" "Microsoft Edge"; do
            [ "$(osascript -e "application \"$B\" is running" 2>/dev/null)" = true ] || continue
            R=$(osascript 2>/dev/null <<OSA
tell application "$B"
  repeat with w in windows
    set i to 1
    repeat with t in tabs of w
      if URL of t contains "$MATCH" then
        set active tab index of w to i
        set index of w to 1
        activate
        return "focused"
      end if
      set i to i + 1
    end repeat
  end repeat
end tell
return "none"
OSA
)
            [ "$R" = focused ] && say "focused the existing $B tab" && return 0
        done
        if [ "$(osascript -e 'application "Safari" is running' 2>/dev/null)" = true ]; then
            R=$(osascript 2>/dev/null <<OSA
tell application "Safari"
  repeat with w in windows
    repeat with t in tabs of w
      if URL of t contains "$MATCH" then
        tell w to set current tab to t
        set index of w to 1
        activate
        return "focused"
      end if
    end repeat
  end repeat
end tell
return "none"
OSA
)
            [ "$R" = focused ] && say "focused the existing Safari tab" && return 0
        fi
    fi
    command -v open >/dev/null 2>&1 && open "$URL"
}

# ── 1. Stop what's running ── ON THIS PORT ONLY ──────────────────────────────
# A rig can run several instances (one per input channel: voice on 8017,
# guitar on 8018, …), so stopping is scoped to $PORT: match the command line
# we launch with, never the bare process name — a name-wide pkill would take
# the other channel's engine down with ours.
for name in "${PROC_NAMES[@]}"; do
    if pkill -f "$name --port $PORT( |\$)" 2>/dev/null; then
        say "stopped running $name (port $PORT)"
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            pgrep -f "$name --port $PORT( |\$)" >/dev/null 2>&1 || break
            sleep 0.3
        done
        pgrep -f "$name --port $PORT( |\$)" >/dev/null 2>&1 \
            && pkill -9 -f "$name --port $PORT( |\$)" 2>/dev/null
    fi
done
# Belt and braces: whatever else is still holding our port (an instance
# launched by hand, a rename) — a stale listener would eat the health check.
if command -v lsof >/dev/null 2>&1; then
    HOLDERS="$(lsof -ti tcp:"$PORT" 2>/dev/null || true)"
    if [ -n "$HOLDERS" ]; then
        say "stopping process(es) on port $PORT: $HOLDERS"
        kill $HOLDERS 2>/dev/null || true
        sleep 0.3
        kill -9 $HOLDERS 2>/dev/null || true
    fi
fi

if [ "$STOP_ONLY" = 1 ]; then
    say "stopped"
    exit 0
fi

# ── 2. Build only if something changed ───────────────────────────────────────
if needs_build; then
    say "building"
    [ "$HEADLESS" = 1 ] && notify "Rebuilding — this can take a minute…"
    if [ "$HEADLESS" = 1 ]; then
        do_build >> "$LAUNCH_LOG" 2>&1 || die "build failed — see the log"
    else
        do_build || die "build failed — nothing was relaunched"
    fi
else
    say "already up to date"
fi

# ── 3. Run ───────────────────────────────────────────────────────────────────
BIN="$(run_cmd)"
[ -x "$BIN" ] || die "no executable at $BIN"
say "log $RUN_LOG"
if [ "$HEADLESS" = 1 ]; then
    # No window to keep alive: detach and let this script finish.
    nohup "$BIN" "${run_args[@]}" > "$RUN_LOG" 2>&1 &
    RUN_PID=$!
    disown $RUN_PID 2>/dev/null || true
else
    # Process substitution, not a pipe, so $! is the app's own PID
    # (with `| tee` it would be tee's, and Ctrl-C would kill the log).
    "$BIN" "${run_args[@]}" > >(tee "$RUN_LOG") 2>&1 &
    RUN_PID=$!
    trap 'kill $RUN_PID 2>/dev/null; exit 0' INT TERM
fi

# ── 4. Wait for the web UI, then focus or open it ────────────────────────────
UI_UP=0
for _ in $(seq 1 40); do
    kill -0 $RUN_PID 2>/dev/null || break
    if curl -fsS -o /dev/null --max-time 1 "http://localhost:$PORT/api/status" 2>/dev/null; then
        UI_UP=1; break
    fi
    sleep 0.5
done

if [ "$UI_UP" = 1 ]; then
    say "web UI up — http://localhost:$PORT"
    [ "$OPEN_UI" = 1 ] && focus_or_open_ui "$PORT"
elif ! kill -0 $RUN_PID 2>/dev/null; then
    tail -5 "$RUN_LOG" >> "$LAUNCH_LOG" 2>/dev/null
    die "exited during startup ($(tail -1 "$RUN_LOG" 2>/dev/null | cut -c1-120))"
else
    say "running, but the web UI has not answered on port $PORT"
fi

if [ "$HEADLESS" = 1 ]; then
    say "launched — leaving $APP_NAME running (pid $RUN_PID)"
    exit 0            # the Dock button's job is done
fi

wait $RUN_PID
STATUS=$?
say "$APP_NAME exited ($STATUS) — log kept at $RUN_LOG"
[ "$STATUS" -ne 0 ] && tail -20 "$RUN_LOG"
exit $STATUS
