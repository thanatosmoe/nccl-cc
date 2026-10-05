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

  if ! timeout 10 "$CC" -S -o "$WORK/test.s" "$WORK/test.c" > "$WORK/cc.log" 2>&1; then
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

  timeout 5 "$WORK/test.exe" > /dev/null 2>&1
  local actual=$?
  if [ "$actual" = 124 ]; then
    echo "FAIL (timeout): $program"
    fail=$((fail + 1))
    return
  fi

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
  timeout 10 "$CC" -S -o "$WORK/test.s" "$WORK/test.c" > "$WORK/cc.log" 2>&1
  local rc=$?
  if [ "$rc" -eq 124 ]; then
    echo "FAIL (compile timeout): $program"
    fail=$((fail + 1))
  elif [ "$rc" -eq 0 ]; then
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

# step 7: assignment
assert 3 'int main() { int a; a = 3; return a; }'
assert 8 'int main() { int a; int b; a = 3; b = 5; return a+b; }'
assert 3 'int main() { int a; return a = 3; }'
assert 8 'int main() { int a = 1; int b = 2; a = b = 8; return a; }'
assert 8 'int main() { int a = 1; int b = 2; a = b = 8; return b; }'
assert 4 'int main() { int a; int b; a = b = 2; return a+b; }'
assert 4 'int main() { int a = 3; int b; b = a = a + 1; return a; }'
assert 5 'int g; int main() { g = 5; return g; }'
assert 10 'int g = 3; int main() { g = g + 7; return g; }'
assert 6 'int g; int main() { int a = 2; g = a = a + 1; return g + a; }'

# step 8: conditional branches
assert 3 'int main() { if (1) return 3; return 5; }'
assert 5 'int main() { if (0) return 3; return 5; }'
assert 3 'int main() { if (1) return 3; else return 5; }'
assert 5 'int main() { if (0) return 3; else return 5; }'
assert 4 'int main() { int a = 0; if (1) a = 3; else a = 5; return a+1; }'
assert 6 'int main() { int a = 0; if (0) a = 3; else a = 5; return a+1; }'
assert 10 'int main() { int a = 5; if (a > 3) { a = a + 3; a = a + 2; } return a; }'
assert 15 'int main() { int x = 5; if (x < 10) if (x < 8) return 15; else return 20; return 30; }'
assert 20 'int main() { int x = 9; if (x < 10) if (x < 8) return 15; else return 20; return 30; }'
assert 2 'int main() { int a = 2; if (a == 1) return 1; else if (a == 2) return 2; else return 3; }'
assert 3 'int main() { int a = 9; if (a == 1) return 1; else if (a == 2) return 2; else return 3; }'
assert 3 'int main() { int a = 2; return a > 1 ? a + 1 : 0; }'
assert 5 'int main() { int a = 2; return a > 3 ? 100 : 5; }'
assert 4 'int main() { int a = 3; int b = 4; return a > b ? a : b; }'
assert 2 'int main() { return 1 ? 2 : 3 ? 4 : 5; }'
assert 4 'int main() { return 0 ? 2 : 1 ? 4 : 5; }'

# step 9: loops
assert 10 'int main() { int i = 0; int s = 0; while (i < 5) { s = s + i; i = i + 1; } return s; }'
assert 55 'int main() { int s = 0; for (int i = 1; i <= 10; i = i + 1) s = s + i; return s; }'
assert 45 'int main() { int s = 0; for (int i = 0; i < 10; i = i + 1) s = s + i; return s; }'
assert 5 'int main() { int i = 0; for (;;) { i = i + 1; if (i == 5) break; } return i; }'
assert 9 'int main() { int i = 0; int s = 0; for (i = 0; i < 10; i = i + 1) { if (i == 5) continue; s = s + 1; } return s; }'
assert 120 'int main() { int f = 1; for (int i = 1; i <= 5; i = i + 1) f = f * i; return f; }'
assert 6 'int main() { int n = 0; for (int i = 0; i < 3; i = i + 1) for (int j = 0; j < 3; j = j + 1) { if (j == 2) break; n = n + 1; } return n; }'
assert 8 'int main() { int i; for (i = 0; i < 100; i = i + 1) { if (i * i > 50) break; } return i; }'
assert 3 'int main() { int i = 0; while (1) { i = i + 1; if (i == 3) break; } return i; }'
assert 3 'int main() { int i; for (i = 0; i < 3; i = i + 1); return i; }'
assert 25 'int main() { int i = 0; int j = 0; int n = 0; while (i < 5) { j = 0; while (j < 5) { n = n + 1; j = j + 1; } i = i + 1; } return n; }'

# step 10: functions
assert 42 'int main() { return foo(); } int foo() { return 42; }'
assert 3 'int main() { return add(1, 2); } int add(int a, int b) { return a + b; }'
assert 10 'int main() { return add(4, 6); } int add(int a, int b) { return a + b; }'
assert 7 'int main() { return sub(10, 3); } int sub(int a, int b) { return a - b; }'
assert 8 'int main() { return mul(add(1, 1), 4); } int add(int a, int b) { return a+b; } int mul(int a, int b) { return a*b; }'
assert 10 'int main() { return add(1, 2) + add(3, 4); } int add(int a, int b) { return a+b; }'
assert 12 'int id(int x) { return x; } int main() { return id(3)+id(4)+id(5); }'
assert 100 'int id(int x) { return x; } int main() { return id(10) * id(10); }'
assert 5 'int sum5(int a, int b, int c, int d, int e) { return a+b+c+d+e; } int main() { return sum5(1,1,1,1,1); }'
assert 55 'int sum6(int a, int b, int c, int d, int e, int f) { return a+b+c+d+e+f; } int main() { return sum6(1,2,3,4,5,40); }'
assert 5 'int f(int x) { if (x > 0) return 5; return 9; } int main() { return f(1); }'
assert 9 'int f(int x) { if (x > 0) return 5; return 9; } int main() { return f(0); }'
assert 7 'int g; int set(int x) { g = x; return 0; } int main() { set(7); return g; }'
assert 6 'int f(int a, int b, int c) { int d = a + b; return d + c; } int main() { return f(1, 2, 3); }'

# step 11: recursion
assert 120 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); } int main() { return fact(5); }'
assert 24 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); } int main() { return fact(4); }'
assert 1 'int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); } int main() { return fact(0); }'
assert 55 'int fib(int n) { if (n < 2) return n; return fib(n-1) + fib(n-2); } int main() { return fib(10); }'
assert 89 'int fib(int n) { if (n < 2) return n; return fib(n-1) + fib(n-2); } int main() { return fib(11); }'
assert 210 'int sum(int n) { if (n == 0) return 0; return n + sum(n - 1); } int main() { return sum(20); }'
assert 9 'int ack(int m, int n) { if (m == 0) return n + 1; if (n == 0) return ack(m - 1, 1); return ack(m - 1, ack(m, n - 1)); } int main() { return ack(2, 3); }'
assert 1 'int is_odd(int n) { if (n == 0) return 0; return is_even(n - 1); } int is_even(int n) { if (n == 0) return 1; return is_odd(n - 1); } int main() { return is_even(10); }'
assert 1 'int is_odd(int n) { if (n == 0) return 0; return is_even(n - 1); } int is_even(int n) { if (n == 0) return 1; return is_odd(n - 1); } int main() { return is_odd(7); }'

# step 12: char type and string literals
assert 65 "int main() { return 'A'; }"
assert 66 "int main() { return 'A' + 1; }"
assert 10 "int main() { return '\n'; }"
assert 0 "int main() { return '\0'; }"
assert 122 "int main() { char c = 'z'; return c; }"
assert 65 "int main() { char c = 321; return c; }"
assert 97 "int main() { char c = 'a'; c = 'A' + 32; return c; }"
assert 65 "int f(char c) { return c; } int main() { return f(65); }"
assert 65 "int f(char c) { return c; } int main() { return f(321); }"
assert 6 'int main() { return printf("hello\n"); }'
assert 3 'int main() { return printf("abc"); }'
assert 5 'int main() { return printf("%d", 12345); }'
assert 1 'int main() { return printf("%c", 65); }'
assert 1 'int main() { char c = 65; return printf("%c", c); }'
assert 12 'int main() { return printf("hello %s\n", "world"); }'

# step 13: arrays
assert 6 'int main() { int a[3]; a[0]=1; a[1]=2; a[2]=3; return a[0]+a[1]+a[2]; }'
assert 10 'int main() { int a[3]; a[2]=10; return a[2]; }'
assert 6 'int main() { int a[2][3]; a[0][0]=1; a[0][1]=2; a[1][2]=3; return a[0][0]+a[0][1]+a[1][2]; }'
assert 5 'int main() { int a[2][3]; a[1][1]=5; return a[1][1]; }'
assert 16 'int main() { int a[5]; for (int i=0;i<5;i=i+1) a[i]=i*i; return a[4]; }'
assert 30 'int main() { int a[5]; for (int i=0;i<5;i=i+1) a[i]=i*i; return a[0]+a[1]+a[2]+a[3]+a[4]; }'
assert 195 "int main() { char s[4]; s[0]='a'; s[1]='b'; return s[0]+s[1]; }"
assert 5 'int g[3]; int main() { g[2]=5; return g[2]; }'
assert 6 'int sum(int a[], int n) { int s=0; for (int i=0;i<n;i=i+1) s=s+a[i]; return s; } int main() { int a[3]; a[0]=1;a[1]=2;a[2]=3; return sum(a,3); }'
assert 36 'int main() { int a[3][3]; int n=0; for (int i=0;i<3;i=i+1) for (int j=0;j<3;j=j+1) { a[i][j]=i*3+j; n=n+a[i][j]; } return n; }'

assert_fail 'int main() { return ; }'
assert_fail 'int main() { return x; }'
assert_fail 'int main() { return -; }'
assert_fail 'int main() { return (3; }'
assert_fail 'int main() { return 1 = 2; }'
assert_fail 'int main() { break; }'
assert_fail 'int main() { continue; }'

# --- report --------------------------------------------------------------
echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
