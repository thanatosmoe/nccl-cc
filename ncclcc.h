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
  TK_KEYWORD, // Keywords
  TK_STR,   // String literals
  TK_NUM,   // Numeric literals
  TK_EOF,   // End-of-file markers
} TokenKind;

typedef struct Token Token;
struct Token {
  TokenKind kind;  // Token kind
  Token *next;     // Next token
  long val;        // If kind is TK_NUM, its value
  char *str;       // If kind is TK_STR, its decoded value
  int str_len;     // Length of TK_STR (excluding the terminating NUL)
  char *loc;       // Token location
  int len;         // Token length
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
  ND_IF,        // "if" statement
  ND_FOR,       // "for" or "while" statement
  ND_BREAK,     // "break" statement
  ND_CONTINUE,  // "continue" statement
  ND_BLOCK,     // "{ ... }"
  ND_FUNCALL,   // Function call
  ND_NUM,       // Integer literal
  ND_STR,       // String literal
  ND_VAR,       // Variable (local or global)
  ND_ASSIGN,    // "="
  ND_COND,      // "?:" conditional
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

// Type
typedef enum {
  TY_CHAR, // char
  TY_INT,  // int
} TypeKind;

typedef struct Type Type;
struct Type {
  TypeKind kind; // Type kind
  int size;      // sizeof() value
};

extern Type *ty_char;
extern Type *ty_int;

// An in-memory string literal.
typedef struct StringLit StringLit;
struct StringLit {
  StringLit *next;
  char *data; // NUL-terminated contents
  int len;    // Length excluding the terminating NUL
  int id;     // Unique id (used for the assembly label)
};

extern StringLit *strings;
extern int str_count;

// Variable.
typedef struct Obj Obj;
struct Obj {
  Obj *next;      // Next variable
  char *name;     // Variable name
  int len;        // Name length
  Type *ty;       // Type
  bool is_global; // True if a global variable
  bool has_init;  // True if a global has an initializer
  int offset;     // Offset from %rbp (locals)
  long init_val;  // Initial value (globals)
};

extern Obj *locals;
extern Obj *globals;

// AST node type
typedef struct Node Node;
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  Node *lhs;     // Left-hand side
  Node *rhs;     // Right-hand side
  Node *cond;    // Used if kind == ND_IF/ND_COND/ND_FOR
  Node *then;    // Used if kind == ND_IF/ND_COND/ND_FOR
  Node *els;     // Used if kind == ND_IF or ND_COND
  Node *init;    // Used if kind == ND_FOR
  Node *inc;     // Used if kind == ND_FOR
  Node *body;    // Used if kind == ND_BLOCK
  Node *args;    // Used if kind == ND_FUNCALL
  char *name;    // Used if kind == ND_FUNCALL
  Obj *var;      // Used if kind == ND_VAR
  Type *ty;      // Result type
  StringLit *str; // Used if kind == ND_STR
  long val;      // Used if kind == ND_NUM
};

// Function.
typedef struct Function Function;
struct Function {
  Function *next; // Next function
  char *name;     // Function name
  Type *ty;       // Return type
  Obj *params;    // Parameters
  Node *body;     // Function body
  Obj *locals;    // Local variables
  int stack_size; // Size of the stack frame (bytes)
};

Function *parse(Token *tok);

//
// codegen.c
//

void codegen(Function *prog, FILE *out);

#endif // NCCLCC_H
