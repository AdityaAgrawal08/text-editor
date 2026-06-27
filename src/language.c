#define _POSIX_C_SOURCE 200809L
#include "language.h"
#include <stdlib.h>
#include <string.h>

/* =====================================================================
 * Per-language configuration table
 * ===================================================================== */
static const LangConfig LANG_TABLE[LANG_COUNT] = {
    [LANG_UNKNOWN] = {LANG_UNKNOWN, "Plain Text", NULL, NULL, NULL,
                      INDENT_SPACES, 4, "()[]{}\"\"''"},
    [LANG_C] = {LANG_C, "C", "//", "/*", "*/", INDENT_SPACES, 4,
                "()[]{}\"\"''"},
    [LANG_CPP] = {LANG_CPP, "C++", "//", "/*", "*/", INDENT_SPACES, 4,
                  "()[]{}\"\"''"},
    [LANG_RUST] = {LANG_RUST, "Rust", "//", "/*", "*/", INDENT_SPACES, 4,
                   "()[]{}\"\"''"},
    [LANG_GO] = {LANG_GO, "Go", "//", "/*", "*/", INDENT_TABS, 1,
                 "()[]{}\"\"''"},
    [LANG_PYTHON] = {LANG_PYTHON, "Python", "#", NULL, NULL, INDENT_SPACES, 4,
                     "()[]{}\"\"''"},
    [LANG_JAVASCRIPT] = {LANG_JAVASCRIPT, "JavaScript", "//", "/*", "*/",
                         INDENT_SPACES, 2, "()[]{}\"\"''"},
    [LANG_TYPESCRIPT] = {LANG_TYPESCRIPT, "TypeScript", "//", "/*", "*/",
                         INDENT_SPACES, 2, "()[]{}\"\"''"},
    [LANG_JSX] = {LANG_JSX, "JSX", "//", "/*", "*/", INDENT_SPACES, 2,
                  "()[]{}\"\"''"},
    [LANG_TSX] = {LANG_TSX, "TSX", "//", "/*", "*/", INDENT_SPACES, 2,
                  "()[]{}\"\"''"},
    [LANG_JAVA] = {LANG_JAVA, "Java", "//", "/*", "*/", INDENT_SPACES, 4,
                   "()[]{}\"\"''"},
    [LANG_KOTLIN] = {LANG_KOTLIN, "Kotlin", "//", "/*", "*/", INDENT_SPACES, 4,
                     "()[]{}\"\"''"},
    [LANG_ZIG] = {LANG_ZIG, "Zig", "//", NULL, NULL, INDENT_SPACES, 4,
                  "()[]{}\"\"''"},
    [LANG_LUA] = {LANG_LUA, "Lua", "--", "--[[", "]]", INDENT_SPACES, 2,
                  "()[]{}\"\"''"},
    [LANG_SHELL] = {LANG_SHELL, "Shell", "#", NULL, NULL, INDENT_SPACES, 4,
                    "()[]{}\"\"''"},
    [LANG_JSON] = {LANG_JSON, "JSON", NULL, NULL, NULL, INDENT_SPACES, 2,
                   "()[]{}\"\""},
    [LANG_YAML] = {LANG_YAML, "YAML", "#", NULL, NULL, INDENT_SPACES, 2,
                   "()[]{}\"\"''"},
    [LANG_TOML] = {LANG_TOML, "TOML", "#", NULL, NULL, INDENT_SPACES, 2,
                   "()[]{}\"\"''"},
    [LANG_MARKDOWN] = {LANG_MARKDOWN, "Markdown", NULL, NULL, NULL,
                       INDENT_SPACES, 4, "()[]{}\"\"''"},
    [LANG_HTML] = {LANG_HTML, "HTML", NULL, "<!--", "-->", INDENT_SPACES, 2,
                   "()[]{}\"\"''"},
    [LANG_CSS] = {LANG_CSS, "CSS", NULL, "/*", "*/", INDENT_SPACES, 2,
                  "()[]{}\"\"''"},
};

/* =====================================================================
 * Extension → language
 * ===================================================================== */
typedef struct {
  const char *ext;
  LangID lang;
} ExtEntry;
static const ExtEntry EXT_TABLE[] = {
    {".c", LANG_C},
    {".h", LANG_C},
    {".cpp", LANG_CPP},
    {".cc", LANG_CPP},
    {".cxx", LANG_CPP},
    {".hpp", LANG_CPP},
    {".hxx", LANG_CPP},
    {".rs", LANG_RUST},
    {".go", LANG_GO},
    {".py", LANG_PYTHON},
    {".pyw", LANG_PYTHON},
    {".js", LANG_JAVASCRIPT},
    {".mjs", LANG_JAVASCRIPT},
    {".cjs", LANG_JAVASCRIPT},
    {".ts", LANG_TYPESCRIPT},
    {".mts", LANG_TYPESCRIPT},
    {".jsx", LANG_JSX},
    {".tsx", LANG_TSX},
    {".java", LANG_JAVA},
    {".kt", LANG_KOTLIN},
    {".kts", LANG_KOTLIN},
    {".zig", LANG_ZIG},
    {".lua", LANG_LUA},
    {".sh", LANG_SHELL},
    {".bash", LANG_SHELL},
    {".zsh", LANG_SHELL},
    {".fish", LANG_SHELL},
    {".json", LANG_JSON},
    {".yaml", LANG_YAML},
    {".yml", LANG_YAML},
    {".toml", LANG_TOML},
    {".md", LANG_MARKDOWN},
    {".markdown", LANG_MARKDOWN},
    {".html", LANG_HTML},
    {".htm", LANG_HTML},
    {".css", LANG_CSS},
    {".scss", LANG_CSS},
    {NULL, LANG_UNKNOWN},
};

/* =====================================================================
 * Shebang tokens
 * ===================================================================== */
typedef struct {
  const char *token;
  LangID lang;
} ShebangEntry;
static const ShebangEntry SHEBANG_TABLE[] = {
    {"python3", LANG_PYTHON},  {"python", LANG_PYTHON},
    {"node", LANG_JAVASCRIPT}, {"nodejs", LANG_JAVASCRIPT},
    {"bash", LANG_SHELL},      {"sh", LANG_SHELL},
    {"zsh", LANG_SHELL},       {"fish", LANG_SHELL},
    {"lua", LANG_LUA},         {NULL, LANG_UNKNOWN},
};

/* =====================================================================
 * Helpers
 * ===================================================================== */
static LangID detect_from_extension(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return LANG_UNKNOWN;
  for (int i = 0; EXT_TABLE[i].ext; i++)
    if (strcmp(dot, EXT_TABLE[i].ext) == 0)
      return EXT_TABLE[i].lang;
  return LANG_UNKNOWN;
}

static LangID detect_from_shebang(const char *content, size_t len) {
  if (!content || len < 2 || content[0] != '#' || content[1] != '!')
    return LANG_UNKNOWN;
  /* First line only */
  char line[256];
  size_t end = 0;
  while (end < len && content[end] != '\n')
    end++;
  size_t copy = end < 255 ? end : 255;
  memcpy(line, content, copy);
  line[copy] = '\0';
  for (int i = 0; SHEBANG_TABLE[i].token; i++)
    if (strstr(line, SHEBANG_TABLE[i].token))
      return SHEBANG_TABLE[i].lang;
  return LANG_UNKNOWN;
}

static LangID detect_from_content(const char *content, size_t len) {
  if (!content || len == 0)
    return LANG_UNKNOWN;

  /* C++ keywords and patterns */
  if (strstr(content, "namespace ") || strstr(content, "template<") ||
      strstr(content, "template <") || strstr(content, "std::") ||
      strstr(content, "#include <iostream>") || strstr(content, "public:") ||
      strstr(content, "private:") || strstr(content, "protected:"))
    return LANG_CPP;

  /* Rust */
  if (strstr(content, "fn main()") || strstr(content, "impl ") ||
      strstr(content, "use std::"))
    return LANG_RUST;

  /* Go */
  if (strstr(content, "package ") && strstr(content, "func "))
    return LANG_GO;

  /* Python */
  if (strstr(content, "def ") && strstr(content, ":"))
    return LANG_PYTHON;

  /* JSON: starts with { or [ after whitespace */
  const char *p = content;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (*p == '{' || *p == '[')
    return LANG_JSON;

  return LANG_UNKNOWN;
}

/* =====================================================================
 * Public API
 * ===================================================================== */
LangID lang_detect(const char *filepath, const char *content,
                   size_t content_len) {
  LangID id = LANG_UNKNOWN;

  /* 1. Extension */
  if (filepath)
    id = detect_from_extension(filepath);

  /* 2. Disambiguate .h: C vs C++ from content */
  if (id == LANG_C && filepath) {
    const char *dot = strrchr(filepath, '.');
    if (dot && strcmp(dot, ".h") == 0 && content) {
      LangID cid = detect_from_content(content, content_len);
      if (cid == LANG_CPP)
        id = LANG_CPP;
    }
  }

  /* 3. Shebang */
  if (id == LANG_UNKNOWN) {
    LangID sid = detect_from_shebang(content, content_len);
    if (sid != LANG_UNKNOWN)
      id = sid;
  }

  /* 4. Content fallback */
  if (id == LANG_UNKNOWN && content && content_len > 0)
    id = detect_from_content(content, content_len);

  return id;
}

const LangConfig *lang_config(LangID id) {
  if (id < 0 || id >= LANG_COUNT)
    id = LANG_UNKNOWN;
  return &LANG_TABLE[id];
}

const char *lang_name(LangID id) { return lang_config(id)->name; }
