/* =====================================================================
 * edoc - command-line toolkit for the EDOC document container
 *
 * Links only against storage.c; shares nothing with the editor binary.
 * Every command is read-only unless explicitly creating a container
 * (import). Exit codes:
 *   0  success
 *   1  usage error
 *   2  verification failed (verify only)
 *   3  I/O or format error
 *
 * Commands: verify | dump | history | export | import | recover
 * ===================================================================== */
#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *g_prog = "edoc";

static int usage(void) {
  fprintf(stderr,
          "usage: %s <command> [args]\n"
          "\n"
          "commands:\n"
          "  verify FILE                  full checksum audit\n"
          "  dump FILE                    structural section table\n"
          "  history FILE                 list embedded versions\n"
          "  export FILE [-v ID|--name X] OUT\n"
          "                               write document/version as plain UTF-8\n"
          "  import TXT FILE [--force]    wrap plain text into a new container\n"
          "  recover FILE                 report pending journal/autosave state\n"
          "\n"
          "exit codes: 0 ok, 1 usage, 2 verification failed, 3 I/O/format\n",
          g_prog);
  return 1;
}

static const char *section_name(uint32_t type) {
  switch (type) {
  case STORAGE_SECTION_DOCUMENT:
    return "DOCUMENT";
  case STORAGE_SECTION_METADATA:
    return "METADATA";
  case STORAGE_SECTION_JOURNAL:
    return "JOURNAL";
  case STORAGE_SECTION_VERSIONS:
    return "VERSIONS";
  default:
    return "UNKNOWN";
  }
}

static void fmt_ts(uint64_t unix_s, char *out, size_t cap) {
  time_t tt = (time_t)unix_s;
  struct tm tmv;
  localtime_r(&tt, &tmv);
  strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
}

/* ------------------------------------------------------------------ */
/* verify                                                              */
/* ------------------------------------------------------------------ */
static int cmd_verify(int argc, char **argv) {
  if (argc != 1)
    return usage();
  StorageStatus st = storage_verify_file(argv[0]);
  if (st == STORAGE_OK) {
    printf("OK   %s: all checksums valid\n", argv[0]);
    return 0;
  }
  printf("FAIL %s: %s\n", argv[0], storage_status_string(st));
  return st == STORAGE_ERR_CORRUPT_CHECKSUM ? 2 : 3;
}

/* ------------------------------------------------------------------ */
/* dump                                                                */
/* ------------------------------------------------------------------ */
typedef struct {
  size_t idx;
} DumpCtx;

static void dump_section(const StorageSectionInfo *info, void *user) {
  DumpCtx *c = user;
  printf("  [%zu] %-9s off=%-8llu len=%-8llu crc=%s\n", c->idx++,
         section_name(info->type), (unsigned long long)info->payload_off,
         (unsigned long long)info->payload_len,
         info->crc_ok ? "ok" : "BAD");
}

static int cmd_dump(int argc, char **argv) {
  if (argc != 1)
    return usage();
  StorageInspectSummary sum;
  DumpCtx ctx = {0};
  StorageStatus st = storage_inspect_file(argv[0], &sum, dump_section, &ctx);
  if (st == STORAGE_ERR_BAD_MAGIC ||
      st == STORAGE_ERR_UNSUPPORTED_VERSION ||
      st == STORAGE_ERR_NOT_FOUND || st == STORAGE_ERR_IO ||
      st == STORAGE_ERR_NOMEM) {
    fprintf(stderr, "%s: %s\n", argv[0], storage_status_string(st));
    return 3;
  }

  char ts[32];
  fmt_ts(sum.header_created_at, ts, sizeof(ts));
  printf("%s\n", argv[0]);
  printf("  format_version : %u%s\n", sum.format_version,
         sum.format_version == STORAGE_FORMAT_VERSION ? ""
                                                      : "  (unsupported)");
  printf("  created_at     : %s\n", ts);
  printf("  file_size      : %llu bytes\n",
         (unsigned long long)sum.file_size);
  printf("  footer_crc     : %s\n", sum.footer_crc_ok ? "ok" : "BAD");
  printf("  sections       : %u claimed, %zu walked\n", sum.section_count,
         sum.sections_walked);
  if (st == STORAGE_ERR_TRUNCATED)
    printf("  integrity      : walk stopped early (%s)\n",
           storage_status_string(st));
  else if (st != STORAGE_OK)
    printf("  integrity      : %s\n", storage_status_string(st));
  return st == STORAGE_OK ? 0 : 3;
}

/* ------------------------------------------------------------------ */
/* history                                                             */
/* ------------------------------------------------------------------ */
static int cmd_history(int argc, char **argv) {
  if (argc != 1)
    return usage();
  StorageVersion *vers = NULL;
  size_t n = 0;
  StorageStatus st = storage_read_versions(argv[0], &vers, &n);
  if (st != STORAGE_OK) {
    fprintf(stderr, "%s: %s\n", argv[0], storage_status_string(st));
    return 3;
  }
  if (n == 0) {
    printf("%s: no embedded versions\n", argv[0]);
    return 0;
  }
  printf("%-8s %-20s %10s  %s\n", "VERSION", "SAVED", "SIZE", "ID");
  for (size_t i = n; i-- > 0;) { /* newest first */
    char ts[32];
    fmt_ts(vers[i].created_at, ts, sizeof(ts));
    char when[48];
    snprintf(when, sizeof(when), "%s", ts);
    printf("%-8s %-20s %8zu B  %llu\n",
           vers[i].name[0] ? vers[i].name : "(unnamed)", when,
           vers[i].doc.len, (unsigned long long)vers[i].id);
  }
  storage_versions_free(vers, n);
  return 0;
}

/* ------------------------------------------------------------------ */
/* export                                                              */
/* ------------------------------------------------------------------ */
static int cmd_export(int argc, char **argv) {
  const char *file = NULL, *out = NULL;
  long want_id = -1; /* -v ID: selects by version id (matches history) */
  const char *want_name = NULL;

  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
      want_id = atol(argv[++i]);
      if (want_id < 1)
        return usage();
    } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      want_name = argv[++i];
    } else if (!file) {
      file = argv[i];
    } else if (!out) {
      out = argv[i];
    } else {
      return usage();
    }
  }
  if (!file || !out)
    return usage();

  ByteBuffer doc;
  bytebuffer_init(&doc);
  if (want_id < 0 && !want_name) {
    StorageStatus st = storage_read_document(file, &doc);
    if (st != STORAGE_OK) {
      fprintf(stderr, "%s: %s\n", file, storage_status_string(st));
      return 3;
    }
  } else {
    StorageVersion *vers = NULL;
    size_t n = 0;
    StorageStatus st = storage_read_versions(file, &vers, &n);
    if (st != STORAGE_OK) {
      fprintf(stderr, "%s: %s\n", file, storage_status_string(st));
      return 3;
    }
    size_t pick = n; /* sentinel: not found */
    for (size_t i = 0; i < n && pick == n; i++) {
      bool hit = want_name ? (strcmp(vers[i].name, want_name) == 0)
                           : ((long)vers[i].id == want_id);
      if (hit)
        pick = i;
    }
    if (pick == n) {
      fprintf(stderr, "%s: no such version%s%s\n", file,
              want_name ? ": " : "", want_name ? want_name : "");
      storage_versions_free(vers, n);
      return 3;
    }
    bytebuffer_append(&doc, vers[pick].doc.data, vers[pick].doc.len);
    storage_versions_free(vers, n);
  }

  FILE *f = fopen(out, "wb");
  if (!f) {
    perror(out);
    bytebuffer_free(&doc);
    return 3;
  }
  size_t total = doc.len;
  size_t w = fwrite(doc.data, 1, total, f);
  fclose(f);
  bytebuffer_free(&doc);
  if (w != total) {
    fprintf(stderr, "%s: short write\n", out);
    return 3;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* import                                                              */
/* ------------------------------------------------------------------ */
static int cmd_import(int argc, char **argv) {
  const char *txt = NULL, *file = NULL;
  bool force = false;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--force") == 0)
      force = true;
    else if (!txt)
      txt = argv[i];
    else if (!file)
      file = argv[i];
    else
      return usage();
  }
  if (!txt || !file)
    return usage();

  FILE *probe = fopen(file, "rb");
  if (probe) {
    fclose(probe);
    if (!force) {
      fprintf(stderr,
              "%s: already exists (use --force to overwrite)\n", file);
      return 3;
    }
    unlink(file);
  }

  /* Read the plain-text source fully. */
  FILE *f = fopen(txt, "rb");
  if (!f) {
    perror(txt);
    return 3;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  ByteBuffer body;
  bytebuffer_init(&body);
  if (sz > 0 &&
      (!bytebuffer_reserve(&body, (size_t)sz) ||
       fread(body.data, 1, (size_t)sz, f) != (size_t)sz)) {
    fprintf(stderr, "%s: read failed\n", txt);
    fclose(f);
    bytebuffer_free(&body);
    return 3;
  }
  body.len = (size_t)(sz > 0 ? sz : 0);
  fclose(f);

  /* Session open is fine here: import IS a write operation. */
  StorageSession *s = NULL;
  ByteBuffer loaded;
  StorageMetadata meta;
  StorageOpenResult res;
  StorageStatus st = storage_session_open(file, &s, &loaded, &meta, &res);
  if (st != STORAGE_OK) {
    fprintf(stderr, "%s: %s\n", file, storage_status_string(st));
    bytebuffer_free(&body);
    return 3;
  }
  bytebuffer_free(&loaded);

  /* Title from the text filename. */
  const char *base = strrchr(txt, '/');
  base = base ? base + 1 : txt;
  snprintf(meta.title, sizeof(meta.title), "%.250s", base);

  st = storage_save(s, file, &body, &meta);
  size_t total = body.len;
  storage_session_close(s);
  bytebuffer_free(&body);
  if (st != STORAGE_OK) {
    fprintf(stderr, "%s: %s\n", file, storage_status_string(st));
    return 3;
  }
  printf("wrote %s (%zu bytes of text)\n", file, total);
  return 0;
}

int main(int argc, char **argv) {
  g_prog = (argc > 0 && argv[0]) ? argv[0] : "edoc";
  {
    const char *slash = strrchr(g_prog, '/');
    if (slash)
      g_prog = slash + 1;
  }

  if (argc < 2)
    return usage();

  const char *cmd = argv[1];
  if (strcmp(cmd, "verify") == 0)
    return cmd_verify(argc - 2, argv + 2);
  if (strcmp(cmd, "dump") == 0)
    return cmd_dump(argc - 2, argv + 2);
  if (strcmp(cmd, "history") == 0)
    return cmd_history(argc - 2, argv + 2);
  if (strcmp(cmd, "export") == 0)
    return cmd_export(argc - 2, argv + 2);
  if (strcmp(cmd, "import") == 0)
    return cmd_import(argc - 2, argv + 2);

  fprintf(stderr, "%s: unknown command '%s'\n", g_prog, cmd);
  return usage();
}
