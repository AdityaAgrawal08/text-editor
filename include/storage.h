#ifndef EDITOR_STORAGE_H
#define EDITOR_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * Storage layer overview
 * -----------------------
 * On-disk container format ("EDOC"): a single file holding a versioned,
 * checksummed document plus an embedded recovery journal. Designed so the
 * document-model layer (blocks/text-runs, to be added later) can serialize
 * into the EDOC_SECTION_DOCUMENT payload without this layer knowing its
 * internal shape — storage.c treats the document body as an opaque,
 * length-prefixed, checksummed blob.
 *
 * File layout (all integers little-endian):
 *
 *   [FileHeader]                          fixed size, magic + format version
 *   [SectionHeader][payload...]   repeated for each section present:
 *       SECTION_DOCUMENT   - the serialized document body
 *       SECTION_METADATA   - key/value metadata (title, author, timestamps)
 *       SECTION_JOURNAL    - pending WAL-style operations not yet
 *                             checkpointed into SECTION_DOCUMENT (crash
 *                             recovery)
 *   [FileFooter]                          whole-file checksum + section index
 *
 * Atomicity model
 * ----------------
 * Saves never write the live file in place. A save writes to
 * "<path>.tmp.<pid>.<random>", fsyncs it, then atomically renames it over
 * the destination (rename() is atomic on POSIX for same-filesystem moves).
 * A crash mid-write leaves only an orphaned tmp file; the destination is
 * never observed in a partially-written state.
 *
 * Crash recovery
 * ----------------
 * Before each destructive edit-session, a journal entry describing the
 * pending mutation is appended to "<path>.journal" (a separate, append-only
 * WAL file flushed on every entry). On clean save, the journal is truncated.
 * On open, if a non-empty journal is found next to a document, the storage
 * layer reports STORAGE_OPEN_RECOVERED and the caller can choose to replay
 * or discard it. This guards against the editor crashing between "user
 * typed something" and "next periodic autosave."
 *
 * Autosave
 * ----------------
 * storage_autosave_tick() is driven by the main loop. It debounces: it only
 * performs an atomic save if the document is dirty AND at least
 * autosave_interval_ms have elapsed since the last save attempt. Autosaves
 * write to a distinct "<path>.autosave" file, never clobbering the user's
 * last explicit save, and are promoted to the real path only on an explicit
 * editor_save() or on recovery prompt acceptance.
 *
 * Integrity verification
 * ----------------
 * Every section is protected by a CRC32 of its payload. The footer holds a
 * CRC32 of the full section index + header. On open, all checksums are
 * verified before any data is handed to the caller; corruption is reported
 * per-section so the caller can decide whether to fall back to the
 * autosave file, the journal, or a backup snapshot.
 *
 * Automatic backups
 * ----------------
 * On every successful explicit save, storage rotates numbered backups
 * "<path>.bak.0" .. "<path>.bak.N-1" (N = STORAGE_MAX_BACKUPS), shifting
 * older ones down and dropping the oldest. This is independent of the
 * history/versioning subsystem (which will track logical document
 * revisions) — these are raw filesystem safety nets.
 */

#define STORAGE_MAGIC 0x434F4445u /* "EDOC" little-endian as uint32 */
#define STORAGE_FORMAT_VERSION 1u
#define STORAGE_MAX_BACKUPS 5
#define STORAGE_DEFAULT_AUTOSAVE_MS 15000u
#define STORAGE_PATH_MAX 4096

typedef enum {
  STORAGE_OK = 0,
  STORAGE_ERR_IO,
  STORAGE_ERR_NOMEM,
  STORAGE_ERR_BAD_MAGIC,
  STORAGE_ERR_UNSUPPORTED_VERSION,
  STORAGE_ERR_CORRUPT_CHECKSUM,
  STORAGE_ERR_TRUNCATED,
  STORAGE_ERR_NOT_FOUND,
  STORAGE_ERR_INVALID_ARG,
  STORAGE_ERR_RENAME_FAILED,
  STORAGE_ERR_LOCK_FAILED,
} StorageStatus;

typedef enum {
  STORAGE_OPEN_CLEAN = 0,     /* opened normally, no recovery data found */
  STORAGE_OPEN_RECOVERED = 1, /* a journal or autosave was found and is
                                  newer/different than the main file */
  STORAGE_OPEN_NEW = 2,       /* path did not exist; caller should treat
                                  as a brand-new empty document */
} StorageOpenResult;

typedef enum {
  STORAGE_SECTION_DOCUMENT = 1,
  STORAGE_SECTION_METADATA = 2,
  STORAGE_SECTION_JOURNAL = 3,
} StorageSectionType;

/* Opaque growable byte buffer used for section payloads. Callers
   (document-model layer) fill this in and hand it to storage_save();
   storage_open() fills one in and hands it back. */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} ByteBuffer;

void bytebuffer_init(ByteBuffer *b);
bool bytebuffer_reserve(ByteBuffer *b, size_t additional);
bool bytebuffer_append(ByteBuffer *b, const void *data, size_t len);
bool bytebuffer_append_u32(ByteBuffer *b, uint32_t v);
bool bytebuffer_append_u64(ByteBuffer *b, uint64_t v);
void bytebuffer_free(ByteBuffer *b);

/* Document metadata persisted alongside the body. Kept separate from the
   document body so future schema changes to the body don't require
   touching metadata parsing and vice versa (forward/backward compat). */
typedef struct {
  char title[256];
  char author[128];
  uint64_t created_at;     /* unix epoch seconds */
  uint64_t modified_at;    /* unix epoch seconds */
  uint32_t schema_version; /* document-model schema version, independent
                               of STORAGE_FORMAT_VERSION */
  uint64_t revision_id;    /* most recent history-system commit id, 0 if
                               history subsystem not in use yet */
} StorageMetadata;

/* A single durable handle bound to one logical document path. Owns the
   journal fd and tracks autosave timing. Created via storage_session_open,
   destroyed via storage_session_close. */
typedef struct StorageSession StorageSession;

/* ---- Lifecycle ---------------------------------------------------- */

/* Opens (or prepares to create) a document session for `path`.
 * - If `path` exists and is a valid EDOC file: loads it, verifies all
 *   checksums, and returns STORAGE_OPEN_CLEAN (or STORAGE_OPEN_RECOVERED
 *   if a newer autosave/journal sibling is detected).
 * - If `path` does not exist: returns STORAGE_OPEN_NEW with an empty
 *   document buffer and default metadata.
 * - On corruption in the primary file, automatically attempts the most
 *   recent backup, then the autosave file, in that order, before giving
 *   up; `out_status` reflects the underlying error from the primary file
 *   even if a fallback succeeded, so the caller can warn the user.
 *
 * On success (*session != NULL), `out_document` and `out_meta` are
 * populated with whatever was loaded (caller does not own out_document's
 * internal buffer beyond reading it before the next storage_* call that
 * mutates it; copy out anything needed).
 */
StorageStatus storage_session_open(const char *path, StorageSession **session,
                                   ByteBuffer *out_document,
                                   StorageMetadata *out_meta,
                                   StorageOpenResult *out_open_result);

void storage_session_close(StorageSession *session);

/* ---- Saving --------------------------------------------------------- */

/* Performs a full, atomic, explicit save: writes a tmp file containing
 * document + metadata sections, fsyncs, renames over `path`, rotates
 * backups, and truncates the journal (the save is now the durable source
 * of truth, so pending recovery data is no longer needed). */
StorageStatus storage_save(StorageSession *session, const char *path,
                           const ByteBuffer *document,
                           const StorageMetadata *meta);

/* Appends a journal entry describing a pending mutation, for crash
 * recovery between autosaves. `op_description` is a short opaque tag
 * (e.g. "insert", "delete", "block-split") used only for diagnostics;
 * the actual recoverable state is the full `document` buffer snapshot
 * passed in, which is always self-sufficient — no replay logic is
 * required to use it; it IS the recovered state. */
StorageStatus storage_journal_append(StorageSession *session,
                                     const char *op_description,
                                     const ByteBuffer *document);

/* Clears the journal file (called after a successful explicit save, or
 * after the caller decides to discard recovered data). */
StorageStatus storage_journal_clear(StorageSession *session);

/* Driven from the main loop every frame. Internally debounces against
 * session->autosave_interval_ms. Returns true if an autosave was actually
 * performed this call (caller may want to flash a status indicator). */
bool storage_autosave_tick(StorageSession *session, const ByteBuffer *document,
                           const StorageMetadata *meta, uint32_t now_ms);

void storage_set_autosave_interval(StorageSession *session, uint32_t ms);
void storage_mark_dirty(StorageSession *session);
bool storage_is_dirty(const StorageSession *session);

/* ---- Recovery --------------------------------------------------------- */

/* If storage_session_open returned STORAGE_OPEN_RECOVERED, the recovered
 * candidate (journal tail or autosave, whichever is newest) is staged
 * internally. Call this to retrieve it for a "Recover unsaved changes?"
 * prompt; call storage_recovery_discard to drop it instead. */
StorageStatus storage_recovery_get(StorageSession *session,
                                   ByteBuffer *out_document,
                                   StorageMetadata *out_meta);
void storage_recovery_discard(StorageSession *session);

/* ---- Integrity --------------------------------------------------------- */

/* Independently verifies a file on disk without opening a session.
 * Useful for a startup self-check or a "Verify Document Integrity" menu
 * command. Returns STORAGE_OK if every section checksum and the footer
 * checksum match. */
StorageStatus storage_verify_file(const char *path);

const char *storage_status_string(StorageStatus status);

/* ---- Backups --------------------------------------------------------- */

/* Returns the path of the Nth most recent backup (0 = most recent) into
 * `out_path` (size out_path_size). Returns false if that backup slot
 * doesn't exist. */
bool storage_backup_path(const char *path, int index, char *out_path,
                         size_t out_path_size);

#endif /* EDITOR_STORAGE_H */
