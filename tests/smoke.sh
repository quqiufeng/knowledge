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
    $BIN serve --port 8799 --ro --pool 2 >/tmp/ksrv.log 2>&1 &
    SPID=$!
    sleep 1
    has '"ok"' "$(curl -s localhost:8799/health)" "GET /health"
    has 'knowledge' "$(curl -s localhost:8799/stats)" "GET /stats"
    has '"key"' "$(curl -s -X POST localhost:8799/jse -d '{"$where":{"$fti":"zmalloc"},"$project":["key"],"$limit":1}')" "POST /jse"
    kill $SPID 2>/dev/null
    wait $SPID 2>/dev/null
else
    echo "  SKIP - curl not found"
fi

echo "[smoke] $pass passed, $fail failed"
[ "$fail" -eq 0 ]
