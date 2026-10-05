#include "ncclcc.h"

static FILE *out;
static int depth;

static int count(void) {
  static int i = 1;
  return i++;
}

static void push(void) {
  fprintf(out, "  push %%rax\n");
  depth++;
}

static void pop(char *arg) {
  fprintf(out, "  pop %s\n", arg);
  depth--;
}

// Emits "<setcc> %%al" followed by zero-extension into rax.
static void emit_cmp(char *setcc) {
  fprintf(out, "  cmp %%rdi, %%rax\n");
  fprintf(out, "  %s %%al\n", setcc);
  fprintf(out, "  movzbl %%al, %%eax\n");
}

static void gen_expr(Node *node);
static void gen_stmt(Node *node);

// Computes the address of an lvalue into rax.
static void gen_addr(Node *node) {
  if (node->kind == ND_VAR) {
    if (node->var->is_global)
      fprintf(out, "  lea %s(%%rip), %%rax\n", node->var->name);
    else
      fprintf(out, "  lea -%d(%%rbp), %%rax\n", node->var->offset);
    return;
  }
  error("not an lvalue");
}

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NUM:
    fprintf(out, "  mov $%ld, %%rax\n", node->val);
    return;
  case ND_VAR:
    gen_addr(node);
    fprintf(out, "  mov (%%rax), %%rax\n");
    return;
  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    pop("%rdi");
    fprintf(out, "  mov %%rax, (%%rdi)\n");
    return;
  case ND_NEG:
    gen_expr(node->lhs);
    fprintf(out, "  neg %%rax\n");
    return;
  case ND_NOT:
    gen_expr(node->lhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  sete %%al\n");
    fprintf(out, "  movzbl %%al, %%eax\n");
    return;
  case ND_BITNOT:
    gen_expr(node->lhs);
    fprintf(out, "  not %%rax\n");
    return;
  case ND_LOGAND: {
    int c = count();
    gen_expr(node->lhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  je .L.false.%d\n", c);
    gen_expr(node->rhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  je .L.false.%d\n", c);
    fprintf(out, "  mov $1, %%rax\n");
    fprintf(out, "  jmp .L.end.%d\n", c);
    fprintf(out, ".L.false.%d:\n", c);
    fprintf(out, "  mov $0, %%rax\n");
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_LOGOR: {
    int c = count();
    gen_expr(node->lhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  jne .L.true.%d\n", c);
    gen_expr(node->rhs);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  jne .L.true.%d\n", c);
    fprintf(out, "  mov $0, %%rax\n");
    fprintf(out, "  jmp .L.end.%d\n", c);
    fprintf(out, ".L.true.%d:\n", c);
    fprintf(out, "  mov $1, %%rax\n");
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
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
  case ND_BITAND:
    fprintf(out, "  and %%rdi, %%rax\n");
    return;
  case ND_BITOR:
    fprintf(out, "  or %%rdi, %%rax\n");
    return;
  case ND_BITXOR:
    fprintf(out, "  xor %%rdi, %%rax\n");
    return;
  case ND_EQ:
    emit_cmp("sete");
    return;
  case ND_NE:
    emit_cmp("setne");
    return;
  case ND_LT:
    emit_cmp("setl");
    return;
  case ND_LE:
    emit_cmp("setle");
    return;
  default:
    break;
  }

  error("invalid expression");
}

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_RETURN:
    gen_expr(node->lhs);
    fprintf(out, "  jmp .L.return.main\n");
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  default:
    break;
  }
  error("invalid statement");
}

void codegen(Node *node, FILE *outfile) {
  out = outfile;

  int nlocals = 0;
  for (Obj *var = locals; var; var = var->next)
    nlocals++;

  // Keep the stack 16-byte aligned at every call site.
  int frame = (nlocals * 8 + 15) / 16 * 16;

  if (globals) {
    fprintf(out, "  .data\n");
    for (Obj *var = globals; var; var = var->next) {
      fprintf(out, "  .globl %s\n", var->name);
      fprintf(out, "%s:\n", var->name);
      fprintf(out, "  .quad %ld\n", var->has_init ? var->init_val : 0);
    }
  }

  fprintf(out, "  .text\n");
  fprintf(out, "  .globl main\n");
  fprintf(out, "main:\n");
  fprintf(out, "  push %%rbp\n");
  fprintf(out, "  mov %%rsp, %%rbp\n");
  if (frame)
    fprintf(out, "  sub $%d, %%rsp\n", frame);

  for (Node *n = node; n; n = n->next)
    gen_stmt(n);

  fprintf(out, ".L.return.main:\n");
  fprintf(out, "  mov %%rbp, %%rsp\n");
  fprintf(out, "  pop %%rbp\n");
  fprintf(out, "  ret\n");
}
