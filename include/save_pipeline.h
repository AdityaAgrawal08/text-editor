#ifndef SAVE_PIPELINE_H
#define SAVE_PIPELINE_H

/*
 * SavePipeline
 *
 * Executes all pre-write stages before any file I/O occurs.
 * Plugs into editor_do_save(); autosave goes through the same path.
 *
 * Stage 1: Validate + normalize internal state
 * Stage 2: Language-aware formatting (external formatter)
 * Stage 3: Rebuild syntax structures (invalidate token cache)
 * Stage 4: Diagnostics collection
 * Stage 5: Persist to disk (delegates to StorageSession)
 * Stage 6: Refresh editor state post-save
 *
 * Formatting failures never block the save:
 *   - FMT_UNAVAILABLE / FMT_NOT_INSTALLED: save proceeds with original text.
 *   - FMT_ERROR / FMT_TIMEOUT: save proceeds with original text, diag shown.
 *   The caller can override with PIPE_FLAG_SKIP_FORMAT.
 */

#include "formatter.h"
#include "language.h"
#include "storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Flags controlling pipeline behaviour */
#define PIPE_FLAG_NONE 0x00
#define PIPE_FLAG_SKIP_FORMAT 0x01 /* force-save without formatting */
#define PIPE_FLAG_AUTOSAVE 0x02    /* autosave path (no user confirmation) */

typedef enum {
  PIPE_OK = 0,
  PIPE_OK_FORMAT_SKIPPED,   /* save succeeded, formatter unavailable */
  PIPE_OK_FORMAT_FAILED,    /* save succeeded, formatter errored */
  PIPE_OK_FORMAT_UNCHANGED, /* save succeeded, formatter found no changes */
  PIPE_ERR_VALIDATE,        /* internal state invalid; save aborted */
  PIPE_ERR_STORAGE,         /* disk write failed */
} PipelineStatus;

typedef struct {
  PipelineStatus status;
  FormatStatus fmt_status;
  char diag[512]; /* human-readable summary */
  bool formatted; /* true if document was modified by formatter */
} PipelineResult;

/*
 * Opaque context passed to the pipeline.
 * The pipeline does NOT depend on the Editor struct; it only works with
 * flat byte buffers, the storage session, and language metadata.
 * This keeps it decoupled and testable.
 */
typedef struct {
  /* Input: serialised document (UTF-8, no trailing NUL) */
  const char *text;
  size_t text_len;

  /* Metadata */
  LangID lang;
  const char *filepath; /* for formatter file-path hints */
  StorageSession *storage;
  StorageMetadata *meta;

  /* Flags */
  uint32_t flags;
} PipelineCtx;

/*
 * Run the pipeline.
 * On PIPE_OK*: result.formatted indicates whether the formatted text
 *              differs from the input; the formatted bytes are in
 *              out_text/out_len (caller must free out_text).
 * On PIPE_ERR_*: out_text is NULL; caller should keep original.
 */
PipelineResult pipeline_run(const PipelineCtx *ctx, char **out_text,
                            size_t *out_len);

const char *pipeline_status_string(PipelineStatus s);

#endif /* SAVE_PIPELINE_H */
