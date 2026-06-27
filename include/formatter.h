#ifndef FORMATTER_H
#define FORMATTER_H

/*
 * FormattingEngine
 *
 * Runs language-specific external formatters on a flat UTF-8 buffer.
 * Called by the save pipeline before every write.
 *
 * Design guarantees:
 *   - Never loses user data on formatter failure (returns original text).
 *   - Never blocks save permanently (timeout + fallback).
 *   - Reports diagnostics via FormatResult.
 *   - Completely decoupled from the editor core; only receives/returns bytes.
 *
 * Adding a new formatter: add one FormatRule to FORMATTER_RULES in formatter.c.
 * No other file needs to change.
 */

#include "language.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  FMT_OK = 0,         /* formatter ran and output differs or matches */
  FMT_UNCHANGED,      /* formatter ran, output identical to input */
  FMT_UNAVAILABLE,    /* no formatter registered for this language */
  FMT_NOT_INSTALLED,  /* formatter binary not found in PATH */
  FMT_ERROR,          /* formatter exited non-zero */
  FMT_TIMEOUT,        /* formatter took too long (killed) */
  FMT_INTERNAL_ERROR, /* pipe/fork/temp-file failure */
} FormatStatus;

typedef struct {
  FormatStatus status;
  char *output; /* formatted text; caller frees; NULL on failure */
  size_t output_len;
  char diag[512]; /* human-readable status/error message */
} FormatResult;

/*
 * Format `input` (len bytes) for language `lang`.
 * On FMT_OK: result.output holds the formatted text.
 * On any error: result.output is NULL; caller must use original input.
 * Caller must free result.output when done.
 *
 * `filepath` is passed to formatters that accept a filename hint
 * (e.g. clang-format reads .clang-format up the directory tree).
 * May be NULL.
 */
FormatResult fmt_run(LangID lang, const char *input, size_t len,
                     const char *filepath);

void fmt_result_free(FormatResult *r);

/* Returns the formatter command name for `lang`, or NULL if none. */
const char *fmt_command_name(LangID lang);

/* Returns true if the formatter binary is present in PATH. */
bool fmt_is_available(LangID lang);

#endif /* FORMATTER_H */
