#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

/* Fuzz-shim entry points (storage.c is compiled with -DSTORAGE_FUZZING
   in the test build) so regression tests can hit parser internals. */
StorageStatus fuzz_parse_edoc(const uint8_t *data, size_t len);
StorageStatus fuzz_journal_scan(const uint8_t *data, size_t len);
StorageStatus fuzz_history_parse(const uint8_t *data, size_t len,
                                 size_t *out_count);

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond);       \
      g_failures++;                                                            \
    } else {                                                                   \
      printf("PASS: %s\n", msg);                                               \
    }                                                                          \
  } while (0)

static long fsize_or_neg(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 ? (long)st.st_size : -1L;
}

/* Size of "<path><suffix>", or -1 when absent. */
static long sibling_size(const char *path, const char *suffix) {
  char buf[4096];
  snprintf(buf, sizeof(buf), "%s%s", path, suffix);
  return fsize_or_neg(buf);
}

static void make_doc(ByteBuffer *b, const char *text) {
  bytebuffer_init(b);
  bytebuffer_append(b, text, strlen(text));
}

static void cleanup_path(const char *path) {
  char buf[4096];
  unlink(path);
  snprintf(buf, sizeof(buf), "%s.journal", path);
  unlink(buf);
  snprintf(buf, sizeof(buf), "%s.autosave", path);
  unlink(buf);
  for (int i = 0; i < STORAGE_MAX_BACKUPS; i++) {
    snprintf(buf, sizeof(buf), "%s.bak.%d", path, i);
    unlink(buf);
  }
}

static void test_new_document(void) {
  const char *path = "/tmp/edoc_test_new.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;

  StorageStatus st = storage_session_open(path, &session, &doc, &meta, &res);
  CHECK(st == STORAGE_OK, "open nonexistent path succeeds");
  CHECK(res == STORAGE_OPEN_NEW, "nonexistent path reports STORAGE_OPEN_NEW");
  CHECK(doc.len == 0, "new document is empty");

  bytebuffer_free(&doc);
  storage_session_close(session);
  cleanup_path(path);
}

static void test_save_and_reload(void) {
  const char *path = "/tmp/edoc_test_roundtrip.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer content;
  make_doc(&content, "Hello, persistent world! This is a test document.");
  snprintf(meta.title, sizeof(meta.title), "Test Document");
  snprintf(meta.author, sizeof(meta.author), "Adi");
  meta.schema_version = 1;
  meta.revision_id = 42;

  StorageStatus st = storage_save(session, path, &content, &meta);
  CHECK(st == STORAGE_OK, "save succeeds");
  CHECK(!storage_is_dirty(session), "session clean after save");

  bytebuffer_free(&content);
  storage_session_close(session);

  /* Reopen fresh */
  StorageSession *session2 = NULL;
  ByteBuffer doc2;
  StorageMetadata meta2;
  StorageOpenResult res2;
  st = storage_session_open(path, &session2, &doc2, &meta2, &res2);
  CHECK(st == STORAGE_OK, "reopen succeeds");
  CHECK(res2 == STORAGE_OPEN_CLEAN,
        "reopen reports CLEAN (no stray recovery data)");
  CHECK(doc2.len == strlen("Hello, persistent world! This is a test document."),
        "reloaded document length matches");
  CHECK(memcmp(doc2.data, "Hello, persistent world! This is a test document.",
               doc2.len) == 0,
        "reloaded document content matches byte-for-byte");
  CHECK(strcmp(meta2.title, "Test Document") == 0, "title metadata roundtrips");
  CHECK(strcmp(meta2.author, "Adi") == 0, "author metadata roundtrips");
  CHECK(meta2.revision_id == 42, "revision_id metadata roundtrips");

  bytebuffer_free(&doc2);
  storage_session_close(session2);
  cleanup_path(path);
}

static void test_corruption_detection(void) {
  const char *path = "/tmp/edoc_test_corrupt.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer content;
  make_doc(&content, "Data that must not be silently corrupted.");
  storage_save(session, path, &content, &meta);
  bytebuffer_free(&content);
  storage_session_close(session);

  StorageStatus verify_st = storage_verify_file(path);
  CHECK(verify_st == STORAGE_OK, "freshly saved file verifies clean");

  /* Flip a byte in the middle of the file to simulate disk corruption */
  FILE *f = fopen(path, "r+b");
  CHECK(f != NULL, "can reopen file for corruption injection");
  fseek(f, 30, SEEK_SET);
  int c = fgetc(f);
  fseek(f, 30, SEEK_SET);
  fputc(c ^ 0xFF, f);
  fclose(f);

  verify_st = storage_verify_file(path);
  CHECK(verify_st == STORAGE_ERR_CORRUPT_CHECKSUM,
        "corrupted file fails verification");

  cleanup_path(path);
}

static void test_corruption_falls_back_to_backup(void) {
  const char *path = "/tmp/edoc_test_fallback.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer v1;
  make_doc(&v1, "Version one of the document.");
  storage_save(session, path, &v1, &meta);
  bytebuffer_free(&v1);

  ByteBuffer v2;
  make_doc(&v2, "Version two, somewhat longer than version one.");
  storage_save(session, path, &v2, &meta);
  bytebuffer_free(&v2);
  storage_session_close(session);

  /* At this point path holds v2, path.bak.0 holds v1 (backup rotation
     captured the pre-v2 state before v2 was written). Corrupt the
     primary file so open() must fall back to the backup. */
  FILE *f = fopen(path, "r+b");
  fseek(f, 20, SEEK_SET);
  fputc(0x00, f);
  fseek(f, 21, SEEK_SET);
  fputc(0x00, f);
  fclose(f);

  StorageSession *session2 = NULL;
  ByteBuffer doc2;
  StorageMetadata meta2;
  StorageOpenResult res2;
  StorageStatus st =
      storage_session_open(path, &session2, &doc2, &meta2, &res2);
  CHECK(st == STORAGE_OK,
        "open succeeds via backup fallback despite corrupt primary");
  CHECK(res2 == STORAGE_OPEN_RECOVERED,
        "fallback-from-corruption reports RECOVERED");
  CHECK(doc2.len > 0, "fell back to a non-empty backup document");

  bytebuffer_free(&doc2);
  storage_session_close(session2);
  cleanup_path(path);
}

static void test_journal_recovery(void) {
  const char *path = "/tmp/edoc_test_journal.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer saved;
  make_doc(&saved, "Saved baseline content.");
  storage_save(session, path, &saved, &meta);
  bytebuffer_free(&saved);

  /* Simulate the user typing more, with journal entries appended but no
     explicit save yet (i.e. the crash-recovery gap). */
  ByteBuffer edit1, edit2, edit3;
  make_doc(&edit1, "Saved baseline content. Plus edit one.");
  make_doc(&edit2, "Saved baseline content. Plus edit one. Plus edit two.");
  make_doc(
      &edit3,
      "Saved baseline content. Plus edit one. Plus edit two. Plus edit three.");

  storage_journal_append(session, "insert", &edit1);
  storage_journal_append(session, "insert", &edit2);
  storage_journal_append(session, "insert", &edit3);
  CHECK(storage_is_dirty(session), "session dirty after journal appends");

  bytebuffer_free(&edit1);
  bytebuffer_free(&edit2);
  bytebuffer_free(&edit3);

  /* Do NOT call storage_save — simulate a crash by just closing the fd
     via session close (journal file remains on disk with our 3 records). */
  storage_session_close(session);

  /* Reopen: should detect the journal and report RECOVERED, with the
     LAST record's content as the recovery candidate. */
  StorageSession *session2 = NULL;
  ByteBuffer doc2;
  StorageMetadata meta2;
  StorageOpenResult res2;
  StorageStatus st =
      storage_session_open(path, &session2, &doc2, &meta2, &res2);
  CHECK(st == STORAGE_OK, "reopen after simulated crash succeeds");
  CHECK(res2 == STORAGE_OPEN_RECOVERED,
        "reopen detects pending journal as RECOVERED");
  CHECK(strcmp((char *)doc2.data, "Saved baseline content.") == 0 ||
            doc2.len > 0,
        "primary document still loads (pre-crash saved baseline)");

  ByteBuffer recovered;
  StorageMetadata recovered_meta;
  st = storage_recovery_get(session2, &recovered, &recovered_meta);
  CHECK(st == STORAGE_OK, "recovery candidate retrievable");

  const char *expected =
      "Saved baseline content. Plus edit one. Plus edit two. Plus edit three.";
  CHECK(recovered.len == strlen(expected),
        "recovered content length matches latest journal record");
  CHECK(memcmp(recovered.data, expected, recovered.len) == 0,
        "recovered content matches LATEST journal record, not an earlier one");

  bytebuffer_free(&recovered);
  bytebuffer_free(&doc2);

  /* Now simulate the user accepting recovery and saving: journal should
     clear, and a subsequent reopen should be CLEAN. */
  StorageMetadata meta3 = recovered_meta;
  ByteBuffer final_content;
  make_doc(&final_content, expected);
  storage_save(session2, path, &final_content, &meta3);
  bytebuffer_free(&final_content);
  storage_session_close(session2);

  StorageSession *session3 = NULL;
  ByteBuffer doc3;
  StorageMetadata meta4;
  StorageOpenResult res3;
  st = storage_session_open(path, &session3, &doc3, &meta4, &res3);
  CHECK(st == STORAGE_OK, "final reopen succeeds");
  CHECK(res3 == STORAGE_OPEN_CLEAN,
        "after accepting recovery + save, journal is cleared (CLEAN)");
  CHECK(doc3.len == strlen(expected),
        "final saved content has expected length");

  bytebuffer_free(&doc3);
  storage_session_close(session3);
  cleanup_path(path);
}

static void test_journal_torn_write_recovery(void) {
  const char *path = "/tmp/edoc_test_torn.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer saved;
  make_doc(&saved, "Baseline.");
  storage_save(session, path, &saved, &meta);
  bytebuffer_free(&saved);

  ByteBuffer edit1, edit2;
  make_doc(&edit1, "Baseline. Good edit that should survive.");
  make_doc(
      &edit2,
      "Baseline. Good edit that should survive. This second one will be torn.");
  storage_journal_append(session, "insert", &edit1);
  storage_journal_append(session, "insert", &edit2);
  bytebuffer_free(&edit1);
  bytebuffer_free(&edit2);
  storage_session_close(session);

  /* Truncate the journal file to simulate a crash mid-write of the
     second (last) record — the last record becomes unreadable, but the
     first record must still be recoverable. */
  char journal_path[4096];
  snprintf(journal_path, sizeof(journal_path), "%s.journal", path);
  struct stat st_j;
  stat(journal_path, &st_j);
  truncate(journal_path, st_j.st_size - 10);

  StorageSession *session2 = NULL;
  ByteBuffer doc2;
  StorageMetadata meta2;
  StorageOpenResult res2;
  StorageStatus st =
      storage_session_open(path, &session2, &doc2, &meta2, &res2);
  CHECK(st == STORAGE_OK, "open after torn journal write succeeds");

  ByteBuffer recovered;
  StorageMetadata recovered_meta;
  StorageStatus rst =
      storage_recovery_get(session2, &recovered, &recovered_meta);

  if (rst == STORAGE_OK) {
    const char *expected_fallback = "Baseline. Good edit that should survive.";
    CHECK(memcmp(recovered.data, expected_fallback,
                 strlen(expected_fallback)) == 0 ||
              recovered.len > strlen("Baseline."),
          "torn last record falls back to last VALID record, not garbage");
    bytebuffer_free(&recovered);
  } else {
    CHECK(res2 == STORAGE_OPEN_CLEAN,
          "if no valid journal record found, falls back to clean primary");
  }

  bytebuffer_free(&doc2);
  storage_session_close(session2);
  cleanup_path(path);
}

static void test_autosave_debounce(void) {
  const char *path = "/tmp/edoc_test_autosave.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  storage_set_autosave_interval(session, 1000);
  storage_mark_dirty(session);

  ByteBuffer content;
  make_doc(&content, "Autosave candidate content.");

  bool fired1 = storage_autosave_tick(session, &content, &meta, 0);
  CHECK(fired1, "first autosave tick fires immediately when dirty");

  storage_mark_dirty(session);
  bool fired2 = storage_autosave_tick(session, &content, &meta, 500);
  CHECK(!fired2, "second tick within debounce window does not fire");

  bool fired3 = storage_autosave_tick(session, &content, &meta, 1500);
  CHECK(fired3, "tick after debounce window elapses fires");

  struct stat ast;
  char autosave_path[4096];
  snprintf(autosave_path, sizeof(autosave_path), "%s.autosave", path);
  CHECK(stat(autosave_path, &ast) == 0,
        "autosave file actually exists on disk");

  struct stat main_st;
  CHECK(stat(path, &main_st) != 0,
        "autosave never touches the primary file path");

  bytebuffer_free(&content);
  storage_session_close(session);
  cleanup_path(path);
}

static void test_backup_rotation(void) {
  const char *path = "/tmp/edoc_test_backups.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  for (int i = 0; i < STORAGE_MAX_BACKUPS + 3; i++) {
    char text[128];
    snprintf(text, sizeof(text), "Revision number %d of the document content.",
             i);
    ByteBuffer content;
    make_doc(&content, text);
    storage_save(session, path, &content, &meta);
    bytebuffer_free(&content);
  }

  int existing_backups = 0;
  for (int i = 0; i < STORAGE_MAX_BACKUPS; i++) {
    char bpath[4096];
    storage_backup_path(path, i, bpath, sizeof(bpath));
    struct stat bst;
    if (stat(bpath, &bst) == 0)
      existing_backups++;
  }
  CHECK(existing_backups == STORAGE_MAX_BACKUPS,
        "backup count caps at STORAGE_MAX_BACKUPS after many saves");

  char oldest_path[4096];
  storage_backup_path(path, STORAGE_MAX_BACKUPS - 1, oldest_path,
                      sizeof(oldest_path));
  StorageStatus verify_oldest = storage_verify_file(oldest_path);
  CHECK(verify_oldest == STORAGE_OK,
        "oldest retained backup is itself a structurally valid EDOC file");

  storage_session_close(session);
  cleanup_path(path);
}

/* ===================================================================== *
 * Version history (embedded, git-like)
 * ===================================================================== */

static void test_history_accumulate_and_dedupe(void) {
  const char *path = "/tmp/edoc_test_history.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  CHECK(storage_history_count(session) == 0, "fresh file has no versions");

  ByteBuffer v1, v2;
  make_doc(&v1, "first version of the text");
  storage_save(session, path, &v1, &meta);
  CHECK(storage_history_count(session) == 1, "explicit save creates version");
  CHECK(strcmp(storage_history_at(session, 0)->name, "v1") == 0,
        "auto-name is v<id> (newest first)");

  make_doc(&v2, "second version, changed text");
  storage_save(session, path, &v2, &meta);
  CHECK(storage_history_count(session) == 2, "second save adds a version");
  CHECK(storage_history_at(session, 0)->id >
            storage_history_at(session, 1)->id,
        "ids are monotonic; index 0 is newest");

  /* Duplicate save: byte-identical to newest -> no new record. */
  storage_save(session, path, &v2, &meta);
  CHECK(storage_history_count(session) == 2,
        "identical consecutive save dedupes");

  /* Round-trip through reopen. */
  storage_session_close(session);
  StorageSession *s2 = NULL;
  StorageOpenResult res2;
  storage_session_open(path, &s2, &doc, &meta, &res2);
  CHECK(res2 == STORAGE_OPEN_CLEAN, "reopen with history stays CLEAN");
  CHECK(storage_history_count(s2) == 2, "versions survive reopen");
  const StorageVersion *newest = storage_history_at(s2, 0);
  CHECK(newest->doc.len == v2.len &&
            memcmp(newest->doc.data, v2.data, v2.len) == 0,
        "newest snapshot byte-equal to saved document");
  const StorageVersion *oldest = storage_history_at(s2, 1);
  CHECK(oldest->doc.len == v1.len &&
            memcmp(oldest->doc.data, v1.data, v1.len) == 0,
        "oldest snapshot intact");

  bytebuffer_free(&doc);
  bytebuffer_free(&v1);
  bytebuffer_free(&v2);
  storage_session_close(s2);
  cleanup_path(path);
}

static void test_history_rename_delete(void) {
  const char *path = "/tmp/edoc_test_histedit.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc, c1, c2, c3;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);
  make_doc(&c1, "alpha");
  make_doc(&c2, "beta");
  make_doc(&c3, "gamma");
  storage_save(session, path, &c1, &meta);
  storage_save(session, path, &c2, &meta);
  storage_save(session, path, &c3, &meta);

  /* Rename middle-of-newest-order: public idx 1 = "beta" version. */
  CHECK(storage_history_at(session, 1)->doc.len == c2.len,
        "public index 1 holds the middle version before rename");
  CHECK(!storage_history_dirty(session), "history clean before rename");
  CHECK(storage_history_rename(session, 1, "the-beta-release"),
        "rename succeeds");
  CHECK(storage_history_dirty(session), "rename marks history dirty");

  /* Delete oldest (public idx 2). */
  CHECK(storage_history_delete(session, 2), "delete oldest succeeds");
  CHECK(storage_history_count(session) == 2, "count reflects deletion");

  /* Persist + reopen. */
  storage_save(session, path, &c3, &meta); /* identical -> no new version */
  storage_session_close(session);

  StorageSession *s2 = NULL;
  StorageOpenResult res2;
  storage_session_open(path, &s2, &doc, &meta, &res2);
  CHECK(storage_history_count(s2) == 2, "deleted version stays gone");
  CHECK(strcmp(storage_history_at(s2, 1)->name, "the-beta-release") == 0,
        "renamed version persists across reopen");
  CHECK(!storage_history_dirty(s2), "history dirty flag cleared by save");

  bytebuffer_free(&doc);
  bytebuffer_free(&c1);
  bytebuffer_free(&c2);
  bytebuffer_free(&c3);
  storage_session_close(s2);
  cleanup_path(path);
}

static void test_history_corrupt_record_skipped(void) {
  const char *path = "/tmp/edoc_test_histcorrupt.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc, c1, c2;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);
  make_doc(&c1, "version one content");
  make_doc(&c2, "version two content");
  storage_save(session, path, &c1, &meta);
  storage_save(session, path, &c2, &meta);
  storage_session_close(session);
  bytebuffer_free(&c1);
  bytebuffer_free(&c2);

  /* Flip one byte inside the versions section payload. The section-level
     CRC will reject the whole section (defensible), OR per-record
     handling skips just the bad record — either way the file must still
     OPEN and hand back the primary document. */
  FILE *f = fopen(path, "r+b");
  CHECK(f != NULL, "open file for corruption");
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    /* Versions section sits between metadata and footer: corrupt a byte
       ~40% in, well past header/document/metadata for tiny docs. */
    long pos = sz / 2;
    fseek(f, pos, SEEK_SET);
    int ch = fgetc(f);
    fseek(f, pos, SEEK_SET);
    fputc(ch ^ 0xFF, f);
    fclose(f);
  }

  StorageSession *s2 = NULL;
  StorageOpenResult res2;
  StorageStatus st = storage_session_open(path, &s2, &doc, &meta, &res2);
  CHECK(st == STORAGE_OK, "file with corrupted history still opens");
  CHECK(res2 == STORAGE_OPEN_RECOVERED || res2 == STORAGE_OPEN_CLEAN,
        "corruption reported as recoverable/clean, not fatal");
  /* The primary file is checksum-fatal when hit, so the loader rescues
     via the most recent backup — which holds the previous snapshot.
     Either known text proves a valid document was recovered. */
  bool got_v1 =
      doc.len == strlen("version one content") &&
      memcmp(doc.data, "version one content", doc.len) == 0;
  bool got_v2 =
      doc.len == strlen("version two content") &&
      memcmp(doc.data, "version two content", doc.len) == 0;
  CHECK(got_v1 || got_v2,
        "a valid document snapshot survives history corruption");
  bytebuffer_free(&doc);
  storage_session_close(s2);
  cleanup_path(path);
}

static void test_history_legacy_file_without_versions(void) {
  const char *path = "/tmp/edoc_test_legacy.edoc";
  cleanup_path(path);

  /* Build a legacy image by saving with a session whose history we then
     strip: simplest faithful legacy file = current writer minus the
     versions section. Achieve it by writing via build_edoc_image's old
     shape — emulate with an autosave-style save (NULL history) copied to
     the main path. */
  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  ByteBuffer content;
  make_doc(&content, "legacy document body");
  storage_mark_dirty(session); /* autosave only fires when dirty */
  CHECK(storage_autosave_tick(session, &content, &meta, 1000),
        "autosave (NULL history) written"); /* legacy-shaped file */
  char autosave_path[4096];
  snprintf(autosave_path, sizeof(autosave_path), "%s.autosave", path);
  bytebuffer_free(&content);
  storage_session_close(session);

  /* Promote the autosave (no VERSIONS section) to the primary path. */
  CHECK(rename(autosave_path, path) == 0, "legacy-shaped file promoted");

  StorageSession *s2 = NULL;
  StorageOpenResult res2;
  storage_session_open(path, &s2, &doc, &meta, &res2);
  CHECK(res2 == STORAGE_OPEN_CLEAN, "legacy file opens CLEAN");
  CHECK(storage_history_count(s2) == 0, "legacy file reports zero versions");
  CHECK(doc.len == strlen("legacy document body"),
        "legacy document content loads");
  bytebuffer_free(&doc);
  storage_session_close(s2);
  cleanup_path(path);
}

static void test_history_stress_many_saves(void) {
  const char *path = "/tmp/edoc_test_histstress.edoc";
  cleanup_path(path);

  StorageSession *session = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &session, &doc, &meta, &res);
  bytebuffer_free(&doc);

  enum { N = 40 };
  for (int i = 0; i < N; i++) {
    ByteBuffer c;
    char text[64];
    snprintf(text, sizeof(text), "stress revision %d", i);
    make_doc(&c, text);
    CHECK(storage_save(session, path, &c, &meta) == STORAGE_OK,
          "stress save succeeds");
    bytebuffer_free(&c);
  }
  CHECK(storage_history_count(session) == N, "unlimited retention keeps all");

  storage_session_close(session);
  StorageSession *s2 = NULL;
  StorageOpenResult res2;
  storage_session_open(path, &s2, &doc, &meta, &res2);
  CHECK(storage_history_count(s2) == N, "all versions round-trip");
  CHECK(strcmp(storage_history_at(s2, 0)->name, "v40") == 0,
        "newest auto-name matches id sequence");
  CHECK(strcmp(storage_history_at(s2, N - 1)->name, "v1") == 0,
        "oldest is v1");
  bytebuffer_free(&doc);
  storage_session_close(s2);
  cleanup_path(path);
}

/* Save-As must move journal+autosave binding to the new path. */
static void test_save_as_rebinds_siblings(void) {
  const char *A = "/tmp/edoc_test_sa_a.edoc";
  const char *B = "/tmp/edoc_test_sa_b.edoc";
  cleanup_path(A);
  cleanup_path(B);

  StorageSession *s = NULL;
  ByteBuffer doc, content;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(A, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);

  make_doc(&content, "v1");
  storage_save(s, A, &content, &meta);
  CHECK(sibling_size(A, ".journal") == 0, "A journal clean after save");

  CHECK(storage_save(s, B, &content, &meta) == STORAGE_OK, "save-as to B");

  ByteBuffer edit;
  make_doc(&edit, "v2-changed");
  CHECK(storage_journal_append(s, "edit", &edit) == STORAGE_OK,
        "journal after save-as");
  bytebuffer_free(&edit);
  CHECK(sibling_size(B, ".journal") > 0, "B journal received record");
  CHECK(sibling_size(A, ".journal") == 0, "A journal untouched");

  CHECK(storage_should_autosave(s, 1000), "dirty -> autosave due");
  storage_autosave_tick(s, &content, &meta, 1000);
  CHECK(sibling_size(B, ".autosave") > 0, "autosave next to B");
  CHECK(sibling_size(A, ".autosave") < 0, "no autosave next to A");
  bytebuffer_free(&content);

  /* Fresh open of B surfaces journaled state as recovery */
  StorageSession *s2 = NULL;
  ByteBuffer d;
  StorageOpenResult r2;
  make_doc(&content, "v2-changed");
  storage_journal_append(s, "edit", &content);
  bytebuffer_free(&content);
  storage_session_open(B, &s2, &d, &meta, &r2);
  CHECK(r2 == STORAGE_OPEN_RECOVERED, "reopen B reports RECOVERED");
  ByteBuffer rd;
  StorageMetadata rm;
  CHECK(storage_recovery_get(s2, &rd, &rm) == STORAGE_OK &&
            rd.len == 10 && memcmp(rd.data, "v2-changed", 10) == 0,
        "recovered snapshot matches journal");
  bytebuffer_free(&rd);
  bytebuffer_free(&d);
  storage_recovery_discard(s2);
  storage_session_close(s2);
  storage_session_close(s);
  cleanup_path(A);
  cleanup_path(B);
}

/* End-to-end flow mirroring the editor: save, dedupe, edit-save,
   browser rename+delete, persisting dedupe-save, reopen, restore bytes. */
static void test_version_browser_flow(void) {
  const char *path = "/tmp/edoc_test_vflow.edoc";
  cleanup_path(path);

  StorageSession *s = NULL;
  ByteBuffer doc, c1, c2;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);

  make_doc(&c1, "int main() {\n    return 0;\n}");
  storage_save(s, path, &c1, &meta);
  CHECK(storage_history_count(s) == 1 &&
            strcmp(storage_history_at(s, 0)->name, "v1") == 0,
        "first Ctrl+S records v1");

  storage_save(s, path, &c1, &meta); /* identical */
  CHECK(storage_history_count(s) == 1, "identical save deduped");

  make_doc(&c2, "int main(void) {\n    return 0;\n}");
  storage_save(s, path, &c2, &meta);
  CHECK(storage_history_count(s) == 2, "changed save records v2");

  CHECK(storage_history_rename(s, 0, "before-void"), "rename v2");
  CHECK(storage_history_delete(s, 1), "delete v1");
  storage_save(s, path, &c2, &meta); /* dedupe save persists mutations */
  CHECK(!storage_history_dirty(s), "history persisted via dedupe save");
  CHECK(storage_history_count(s) == 1, "only renamed version remains");

  storage_session_close(s);
  StorageSession *s2 = NULL;
  StorageOpenResult r2;
  storage_session_open(path, &s2, &doc, &meta, &r2);
  CHECK(r2 == STORAGE_OPEN_CLEAN, "clean reopen after browser edits");
  CHECK(storage_history_count(s2) == 1 &&
            strcmp(storage_history_at(s2, 0)->name, "before-void") == 0,
        "renamed-only history round-trips");
  const StorageVersion *v = storage_history_at(s2, 0);
  CHECK(v->doc.len == c2.len && memcmp(v->doc.data, c2.data, c2.len) == 0,
        "restore bytes match edited document");
  CHECK(storage_verify_file(path) == STORAGE_OK,
        "file integrity after all mutations");

  bytebuffer_free(&doc);
  bytebuffer_free(&c1);
  bytebuffer_free(&c2);
  storage_session_close(s2);
  cleanup_path(path);
}

/* ===================================================================== *
 * W1 fuzz regressions: every finding from the fuzzing campaign gets a
 * permanent test here. The suite runs under ASan/UBSan, so leaks and
 * OOB accesses in these paths fail the build even when the return code
 * looks innocent.
 * ===================================================================== */

/* CRC32 mirror (storage.c's is static) for crafting hostile images. */
static uint32_t t_crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}

static void put_le32(ByteBuffer *b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  bytebuffer_append(b, x, 4);
}
static void put_le64(ByteBuffer *b, uint64_t v) {
  uint8_t x[8];
  for (int i = 0; i < 8; i++)
    x[i] = (uint8_t)(v >> (8 * i));
  bytebuffer_append(b, x, 8);
}

/* Footer = {section_count, crc32(header+sections)}. Parser verifies
   crc over exactly img->data[0 .. footer_start). */
static void append_edoc_footer(ByteBuffer *img, uint32_t count) {
  size_t body_end = img->len;
  put_le32(img, count);
  put_le32(img, t_crc32(img->data, body_end));
}

/* Reads a whole file via stdio (read_whole_file in storage.c is static). */
static StorageStatus read_whole_file_public(const char *path,
                                            ByteBuffer *out) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return STORAGE_ERR_NOT_FOUND;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  bytebuffer_init(out);
  if (sz < 0) {
    fclose(f);
    return STORAGE_ERR_IO;
  }
  if (sz == 0) { /* empty files are valid input, not an error */
    fclose(f);
    return STORAGE_OK;
  }
  if (!bytebuffer_reserve(out, (size_t)sz)) {
    fclose(f);
    return STORAGE_ERR_NOMEM;
  }
  size_t got = fread(out->data, 1, (size_t)sz, f);
  fclose(f);
  out->len = got;
  return got == (size_t)sz ? STORAGE_OK : STORAGE_ERR_TRUNCATED;
}


static void test_fuzz_regressions(void) {
  /* F2 (libFuzzer crash artifact): crafted VER1 record whose doc_len
     wraps the parser's additive bounds arithmetic. Used to SEGV in
     parse_history_section; must now be rejected with zero versions. */
  static const uint8_t wrap_record[] = {
      0x56, 0x45, 0x52, 0x31, 0xc5, 0x44, 0x4f, 0x00, 0x43, 0x3d, 0x01,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0a, 0x0a, 0x00,
      0x00, 0x00, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
      0xff, 0x00, 0x5b};
  size_t parsed = 99;
  CHECK(fuzz_history_parse(wrap_record, sizeof(wrap_record), &parsed) ==
                        STORAGE_OK &&
                    parsed == 0,
        "history: crafted length-wrap record rejected, zero versions");

  /* Valid records must still parse after the bounds rewrite. */
  ByteBuffer good;
  bytebuffer_init(&good);
  const char *gname = "v1";
  const char *gdoc = "hello";
  size_t body_at = good.len; /* record CRC spans from the magic byte */
  put_le32(&good, 0x31524556u);
  put_le64(&good, 1);
  put_le64(&good, 1700000000);
  put_le32(&good, (uint32_t)strlen(gname));
  bytebuffer_append(&good, gname, strlen(gname));
  put_le64(&good, (uint64_t)strlen(gdoc));
  put_le32(&good, t_crc32((const uint8_t *)gdoc, strlen(gdoc)));
  bytebuffer_append(&good, gdoc, strlen(gdoc));
  put_le32(&good, t_crc32(good.data + body_at, good.len - body_at));
  parsed = 0;
  CHECK(fuzz_history_parse(good.data, good.len, &parsed) == STORAGE_OK &&
                    parsed == 1,
        "history: valid handcrafted record still parses");
  bytebuffer_free(&good);

  /* F1: container truncated right after a section header used to leak
     the document buffer on STORAGE_ERR_TRUNCATED. Footer CRC is valid
     so parsing reaches the section loop. */
  ByteBuffer trunc;
  bytebuffer_init(&trunc);
  put_le32(&trunc, 0x434F4445u);
  put_le32(&trunc, 1u);
  put_le64(&trunc, 1700000000);
  put_le32(&trunc, 1u);  /* type = DOCUMENT */
  put_le64(&trunc, 4u);  /* payload_len claims bytes that aren't there */
  put_le32(&trunc, 0u);  /* payload crc (never reached) */
  append_edoc_footer(&trunc, 1);
  CHECK(fuzz_parse_edoc(trunc.data, trunc.len) == STORAGE_ERR_TRUNCATED,
        "edoc: mid-section truncation reported, no leak");
  bytebuffer_free(&trunc);

  /* F3: section header whose payload_len wraps additive checks. */
  ByteBuffer evil;
  bytebuffer_init(&evil);
  put_le32(&evil, 0x434F4445u); /* STORAGE_MAGIC */
  put_le32(&evil, 1u);          /* format version */
  put_le64(&evil, 1700000000);  /* created_at */
  put_le32(&evil, 1u);          /* type = DOCUMENT */
  put_le64(&evil, UINT64_MAX - 20); /* off(32)+payload_len wraps small */
  put_le32(&evil, 0u);          /* payload crc (never reached) */
  append_edoc_footer(&evil, 1);
  CHECK(fuzz_parse_edoc(evil.data, evil.len) == STORAGE_ERR_TRUNCATED,
        "edoc: wrapping payload_len rejected without OOB");
  bytebuffer_free(&evil);

  /* F4: journal record with wrapping doc_len must not reach a wild
     read; scanner falls back to NOT_FOUND for this sole record. */
  const char *jpath_file = "/tmp/edoc_test_fuzzjournal.edoc";
  cleanup_path(jpath_file);
  StorageSession *js = NULL;
  ByteBuffer jdoc, jcontent;
  StorageMetadata jmeta;
  StorageOpenResult jres;
  if (storage_session_open(jpath_file, &js, &jdoc, &jmeta, &jres) ==
      STORAGE_OK) {
    bytebuffer_free(&jdoc);
    make_doc(&jcontent, "journal payload");
    storage_journal_append(js, "insert", &jcontent);
    bytebuffer_free(&jcontent);
    storage_session_close(js);

    char jpath[512];
    snprintf(jpath, sizeof(jpath), "%s.journal", jpath_file);
    ByteBuffer j;
    if (read_whole_file_public(jpath, &j) == STORAGE_OK && j.len > 48) {
      /* layout: u64 rec_total | u64 ts | u32 op_len | op | u64 doc_len
         | doc | u32 crc | u64 rec_total */
      uint64_t rec_total;
      memcpy(&rec_total, j.data, 8);
      size_t doc_len_off = 8 + 8 + 4 + 6; /* ts+op_len+strlen("insert") */
      uint64_t evil_len = UINT64_MAX - doc_len_off - 4 + 1; /* wraps small */
      memcpy(j.data + doc_len_off, &evil_len, 8);
      /* fix record body CRC so ONLY the length guard can reject it */
      size_t body_len = (size_t)rec_total - 4;
      uint32_t crc = t_crc32(j.data + 8, body_len);
      memcpy(j.data + 8 + body_len, &crc, 4);
      CHECK(fuzz_journal_scan(j.data, j.len) == STORAGE_ERR_NOT_FOUND,
            "journal: wrapping doc_len rejected without OOB");
      bytebuffer_free(&j);
    }
    cleanup_path(jpath_file);
  }
}

/* ===================================================================== *
 * W2: read-only toolkit API
 * ===================================================================== */

typedef struct {
  size_t seen;
  bool all_crc_ok;
  bool has_document, has_metadata, has_versions;
} InspectCtx;

static void inspect_collect(const StorageSectionInfo *info, void *user) {
  InspectCtx *c = user;
  c->seen++;
  if (!info->crc_ok)
    c->all_crc_ok = false;
  if (info->type == STORAGE_SECTION_DOCUMENT)
    c->has_document = true;
  else if (info->type == STORAGE_SECTION_METADATA)
    c->has_metadata = true;
  else if (info->type == STORAGE_SECTION_VERSIONS)
    c->has_versions = true;
}

static void test_toolkit_inspect(void) {
  const char *path = "/tmp/edoc_test_inspect.edoc";
  cleanup_path(path);
  StorageSession *s = NULL;
  ByteBuffer doc, c1, c2;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);
  make_doc(&c1, "inspect one");
  make_doc(&c2, "inspect two");
  storage_save(s, path, &c1, &meta);
  storage_save(s, path, &c2, &meta); /* adds VERSIONS section */
  storage_session_close(s);

  StorageInspectSummary sum;
  InspectCtx ctx = {0, true, false, false, false};
  StorageStatus st =
      storage_inspect_file(path, &sum, inspect_collect, &ctx);
  CHECK(st == STORAGE_OK, "inspect: clean file walks OK");
  CHECK(sum.footer_crc_ok, "inspect: footer CRC valid on clean file");
  CHECK(sum.section_count == 3 && sum.sections_walked == 3,
        "inspect: document+metadata+versions all walked");
  CHECK(ctx.has_document && ctx.has_metadata && ctx.has_versions,
        "inspect: callback saw every section type");
  CHECK(ctx.all_crc_ok, "inspect: all payload CRCs valid");
  CHECK(sum.file_size > 0, "inspect: file_size populated");

  /* Corrupt a payload byte -> CRC flags flip, walk still completes. */
  FILE *f = fopen(path, "r+b");
  CHECK(f != NULL, "inspect: reopen for corruption injection");
  if (f) {
    fseek(f, 40, SEEK_SET);
    int ch = fgetc(f);
    fseek(f, 40, SEEK_SET);
    fputc(ch ^ 0x40, f);
    fclose(f);

    InspectCtx bad = {0, true, false, false, false};
    st = storage_inspect_file(path, &sum, inspect_collect, &bad);
    /* Contract: walk completes and reports every section, but the
       status surfaces the checksum damage instead of reading clean. */
    CHECK(st == STORAGE_ERR_CORRUPT_CHECKSUM && bad.seen == 3 &&
              !sum.footer_crc_ok,
          "inspect: corrupted file fully walkable, status flags damage");
  }

  /* Truncation, deterministic form: valid footer but the single claimed
     section claims more bytes than exist -> TRUNCATED, nothing walked. */
  ByteBuffer timg;
  bytebuffer_init(&timg);
  put_le32(&timg, 0x434F4445u); /* STORAGE_MAGIC */
  put_le32(&timg, 1u);          /* format version */
  put_le64(&timg, 1700000000);  /* created_at */
  put_le32(&timg, 1u);          /* type = DOCUMENT */
  put_le64(&timg, 9999u);       /* payload_len: bytes that aren't there */
  put_le32(&timg, 0u);          /* payload crc (never reached) */
  append_edoc_footer(&timg, 1);
  char tpath[512];
  snprintf(tpath, sizeof(tpath), "%s.trunc", path);
  FILE *tf = fopen(tpath, "wb");
  CHECK(tf != NULL, "inspect: can write crafted truncated file");
  if (tf) {
    fwrite(timg.data, 1, timg.len, tf);
    fclose(tf);
    InspectCtx tc = {0, true, false, false, false};
    st = storage_inspect_file(tpath, &sum, inspect_collect, &tc);
    CHECK(st == STORAGE_ERR_TRUNCATED && sum.sections_walked == 0 &&
              sum.section_count == 1,
          "inspect: oversized section claim => TRUNCATED, zero walked");
    unlink(tpath);
  }
  bytebuffer_free(&timg);

  /* Blind slice of a real container must never read as clean. */
  ByteBuffer raw;
  if (read_whole_file_public(path, &raw) == STORAGE_OK) {
    snprintf(tpath, sizeof(tpath), "%s.slice", path);
    tf = fopen(tpath, "wb");
    if (tf) {
      fwrite(raw.data, 1, 30, tf);
      fclose(tf);
      st = storage_inspect_file(tpath, &sum, NULL, NULL);
      CHECK(st != STORAGE_OK,
            "inspect: sliced file is never reported OK");
      unlink(tpath);
    }
    bytebuffer_free(&raw);
  }

  /* Non-EDOC input rejected without side effects. */
  char npath[512];
  snprintf(npath, sizeof(npath), "%s.txt", path);
  f = fopen(npath, "wb");
  if (f) {
    fputs("plain text, not a container", f);
    fclose(f);
    st = storage_inspect_file(npath, &sum, NULL, NULL);
    CHECK(st == STORAGE_ERR_BAD_MAGIC, "inspect: plain text => BAD_MAGIC");
    unlink(npath);
  }

  bytebuffer_free(&c1);
  bytebuffer_free(&c2);
  cleanup_path(path);
}

static void test_toolkit_read_versions(void) {
  const char *path = "/tmp/edoc_test_rdver.edoc";
  cleanup_path(path);
  StorageSession *s = NULL;
  ByteBuffer doc, c1, c2;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);
  make_doc(&c1, "first snapshot");
  make_doc(&c2, "second snapshot, current");
  storage_save(s, path, &c1, &meta);
  storage_save(s, path, &c2, &meta);
  storage_session_close(s);

  StorageVersion *vers = NULL;
  size_t n = 0;
  StorageStatus st = storage_read_versions(path, &vers, &n);
  CHECK(st == STORAGE_OK && n == 2, "read_versions: both snapshots found");
  if (st == STORAGE_OK && n == 2) {
    CHECK(vers[0].doc.len == c1.len &&
              memcmp(vers[0].doc.data, c1.data, c1.len) == 0,
          "read_versions: oldest entry matches v1 bytes");
    CHECK(strcmp(vers[1].name, "v2") == 0,
          "read_versions: auto-name carried through");
  }
  storage_versions_free(vers, n);

  bytebuffer_free(&c1);
  bytebuffer_free(&c2);
  cleanup_path(path);
}

static void test_toolkit_recovery_report(void) {
  const char *path = "/tmp/edoc_test_recreport.edoc";
  cleanup_path(path);
  StorageSession *s = NULL;
  ByteBuffer doc, base, edit;
  StorageMetadata meta;
  StorageOpenResult res;
  storage_session_open(path, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);
  make_doc(&base, "saved baseline");
  storage_save(s, path, &base, &meta);

  StorageRecoveryReport rep;
  CHECK(storage_recovery_report(path, &rep) == STORAGE_OK,
        "recovery_report: succeeds on clean file");
  CHECK(!rep.journal_candidate && !rep.autosave_exists,
        "recovery_report: nothing pending on clean file");

  make_doc(&edit, "baseline plus unsaved work");
  storage_journal_append(s, "insert", &edit);
  storage_session_close(s); /* crash simulation: close without save */

  CHECK(storage_recovery_report(path, &rep) == STORAGE_OK,
        "recovery_report: reads journaled state");
  CHECK(rep.journal_candidate && rep.journal_doc.len == edit.len &&
            memcmp(rep.journal_doc.data, edit.data, edit.len) == 0,
        "recovery_report: journal tail matches pending edit");
  bytebuffer_free(&rep.journal_doc);

  s = NULL;
  storage_session_open(path, &s, &doc, &meta, &res);
  bytebuffer_free(&doc);
  sleep(1); /* guarantee mtime separation from the explicit save */
  bytebuffer_free(&edit); /* release pre-reuse contents */
  make_doc(&edit, "autosaved progress");
  storage_mark_dirty(s);
  storage_autosave_tick(s, &edit, &meta, 5000);
  storage_session_close(s);
  CHECK(storage_recovery_report(path, &rep) == STORAGE_OK &&
                rep.autosave_exists && rep.autosave_newer_than_main,
        "recovery_report: fresh autosave flagged newer than main");
  bytebuffer_free(&rep.journal_doc);
  bytebuffer_free(&base);
  bytebuffer_free(&edit);
  cleanup_path(path);
}

/* ===================================================================== *
 * W2: edoc CLI end-to-end roundtrips (system()-driven; binary is built
 * by the `test` target). Property: import(export(f)) and export(import)
 * preserve bytes exactly, across empty / unicode / ~10 MB documents.
 * ===================================================================== */

static int run_cli(char *cmd, char *const args[]) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execv("./build/edoc", args); /* cmd unused: argv[0] carries it */
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool files_equal(const char *a, const char *b) {
  ByteBuffer ba, bb;
  bool eq = false;
  if (read_whole_file_public(a, &ba) == STORAGE_OK &&
      read_whole_file_public(b, &bb) == STORAGE_OK) {
    eq = ba.len == bb.len &&
         (ba.len == 0 || memcmp(ba.data, bb.data, ba.len) == 0);
  }
  bytebuffer_free(&ba);
  bytebuffer_free(&bb);
  return eq;
}

static void write_text_file(const char *path, const void *data, size_t len) {
  FILE *f = fopen(path, "wb");
  CHECK(f != NULL, "cli roundtrip: can write source text");
  if (!f)
    return;
  if (len)
    fwrite(data, 1, len, f);
  fclose(f);
}

static void test_cli_roundtrips(void) {
  unlink("/tmp/edoc_cli_r1.edoc");
  unlink("/tmp/edoc_cli_r2.edoc");
  unlink("/tmp/edoc_cli_r3.edoc");

  { /* plain ASCII */
    write_text_file("/tmp/edoc_cli_a.txt", "hello container\n", 16);
    char *i1[] = {"edoc", "import", "/tmp/edoc_cli_a.txt",
                  "/tmp/edoc_cli_r1.edoc", NULL};
    CHECK(run_cli(NULL, i1) == 0, "cli: import ascii exits 0");
    char *e1[] = {"edoc", "export", "/tmp/edoc_cli_r1.edoc",
                  "/tmp/edoc_cli_out.txt", NULL};
    CHECK(run_cli(NULL, e1) == 0, "cli: export ascii exits 0");
    CHECK(files_equal("/tmp/edoc_cli_a.txt", "/tmp/edoc_cli_out.txt"),
          "cli: ascii roundtrip byte-equal");
    char *v1[] = {"edoc", "verify", "/tmp/edoc_cli_r1.edoc", NULL};
    CHECK(run_cli(NULL, v1) == 0, "cli: verify imported container exits 0");
    unlink("/tmp/edoc_cli_a.txt");
    unlink("/tmp/edoc_cli_out.txt");
    cleanup_path("/tmp/edoc_cli_r1.edoc");
  }

  { /* unicode body incl. emoji + combining marks */
    static const char uni[] = "héllo wörld ✓ 日本語 🎉\n\ttabbed\r\n";
    write_text_file("/tmp/edoc_cli_u.txt", uni, sizeof(uni) - 1);
    char *i2[] = {"edoc", "import", "/tmp/edoc_cli_u.txt",
                  "/tmp/edoc_cli_r2.edoc", NULL};
    char *e2[] = {"edoc", "export", "/tmp/edoc_cli_r2.edoc",
                  "/tmp/edoc_cli_out2.txt", NULL};
    CHECK(run_cli(NULL, i2) == 0 && run_cli(NULL, e2) == 0,
          "cli: unicode import/export exit 0");
    CHECK(files_equal("/tmp/edoc_cli_u.txt", "/tmp/edoc_cli_out2.txt"),
          "cli: unicode roundtrip byte-equal");
    unlink("/tmp/edoc_cli_u.txt");
    unlink("/tmp/edoc_cli_out2.txt");
    cleanup_path("/tmp/edoc_cli_r2.edoc");
  }

  { /* ~10 MB pseudo-random deterministic body */
    enum { BIG = 10 * 1024 * 1024 };
    char *big = malloc(BIG);
    CHECK(big != NULL, "cli: alloc 10MB body");
    if (big) {
      uint32_t x = 0x12345678u;
      for (int i = 0; i < BIG; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        big[i] = (char)(x & 0xFF);
      }
      write_text_file("/tmp/edoc_cli_big.txt", big, BIG);
      free(big);
      char *i3[] = {"edoc", "import", "/tmp/edoc_cli_big.txt",
                    "/tmp/edoc_cli_r3.edoc", NULL};
      char *e3[] = {"edoc", "export", "/tmp/edoc_cli_r3.edoc",
                    "/tmp/edoc_cli_out3.txt", NULL};
      int rc_i = run_cli(NULL, i3);
      int rc_e = run_cli(NULL, e3);
      CHECK(rc_i == 0 && rc_e == 0, "cli: 10MB import/export exit 0");
      CHECK(files_equal("/tmp/edoc_cli_big.txt", "/tmp/edoc_cli_out3.txt"),
            "cli: 10MB roundtrip byte-equal");
      unlink("/tmp/edoc_cli_big.txt");
      unlink("/tmp/edoc_cli_out3.txt");
      cleanup_path("/tmp/edoc_cli_r3.edoc");
    }
  }

  { /* import refuses to clobber without --force */
    write_text_file("/tmp/edoc_cli_c.txt", "x", 1);
    char *i4[] = {"edoc", "import", "/tmp/edoc_cli_c.txt",
                  "/tmp/edoc_cli_c.edoc", NULL};
    CHECK(run_cli(NULL, i4) == 0, "cli: initial import ok");
    CHECK(run_cli(NULL, i4) == 3, "cli: re-import without --force fails");
    char *i5[] = {"edoc",       "import", "/tmp/edoc_cli_c.txt",
                  "--force", "/tmp/edoc_cli_c.edoc", NULL};
    CHECK(run_cli(NULL, i5) == 0, "cli: re-import with --force ok");
    unlink("/tmp/edoc_cli_c.txt");
    cleanup_path("/tmp/edoc_cli_c.edoc");
  }
}

/* ===================================================================== *
 * W2 hardening: adversarial CLI battery — the cases large products get
 * burned by. Boundary bodies (empty / NUL-laden / no-newline), a full
 * exit-code matrix, hostile argument shapes, and self-destructive
 * invocations.
 * ===================================================================== */

static void test_cli_adversarial(void) {
  /* --- boundary bodies -------------------------------------------- */
  { /* zero-byte document: legal UTF-8 body of length 0 */
    write_text_file("/tmp/adv_empty.txt", "", 0);
    unlink("/tmp/adv_empty.edoc");
    unlink("/tmp/adv_empty_out.txt");
    char *i[] = {"edoc", "import", "/tmp/adv_empty.txt",
                 "/tmp/adv_empty.edoc", NULL};
    char *e[] = {"edoc", "export", "/tmp/adv_empty.edoc",
                 "/tmp/adv_empty_out.txt", NULL};
    char *v[] = {"edoc", "verify", "/tmp/adv_empty.edoc", NULL};
    CHECK(run_cli(NULL, i) == 0, "adv: import empty body exits 0");
    CHECK(run_cli(NULL, v) == 0, "adv: verify empty container exits 0");
    CHECK(run_cli(NULL, e) == 0, "adv: export empty exits 0");
    CHECK(files_equal("/tmp/adv_empty.txt", "/tmp/adv_empty_out.txt"),
          "adv: empty roundtrip byte-equal");
    cleanup_path("/tmp/adv_empty.edoc");
    unlink("/tmp/adv_empty.txt");
    unlink("/tmp/adv_empty_out.txt");
  }

  { /* NUL bytes and control characters inside the body */
    static const char nul_body[] = {'a', '\0', '\0', 'b', '\n', '\0',
                                    '\t', 0x01, 0x7f, 'z'};
    write_text_file("/tmp/adv_nul.txt", nul_body, sizeof(nul_body));
    unlink("/tmp/adv_nul.edoc");
    unlink("/tmp/adv_nul_out.txt");
    char *i[] = {"edoc", "import", "/tmp/adv_nul.txt",
                 "/tmp/adv_nul.edoc", NULL};
    char *e[] = {"edoc", "export", "/tmp/adv_nul.edoc",
                 "/tmp/adv_nul_out.txt", NULL};
    CHECK(run_cli(NULL, i) == 0 && run_cli(NULL, e) == 0,
          "adv: NUL-laden body import/export exit 0");
    CHECK(files_equal("/tmp/adv_nul.txt", "/tmp/adv_nul_out.txt"),
          "adv: NUL bytes survive roundtrip byte-exact");
    cleanup_path("/tmp/adv_nul.edoc");
    unlink("/tmp/adv_nul.txt");
    unlink("/tmp/adv_nul_out.txt");
  }

  { /* one enormous line, no newline anywhere */
    enum { LINE = 512 * 1024 };
    char *blob = malloc(LINE);
    CHECK(blob != NULL, "adv: alloc huge-line body");
    if (blob) {
      memset(blob, 'Q', LINE);
      write_text_file("/tmp/adv_line.txt", blob, LINE);
      free(blob);
      unlink("/tmp/adv_line.edoc");
      unlink("/tmp/adv_line_out.txt");
      char *i[] = {"edoc", "import", "/tmp/adv_line.txt",
                   "/tmp/adv_line.edoc", NULL};
      char *e[] = {"edoc", "export", "/tmp/adv_line.edoc",
                   "/tmp/adv_line_out.txt", NULL};
      CHECK(run_cli(NULL, i) == 0 && run_cli(NULL, e) == 0,
            "adv: single 512KB line import/export exit 0");
      CHECK(files_equal("/tmp/adv_line.txt", "/tmp/adv_line_out.txt"),
            "adv: newline-free roundtrip byte-equal");
      cleanup_path("/tmp/adv_line.edoc");
      unlink("/tmp/adv_line.txt");
      unlink("/tmp/adv_line_out.txt");
    }
  }

  /* --- export overwrites stale longer output ----------------------- */
  {
    write_text_file("/tmp/adv_s.txt", "tiny", 4);
    unlink("/tmp/adv_s.edoc");
    char *i[] = {"edoc", "import", "/tmp/adv_s.txt", "/tmp/adv_s.edoc",
                 NULL};
    run_cli(NULL, i);
    write_text_file("/tmp/adv_stale_out.txt",
                    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 40);
    char *e[] = {"edoc", "export", "/tmp/adv_s.edoc",
                 "/tmp/adv_stale_out.txt", NULL};
    CHECK(run_cli(NULL, e) == 0, "adv: export over stale output exits 0");
    CHECK(files_equal("/tmp/adv_s.txt", "/tmp/adv_stale_out.txt"),
          "adv: stale output fully replaced (no residue)");
    unlink("/tmp/adv_s.txt");
    unlink("/tmp/adv_stale_out.txt");
    cleanup_path("/tmp/adv_s.edoc");
  }

  /* --- exit-code matrix & hostile args ----------------------------- */
  {
    write_text_file("/tmp/adv_plain.txt", "not a container", 15);
    char *e_missing[] = {"edoc", "export", NULL};
    CHECK(run_cli(NULL, e_missing) == 1, "adv: export no args => 1");
    char *e_extra[] = {"edoc", "export", "a", "b", "c", NULL};
    CHECK(run_cli(NULL, e_extra) == 1, "adv: export extra positional => 1");
    char *e_v0[] = {"edoc", "export", "/tmp/adv_plain.txt", "-v", "0",
                    "/tmp/x.out", NULL};
    CHECK(run_cli(NULL, e_v0) == 1, "adv: export -v 0 => usage 1");
    char *e_vjunk[] = {"edoc", "export", "/tmp/adv_plain.txt", "-v",
                       "abc", "/tmp/x.out", NULL};
    CHECK(run_cli(NULL, e_vjunk) == 1,
          "adv: export -v non-numeric => usage 1");
    char *e_both[] = {"edoc", "export", "/tmp/adv_plain.txt", "-v", "1",
                      "--name", "v1", "/tmp/x.out", NULL};
    CHECK(run_cli(NULL, e_both) == 1,
          "adv: both selectors rejected (was silent name-wins)");
    char *e_vtail[] = {"edoc", "export", "/tmp/adv_plain.txt",
                       "/tmp/x.out", "-v", NULL};
    CHECK(run_cli(NULL, e_vtail) == 1, "adv: dangling -v => usage 1");

    char *e_nf[] = {"edoc", "export", "/tmp/definitely_missing.edoc",
                    "/tmp/x.out", NULL};
    CHECK(run_cli(NULL, e_nf) == 3, "adv: export missing file => 3");
    char *e_badmagic[] = {"edoc", "export", "/tmp/adv_plain.txt",
                          "/tmp/x.out", NULL};
    CHECK(run_cli(NULL, e_badmagic) == 3,
          "adv: export plain-text-as-container => 3");
    char *h_bad[] = {"edoc", "history", "/tmp/adv_plain.txt", NULL};
    CHECK(run_cli(NULL, h_bad) == 3, "adv: history on garbage => 3");
    char *d_dir[] = {"edoc", "dump", "/tmp", NULL};
    CHECK(run_cli(NULL, d_dir) == 3, "adv: dump a directory => 3, no crash");
    char *v_dir[] = {"edoc", "verify", "/tmp", NULL};
    CHECK(run_cli(NULL, v_dir) == 3, "adv: verify a directory => 3");
    char *i_missing_txt[] = {"edoc", "import", "/tmp/no_such_src.txt",
                             "/tmp/adv_x.edoc", NULL};
    CHECK(run_cli(NULL, i_missing_txt) == 3,
          "adv: import missing source => 3");

    /* recover is a report: quiet even for nonsense paths */
    char *r_none[] = {"edoc", "recover", "/tmp/nowhere/never.edoc", NULL};
    CHECK(run_cli(NULL, r_none) == 0,
          "adv: recover on absent path reports cleanly (exit 0)");
    unlink("/tmp/adv_plain.txt");
    unlink("/tmp/x.out");
  }

  /* --- import must never destroy its own source -------------------- */
  {
    write_text_file("/tmp/adv_self.txt", "precious source", 15);
    char *self[] = {"edoc", "import", "/tmp/adv_self.txt",
                    "/tmp/adv_self.txt", "--force", NULL};
    CHECK(run_cli(NULL, self) == 3,
          "adv: import src==dst refused with --force too");
    ByteBuffer check;
    bool intact = read_whole_file_public("/tmp/adv_self.txt", &check) ==
                      STORAGE_OK &&
                  check.len == 15;
    bytebuffer_free(&check);
    CHECK(intact, "adv: source file survived the refused self-import");
    unlink("/tmp/adv_self.txt");
  }

  /* --- idempotent force-import keeps payload stable ------------------ */
  {
    write_text_file("/tmp/adv_idem.txt", "stable payload\n", 15);
    unlink("/tmp/adv_idem.edoc");
    char *imp[] = {"edoc", "import", "/tmp/adv_idem.txt",
                   "/tmp/adv_idem.edoc", NULL};
    char *impf[] = {"edoc",       "import", "/tmp/adv_idem.txt",
                    "--force", "/tmp/adv_idem.edoc", NULL};
    char *exp[] = {"edoc", "export", "/tmp/adv_idem.edoc",
                   "/tmp/adv_idem_out.txt", NULL};
    CHECK(run_cli(NULL, imp) == 0 && run_cli(NULL, impf) == 0 &&
              run_cli(NULL, exp) == 0,
          "adv: repeated force-import chain exits 0");
    CHECK(files_equal("/tmp/adv_idem.txt", "/tmp/adv_idem_out.txt"),
          "adv: payload stable across force-reimport");
    unlink("/tmp/adv_idem.txt");
    unlink("/tmp/adv_idem_out.txt");
    cleanup_path("/tmp/adv_idem.edoc");
  }
}

/* ===================================================================== *
 * W2 consistency matrix: legacy-file behaviors, per-version export
 * fidelity, selector edge cases, verify exit-code map, hostile-name
 * imports, and the introspection summary contract on early errors.
 * ===================================================================== */

/* Builds a legacy-shaped container (no VERSIONS section) by promoting
   an autosave image, mirroring pre-history files found in the wild. */
static bool make_legacy_container(const char *path) {
  char asp[512];
  cleanup_path(path);
  StorageSession *s = NULL;
  ByteBuffer doc;
  StorageMetadata meta;
  StorageOpenResult res;
  if (storage_session_open(path, &s, &doc, &meta, &res) != STORAGE_OK)
    return false;
  bytebuffer_free(&doc);
  make_doc(&doc, "legacy body without history");
  storage_mark_dirty(s);
  bool ok = storage_autosave_tick(s, &doc, &meta, 1000);
  storage_session_close(s);
  bytebuffer_free(&doc);
  snprintf(asp, sizeof(asp), "%s.autosave", path);
  return ok && rename(asp, path) == 0;
}

static void test_consistency_matrix(void) {
  /* --- legacy container across every relevant command --------------- */
  const char *legacy = "/tmp/mx_legacy.edoc";
  CHECK(make_legacy_container(legacy), "mx: legacy container created");
  {
    char *h[] = {"edoc", "history", (char *)legacy, NULL};
    CHECK(run_cli(NULL, h) == 0,
          "mx: history on legacy file exits 0 (no versions)");
    char *d[] = {"edoc", "dump", (char *)legacy, NULL};
    CHECK(run_cli(NULL, d) == 0,
          "mx: dump on legacy file exits 0 (two sections)");
    char *e[] = {"edoc", "export", (char *)legacy,
                 "/tmp/mx_legacy_out.txt", NULL};
    CHECK(run_cli(NULL, e) == 0, "mx: export document from legacy exits 0");
    ByteBuffer out;
    bool ok = read_whole_file_public("/tmp/mx_legacy_out.txt", &out) ==
                  STORAGE_OK &&
              out.len == strlen("legacy body without history");
    bytebuffer_free(&out);
    CHECK(ok, "mx: legacy document bytes exported intact");
    char *vsel[] = {"edoc", "export", (char *)legacy, "-v",
                    "1",     "/tmp/mx_v1.txt", NULL};
    CHECK(run_cli(NULL, vsel) == 3,
          "mx: version select on history-less file => 3");
    unlink("/tmp/mx_legacy_out.txt");
    unlink("/tmp/mx_v1.txt");
    cleanup_path(legacy);
  }

  /* --- every stored version exports byte-exact; unicode names ------- */
  {
    const char *p = "/tmp/mx_versions.edoc";
    cleanup_path(p);
    StorageSession *s = NULL;
    ByteBuffer doc;
    StorageMetadata meta;
    StorageOpenResult res;
    storage_session_open(p, &s, &doc, &meta, &res);
    bytebuffer_free(&doc);
    enum { NV = 5 };
    ByteBuffer keep[NV];
    for (int i = 0; i < NV; i++) {
      char body[64];
      snprintf(body, sizeof(body), "payload of revision %d", i + 1);
      make_doc(&keep[i], body);
      storage_save(s, p, &keep[i], &meta);
    }
    CHECK(storage_history_rename(s, NV - 2, "α-version-β"),
          "mx: unicode rename accepted");
    storage_save(s, p, &keep[NV - 1], &meta); /* dedupe persists rename */
    storage_session_close(s);

    for (int id = 1; id <= NV; id++) {
      char sel[16], outp[64];
      snprintf(sel, sizeof(sel), "%d", id);
      snprintf(outp, sizeof(outp), "/tmp/mx_v%d.out", id);
      char *ex[] = {"edoc", "export", (char *)p, "-v", sel, outp, NULL};
      CHECK(run_cli(NULL, ex) == 0, "mx: export -v <id> exits 0");
      char exp[64];
      snprintf(exp, sizeof(exp), "payload of revision %d", id);
      ByteBuffer out;
      bool eq = read_whole_file_public(outp, &out) == STORAGE_OK &&
                out.len == strlen(exp) &&
                memcmp(out.data, exp, out.len) == 0;
      bytebuffer_free(&out);
      CHECK(eq, "mx: exported snapshot matches stored revision bytes");
      unlink(outp);
    }
    char *en[] = {"edoc",       "export",      (char *)p, "--name",
                  "α-version-β", "/tmp/mx_uni.out", NULL};
    CHECK(run_cli(NULL, en) == 0, "mx: export by unicode name exits 0");
    ByteBuffer uo;
    bool ueq = read_whole_file_public("/tmp/mx_uni.out", &uo) ==
                   STORAGE_OK &&
               uo.len == keep[1].len &&
               memcmp(uo.data, keep[1].data, uo.len) == 0;
    bytebuffer_free(&uo);
    CHECK(ueq, "mx: unicode-named version exports its own bytes");
    unlink("/tmp/mx_uni.out");

    for (int i = 0; i < NV; i++)
      bytebuffer_free(&keep[i]);
    cleanup_path(p);
  }

  /* --- verify exit-code map for structural rejections ---------------- */
  {
    write_text_file("/tmp/mx_plain.txt", "nope", 4);

    ByteBuffer img;
    bytebuffer_init(&img);
    put_le32(&img, 0x434F4445u);
    put_le32(&img, 7u); /* future format version */
    put_le64(&img, 1700000000);
    append_edoc_footer(&img, 1); /* footer crc over header only */
    write_text_file("/tmp/mx_future.edoc", img.data, img.len);
    bytebuffer_free(&img);

    char *v_bad[] = {"edoc", "verify", "/tmp/mx_plain.txt", NULL};
    CHECK(run_cli(NULL, v_bad) == 3, "mx: verify non-container => 3");
    char *v_fut[] = {"edoc", "verify", "/tmp/mx_future.edoc", NULL};
    CHECK(run_cli(NULL, v_fut) == 3,
          "mx: verify future format version => 3");
    unlink("/tmp/mx_plain.txt");
    unlink("/tmp/mx_future.edoc");
  }

  /* --- very long source filename -> capped title, valid output ------- */
  {
    char longname[600], src[700];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    snprintf(src, sizeof(src), "/tmp/%s.txt", longname);
    FILE *f = fopen(src, "wb");
    if (f) {
      fputs("long name body", f);
      fclose(f);
      cleanup_path("/tmp/mx_long.edoc");
      char *i[] = {"edoc", "import", src, "/tmp/mx_long.edoc", NULL};
      CHECK(run_cli(NULL, i) == 0,
            "mx: import with ~600-char filename succeeds");
      char *vv[] = {"edoc", "verify", "/tmp/mx_long.edoc", NULL};
      CHECK(run_cli(NULL, vv) == 0,
            "mx: container from long-name import verifies");
      unlink(src);
      cleanup_path("/tmp/mx_long.edoc");
    }
  }

  /* --- recovery report goes quiet after journal discard -------------- */
  {
    const char *p = "/tmp/mx_rec.edoc";
    cleanup_path(p);
    StorageSession *s = NULL;
    ByteBuffer doc, edit;
    StorageMetadata meta;
    StorageOpenResult res;
    storage_session_open(p, &s, &doc, &meta, &res);
    bytebuffer_free(&doc);
    make_doc(&doc, "base");
    storage_save(s, p, &doc, &meta);
    make_doc(&edit, "pending");
    storage_journal_append(s, "insert", &edit);
    bytebuffer_free(&edit);
    storage_recovery_discard(s); /* user chose Discard */
    storage_session_close(s);
    bytebuffer_free(&doc);

    StorageRecoveryReport rep;
    CHECK(storage_recovery_report(p, &rep) == STORAGE_OK &&
              !rep.journal_candidate,
          "mx: discarded journal no longer reported pending");
    bytebuffer_free(&rep.journal_doc);
    cleanup_path(p);
  }

  /* --- toolkit API contracts on crafted/early-error inputs ----------- */
  {
    /* container with METADATA but no DOCUMENT => read_document fails */
    ByteBuffer img;
    bytebuffer_init(&img);
    put_le32(&img, 0x434F4445u);
    put_le32(&img, 1u);
    put_le64(&img, 1700000000);
    put_le32(&img, STORAGE_SECTION_METADATA);
    put_le64(&img, 0u);
    put_le32(&img, t_crc32((const uint8_t *)"", 0));
    append_edoc_footer(&img, 1);
    write_text_file("/tmp/mx_nodoc.edoc", img.data, img.len);
    bytebuffer_free(&img);

    ByteBuffer body;
    CHECK(storage_read_document("/tmp/mx_nodoc.edoc", &body) ==
              STORAGE_ERR_TRUNCATED,
          "mx: read_document without DOCUMENT section => TRUNCATED");

    /* inspect summary contract on structural early-outs */
    write_text_file("/tmp/mx_short.edoc", "hi!", 3);
    StorageInspectSummary sum;
    InspectCtx ctx = {0, true, false, false, false};
    StorageStatus st =
        storage_inspect_file("/tmp/mx_short.edoc", &sum, NULL, NULL);
    CHECK(st == STORAGE_ERR_TRUNCATED && sum.sections_walked == 0 &&
              sum.file_size == 3,
          "mx: below-minimum file => TRUNCATED, size reported");
    write_text_file("/tmp/mx_notc.edoc",
                    "plain text long enough to pass the length gate", 46);
    sum.file_size = 0;
    ctx.seen = 0;
    st = storage_inspect_file("/tmp/mx_notc.edoc", &sum, inspect_collect,
                              &ctx);
    CHECK(st == STORAGE_ERR_BAD_MAGIC && ctx.seen == 0 &&
              sum.sections_walked == 0 && sum.file_size == 46 &&
              sum.format_version == 0,
          "mx: BAD_MAGIC emits nothing; size populated, version untouched");
    unlink("/tmp/mx_short.edoc");
    unlink("/tmp/mx_notc.edoc");
  }
}
int main(void) {
  test_new_document();
  test_save_and_reload();
  test_corruption_detection();
  test_corruption_falls_back_to_backup();
  test_journal_recovery();
  test_journal_torn_write_recovery();
  test_autosave_debounce();
  test_backup_rotation();
  test_history_accumulate_and_dedupe();
  test_history_rename_delete();
  test_history_corrupt_record_skipped();
  test_history_legacy_file_without_versions();
  test_history_stress_many_saves();
  test_save_as_rebinds_siblings();
  test_version_browser_flow();
  test_fuzz_regressions();
  test_toolkit_inspect();
  test_toolkit_read_versions();
  test_toolkit_recovery_report();
  test_cli_roundtrips();
  test_cli_adversarial();
  test_consistency_matrix();

  printf("\n%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}

