#ifndef NCCLCC_H
#define NCCLCC_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define NCCL_NORETURN __attribute__((noreturn))
#else
#define NCCL_NORETURN
#endif

//
// tokenize.c
//

typedef enum {
  TK_IDENT, // Identifiers
  TK_PUNCT, // Punctuators
  TK_NUM,   // Numeric literals
  TK_EOF,   // End-of-file markers
} TokenKind;

typedef struct Token Token;
struct Token {
  TokenKind kind; // Token kind
  Token *next;    // Next token
  long val;       // If kind is TK_NUM, its value
  char *loc;      // Token location
  int len;        // Token length
};

extern char *current_input;

// Reports an error and exit.
NCCL_NORETURN void error(char *fmt, ...);

// Reports an error message in the following format and exit.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
NCCL_NORETURN void error_at(char *loc, char *fmt, ...);

// Returns true if the token matches the given string.
bool equal(Token *tok, char *op);

// Consumes the current token if it matches `op`.
Token *skip(Token *tok, char *op);

// Tokenize `input` and returns a linked list of tokens.
Token *tokenize(char *input);

//
// parse.c
//

typedef enum {
  ND_RETURN,    // "return" statement
  ND_EXPR_STMT, // Expression statement
  ND_NUM,       // Integer literal
  ND_LVAR,      // Local variable
  ND_ASSIGN,    // "="
  ND_NEG,       // Unary minus
  ND_NOT,    // Logical negation
  ND_BITNOT, // Bitwise complement
  ND_ADD,    // +
  ND_SUB,    // -
  ND_MUL,    // *
  ND_DIV,    // /
  ND_MOD,    // %
  ND_EQ,     // ==
  ND_NE,     // !=
  ND_LT,     // <
  ND_LE,     // <=
  ND_BITAND, // &
  ND_BITOR,  // |
  ND_BITXOR, // ^
  ND_LOGAND, // &&
  ND_LOGOR,  // ||
} NodeKind;

// Variable (local for now).
typedef struct Obj Obj;
struct Obj {
  Obj *next;  // Next variable
  char *name; // Variable name
  int len;    // Name length
  int offset; // Offset from %rbp
};

extern Obj *locals;

// AST node type
typedef struct Node Node;
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  Node *lhs;     // Left-hand side
  Node *rhs;     // Right-hand side
  Obj *var;      // Used if kind == ND_LVAR or ND_ASSIGN
  long val;      // Used if kind == ND_NUM
};

Node *parse(Token *tok);

//
// codegen.c
//

void codegen(Node *node, FILE *out);

#endif // NCCLCC_H
