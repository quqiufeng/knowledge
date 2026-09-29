#!/bin/bash
# One-command pipeline: repo -> my_db analysis -> Knowledge import.
#
#   usage: ingest.sh <repo_path> [options]
#     --project <name>   project name (default: basename of repo)
#     --cache <dir>      my_db analysis cache (default: /opt/code_caches/<project>_cache)
#     --workers <n>      index workers (default: 4)
#     --skip-analyze     only import (analysis already exists)
#     --skip-hnsw        skip HNSW build
#     --skip-import      only analyze
#
#   env: MYDB (default /opt/my_db), KNOWLEDGE (default <script>/../knowledge)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KNOWLEDGE="${KNOWLEDGE:-$SCRIPT_DIR/../knowledge}"
MYDB="${MYDB:-/opt/my_db}"

REPO=""
PROJECT=""
CACHE=""
WORKERS=4
SKIP_ANALYZE=0
SKIP_HNSW=0
SKIP_IMPORT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --project) PROJECT="$2"; shift 2 ;;
        --cache) CACHE="$2"; shift 2 ;;
        --workers) WORKERS="$2"; shift 2 ;;
        --skip-analyze) SKIP_ANALYZE=1; shift ;;
        --skip-hnsw) SKIP_HNSW=1; shift ;;
        --skip-import) SKIP_IMPORT=1; shift ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) REPO="$1"; shift ;;
    esac
done

if [ -z "$REPO" ]; then
    echo "usage: ingest.sh <repo_path> [--project name] [--cache dir] [--workers n] [--skip-analyze] [--skip-hnsw] [--skip-import]" >&2
    exit 2
fi
[ -d "$REPO" ] || { echo "repo not found: $REPO" >&2; exit 1; }

REPO="$(cd "$REPO" && pwd)"
[ -n "$PROJECT" ] || PROJECT="$(basename "$REPO")"
[ -n "$CACHE" ] || CACHE="/opt/code_caches/${PROJECT}_cache"

echo "[ingest] repo=$REPO project=$PROJECT cache=$CACHE"

if [ "$SKIP_ANALYZE" -eq 0 ]; then
    export LD_LIBRARY_PATH="$MYDB:/data/cuda/lib64:/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:${LD_LIBRARY_PATH:-}"
    echo "[ingest] index ..."
    ( cd "$MYDB" && ./ai_code_search.sh index "$REPO" "$CACHE" "$WORKERS" )
    echo "[ingest] vector ..."
    ( cd "$MYDB" && ./ai_code_search.sh vector "$CACHE" "$PROJECT" )

    BIN="$(ls "$CACHE"/vectors/*.jina.bin 2>/dev/null | head -1 || true)"
    if [ "$SKIP_HNSW" -eq 0 ] && [ -n "$BIN" ]; then
        echo "[ingest] hnsw: $BIN"
        ( cd "$MYDB" && ./tools/build_hnsw_index "$BIN" --threads 8 )
    fi
fi

if [ "$SKIP_IMPORT" -eq 0 ]; then
    echo "[ingest] import -> PostgreSQL ..."
    "$KNOWLEDGE" import --analysis-dir "$CACHE" --project "$PROJECT" --root "$REPO"
fi

echo "[ingest] done. try:"
echo "  export KNOWLEDGE_VECTOR_CMD=$SCRIPT_DIR/vector_provider.sh"
echo "  export ANALYSIS_DIR=$CACHE PROJECT=$PROJECT ROOT=$REPO"
echo "  $KNOWLEDGE search \"<natural language>\" --k 5"
