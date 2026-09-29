#!/bin/bash
# Smoke tests for the knowledge binary (JSE compiler + PG end-to-end).
set -uo pipefail
cd "$(dirname "$0")/.."
BIN=./knowledge
pass=0
fail=0

ok()  { echo "  ok   - $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL - $1"; fail=$((fail + 1)); }
has() { if echo "$2" | grep -q "$1"; then ok "$3"; else bad "$3"; fi; }
rejects() { if eval "$1" >/dev/null 2>&1; then bad "$2 (expected rejection)"; else ok "$2"; fi; }

[ -x "$BIN" ] || { echo "build first: make"; exit 1; }

echo "[smoke] compiler"

tokens=$($BIN tokenize "__alloc_pages_slowpath AllocPageSlow")
has '"alloc"' "$tokens" "tokenize splits snake_case"
has '"allocpageslow"' "$tokens" "tokenize splits camelCase"

sql=$(echo '{"$where":{"$fti":"page_alloc"}}' | $BIN compile)
has 'to_tsquery' "$sql" "compile \$fti -> tsquery"
has 'search_tsv' "$sql" "compile uses search_tsv"

rejects "echo '{\"\$where\":{\"\$meta\":{\"path\":\"kind\"}}}' | $BIN compile" "\$meta without operator rejected"
rejects "echo '{\"\$where\":{\"\$and\":[{\"\$fti\":\"a\"}],\"\$fti\":\"b\"}}' | $BIN compile" "multiple operators rejected"
rejects "echo '{\"\$where\":{\"\$meta\":{\"path\":\"kind; DROP TABLE knowledge;--\",\"\$eq\":\"x\"}}}' | $BIN compile" "path injection rejected"
rejects "echo '{\"\$where\":{\"\$fti\":\"a\"},\"\$order\":{\"\$search_score\":\"desc\"}}' | $BIN compile" "\$search_score requires \$search"
rejects "echo '{\"\$where\":{\"\$meta\":{\"path\":\"lang\",\"\$eq\":\"c\"}},\"\$order\":{\"\$fti_rank\":\"desc\"}}' | $BIN compile" "\$fti_rank requires \$fti"
rejects "echo '{\"\$where\":{\"\$triple\":{}}}' | $BIN compile" "\$triple without endpoint rejected"
rejects "echo '{\"\$where\":{\"\$k-hop\":{\"\$from\":\"/k\",\"predicates\":[\"/pred/calls\"],\"\$depth\":2,\"where\":{\"\$knn\":{\"vector\":[1],\"k\":1}}}}}' | $BIN compile" "\$knn inside \$k-hop.where rejected"
rejects "echo '{\"\$where\":{\"\$and\":[{\"\$knn\":{\"vector\":[1]}},{\"\$knn\":{\"vector\":[2]}}]}}' | $BIN compile" "duplicate \$knn rejected"
rejects "echo '{\"\$where\":{\"\$meta\":{\"path\":\"symbol\",\"\$eq\":\"x\"}},\"\$project\":[\"\$knn_distance\"]}' | $BIN compile" "\$knn_distance without \$knn rejected"
kdc=$(echo '{"$where":{"$knn":{"vector":[0.1,0.2],"k":3}},"$project":["key","$knn_distance"]}' | $BIN compile)
has '\$knn_distance' "$kdc" "\$knn_distance projects the distance"
esc=$(echo '{"$where":{"$key":{"$prefix":"/data/a_1"}},"$project":["key"]}' | $BIN compile)
has 'ESCAPE' "$esc" "\$prefix escapes LIKE wildcards"

echo "[smoke] postgres"

if ! $BIN seed >/dev/null 2>&1; then
    echo "  SKIP - database unavailable"
    echo "[smoke] $pass passed, $fail failed"
    [ "$fail" -eq 0 ] || exit 1
    exit 0
fi

rows=$(echo '{"$where":{"$and":[{"$meta":{"path":"kind","$eq":"function"}},{"$fti":"page_alloc"}]},"$project":["key"],"$limit":50}' | $BIN query | grep -c '"key"')
[ "$rows" -ge 4 ] && ok "full-text query returns $rows rows" || bad "full-text query returned $rows rows (expected >=4)"

out=$(echo '{"$where":{"$triple":{"subject":"/code/local/linux/mm/page_alloc.c/__alloc_pages_slowpath","predicate":"/pred/calls","direction":"out"}},"$project":["key"],"$limit":10}' | $BIN query)
has 'prepare_alloc_pages' "$out" "\$triple direction out returns callee"
if echo "$out" | grep -q '"meta.symbol": "__alloc_pages_slowpath"'; then
    bad "\$triple direction out leaked the anchor"
else
    ok "\$triple direction out excludes anchor"
fi

compiled=$(echo '{"$where":{"$triple":{"subject":"/code/local/linux/mm/page_alloc.c/__alloc_pages_slowpath","predicate":"/pred/calls","direction":"out"}},"$project":["key"]}' | $BIN compile)
has '"params": \[\]' "$compiled" "key resolver emits integer literals (no params)"

compiled=$(echo '{"$where":{"$meta":{"path":"file","$prefix":"mm/"}},"$project":["key"]}' | $BIN compile)
has 'LIKE' "$compiled" "\$prefix compiles to LIKE"
compiled=$(echo '{"$where":{"$meta":{"path":"symbol","$ilike":"%alloc%"}},"$count":true}' | $BIN compile)
has 'count(\*)' "$compiled" "\$count compiles to count(*)"
compiled=$(echo '{"$where":{"$key":{"$prefix":"/code/local/mem/"}},"$project":["key"]}' | $BIN compile)
has 'knowledge.key LIKE' "$compiled" "\$key compiles to key LIKE"
compiled=$(echo '{"$where":{"$meta":{"path":"lang","$eq":"c"}},"$group_by":"meta.file"}' | $BIN compile)
has 'GROUP BY' "$compiled" "\$group_by compiles to GROUP BY"
compiled=$(echo '{"$where":{"$meta":{"path":"kind","$eq":"function"}},"$project":["key"],"$order":{"$in_degree":"desc"}}' | $BIN compile)
has 'count(\*) FROM statement' "$compiled" "\$in_degree order compiles to degree subquery"
compiled=$(echo '{"$where":{"$knn":{"vector":[0.1,0.2],"k":3}},"$project":["key"]}' | $BIN compile)
has '<=>' "$compiled" "\$knn compiles to pgvector distance"
has 'is_active' "$compiled" "active scope injected by default"

out=$(echo '{"$where":{"$meta":{"path":"symbol","$eq":5}},"$count":true}' | $BIN query 2>&1)
if echo "$out" | grep -q '"error"'; then bad "numeric compare on text meta (no cast error)"; else ok "numeric compare on text meta (no cast error)"; fi
out=$(echo '{"$where":{"$key":{"$eq":123}},"$count":true}' | $BIN query 2>&1)
if echo "$out" | grep -q '"error"'; then bad "numeric \$eq on key (no cast error)"; else ok "numeric \$eq on key (no cast error)"; fi
ne=$(echo '{"$where":{"$meta":{"path":"symbol","$ne":999999999}},"$count":true}' | $BIN query 2>&1)
if echo "$ne" | grep -q '"error"'; then bad "numeric \$ne on text meta"; else ok "numeric \$ne on text meta"; fi
gt=$(echo '{"$where":{"$meta":{"path":"line","$gt":400}},"$count":true}' | $BIN query 2>&1)
if echo "$gt" | grep -q '"error"'; then bad "numeric range on text meta"; else ok "numeric range on text meta"; fi

idrow=$(echo '{"$where":{"$key":{"$eq":"/code/local/linux/mm/page_alloc.c/alloc_pages"}},"$project":["id","version","meta.line"]}' | $BIN query 2>&1)
if echo "$idrow" | grep -qE '"id": [0-9]+' && ! echo "$idrow" | grep -q '"version": "'; then
    ok "db rows project native numbers"
else
    bad "db rows project native numbers ($idrow)"
fi

hybrid=$($BIN search "alloc pages" --k 3 --vector-cmd ./tools/vector_provider_stub.sh 2>&1)
has '"rrf"' "$hybrid" "hybrid search emits rrf score"

echo "[smoke] vector provider robustness"

noisy=$(mktemp /tmp/vp_noisy.XXXXXX.sh)
cat >"$noisy" <<'EOF'
#!/bin/bash
echo "[INFO] model warmup"
echo '{"results":[{"key":"/code/local/linux/mm/page_alloc.c/alloc_pages","score":0.9}]}'
EOF
chmod +x "$noisy"
out=$($BIN search "memory" --k 5 --vector-cmd "$noisy" 2>&1)
has 'alloc_pages' "$out" "search tolerates noisy provider output"
rm -f "$noisy"

recf=$(mktemp /tmp/rec.XXXXXX.jsonl)
printf '{"key":"/data/smoke/k1","v":"hello world","n":1}\n' > "$recf"
nin=$($BIN compile <<< '{"$where":{"$meta":{"path":"line","$in":[312,412]}},"$project":["key"]}' 2>&1)
has '::numeric' "$nin" "numeric \$in casts text to numeric"
out=$(echo '{"$where":{"$meta":{"path":"line","$in":[312,412]}},"$count":true}' | $BIN query 2>&1)
if echo "$out" | grep -q '"error"'; then bad "numeric \$in query"; else ok "numeric \$in query"; fi

n=$($BIN import-records "$recf" --kind smokerec 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('imported',''))" 2>/dev/null)
[ "$n" = "1" ] && ok "import-records (jsonl -> entry)" || bad "import-records (got '$n')"
rm -f "$recf"

echo "[smoke] write integrity"
$BIN put --key /data/smoke/vstable --meta '{"t":1}' --content '{"text":"same"}' --agent smoke >/dev/null 2>&1
v1=$(echo '{"$where":{"$key":{"$eq":"/data/smoke/vstable"}},"$project":["version"]}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['version'])" 2>/dev/null)
$BIN put --key /data/smoke/vstable --meta '{"t":1}' --content '{"text":"same"}' --agent smoke >/dev/null 2>&1
v2=$(echo '{"$where":{"$key":{"$eq":"/data/smoke/vstable"}},"$project":["version"]}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['version'])" 2>/dev/null)
[ -n "$v1" ] && [ "$v1" = "$v2" ] && ok "identical put keeps version stable" || bad "identical put keeps version (v1='$v1' v2='$v2')"
$BIN put --key /data/smoke/vstable --meta '{"t":2}' --content '{"text":"same"}' --agent smoke >/dev/null 2>&1
v3=$(echo '{"$where":{"$key":{"$eq":"/data/smoke/vstable"}},"$project":["version"]}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['version'])" 2>/dev/null)
[ -n "$v3" ] && [ "$v3" != "$v2" ] && ok "changed put bumps version" || bad "changed put bumps version (v2='$v2' v3='$v3')"
$BIN forget /data/smoke/vstable --force --agent smoke >/dev/null 2>&1

$BIN put --key /data/smoke/reactiv --meta '{"t":1}' --content '{"text":"r"}' --agent smoke >/dev/null 2>&1
$BIN forget /data/smoke/reactiv --force --agent smoke >/dev/null 2>&1
$BIN put --key /data/smoke/reactiv --meta '{"t":1}' --content '{"text":"r2"}' --agent smoke >/dev/null 2>&1
ra=$(echo '{"$where":{"$key":{"$eq":"/data/smoke/reactiv"}},"$count":true}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
[ "$ra" = "1" ] && ok "put reactivates archived entry" || bad "put reactivates archived entry (count=$ra)"
$BIN forget /data/smoke/reactiv --force --agent smoke >/dev/null 2>&1

if $BIN link /data/smoke/no-such-a /pred/about /data/smoke/no-such-b >/dev/null 2>&1; then
    bad "link rejects missing endpoints"
else
    ok "link rejects missing endpoints"
fi
if $BIN remember --agent smoke --text dangling --about /data/smoke/no-such-about >/dev/null 2>&1; then
    bad "remember rejects dangling --about"
else
    ok "remember rejects dangling --about"
fi

recf2=$(mktemp /tmp/rec2.XXXXXX.jsonl)
printf '{broken\n{"key":"/data/smoke/abs1","v":1}\n{"novalue":1}\n' > "$recf2"
res=$($BIN import-records "$recf2" --prefix /data/pfx/ --kind smk2 2>/dev/null)
imp=$(echo "$res" | python3 -c "import sys,json;print(json.load(sys.stdin).get('imported',''))" 2>/dev/null)
skp=$(echo "$res" | python3 -c "import sys,json;print(json.load(sys.stdin).get('skipped',''))" 2>/dev/null)
[ "$imp" = "1" ] && [ "$skp" = "2" ] && ok "import-records reports skipped ($imp/$skp)" || bad "import-records skipped (got '$res')"
abs=$(echo '{"$where":{"$key":{"$eq":"/data/smoke/abs1"}},"$count":true}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
dbl=$(echo '{"$where":{"$key":{"$prefix":"/data/pfx//data"}},"$count":true}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
[ "$abs" = "1" ] && [ "$dbl" = "0" ] && ok "absolute keys ignore --prefix" || bad "absolute keys ignore --prefix (abs=$abs dbl=$dbl)"
rm -f "$recf2"

$BIN put --key /data/smoke/pfx2/a_1 --meta '{}' --content '{}' --agent smoke >/dev/null 2>&1
$BIN put --key /data/smoke/pfx2/ax1 --meta '{}' --content '{}' --agent smoke >/dev/null 2>&1
$BIN link /data/smoke/pfx2/a_1 /pred/about /data/smoke/pfx2/ax1 >/dev/null 2>&1
$BIN link /data/smoke/pfx2/ax1 /pred/about /data/smoke/pfx2/a_1 >/dev/null 2>&1
esubj=$($BIN export --preset edges --key-prefix /data/smoke/pfx2/a_1 --limit 10 2>/dev/null | python3 -c "import sys,json;[print(json.loads(l)['subject']) for l in sys.stdin]" 2>/dev/null)
[ "$esubj" = "/data/smoke/pfx2/a_1" ] && ok "export edges prefix is literal (underscore)" || bad "export edges prefix (got '$esubj')"
$BIN forget /data/smoke/pfx2/a_1 --force --agent smoke >/dev/null 2>&1
$BIN forget /data/smoke/pfx2/ax1 --force --agent smoke >/dev/null 2>&1
$BIN forget /data/smoke/abs1 --force --agent smoke >/dev/null 2>&1

echo "[smoke] export + context"
ec1=$(mktemp /tmp/ec1.XXXXXX.jsonl)
ec2=$(mktemp /tmp/ec2.XXXXXX.jsonl)
$BIN export --preset context --key-prefix /code/local/linux/mm/page_alloc.c/ --limit 3 --depth 1 --no-content >"$ec1" 2>/dev/null
$BIN export --preset context --key-prefix /code/local/linux/mm/page_alloc.c/ --limit 3 --depth 1 --no-content >"$ec2" 2>/dev/null
if cmp -s "$ec1" "$ec2" && [ -s "$ec1" ]; then ok "export context is deterministic"; else bad "export context is deterministic"; fi
ehead=$(head -n 1 "$ec1")
has '"definition"' "$ehead" "export context has definition"
has '"callers"' "$ehead" "export context has callers"
has '"paths"' "$ehead" "export context has paths"
rm -f "$ec1" "$ec2"

ctx=$($BIN context /code/local/linux/mm/page_alloc.c/alloc_pages --depth 0 --no-content 2>/dev/null)
has '"related"' "$ctx" "context always includes related"
ctx9=$($BIN context /code/local/linux/mm/page_alloc.c/alloc_pages --depth 99 --no-content 2>/dev/null)
has '"paths"' "$ctx9" "context clamps depth instead of failing"

echo "[smoke] agent memory"
mkey=$($BIN remember --agent smoke --text "smoke memory event" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('key',''))" 2>/dev/null)
case "$mkey" in /mem/smoke/events/*) ok "remember creates event" ;; *) bad "remember creates event" ;; esac
TOPIC="t$$"
$BIN fact --agent smoke --topic "$TOPIC" --value '"v1"' >/dev/null 2>&1
$BIN fact --agent smoke --topic "$TOPIC" --value '"v2"' >/dev/null 2>&1
arc=$(echo "{\"\$where\":{\"\$key\":{\"\$prefix\":\"/mem/smoke/facts/$TOPIC/@archive/\"}},\"\$count\":true}" | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
[ "$arc" = "1" ] && ok "fact archives previous value" || bad "fact archives previous value (got '$arc')"
$BIN forget "$mkey" >/dev/null 2>&1
after=$(echo '{"$where":{"$key":{"$prefix":"/mem/smoke/events/"}},"$count":true}' | $BIN query | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
[ "$after" = "0" ] && ok "forget archives; default scope excludes it" || bad "forget scope (got '$after')"

echo "[smoke] spec + actions"
$BIN spec-set smoke_spec '{"required":["meta.x"],"types":{"meta.x":"integer"}}' >/dev/null 2>&1
okput=$($BIN put --key /data/smoke/spec1 --meta '{"x":1}' --content '{}' --category smoke_spec 2>&1)
has '/data/smoke/spec1' "$okput" "put passes spec"
badput=$($BIN put --key /data/smoke/spec2 --meta '{}' --content '{}' --category smoke_spec 2>&1)
has 'missing required' "$badput" "put rejects spec violation"
aud=$(echo '{"$where":{"$key":{"$prefix":"/audit/"}},"$count":true}' | $BIN query 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['count'])" 2>/dev/null)
[ -n "$aud" ] && [ "$aud" -gt 0 ] && ok "actions are audited" || bad "actions are audited (got '$aud')"

echo "[smoke] http api"
if command -v curl >/dev/null 2>&1; then
    PORT=$(python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()" 2>/dev/null)
    PORT=${PORT:-8799}
    $BIN serve --port "$PORT" --ro --pool 2 >/tmp/ksrv.log 2>&1 &
    SPID=$!
    sleep 1
    has '"ok"' "$(curl -s "localhost:$PORT/health")" "GET /health"
    has 'knowledge' "$(curl -s "localhost:$PORT/stats")" "GET /stats"
    has '"key"' "$(curl -s -X POST "localhost:$PORT/jse" -d '{"$where":{"$fti":"zmalloc"},"$project":["key"],"$limit":1}')" "POST /jse"
    negk=$(curl -s "localhost:$PORT/search?q=slab&k=-5")
    if echo "$negk" | grep -q '"results"' && ! echo "$negk" | grep -q '_M_'; then
        ok "GET /search clamps negative k"
    else
        bad "GET /search clamps negative k"
    fi
    has '"paths"' "$(curl -s "localhost:$PORT/context?key=/code/local/linux/mm/page_alloc.c/alloc_pages&depth=99")" "GET /context clamps depth"
    st=$(curl -s -o /tmp/kjse.out -w "%{http_code}" -X POST "localhost:$PORT/jse" -d '{"$where":{"$k-hop":{"from":"/x","predicates":["/pred/calls"],"depth":1,"where":{"$knn":{"vector":[1],"k":1}}}}}')
    if [ "$st" = "400" ] && grep -q '\$knn' /tmp/kjse.out; then ok "POST /jse rejects \$knn in \$k-hop.where"; else bad "POST /jse rejects \$knn in \$k-hop.where (got $st)"; fi
    has '"callers"' "$(curl -s "localhost:$PORT/context?key=/code/local/linux/mm/page_alloc.c/alloc_pages&depth=1&exclude_headers")" "GET /context exclude_headers"
    kill $SPID 2>/dev/null
    wait $SPID 2>/dev/null
else
    echo "  SKIP - curl not found"
fi

echo "[smoke] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
