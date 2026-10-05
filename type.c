#include "ncclcc.h"

static Type char_type = {TY_CHAR, 1};
static Type int_type = {TY_INT, 8};

Type *ty_char = &char_type;
Type *ty_int = &int_type;

StringLit *strings;
int str_count;
