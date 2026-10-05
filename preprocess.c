#include "ncclcc.h"

typedef struct MacroParam MacroParam;
struct MacroParam {
  MacroParam *next;
  char *name;
};

typedef struct Macro Macro;
struct Macro {
  Macro *next;
  char *name;
  bool is_objlike;
  MacroParam *params;
  Token *body;
};

typedef struct MacroArg MacroArg;
struct MacroArg {
  MacroArg *next;
  char *name;
  Token *tok; // NULL-terminated token list
};

static Macro *macros;

static bool cond_active[64];
static bool cond_ever[64];
static int cond_depth;

static bool cur_active(void) {
  for (int i = 0; i < cond_depth; i++)
    if (!cond_active[i])
      return false;
  return true;
}

static bool parent_active(int d) {
  for (int i = 0; i < d; i++)
    if (!cond_active[i])
      return false;
  return true;
}

static bool is_hash(Token *tok) {
  return tok->kind == TK_PUNCT && tok->len == 1 && *tok->loc == '#';
}

static Token *copy_token(Token *tok) {
  Token *t = calloc(1, sizeof(Token));
  *t = *tok;
  t->next = NULL;
  return t;
}

static Token *copy_list(Token *tok) {
  Token head = {0};
  Token *cur = &head;
  for (; tok; tok = tok->next)
    cur = cur->next = copy_token(tok);
  cur->next = NULL;
  return head.next;
}

static Token *append_tokens(Token *list, Token *rest) {
  if (!list)
    return rest;
  Token *end = list;
  while (end->next)
    end = end->next;
  end->next = rest;
  return list;
}

static Macro *find_macro_str(Token *tok) {
  for (Macro *m = macros; m; m = m->next)
    if ((int)strlen(m->name) == tok->len && !strncmp(m->name, tok->loc, tok->len))
      return m;
  return NULL;
}

// Advances to the first token of the next line.
static Token *skip_line(Token *tok) {
  while (tok->kind != TK_EOF && !tok->at_bol)
    tok = tok->next;
  return tok;
}

static char *str_concat(char *a, char *b) {
  char *s = calloc(strlen(a) + strlen(b) + 1, 1);
  strcpy(s, a);
  strcat(s, b);
  return s;
}

static bool file_exists(char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  fclose(f);
  return true;
}

static char *search_include(char *fname) {
  for (int i = 0; i < include_path_count; i++) {
    char *path = str_concat(str_concat(include_paths[i], "/"), fname);
    if (file_exists(path))
      return path;
  }
  if (file_exists(fname))
    return fname;
  return NULL;
}

//
// Constant expression evaluation for #if / #elif
//

static long eval_expr(Token **rest, Token *tok);

static long eval_primary(Token **rest, Token *tok) {
  if (equal(tok, "(")) {
    long val = eval_expr(&tok, tok->next);
    *rest = skip(tok, ")");
    return val;
  }

  if (equal(tok, "defined")) {
    Token *name = tok->next;
    if (equal(name, "(")) {
      name = name->next;
      long val = find_macro_str(name) ? 1 : 0;
      Token *rparen = name->next;
      if (!equal(rparen, ")"))
        error_at(rparen->loc, "expected ')'");
      *rest = rparen->next;
      return val;
    }
    *rest = name->next;
    return find_macro_str(name) ? 1 : 0;
  }

  if (tok->kind == TK_NUM) {
    *rest = tok->next;
    return tok->val;
  }

  if (tok->kind == TK_IDENT || tok->kind == TK_KEYWORD) {
    Macro *m = find_macro_str(tok);
    *rest = tok->next;
    if (m && m->is_objlike) {
      // Evaluate the macro body as a sub-expression. Terminate it with an
      // EOF token so the evaluator never touches NULL.
      Token *body = copy_list(m->body);
      if (!body)
        return 0;
      Token *end = body;
      while (end->next)
        end = end->next;
      Token *eof = calloc(1, sizeof(Token));
      eof->kind = TK_EOF;
      end->next = eof;
      Token *dummy;
      return eval_expr(&dummy, body);
    }
    return 0;
  }

  error_at(tok->loc, "invalid #if expression");
}

static long eval_unary(Token **rest, Token *tok) {
  if (equal(tok, "!")) return !eval_unary(rest, tok->next);
  if (equal(tok, "-")) return -eval_unary(rest, tok->next);
  if (equal(tok, "+")) return eval_unary(rest, tok->next);
  if (equal(tok, "~")) return ~eval_unary(rest, tok->next);
  return eval_primary(rest, tok);
}

static long eval_mul(Token **rest, Token *tok) {
  long val = eval_unary(&tok, tok);
  for (;;) {
    if (equal(tok, "*")) val *= eval_unary(&tok, tok->next);
    else if (equal(tok, "/")) { long r = eval_unary(&tok, tok->next); val = r ? val / r : 0; }
    else if (equal(tok, "%")) { long r = eval_unary(&tok, tok->next); val = r ? val % r : 0; }
    else break;
  }
  *rest = tok;
  return val;
}

static long eval_add(Token **rest, Token *tok) {
  long val = eval_mul(&tok, tok);
  for (;;) {
    if (equal(tok, "+")) val += eval_mul(&tok, tok->next);
    else if (equal(tok, "-")) val -= eval_mul(&tok, tok->next);
    else break;
  }
  *rest = tok;
  return val;
}

static long eval_shift(Token **rest, Token *tok) {
  long val = eval_add(&tok, tok);
  for (;;) {
    if (equal(tok, "<<")) val <<= eval_add(&tok, tok->next);
    else if (equal(tok, ">>")) val >>= eval_add(&tok, tok->next);
    else break;
  }
  *rest = tok;
  return val;
}

static long eval_rel(Token **rest, Token *tok) {
  long val = eval_shift(&tok, tok);
  for (;;) {
    if (equal(tok, "<")) val = val < eval_shift(&tok, tok->next);
    else if (equal(tok, "<=")) val = val <= eval_shift(&tok, tok->next);
    else if (equal(tok, ">")) val = val > eval_shift(&tok, tok->next);
    else if (equal(tok, ">=")) val = val >= eval_shift(&tok, tok->next);
    else break;
  }
  *rest = tok;
  return val;
}

static long eval_eq(Token **rest, Token *tok) {
  long val = eval_rel(&tok, tok);
  for (;;) {
    if (equal(tok, "==")) val = val == eval_rel(&tok, tok->next);
    else if (equal(tok, "!=")) val = val != eval_rel(&tok, tok->next);
    else break;
  }
  *rest = tok;
  return val;
}

static long eval_bitand(Token **rest, Token *tok) {
  long val = eval_eq(&tok, tok);
  while (equal(tok, "&")) val &= eval_eq(&tok, tok->next);
  *rest = tok;
  return val;
}

static long eval_bitxor(Token **rest, Token *tok) {
  long val = eval_bitand(&tok, tok);
  while (equal(tok, "^")) val ^= eval_bitand(&tok, tok->next);
  *rest = tok;
  return val;
}

static long eval_bitor(Token **rest, Token *tok) {
  long val = eval_bitxor(&tok, tok);
  while (equal(tok, "|")) val |= eval_bitxor(&tok, tok->next);
  *rest = tok;
  return val;
}

static long eval_land(Token **rest, Token *tok) {
  long val = eval_bitor(&tok, tok);
  while (equal(tok, "&&")) val = (val && eval_bitor(&tok, tok->next)) ? 1 : 0;
  *rest = tok;
  return val;
}

static long eval_expr(Token **rest, Token *tok) {
  long val = eval_land(&tok, tok);
  while (equal(tok, "||")) val = (val || eval_land(&tok, tok->next)) ? 1 : 0;
  *rest = tok;
  return val;
}

//
// Macro expansion
//

static MacroArg *read_macro_args(Token **rest, Token *tok, Macro *m) {
  Token *t = tok->next->next; // Skip the name and "("
  MacroParam *param = m->params;

  MacroArg head = {0};
  MacroArg *cur = &head;

  while (!equal(t, ")")) {
    if (cur != &head)
      t = skip(t, ",");

    MacroArg *arg = calloc(1, sizeof(MacroArg));
    if (param) {
      arg->name = param->name;
      param = param->next;
    }

    Token ah = {0};
    Token *ac = &ah;
    int depth = 0;
    while (t->kind != TK_EOF && !(depth == 0 && (equal(t, ",") || equal(t, ")")))) {
      if (equal(t, "("))
        depth++;
      else if (equal(t, ")"))
        depth--;
      ac = ac->next = copy_token(t);
      t = t->next;
    }
    ac->next = NULL;
    arg->tok = ah.next;

    cur = cur->next = arg;
  }

  *rest = t->next; // Skip ")"
  return head.next;
}

static Token *subst(Token *body, MacroArg *args) {
  Token head = {0};
  Token *cur = &head;

  for (Token *t = body; t; t = t->next) {
    MacroArg *arg = NULL;
    if (t->kind == TK_IDENT || t->kind == TK_KEYWORD) {
      for (MacroArg *a = args; a; a = a->next)
        if (a->name && (int)strlen(a->name) == t->len && !strncmp(a->name, t->loc, t->len)) {
          arg = a;
          break;
        }
    }

    if (arg) {
      for (Token *x = arg->tok; x; x = x->next)
        cur = cur->next = copy_token(x);
      continue;
    }
    cur = cur->next = copy_token(t);
  }

  cur->next = NULL;
  return head.next;
}

// If *tokp names a macro, replace it with its expansion and return true.
static bool expand_macro(Token **tokp) {
  Token *tok = *tokp;
  if (tok->kind != TK_IDENT && tok->kind != TK_KEYWORD)
    return false;

  Macro *m = find_macro_str(tok);
  if (!m)
    return false;
  if (!m->is_objlike && !equal(tok->next, "("))
    return false;

  Token *rest;
  Token *body;
  if (m->is_objlike) {
    rest = tok->next;
    body = copy_list(m->body);
  } else {
    MacroArg *args = read_macro_args(&rest, tok, m);
    body = subst(m->body, args);
  }

  *tokp = append_tokens(body, rest);
  return true;
}

//
// Directives
//

static Token *read_define(Token *tok) {
  tok = tok->next->next; // Skip "#define"
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a macro name");

  Macro *m = calloc(1, sizeof(Macro));
  m->name = tok_strdup(tok);
  m->is_objlike = true;
  tok = tok->next;

  if (equal(tok, "(") && !tok->has_space) {
    m->is_objlike = false;
    tok = tok->next;

    MacroParam head = {0};
    MacroParam *cur = &head;
    while (!equal(tok, ")")) {
      if (cur != &head)
        tok = skip(tok, ",");
      if (tok->kind != TK_IDENT)
        error_at(tok->loc, "expected a macro parameter name");
      MacroParam *p = calloc(1, sizeof(MacroParam));
      p->name = tok_strdup(tok);
      cur = cur->next = p;
      tok = tok->next;
    }
    m->params = head.next;
    tok = tok->next; // Skip ")"
  }

  Token bhead = {0};
  Token *bcur = &bhead;
  while (tok->kind != TK_EOF && !tok->at_bol) {
    bcur = bcur->next = copy_token(tok);
    tok = tok->next;
  }
  bcur->next = NULL;
  m->body = bhead.next;

  m->next = macros;
  macros = m;
  return tok;
}

static Token *read_undef(Token *tok) {
  tok = tok->next->next; // Skip "#undef"
  if (tok->kind != TK_IDENT)
    error_at(tok->loc, "expected a macro name");
  Macro **p = &macros;
  while (*p) {
    if ((int)strlen((*p)->name) == tok->len && !strncmp((*p)->name, tok->loc, tok->len)) {
      *p = (*p)->next;
      break;
    }
    p = &(*p)->next;
  }
  return skip_line(tok->next);
}

static Token *preprocess2(Token *tok);

static Token *include_file(Token *rest, char *path) {
  char *src = read_file(path);
  Token *t = tokenize(src);
  t = preprocess2(t);

  if (t->kind == TK_EOF)
    return rest;

  Token *p = t;
  while (p->next->kind != TK_EOF)
    p = p->next;
  p->next = rest;
  return t;
}

static Token *read_include(Token *tok) {
  Token *t = tok->next->next; // Skip "#include"
  char *fname;

  if (t->kind == TK_STR) {
    fname = t->str;
    tok = t->next;
  } else if (equal(t, "<")) {
    char *buf = calloc(1, 1);
    Token *x = t->next;
    while (x->kind != TK_EOF && !equal(x, ">")) {
      buf = str_concat(buf, tok_strdup(x));
      x = x->next;
    }
    fname = buf;
    tok = x->next;
  } else {
    error_at(t->loc, "expected a file name");
  }

  tok = skip_line(tok);
  char *path = search_include(fname);
  if (!path)
    error("%s: no such include file", fname);
  return include_file(tok, path);
}

// Handles a directive. `tok` points at the '#'.
static Token *directive(Token *tok) {
  Token *hash = tok;
  tok = tok->next;
  if (tok->kind == TK_EOF)
    return tok;

  if (equal(tok, "define")) {
    if (!cur_active())
      return skip_line(tok);
    return read_define(hash);
  }

  if (equal(tok, "undef")) {
    if (!cur_active())
      return skip_line(tok);
    return read_undef(hash);
  }

  if (equal(tok, "include")) {
    if (!cur_active())
      return skip_line(tok);
    return read_include(hash);
  }

  if (equal(tok, "ifdef") || equal(tok, "ifndef")) {
    bool neg = equal(tok, "ifndef");
    Token *name = tok->next;
    bool parent = cur_active();
    bool v = parent && ((find_macro_str(name) != NULL) != neg);
    cond_active[cond_depth] = v;
    cond_ever[cond_depth] = v;
    cond_depth++;
    return skip_line(tok);
  }

  if (equal(tok, "if")) {
    bool parent = cur_active();
    long v = 0;
    if (parent) {
      Token *dummy;
      v = eval_expr(&dummy, tok->next);
    }
    cond_active[cond_depth] = parent && (v != 0);
    cond_ever[cond_depth] = cond_active[cond_depth];
    cond_depth++;
    return skip_line(tok);
  }

  if (equal(tok, "elif")) {
    if (cond_depth == 0)
      error_at(tok->loc, "stray #elif");
    int d = cond_depth - 1;
    bool parent = parent_active(d);
    bool v = false;
    if (parent && !cond_ever[d]) {
      Token *dummy;
      v = eval_expr(&dummy, tok->next) != 0;
    }
    cond_active[d] = v;
    cond_ever[d] = cond_ever[d] || v;
    return skip_line(tok);
  }

  if (equal(tok, "else")) {
    if (cond_depth == 0)
      error_at(tok->loc, "stray #else");
    int d = cond_depth - 1;
    bool v = parent_active(d) && !cond_ever[d];
    cond_active[d] = v;
    cond_ever[d] = true;
    return skip_line(tok);
  }

  if (equal(tok, "endif")) {
    if (cond_depth == 0)
      error_at(tok->loc, "stray #endif");
    cond_depth--;
    return skip_line(tok);
  }

  if (equal(tok, "pragma")) {
    // Only "once" is meaningful, and include guards make it optional.
    return skip_line(tok);
  }

  if (equal(tok, "error")) {
    if (cur_active())
      error_at(tok->loc, "#error");
    return skip_line(tok);
  }

  return skip_line(tok);
}

static Token *preprocess2(Token *tok) {
  Token head = {0};
  Token *cur = &head;
  int guard = 0;

  while (tok->kind != TK_EOF) {
    if (is_hash(tok) && tok->at_bol) {
      tok = directive(tok);
      continue;
    }

    if (!cur_active()) {
      tok = tok->next;
      continue;
    }

    if (expand_macro(&tok)) {
      if (++guard > 10000000)
        error("macro expansion is too deep");
      continue;
    }

    cur = cur->next = copy_token(tok);
    tok = tok->next;
  }

  cur->next = tok;
  return head.next;
}

Token *preprocess(Token *tok) {
  return preprocess2(tok);
}
