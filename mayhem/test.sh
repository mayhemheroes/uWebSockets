#!/usr/bin/env bash
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{"results":{"tool":{"name":"$tool"},"summary":{"tests":$tests,"passed":$passed,"failed":$failed,"pending":$pending,"skipped":$skipped,"other":$other}}}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Behavioral oracle (anti-reward-hack, SPEC §6.3): each test must exit 0 AND print a completion marker
# that is only reached after all its assert()s hold. A program neutered to exit(0) prints no marker, so
# the marker'd tests FAIL and test.sh fails — the oracle asserts behavior, not just the exit code.
BIN_DIR=/mayhem/tests-bin
TESTS=(Query ChunkedEncoding TopicTree HttpRouter BloomFilter ExtensionsNegotiator HttpParser)
declare -A MARK=(
  [Query]=""                                     # asserts only (no stdout); guarded by exit code
  [ChunkedEncoding]="ALL BRUTEFORCE DONE"
  [TopicTree]="TestReorderingv19"
  [HttpRouter]="Duration:"
  [BloomFilter]="Total false positives:"
  [ExtensionsNegotiator]="ALL PASS"
  [HttpParser]="HTTP DONE"
)
passed=0; failed=0

for t in "${TESTS[@]}"; do
  bin="$BIN_DIR/$t"
  if [ ! -x "$bin" ]; then
    echo "ERROR: missing test binary $bin" >&2
    failed=$((failed + 1)); continue
  fi
  out="$("$bin" 2>&1)"; rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "FAIL $t (exit $rc)" >&2
    failed=$((failed + 1)); continue
  fi
  marker="${MARK[$t]}"
  if [ -n "$marker" ] && ! printf '%s' "$out" | grep -qF -- "$marker"; then
    echo "FAIL $t (missing behavioral marker: '$marker')" >&2
    failed=$((failed + 1)); continue
  fi
  echo "PASS $t"
  passed=$((passed + 1))
done

emit_ctrf "uwebsockets-tests" "$passed" "$failed" 0
