#!/usr/bin/env bash
# Test suite for nccl-cc.
#
# Each `assert <expected> <program>` compiles a complete C program, links it,
# runs it and checks the process exit status.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CC="$ROOT/ncclcc"
GCC="${GCC:-gcc}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# assert <expected-exit-code> <full-program>
assert() {
  local expected="$1"
  local program="$2"

  printf '%s\n' "$program" > "$WORK/test.c"

  if ! "$CC" -S -o "$WORK/test.s" "$WORK/test.c" > "$WORK/cc.log" 2>&1; then
    echo "FAIL (compile): $program"
    sed 's/^/    /' "$WORK/cc.log"
    fail=$((fail + 1))
    return
  fi

  if ! "$GCC" -o "$WORK/test.exe" "$WORK/test.s" > "$WORK/as.log" 2>&1; then
    echo "FAIL (assemble): $program"
    sed 's/^/    /' "$WORK/as.log"
    fail=$((fail + 1))
    return
  fi

  "$WORK/test.exe"
  local actual=$?

  if [ "$actual" = "$expected" ]; then
    pass=$((pass + 1))
  else
    echo "FAIL: expected $expected but got $actual: $program"
    fail=$((fail + 1))
  fi
}

# assert_fail <program>: the compiler must reject the program with an error.
assert_fail() {
  local program="$1"
  printf '%s\n' "$program" > "$WORK/test.c"
  if "$CC" -S -o "$WORK/test.s" "$WORK/test.c" > "$WORK/cc.log" 2>&1; then
    echo "FAIL (should not compile): $program"
    fail=$((fail + 1))
  else
    pass=$((pass + 1))
  fi
}

# --- build ---------------------------------------------------------------
(cd "$ROOT" && ./build.sh) || { echo "build failed"; exit 1; }

# --- tests ---------------------------------------------------------------
# step 1: return constant
assert 0 'int main() { return 0; }'
assert 42 'int main() { return 42; }'
assert 7 'int main() { return 7; }'

assert_fail 'int main() { return ; }'
assert_fail 'int main() { return x; }'

# --- report --------------------------------------------------------------
echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
