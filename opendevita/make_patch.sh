#!/bin/bash
# make_patch.sh — writes the Vita changes in ./OpenDE back to patches/opende-vita.patch.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/OpenDE"
git -C "$SRC" add -A
git -C "$SRC" diff --cached --binary 72e78a65ceff277e7f2e03979874533adf6fa905 > "$ROOT/patches/opende-vita.patch"
echo "wrote patches/opende-vita.patch ($(wc -l < "$ROOT/patches/opende-vita.patch") lines)"
