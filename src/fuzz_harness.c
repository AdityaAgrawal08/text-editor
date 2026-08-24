/* =====================================================================
 * fuzz_harness.c - in-tree dumb fuzzer for the EDOC storage parsers
 *
 * Targets (each wrapper owns full cleanup; production-identical paths):
 *   edoc    -> fuzz_parse_edoc     (.edoc container image)
 *   journal -> fuzz_journal_scan   (.journal WAL backward scan)
 *   history -> fuzz_history_parse  (embedded VERSIONS section)
 *
 * Usage:
 *   ./build/fuzz_harness [edoc|journal|history|all] [N] [SEED]
 *
 * Seeds are generated at startup through the PUBLIC storage API so
 * mutations tear apart real structure rather than pure noise; a
 * handcrafted VERSIONS-section blob covers the history parser.
 * Mutations include u32/u64 pokes aimed at the length fields parsers
 * trust most. Build with CC=clang + 'make fuzz-libfuzzer' for the
 * coverage-guided libFuzzer variant (-DLIBFUZZER swaps main() out).
 * ===================================================================== */
#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

StorageStatus fuzz_parse_edoc(const uint8_t *data, size_t len);
StorageStatus fuzz_journal_scan(const uint8_t *data, size_t len);
StorageStatus fuzz_history_parse(const uint8_t *data, size_t len, size_t *out_count);

#define MAX_INPUT (64u * 1024u)

#ifndef LIBFUZZER

/* Per-process seed workspace: parallel campaigns (CI matrix jobs, a
   second terminal) must never race on the same temp files. */
static char g_seed_path[128];
static void seed_path_init(void) {
  snprintf(g_seed_path, sizeof(g_seed_path), "/tmp/palimpsest_fuzz_%ld.edoc",
           (long)getpid());
}

/* ------------------------------------------------------------------ *
 * Deterministic RNG (xorshift64*): same SEED => identical campaign
 * ------------------------------------------------------------------ */
static uint64_t g_rng;
static uint64_t rng_next(void) {
  g_rng ^= g_rng >> 12;
  g_rng ^= g_rng << 25;
  g_rng ^= g_rng >> 27;
  return g_rng * 2685821657736338717ULL;
}
static uint32_t rng_below(uint32_t n) {
  return (uint32_t)(rng_next() % n);
}

typedef struct {
  uint8_t *data;
  size_t len;
} Blob;

static Blob g_seed_edoc;      /* document + metadata sections */
static Blob g_seed_edoc_hist; /* + VERSIONS section           */
static Blob g_seed_journal;   /* three valid WAL records      */
static Blob g_seed_history;   /* handcrafted VERSIONS payload */

static Blob read_file_bytes(const char *path) {
  Blob b = {NULL, 0};
  FILE *f = fopen(path, "rb");
  if (!f)
    return b;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  if (sz > 0 && (size_t)sz <= MAX_INPUT * 4) {
    b.data = malloc((size_t)sz);
    if (b.data && fread(b.data, 1, (size_t)sz, f) == (size_t)sz)
      b.len = (size_t)sz;
    else {
      free(b.data);
      b.data = NULL;
    }
  }
  fclose(f);
  return b;
}

/* CRC32 (IEEE 802.3) mirror for handcrafting history records */
static uint32_t crc32_of(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}

static void le32(Blob *b, uint32_t v) {
  b->data[b->len++] = (uint8_t)v;
  b->data[b->len++] = (uint8_t)(v >> 8);
  b->data[b->len++] = (uint8_t)(v >> 16);
  b->data[b->len++] = (uint8_t)(v >> 24);
}
static void le64(Blob *b, uint64_t v) {
  for (int i = 0; i < 8; i++)
    b->data[b->len++] = (uint8_t)(v >> (8 * i));
}

static void gen_history_seed(void) {
  /* Record layout (see storage.c): magic,id,created_at,name_len,name,
   * doc_len,doc_crc32,doc_bytes,rec_crc32(body). Three records with
   * mixed content force multi-record parsing paths. */
  const char *names[3] = {"v1", "before-main-draft", ""};
  const char *docs[3] = {"alpha body", "beta body", ""};

  Blob b = {calloc(MAX_INPUT, 1), 0};
  for (int r = 0; r < 3; r++) {
    size_t body_start = b.len;
    le32(&b, 0x31524556u); /* "VER1" little-endian */
    le64(&b, (uint64_t)(r + 1));
    le64(&b, (uint64_t)1700000000 + r);
    uint32_t nlen = (uint32_t)strlen(names[r]);
    le32(&b, nlen);
    memcpy(b.data + b.len, names[r], nlen);
    b.len += nlen;
    size_t dlen = strlen(docs[r]);
    le64(&b, (uint64_t)dlen);
    le32(&b, crc32_of((const uint8_t *)docs[r], dlen));
    memcpy(b.data + b.len, docs[r], dlen);
    b.len += dlen;
    le32(&b, crc32_of(b.data + body_start, b.len - body_start));
  }
  g_seed_history = b;
}

static void remove_seeds(void) {
  char p[280];
  snprintf(p, sizeof(p), "%s", g_seed_path);
  unlink(p);
  snprintf(p, sizeof(p), "%s.journal", g_seed_path);
  unlink(p);
  snprintf(p, sizeof(p), "%s.autosave", g_seed_path);
  unlink(p);
  for (int i = 0; i < STORAGE_MAX_BACKUPS; i++) {
    char bp[300];
    snprintf(bp, sizeof(bp), "%s.bak.%d", g_seed_path, i);
    unlink(bp);
  }
}

static int gen_public_api_seeds(void) {
  StorageSession *s = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;

  remove_seeds();

  if (storage_session_open(g_seed_path, &s, &doc, &meta, &res) != STORAGE_OK)
    return -1;
  bytebuffer_free(&doc);
  memset(&meta, 0, sizeof(meta));
  meta.created_at = 1700000000;
  meta.modified_at = meta.created_at;
  meta.schema_version = 1;

  const char *text1 = "int main() {\n    return 0;\n}\n";
  bytebuffer_init(&doc);
  bytebuffer_append(&doc, text1, strlen(text1));
  if (storage_save(s, g_seed_path, &doc, &meta) != STORAGE_OK) {
    bytebuffer_free(&doc);
    storage_session_close(s);
    return -1;
  }
  g_seed_edoc = read_file_bytes(g_seed_path);

  const char *text2 = "int main(void) {\n    return 1;\n}\n";
  bytebuffer_free(&doc); /* release text1's buffer before reusing */
  bytebuffer_init(&doc);
  bytebuffer_append(&doc, text2, strlen(text2));
  if (storage_save(s, g_seed_path, &doc, &meta) != STORAGE_OK) {
    bytebuffer_free(&doc);
    storage_session_close(s);
    return -1;
  }
  g_seed_edoc_hist = read_file_bytes(g_seed_path);
  bytebuffer_free(&doc);

  /* Leave a non-empty journal behind: close without saving. */
  ByteBuffer edit;
  bytebuffer_init(&edit);
  bytebuffer_append(&edit, "pending edit one", 16);
  storage_journal_append(s, "insert", &edit);
  bytebuffer_free(&edit);
  storage_session_close(s);

  char jpath[300];
  snprintf(jpath, sizeof(jpath), "%s.journal", g_seed_path);
  g_seed_journal = read_file_bytes(jpath);

  remove_seeds();
  return (g_seed_edoc.data && g_seed_edoc_hist.data && g_seed_journal.data)
             ? 0
             : -1;
}

/* ------------------------------------------------------------------ *
 * Mutations
 * ------------------------------------------------------------------ */
static void mutate(uint8_t *buf, size_t *len) {
  int rounds = 1 + (int)rng_below(8);
  for (int i = 0; i < rounds && *len > 0; i++) {
    switch (rng_below(9)) {
    case 0: /* single bit flip */
      buf[rng_below((uint32_t)*len)] ^= (uint8_t)(1u << rng_below(8));
      break;
    case 1: { /* interesting byte */
      static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE};
      buf[rng_below((uint32_t)*len)] = interesting[rng_below(6)];
      break;
    }
    case 2: /* fully random byte */
      buf[rng_below((uint32_t)*len)] = (uint8_t)rng_next();
      break;
    case 3: /* truncate */
      *len = rng_below((uint32_t)*len);
      break;
    case 4: /* delete small span */
      if (*len > 4) {
        size_t at = rng_below((uint32_t)(*len - 1));
        size_t n = 1 + rng_below(8);
        if (at + n < *len) {
          memmove(buf + at, buf + at + n, *len - at - n);
          *len -= n;
        } else {
          *len = at;
        }
      }
      break;
    case 5: /* duplicate chunk to tail */
      if (*len > 8 && *len + 16 < MAX_INPUT) {
        size_t at = rng_below((uint32_t)(*len / 2));
        size_t n = 1 + rng_below(16);
        if (at + n <= *len) {
          memmove(buf + *len, buf + at, n);
          *len += n;
        }
      }
      break;
    case 6: { /* u32 length-field poke */
      if (*len >= 8) {
        size_t at = rng_below((uint32_t)(*len - 4));
        uint32_t vals[5] = {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu,
                            (uint32_t)*len + rng_below(64)};
        memcpy(buf + at, &vals[rng_below(5)], 4);
      }
      break;
    }
    case 7: { /* u64 length-field poke */
      if (*len >= 16) {
        size_t at = rng_below((uint32_t)(*len - 8));
        uint64_t v = (rng_below(2)) ? UINT64_MAX : (uint64_t)*len + 4096;
        memcpy(buf + at, &v, 8);
      }
      break;
    }
    case 8: { /* splice tail from a different seed */
      if (*len > 16 && *len + 128 < MAX_INPUT) {
        Blob *src = (rng_below(2)) ? &g_seed_edoc_hist : &g_seed_journal;
        if (src->len > 64) {
          size_t from = rng_below((uint32_t)(src->len / 2));
          size_t n = 1 + rng_below(128);
          if (from + n <= src->len) {
            memcpy(buf + *len, src->data + from, n);
            *len += n;
          }
        }
      }
      break;
    }
    }
  }
}

enum { T_EDOC = 1, T_JOURNAL = 2, T_HISTORY = 4 };

static size_t load_base(uint8_t *dst, unsigned mask) {
  uint32_t roll = rng_below(100);
  if (roll < 4)
    return 0; /* empty input */
  if (roll < 20) {
    size_t n = 1 + rng_below(MAX_INPUT);
    for (size_t i = 0; i < n; i++)
      dst[i] = (uint8_t)rng_next();
    return n; /* pure noise */
  }

  Blob *pool[4];
  int n = 0;
  if (mask & T_EDOC) {
    pool[n++] = &g_seed_edoc;
    pool[n++] = &g_seed_edoc_hist;
  }
  if (mask & T_JOURNAL)
    pool[n++] = &g_seed_journal;
  if (mask & T_HISTORY)
    pool[n++] = &g_seed_history;
  Blob *src = pool[rng_below((uint32_t)n)];
  size_t cpy = src->len < MAX_INPUT ? src->len : MAX_INPUT;
  memcpy(dst, src->data, cpy);
  return cpy;
}

/* ------------------------------------------------------------------ *
 * Exec + outcome histogram
 * ------------------------------------------------------------------ */
#define HIST_SIZE (STORAGE_ERR_LOCK_FAILED + 1)
typedef struct {
  uint64_t counts[HIST_SIZE];
} Hist;

static void run_target(unsigned t, const uint8_t *data, size_t len, Hist *h) {
  StorageStatus st = STORAGE_OK;
  switch (t) {
  case T_EDOC:
    st = fuzz_parse_edoc(data, len);
    break;
  case T_JOURNAL:
    st = fuzz_journal_scan(data, len);
    break;
  case T_HISTORY:
    st = fuzz_history_parse(data, len, NULL);
    break;
  }
  if (st >= 0 && (size_t)st < HIST_SIZE)
    h->counts[st]++;
}

#else /* LIBFUZZER: coverage-guided entry, no corpus machinery needed */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  uint8_t copy[MAX_INPUT];
  if (size > MAX_INPUT)
    size = MAX_INPUT;
  memcpy(copy, data, size);
  fuzz_parse_edoc(copy, size);
  fuzz_journal_scan(copy, size);
  fuzz_history_parse(copy, size, NULL);
  return 0;
}

#endif /* LIBFUZZER */

#ifndef LIBFUZZER

static const char *tname(unsigned t) {
  return t == T_EDOC   ? "edoc"
         : t == T_JOURNAL ? "journal"
                          : "history";
}

static void print_hist(const char *label, const Hist *h) {
  fprintf(stderr, "  %-8s outcomes:", label);
  for (int s = 0; s < HIST_SIZE; s++)
    if (h->counts[s])
      fprintf(stderr, " %s=%llu", storage_status_string((StorageStatus)s),
              (unsigned long long)h->counts[s]);
  fputc('\n', stderr);
}

int main(int argc, char **argv) {
  const char *what = (argc > 1) ? argv[1] : "all";
  long n_execs = (argc > 2) ? atol(argv[2]) : 100000L;
  uint64_t seed = (argc > 3) ? strtoull(argv[3], NULL, 0) : 42ULL;
  if (n_execs < 1)
    n_execs = 1;
  g_rng = seed ? seed : 1;

  seed_path_init();
  gen_history_seed();
  if (gen_public_api_seeds() != 0) {
    fprintf(stderr, "fatal: could not generate seed corpus\n");
    free(g_seed_edoc.data);
    free(g_seed_edoc_hist.data);
    free(g_seed_journal.data);
    free(g_seed_history.data);
    return 1;
  }
  fprintf(stderr,
          "seeds: edoc=%zuB edoc+hist=%zuB journal=%zuB history=%zuB\n",
          g_seed_edoc.len, g_seed_edoc_hist.len, g_seed_journal.len,
          g_seed_history.len);

  unsigned targets[3];
  int nt = 0;
  if (strcmp(what, "all") == 0 || strcmp(what, "edoc") == 0)
    targets[nt++] = T_EDOC;
  if (strcmp(what, "all") == 0 || strcmp(what, "journal") == 0)
    targets[nt++] = T_JOURNAL;
  if (strcmp(what, "all") == 0 || strcmp(what, "history") == 0)
    targets[nt++] = T_HISTORY;
  if (nt == 0) {
    fprintf(stderr, "usage: %s [edoc|journal|history|all] [N] [SEED]\n",
            argv[0]);
    return 1;
  }

  Hist hists[3];
  memset(hists, 0, sizeof(hists));
  uint8_t buf[MAX_INPUT];

  long progress = n_execs / 10;
  if (progress < 1)
    progress = 1;
  for (long i = 0; i < n_execs; i++) {
    unsigned t = targets[i % (unsigned)nt];
    Hist *h = &hists[i % (unsigned)nt];
    size_t len = load_base(buf, t);
    mutate(buf, &len);
    run_target(t, buf, len, h);
    if ((i + 1) % progress == 0)
      fprintf(stderr, "[%s] %ld/%ld execs\n", tname(t), i + 1, n_execs);
  }

  fprintf(stderr, "\ndone: %ld execs, seed=%llu\n", n_execs,
          (unsigned long long)seed);
  for (int k = 0; k < nt; k++)
    print_hist(tname(targets[k]), &hists[k]);

  free(g_seed_edoc.data);
  free(g_seed_edoc_hist.data);
  free(g_seed_journal.data);
  free(g_seed_history.data);
  return 0;
}

#endif /* !LIBFUZZER */
