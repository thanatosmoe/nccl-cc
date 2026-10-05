#include "ncclcc.h"

static FILE *out;

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NUM:
    fprintf(out, "  mov $%ld, %%rax\n", node->val);
    return;
  }
  error("invalid expression");
}

void codegen(Node *node, FILE *outfile) {
  out = outfile;

  fprintf(out, "  .text\n");
  fprintf(out, "  .globl main\n");
  fprintf(out, "main:\n");
  fprintf(out, "  push %%rbp\n");
  fprintf(out, "  mov %%rsp, %%rbp\n");
  gen_expr(node);
  fprintf(out, "  pop %%rbp\n");
  fprintf(out, "  ret\n");
}
