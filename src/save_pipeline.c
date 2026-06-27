#define _POSIX_C_SOURCE 200809L
#include "save_pipeline.h"
#include "formatter.h"
#include "language.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =====================================================================
 * Stage 1: Validate + normalize
 * ===================================================================== */
static bool stage_validate(const PipelineCtx *ctx, char *diag, size_t diag_sz) {
  if (!ctx->storage) {
    snprintf(diag, diag_sz, "no storage session bound");
    return false;
  }
  if (!ctx->text && ctx->text_len > 0) {
    snprintf(diag, diag_sz, "document buffer is NULL with non-zero length");
    return false;
  }
  /* Meta sanity */
  if (ctx->meta && ctx->meta->created_at == 0)
    ctx->meta->created_at = (uint64_t)time(NULL);
  return true;
}

/* =====================================================================
 * Stage 2: Language-aware formatting
 * ===================================================================== */
static FormatResult stage_format(const PipelineCtx *ctx) {
  if (ctx->flags & PIPE_FLAG_SKIP_FORMAT) {
    FormatResult r = {FMT_UNAVAILABLE, NULL, 0, "formatting skipped by caller"};
    return r;
  }
  if (ctx->lang == LANG_UNKNOWN) {
    FormatResult r = {FMT_UNAVAILABLE, NULL, 0,
                      "language unknown, skipping formatter"};
    return r;
  }
  return fmt_run(ctx->lang, ctx->text, ctx->text_len, ctx->filepath);
}

/* =====================================================================
 * Stage 3: Rebuild syntax structures
 * The pipeline itself doesn't hold a syntax tree; it signals the
 * caller (editor) that the document changed so xcaches are dirtied.
 * Nothing to do here at the pipeline level — handled by caller after
 * pipeline_run returns.
 * ===================================================================== */

/* =====================================================================
 * Stage 4: Diagnostics
 * For now: report formatter outcome. Future: linter integration.
 * ===================================================================== */
static void stage_diagnostics(const PipelineCtx *ctx, const FormatResult *fr,
                              PipelineResult *out) {
  (void)ctx;
  /* Propagate formatter diagnostic into pipeline result diag */
  if (fr->diag[0])
    snprintf(out->diag, sizeof(out->diag), "%s", fr->diag);
}

/* =====================================================================
 * Stage 5: Persist to disk
 * ===================================================================== */
static bool stage_persist(const PipelineCtx *ctx, const char *text,
                          size_t text_len, char *diag, size_t diag_sz) {
  ByteBuffer buf;
  bytebuffer_init(&buf);
  if (!bytebuffer_append(&buf, text, text_len)) {
    bytebuffer_free(&buf);
    snprintf(diag, diag_sz, "out of memory building write buffer");
    return false;
  }

  StorageStatus st = storage_save(ctx->storage, ctx->filepath, &buf, ctx->meta);
  bytebuffer_free(&buf);

  if (st != STORAGE_OK) {
    snprintf(diag, diag_sz, "storage error: %s", storage_status_string(st));
    return false;
  }
  return true;
}

/* =====================================================================
 * Public API
 * ===================================================================== */
PipelineResult pipeline_run(const PipelineCtx *ctx, char **out_text,
                            size_t *out_len) {
  PipelineResult result = {PIPE_OK, FMT_UNAVAILABLE, "", false};
  *out_text = NULL;
  *out_len = 0;

  /* Stage 1: Validate */
  if (!stage_validate(ctx, result.diag, sizeof(result.diag))) {
    result.status = PIPE_ERR_VALIDATE;
    return result;
  }

  /* Stage 2: Format */
  FormatResult fr = stage_format(ctx);
  result.fmt_status = fr.status;

  const char *write_text = ctx->text;
  size_t write_len = ctx->text_len;
  char *fmt_buf = NULL;

  switch (fr.status) {
  case FMT_OK:
    /* Formatter produced new text — use it */
    write_text = fr.output;
    write_len = fr.output_len;
    fmt_buf = fr.output; /* will free after persist */
    result.formatted = true;
    break;

  case FMT_UNCHANGED:
    /* Formatter ran but no change — save original */
    result.status = PIPE_OK_FORMAT_UNCHANGED;
    fmt_result_free(&fr);
    break;

  case FMT_UNAVAILABLE:
  case FMT_NOT_INSTALLED:
    result.status = PIPE_OK_FORMAT_SKIPPED;
    break;

  case FMT_ERROR:
  case FMT_TIMEOUT:
  case FMT_INTERNAL_ERROR:
    /* Formatter failed — save proceeds with original, warn user */
    result.status = PIPE_OK_FORMAT_FAILED;
    /* fr.diag is already populated */
    break;
  }

  /* Stage 3: (syntax rebuild handled by caller post-return) */

  /* Stage 4: Diagnostics */
  stage_diagnostics(ctx, &fr, &result);

  /* Stage 5: Persist */
  char persist_diag[256] = "";
  if (!stage_persist(ctx, write_text, write_len, persist_diag,
                     sizeof(persist_diag))) {
    free(fmt_buf);
    result.status = PIPE_ERR_STORAGE;
    snprintf(result.diag, sizeof(result.diag), "%s", persist_diag);
    return result;
  }

  /* Stage 6: Hand back final text to caller for state refresh */
  if (result.formatted && fmt_buf) {
    *out_text = fmt_buf; /* ownership transferred to caller */
    *out_len = write_len;
  } else {
    free(fmt_buf);
  }

  if (result.status == PIPE_OK && !result.diag[0])
    snprintf(result.diag, sizeof(result.diag), "saved");

  return result;
}

const char *pipeline_status_string(PipelineStatus s) {
  switch (s) {
  case PIPE_OK:
    return "OK";
  case PIPE_OK_FORMAT_SKIPPED:
    return "saved (no formatter)";
  case PIPE_OK_FORMAT_FAILED:
    return "saved (formatter error)";
  case PIPE_OK_FORMAT_UNCHANGED:
    return "saved (formatter: no changes)";
  case PIPE_ERR_VALIDATE:
    return "save aborted: validation failed";
  case PIPE_ERR_STORAGE:
    return "save failed: disk error";
  }
  return "unknown";
}
