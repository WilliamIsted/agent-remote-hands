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
# Sign + notarise the macos-modern agent.
#
# Prerequisites:
#   - A Developer ID Application certificate in your keychain
#   - `notarytool` credentials set up. Either:
#       * App-specific password stored under a notary profile via:
#         `xcrun notarytool store-credentials NOTARY_PROFILE --apple-id <id> --team-id <tid> --password <app-pwd>`
#       * Or pass --apple-id / --team-id / --password on each run
#   - swift build -c release --arch arm64 --arch x86_64 produces the
#     Universal 2 binary at .build/apple/Products/Release/rha-mac
#
# Usage:
#   Tools/sign-macos-modern.sh \
#       --identity "Developer ID Application: <Your Name> (TEAMID)" \
#       [--profile NOTARY_PROFILE] \
#       [--output ./dist/rha-mac.modern.universal2]
#
# The script:
#   1. Builds Universal 2 (arm64 + x86_64) via swift build
#   2. codesigns with Hardened Runtime and our entitlements
#   3. Zips for notarisation (notarytool requires .zip / .pkg / .dmg)
#   4. Submits to notarytool and polls for result
#   5. Staples the ticket back onto the binary if successful

set -euo pipefail

IDENTITY=""
NOTARY_PROFILE=""
OUTPUT="./dist/rha-mac.modern.universal2"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --identity) IDENTITY="$2"; shift 2 ;;
        --profile)  NOTARY_PROFILE="$2"; shift 2 ;;
        --output)   OUTPUT="$2"; shift 2 ;;
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

if [[ -z "$IDENTITY" ]]; then
    echo "error: --identity required (e.g. 'Developer ID Application: Your Name (TEAMID)')" >&2
    echo "list available identities: security find-identity -v -p codesigning" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
AGENT_DIR="$REPO_ROOT/agents/macos-modern"
ENTITLEMENTS="$AGENT_DIR/rha-mac.entitlements"

# 1. Build Universal 2.
echo "=== building Universal 2 release binary ==="
(cd "$AGENT_DIR" && swift build -c release --arch arm64 --arch x86_64)
RAW_BINARY="$AGENT_DIR/.build/apple/Products/Release/rha-mac"
if [[ ! -x "$RAW_BINARY" ]]; then
    # SwiftPM puts the universal binary at a slightly different path
    # depending on toolchain; fall back to the single-arch product dir.
    RAW_BINARY="$AGENT_DIR/.build/release/rha-mac"
fi
if [[ ! -x "$RAW_BINARY" ]]; then
    echo "error: no release binary found after build" >&2
    exit 1
fi
echo "  binary: $RAW_BINARY"
file "$RAW_BINARY" | sed 's/^/  /'

# 2. Stage + sign.
mkdir -p "$(dirname "$OUTPUT")"
cp "$RAW_BINARY" "$OUTPUT"
echo ""
echo "=== codesigning ==="
codesign --force --options runtime --timestamp \
    --sign "$IDENTITY" \
    --entitlements "$ENTITLEMENTS" \
    "$OUTPUT"
codesign --verify --verbose "$OUTPUT"
echo "  signed: $OUTPUT"

# 3. Zip for notarisation.
ZIP_PATH="${OUTPUT}.zip"
echo ""
echo "=== zipping for notarytool ==="
rm -f "$ZIP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$OUTPUT" "$ZIP_PATH"

# 4. Submit + wait.
echo ""
echo "=== notarytool submit ==="
NOTARY_ARGS=()
if [[ -n "$NOTARY_PROFILE" ]]; then
    NOTARY_ARGS+=(--keychain-profile "$NOTARY_PROFILE")
else
    echo "warning: no --profile set; notarytool will need credentials in env or args" >&2
fi
xcrun notarytool submit "$ZIP_PATH" --wait "${NOTARY_ARGS[@]}"

# 5. Staple (notarytool returns success only if the ticket is issued).
echo ""
echo "=== stapling ==="
xcrun stapler staple "$OUTPUT"
xcrun stapler validate "$OUTPUT"

echo ""
echo "done."
echo "  signed + notarised + stapled binary: $OUTPUT"
echo "  bundle zip:                          $ZIP_PATH"
echo ""
echo "Verify on a fresh machine: spctl -a -vv $OUTPUT"
