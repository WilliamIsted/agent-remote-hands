#!/usr/bin/env bash
#
# Copyright 2026 William Isted and contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Install the macos-modern agent as a launchd LaunchAgent in the current
# user's Aqua session. NOT a LaunchDaemon — daemons run in PID 1's
# bootstrap context with no window-server access, which is the macOS
# analogue of Windows Session 0.
#
# Usage:
#   Tools/install-macos-modern.sh [--port N] [--install-dir DIR] [--uninstall]
#
# Defaults:
#   --port         8765
#   --install-dir  ~/Applications/rha-mac
#
# The script:
#   1. Confirms a release build exists (runs `swift build -c release` if not)
#   2. Copies the binary to <install-dir>/rha-mac
#   3. Writes ~/Library/LaunchAgents/me.isted.rha.plist
#   4. Loads it via `launchctl bootstrap gui/$UID`
#
# The agent will start at next login (and immediately, via bootstrap).
# `KeepAlive { Crashed: true }` restarts it if it crashes; clean exits
# are not restarted automatically.
#
# Uninstall: --uninstall does the reverse — bootout + plist remove +
# binary remove. The token file under ~/Library/Application Support/
# AgentRemoteHands/ is left in place; remove it manually if you want a
# truly clean slate.

set -euo pipefail

PORT=8765
INSTALL_DIR="$HOME/Applications/rha-mac"
UNINSTALL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            PORT="$2"; shift 2 ;;
        --install-dir)
            INSTALL_DIR="$2"; shift 2 ;;
        --uninstall)
            UNINSTALL=1; shift ;;
        -h|--help)
            head -30 "$0" | tail -28
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

PLIST_PATH="$HOME/Library/LaunchAgents/me.isted.rha.plist"
LABEL="me.isted.rha"
BINARY_PATH="$INSTALL_DIR/rha-mac"

if [[ "$UNINSTALL" == "1" ]]; then
    echo "uninstalling launchd agent $LABEL..."
    if launchctl print "gui/$UID/$LABEL" > /dev/null 2>&1; then
        launchctl bootout "gui/$UID" "$PLIST_PATH" 2>/dev/null || true
    fi
    rm -f "$PLIST_PATH"
    rm -f "$BINARY_PATH"
    echo "removed $PLIST_PATH and $BINARY_PATH"
    echo "(token file at ~/Library/Application Support/AgentRemoteHands/ left in place)"
    exit 0
fi

# Locate the agent source tree (script lives at <repo>/Tools/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
AGENT_DIR="$REPO_ROOT/agents/macos-modern"
SOURCE_BINARY="$AGENT_DIR/.build/release/rha-mac"

# Build if needed.
if [[ ! -x "$SOURCE_BINARY" ]]; then
    echo "release binary not found; building..."
    (cd "$AGENT_DIR" && swift build -c release)
fi
if [[ ! -x "$SOURCE_BINARY" ]]; then
    echo "error: build produced no binary at $SOURCE_BINARY" >&2
    exit 1
fi

# Stage binary.
mkdir -p "$INSTALL_DIR"
cp "$SOURCE_BINARY" "$BINARY_PATH"
chmod +x "$BINARY_PATH"
echo "installed binary: $BINARY_PATH"

# Write plist.
mkdir -p "$(dirname "$PLIST_PATH")"
cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$BINARY_PATH</string>
        <string>--port</string>
        <string>$PORT</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>Crashed</key>
        <true/>
    </dict>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>LimitLoadToSessionType</key>
    <string>Aqua</string>
    <key>StandardOutPath</key>
    <string>$HOME/Library/Logs/rha-mac.out.log</string>
    <key>StandardErrorPath</key>
    <string>$HOME/Library/Logs/rha-mac.err.log</string>
</dict>
</plist>
EOF
echo "wrote plist: $PLIST_PATH"

# Replace any prior load.
if launchctl print "gui/$UID/$LABEL" > /dev/null 2>&1; then
    launchctl bootout "gui/$UID" "$PLIST_PATH" 2>/dev/null || true
fi
launchctl bootstrap "gui/$UID" "$PLIST_PATH"
echo "loaded via launchctl bootstrap gui/$UID"

echo ""
echo "done. The agent should be running on port $PORT."
echo "  status: launchctl print gui/\$UID/$LABEL"
echo "  logs:   tail -f ~/Library/Logs/rha-mac.{out,err}.log"
echo "  stop:   launchctl bootout gui/\$UID $PLIST_PATH"
