#include "ncclcc.h"

Obj *locals;

static Node *new_node(NodeKind kind) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr) {
  Node *node = new_node(kind);
  node->lhs = expr;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs) {
  Node *node = new_node(kind);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

static Node *new_num(long val) {
  Node *node = new_node(ND_NUM);
  node->val = val;
  return node;
}

static Node *new_var_node(Obj *var) {
  Node *node = new_node(ND_LVAR);
  node->var = var;
  return node;
}

static char *token_str(Token *tok) {
  char *s = calloc(tok->len + 1, 1);
  memcpy(s, tok->loc, tok->len);
  return s;
}

static int local_count;

// Finds a local variable by name, or NULL.
static Obj *find_lvar(Token *tok) {
  for (Obj *var = locals; var; var = var->next)
    if (var->len == tok->len && !memcmp(tok->loc, var->name, tok->len))
      return var;
  return NULL;
}

// Declares a new local variable.
static Obj *new_lvar(Token *tok) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = token_str(tok);
  var->len = tok->len;
  var->offset = 8 * ++local_count;
  var->next = locals;
  locals = var;
  return var;
}

static Node *expr(Token **rest, Token *tok);
static Node *logor(Token **rest, Token *tok);
static Node *logand(Token **rest, Token *tok);
static Node *bitor(Token **rest, Token *tok);
static Node *bitxor(Token **rest, Token *tok);
static Node *bitand(Token **rest, Token *tok);
static Node *equality(Token **rest, Token *tok);
static Node *relational(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);

// expr = logor
static Node *expr(Token **rest, Token *tok) {
  return logor(rest, tok);
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

// relational = add ("<" add | "<=" add | ">" add | ">=" add)*
static Node *relational(Token **rest, Token *tok) {
  Node *node = add(&tok, tok);
  for (;;) {
    if (equal(tok, "<"))
      node = new_binary(ND_LT, node, add(&tok, tok->next));
    else if (equal(tok, "<="))
      node = new_binary(ND_LE, node, add(&tok, tok->next));
    else if (equal(tok, ">"))
      node = new_binary(ND_LT, add(&tok, tok->next), node);
    else if (equal(tok, ">="))
      node = new_binary(ND_LE, add(&tok, tok->next), node);
    else
      break;
  }
  *rest = tok;
  return node;
}

// add = mul ("+" mul | "-" mul)*
static Node *add(Token **rest, Token *tok) {
  Node *node = mul(&tok, tok);
  for (;;) {
    if (equal(tok, "+"))
      node = new_binary(ND_ADD, node, mul(&tok, tok->next));
    else if (equal(tok, "-"))
      node = new_binary(ND_SUB, node, mul(&tok, tok->next));
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

// unary = ("+" | "-" | "!" | "~") unary | primary
static Node *unary(Token **rest, Token *tok) {
  if (equal(tok, "+"))
    return unary(rest, tok->next);
  if (equal(tok, "-"))
    return new_unary(ND_NEG, unary(rest, tok->next));
  if (equal(tok, "!"))
    return new_unary(ND_NOT, unary(rest, tok->next));
  if (equal(tok, "~"))
    return new_unary(ND_BITNOT, unary(rest, tok->next));
  return primary(rest, tok);
}

// primary = "(" expr ")" | ident | num
static Node *primary(Token **rest, Token *tok) {
  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }

  if (tok->kind == TK_IDENT) {
    Obj *var = find_lvar(tok);
    if (!var)
      error_at(tok->loc, "undefined variable");
    *rest = tok->next;
    return new_var_node(var);
  }

  if (tok->kind == TK_NUM) {
    Node *node = new_num(tok->val);
    *rest = tok->next;
    return node;
  }

  error_at(tok->loc, "expected an expression");
}

// decl = "int" ident ("=" expr)?
static Node *decl(Token **rest, Token *tok) {
  tok = skip(tok, "int");
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a variable name");
  Obj *var = new_lvar(tok);
  tok = tok->next;

  if (equal(tok, "=")) {
    Node *lhs = new_var_node(var);
    Node *rhs = expr(&tok, tok->next);
    *rest = skip(tok, ";");
    Node *node = new_node(ND_EXPR_STMT);
    node->lhs = new_binary(ND_ASSIGN, lhs, rhs);
    return node;
  }

  *rest = skip(tok, ";");
  return NULL;
}

// stmt = "return" expr ";"
//      | decl
//      | expr ";"
static Node *stmt(Token **rest, Token *tok) {
  if (equal(tok, "return")) {
    Node *node = new_node(ND_RETURN);
    node->lhs = expr(&tok, tok->next);
    *rest = skip(tok, ";");
    return node;
  }

  if (equal(tok, "int"))
    return decl(rest, tok);

  Node *node = new_node(ND_EXPR_STMT);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

// program = "int" "main" "(" ")" "{" stmt* "}"
Node *parse(Token *tok) {
  tok = skip(tok, "int");
  tok = skip(tok, "main");
  tok = skip(tok, "(");
  tok = skip(tok, ")");
  tok = skip(tok, "{");

  Node head = {0};
  Node *cur = &head;
  while (!equal(tok, "}")) {
    Node *node = stmt(&tok, tok);
    if (node) {
      cur = cur->next = node;
    }
  }
  skip(tok, "}");
  return head.next;
}
