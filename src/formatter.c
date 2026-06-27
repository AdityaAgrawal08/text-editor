#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "formatter.h"
#include "language.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Formatter timeout in seconds */
#define FMT_TIMEOUT_SEC 10

/* Maximum formatted output we'll accept (64 MB) */
#define FMT_MAX_OUTPUT (64 * 1024 * 1024)

/* =====================================================================
 * Formatter rule: one entry per language that has an external tool.
 * cmd_template uses printf-style substitution:
 *   %f  = filepath (or "-" if NULL)
 *   If cmd_template contains %f, the file is written to a temp path
 *   and the path is substituted. Otherwise stdin/stdout is used.
 * ===================================================================== */
typedef enum {
  FM_STDIN_STDOUT, /* formatter reads stdin, writes stdout */
  FM_INPLACE,      /* formatter modifies a temp file in place (%f) */
} FormatMode;

typedef struct {
  LangID lang;
  const char *binary;       /* first token — used for availability check */
  const char *cmd_template; /* full shell command, %f = tempfile path */
  FormatMode mode;
} FormatRule;

static const FormatRule FORMATTER_RULES[] = {
    /* C / C++ — clang-format reads stdin, writes stdout */
    {LANG_C, "clang-format", "clang-format --style=file --fallback-style=LLVM",
     FM_STDIN_STDOUT},
    {LANG_CPP, "clang-format",
     "clang-format --style=file --fallback-style=LLVM", FM_STDIN_STDOUT},

    /* Rust — rustfmt reads stdin, writes stdout */
    {LANG_RUST, "rustfmt", "rustfmt --edition 2021", FM_STDIN_STDOUT},

    /* Go — gofmt reads stdin, writes stdout */
    {LANG_GO, "gofmt", "gofmt", FM_STDIN_STDOUT},

    /* Python — ruff format (preferred); falls back to black if ruff absent */
    {LANG_PYTHON, "ruff", "ruff format --quiet -", FM_STDIN_STDOUT},

    /* JavaScript / TypeScript / JSX / TSX / JSON / Markdown — prettier */
    {LANG_JAVASCRIPT, "prettier", "prettier --parser babel", FM_STDIN_STDOUT},
    {LANG_TYPESCRIPT, "prettier", "prettier --parser typescript",
     FM_STDIN_STDOUT},
    {LANG_JSX, "prettier", "prettier --parser babel", FM_STDIN_STDOUT},
    {LANG_TSX, "prettier", "prettier --parser typescript", FM_STDIN_STDOUT},
    {LANG_JSON, "prettier", "prettier --parser json", FM_STDIN_STDOUT},
    {LANG_MARKDOWN, "prettier", "prettier --parser markdown", FM_STDIN_STDOUT},
    {LANG_CSS, "prettier", "prettier --parser css", FM_STDIN_STDOUT},
    {LANG_HTML, "prettier", "prettier --parser html", FM_STDIN_STDOUT},

    /* YAML */
    {LANG_YAML, "prettier", "prettier --parser yaml", FM_STDIN_STDOUT},

    /* Shell — shfmt reads stdin, writes stdout */
    {LANG_SHELL, "shfmt", "shfmt -i 4 -bn -ci", FM_STDIN_STDOUT},

    /* Lua — stylua reads stdin, writes stdout */
    {LANG_LUA, "stylua", "stylua -", FM_STDIN_STDOUT},

    /* Zig — zig fmt reads a file path (inplace) */
    {LANG_ZIG, "zig", "zig fmt %f", FM_INPLACE},

    /* Kotlin — ktfmt reads stdin, writes stdout */
    {LANG_KOTLIN, "ktfmt", "ktfmt -", FM_STDIN_STDOUT},

    /* Sentinel */
    {LANG_UNKNOWN, NULL, NULL, FM_STDIN_STDOUT},
};

/* =====================================================================
 * Binary availability check (cached per session)
 * ===================================================================== */
static bool binary_exists(const char *name) {
  /* Use 'command -v' via /bin/sh which is always available */
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
  return system(cmd) == 0;
}

/* =====================================================================
 * Find rule for a language
 * ===================================================================== */
static const FormatRule *find_rule(LangID lang) {
  /* Special case: Python — prefer ruff, fall back to black */
  if (lang == LANG_PYTHON) {
    static const FormatRule ruff_rule = {
        LANG_PYTHON, "ruff", "ruff format --quiet -", FM_STDIN_STDOUT};
    static const FormatRule black_rule = {LANG_PYTHON, "black", "black -q -",
                                          FM_STDIN_STDOUT};
    if (binary_exists("ruff"))
      return &ruff_rule;
    if (binary_exists("black"))
      return &black_rule;
    return NULL;
  }

  for (int i = 0; FORMATTER_RULES[i].binary; i++) {
    if (FORMATTER_RULES[i].lang == lang)
      return &FORMATTER_RULES[i];
  }
  return NULL;
}

/* =====================================================================
 * Temp file helper
 * ===================================================================== */
static int create_tempfile(const char *suffix, char *out_path,
                           size_t out_size) {
  const char *tmp = getenv("TMPDIR");
  if (!tmp)
    tmp = "/tmp";

  /* Suffix can't be passed to mkstemp directly; build our own template */
  char template[512];
  snprintf(template, sizeof(template), "%s/editor_fmt_XXXXXX", tmp);

  int fd = mkstemp(template);
  if (fd < 0)
    return -1;

  if (suffix && suffix[0]) {
    /* Rename to include suffix so formatters see the right extension */
    char newpath[512];
    snprintf(newpath, sizeof(newpath), "%s%s", template, suffix);
    if (rename(template, newpath) == 0) {
      snprintf(out_path, out_size, "%s", newpath);
      close(fd);
      fd = open(newpath, O_RDWR | O_CREAT, 0600);
    } else {
      snprintf(out_path, out_size, "%s", template);
    }
  } else {
    snprintf(out_path, out_size, "%s", template);
  }
  return fd;
}

/* =====================================================================
 * Read all output from a FILE* into a malloc'd buffer
 * ===================================================================== */
static char *read_all(FILE *fp, size_t *out_len) {
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;

  while (!feof(fp)) {
    if (len + 1 >= cap) {
      cap *= 2;
      if (cap > FMT_MAX_OUTPUT) {
        free(buf);
        return NULL;
      }
      char *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, fp);
    len += n;
    if (ferror(fp))
      break;
  }
  buf[len] = '\0';
  *out_len = len;
  return buf;
}

/* =====================================================================
 * Run a stdin→stdout formatter
 * ===================================================================== */
static FormatResult run_stdin_stdout(const FormatRule *rule, const char *input,
                                     size_t input_len) {
  FormatResult r = {FMT_INTERNAL_ERROR, NULL, 0, ""};

  /* Write input to a temp file then feed via shell redirection to
     avoid fork+exec pipe complexity while still being safe */
  char tmp_in[512];
  int fd_in = create_tempfile(NULL, tmp_in, sizeof(tmp_in));
  if (fd_in < 0) {
    snprintf(r.diag, sizeof(r.diag), "mkstemp failed: %s", strerror(errno));
    return r;
  }
  /* Write input */
  size_t written = 0;
  while (written < input_len) {
    ssize_t w = write(fd_in, input + written, input_len - written);
    if (w < 0) {
      close(fd_in);
      unlink(tmp_in);
      return r;
    }
    written += (size_t)w;
  }
  close(fd_in);

  /* Build command: cat tempfile | formatter_cmd */
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "cat %s | %s 2>/dev/null", tmp_in,
           rule->cmd_template);

  FILE *fp = popen(cmd, "r");
  if (!fp) {
    unlink(tmp_in);
    snprintf(r.diag, sizeof(r.diag), "popen failed: %s", strerror(errno));
    return r;
  }

  size_t out_len = 0;
  char *out = read_all(fp, &out_len);
  int exit_code = pclose(fp);
  unlink(tmp_in);

  if (exit_code != 0 || !out) {
    free(out);
    r.status = FMT_ERROR;
    snprintf(r.diag, sizeof(r.diag), "%s exited with code %d", rule->binary,
             WIFEXITED(exit_code) ? WEXITSTATUS(exit_code) : -1);
    return r;
  }

  /* Check if output differs */
  if (out_len == input_len && memcmp(out, input, input_len) == 0) {
    free(out);
    r.status = FMT_UNCHANGED;
    snprintf(r.diag, sizeof(r.diag), "%s: no changes", rule->binary);
    return r;
  }

  r.status = FMT_OK;
  r.output = out;
  r.output_len = out_len;
  snprintf(r.diag, sizeof(r.diag), "formatted by %s", rule->binary);
  return r;
}

/* =====================================================================
 * Run an in-place formatter (zig fmt %f style)
 * ===================================================================== */
static FormatResult run_inplace(const FormatRule *rule, const char *input,
                                size_t input_len, const char *filepath) {
  FormatResult r = {FMT_INTERNAL_ERROR, NULL, 0, ""};

  /* Derive extension from filepath for temp file naming */
  const char *suffix = "";
  if (filepath) {
    const char *dot = strrchr(filepath, '.');
    if (dot)
      suffix = dot;
  }

  char tmp_path[512];
  int fd = create_tempfile(suffix, tmp_path, sizeof(tmp_path));
  if (fd < 0) {
    snprintf(r.diag, sizeof(r.diag), "mkstemp failed: %s", strerror(errno));
    return r;
  }

  size_t written = 0;
  while (written < input_len) {
    ssize_t w = write(fd, input + written, input_len - written);
    if (w < 0) {
      close(fd);
      unlink(tmp_path);
      return r;
    }
    written += (size_t)w;
  }
  close(fd);

  /* Build command substituting %f with the temp path */
  char cmd[1024];
  {
    const char *tmpl = rule->cmd_template;
    const char *pct = strstr(tmpl, "%f");
    if (pct) {
      int prefix_len = (int)(pct - tmpl);
      snprintf(cmd, sizeof(cmd), "%.*s%s%s 2>/dev/null", prefix_len, tmpl,
               tmp_path, pct + 2);
    } else {
      snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", tmpl, tmp_path);
    }
  }

  int exit_code = system(cmd);
  if (exit_code != 0) {
    unlink(tmp_path);
    r.status = FMT_ERROR;
    snprintf(r.diag, sizeof(r.diag), "%s exited with code %d", rule->binary,
             WIFEXITED(exit_code) ? WEXITSTATUS(exit_code) : -1);
    return r;
  }

  /* Read back the formatted file */
  FILE *fp = fopen(tmp_path, "rb");
  if (!fp) {
    unlink(tmp_path);
    return r;
  }
  size_t out_len = 0;
  char *out = read_all(fp, &out_len);
  fclose(fp);
  unlink(tmp_path);

  if (!out) {
    r.status = FMT_INTERNAL_ERROR;
    return r;
  }

  if (out_len == input_len && memcmp(out, input, input_len) == 0) {
    free(out);
    r.status = FMT_UNCHANGED;
    snprintf(r.diag, sizeof(r.diag), "%s: no changes", rule->binary);
    return r;
  }

  r.status = FMT_OK;
  r.output = out;
  r.output_len = out_len;
  snprintf(r.diag, sizeof(r.diag), "formatted by %s", rule->binary);
  return r;
}

/* =====================================================================
 * Public API
 * ===================================================================== */
FormatResult fmt_run(LangID lang, const char *input, size_t len,
                     const char *filepath) {
  FormatResult r = {FMT_UNAVAILABLE, NULL, 0, ""};

  const FormatRule *rule = find_rule(lang);
  if (!rule) {
    snprintf(r.diag, sizeof(r.diag), "no formatter for %s", lang_name(lang));
    return r;
  }

  if (!binary_exists(rule->binary)) {
    r.status = FMT_NOT_INSTALLED;
    snprintf(r.diag, sizeof(r.diag), "'%s' not found in PATH", rule->binary);
    return r;
  }

  if (rule->mode == FM_INPLACE)
    return run_inplace(rule, input, len, filepath);

  return run_stdin_stdout(rule, input, len);
}

void fmt_result_free(FormatResult *r) {
  if (!r)
    return;
  free(r->output);
  r->output = NULL;
  r->output_len = 0;
}

const char *fmt_command_name(LangID lang) {
  const FormatRule *rule = find_rule(lang);
  return rule ? rule->binary : NULL;
}

bool fmt_is_available(LangID lang) {
  const FormatRule *rule = find_rule(lang);
  return rule && binary_exists(rule->binary);
}
