#include "ncclcc.h"

static FILE *out;
static int depth;

#define MAX_LOOP 256
static int brk_labels[MAX_LOOP];
static int cont_labels[MAX_LOOP];
static int loop_depth;

static char *current_fn;

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
  if (node->kind == ND_DEREF) {
    gen_expr(node->lhs);
    return;
  }
  error("not an lvalue");
}

// Generates a function call following the Microsoft x64 calling convention:
// the first four integer arguments go in rcx/rdx/r8/r9, the rest on the
// stack; the caller reserves 32 bytes of shadow space and keeps rsp 16-byte
// aligned at the call.
static void gen_funcall(Node *node) {
  int nargs = 0;
  for (Node *arg = node->args; arg; arg = arg->next)
    nargs++;

  int num_stack = nargs > 4 ? nargs - 4 : 0;
  int total = 32 + 8 * num_stack;
  if ((depth + num_stack) % 2)
    total += 8;
  fprintf(out, "  sub $%d, %%rsp\n", total);
  depth += total / 8;

  int i = 0;
  for (Node *arg = node->args; arg; arg = arg->next) {
    gen_expr(arg);
    if (i < 4)
      fprintf(out, "  mov %%rax, %d(%%rsp)\n", 8 * i);
    else
      fprintf(out, "  mov %%rax, %d(%%rsp)\n", 32 + 8 * (i - 4));
    i++;
  }

  fprintf(out, "  mov 0(%%rsp), %%rcx\n");
  fprintf(out, "  mov 8(%%rsp), %%rdx\n");
  fprintf(out, "  mov 16(%%rsp), %%r8\n");
  fprintf(out, "  mov 24(%%rsp), %%r9\n");
  fprintf(out, "  xor %%eax, %%eax\n");
  fprintf(out, "  call %s\n", node->name);
  fprintf(out, "  add $%d, %%rsp\n", total);

  depth -= total / 8;
}

static void gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NUM:
    fprintf(out, "  mov $%ld, %%rax\n", node->val);
    return;
  case ND_VAR:
    gen_addr(node);
    if (node->var->ty->kind == TY_ARRAY)
      return; // Arrays evaluate to their address.
    if (node->var->ty->size == 1)
      fprintf(out, "  movsbl (%%rax), %%eax\n");
    else
      fprintf(out, "  mov (%%rax), %%rax\n");
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    if (node->ty->kind == TY_ARRAY)
      return; // Address of the first element.
    if (node->ty->size == 1)
      fprintf(out, "  movsbl (%%rax), %%eax\n");
    else
      fprintf(out, "  mov (%%rax), %%rax\n");
    return;
  case ND_STR:
    fprintf(out, "  lea .L.str.%d(%%rip), %%rax\n", node->str->id);
    return;
  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    pop("%rdi");
    if (node->lhs->ty->size == 1)
      fprintf(out, "  mov %%al, (%%rdi)\n");
    else
      fprintf(out, "  mov %%rax, (%%rdi)\n");
    return;
  case ND_FUNCALL:
    gen_funcall(node);
    return;
  case ND_COND: {
    int c = count();
    gen_expr(node->cond);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  je .L.else.%d\n", c);
    gen_expr(node->then);
    fprintf(out, "  jmp .L.end.%d\n", c);
    fprintf(out, ".L.else.%d:\n", c);
    gen_expr(node->els);
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
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
    fprintf(out, "  jmp .L.return.%s\n", current_fn);
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  case ND_IF: {
    int c = count();
    gen_expr(node->cond);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  je .L.else.%d\n", c);
    gen_stmt(node->then);
    fprintf(out, "  jmp .L.end.%d\n", c);
    fprintf(out, ".L.else.%d:\n", c);
    if (node->els)
      gen_stmt(node->els);
    fprintf(out, ".L.end.%d:\n", c);
    return;
  }
  case ND_FOR: {
    int c = count();
    if (node->init)
      gen_stmt(node->init);
    if (loop_depth >= MAX_LOOP)
      error("loop nesting too deep");
    brk_labels[loop_depth] = c;
    cont_labels[loop_depth] = c;
    loop_depth++;

    fprintf(out, ".L.begin.%d:\n", c);
    if (node->cond) {
      gen_expr(node->cond);
      fprintf(out, "  cmp $0, %%rax\n");
      fprintf(out, "  je .L.end.%d\n", c);
    }
    gen_stmt(node->then);
    fprintf(out, ".L.continue.%d:\n", c);
    if (node->inc)
      gen_stmt(node->inc);
    fprintf(out, "  jmp .L.begin.%d\n", c);
    fprintf(out, ".L.end.%d:\n", c);

    loop_depth--;
    return;
  }
  case ND_BREAK:
    if (loop_depth == 0)
      error("stray break");
    fprintf(out, "  jmp .L.end.%d\n", brk_labels[loop_depth - 1]);
    return;
  case ND_CONTINUE:
    if (loop_depth == 0)
      error("stray continue");
    fprintf(out, "  jmp .L.continue.%d\n", cont_labels[loop_depth - 1]);
    return;
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  default:
    break;
  }
  error("invalid statement");
}

static void gen_function(Function *fn) {
  current_fn = fn->name;
  depth = 0;
  loop_depth = 0;

  fprintf(out, "  .text\n");
  fprintf(out, "  .globl %s\n", fn->name);
  fprintf(out, "%s:\n", fn->name);
  fprintf(out, "  push %%rbp\n");
  fprintf(out, "  mov %%rsp, %%rbp\n");

  int frame = (fn->stack_size + 15) / 16 * 16;
  if (frame)
    fprintf(out, "  sub $%d, %%rsp\n", frame);

  // Copy incoming arguments into their stack slots.
  static char *argreg[] = {"%rcx", "%rdx", "%r8", "%r9"};
  static char *argreg8[] = {"%cl", "%dl", "%r8b", "%r9b"};
  int i = 0;
  for (Obj *var = fn->params; var; var = var->next) {
    int size = var->ty->size;
    if (i < 4) {
      if (size == 1)
        fprintf(out, "  mov %s, -%d(%%rbp)\n", argreg8[i], var->offset);
      else
        fprintf(out, "  mov %s, -%d(%%rbp)\n", argreg[i], var->offset);
    } else {
      // [rbp+0]=saved rbp, [rbp+8]=return address, [rbp+16..47]=shadow space,
      // so the 5th argument starts at [rbp+48].
      fprintf(out, "  mov %d(%%rbp), %%rax\n", 48 + 8 * (i - 4));
      if (size == 1)
        fprintf(out, "  mov %%al, -%d(%%rbp)\n", var->offset);
      else
        fprintf(out, "  mov %%rax, -%d(%%rbp)\n", var->offset);
    }
    i++;
  }

  for (Node *n = fn->body; n; n = n->next)
    gen_stmt(n);

  fprintf(out, ".L.return.%s:\n", fn->name);
  fprintf(out, "  mov %%rbp, %%rsp\n");
  fprintf(out, "  pop %%rbp\n");
  fprintf(out, "  ret\n");
}

void codegen(Function *prog, FILE *outfile) {
  out = outfile;

  if (globals || strings) {
    fprintf(out, "  .data\n");

    for (Obj *var = globals; var; var = var->next) {
      fprintf(out, "  .globl %s\n", var->name);
      fprintf(out, "%s:\n", var->name);
      if (var->ty->kind == TY_ARRAY)
        fprintf(out, "  .zero %d\n", var->ty->size);
      else if (var->ty->size == 1)
        fprintf(out, "  .byte %ld\n", var->has_init ? var->init_val : 0);
      else
        fprintf(out, "  .quad %ld\n", var->has_init ? var->init_val : 0);
    }

    for (StringLit *s = strings; s; s = s->next) {
      fprintf(out, ".L.str.%d:\n", s->id);
      for (int i = 0; i <= s->len; i++)
        fprintf(out, "  .byte %d\n", i < s->len ? (unsigned char)s->data[i] : 0);
    }
  }

  for (Function *fn = prog; fn; fn = fn->next)
    gen_function(fn);
}
