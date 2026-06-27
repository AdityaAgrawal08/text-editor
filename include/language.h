#ifndef LANGUAGE_H
#define LANGUAGE_H

/*
 * LanguageRegistry
 * Detects language from path, shebang, or content.
 * Provides per-language configuration used by SyntaxEngine,
 * FormattingEngine, and DiagnosticsEngine.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  LANG_UNKNOWN = 0,
  LANG_C,
  LANG_CPP,
  LANG_RUST,
  LANG_GO,
  LANG_PYTHON,
  LANG_JAVASCRIPT,
  LANG_TYPESCRIPT,
  LANG_JSX,
  LANG_TSX,
  LANG_JAVA,
  LANG_KOTLIN,
  LANG_ZIG,
  LANG_LUA,
  LANG_SHELL,
  LANG_JSON,
  LANG_YAML,
  LANG_TOML,
  LANG_MARKDOWN,
  LANG_HTML,
  LANG_CSS,
  LANG_COUNT,
} LangID;

/* Indent style used by a language's formatter */
typedef enum {
  INDENT_SPACES,
  INDENT_TABS,
} IndentStyle;

typedef struct {
  LangID id;
  const char *name;               /* display name */
  const char *comment_line;       /* "//" or "#" or "--" etc., NULL if none */
  const char *comment_block_open; /* block comment open token, NULL if none */
  const char *comment_block_close;
  IndentStyle indent_style;
  int indent_width; /* spaces per indent level */
  /* Auto-pair characters: NUL-terminated pairs of (open, close) */
  const char *auto_pairs; /* e.g. "()[]{}\"\"''" */
} LangConfig;

/* Detect language from file path (extension), then shebang, then content.
   content may be NULL if not available yet. */
LangID lang_detect(const char *filepath, const char *content,
                   size_t content_len);

/* Return config for a language. Always returns a valid pointer (LANG_UNKNOWN
   returns a sensible default). */
const LangConfig *lang_config(LangID id);

const char *lang_name(LangID id);

#endif /* LANGUAGE_H */
