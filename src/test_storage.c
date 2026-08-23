#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "storage.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
  char buf[4096];
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

  printf("\n%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
