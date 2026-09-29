#!/bin/bash
# Embed a single text with my_db's Jina model -> JSON array of 768 floats.
#   usage: embed_query.sh "<text>"
#   env:   MYDB (default /opt/my_db)
set -euo pipefail

TEXT="${1:?usage: embed_query.sh \"<text>\"}"
MYDB="${MYDB:-/opt/my_db}"
export LD_LIBRARY_PATH="$MYDB:/data/cuda/lib64:/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:${LD_LIBRARY_PATH:-}"

D="$(mktemp -d)"
trap 'rm -rf "$D"' EXIT
printf '%s\n' "$TEXT" > "$D/chunks_text.txt"
printf '{"name":"q","file":"q","kind":"function","line_start":0,"language":"c","signature":"","content":"","docstring":""}\n' > "$D/chunks_meta.jsonl"

"$MYDB/tools/batch_embedder" "$D" --name q >/dev/null 2>&1

BIN="$D/vectors/code_local_q.jina.bin"
python3 - "$BIN" <<'PY'
import struct, sys, json
with open(sys.argv[1], "rb") as f:
    count, dim = struct.unpack("<II", f.read(8))
    if count == 0:
        print("[]")
        sys.exit(0)
    nl = struct.unpack("<I", f.read(4))[0]
    f.read(nl)
    vec = struct.unpack("<%df" % dim, f.read(dim * 4))
print(json.dumps([round(v, 7) for v in vec]))
PY
