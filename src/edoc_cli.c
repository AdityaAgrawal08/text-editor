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

static const char *g_prog = "edoc";

static int usage(void) {
  fprintf(stderr,
          "usage: %s <command> [args]\n"
          "\n"
          "commands:\n"
          "  verify FILE                  full checksum audit\n"
          "  dump FILE                    structural section table\n"
          "  history FILE                 list embedded versions\n"
          "  export FILE [-v N|--name X] OUT\n"
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

  fprintf(stderr, "%s: unknown command '%s'\n", g_prog, cmd);
  return usage();
}
