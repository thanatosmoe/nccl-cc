#include "ncclcc.h"

static FILE *out;

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NUM:
    fprintf(out, "  mov $%ld, %%rax\n", node->val);
    return;
  case ND_NEG:
    gen_expr(node->lhs);
    fprintf(out, "  neg %%rax\n");
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    fprintf(out, "  not %%rax\n");
    return;
  case ND_NOT:
    gen_expr(node->lhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  sete %%al\n");
    fprintf(out, "  movzbl %%al, %%eax\n");
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
