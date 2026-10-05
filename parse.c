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
  Node *node = new_node(ND_VAR);
  node->var = var;
  return node;
}

static char *token_str(Token *tok) {
  char *s = calloc(tok->len + 1, 1);
  memcpy(s, tok->loc, tok->len);
  return s;
}

static int local_count;

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

// Declares a new global variable.
static Obj *new_gvar(Token *tok) {
  Obj *var = calloc(1, sizeof(Obj));
  var->name = token_str(tok);
  var->len = tok->len;
  var->is_global = true;
  var->next = globals;
  globals = var;
  return var;
}

static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
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
static Node *declaration(Token **rest, Token *tok);

// expr = assign
static Node *expr(Token **rest, Token *tok) {
  return assign(rest, tok);
}

// assign = conditional ("=" assign)?
static Node *assign(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);
  if (equal(tok, "="))
    return new_binary(ND_ASSIGN, node, assign(rest, tok->next));
  *rest = tok;
  return node;
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
    Obj *var = find_var(tok);
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

// global_decl = "int" ident ("=" num)?
static void global_decl(Token **rest, Token *tok) {
  tok = skip(tok, "int");
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a variable name");

  Obj *var = new_gvar(tok);
  tok = tok->next;

  if (equal(tok, "=")) {
    Node *node = expr(&tok, tok->next);
    if (node->kind != ND_NUM)
      error_at(tok->loc, "global initializer must be a constant");
    var->init_val = node->val;
    var->has_init = true;
  }

  *rest = skip(tok, ";");
}

// declaration = "int" ident ("=" expr)?
static Node *declaration(Token **rest, Token *tok) {
  tok = skip(tok, "int");
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a variable name");

  Obj *var = new_lvar(tok);
  tok = tok->next;

  if (equal(tok, "=")) {
    Node *lhs = new_var_node(var);
    Node *rhs = expr(&tok, tok->next);
    *rest = tok;
    Node *node = new_node(ND_EXPR_STMT);
    node->lhs = new_binary(ND_ASSIGN, lhs, rhs);
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
    if (equal(tok, "int")) {
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

  if (equal(tok, "int")) {
    Node *node = declaration(&tok, tok);
    *rest = skip(tok, ";");
    return node;
  }

  Node *node = new_node(ND_EXPR_STMT);
  node->lhs = expr(&tok, tok);
  *rest = skip(tok, ";");
  return node;
}

static bool is_function(Token *tok) {
  return equal(tok, "int") && tok->next->kind == TK_IDENT &&
         equal(tok->next->next, "(");
}

// program = global_decl* "int" "main" "(" ")" "{" stmt* "}"
Node *parse(Token *tok) {
  while (equal(tok, "int") && !is_function(tok))
    global_decl(&tok, tok);

  tok = skip(tok, "int");
  tok = skip(tok, "main");
  tok = skip(tok, "(");
  tok = skip(tok, ")");
  tok = skip(tok, "{");

  Node head = {0};
  Node *cur = &head;
  while (!equal(tok, "}")) {
    Node *node = stmt(&tok, tok);
    if (node)
      cur = cur->next = node;
  }
  skip(tok, "}");
  return head.next;
}
