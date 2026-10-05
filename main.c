#include "ncclcc.h"

static char *input_path;
static char *output_path;
static bool opt_S;

char **include_paths;
int include_path_count;

static void usage(int status) {
  fprintf(stderr, "ncclcc [ -S ] [ -I<dir> ] [ -o <path> ] <file>\n");
  exit(status);
}

char *read_file(char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    error("cannot open %s: %s", path, strerror(errno));

  if (fseek(fp, 0, SEEK_END) != 0)
    error("%s: fseek failed", path);
  long size = ftell(fp);
  if (size < 0)
    error("%s: ftell failed", path);
  rewind(fp);

  char *buf = calloc(1, size + 2);
  if (size > 0 && fread(buf, size, 1, fp) != 1)
    error("%s: fread failed", path);
  buf[size] = '\n';
  buf[size + 1] = '\0';
  fclose(fp);
  return buf;
}

// Replaces the file extension of `path` with `ext`, keeping the directory.
static char *replace_ext(char *path, char *ext) {
  char *base = path;
  for (char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      base = p + 1;

  char *dot = strrchr(base, '.');
  int len = dot ? (int)(dot - path) : (int)strlen(path);

  char *out = calloc(1, len + strlen(ext) + 1);
  memcpy(out, path, len);
  strcpy(out + len, ext);
  return out;
}

static int run(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char cmd[8192];
  vsnprintf(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);
  return system(cmd);
}

// Adds a directory to the include search path.
static void add_include_path(char *path) {
  include_paths = realloc(include_paths, sizeof(char *) * (include_path_count + 1));
  include_paths[include_path_count++] = path;
}

// Returns the directory part of `path`, or "." if there is none.
static char *dir_name(char *path) {
  char *slash = NULL;
  for (char *p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      slash = p;
  if (!slash)
    return ".";
  int len = slash - path;
  if (len == 0)
    len = 1; // Keep the root "/"
  char *s = calloc(len + 1, 1);
  memcpy(s, path, len);
  return s;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }
    if (!strcmp(argv[i], "-o")) {
      if (++i >= argc)
        usage(1);
      output_path = argv[i];
      continue;
    }
    if (!strncmp(argv[i], "-o", 2)) {
      output_path = argv[i] + 2;
      continue;
    }
    if (!strcmp(argv[i], "-I")) {
      if (++i >= argc)
        usage(1);
      add_include_path(argv[i]);
      continue;
    }
    if (!strncmp(argv[i], "-I", 2)) {
      add_include_path(argv[i] + 2);
      continue;
    }
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
      usage(0);
    if (argv[i][0] == '-' && argv[i][1])
      usage(1);
    input_path = argv[i];
  }

  if (!input_path)
    usage(1);

  // The directory of the input file is searched first for #include "...".
  add_include_path(dir_name(input_path));

  char *input = read_file(input_path);
  Token *tok = tokenize(input);
  tok = preprocess(tok);
  Function *prog = parse(tok);

  char *asm_path = opt_S
      ? (output_path ? output_path : replace_ext(input_path, ".s"))
      : replace_ext(input_path, ".tmp.s");

  FILE *out = fopen(asm_path, "w");
  if (!out)
    error("cannot open output file: %s", asm_path);
  codegen(prog, out);
  fclose(out);

  if (opt_S)
    return 0;

  char *exe_path = output_path ? output_path : replace_ext(input_path, ".exe");
  int rc = run("gcc -o \"%s\" \"%s\"", exe_path, asm_path);
  remove(asm_path);
  if (rc != 0)
    error("assembler failed");
  return 0;
}
