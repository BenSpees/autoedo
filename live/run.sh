#!/usr/bin/env bash
# Compatibility wrapper — the launcher logic lives in tools/autoedo.command
# (terminal mode when run directly; the Dock bundle tools/AutoEDO.app runs it
# with --headless). Flags pass through: --stop, --no-ui, --headless.
exec "$(cd "$(dirname "$0")" && pwd)/tools/autoedo.command" "$@"
