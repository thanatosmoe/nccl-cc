#include "ncclcc.h"

static FILE *out;
static int depth;

#define MAX_LOOP 256
static int brk_labels[MAX_LOOP];
static int cont_labels[MAX_LOOP];
static int brk_depth;
static int cont_depth;

static char *current_fn;

static int count(void) {
  static int i = 1;
  return i++;
}

// Collects the case labels of a switch body and assigns each a unique label.
static void collect_cases(Node *node, Node ***arr, int *n, int *default_label) {
  if (!node)
    return;

  switch (node->kind) {
  case ND_CASE: {
    bool is_default = (node->label == -1);
    node->label = count();
    if (is_default)
      *default_label = node->label;
    *arr = realloc(*arr, sizeof(Node *) * (*n + 1));
    (*arr)[(*n)++] = node;
    collect_cases(node->lhs, arr, n, default_label);
    return;
  }
  case ND_BLOCK:
    for (Node *x = node->body; x; x = x->next)
      collect_cases(x, arr, n, default_label);
    return;
  default:
    return;
  }
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

// Loads a value of type `ty` from the address in rax.
static void load(Type *ty) {
  if (ty->kind == TY_ARRAY || ty->kind == TY_STRUCT)
    return; // Aggregates evaluate to their address.
  if (ty->size == 1)
    fprintf(out, "  movsbq (%%rax), %%rax\n");
  else
    fprintf(out, "  mov (%%rax), %%rax\n");
}

// Stores rax into the address in rdi, according to `ty`.
static void store(Type *ty) {
  if (ty->kind == TY_STRUCT) {
    int sz = ty->size;
    int i = 0;
    for (; i + 8 <= sz; i += 8) {
      fprintf(out, "  mov %d(%%rax), %%rcx\n", i);
      fprintf(out, "  mov %%rcx, %d(%%rdi)\n", i);
    }
    for (; i < sz; i++) {
      fprintf(out, "  movzbl %d(%%rax), %%ecx\n", i);
      fprintf(out, "  mov %%cl, %d(%%rdi)\n", i);
    }
    return;
  }
  if (ty->size == 1)
    fprintf(out, "  mov %%al, (%%rdi)\n");
  else
    fprintf(out, "  mov %%rax, (%%rdi)\n");
}

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
  if (node->kind == ND_MEMBER) {
    gen_addr(node->lhs);
    if (node->member->offset)
      fprintf(out, "  add $%d, %%rax\n", node->member->offset);
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
    load(node->var->ty);
    return;
  case ND_MEMBER:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;
  case ND_STR:
    fprintf(out, "  lea .L.str.%d(%%rip), %%rax\n", node->str->id);
    return;
  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    pop("%rdi");
    store(node->lhs->ty);
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
  case ND_CAST:
    gen_expr(node->lhs);
    if (node->ty->size == 1)
      fprintf(out, "  movsbq %%al, %%rax\n");
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_expr(node->rhs);
    return;
  case ND_PRE_INC:
  case ND_PRE_DEC: {
    int step = node->lhs->ty->base ? node->lhs->ty->base->size : 1;
    gen_addr(node->lhs);
    push();
    load(node->lhs->ty);
    fprintf(out, node->kind == ND_PRE_INC ? "  add $%d, %%rax\n" : "  sub $%d, %%rax\n",
            step);
    pop("%rdi");
    store(node->lhs->ty);
    return;
  }
  case ND_POST_INC:
  case ND_POST_DEC: {
    int step = node->lhs->ty->base ? node->lhs->ty->base->size : 1;
    gen_addr(node->lhs);
    push();
    load(node->lhs->ty);
    fprintf(out, "  mov %%rax, %%rcx\n");
    fprintf(out, node->kind == ND_POST_INC ? "  add $%d, %%rax\n" : "  sub $%d, %%rax\n",
            step);
    pop("%rdi");
    store(node->lhs->ty);
    fprintf(out, "  mov %%rcx, %%rax\n");
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
  case ND_SHL:
    fprintf(out, "  mov %%rdi, %%rcx\n");
    fprintf(out, "  shl %%cl, %%rax\n");
    return;
  case ND_SHR:
    fprintf(out, "  mov %%rdi, %%rcx\n");
    fprintf(out, "  sar %%cl, %%rax\n");
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
    if (brk_depth >= MAX_LOOP || cont_depth >= MAX_LOOP)
      error("loop nesting too deep");
    brk_labels[brk_depth++] = c;
    cont_labels[cont_depth++] = c;

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

    brk_depth--;
    cont_depth--;
    return;
  }
  case ND_DO: {
    int c = count();
    if (brk_depth >= MAX_LOOP || cont_depth >= MAX_LOOP)
      error("loop nesting too deep");
    brk_labels[brk_depth++] = c;
    cont_labels[cont_depth++] = c;

    fprintf(out, ".L.begin.%d:\n", c);
    gen_stmt(node->then);
    fprintf(out, ".L.continue.%d:\n", c);
    gen_expr(node->cond);
    fprintf(out, "  cmp $0, %%rax\n");
    fprintf(out, "  jne .L.begin.%d\n", c);
    fprintf(out, ".L.end.%d:\n", c);

    brk_depth--;
    cont_depth--;
    return;
  }
  case ND_SWITCH: {
    Node **cases = NULL;
    int ncases = 0;
    int default_label = -1;
    collect_cases(node->then, &cases, &ncases, &default_label);

    int c = count();
    if (brk_depth >= MAX_LOOP)
      error("switch nesting too deep");
    brk_labels[brk_depth++] = c;

    gen_expr(node->cond);
    for (int i = 0; i < ncases; i++) {
      if (cases[i]->label == default_label)
        continue;
      fprintf(out, "  cmp $%ld, %%rax\n", cases[i]->val);
      fprintf(out, "  je .L.case.%d\n", cases[i]->label);
    }
    if (default_label >= 0)
      fprintf(out, "  jmp .L.case.%d\n", default_label);
    else
      fprintf(out, "  jmp .L.end.%d\n", c);

    gen_stmt(node->then);
    fprintf(out, ".L.end.%d:\n", c);

    brk_depth--;
    free(cases);
    return;
  }
  case ND_CASE:
    fprintf(out, ".L.case.%d:\n", node->label);
    gen_stmt(node->lhs);
    return;
  case ND_BREAK:
    if (brk_depth == 0)
      error("stray break");
    fprintf(out, "  jmp .L.end.%d\n", brk_labels[brk_depth - 1]);
    return;
  case ND_CONTINUE:
    if (cont_depth == 0)
      error("stray continue");
    fprintf(out, "  jmp .L.continue.%d\n", cont_labels[cont_depth - 1]);
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
  brk_depth = 0;
  cont_depth = 0;

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
      if (var->ty->kind == TY_ARRAY || var->ty->kind == TY_STRUCT)
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
