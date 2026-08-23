#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ===================================================================== *
 * ByteBuffer
 * ===================================================================== */

void bytebuffer_init(ByteBuffer *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

bool bytebuffer_reserve(ByteBuffer *b, size_t additional) {
  size_t needed = b->len + additional;
  if (needed <= b->cap)
    return true;
  size_t new_cap = b->cap ? b->cap : 256;
  while (new_cap < needed) {
    if (new_cap > (SIZE_MAX / 2)) {
      new_cap = needed;
      break;
    }
    new_cap *= 2;
  }
  uint8_t *new_data = realloc(b->data, new_cap);
  if (!new_data)
    return false;
  b->data = new_data;
  b->cap = new_cap;
  return true;
}

bool bytebuffer_append(ByteBuffer *b, const void *data, size_t len) {
  if (len == 0)
    return true;
  if (!bytebuffer_reserve(b, len))
    return false;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return true;
}

bool bytebuffer_append_u32(ByteBuffer *b, uint32_t v) {
  uint8_t le[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                   (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF)};
  return bytebuffer_append(b, le, 4);
}

bool bytebuffer_append_u64(ByteBuffer *b, uint64_t v) {
  uint8_t le[8];
  for (int i = 0; i < 8; i++)
    le[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
  return bytebuffer_append(b, le, 8);
}

void bytebuffer_free(ByteBuffer *b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

/* ===================================================================== *
 * Little-endian primitive readers (format is LE on disk regardless of
 * host endianness, so this code is portable to BE hosts too).
 * ===================================================================== */

static uint32_t read_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v |= ((uint64_t)p[i]) << (8 * i);
  return v;
}

/* ===================================================================== *
 * CRC32 (IEEE 802.3 polynomial), table-driven, generated at first use.
 * ===================================================================== */

static uint32_t g_crc32_table[256];
static bool g_crc32_table_ready = false;

static void crc32_init_table(void) {
  if (g_crc32_table_ready)
    return;
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    g_crc32_table[i] = c;
  }
  g_crc32_table_ready = true;
}

static uint32_t crc32_compute(const uint8_t *data, size_t len) {
  crc32_init_table();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++)
    c = g_crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ===================================================================== *
 * On-disk structure constants
 *
 * FileHeader:   u32 magic, u32 format_version, u64 created_at  (16 bytes)
 * SectionHeader:u32 type, u64 payload_len, u32 payload_crc32   (16 bytes)
 * FileFooter:   u32 section_count, u32 footer_crc32            (8 bytes)
 * ===================================================================== */

#define FILE_HEADER_SIZE 16
#define SECTION_HEADER_SIZE 16
#define FILE_FOOTER_SIZE 8

typedef struct {
  StorageSectionType type;
  ByteBuffer payload;
} Section;

struct StorageSession {
  char path[STORAGE_PATH_MAX];
  char journal_path[STORAGE_PATH_MAX];
  char autosave_path[STORAGE_PATH_MAX];
  bool dirty;
  uint32_t autosave_interval_ms;
  uint32_t last_autosave_attempt_ms;
  bool has_attempted_autosave; /* false until the first tick; guards against
                                    the now_ms==0 edge case where a zero-
                                    initialized last_autosave_attempt_ms would
                                    otherwise make (now_ms - last) appear to
                                    be within the debounce window even though
                                    no autosave has ever actually run */
  bool has_recovery_candidate;
  ByteBuffer recovery_document;
  StorageMetadata recovery_meta;
  int journal_fd; /* kept open, append-only, for low-latency journaling */

  /* Embedded version history (oldest-first in memory; public accessors
     reverse the order so index 0 = newest). */
  StorageVersion *hist;
  size_t hist_len;
  size_t hist_cap;
  uint64_t hist_next_id;
  bool hist_dirty; /* renames/deletes awaiting persistence */
};

/* ===================================================================== *
 * Path helpers
 * ===================================================================== */

static void derive_sibling_path(const char *path, const char *suffix, char *out,
                                size_t out_size) {
  snprintf(out, out_size, "%s%s", path, suffix);
}

/* ===================================================================== *
 * Low-level atomic write: write full buffer to a uniquely-named temp file
 * in the same directory as `final_path`, fsync it, then rename() over
 * final_path. Same-filesystem rename is atomic on POSIX, so readers never
 * observe a partial file.
 * ===================================================================== */

static StorageStatus atomic_write_file(const char *final_path,
                                       const uint8_t *data, size_t len) {
  char tmp_path[STORAGE_PATH_MAX];
  pid_t pid = getpid();
  unsigned int salt = (unsigned int)time(NULL) ^ (unsigned int)pid;
  int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d.%x", final_path,
                   (int)pid, salt);
  if (n < 0 || (size_t)n >= sizeof(tmp_path))
    return STORAGE_ERR_INVALID_ARG;

  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return STORAGE_ERR_IO;

  size_t written = 0;
  while (written < len) {
    ssize_t w = write(fd, data + written, len - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      unlink(tmp_path);
      return STORAGE_ERR_IO;
    }
    written += (size_t)w;
  }

  if (fsync(fd) != 0) {
    close(fd);
    unlink(tmp_path);
    return STORAGE_ERR_IO;
  }
  if (close(fd) != 0) {
    unlink(tmp_path);
    return STORAGE_ERR_IO;
  }

  if (rename(tmp_path, final_path) != 0) {
    unlink(tmp_path);
    return STORAGE_ERR_RENAME_FAILED;
  }

  /* Best-effort fsync of the containing directory so the rename itself
     is durable across a crash, not just the file content. Not fatal if
     unsupported (some filesystems/platforms reject O_DIRECTORY opens
     for fsync purposes); we don't fail the save over it. */
  char dir_path[STORAGE_PATH_MAX];
  snprintf(dir_path, sizeof(dir_path), "%s", final_path);
  char *slash = strrchr(dir_path, '/');
  if (slash) {
    *slash = '\0';
    int dfd = open(dir_path, O_RDONLY);
    if (dfd >= 0) {
      fsync(dfd);
      close(dfd);
    }
  }

  return STORAGE_OK;
}

static StorageStatus read_whole_file(const char *path, ByteBuffer *out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return STORAGE_ERR_NOT_FOUND;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return STORAGE_ERR_IO;
  }
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return STORAGE_ERR_IO;
  }
  rewind(f);

  bytebuffer_init(out);
  if (size > 0 && !bytebuffer_reserve(out, (size_t)size)) {
    fclose(f);
    return STORAGE_ERR_NOMEM;
  }

  size_t total_read = 0;
  while (total_read < (size_t)size) {
    size_t r = fread(out->data + total_read, 1, (size_t)size - total_read, f);
    if (r == 0) {
      if (feof(f))
        break;
      fclose(f);
      bytebuffer_free(out);
      return STORAGE_ERR_IO;
    }
    total_read += r;
  }
  out->len = total_read;
  fclose(f);
  return STORAGE_OK;
}

/* ===================================================================== *
 * EDOC serialization
 * ===================================================================== */

/* History serde lives below the journal section; forward decls for the
   image builder/parser above. */
static void history_free(StorageVersion *hist, size_t len);
static void serialize_history_section(const StorageVersion *hist, size_t len,
                                      ByteBuffer *out);
static StorageStatus parse_history_section(const uint8_t *data, size_t len,
                                           StorageVersion **out_hist,
                                           size_t *out_len);

static void serialize_metadata(const StorageMetadata *meta, ByteBuffer *out) {
  bytebuffer_init(out);
  uint32_t title_len = (uint32_t)strnlen(meta->title, sizeof(meta->title));
  uint32_t author_len = (uint32_t)strnlen(meta->author, sizeof(meta->author));
  bytebuffer_append_u32(out, title_len);
  bytebuffer_append(out, meta->title, title_len);
  bytebuffer_append_u32(out, author_len);
  bytebuffer_append(out, meta->author, author_len);
  bytebuffer_append_u64(out, meta->created_at);
  bytebuffer_append_u64(out, meta->modified_at);
  bytebuffer_append_u32(out, meta->schema_version);
  bytebuffer_append_u64(out, meta->revision_id);
}

static bool deserialize_metadata(const uint8_t *data, size_t len,
                                 StorageMetadata *out) {
  memset(out, 0, sizeof(*out));
  size_t off = 0;
  if (off + 4 > len)
    return false;
  uint32_t title_len = read_u32_le(data + off);
  off += 4;
  if (title_len >= sizeof(out->title) || off + title_len > len)
    return false;
  memcpy(out->title, data + off, title_len);
  out->title[title_len] = '\0';
  off += title_len;

  if (off + 4 > len)
    return false;
  uint32_t author_len = read_u32_le(data + off);
  off += 4;
  if (author_len >= sizeof(out->author) || off + author_len > len)
    return false;
  memcpy(out->author, data + off, author_len);
  out->author[author_len] = '\0';
  off += author_len;

  if (off + 8 > len)
    return false;
  out->created_at = read_u64_le(data + off);
  off += 8;
  if (off + 8 > len)
    return false;
  out->modified_at = read_u64_le(data + off);
  off += 8;
  if (off + 4 > len)
    return false;
  out->schema_version = read_u32_le(data + off);
  off += 4;
  if (off + 8 > len)
    return false;
  out->revision_id = read_u64_le(data + off);
  off += 8;
  return true;
}

/* Builds a complete EDOC file image in memory: header + sections + footer.
   `history` may be NULL (autosave files carry no version section).
   Caller owns and frees the returned ByteBuffer. */
static StorageStatus build_edoc_image(const ByteBuffer *document,
                                      const StorageMetadata *meta,
                                      const ByteBuffer *journal_tail,
                                      const StorageVersion *hist,
                                      size_t hist_len,
                                      ByteBuffer *out_image) {
  bytebuffer_init(out_image);

  uint64_t created = (uint64_t)time(NULL);
  if (!bytebuffer_append_u32(out_image, STORAGE_MAGIC))
    goto nomem;
  if (!bytebuffer_append_u32(out_image, STORAGE_FORMAT_VERSION))
    goto nomem;
  if (!bytebuffer_append_u64(out_image, created))
    goto nomem;

  int section_count = 0;

  /* SECTION_DOCUMENT */
  {
    uint32_t crc = crc32_compute(document->data, document->len);
    if (!bytebuffer_append_u32(out_image, STORAGE_SECTION_DOCUMENT))
      goto nomem;
    if (!bytebuffer_append_u64(out_image, (uint64_t)document->len))
      goto nomem;
    if (!bytebuffer_append_u32(out_image, crc))
      goto nomem;
    if (!bytebuffer_append(out_image, document->data, document->len))
      goto nomem;
    section_count++;
  }

  /* SECTION_METADATA */
  {
    ByteBuffer meta_buf;
    serialize_metadata(meta, &meta_buf);
    uint32_t crc = crc32_compute(meta_buf.data, meta_buf.len);
    if (!bytebuffer_append_u32(out_image, STORAGE_SECTION_METADATA)) {
      bytebuffer_free(&meta_buf);
      goto nomem;
    }
    if (!bytebuffer_append_u64(out_image, (uint64_t)meta_buf.len)) {
      bytebuffer_free(&meta_buf);
      goto nomem;
    }
    if (!bytebuffer_append_u32(out_image, crc)) {
      bytebuffer_free(&meta_buf);
      goto nomem;
    }
    if (!bytebuffer_append(out_image, meta_buf.data, meta_buf.len)) {
      bytebuffer_free(&meta_buf);
      goto nomem;
    }
    bytebuffer_free(&meta_buf);
    section_count++;
  }

  /* SECTION_JOURNAL (optional — only present when caller supplies a
     non-empty pending-journal tail, e.g. when promoting an autosave) */
  if (journal_tail && journal_tail->len > 0) {
    uint32_t crc = crc32_compute(journal_tail->data, journal_tail->len);
    if (!bytebuffer_append_u32(out_image, STORAGE_SECTION_JOURNAL))
      goto nomem;
    if (!bytebuffer_append_u64(out_image, (uint64_t)journal_tail->len))
      goto nomem;
    if (!bytebuffer_append_u32(out_image, crc))
      goto nomem;
    if (!bytebuffer_append(out_image, journal_tail->data, journal_tail->len))
      goto nomem;
    section_count++;
  }

  /* SECTION_VERSIONS (optional — embedded git-like history) */
  if (hist && hist_len > 0) {
    ByteBuffer hist_buf;
    serialize_history_section(hist, hist_len, &hist_buf);
    uint32_t crc = crc32_compute(hist_buf.data, hist_buf.len);
    bool ok = bytebuffer_append_u32(out_image, STORAGE_SECTION_VERSIONS) &&
              bytebuffer_append_u64(out_image, (uint64_t)hist_buf.len) &&
              bytebuffer_append_u32(out_image, crc) &&
              bytebuffer_append(out_image, hist_buf.data, hist_buf.len);
    bytebuffer_free(&hist_buf);
    if (!ok)
      goto nomem;
    section_count++;
  }

  /* Footer: section_count + crc32 over everything written so far
     (header + all sections), giving whole-file tamper/corruption
     detection independent of the per-section checksums. */
  uint32_t footer_crc = crc32_compute(out_image->data, out_image->len);
  if (!bytebuffer_append_u32(out_image, (uint32_t)section_count))
    goto nomem;
  if (!bytebuffer_append_u32(out_image, footer_crc))
    goto nomem;

  return STORAGE_OK;

nomem:
  bytebuffer_free(out_image);
  return STORAGE_ERR_NOMEM;
}

/* Parses a raw EDOC byte image already loaded into memory. Verifies every
   checksum before returning anything. out_history may be NULL (caller
   doesn't want versions decoded, e.g. integrity verification). */
static StorageStatus parse_edoc_image(const uint8_t *data, size_t len,
                                      ByteBuffer *out_document,
                                      StorageMetadata *out_meta,
                                      StorageVersion **out_history,
                                      size_t *out_history_len) {
  if (out_history) {
    *out_history = NULL;
    *out_history_len = 0;
  }
  if (len < FILE_HEADER_SIZE + FILE_FOOTER_SIZE)
    return STORAGE_ERR_TRUNCATED;

  uint32_t magic = read_u32_le(data);
  if (magic != STORAGE_MAGIC)
    return STORAGE_ERR_BAD_MAGIC;

  uint32_t version = read_u32_le(data + 4);
  if (version != STORAGE_FORMAT_VERSION)
    return STORAGE_ERR_UNSUPPORTED_VERSION;

  /* Footer is the last 8 bytes. */
  uint32_t section_count = read_u32_le(data + len - 8);
  uint32_t footer_crc = read_u32_le(data + len - 4);
  uint32_t computed_footer_crc = crc32_compute(data, len - 8);
  if (computed_footer_crc != footer_crc)
    return STORAGE_ERR_CORRUPT_CHECKSUM;

  bool got_document = false;
  bool got_metadata = false;
  bytebuffer_init(out_document);
  memset(out_meta, 0, sizeof(*out_meta));

  size_t off = FILE_HEADER_SIZE;
  size_t end = len - 8; /* exclude footer */

  for (uint32_t i = 0; i < section_count; i++) {
    if (off + SECTION_HEADER_SIZE > end) {
      bytebuffer_free(out_document);
      return STORAGE_ERR_TRUNCATED;
    }

    uint32_t type = read_u32_le(data + off);
    uint64_t payload_len = read_u64_le(data + off + 4);
    uint32_t payload_crc = read_u32_le(data + off + 12);
    off += SECTION_HEADER_SIZE;

    if (off + payload_len > end) {
      bytebuffer_free(out_document);
      return STORAGE_ERR_TRUNCATED;
    }

    uint32_t computed_crc = crc32_compute(data + off, (size_t)payload_len);
    if (computed_crc != payload_crc) {
      bytebuffer_free(out_document);
      return STORAGE_ERR_CORRUPT_CHECKSUM;
    }

    if (type == STORAGE_SECTION_DOCUMENT) {
      if (!bytebuffer_append(out_document, data + off, (size_t)payload_len)) {
        bytebuffer_free(out_document);
        return STORAGE_ERR_NOMEM;
      }
      got_document = true;
    } else if (type == STORAGE_SECTION_METADATA) {
      if (!deserialize_metadata(data + off, (size_t)payload_len, out_meta)) {
        bytebuffer_free(out_document);
        return STORAGE_ERR_TRUNCATED;
      }
      got_metadata = true;
    } else if (type == STORAGE_SECTION_VERSIONS && out_history &&
               *out_history == NULL) {
      StorageVersion *vh = NULL;
      size_t vlen = 0;
      parse_history_section(data + off, (size_t)payload_len, &vh, &vlen);
      /* Empty/malformed section is fine; keep whatever decoded. */
      *out_history = vh;
      *out_history_len = vlen;
    }
    /* STORAGE_SECTION_JOURNAL and any unknown future section types are
       intentionally skipped here for forward compatibility: an older
       binary opening a newer file with extra section types simply
       ignores them rather than failing to parse. */

    off += (size_t)payload_len;
  }

  if (!got_document) {
    bytebuffer_free(out_document);
    return STORAGE_ERR_TRUNCATED;
  }
  if (!got_metadata) {
    /* Not fatal — older/partial files might lack metadata. Leave the
       zeroed default and let the caller fill in sensible values. */
  }

  return STORAGE_OK;
}

/* ===================================================================== *
 * Journal (WAL) format
 *
 * Append-only file of records:
 *   u64 timestamp_ms_ish (we use unix seconds, widened)
 *   u32 op_description_len
 *   bytes op_description
 *   u64 document_len
 *   bytes document (full snapshot — see header comment: self-sufficient,
 *                    no replay needed)
 *   u32 record_crc32 (over everything above in this record)
 *
 * The journal is read tail-first on recovery: only the LAST valid record
 * matters, since each record is a full snapshot. Earlier records are kept
 * only as a safety margin in case the tail record itself is torn by a
 * crash mid-append; recovery walks backward until it finds a record whose
 * crc32 validates.
 * ===================================================================== */

static StorageStatus journal_append_record(int fd, const char *op_desc,
                                           const ByteBuffer *document) {
  ByteBuffer rec;
  bytebuffer_init(&rec);

  uint64_t ts = (uint64_t)time(NULL);
  uint32_t op_len = (uint32_t)strlen(op_desc);

  if (!bytebuffer_append_u64(&rec, ts))
    goto nomem;
  if (!bytebuffer_append_u32(&rec, op_len))
    goto nomem;
  if (!bytebuffer_append(&rec, op_desc, op_len))
    goto nomem;
  if (!bytebuffer_append_u64(&rec, (uint64_t)document->len))
    goto nomem;
  if (!bytebuffer_append(&rec, document->data, document->len))
    goto nomem;

  uint32_t crc = crc32_compute(rec.data, rec.len);
  if (!bytebuffer_append_u32(&rec, crc))
    goto nomem;

  /* Prefix each record with its own total length so tail-scanning on
     recovery doesn't require re-deriving offsets from variable-length
     fields in forward order. */
  uint8_t len_prefix[8];
  uint64_t rec_total = (uint64_t)rec.len;
  for (int i = 0; i < 8; i++)
    len_prefix[i] = (uint8_t)((rec_total >> (8 * i)) & 0xFF);

  size_t written = 0;
  while (written < 8) {
    ssize_t w = write(fd, len_prefix + written, 8 - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      bytebuffer_free(&rec);
      return STORAGE_ERR_IO;
    }
    written += (size_t)w;
  }

  written = 0;
  while (written < rec.len) {
    ssize_t w = write(fd, rec.data + written, rec.len - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      bytebuffer_free(&rec);
      return STORAGE_ERR_IO;
    }
    written += (size_t)w;
  }

  /* Trailing length repeated after the record lets backward-scanning
     recovery find record boundaries without a forward index. */
  written = 0;
  while (written < 8) {
    ssize_t w = write(fd, len_prefix + written, 8 - written);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      bytebuffer_free(&rec);
      return STORAGE_ERR_IO;
    }
    written += (size_t)w;
  }

  fsync(fd);
  bytebuffer_free(&rec);
  return STORAGE_OK;

nomem:
  bytebuffer_free(&rec);
  return STORAGE_ERR_NOMEM;
}

/* Scans a fully-loaded journal file image backward and returns the last
   structurally valid (crc-correct) record's document snapshot. Returns
   STORAGE_ERR_NOT_FOUND if the journal is empty or contains no valid
   record (e.g. truncated mid-write by a crash). */
static StorageStatus journal_find_last_valid(const uint8_t *data, size_t len,
                                             ByteBuffer *out_document) {
  if (len < 8)
    return STORAGE_ERR_NOT_FOUND;

  size_t cursor = len;
  while (cursor >= 8) {
    uint64_t rec_len = read_u64_le(data + cursor - 8);
    if (rec_len == 0 || rec_len > cursor - 8) {
      /* Torn/garbage trailer (e.g. crash mid-append left a partial
         trailing length). Step back one byte and keep scanning instead
         of abandoning the search: earlier complete records may still be
         recoverable. */
      cursor--;
      continue;
    }

    size_t rec_start = cursor - 8 - rec_len;
    /* validate matching leading length prefix exists and matches */
    if (rec_start < 8)
      break;
    uint64_t leading_len = read_u64_le(data + rec_start - 8);
    if (leading_len != rec_len) {
      cursor = rec_start; /* try the record before this malformed one */
      continue;
    }

    const uint8_t *rec = data + rec_start;
    /* record layout: u64 ts, u32 op_len, op_bytes, u64 doc_len, doc_bytes, u32
     * crc */
    if (rec_len < 8 + 4 + 8 + 4) {
      cursor = rec_start - 8;
      continue;
    }
    uint32_t stored_crc = read_u32_le(rec + rec_len - 4);
    uint32_t computed_crc = crc32_compute(rec, (size_t)rec_len - 4);
    if (stored_crc != computed_crc) {
      cursor = rec_start - 8;
      continue;
    }

    size_t off = 8; /* skip timestamp */
    uint32_t op_len = read_u32_le(rec + off);
    off += 4;
    if (off + op_len + 8 > rec_len - 4) {
      cursor = rec_start - 8;
      continue;
    }
    off += op_len;
    uint64_t doc_len = read_u64_le(rec + off);
    off += 8;
    if (off + doc_len > rec_len - 4) {
      cursor = rec_start - 8;
      continue;
    }

    bytebuffer_init(out_document);
    if (!bytebuffer_append(out_document, rec + off, (size_t)doc_len))
      return STORAGE_ERR_NOMEM;
    return STORAGE_OK;
  }

  return STORAGE_ERR_NOT_FOUND;
}

/* ===================================================================== *
 * Version history (embedded SECTION_VERSIONS)
 *
 * Section payload = records back-to-back. Each record:
 *   u32 rec_magic ("VER1")
 *   u64 id
 *   u64 created_at
 *   u32 name_len, name bytes (<= STORAGE_VERSION_NAME_MAX-1)
 *   u64 doc_len, u32 doc_crc32, doc bytes
 *   u32 rec_crc32 (over all preceding bytes of this record)
 *
 * The enclosing section already carries a payload CRC (verified before
 * this parser runs) and saves are atomic tmp+rename, so records cannot be
 * torn by crashes; the per-record CRC is defense-in-depth against crafted
 * or foreign corruption. On any malformed record, parsing stops and the
 * versions decoded so far are kept.
 * ===================================================================== */

static void history_free(StorageVersion *hist, size_t len) {
  for (size_t i = 0; i < len; i++)
    bytebuffer_free(&hist[i].doc);
  free(hist);
}

static bool history_push(StorageSession *s, uint64_t id, uint64_t ts,
                         const char *name, const uint8_t *doc, size_t doc_len) {
  if (s->hist_len == s->hist_cap) {
    size_t nc = s->hist_cap ? s->hist_cap * 2 : 8;
    StorageVersion *nh = realloc(s->hist, nc * sizeof(StorageVersion));
    if (!nh)
      return false;
    s->hist = nh;
    s->hist_cap = nc;
  }
  StorageVersion *v = &s->hist[s->hist_len++];
  memset(v, 0, sizeof(*v));
  v->id = id;
  v->created_at = ts;
  snprintf(v->name, sizeof(v->name), "%s", name && name[0] ? name : "");
  bytebuffer_init(&v->doc);
  if (doc_len > 0) {
    if (!bytebuffer_append(&v->doc, doc, doc_len)) {
      s->hist_len--;
      return false;
    }
  }
  if (id >= s->hist_next_id)
    s->hist_next_id = id + 1;
  return true;
}

static void serialize_history_section(const StorageVersion *hist, size_t len,
                                      ByteBuffer *out) {
  bytebuffer_init(out);
  for (size_t i = 0; i < len; i++) {
    const StorageVersion *v = &hist[i];
    ByteBuffer rec;
    bytebuffer_init(&rec);

    uint32_t name_len = (uint32_t)strnlen(v->name, sizeof(v->name));
    bool ok = bytebuffer_append_u32(&rec, STORAGE_VERSION_REC_MAGIC) &&
              bytebuffer_append_u64(&rec, v->id) &&
              bytebuffer_append_u64(&rec, v->created_at) &&
              bytebuffer_append_u32(&rec, name_len) &&
              bytebuffer_append(&rec, v->name, name_len) &&
              bytebuffer_append_u64(&rec, (uint64_t)v->doc.len) &&
              bytebuffer_append_u32(&rec, crc32_compute(v->doc.data,
                                                        v->doc.len)) &&
              bytebuffer_append(&rec, v->doc.data, v->doc.len);
    if (!ok) {
      bytebuffer_free(&rec);
      continue; /* OOM: drop this record rather than corrupt the section */
    }

    uint32_t crc = crc32_compute(rec.data, rec.len);
    if (!bytebuffer_append_u32(&rec, crc)) {
      bytebuffer_free(&rec);
      continue;
    }
    if (!bytebuffer_append(out, rec.data, rec.len))
      break; /* OOM on outer buffer: keep what we have */
    bytebuffer_free(&rec);
  }
}

/* Parses a VERSIONS section payload into a freshly-allocated array.
   Returns STORAGE_OK even when individual records are malformed — those
   are skipped — and only fails on out-of-memory. Caller frees via
   history_free(). */
static StorageStatus parse_history_section(const uint8_t *data, size_t len,
                                           StorageVersion **out_hist,
                                           size_t *out_len) {
  *out_hist = NULL;
  *out_len = 0;

  /* Decode into a temporary session-less accumulator: reuse a scratch
     session struct purely for its list fields. */
  StorageSession scratch;
  memset(&scratch, 0, sizeof(scratch));

  size_t off = 0;
  while (off + 4 <= len) {
    uint32_t magic = read_u32_le(data + off);
    if (magic != STORAGE_VERSION_REC_MAGIC)
      break; /* unknown tail bytes: stop defensively */

    if (off + 4 + 8 + 8 + 4 > len)
      break;
    uint64_t id = read_u64_le(data + off + 4);
    uint64_t ts = read_u64_le(data + off + 12);
    uint32_t name_len = read_u32_le(data + off + 20);
    off += 24;

    if (name_len >= STORAGE_VERSION_NAME_MAX || off + name_len > len)
      break;
    const char *name = (const char *)(data + off);
    off += name_len;

    if (off + 8 + 4 > len)
      break;
    uint64_t doc_len = read_u64_le(data + off);
    off += 12; /* doc_len field (8) + stored per-record doc CRC (4); the
                  record CRC below re-covers the whole body, so the
                  standalone doc CRC is read implicitly via body check */

    if (doc_len > SIZE_MAX - 1 || off + doc_len + 4 > len)
      break;

    const uint8_t *doc = data + off;
    off += (size_t)doc_len;
    if (off + 4 > len)
      break;
    uint32_t rec_crc = read_u32_le(data + off);
    off += 4;

    /* Per-record CRC over everything from magic through doc bytes. */
    size_t body_len =
        4 + 8 + 8 + 4 + name_len + 8 + 4 + (size_t)doc_len;
    if (crc32_compute(data + off - 4 - body_len, body_len) != rec_crc)
      continue; /* skip this record, try the next */

    char name_buf[STORAGE_VERSION_NAME_MAX];
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';
    if (!history_push(&scratch, id, ts, name_buf, doc, (size_t)doc_len))
      break; /* OOM */
  }

  if (scratch.hist_len == 0) {
    free(scratch.hist);
    return STORAGE_OK; /* empty section is valid */
  }
  *out_hist = scratch.hist;
  *out_len = scratch.hist_len;
  return STORAGE_OK;
}

/* ===================================================================== *
 * Backups
 * ===================================================================== */

bool storage_backup_path(const char *path, int index, char *out_path,
                         size_t out_path_size) {
  if (index < 0 || index >= STORAGE_MAX_BACKUPS)
    return false;
  int n = snprintf(out_path, out_path_size, "%s.bak.%d", path, index);
  return n > 0 && (size_t)n < out_path_size;
}

static void rotate_backups(const char *path) {
  char oldest[STORAGE_PATH_MAX];
  if (storage_backup_path(path, STORAGE_MAX_BACKUPS - 1, oldest,
                          sizeof(oldest)))
    unlink(oldest); /* drop the oldest unconditionally; ignore ENOENT */

  for (int i = STORAGE_MAX_BACKUPS - 2; i >= 0; i--) {
    char src[STORAGE_PATH_MAX], dst[STORAGE_PATH_MAX];
    storage_backup_path(path, i, src, sizeof(src));
    storage_backup_path(path, i + 1, dst, sizeof(dst));
    /* rename() silently no-ops via ENOENT if src doesn't exist yet
       (e.g. early in the document's life before N backups accumulate) */
    rename(src, dst);
  }

  char first_backup[STORAGE_PATH_MAX];
  storage_backup_path(path, 0, first_backup, sizeof(first_backup));

  struct stat st;
  if (stat(path, &st) == 0) {
    ByteBuffer current;
    if (read_whole_file(path, &current) == STORAGE_OK) {
      atomic_write_file(first_backup, current.data, current.len);
      bytebuffer_free(&current);
    }
  }
}

/* ===================================================================== *
 * Orphaned temp files
 * ===================================================================== */

/* A crash between open(tmp) and rename() leaves "<path>.tmp.<pid>.<salt>"
   behind. On session open, remove leftovers from OTHER pids that are at
   least an hour old (never touch a concurrent editor's in-flight tmp). */
static void sweep_stale_tmp_files(const char *path) {
  char dir[STORAGE_PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  const char *base = path;
  if (slash) {
    *slash = '\0';
    base = slash + 1;
  } else {
    snprintf(dir, sizeof(dir), "%s", ".");
  }

  size_t base_len = strlen(base);
  if (base_len == 0 || base_len >= sizeof(((struct dirent *)0)->d_name))
    return;

  DIR *d = opendir(dir);
  if (!d)
    return;

  pid_t self = getpid();
  time_t now = time(NULL);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strncmp(de->d_name, base, base_len) != 0)
      continue;
    const char *tail = de->d_name + base_len;
    if (strncmp(tail, ".tmp.", 5) != 0)
      continue;
    int pid;
    unsigned salt;
    if (sscanf(tail, ".tmp.%d.%x", &pid, &salt) != 2)
      continue;
    if ((pid_t)pid == self)
      continue;

    char full[STORAGE_PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
    if (n < 0 || (size_t)n >= sizeof(full))
      continue;

    struct stat st;
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    if (now - st.st_mtime < 3600)
      continue;
    unlink(full);
  }
  closedir(d);
}

/* ===================================================================== *
 * Session lifecycle
 * ===================================================================== */

static StorageStatus try_load_edoc_file(const char *path,
                                        ByteBuffer *out_document,
                                        StorageMetadata *out_meta,
                                        StorageVersion **out_history,
                                        size_t *out_history_len) {
  ByteBuffer raw;
  StorageStatus st = read_whole_file(path, &raw);
  if (st != STORAGE_OK)
    return st;
  st = parse_edoc_image(raw.data, raw.len, out_document, out_meta, out_history,
                        out_history_len);
  bytebuffer_free(&raw);
  return st;
}

StorageStatus storage_session_open(const char *path, StorageSession **session,
                                   ByteBuffer *out_document,
                                   StorageMetadata *out_meta,
                                   StorageOpenResult *out_open_result) {
  if (!path || !session || !out_document || !out_meta || !out_open_result)
    return STORAGE_ERR_INVALID_ARG;

  StorageSession *s = calloc(1, sizeof(StorageSession));
  if (!s)
    return STORAGE_ERR_NOMEM;

  snprintf(s->path, sizeof(s->path), "%s", path);
  derive_sibling_path(path, ".journal", s->journal_path,
                      sizeof(s->journal_path));
  derive_sibling_path(path, ".autosave", s->autosave_path,
                      sizeof(s->autosave_path));
  sweep_stale_tmp_files(path);
  s->autosave_interval_ms = STORAGE_DEFAULT_AUTOSAVE_MS;
  s->dirty = false;
  s->has_recovery_candidate = false;
  bytebuffer_init(&s->recovery_document);
  memset(&s->recovery_meta, 0, sizeof(s->recovery_meta));

  s->journal_fd = open(s->journal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (s->journal_fd < 0) {
    free(s);
    return STORAGE_ERR_IO;
  }

  struct stat st_main;
  bool main_exists = (stat(path, &st_main) == 0);

  StorageStatus primary_status = STORAGE_ERR_NOT_FOUND;
  bool loaded_primary = false;

  if (main_exists) {
    primary_status = try_load_edoc_file(path, out_document, out_meta,
                                        &s->hist, &s->hist_len);
    if (primary_status == STORAGE_OK)
      loaded_primary = true;
    else {
      history_free(s->hist, s->hist_len);
      s->hist = NULL;
      s->hist_len = 0;
    }
  }

  if (!loaded_primary) {
    /* Try backups, most recent first. */
    for (int i = 0; i < STORAGE_MAX_BACKUPS && !loaded_primary; i++) {
      char bpath[STORAGE_PATH_MAX];
      if (!storage_backup_path(path, i, bpath, sizeof(bpath)))
        break;
      struct stat bst;
      if (stat(bpath, &bst) != 0)
        continue;
      if (try_load_edoc_file(bpath, out_document, out_meta, &s->hist,
                             &s->hist_len) == STORAGE_OK)
        loaded_primary = true;
      else {
        history_free(s->hist, s->hist_len);
        s->hist = NULL;
        s->hist_len = 0;
      }
    }
  }

  if (!loaded_primary) {
    /* Try the autosave file as a last resort before declaring this a
       brand-new document. */
    struct stat ast;
    if (stat(s->autosave_path, &ast) == 0 &&
        try_load_edoc_file(s->autosave_path, out_document, out_meta, &s->hist,
                           &s->hist_len) == STORAGE_OK) {
      loaded_primary = true;
    } else {
      history_free(s->hist, s->hist_len);
      s->hist = NULL;
      s->hist_len = 0;
    }
  }

  if (!loaded_primary && !main_exists) {
    bytebuffer_init(out_document);
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->created_at = (uint64_t)time(NULL);
    out_meta->modified_at = out_meta->created_at;
    out_meta->schema_version = 1;
    s->hist_next_id = 1; /* human-friendly: first version is v1 */
    *out_open_result = STORAGE_OPEN_NEW;
    *session = s;
    return STORAGE_OK;
  }

  if (!loaded_primary) {
    close(s->journal_fd);
    free(s);
    return primary_status; /* genuinely unreadable: bad magic / unsupported
                               version / truncated / corrupt, with no
                               usable fallback */
  }

  /* Check for a newer autosave than what we just loaded as primary
     (mtime comparison), and for a non-empty journal with a valid tail
     record — either constitutes "recovered" state the editor should
     offer to restore. */
  *out_open_result = STORAGE_OPEN_CLEAN;

  struct stat autosave_st;
  bool autosave_newer = false;
  if (stat(s->autosave_path, &autosave_st) == 0) {
    if (!main_exists || autosave_st.st_mtime >= st_main.st_mtime)
      autosave_newer = true;
  }

  ByteBuffer journal_raw;
  StorageStatus journal_read_status =
      read_whole_file(s->journal_path, &journal_raw);
  bool journal_has_valid_tail = false;
  ByteBuffer journal_candidate;
  bytebuffer_init(&journal_candidate);

  if (journal_read_status == STORAGE_OK && journal_raw.len > 0) {
    if (journal_find_last_valid(journal_raw.data, journal_raw.len,
                                &journal_candidate) == STORAGE_OK) {
      journal_has_valid_tail = true;
    }
    bytebuffer_free(&journal_raw);
  }

  if (journal_has_valid_tail) {
    s->has_recovery_candidate = true;
    bytebuffer_free(&s->recovery_document);
    s->recovery_document = journal_candidate; /* transfer ownership */
    s->recovery_meta = *out_meta;
    s->recovery_meta.modified_at = (uint64_t)time(NULL);
    *out_open_result = STORAGE_OPEN_RECOVERED;
  } else {
    bytebuffer_free(&journal_candidate);
    if (autosave_newer) {
      ByteBuffer autosave_doc;
      StorageMetadata autosave_meta;
      StorageVersion *av_hist = NULL;
      size_t av_hist_len = 0;
      if (try_load_edoc_file(s->autosave_path, &autosave_doc, &autosave_meta,
                             &av_hist, &av_hist_len) == STORAGE_OK) {
        history_free(av_hist, av_hist_len); /* autosave history not used */
        bool differs = (autosave_doc.len != out_document->len) ||
                       (autosave_doc.len > 0 &&
                        memcmp(autosave_doc.data, out_document->data,
                               autosave_doc.len) != 0);
        if (differs) {
          s->has_recovery_candidate = true;
          bytebuffer_free(&s->recovery_document);
          s->recovery_document = autosave_doc;
          s->recovery_meta = autosave_meta;
          *out_open_result = STORAGE_OPEN_RECOVERED;
        } else {
          bytebuffer_free(&autosave_doc);
        }
      }
    }
  }

  if (primary_status != STORAGE_OK) {
    /* We loaded from a backup or autosave fallback rather than the
       primary file (which itself was missing/corrupt). Surface this as
       a recovered state too, since the editor should tell the user. */
    *out_open_result = STORAGE_OPEN_RECOVERED;
  }

  /* Version ids are 1-based; a file with no embedded history starts at
     v1, and any parsed history leaves next_id at max(id)+1 already. */
  if (s->hist_next_id == 0)
    s->hist_next_id = 1;

  *session = s;
  return STORAGE_OK;
}

void storage_session_close(StorageSession *session) {
  if (!session)
    return;
  if (session->journal_fd >= 0)
    close(session->journal_fd);
  bytebuffer_free(&session->recovery_document);
  history_free(session->hist, session->hist_len);
  free(session);
}

/* ===================================================================== *
 * Saving
 * ===================================================================== */

StorageStatus storage_save(StorageSession *session, const char *path,
                           const ByteBuffer *document,
                           const StorageMetadata *meta) {
  if (!session || !path || !document || !meta)
    return STORAGE_ERR_INVALID_ARG;

  StorageMetadata meta_copy = *meta;
  meta_copy.modified_at = (uint64_t)time(NULL);
  if (meta_copy.created_at == 0)
    meta_copy.created_at = meta_copy.modified_at;

  /* Record this save as a version unless it is byte-identical to the
     newest one already stored ("nothing to commit"). */
  if (session->hist_len == 0 ||
      session->hist[session->hist_len - 1].doc.len != document->len ||
      (document->len > 0 &&
       memcmp(session->hist[session->hist_len - 1].doc.data, document->data,
              document->len) != 0)) {
    char auto_name[STORAGE_VERSION_NAME_MAX];
    snprintf(auto_name, sizeof(auto_name), "v%llu",
             (unsigned long long)session->hist_next_id);
    if (!history_push(session, session->hist_next_id, (uint64_t)time(NULL),
                      auto_name, document->data, document->len))
      return STORAGE_ERR_NOMEM;
  }

  ByteBuffer image;
  StorageStatus st = build_edoc_image(document, &meta_copy, NULL, session->hist,
                                      session->hist_len, &image);
  if (st != STORAGE_OK)
    return st;

  /* Rotate backups BEFORE overwriting the primary file, so the backup
     chain reflects "previous saved states," not the one we're about to
     write. */
  rotate_backups(path);

  st = atomic_write_file(path, image.data, image.len);
  bytebuffer_free(&image);
  if (st != STORAGE_OK)
    return st;

  bool rebind = (strcmp(path, session->path) != 0);
  if (rebind) {
    /* Clear the journal bound to the OLD path first, so a stale
       non-empty journal doesn't trigger a spurious recovery prompt the
       next time the old file is opened. */
    storage_journal_clear(session);
    snprintf(session->path, sizeof(session->path), "%s", path);
    derive_sibling_path(path, ".journal", session->journal_path,
                        sizeof(session->journal_path));
    derive_sibling_path(path, ".autosave", session->autosave_path,
                        sizeof(session->autosave_path));
  }

  storage_journal_clear(session);

  session->dirty = false;
  session->hist_dirty = false; /* renames/deletes persisted with this save */
  return STORAGE_OK;
}

/* Reopen the journal fd if it died (e.g. storage_journal_clear's reopen
   failed once). Self-heals instead of silently killing all future
   journaling for the session. */
static StorageStatus journal_ensure_fd(StorageSession *session) {
  if (session->journal_fd >= 0)
    return STORAGE_OK;
  session->journal_fd =
      open(session->journal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  return session->journal_fd >= 0 ? STORAGE_OK : STORAGE_ERR_IO;
}

StorageStatus storage_journal_append(StorageSession *session,
                                     const char *op_description,
                                     const ByteBuffer *document) {
  if (!session || !op_description || !document)
    return STORAGE_ERR_INVALID_ARG;
  if (journal_ensure_fd(session) != STORAGE_OK)
    return STORAGE_ERR_IO;
  StorageStatus st =
      journal_append_record(session->journal_fd, op_description, document);
  if (st == STORAGE_OK)
    session->dirty = true;
  return st;
}

StorageStatus storage_journal_clear(StorageSession *session) {
  if (!session)
    return STORAGE_ERR_INVALID_ARG;
  if (session->journal_fd >= 0)
    close(session->journal_fd);
  session->journal_fd =
      open(session->journal_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (session->journal_fd < 0)
    return STORAGE_ERR_IO;
  fsync(session->journal_fd);
  return STORAGE_OK;
}

bool storage_should_autosave(const StorageSession *session, uint32_t now_ms) {
  if (!session || !session->dirty)
    return false;
  if (session->has_attempted_autosave &&
      (uint32_t)(now_ms - session->last_autosave_attempt_ms) <
          session->autosave_interval_ms)
    return false;
  return true;
}

bool storage_autosave_tick(StorageSession *session, const ByteBuffer *document,
                           const StorageMetadata *meta, uint32_t now_ms) {
  if (!document || !meta)
    return false;
  if (!storage_should_autosave(session, now_ms))
    return false;

  session->has_attempted_autosave = true;
  session->last_autosave_attempt_ms = now_ms;

  StorageMetadata meta_copy = *meta;
  meta_copy.modified_at = (uint64_t)time(NULL);
  if (meta_copy.created_at == 0)
    meta_copy.created_at = meta_copy.modified_at;

  ByteBuffer image;
  /* Autosave files deliberately carry NO version section (NULL history):
     they are crash-recovery artifacts, not history commits. */
  if (build_edoc_image(document, &meta_copy, NULL, NULL, 0, &image) !=
      STORAGE_OK)
    return false;

  StorageStatus st =
      atomic_write_file(session->autosave_path, image.data, image.len);
  bytebuffer_free(&image);
  if (st != STORAGE_OK)
    return false;

  /* A successful autosave also means the journal's job (covering the gap
     between explicit saves) has been superseded for everything up to
     this point, so we can safely truncate it to bound its growth. */
  storage_journal_clear(session);
  return true;
}

void storage_set_autosave_interval(StorageSession *session, uint32_t ms) {
  if (session)
    session->autosave_interval_ms = ms;
}

bool storage_autosave_path(const StorageSession *session, char *out_path,
                           size_t out_path_size) {
  if (!session || !out_path || out_path_size == 0)
    return false;
  int n = snprintf(out_path, out_path_size, "%s", session->autosave_path);
  return n > 0 && (size_t)n < out_path_size;
}

void storage_mark_dirty(StorageSession *session) {
  if (session)
    session->dirty = true;
}

bool storage_is_dirty(const StorageSession *session) {
  return session ? session->dirty : false;
}

/* ===================================================================== *
 * Recovery accessors
 * ===================================================================== */

StorageStatus storage_recovery_get(StorageSession *session,
                                   ByteBuffer *out_document,
                                   StorageMetadata *out_meta) {
  if (!session || !out_document || !out_meta)
    return STORAGE_ERR_INVALID_ARG;
  if (!session->has_recovery_candidate)
    return STORAGE_ERR_NOT_FOUND;

  bytebuffer_init(out_document);
  if (!bytebuffer_append(out_document, session->recovery_document.data,
                         session->recovery_document.len))
    return STORAGE_ERR_NOMEM;
  *out_meta = session->recovery_meta;
  return STORAGE_OK;
}

void storage_recovery_discard(StorageSession *session) {
  if (!session)
    return;
  session->has_recovery_candidate = false;
  bytebuffer_free(&session->recovery_document);
  storage_journal_clear(session);
}

/* ===================================================================== *
 * Standalone verification
 * ===================================================================== */

StorageStatus storage_verify_file(const char *path) {
  ByteBuffer raw;
  StorageStatus st = read_whole_file(path, &raw);
  if (st != STORAGE_OK)
    return st;

  ByteBuffer doc;
  StorageMetadata meta;
  st = parse_edoc_image(raw.data, raw.len, &doc, &meta, NULL, NULL);
  bytebuffer_free(&raw);
  if (st == STORAGE_OK)
    bytebuffer_free(&doc);
  return st;
}

/* ===================================================================== *
 * Version history accessors
 * ===================================================================== */

size_t storage_history_count(const StorageSession *session) {
  return session ? session->hist_len : 0;
}

const StorageVersion *storage_history_at(const StorageSession *session,
                                         size_t index) {
  if (!session || index >= session->hist_len)
    return NULL;
  /* Public order is newest-first; internal storage is oldest-first. */
  return &session->hist[session->hist_len - 1 - index];
}

bool storage_history_rename(StorageSession *session, size_t index,
                            const char *name) {
  if (!session || !name || !name[0])
    return false;
  StorageVersion *v = (StorageVersion *)storage_history_at(session, index);
  if (!v)
    return false;
  snprintf(v->name, sizeof(v->name), "%s", name);
  session->hist_dirty = true;
  return true;
}

bool storage_history_delete(StorageSession *session, size_t index) {
  if (!session || index >= session->hist_len)
    return false;
  size_t internal = session->hist_len - 1 - index;
  bytebuffer_free(&session->hist[internal].doc);
  memmove(&session->hist[internal], &session->hist[internal + 1],
          (session->hist_len - internal - 1) * sizeof(StorageVersion));
  session->hist_len--;
  session->hist_dirty = true;
  return true;
}

bool storage_history_dirty(const StorageSession *session) {
  return session ? session->hist_dirty : false;
}

const char *storage_status_string(StorageStatus status) {
  switch (status) {
  case STORAGE_OK:
    return "OK";
  case STORAGE_ERR_IO:
    return "I/O error";
  case STORAGE_ERR_NOMEM:
    return "out of memory";
  case STORAGE_ERR_BAD_MAGIC:
    return "not a valid document file";
  case STORAGE_ERR_UNSUPPORTED_VERSION:
    return "unsupported file format version";
  case STORAGE_ERR_CORRUPT_CHECKSUM:
    return "checksum mismatch (file corrupted)";
  case STORAGE_ERR_TRUNCATED:
    return "file truncated/incomplete";
  case STORAGE_ERR_NOT_FOUND:
    return "file not found";
  case STORAGE_ERR_INVALID_ARG:
    return "invalid argument";
  case STORAGE_ERR_RENAME_FAILED:
    return "atomic rename failed";
  case STORAGE_ERR_LOCK_FAILED:
    return "lock failed";
  }
  return "unknown error";
}

/* ===================================================================== *
 * Fuzz-harness entry points (compiled out unless -DSTORAGE_FUZZING).
 * Each wrapper owns full cleanup so a harness can call it millions of
 * times with arbitrary bytes; behavior is identical to production paths.
 * ===================================================================== */
#ifdef STORAGE_FUZZING

StorageStatus fuzz_parse_edoc(const uint8_t *data, size_t len) {
  ByteBuffer doc;
  StorageMetadata meta;
  StorageVersion *hist = NULL;
  size_t hist_len = 0;
  StorageStatus st =
      parse_edoc_image(data, len, &doc, &meta, &hist, &hist_len);
  /* Ownership contract of parse_edoc_image: on STORAGE_OK the document
   * buffer is live and ours; on any failure it was freed internally.
   * History, however, is ALWAYS caller-owned whenever the pointer was
   * set — including failure paths that abort mid-section-loop. */
  if (st == STORAGE_OK)
    bytebuffer_free(&doc);
  history_free(hist, hist_len);
  return st;
}

StorageStatus fuzz_journal_scan(const uint8_t *data, size_t len) {
  ByteBuffer doc;
  bytebuffer_init(&doc);
  StorageStatus st = journal_find_last_valid(data, len, &doc);
  if (st == STORAGE_OK)
    bytebuffer_free(&doc);
  return st;
}

StorageStatus fuzz_history_parse(const uint8_t *data, size_t len) {
  StorageVersion *hist = NULL;
  size_t hist_len = 0;
  parse_history_section(data, len, &hist, &hist_len);
  history_free(hist, hist_len);
  return STORAGE_OK;
}

#endif /* STORAGE_FUZZING */
