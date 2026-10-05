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

# step 2: unary operators
assert 5 'int main() { return +5; }'
assert 10 'int main() { return -(-10); }'
assert 3 'int main() { return (-(-3)); }'
assert 0 'int main() { return !5; }'
assert 1 'int main() { return !0; }'
assert 42 'int main() { return ~(-43); }'
assert 0 'int main() { return ~(-1); }'

# step 3: binary arithmetic + - * / %
assert 21 'int main() { return 5+20-4; }'
assert 41 'int main() { return 12 + 34 - 5; }'
assert 47 'int main() { return 5+6*7; }'
assert 15 'int main() { return 5*(9-6); }'
assert 4 'int main() { return (3+5)/2; }'
assert 10 'int main() { return -10+20; }'
assert 10 'int main() { return - -10; }'
assert 10 'int main() { return - - +10; }'
assert 14 'int main() { return 2+3*4; }'
assert 20 'int main() { return (2+3)*4; }'
assert 2 'int main() { return 7/3; }'
assert 1 'int main() { return 7%3; }'
assert 8 'int main() { return 100/7/3+4; }'
assert 5 'int main() { return 2+9/3; }'

# step 4: comparison, logical and bitwise operators
assert 1 'int main() { return 1==1; }'
assert 0 'int main() { return 1==2; }'
assert 1 'int main() { return 1!=2; }'
assert 0 'int main() { return 1!=1; }'
assert 1 'int main() { return 1<2; }'
assert 0 'int main() { return 2<1; }'
assert 1 'int main() { return 1<=2; }'
assert 1 'int main() { return 2<=2; }'
assert 0 'int main() { return 3<=2; }'
assert 1 'int main() { return 2>1; }'
assert 1 'int main() { return 2>=2; }'
assert 0 'int main() { return 1>=2; }'
assert 1 'int main() { return 1<2==1; }'
assert 1 'int main() { return 1&&1; }'
assert 0 'int main() { return 1&&0; }'
assert 1 'int main() { return 0||1; }'
assert 0 'int main() { return 0||0; }'
assert 0 'int main() { return 0||1&&0; }'
assert 1 'int main() { return (0||1)&&1; }'
assert 2 'int main() { return !0+1; }'
assert 0 'int main() { return 1&0; }'
assert 1 'int main() { return 1&1; }'
assert 1 'int main() { return 0|1; }'
assert 3 'int main() { return 1|2; }'
assert 3 'int main() { return 1^2; }'
assert 0 'int main() { return 1^1; }'
assert 3 'int main() { return 1|2&3; }'
assert 7 'int main() { return 1+2|4; }'

# step 5: local variables and multiple statements
assert 3 'int main() { int a = 3; return a; }'
assert 8 'int main() { int a = 3; int b = 5; return a+b; }'
assert 11 'int main() { int x = 2; int y = 3; return x*y + x + y; }'
assert 3 'int main() { int foo = 3; return foo; }'
assert 6 'int main() { int a; int b; int c; return 6; }'
assert 17 'int main() { int a = 10; int b = 7; a; b; return a+b; }'
assert 3 'int main() { int a = 3; a; return a; }'
assert 10 'int main() { int a = 1; int b = 2; int c = 3; int d = 4; return a+b+c+d; }'

# step 6: global variables
assert 0 'int g; int main() { return g; }'
assert 3 'int g = 3; int main() { return g; }'
assert 7 'int a = 1; int b = 6; int main() { return a+b; }'
assert 3 'int g = 3; int main() { int a = g; return a; }'
assert 21 'int g = 10; int main() { return g*2+1; }'
assert 42 'int g = 42; int main() { int g = 1; return g + 41; }'

assert_fail 'int main() { return ; }'
assert_fail 'int main() { return x; }'
assert_fail 'int main() { return -; }'
assert_fail 'int main() { return (3; }'

# --- report --------------------------------------------------------------
echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
