#include "ncclcc.h"

static FILE *out;
static int depth;

static void push(void) {
  fprintf(out, "  push %%rax\n");
  depth++;
}

static void pop(char *arg) {
  fprintf(out, "  pop %s\n", arg);
  depth--;
}

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
  default:
    break;
  }

  // Binary operators. Evaluate rhs first, keep it on the stack, then lhs.
  gen_expr(node->rhs);
  push();
  gen_expr(node->lhs);
  pop("%rdi");

  switch (node->kind) {
  case ND_ADD:
    fprintf(out, "  add %%rdi, %%rax\n");
    return;
  case ND_SUB:
    fprintf(out, "  sub %%rdi, %%rax\n");
    return;
  case ND_MUL:
    fprintf(out, "  imul %%rdi, %%rax\n");
    return;
  case ND_DIV:
    fprintf(out, "  cqo\n");
    fprintf(out, "  idiv %%rdi\n");
    return;
  case ND_MOD:
    fprintf(out, "  cqo\n");
    fprintf(out, "  idiv %%rdi\n");
    fprintf(out, "  mov %%rdx, %%rax\n");
    return;
  default:
    break;
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
