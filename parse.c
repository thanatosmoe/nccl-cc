#include "ncclcc.h"

// expr = num
static Node *expr(Token **rest, Token *tok) {
  if (tok->kind != TK_NUM)
    error_at(tok->loc, "expected a number");

  Node *node = calloc(1, sizeof(Node));
  node->kind = ND_NUM;
  node->val = tok->val;
  *rest = tok->next;
  return node;
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
