#!/bin/bash
# Vector provider adapter for the ebook knowledge base (my_db book cache).
#   argv: <query> <k>   ->  {"results":[{"key":name,"score":...}]}
#   env:  BOOK_CACHE (default /book/cache), MYDB (default /opt/my_db)
set -euo pipefail

QUERY="$1"
K="${2:-10}"
MYDB="${MYDB:-/opt/my_db}"
CACHE="${BOOK_CACHE:-/book/cache}"

export LD_LIBRARY_PATH="$MYDB:/data/cuda/lib64:/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:${LD_LIBRARY_PATH:-}"

RAW="$("$MYDB/tools/cache_query" "$QUERY" --analysis-dir "$CACHE" --type search \
        --max-results "$K" 2>/dev/null | grep '^{' | head -1)"

python3 - "$RAW" <<'PY'
import sys, json
raw = sys.argv[1]
if not raw:
    print('{"results":[]}')
    sys.exit(0)
d = json.loads(raw)
out = []
for r in d.get("results", []):
    name = r.get("name")
    if name:
        out.append({"key": name, "score": r.get("score", 0.0)})
print(json.dumps({"results": out}))
PY
