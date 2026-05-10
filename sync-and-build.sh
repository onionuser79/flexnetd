#!/usr/bin/env bash
set -euo pipefail

HOST=iw2ohx-gw
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_DIR="/home/iw2ohx/xnet_investigation_agent/flexnetd"

echo ">> rsync $LOCAL_DIR/ -> $HOST:$REMOTE_DIR/"
rsync -az --delete \
    --exclude '.git' \
    --exclude '*.o' \
    --exclude '*.log' \
    --exclude 'sync-and-build.sh' \
    "$LOCAL_DIR/" "$HOST:$REMOTE_DIR/"

echo ">> remote make in $REMOTE_DIR"
ssh "$HOST" "cd '$REMOTE_DIR' && make ${*:-}"
