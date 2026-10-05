#include "ncclcc.h"

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

static Node *expr(Token **rest, Token *tok);
static Node *add(Token **rest, Token *tok);
static Node *mul(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);

// expr = add
static Node *expr(Token **rest, Token *tok) {
  return add(rest, tok);
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

// primary = "(" expr ")" | num
static Node *primary(Token **rest, Token *tok) {
  if (equal(tok, "(")) {
    Node *node = expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return node;
  }
  if (tok->kind == TK_NUM) {
    Node *node = new_num(tok->val);
    *rest = tok->next;
    return node;
  }
  error_at(tok->loc, "expected an expression");
}

// stmt = "return" expr ";"
static Node *stmt(Token **rest, Token *tok) {
  tok = skip(tok, "return");
  Node *node = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

// program = "int" "main" "(" ")" "{" stmt "}"
Node *parse(Token *tok) {
  tok = skip(tok, "int");
  tok = skip(tok, "main");
  tok = skip(tok, "(");
  tok = skip(tok, ")");
  tok = skip(tok, "{");
  Node *node = stmt(&tok, tok);
  skip(tok, "}");
  return node;
}
