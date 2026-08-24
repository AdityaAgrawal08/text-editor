# The EDOC Document Container Specification

*Version 1 — August 2026*
*Reference implementation: `src/storage.c` · Toolkit: `build/edoc`*

## 1. Status & scope

EDOC (Embedded DOCument container) is the on-disk format of the
Palimpsest editor. It packages a UTF-8 text document together with
metadata, an embedded version history, and integrity checksums into a
single crash-safe file. This document specifies the byte layout, the
integrity model, and the recovery contract for format version **1**.

The companion toolkit (`edoc`) can verify, inspect, and convert these
files without the editor.

## 2. Conventions

- All integers are **little-endian**, regardless of host byte order.
- "CRC" means CRC-32 (IEEE 802.3 reflected, poly `0xEDB88320`,
  init/final-XOR `0xFFFFFFFF`).
- Offsets are absolute byte positions from file start unless stated.
- Strings are raw bytes; lengths are explicit and never NUL-terminated.

## 3. Container layout

```
offset  size  region
0       16    FileHeader
16      ...   SectionHeader + payload   (repeated, back to back)
N-8     4     section_count   ┐ FileFooter
N-4     4     footer_crc32    ┘
```

There is no alignment padding anywhere: every field immediately follows
the previous one.

### 3.1 FileHeader

| offset | size | field         | notes                                   |
|-------:|-----:|---------------|-----------------------------------------|
| 0      | 4    | magic         | `"EDOC"` = `0x434F4445` little-endian   |
| 4      | 4    | format_version| must equal `1`; readers reject larger   |
| 8      | 8    | created_at    | unix seconds when file was first written |

### 3.2 SectionHeader

| offset | size | field       |
|-------:|-----:|-------------|
| +0     | 4    | type        |
| +4     | 8    | payload_len |
| +12    | 4    | payload_crc |

Defined types:

| value | name     | payload meaning                       |
|------:|----------|----------------------------------------|
| 1     | DOCUMENT | the document body, plain UTF-8         |
| 2     | METADATA | key fields, fixed layout (see 3.3)     |
| 3     | JOURNAL  | reserved in-file WAL tail (unused v1)  |
| 4     | VERSIONS | zero or more version records (see 3.4) |

Unknown types are **skipped, never rejected** — forward compatibility:
older readers open newer files that carry extra sections.

Exactly one DOCUMENT section is required. METADATA is optional-in-
theory but always written by v1 implementations. Readers must treat
absence of either as a structural failure (`TRUNCATED`), matching the
reference parser.

### 3.3 METADATA payload

```
u32 title_len   u8 title[title_len]
u32 author_len  u8 author[author_len]
u64 created_at
u64 modified_at
u32 schema_version          (document-model schema, independent of
                             the container's format_version)
u64 revision_id             (history-system commit id, 0 if unused)
```

Title/author are capped at 255/127 bytes by the reference writer.

### 3.4 VERSIONS records

Each record is self-contained:

```
u32  magic "VER1" (= 0x31524556 LE)
u64  id            monotonic per file; auto-names are "v<id>"
u64  created_at    unix seconds of the snapshot
u32  name_len      ≤ 63
u8   name[name_len] user-visible label ("v1", "before-refactor", …)
u64  doc_len
u32  doc_crc       redundancy: covered again by rec_crc below
u8   doc[doc_len]  full document snapshot
u32  rec_crc       CRC over everything from magic through doc[]
```

Records appear oldest-first. Any malformed record stops parsing at that
point; earlier valid records remain usable. A missing or empty VERSIONS
section means "no history yet" — legacy files open normally.

### 3.5 FileFooter

| offset | size | field         |
|-------:|-----:|---------------|
| N-8    | 4    | section_count |
| N-4    | 4    | footer_crc32  |

`section_count` is the number of sections the writer emitted. Readers
must not trust it blindly: they walk only while structure allows and
compare walked-vs-claimed afterwards.

## 4. CRC coverage map

| checksum      | protects                                             |
|---------------|-------------------------------------------------------|
| payload_crc   | one section's payload bytes                           |
| footer_crc32  | **everything except the footer itself** (header + all sections incl. their headers) |
| VER1 rec_crc  | one version record from magic through doc[]           |
| journal rec_crc | one journal record body (see §6)                    |

Design rule: *no checksum ever covers another checksum.* This keeps each
verifier independent and makes partial-recovery decisions local.

## 5. Atomicity contract

Writers never modify a live file in place:

1. Write full image to `<path>.tmp.<pid>.<salt>` in the same directory.
2. `fsync()` the temp file; `close()`.
3. `rename(temp, path)` — atomic on POSIX same-filesystem.
4. Best-effort `fsync()` of the containing directory so the rename
   itself survives power loss.

A crash mid-write can leave only an orphaned temp file; readers never
observe a torn main file. Stale temps from dead processes (≥ 1 hour old,
different pid) may be swept by editors at session start.

Reader obligations: verify footer CRC before trusting any section;
verify per-section CRC before using payloads; treat any mismatch as
corruption, never attempt repair.

## 6. Journal sidecar (`.journal` WAL)

Between explicit saves, editors append full-snapshot records to
`<path>.journal` so a crash loses at most the debounce window (~800 ms).

Record framing on disk:

```
u64 rec_total  ─ leading length prefix
body           = u64 ts ‖ u32 op_len ‖ op_desc ‖ u64 doc_len ‖ doc_bytes
u32 body_crc   ─ CRC over body
u64 rec_total  ─ trailing length prefix (repeated)
```

Recovery scans **backward**: read trailing length → jump to candidate
start → check leading length matches → verify body CRC → accept or step
back and retry. Torn final writes (crash mid-append) therefore lose only
that record. Each record is self-sufficient; there is no replay logic.

The journal is truncated after any successful explicit save or autosave,
and removed when the user explicitly discards unsaved work.

## 7. Sibling files

| path                  | purpose                                  |
|-----------------------|-------------------------------------------|
| `<file>.journal`      | pending-state WAL (§6)                    |
| `<file>.autosave`     | periodic EDOC image, **never** promoted automatically; carries no VERSIONS section |
| `<file>.bak.0..4`     | five most recent pre-save images, newest first |

## 8. Schema versioning & migration policy

- Container evolution bumps `format_version`. Version-1 readers reject
  files claiming a larger version with `UNSUPPORTED_VERSION` — they
  never guess.
- Additive changes (new optional section types) do **not** bump the
  version; unknown sections are skipped.
- Destructive changes require a new version plus a migration tool
  shipped alongside the old reader for one release cycle.
- `schema_version` inside METADATA tracks the document-model schema
  independently, so body-format churn does not force container bumps.

## 9. Worked example

`printf 'Hi' > hello.txt && edoc import hello.txt example.edoc`
produces this 163-byte image (excerpt through the document payload
shown; timestamps vary between runs):

```
00000000  45 44 4f 43 01 00 00 00  ed 11 8c 6a 00 00 00 00  EDOC.......j....
00000010  01 00 00 00 02 00 00 00  00 00 00 00 0e 0e 17 4d  ...............M
00000020  48 69                                     Hi
```

Annotated walk:

| offset | bytes                | meaning                              |
|-------:|----------------------|---------------------------------------|
| 0      | `45 44 4f 43`        | magic "EDOC"                          |
| 4      | `01 00 00 00`        | format_version = 1                    |
| 8      | `ed 11 8c 6a …`      | created_at                            |
| 16     | `01 00 00 00`        | section type = DOCUMENT               |
| 20     | `02 00 …`            | payload_len = 2                       |
| 28     | `0e 0e 17 4d`        | payload_crc("Hi")                     |
| 32     | `48 69`              | payload = "Hi"                        |
| 34     | `02 00 00 00`        | type = METADATA …                     |
| 50     | 45 bytes             | title "hello.txt", timestamps, ids    |
| 95     | `04 00 00 00`        | type = VERSIONS                       |
| 111    | 44 bytes             | one VER1 record snapshotting "Hi"     |
| 155    | `03 00 00 00`        | footer: 3 sections claimed            |
| 159    | `d9 0f c3 30`        | footer CRC over bytes [0,155)         |

Cross-check with the toolkit:

```
$ edoc dump example.edoc
  [0] DOCUMENT  off=32       len=2        crc=ok
  [1] METADATA  off=50       len=45       crc=ok
  [2] VERSIONS  off=111      len=44       crc=ok
```

## 10. Toolkit appendix

```
edoc verify FILE                  exit 0 clean / 2 corrupt / 3 io-format
edoc dump FILE                    section table + summary
edoc history FILE                 newest-first version list
edoc export FILE [-v ID|--name X] OUT
edoc import TXT FILE [--force]
edoc recover FILE                 report-only; never mutates anything
```

## 11. Security posture

All parsers consume untrusted bytes with subtraction-based bounds
checks (immune to integer-wraparound length fields). The three parsing
entry points are continuously fuzzed under ASan/UBSan — see
`Implementation.md` for campaign evidence and locked regression cases,
including a crafted length-wrap record that previously caused an OOB
read and now serves as a permanent test fixture.
