#!/bin/sh
# Host-side test suite for the espclaw Lua package.
# Run from the repo root: sh tests/run.sh
set -e
for t in tests/test_sse.lua tests/test_backends.lua tests/test_config.lua tests/test_sandbox.lua; do
  echo "=== $t ==="
  lua "$t"
  echo ""
done
echo "all suites passed"
