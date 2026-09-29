#!/bin/bash
# Real vector provider adapter: my_db cache_query -> Knowledge provider contract.
#   argv: <query> <k>
#   env:  ANALYSIS_DIR (required, e.g. /opt/code_caches/redis_cache)
#         PROJECT      (default: analysis dir name minus _cache)
#         ROOT         (default: /opt/$PROJECT)
#         MYDB         (default: /opt/my_db)
set -euo pipefail

QUERY="$1"
K="${2:-10}"
ANALYSIS_DIR="${ANALYSIS_DIR:?set ANALYSIS_DIR to a my_db cache dir}"
PROJECT="${PROJECT:-$(basename "$ANALYSIS_DIR" | sed 's/_cache$//')}"
ROOT="${ROOT:-/opt/$PROJECT}"
MYDB="${MYDB:-/opt/my_db}"

export LD_LIBRARY_PATH="$MYDB:/data/cuda/lib64:/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:${LD_LIBRARY_PATH:-}"

RAW="$("$MYDB/tools/cache_query" "$QUERY" --analysis-dir "$ANALYSIS_DIR" --type search \
        --max-results "$K" 2>/dev/null | grep '^{' | head -1)"

python3 - "$RAW" "$PROJECT" "$ROOT" <<'PY'
import sys, json, os
raw, project, root = sys.argv[1], sys.argv[2], sys.argv[3]
if not raw:
    print('{"results":[]}')
    sys.exit(0)
d = json.loads(raw)
out = []
for r in d.get("results", []):
    f = r.get("file") or ""
    rel = os.path.relpath(f, root) if f.startswith(root) else f.lstrip("/")
    out.append({"key": f"/code/local/{project}/{rel}/{r.get('name')}", "score": r.get("score", 0.0)})
print(json.dumps({"results": out}))
PY
