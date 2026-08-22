#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "formatter.h"
#include "language.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
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
 * Binary availability check
 * Each probe forks a shell (`command -v`), so results are cached for the
 * process lifetime — rules reference static strings that never dangle.
 * ===================================================================== */
static bool binary_exists(const char *name) {
  static const char *cache_name[32];
  static signed char cache_state[32]; /* 0 = unchecked, 1 = yes, -1 = no */
  static int cache_n = 0;

  for (int i = 0; i < cache_n; i++) {
    if (cache_name[i] == name || strcmp(cache_name[i], name) == 0)
      return cache_state[i] > 0;
  }

  /* Use 'command -v' via /bin/sh which is always available */
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
  bool found = (system(cmd) == 0);

  if (cache_n < (int)(sizeof(cache_name) / sizeof(cache_name[0]))) {
    cache_name[cache_n] = name;
    cache_state[cache_n] = found ? 1 : -1;
    cache_n++;
  }
  return found;
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
      int nfd = open(newpath, O_RDWR, 0600);
      if (nfd < 0) {
        unlink(newpath); /* don't leak the renamed temp */
        return -1;
      }
      snprintf(out_path, out_size, "%s", newpath);
      close(fd);
      return nfd;
    }
    /* rename failed: keep the original template name */
  }
  snprintf(out_path, out_size, "%s", template);
  return fd;
}

/* =====================================================================
 * Child process machinery: fork/exec (no shell), poll-driven output
 * collection, hard timeout with SIGKILL.
 * ===================================================================== */

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

#define FMT_MAX_ARGS 64

static void fmt_free_argv(char **argv) {
  if (!argv)
    return;
  for (int i = 0; argv[i]; i++)
    free(argv[i]);
  free(argv);
}

/* Tokenize cmd_template on whitespace into an argv array, replacing the
   first "%f" inside each token with `subst`. Returns a NULL-terminated
   malloc'd array of malloc'd strings; free via fmt_free_argv. NULL on
   allocation failure. */
static char **fmt_tokenize(const char *tmpl, const char *subst) {
  char **argv = calloc(FMT_MAX_ARGS + 1, sizeof(char *));
  if (!argv)
    return NULL;
  char *buf = strdup(tmpl);
  if (!buf) {
    free(argv);
    return NULL;
  }
  int argc = 0;
  char *save = NULL;
  bool ok = true;
  for (char *tok = strtok_r(buf, " \t", &save); tok && argc < FMT_MAX_ARGS;
       tok = strtok_r(NULL, " \t", &save)) {
    const char *pct = strstr(tok, "%f");
    if (!pct) {
      argv[argc] = strdup(tok);
    } else {
      size_t n = strlen(tok) - strlen("%f") + strlen(subst) + 1;
      argv[argc] = malloc(n);
      if (argv[argc])
        snprintf(argv[argc], n, "%.*s%s%s", (int)(pct - tok), tok, subst,
                 pct + 2);
    }
    if (!argv[argc]) {
      ok = false;
      break;
    }
    argc++;
  }
  free(buf);
  if (!ok) {
    fmt_free_argv(argv);
    return NULL;
  }
  return argv;
}

typedef struct {
  pid_t pid;
  int out_fd; /* pipe read end, or -1 when stdout went to /dev/null */
} SpawnedProc;

/* Fork+exec argv[0], stdin read from stdin_fd. If `capture`, child stdout
   is a pipe whose read end the parent keeps; otherwise stdout and stderr
   both go to /dev/null. Child stderr always goes to /dev/null. */
static bool proc_spawn(char **argv, int stdin_fd, bool capture,
                       SpawnedProc *p) {
  int fds[2] = {-1, -1};
  if (capture && pipe(fds) != 0)
    return false;

  pid_t pid = fork();
  if (pid < 0) {
    if (fds[0] >= 0) {
      close(fds[0]);
      close(fds[1]);
    }
    return false;
  }
  if (pid == 0) {
    if (fds[0] >= 0)
      close(fds[0]);
    dup2(stdin_fd, STDIN_FILENO);
    if (fds[1] >= 0) {
      dup2(fds[1], STDOUT_FILENO);
      close(fds[1]);
    } else {
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        close(dn);
      }
    }
    int dn2 = open("/dev/null", O_WRONLY);
    if (dn2 >= 0) {
      dup2(dn2, STDERR_FILENO);
      close(dn2);
    }
    execvp(argv[0], argv);
    _exit(127); /* exec failed */
  }
  if (fds[1] >= 0)
    close(fds[1]);
  p->pid = pid;
  p->out_fd = fds[0];
  return true;
}

/* Reap the child, enforcing the deadline: on expiry send SIGKILL and
   block until reaped. Returns the waitpid status; *timed_out reports
   whether we had to kill. -1 means waitpid failed. */
static int proc_wait(pid_t pid, uint64_t deadline, bool *timed_out) {
  *timed_out = false;
  for (;;) {
    int status;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return status;
    if (r < 0)
      return -1;
    if (now_ms() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      *timed_out = true;
      return status;
    }
    struct timespec ts = {0, 2 * 1000 * 1000}; /* 2 ms */
    nanosleep(&ts, NULL);
  }
}

/* Run argv with stdin from stdin_path, capturing stdout into a malloc'd
   buffer. Enforces FMT_TIMEOUT_SEC and FMT_MAX_OUTPUT.
   On success sets out and out_len and returns FMT_OK. On failure returns
   FMT_TIMEOUT / FMT_ERROR / FMT_INTERNAL_ERROR with diag filled in; out
   is then NULL. */
static FormatStatus run_captured(char **argv, const char *stdin_path,
                                 char **out, size_t *out_len, char *diag,
                                 size_t dsz) {
  *out = NULL;
  *out_len = 0;

  int in_fd = open(stdin_path, O_RDONLY);
  if (in_fd < 0) {
    snprintf(diag, dsz, "open temp input failed: %s", strerror(errno));
    return FMT_INTERNAL_ERROR;
  }

  SpawnedProc p;
  if (!proc_spawn(argv, in_fd, true, &p)) {
    close(in_fd);
    snprintf(diag, dsz, "fork/pipe failed: %s", strerror(errno));
    return FMT_INTERNAL_ERROR;
  }
  close(in_fd);

  uint64_t deadline = now_ms() + (uint64_t)FMT_TIMEOUT_SEC * 1000u;
  size_t cap = 8192, len = 0;
  char *buf = malloc(cap);
  bool timed_out = false, io_err = false, too_big = false, oom = false;

  while (!oom && !io_err && !too_big && !timed_out && buf) {
    uint64_t now = now_ms();
    if (now >= deadline) {
      timed_out = true;
      break;
    }
    uint64_t remaining = deadline - now;
    struct pollfd pf = {.fd = p.out_fd, .events = POLLIN, .revents = 0};
    int pr = poll(&pf, 1, (int)((remaining > 250) ? 250 : remaining));
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      io_err = true;
      break;
    }
    if (pr == 0)
      continue;

    char chunk[16384];
    ssize_t n = read(p.out_fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      io_err = true;
      break;
    }
    if (n == 0)
      break; /* EOF */
    if (len + (size_t)n > FMT_MAX_OUTPUT) {
      too_big = true;
      break;
    }
    if (len + (size_t)n + 1 > cap) {
      size_t nc = cap;
      while (nc < len + (size_t)n + 1)
        nc *= 2;
      char *nb = realloc(buf, nc);
      if (!nb) {
        oom = true;
        break;
      }
      buf = nb;
      cap = nc;
    }
    memcpy(buf + len, chunk, (size_t)n);
    len += (size_t)n;
  }

  int status = 0;
  if (timed_out || too_big || io_err || oom) {
    kill(p.pid, SIGKILL);
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {
    }
  } else {
    /* Drain complete (EOF); reap within deadline. */
    bool wt = false;
    status = proc_wait(p.pid, deadline, &wt);
    if (wt)
      timed_out = true;
  }
  if (p.out_fd >= 0)
    close(p.out_fd);

  if (oom) {
    free(buf);
    snprintf(diag, dsz, "out of memory reading formatter output");
    return FMT_INTERNAL_ERROR;
  }
  if (io_err) {
    free(buf);
    snprintf(diag, dsz, "error reading formatter output: %s",
             strerror(errno));
    return FMT_INTERNAL_ERROR;
  }
  if (too_big) {
    free(buf);
    snprintf(diag, dsz, "formatter output exceeded %d MB limit",
             (int)(FMT_MAX_OUTPUT / (1024 * 1024)));
    return FMT_ERROR;
  }
  if (timed_out) {
    free(buf);
    snprintf(diag, dsz, "'%s' timed out after %d s", argv[0],
             FMT_TIMEOUT_SEC);
    return FMT_TIMEOUT;
  }
  if (!buf) { /* OOM before loop started */
    snprintf(diag, dsz, "out of memory");
    return FMT_INTERNAL_ERROR;
  }

  buf[len] = '\0';
  *out = buf;
  *out_len = len;

  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (!WIFEXITED(status) || code != 0) {
    free(*out);
    *out = NULL;
    *out_len = 0;
    snprintf(diag, dsz, "%s exited with code %d", argv[0], code);
    return FMT_ERROR;
  }
  return FMT_OK;
}

/* Run argv with stdin from stdin_path, discarding all output. Enforces
   FMT_TIMEOUT_SEC. Returns FMT_OK only if the child exited 0 in time. */
static FormatStatus run_detached(char **argv, const char *stdin_path,
                                 char *diag, size_t dsz) {
  int in_fd = open(stdin_path, O_RDONLY);
  if (in_fd < 0) {
    snprintf(diag, dsz, "open temp input failed: %s", strerror(errno));
    return FMT_INTERNAL_ERROR;
  }

  SpawnedProc p;
  if (!proc_spawn(argv, in_fd, false, &p)) {
    close(in_fd);
    snprintf(diag, dsz, "fork failed: %s", strerror(errno));
    return FMT_INTERNAL_ERROR;
  }
  close(in_fd);

  uint64_t deadline = now_ms() + (uint64_t)FMT_TIMEOUT_SEC * 1000u;
  bool timed_out = false;
  int status = proc_wait(p.pid, deadline, &timed_out);

  if (timed_out) {
    snprintf(diag, dsz, "'%s' timed out after %d s", argv[0], FMT_TIMEOUT_SEC);
    return FMT_TIMEOUT;
  }
  if (status < 0) {
    snprintf(diag, dsz, "waitpid failed: %s", strerror(errno));
    return FMT_INTERNAL_ERROR;
  }
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (!WIFEXITED(status) || code != 0) {
    snprintf(diag, dsz, "%s exited with code %d", argv[0], code);
    return FMT_ERROR;
  }
  return FMT_OK;
}

/* Shared post-run classification for both runners: empty-output guard
   (never let a silent formatter wipe the document), unchanged check. */
static FormatResult classify_output(const FormatRule *rule, char *out,
                                    size_t out_len, const char *input,
                                    size_t input_len, FormatStatus st,
                                    const char *diag_in) {
  FormatResult r = {st, NULL, 0, ""};
  if (st != FMT_OK) {
    snprintf(r.diag, sizeof(r.diag), "%s", diag_in);
    return r;
  }
  if (input_len > 0 && out_len == 0) {
    free(out);
    r.status = FMT_ERROR;
    snprintf(r.diag, sizeof(r.diag),
             "%s produced empty output; keeping original text",
             rule->binary);
    return r;
  }
  if (out_len == input_len &&
      (input_len == 0 || memcmp(out, input, input_len) == 0)) {
    free(out);
    r.status = FMT_UNCHANGED;
    snprintf(r.diag, sizeof(r.diag), "%s: no changes", rule->binary);
    return r;
  }
  r.output = out;
  r.output_len = out_len;
  snprintf(r.diag, sizeof(r.diag), "formatted by %s", rule->binary);
  return r;
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

  /* Write input to a temp file; the child reads it as stdin. This avoids
     pipe deadlock on large inputs and keeps the command shell-free. */
  char tmp_in[512];
  int fd_in = create_tempfile(NULL, tmp_in, sizeof(tmp_in));
  if (fd_in < 0) {
    snprintf(r.diag, sizeof(r.diag), "mkstemp failed: %s", strerror(errno));
    return r;
  }

  size_t written = 0;
  while (written < input_len) {
    ssize_t w = write(fd_in, input + written, input_len - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      close(fd_in);
      unlink(tmp_in);
      snprintf(r.diag, sizeof(r.diag), "temp write failed: %s",
               strerror(errno));
      return r;
    }
    written += (size_t)w;
  }
  close(fd_in);

  char **argv = fmt_tokenize(rule->cmd_template, "-");
  if (!argv) {
    unlink(tmp_in);
    snprintf(r.diag, sizeof(r.diag), "out of memory building command");
    return r;
  }

  char *out = NULL;
  size_t out_len = 0;
  char diag[512] = "";
  FormatStatus st = run_captured(argv, tmp_in, &out, &out_len, diag,
                                 sizeof(diag));
  unlink(tmp_in);
  fmt_free_argv(argv);

  return classify_output(rule, out, out_len, input, input_len, st, diag);
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
      if (errno == EINTR)
        continue;
      close(fd);
      unlink(tmp_path);
      snprintf(r.diag, sizeof(r.diag), "temp write failed: %s",
               strerror(errno));
      return r;
    }
    written += (size_t)w;
  }
  close(fd);

  char **argv = fmt_tokenize(rule->cmd_template, tmp_path);
  if (!argv) {
    unlink(tmp_path);
    snprintf(r.diag, sizeof(r.diag), "out of memory building command");
    return r;
  }

  char diag[512] = "";
  FormatStatus st = run_detached(argv, "/dev/null", diag, sizeof(diag));
  fmt_free_argv(argv);

  if (st != FMT_OK) {
    unlink(tmp_path);
    r.status = st;
    snprintf(r.diag, sizeof(r.diag), "%s", diag);
    return r;
  }

  /* Read back the formatted file */
  FILE *fp = fopen(tmp_path, "rb");
  if (!fp) {
    unlink(tmp_path);
    snprintf(r.diag, sizeof(r.diag), "cannot read formatted output");
    return r;
  }
  size_t out_len = 0;
  char *out = read_all(fp, &out_len);
  fclose(fp);
  unlink(tmp_path);

  if (!out) {
    snprintf(r.diag, sizeof(r.diag), "out of memory reading formatted file");
    return r;
  }

  return classify_output(rule, out, out_len, input, input_len, FMT_OK, "");
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
