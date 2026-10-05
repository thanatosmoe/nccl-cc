#!/usr/bin/env bash
# Build nccl-cc with the host C compiler.
set -e
cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--std=gnu11 -Wall -Wextra -O2}"

$CC $CFLAGS -o ncclcc main.c tokenize.c parse.c type.c codegen.c

echo "built ./ncclcc"
