#!/bin/bash
# Prepares the ReXGlue SDK source tree for the Android build:
#   v0.10.0 + patches/rexglue-sdk (NovaPowers) + patches/rexglue-sdk-android
# Usage: android/setup_sdk.sh <dest-dir>
set -euo pipefail
DEST=$1
REPO=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -d "$DEST/.git" ]; then
  git clone --branch v0.10.0 --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/rexglue/rexglue-sdk "$DEST"
fi
cp -r "$REPO/patches/rexglue-sdk/." "$DEST/"
(cd "$REPO/patches/rexglue-sdk-android" && find . -type f ! -name README.md -exec cp --parents {} "$DEST/" \;)
echo "SDK listo en $DEST"
