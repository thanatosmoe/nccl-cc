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

static int read_punct(char *p) {
  static char *ops[] = {
      "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "%=",
      "++", "--", "&&", "||", "<<", ">>", "&=", "|=", "^=", "!=",
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); i++)
    if (startswith(p, ops[i]))
      return strlen(ops[i]);
  return ispunct((unsigned char)*p) ? 1 : 0;
}

// Tokenize `input` and returns a linked list of tokens.
Token *tokenize(char *input) {
  current_input = input;
  char *p = input;
  Token head = {0};
  Token *cur = &head;

  while (*p) {
    // Skip line comments.
    if (startswith(p, "//")) {
      p += 2;
      while (*p && *p != '\n')
        p++;
      continue;
    }

    // Skip block comments.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      p = q + 2;
      continue;
    }

    // Skip whitespace characters.
    if (isspace((unsigned char)*p)) {
      p++;
      continue;
    }

    // Numeric literal
    if (isdigit((unsigned char)*p)) {
      char *start = p;
      long val = strtol(p, &p, 10);
      cur = cur->next = new_token(TK_NUM, start, p);
      cur->val = val;
      continue;
    }

    // Identifier
    if (is_ident1(*p)) {
      char *start = p;
      do {
        p++;
      } while (is_ident2(*p));
      cur = cur->next = new_token(TK_IDENT, start, p);
      continue;
    }

    // Punctuators
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
      p += punct_len;
      continue;
    }

    error_at(p, "invalid token");
  }

  cur = cur->next = new_token(TK_EOF, p, p);
  return head.next;
}
