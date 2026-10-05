#include "ncclcc.h"

Obj *locals;
Obj *globals;

static Node *new_node(NodeKind kind) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr) {
  Node *node = new_node(kind);
  node->lhs = expr;
  node->ty = ty_int;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs) {
  Node *node = new_node(kind);
  node->lhs = lhs;
  node->rhs = rhs;
  node->ty = ty_int;
  return node;
}

static Node *new_num(long val) {
  Node *node = new_node(ND_NUM);
  node->val = val;
  node->ty = ty_int;
  return node;
}

static Node *new_var_node(Obj *var) {
  Node *node = new_node(ND_VAR);
  node->var = var;
  node->ty = var->ty;
  return node;
}

static char *token_str(Token *tok) {
  char *s = calloc(tok->len + 1, 1);
  memcpy(s, tok->loc, tok->len);
  return s;
}

static int align_of(Type *ty) {
  if (ty->kind == TY_ARRAY)
    return align_of(ty->base);
  if (ty->kind == TY_STRUCT)
    return 8; // Member types are char/int/pointer (alignments 1 or 8).
  return ty->size;
}

static int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

static int frame_size;

static bool same_name(Obj *var, Token *tok) {
  return var->len == tok->len && !memcmp(tok->loc, var->name, tok->len);
}

// Finds a variable by name (locals take precedence over globals).
static Obj *find_var(Token *tok) {
  for (Obj *var = locals; var; var = var->next)
    if (same_name(var, tok))
      return var;
  for (Obj *var = globals; var; var = var->next)
    if (same_name(var, tok))
      return var;
  return NULL;
}

// Allocates a variable without linking it into a list.
static Obj *new_var(Token *tok, Type *ty) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = token_str(tok);
  var->len = tok->len;
  var->ty = ty;
  frame_size = align_to(frame_size + ty->size, align_of(ty));
  var->offset = frame_size;
  return var;
}

// Declares a new local variable.
static Obj *new_lvar(Token *tok, Type *ty) {
  Obj *var = new_var(tok, ty);
  var->next = locals;
  locals = var;
  return var;
}

// Declares a new global variable.
static Obj *new_gvar(Token *tok, Type *ty) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = token_str(tok);
  var->len = tok->len;
  var->ty = ty;
  var->is_global = true;
  var->next = globals;
  globals = var;
  return var;
}

static Type *struct_tags;

typedef struct Typedef Typedef;
struct Typedef {
  Typedef *next;
  char *name;
  Type *ty;
};
static Typedef *typedefs;

typedef struct EnumConst EnumConst;
struct EnumConst {
  EnumConst *next;
  char *name;
  long val;
};
static EnumConst *enum_consts;

static bool is_typename(Token *tok);
static bool tok_eq_str(Token *tok, char *s);
static Node *conditional(Token **rest, Token *tok);
static Type *declspec(Token **rest, Token *tok);
static Type *declarator(Token **rest, Token *tok, Type *ty, Token **name);
static Type *struct_decl(Token **rest, Token *tok);
static Type *enum_decl(Token **rest, Token *tok);
static void typedef_decl(Token **rest, Token *tok);

// declspec = ("static"|"extern"|"const"|...) *
//            ( "char" | "int" | "long" | "short" | "unsigned" | "signed"
//              | struct-decl | enum-decl | ident )
static Type *declspec(Token **rest, Token *tok) {
  for (;;) {
    if (equal(tok, "static") || equal(tok, "extern") || equal(tok, "const") ||
        equal(tok, "inline") || equal(tok, "register") || equal(tok, "volatile"))
      tok = tok->next;
    else
      break;
  }

  if (equal(tok, "char")) {
    *rest = tok->next;
    return ty_char;
  }
  if (equal(tok, "int") || equal(tok, "void")) {
    *rest = tok->next;
    return ty_int;
  }
  if (equal(tok, "long") || equal(tok, "short") || equal(tok, "unsigned") ||
      equal(tok, "signed")) {
    tok = tok->next;
    if (equal(tok, "int"))
      tok = tok->next;
    *rest = tok;
    return ty_int;
  }
  if (equal(tok, "struct"))
    return struct_decl(rest, tok);
  if (equal(tok, "enum"))
    return enum_decl(rest, tok);

  for (Typedef *td = typedefs; td; td = td->next)
    if (tok_eq_str(tok, td->name)) {
      *rest = tok->next;
      return td->ty;
    }

  error_at(tok->loc, "expected a type name");
}

static bool find_enum(Token *tok, long *val) {
  for (EnumConst *e = enum_consts; e; e = e->next)
    if (tok_eq_str(tok, e->name)) {
      *val = e->val;
      return true;
    }
  return false;
}

static Type *pointer_to(Type *base) {
  Type *ty = calloc(1, sizeof(Type));
  ty->kind = TY_PTR;
  ty->size = 8;
  ty->base = base;
  return ty;
}

static Type *array_of(Type *base, int len) {
  Type *ty = calloc(1, sizeof(Type));
  ty->kind = TY_ARRAY;
  ty->size = base->size * len;
  ty->base = base;
  ty->array_len = len;
  return ty;
}

static bool is_integer(Type *ty) {
  return ty->kind == TY_CHAR || ty->kind == TY_INT;
}

// type-suffix = ("[" num "]")*
static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (equal(tok, "[")) {
    tok = tok->next;
    // An unsized array (`int a[]`) is only allowed for parameters, where it
    // decays to a pointer. Represent it as an array of length 0.
    if (equal(tok, "]")) {
      ty = type_suffix(rest, tok->next, ty);
      return array_of(ty, 0);
    }
    if (tok->kind != TK_NUM)
      error_at(tok->loc, "expected an array size");
    int len = tok->val;
    tok = skip(tok->next, "]");
    ty = type_suffix(rest, tok, ty);
    return array_of(ty, len);
  }
  *rest = tok;
  return ty;
}

// declarator = "*"* ident type-suffix
static Type *declarator(Token **rest, Token *tok, Type *ty, Token **name) {
  while (equal(tok, "*")) {
    ty = pointer_to(ty);
    tok = tok->next;
  }
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a variable name");
  *name = tok;
  tok = tok->next;
  return type_suffix(rest, tok, ty);
}

static bool tok_eq_str(Token *tok, char *s) {
  return tok->len == (int)strlen(s) && !strncmp(tok->loc, s, tok->len);
}

// struct-decl = "struct" ident ("{" (declspec declarator ("," declarator)* ";")* "}")?
static Type *struct_decl(Token **rest, Token *tok) {
  tok = tok->next; // Skip "struct"

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  if (!equal(tok, "{")) {
    if (!tag)
      error_at(tok->loc, "expected a struct tag name or '{'");
    for (Type *ty = struct_tags; ty; ty = ty->next)
      if (ty->tag && tok_eq_str(tag, ty->tag)) {
        *rest = tok;
        return ty;
      }
    error_at(tag->loc, "unknown struct tag");
  }

  Type *ty = calloc(1, sizeof(Type));
  ty->kind = TY_STRUCT;
  ty->tag = tag ? token_str(tag) : NULL;

  // Register the tag before parsing members so the struct can refer to itself.
  if (tag) {
    ty->next = struct_tags;
    struct_tags = ty;
  }

  Member head = {0};
  Member *cur = &head;
  tok = tok->next;
  while (!equal(tok, "}")) {
    Type *mty = declspec(&tok, tok);
    for (;;) {
      Token *name;
      Type *mt = declarator(&tok, tok, mty, &name);
      Member *m = calloc(1, sizeof(Member));
      m->ty = mt;
      m->name = token_str(name);
      cur = cur->next = m;
      if (equal(tok, ",")) {
        tok = tok->next;
        continue;
      }
      break;
    }
    tok = skip(tok, ";");
  }
  tok = tok->next; // Skip "}"

  int offset = 0;
  int max_align = 1;
  for (Member *m = head.next; m; m = m->next) {
    int align = align_of(m->ty);
    offset = align_to(offset, align);
    m->offset = offset;
    offset += m->ty->size;
    if (max_align < align)
      max_align = align;
  }
  ty->size = align_to(offset, max_align);
  ty->members = head.next;

  *rest = tok;
  return ty;
}

// Evaluates a constant integer expression (used for enum values).
static long const_eval(Node *node) {
  switch (node->kind) {
  case ND_NUM: return node->val;
  case ND_NEG: return -const_eval(node->lhs);
  case ND_NOT: return !const_eval(node->lhs);
  case ND_BITNOT: return ~const_eval(node->lhs);
  case ND_ADD: return const_eval(node->lhs) + const_eval(node->rhs);
  case ND_SUB: return const_eval(node->lhs) - const_eval(node->rhs);
  case ND_MUL: return const_eval(node->lhs) * const_eval(node->rhs);
  case ND_DIV: { long r = const_eval(node->rhs); return r ? const_eval(node->lhs) / r : 0; }
  case ND_MOD: { long r = const_eval(node->rhs); return r ? const_eval(node->lhs) % r : 0; }
  case ND_SHL: return const_eval(node->lhs) << const_eval(node->rhs);
  case ND_SHR: return const_eval(node->lhs) >> const_eval(node->rhs);
  case ND_BITAND: return const_eval(node->lhs) & const_eval(node->rhs);
  case ND_BITOR: return const_eval(node->lhs) | const_eval(node->rhs);
  case ND_BITXOR: return const_eval(node->lhs) ^ const_eval(node->rhs);
  default: error("not a constant expression");
  }
}

// enum-decl = "enum" ident? ("{" enumerator ("," enumerator)* ","? "}")?
static Type *enum_decl(Token **rest, Token *tok) {
  tok = tok->next; // Skip "enum"
  if (tok->kind == TK_IDENT)
    tok = tok->next; // Optional tag

  if (!equal(tok, "{")) {
    *rest = tok; // Reference to an enum tag; treat as int.
    return ty_int;
  }

  tok = tok->next;
  long val = 0;
  while (!equal(tok, "}")) {
    if (tok->kind != TK_IDENT)
      error_at(tok->loc, "expected an enumerator name");
    EnumConst *e = calloc(1, sizeof(EnumConst));
    e->name = token_str(tok);
    tok = tok->next;

    if (equal(tok, "=")) {
      Node *n = conditional(&tok, tok->next);
      val = const_eval(n);
    }
    e->val = val++;
    e->next = enum_consts;
    enum_consts = e;

    if (equal(tok, ",")) {
      tok = tok->next;
      if (equal(tok, "}"))
        break;
      continue;
    }
    break;
  }
  *rest = skip(tok, "}");
  return ty_int;
}

static void add_typedef(Token *name, Type *ty) {
  Typedef *td = calloc(1, sizeof(Typedef));
  td->name = token_str(name);
  td->ty = ty;
  td->next = typedefs;
  typedefs = td;
}

// typedef = "typedef" declspec declarator ("," declarator)* ";"
static void typedef_decl(Token **rest, Token *tok) {
  Type *base = declspec(&tok, tok->next);
  for (;;) {
    Token *name;
    Type *ty = declarator(&tok, tok, base, &name);
    add_typedef(name, ty);
    if (equal(tok, ",")) {
      tok = tok->next;
      continue;
    }
    break;
  }
  *rest = skip(tok, ";");
}

// Builds a member-access node `lhs.name`.
static Node *struct_ref(Node *lhs, Token *name) {
  if (lhs->ty->kind != TY_STRUCT)
    error_at(name->loc, "not a struct");

  for (Member *m = lhs->ty->members; m; m = m->next)
    if (tok_eq_str(name, m->name)) {
      Node *node = new_unary(ND_MEMBER, lhs);
      node->member = m;
      node->ty = m->ty;
      return node;
    }
  error_at(name->loc, "no such member");
}

static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *new_add(Node *lhs, Node *rhs);
static Node *new_sub(Node *lhs, Node *rhs);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *shift(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *declaration(Token **rest, Token *tok);

// expr = assign ("," assign)*
static Node *expr(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);
  while (equal(tok, ",")) {
    Node *rhs = assign(&tok, tok->next);
    Node *n = new_binary(ND_COMMA, node, rhs);
    n->ty = rhs->ty;
    node = n;
  }
  *rest = tok;
  return node;
}

// assign = conditional (("=" | "+=" | ...) assign)?
static Node *assign(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);

  if (equal(tok, "=")) {
    Node *as = new_binary(ND_ASSIGN, node, assign(rest, tok->next));
    as->ty = node->ty;
    return as;
  }

  NodeKind op;
  if (equal(tok, "+=")) op = ND_ADD;
  else if (equal(tok, "-=")) op = ND_SUB;
  else if (equal(tok, "*=")) op = ND_MUL;
  else if (equal(tok, "/=")) op = ND_DIV;
  else if (equal(tok, "%=")) op = ND_MOD;
  else if (equal(tok, "&=")) op = ND_BITAND;
  else if (equal(tok, "|=")) op = ND_BITOR;
  else if (equal(tok, "^=")) op = ND_BITXOR;
  else if (equal(tok, "<<=")) op = ND_SHL;
  else if (equal(tok, ">>=")) op = ND_SHR;
  else {
    *rest = tok;
    return node;
  }

  Node *rhs = assign(rest, tok->next);
  Node *val;
  if (op == ND_ADD)
    val = new_add(node, rhs);
  else if (op == ND_SUB)
    val = new_sub(node, rhs);
  else
    val = new_binary(op, node, rhs);
  Node *as = new_binary(ND_ASSIGN, node, val);
  as->ty = node->ty;
  return as;
}

// conditional = logor ("?" expr ":" conditional)?
static Node *conditional(Token **rest, Token *tok) {
  Node *node = logor(&tok, tok);
  if (equal(tok, "?")) {
    Node *cond = new_node(ND_COND);
    cond->cond = node;
    cond->then = expr(&tok, tok->next);
    tok = skip(tok, ":");
    cond->els = conditional(&tok, tok);
    cond->ty = cond->then->ty;
    *rest = tok;
    return cond;
  }
  *rest = tok;
  return node;
}

// logor = logand ("||" logand)*
static Node *logor(Token **rest, Token *tok) {
  Node *node = logand(&tok, tok);
  while (equal(tok, "||"))
    node = new_binary(ND_LOGOR, node, logand(&tok, tok->next));
  *rest = tok;
  return node;
}

// logand = bitor ("&&" bitor)*
static Node *logand(Token **rest, Token *tok) {
  Node *node = bitor(&tok, tok);
  while (equal(tok, "&&"))
    node = new_binary(ND_LOGAND, node, bitor(&tok, tok->next));
  *rest = tok;
  return node;
}

// bitor = bitxor ("|" bitxor)*
static Node *bitor(Token **rest, Token *tok) {
  Node *node = bitxor(&tok, tok);
  while (equal(tok, "|"))
    node = new_binary(ND_BITOR, node, bitxor(&tok, tok->next));
  *rest = tok;
  return node;
}

// bitxor = bitand ("^" bitand)*
static Node *bitxor(Token **rest, Token *tok) {
  Node *node = bitand(&tok, tok);
  while (equal(tok, "^"))
    node = new_binary(ND_BITXOR, node, bitand(&tok, tok->next));
  *rest = tok;
  return node;
}

// bitand = equality ("&" equality)*
static Node *bitand(Token **rest, Token *tok) {
  Node *node = equality(&tok, tok);
  while (equal(tok, "&"))
    node = new_binary(ND_BITAND, node, equality(&tok, tok->next));
  *rest = tok;
  return node;
}

// equality = relational ("==" relational | "!=" relational)*
static Node *equality(Token **rest, Token *tok) {
  Node *node = relational(&tok, tok);
  for (;;) {
    if (equal(tok, "=="))
      node = new_binary(ND_EQ, node, relational(&tok, tok->next));
    else if (equal(tok, "!="))
      node = new_binary(ND_NE, node, relational(&tok, tok->next));
    else
      break;
  }
  *rest = tok;
  return node;
}

// relational = shift ("<" shift | "<=" shift | ">" shift | ">=" shift)*
static Node *relational(Token **rest, Token *tok) {
  Node *node = shift(&tok, tok);
  for (;;) {
    if (equal(tok, "<"))
      node = new_binary(ND_LT, node, shift(&tok, tok->next));
    else if (equal(tok, "<="))
      node = new_binary(ND_LE, node, shift(&tok, tok->next));
    else if (equal(tok, ">"))
      node = new_binary(ND_LT, shift(&tok, tok->next), node);
    else if (equal(tok, ">="))
      node = new_binary(ND_LE, shift(&tok, tok->next), node);
    else
      break;
  }
  *rest = tok;
  return node;
}

// shift = add ("<<" add | ">>" add)*
static Node *shift(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);
  for (;;) {
    if (equal(tok, "<<"))
      node = new_binary(ND_SHL, node, add(&tok, tok->next));
    else if (equal(tok, ">>"))
      node = new_binary(ND_SHR, node, add(&tok, tok->next));
    else
      break;
  }
  *rest = tok;
  return node;
}

// Builds `lhs + rhs`, scaling the integer operand when one side is a pointer.
static Node *new_add(Node *lhs, Node *rhs) {
  if (is_integer(lhs->ty) && is_integer(rhs->ty)) {
    Node *node = new_binary(ND_ADD, lhs, rhs);
    node->ty = ty_int;
    return node;
  }
  if (lhs->ty->base && rhs->ty->base)
    error("invalid operands to '+'");

  // Canonicalize so that the pointer is on the left.
  if (!lhs->ty->base) {
    Node *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  Node *scaled = new_binary(ND_MUL, rhs, new_num(lhs->ty->base->size));
  scaled->ty = ty_int;
  Node *node = new_binary(ND_ADD, lhs, scaled);
  node->ty = pointer_to(lhs->ty->base);
  return node;
}

// Builds `lhs - rhs`, scaling pointers as needed.
static Node *new_sub(Node *lhs, Node *rhs) {
  if (is_integer(lhs->ty) && is_integer(rhs->ty)) {
    Node *node = new_binary(ND_SUB, lhs, rhs);
    node->ty = ty_int;
    return node;
  }

  if (lhs->ty->base && rhs->ty->base) {
    // Pointer difference: byte distance divided by the element size.
    Node *diff = new_binary(ND_SUB, lhs, rhs);
    diff->ty = ty_int;
    Node *node = new_binary(ND_DIV, diff, new_num(lhs->ty->base->size));
    node->ty = ty_int;
    return node;
  }

  Node *scaled = new_binary(ND_MUL, rhs, new_num(lhs->ty->base->size));
  scaled->ty = ty_int;
  Node *node = new_binary(ND_SUB, lhs, scaled);
  node->ty = pointer_to(lhs->ty->base);
  return node;
}

// add = mul ("+" mul | "-" mul)*
static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);
  for (;;) {
    if (equal(tok, "+"))
      node = new_add(node, mul(&tok, tok->next));
    else if (equal(tok, "-"))
      node = new_sub(node, mul(&tok, tok->next));
    else
      break;
  }
  *rest = tok;
  return node;
}

// mul = unary ("*" unary | "/" unary | "%" unary)*
static Node *mul(Token **rest, Token *tok) {
  Node *node = unary(&tok, tok);
  for (;;) {
    if (equal(tok, "*"))
      node = new_binary(ND_MUL, node, unary(&tok, tok->next));
    else if (equal(tok, "/"))
      node = new_binary(ND_DIV, node, unary(&tok, tok->next));
    else if (equal(tok, "%"))
      node = new_binary(ND_MOD, node, unary(&tok, tok->next));
    else
      break;
  }
  *rest = tok;
  return node;
}

// unary = ("+" | "-" | "!" | "~" | "&" | "*" | "++" | "--" | "sizeof") unary
//       | cast
static Node *unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return unary(rest, tok->next);
  if (equal(tok, "-")) {
    Node *node = new_unary(ND_NEG, unary(rest, tok->next));
    node->ty = ty_int;
    return node;
  }
  if (equal(tok, "!")) {
    Node *node = new_unary(ND_NOT, unary(rest, tok->next));
    node->ty = ty_int;
    return node;
  }
  if (equal(tok, "~")) {
    Node *node = new_unary(ND_BITNOT, unary(rest, tok->next));
    node->ty = ty_int;
    return node;
  }
  if (equal(tok, "&")) {
    Node *node = new_unary(ND_ADDR, unary(rest, tok->next));
    node->ty = pointer_to(node->lhs->ty);
    return node;
  }
  if (equal(tok, "*")) {
    Node *node = new_unary(ND_DEREF, unary(rest, tok->next));
    if (!node->lhs->ty->base)
      error_at(tok->loc, "invalid pointer dereference");
    node->ty = node->lhs->ty->base;
    return node;
  }
  if (equal(tok, "++")) {
    Node *node = new_unary(ND_PRE_INC, unary(rest, tok->next));
    node->ty = node->lhs->ty;
    return node;
  }
  if (equal(tok, "--")) {
    Node *node = new_unary(ND_PRE_DEC, unary(rest, tok->next));
    node->ty = node->lhs->ty;
    return node;
  }
  if (equal(tok, "sizeof")) {
    Token *t = tok->next;
    if (equal(t, "(") && is_typename(t->next)) {
      Type *ty = declspec(&t, t->next);
      *rest = skip(t, ")");
      return new_num(ty->size);
    }
    Node *operand = unary(&t, t);
    *rest = t;
    return new_num(operand->ty->size);
  }
  return cast(rest, tok);
}

// cast = "(" typename ")" cast | postfix
static Node *cast(Token **rest, Token *tok) {
  if (equal(tok, "(") && is_typename(tok->next)) {
    Type *ty = declspec(&tok, tok->next);
    tok = skip(tok, ")");
    Node *node = new_unary(ND_CAST, cast(rest, tok));
    node->ty = ty;
    return node;
  }
  return postfix(rest, tok);
}

// postfix = primary ("[" expr "]" | "." ident | "->" ident)*
static Node *postfix(Token **rest, Token *tok) {
  Node *node = primary(&tok, tok);

  for (;;) {
    if (equal(tok, "[")) {
      Node *idx = expr(&tok, tok->next);
      tok = skip(tok, "]");
      Node *add = new_add(node, idx);
      Node *deref = new_unary(ND_DEREF, add);
      deref->ty = add->ty->base;
      node = deref;
      continue;
    }

    if (equal(tok, ".")) {
      node = struct_ref(node, tok->next);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "->")) {
      Node *deref = new_unary(ND_DEREF, node);
      if (!node->ty->base)
        error_at(tok->loc, "not a pointer");
      deref->ty = node->ty->base;
      node = struct_ref(deref, tok->next);
      tok = tok->next->next;
      continue;
    }

    if (equal(tok, "++")) {
      Node *node2 = new_unary(ND_POST_INC, node);
      node2->ty = node->ty;
      node = node2;
      tok = tok->next;
      continue;
    }

    if (equal(tok, "--")) {
      Node *node2 = new_unary(ND_POST_DEC, node);
      node2->ty = node->ty;
      node = node2;
      tok = tok->next;
      continue;
    }

    break;
  }

  *rest = tok;
  return node;
}

// primary = "(" expr ")" | ident "(" (assign ("," assign)*)? ")" | ident | num
static Node *primary(Token **rest, Token *tok) {
  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT && equal(tok->next, "(")) {
    Node *node = new_node(ND_FUNCALL);
    node->name = token_str(tok);
    node->ty = ty_int;
    tok = tok->next->next;

    Node head = {0};
    Node *cur = &head;
    while (!equal(tok, ")")) {
      if (cur != &head)
        tok = skip(tok, ",");
      cur = cur->next = assign(&tok, tok);
    }
    node->args = head.next;
    *rest = skip(tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    long enum_val;
    if (find_enum(tok, &enum_val)) {
      *rest = tok->next;
      return new_num(enum_val);
    }
    Obj *var = find_var(tok);
    if (!var)
      error_at(tok->loc, "undefined variable");
    *rest = tok->next;
    return new_var_node(var);
  }

  if (tok->kind == TK_STR) {
    StringLit *s = calloc(1, sizeof(StringLit));
    s->data = tok->str;
    s->len = tok->str_len;
    s->id = str_count++;
    s->next = strings;
    strings = s;

    Node *node = new_node(ND_STR);
    node->str = s;
    node->ty = ty_int; // Address of the string (pointer not yet modeled)
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_NUM) {
    Node *node = new_num(tok->val);
    *rest = tok->next;
    return node;
  }

  error_at(tok->loc, "expected an expression");
}

// global_decl = declspec declarator ("=" num)?
static void global_decl(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok);
  if (equal(tok, ";")) { // Bare type declaration.
    *rest = tok->next;
    return;
  }
  Token *name;
  ty = declarator(&tok, tok, ty, &name);
  Obj *var = new_gvar(name, ty);

  if (equal(tok, "=")) {
    Node *node = expr(&tok, tok->next);
    if (node->kind != ND_NUM)
      error_at(tok->loc, "global initializer must be a constant");
    var->init_val = node->val;
    var->has_init = true;
  }

  *rest = skip(tok, ";");
}

// declaration = declspec declarator ("=" expr)?
static Node *declaration(Token **rest, Token *tok) {
  Type *ty = declspec(&tok, tok);
  if (equal(tok, ";")) { // Bare type declaration (e.g. a struct definition).
    *rest = tok;
    return NULL;
  }
  Token *name;
  ty = declarator(&tok, tok, ty, &name);
  Obj *var = new_lvar(name, ty);

  if (equal(tok, "=")) {
    if (ty->kind == TY_ARRAY)
      error_at(tok->loc, "array initializer is not supported yet");
    Node *lhs = new_var_node(var);
    Node *rhs = expr(&tok, tok->next);
    *rest = tok;
    Node *node = new_node(ND_EXPR_STMT);
    Node *as = new_binary(ND_ASSIGN, lhs, rhs);
    as->ty = ty;
    node->lhs = as;
    return node;
  }

  *rest = tok;
  return NULL;
}

// stmt = "return" expr ";"
//      | "if" "(" expr ")" stmt ("else" stmt)?
//      | "for" "(" ... ")" stmt
//      | "while" "(" expr ")" stmt
//      | "break" ";"
//      | "continue" ";"
//      | "{" stmt* "}"
//      | "int" ident ("=" expr)? ";"
//      | expr ";"
static Node *stmt(Token **rest, Token *tok) {
  if (equal(tok, ";")) {
    Node *node = new_node(ND_BLOCK); // empty statement
    *rest = tok->next;
    return node;
  }

  if (equal(tok, "typedef")) {
    typedef_decl(&tok, tok);
    *rest = tok;
    return NULL;
  }

  if (equal(tok, "do")) {
    Node *node = new_node(ND_DO);
    node->then = stmt(&tok, tok->next);
    tok = skip(tok, "while");
    tok = skip(tok, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
  }

  if (equal(tok, "switch")) {
    Node *node = new_node(ND_SWITCH);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  if (equal(tok, "case")) {
    Node *node = new_node(ND_CASE);
    Node *v = expr(&tok, tok->next);
    if (v->kind != ND_NUM)
      error_at(tok->loc, "case value must be a constant");
    node->val = v->val;
    tok = skip(tok, ":");
    node->lhs = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  if (equal(tok, "default")) {
    Node *node = new_node(ND_CASE);
    node->label = -1;
    tok = skip(tok->next, ":");
    node->lhs = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  if (equal(tok, "if")) {
    Node *node = new_node(ND_IF);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    if (equal(tok, "else"))
      node->els = stmt(&tok, tok->next);
    *rest = tok;
    return node;
  }

  if (equal(tok, "for")) {
    Node *node = new_node(ND_FOR);
    tok = skip(tok->next, "(");
    if (is_typename(tok)) {
      node->init = declaration(&tok, tok);
    } else if (!equal(tok, ";")) {
      Node *e = new_node(ND_EXPR_STMT);
      e->lhs = expr(&tok, tok);
      node->init = e;
    }
    tok = skip(tok, ";");
    if (!equal(tok, ";"))
      node->cond = expr(&tok, tok);
    tok = skip(tok, ";");
    if (!equal(tok, ")")) {
      Node *e = new_node(ND_EXPR_STMT);
      e->lhs = expr(&tok, tok);
      node->inc = e;
    }
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  if (equal(tok, "while")) {
    Node *node = new_node(ND_FOR);
    tok = skip(tok->next, "(");
    node->cond = expr(&tok, tok);
    tok = skip(tok, ")");
    node->then = stmt(&tok, tok);
    *rest = tok;
    return node;
  }

  if (equal(tok, "break")) {
    Node *node = new_node(ND_BREAK);
    *rest = skip(tok->next, ";");
    return node;
  }

  if (equal(tok, "continue")) {
    Node *node = new_node(ND_CONTINUE);
    *rest = skip(tok->next, ";");
    return node;
  }

  if (equal(tok, "{")) {
    Node *node = new_node(ND_BLOCK);
    Node head = {0};
    Node *cur = &head;
    tok = tok->next;
    while (!equal(tok, "}")) {
      Node *s = stmt(&tok, tok);
      if (s)
        cur = cur->next = s;
    }
    *rest = tok->next;
    node->body = head.next;
    return node;
  }

  if (equal(tok, "return")) {
    Node *node = new_node(ND_RETURN);
    node->lhs = expr(&tok, tok->next);
    *rest = skip(tok, ";");
    return node;
  }

  if (is_typename(tok)) {
    Node *node = declaration(&tok, tok);
    *rest = skip(tok, ";");
    return node;
  }

  Node *node = new_node(ND_EXPR_STMT);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

// Returns true if the token starts a type name.
static bool is_typename(Token *tok) {
  if (equal(tok, "int") || equal(tok, "char") || equal(tok, "struct") ||
      equal(tok, "enum") || equal(tok, "long") || equal(tok, "short") ||
      equal(tok, "unsigned") || equal(tok, "signed") || equal(tok, "void") ||
      equal(tok, "const") || equal(tok, "static") || equal(tok, "extern"))
    return true;

  for (Typedef *td = typedefs; td; td = td->next)
    if (tok_eq_str(tok, td->name))
      return true;
  return false;
}

static bool is_function(Token *tok) {
  while (equal(tok, "static") || equal(tok, "extern") || equal(tok, "const") ||
         equal(tok, "inline") || equal(tok, "volatile"))
    tok = tok->next;
  return is_typename(tok) && tok->next->kind == TK_IDENT &&
         equal(tok->next->next, "(");
}

// function = declspec ident "(" (param ("," param)*)? ")" "{" stmt* "}"
static Function *function(Token **rest, Token *tok) {
  Type *ret_ty = declspec(&tok, tok);

  Function *fn = calloc(1, sizeof(Function));
  fn->ty = ret_ty;
  fn->name = token_str(tok);
  tok = tok->next;
  tok = skip(tok, "(");

  locals = NULL;
  frame_size = 0;

  Obj head = {0};
  Obj *cur = &head;
  while (!equal(tok, ")")) {
    if (cur != &head)
      tok = skip(tok, ",");
    Type *pty = declspec(&tok, tok);
    Token *pname;
    pty = declarator(&tok, tok, pty, &pname);
    if (pty->kind == TY_ARRAY)
      pty = pointer_to(pty->base); // Array parameters decay to pointers.
    cur = cur->next = new_var(pname, pty);
  }
  fn->params = head.next;
  locals = head.next; // Parameters are the initial local variables.
  tok = tok->next;    // Skip ")"

  tok = skip(tok, "{");
  Node bhead = {0};
  Node *bcur = &bhead;
  while (!equal(tok, "}")) {
    Node *s = stmt(&tok, tok);
    if (s)
      bcur = bcur->next = s;
  }
  tok = tok->next; // Skip "}"

  fn->body = bhead.next;
  fn->locals = locals;
  fn->stack_size = frame_size;
  *rest = tok;
  return fn;
}

// program = (function | global_decl)*
Function *parse(Token *tok) {
  Function head = {0};
  Function *cur = &head;

  while (tok->kind != TK_EOF) {
    locals = NULL;
    frame_size = 0;
    if (equal(tok, "typedef"))
      typedef_decl(&tok, tok);
    else if (is_function(tok))
      cur = cur->next = function(&tok, tok);
    else
      global_decl(&tok, tok);
  }
  return head.next;
}
