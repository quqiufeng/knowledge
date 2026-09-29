#!/bin/bash
# Stub vector provider: mimics the external engine contract.
# argv: <query> <k>  ->  JSON {"results":[{"key":...,"score":...}]}
printf '{"results":[{"key":"/code/local/linux/mm/page_alloc.c/prepare_alloc_pages","score":0.91},{"key":"/code/local/linux/mm/page_alloc.c/alloc_pages","score":0.88},{"key":"/code/local/linux/mm/page_alloc.c/__alloc_pages_may_oom","score":0.72}]}\n'
