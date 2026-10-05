#ifndef NCCLCC_H
#define NCCLCC_H

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
void error(char *fmt, ...);

// Reports an error message in the following format and exit.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
void error_at(char *loc, char *fmt, ...);

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
  ND_NUM, // Integer literal
} NodeKind;

// AST node type
typedef struct Node Node;
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  long val;      // Used if kind == ND_NUM
};

Node *parse(Token *tok);

//
// codegen.c
//

void codegen(Node *node, FILE *out);

#endif // NCCLCC_H
