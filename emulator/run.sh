#!/usr/bin/env bash
# Compile et lance JellySync dans l'émulateur PC.
#   ./emulator/run.sh          → app seule (vrai serveur Jellyfin)
#   ./emulator/run.sh --mock   → démarre aussi un faux Jellyfin sur http://127.0.0.1:8096
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-emu"

cmake -S "$ROOT/emulator" -B "$BUILD" >/dev/null
cmake --build "$BUILD" -j4

if [ "$1" = "--mock" ]; then
    python3 "$ROOT/emulator/mock_jellyfin.py" &
    MOCK_PID=$!
    trap 'kill $MOCK_PID 2>/dev/null' EXIT
    sleep 0.5
fi

"$BUILD/jellysync-emu"
