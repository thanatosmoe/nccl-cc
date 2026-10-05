#include "ncclcc.h"

char *current_input;

// Reports an error and exit.
NCCL_NORETURN void error(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

// Reports an error message in the following format and exit.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
NCCL_NORETURN void error_at(char *loc, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  int pos = loc - current_input;
  fprintf(stderr, "%s\n", current_input);
  fprintf(stderr, "%*s", pos, "");
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

bool equal(Token *tok, char *op) {
  return memcmp(tok->loc, op, tok->len) == 0 && op[tok->len] == '\0';
}

Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_at(tok->loc, "expected '%s'", op);
  return tok->next;
}

static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = calloc(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  return tok;
}

static bool startswith(char *p, char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

static bool is_ident1(char c) {
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_';
}

static bool is_ident2(char c) {
  return is_ident1(c) || ('0' <= c && c <= '9');
}

static bool is_keyword(Token *tok) {
  static char *kw[] = {
      "return", "if",     "else",   "for",    "while",   "break",
      "continue", "int",  "char",   "sizeof", "struct",  "union",
      "typedef", "enum",  "void",   "static", "extern",  "switch",
      "case",   "default", "do",    "goto",   "const",   "long",
      "short",  "unsigned", "signed",
  };
  for (size_t i = 0; i < sizeof(kw) / sizeof(*kw); i++)
    if (equal(tok, kw[i]))
      return true;
  return false;
}

static int read_punct(char *p) {
  static char *ops[] = {
      "<<=", ">>=", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=",
      "/=",  "%=",  "++", "--", "&&", "||", "<<", ">>", "&=", "|=",
      "^=",
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++)
    if (startswith(p, ops[i]))
      return strlen(ops[i]);
  return ispunct((unsigned char)*p) ? 1 : 0;
}

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7')
      c = (c << 3) + (*p++ - '0');
    if ('0' <= *p && *p <= '7')
      c = (c << 3) + (*p++ - '0');
    *new_pos = p;
    return c;
  }

  if (*p == 'x') {
    p++;
    if (!isxdigit((unsigned char)*p))
      error_at(p, "invalid hexadecimal escape sequence");
    int c = 0;
    for (; isxdigit((unsigned char)*p); p++) {
      int d = isdigit((unsigned char)*p) ? *p - '0'
                                         : (tolower((unsigned char)*p) - 'a' + 10);
      c = (c << 4) + d;
    }
    *new_pos = p;
    return c;
  }

  switch (*p) {
  case 'a': *new_pos = p + 1; return '\a';
  case 'b': *new_pos = p + 1; return '\b';
  case 't': *new_pos = p + 1; return '\t';
  case 'n': *new_pos = p + 1; return '\n';
  case 'v': *new_pos = p + 1; return '\v';
  case 'f': *new_pos = p + 1; return '\f';
  case 'r': *new_pos = p + 1; return '\r';
  case 'e': *new_pos = p + 1; return 27;
  default: *new_pos = p + 1; return (unsigned char)*p;
  }
}

static Token *read_char_literal(char **pp) {
  char *start = *pp;
  char *p = start + 1;
  if (*p == '\0')
    error_at(start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = (unsigned char)*p++;

  if (*p != '\'')
    error_at(p, "unclosed char literal");

  Token *tok = new_token(TK_NUM, start, p + 1);
  tok->val = c;
  *pp = p + 1;
  return tok;
}

static Token *read_string_literal(char **pp) {
  char *start = *pp;
  char *p = start + 1;

  int cap = 16;
  int len = 0;
  char *buf = calloc(cap, 1);

  for (;;) {
    if (*p == '\0')
      error_at(start, "unclosed string literal");
    if (*p == '"')
      break;

    int c;
    if (*p == '\\')
      c = read_escaped_char(&p, p + 1);
    else
      c = (unsigned char)*p++;

    if (len + 1 >= cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
    buf[len++] = c;
  }
  buf[len] = '\0';

  Token *tok = new_token(TK_STR, start, p + 1);
  tok->str = buf;
  tok->str_len = len;
  *pp = p + 1;
  return tok;
}

char *tok_strdup(Token *tok) {
  char *s = calloc(tok->len + 1, 1);
  memcpy(s, tok->loc, tok->len);
  return s;
}

// Tokenize `input` and returns a linked list of tokens.
Token *tokenize(char *input) {
  current_input = input;
  char *p = input;
  Token head = {0};
  Token *cur = &head;

  bool at_bol = true;
  bool has_space = false;

  while (*p) {
    // Newline
    if (*p == '\n') {
      p++;
      at_bol = true;
      has_space = false;
      continue;
    }

    // Skip line comments.
    if (startswith(p, "//")) {
      p += 2;
      while (*p && *p != '\n')
        p++;
      has_space = true;
      continue;
    }

    // Skip block comments.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      if (memchr(p, '\n', q - p))
        at_bol = true;
      p = q + 2;
      has_space = true;
      continue;
    }

    // Skip other whitespace characters.
    if (isspace((unsigned char)*p)) {
      p++;
      has_space = true;
      continue;
    }

    bool tok_at_bol = at_bol;
    bool tok_has_space = has_space;
    Token *tok;

    // String literal
    if (*p == '"') {
      tok = read_string_literal(&p);
    }
    // Character literal
    else if (*p == '\'') {
      tok = read_char_literal(&p);
    }
    // Numeric literal
    else if (isdigit((unsigned char)*p)) {
      char *start = p;
      long val = strtol(p, &p, 10);
      tok = new_token(TK_NUM, start, p);
      tok->val = val;
    }
    // Identifier or keyword
    else if (is_ident1(*p)) {
      char *start = p;
      do {
        p++;
      } while (is_ident2(*p));
      tok = new_token(TK_IDENT, start, p);
      if (is_keyword(tok))
        tok->kind = TK_KEYWORD;
    }
    // Punctuators
    else {
      int punct_len = read_punct(p);
      if (!punct_len)
        error_at(p, "invalid token");
      tok = new_token(TK_PUNCT, p, p + punct_len);
      p += punct_len;
    }

    tok->at_bol = tok_at_bol;
    tok->has_space = tok_has_space;
    cur = cur->next = tok;
    at_bol = false;
    has_space = false;
  }

  Token *eof = new_token(TK_EOF, p, p);
  eof->at_bol = true;
  cur = cur->next = eof;
  return head.next;
}
