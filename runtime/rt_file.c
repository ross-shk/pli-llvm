/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_file.c — PL/I runtime library (pli_runtime): named files + SEQUENTIAL RECORD. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* Record/stream files route through pli_plat_fopen/pli_plat_fclose/
 * pli_plat_fread/pli_plat_fwrite (the platform seam). */
/* Defined with the record state below; pli_file_close resets it too. */
static void rt_rec_reset(long long slot);

void pli_string_put_open(char *buf, long long cap) {
  rt_out_lock();
  rt_out_buf = buf;
  rt_out_cap = (size_t)cap;
  rt_out_len = 0;
  rt_items_on_line = 0;
  rt_out_unlock();
}

/* End a STRING PUT: blank-pad the unused tail, then return to SYSPRINT.
 * Returns the number of characters written. */
long long pli_string_put_close(char *buf, long long cap) {
  (void)buf;
  rt_out_lock();
  size_t len = rt_out_len;
  if (rt_out_buf && rt_out_len < (size_t)cap)
    pli_memset(rt_out_buf + rt_out_len, ' ', (size_t)cap - rt_out_len);
  rt_out_buf = NULL;
  rt_out_cap = rt_out_len = 0;
  rt_out_unlock();
  return (long long)len;
}

/* STRING (rule 105) GET: read list-directed input from buf (len bytes). */
void pli_string_get_open(char *buf, long long len) {
  rt_in_lock();
  rt_in_buf = buf;
  rt_in_len = (size_t)len;
  rt_in_pos = 0;
  rt_in_col = 0;
  rt_in_unlock();
}

/* End a STRING GET: return to SYSIN. */
void pli_string_get_close(void) {
  rt_in_lock();
  rt_in_buf = NULL;
  rt_in_len = rt_in_pos = 0;
  rt_in_col = 0;
  rt_in_unlock();
}

/* Mirrors Stmt::kUndefinedfileBaseCondKey in src/ast.h: ON UNDEFINEDFILE(f) is
 * keyed by the FILE variable's runtime slot, like ON ENDFILE(f). */
#define RT_UNDEFINEDFILE_KEY_BASE (-2000LL)

/* UNDEFINEDFILE (rules (97),(100)): an OPEN could not open its file. Run the
 * ON-unit established for this file; with no unit the condition's implicit
 * action is ERROR, as before it was served. A resume continues after the OPEN
 * and leaves the file closed, so later I/O on it raises ERROR. */
static void rt_undef_file(long long slot) {
  char *ctx = NULL;
  char *fn = pli_on_top_fn_pair(RT_UNDEFINEDFILE_KEY_BASE - slot, (char *)&ctx);
  if (!fn) {
    pli_signal_error("OPEN failed: the file could not be opened");
    return;
  }
  /* IRGen's handler takes its capture context as the only argument; the
   * pointer round-trip through a union avoids an object-to-function cast. */
  union {
    char *obj;
    void (*fn)(void *);
  } u;
  u.obj = fn;
  u.fn(ctx);
}

/* OPEN (rules 100-103): open the file in `slot` with the given name (namelen
 * bytes, not nul-terminated). mode 0 = INPUT, 1 = OUTPUT. The FILE variable's
 * slot is a compile-time constant, so rt_pli_files[slot] is stable across the
 * OPEN/GET/PUT/CLOSE statements that name it. nonprint=1 (ADR-204): output
 * to this file is silently discarded. */
void pli_file_open(long long slot, const char *name, long long namelen, long long mode,
                   long long nonprint) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  /* A negative name length is a caller bug: the size_t cast would ask for a
   * huge copy and overrun the buffer. */
  if (namelen < 0) namelen = 0;
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  pli_memcpy(buf, name, n);
  buf[n] = '\0';
  if (rt_pli_files[slot]) {
    rt_file_out_detach(rt_pli_files[slot]);
    pli_plat_fclose(rt_pli_files[slot]);
  }
  if (nonprint) {
    rt_pli_files[slot] = NULL;
    rt_pli_nonprint[slot] = 1;
  } else {
    rt_pli_files[slot] = pli_plat_fopen(buf, mode ? "w" : "r");
    rt_pli_nonprint[slot] = 0;
    /* A failed OPEN must not be silent: it raises UNDEFINEDFILE (rule 97)
     * against this file, the slot stays NULL, and later I/O on the slot
     * raises ERROR instead of falling back to the console. */
    if (!rt_pli_files[slot])
      rt_undef_file(slot);
  }
}

/* CLOSE (rule 102): close the file in `slot` and release the slot. */
void pli_file_close(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  rt_rec_reset(slot);
  if (rt_pli_files[slot]) {
    rt_file_out_detach(rt_pli_files[slot]);
    pli_plat_fclose(rt_pli_files[slot]);
    rt_pli_files[slot] = NULL;
  }
}

/* FILE ( f ) output routing (rule 105): rt_put_raw writes to the named file until
 * the matching unselect returns to SYSPRINT. */
void pli_put_select(long long slot) {
  int open;
  /* ADR-220: drain any buffered stdout before routing to a named file so
   * output ordering stays correct. */
  rt_flush_stdout();
  rt_out_lock();
  open = slot >= 0 && slot < PLI_MAX_FILES &&
         (rt_pli_files[slot] || rt_pli_nonprint[slot]);
  if (!open) {
    /* A CLOSE, or a failed OPEN whose UNDEFINEDFILE handler resumed: sending
     * the file's data to the console instead would be a silent wrong answer.
     * Discard what a resuming ERROR handler lets through. */
    rt_out_f = NULL;
    rt_out_discard = 1;
    rt_out_unlock();
    pli_signal_error("PUT to a file that is not open");
    return;
  }
  if (rt_pli_nonprint[slot]) {
    rt_out_f = NULL;  /* NONPRINT: discard output (ADR-204) */
    rt_out_discard = 1;
  } else {
    rt_out_f = rt_pli_files[slot];
    rt_out_discard = 0;
  }
  rt_col = 0;
  rt_items_on_line = 0;
  rt_out_unlock();
}

void pli_put_unselect(void) {
  rt_out_lock();
  rt_out_f = NULL;
  rt_out_discard = 0;
  rt_out_unlock();
}

/* FILE ( f ) input routing (rule 105): rt_next_char reads from the named file. */
void pli_get_select(long long slot) {
  int open;
  rt_in_lock();
  open = slot >= 0 && slot < PLI_MAX_FILES && rt_pli_files[slot] != NULL;
  if (!open) {
    /* Mirror the output path: reading SYSIN for a named file that is not open
     * would be a silent wrong answer. */
    rt_select_input(NULL);
    rt_in_col = 0;
    rt_in_unlock();
    pli_signal_error("GET from a file that is not open");
    return;
  }
  rt_select_input(rt_pli_files[slot]);
  rt_in_col = 0;
  rt_in_unlock();
}

void pli_get_unselect(void) {
  rt_in_lock();
  rt_select_input(NULL);
  rt_in_col = 0;
  rt_in_unlock();
}

/* The record most recently transferred on a sequential record slot: REWRITE
 * replaces it and DELETE removes it (rules (112),(113)). A successful READ or
 * WRITE makes its record current; a failed transfer (ENDFILE) clears it. */
struct rt_rec_pos {
  long long off;  /* first byte of the current record */
  long long size; /* its byte length */
  int valid;
};
static struct rt_rec_pos rt_rec[PLI_MAX_FILES];

/* Mirrors Stmt::kEndfileBaseCondKey in src/ast.h: ON ENDFILE(f) is keyed by
 * the FILE variable's runtime slot so each file has its own handler. */
#define RT_ENDFILE_KEY_BASE (-1000LL)

static void rt_rec_reset(long long slot) {
  if (slot >= 0 && slot < PLI_MAX_FILES)
    rt_rec[slot].valid = 0;
}

static void rt_rec_set(long long slot, long long off, long long size) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  rt_rec[slot].off = off;
  rt_rec[slot].size = size;
  rt_rec[slot].valid = 1;
}

/* ENDFILE (rules (97),(112)): a READ ran past the last record. Run the ON-unit
 * established for this file, then resume after the READ; with no unit the
 * condition is unhandled and raises ERROR, as before ENDFILE was served. */
static void rt_rec_endfile(long long slot) {
  rt_rec_reset(slot);
  char *ctx = NULL;
  char *fn = pli_on_top_fn_pair(RT_ENDFILE_KEY_BASE - slot, (char *)&ctx);
  if (!fn) {
    pli_signal_error("ENDFILE on record file: no more records");
    return;
  }
  /* IRGen's handler takes its capture context as the only argument; the
   * pointer round-trip through a union avoids an object-to-function cast. */
  union {
    char *obj;
    void (*fn)(void *);
  } u;
  u.obj = fn;
  u.fn(ctx);
}

/* Require a current record for REWRITE/DELETE; `want` < 0 accepts any size.
 * A size change would resize the record in a file whose layout is fixed by
 * the transfer type, so it is an ERROR. */
static int rt_rec_current(long long slot, long long want, long long *off, const char *what) {
  if (slot < 0 || slot >= PLI_MAX_FILES || !rt_rec[slot].valid) {
    pli_signal_error(what);
    return 0;
  }
  if (want >= 0 && rt_rec[slot].size != want) {
    pli_signal_error("REWRITE changes the record size");
    return 0;
  }
  *off = rt_rec[slot].off;
  return 1;
}

/* SEQUENTIAL RECORD files (rules (112),(113), ADR-101): fixed-size binary
 * records on the same slot table as stream files. Each WRITE appends one
 * record, each READ consumes one; the byte size comes from the caller's
 * type (FIXED 8, FLOAT 8, BIT 1, CHAR n). A short READ (EOF) raises ENDFILE:
 * the ON-unit for that file runs and the READ resumes, or the condition is
 * unhandled and raises ERROR (rule (97)). An UPDATE open permits REWRITE of
 * the current record and DELETE of it in place. Images are host byte order
 * (implementation-defined).
 * nonprint=1 (ADR-204): like pli_file_open, output is silently discarded. */
void pli_file_open_record(long long slot, const char *name, long long namelen,
                          long long mode, long long nonprint) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  /* A negative name length is a caller bug: the size_t cast would ask for a
   * huge copy and overrun the buffer. */
  if (namelen < 0) namelen = 0;
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  pli_memcpy(buf, name, n);
  buf[n] = '\0';
  rt_rec_reset(slot);
  if (rt_pli_files[slot]) {
    rt_file_out_detach(rt_pli_files[slot]);
    pli_plat_fclose(rt_pli_files[slot]);
  }
  if (nonprint) {
    rt_pli_files[slot] = NULL;
    rt_pli_nonprint[slot] = 1;
    return;
  }
  rt_pli_nonprint[slot] = 0;
  /* mode 0 = INPUT, 1 = OUTPUT, 2 = UPDATE (read and write in place). */
  rt_pli_files[slot] = pli_plat_fopen(buf, mode == 2 ? "r+b" : (mode ? "wb" : "rb"));
  if (!rt_pli_files[slot])
    rt_undef_file(slot);
}

static FILE *rec_file(long long slot, const char *what) {
  if (slot < 0 || slot >= PLI_MAX_FILES || !rt_pli_files[slot]) {
    pli_signal_error("record file is not open");
    return NULL;
  }
  (void)what;
  return rt_pli_files[slot];
}

void pli_record_write_fixed(long long slot, long long v) {
  FILE *f = rec_file(slot, "WRITE");
  long long off;
  if (!f)
    return;
  off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
  rt_rec_set(slot, off, (long long)sizeof v);
}

void pli_record_write_float(long long slot, double v) {
  FILE *f = rec_file(slot, "WRITE");
  long long off;
  if (!f)
    return;
  off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
  rt_rec_set(slot, off, (long long)sizeof v);
}

void pli_record_write_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "WRITE");
  long long off;
  if (!f || len <= 0)
    return;
  off = pli_plat_ftell(f);
  if (pli_plat_fwrite(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("WRITE to record file failed");
  rt_rec_set(slot, off, len);
}

void pli_record_write_bit(long long slot, unsigned char v) {
  FILE *f = rec_file(slot, "WRITE");
  long long off;
  if (!f)
    return;
  off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
  rt_rec_set(slot, off, (long long)sizeof v);
}

long long pli_record_read_fixed(long long slot) {
  FILE *f = rec_file(slot, "READ");
  long long v = 0;
  long long off;
  if (!f)
    return 0;
  off = pli_plat_ftell(f);
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1) {
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  return v;
}

double pli_record_read_float(long long slot) {
  FILE *f = rec_file(slot, "READ");
  double v = 0;
  long long off;
  if (!f)
    return 0;
  off = pli_plat_ftell(f);
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1) {
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  return v;
}

void pli_record_read_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "READ");
  long long off;
  if (!f || len <= 0)
    return;
  off = pli_plat_ftell(f);
  if (pli_plat_fread(p, 1, (size_t)len, f) != (size_t)len) {
    rt_rec_endfile(slot);
    return;
  }
  rt_rec_set(slot, off, len);
}

unsigned char pli_record_read_bit(long long slot) {
  FILE *f = rec_file(slot, "READ");
  unsigned char v = 0;
  long long off;
  if (!f)
    return 0;
  off = pli_plat_ftell(f);
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1) {
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  return v;
}

/* REWRITE (rule (112)): overwrite the current record in place. The transfer
 * type fixes the record size, so a different size is an ERROR (see
 * rt_rec_current). */
void pli_record_rewrite_fixed(long long slot, long long v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long off;
  if (!f || !rt_rec_current(slot, (long long)sizeof v, &off,
                            "REWRITE without a current record"))
    return;
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("REWRITE to record file failed");
  (void)pli_plat_fseek(f, off + (long long)sizeof v, SEEK_SET);
}

void pli_record_rewrite_float(long long slot, double v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long off;
  if (!f || !rt_rec_current(slot, (long long)sizeof v, &off,
                            "REWRITE without a current record"))
    return;
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("REWRITE to record file failed");
  (void)pli_plat_fseek(f, off + (long long)sizeof v, SEEK_SET);
}

void pli_record_rewrite_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "REWRITE");
  long long off;
  if (!f || len <= 0)
    return;
  if (!rt_rec_current(slot, len, &off, "REWRITE without a current record"))
    return;
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("REWRITE to record file failed");
  (void)pli_plat_fseek(f, off + len, SEEK_SET);
}

void pli_record_rewrite_bit(long long slot, unsigned char v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long off;
  if (!f || !rt_rec_current(slot, (long long)sizeof v, &off,
                            "REWRITE without a current record"))
    return;
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("REWRITE to record file failed");
  (void)pli_plat_fseek(f, off + (long long)sizeof v, SEEK_SET);
}

/* DELETE (rule (112)): remove the current record. The file is a plain byte
 * stream with no per-record marker, so the tail is copied down in chunks and
 * the file truncated; the next READ then sees the following record. */
void pli_record_delete(long long slot) {
  FILE *f = rec_file(slot, "DELETE");
  long long off, size, end, src, dst;
  char buf[8192];
  if (!f || !rt_rec_current(slot, -1, &off, "DELETE without a current record"))
    return;
  size = rt_rec[slot].size;
  if (pli_plat_fseek(f, 0, SEEK_END) != 0)
    pli_signal_error("DELETE from record file failed");
  end = pli_plat_ftell(f);
  src = off + size;
  dst = off;
  if (src > end)
    pli_signal_error("DELETE from record file failed");
  while (src < end) {
    long long n = end - src;
    if (n > (long long)sizeof buf)
      n = (long long)sizeof buf;
    if (pli_plat_fseek(f, src, SEEK_SET) != 0 ||
        pli_plat_fread(buf, 1, (size_t)n, f) != (size_t)n ||
        pli_plat_fseek(f, dst, SEEK_SET) != 0 ||
        pli_plat_fwrite(buf, 1, (size_t)n, f) != (size_t)n)
      pli_signal_error("DELETE from record file failed");
    src += n;
    dst += n;
  }
  if (pli_plat_ftruncate(f, dst) != 0)
    pli_signal_error("DELETE from record file failed");
  rt_rec_reset(slot);
  (void)pli_plat_fseek(f, off, SEEK_SET);
}

/* FILEOPEN(f) (TR 123): returns 1 if the file in slot f is open, 0 otherwise. */
int pli_file_open_slot(long long slot) {
  return slot >= 0 && slot < PLI_MAX_FILES && rt_pli_files[slot] != NULL;
}

/* LINENO(f) (TR 123): returns the current line counter for the file in slot f.
   M0 has no per-file line tracking; return 0. */
long long pli_lineno(long long slot) {
  (void)slot;
  return 0;
}

/* PAGENO(f) (TR 123): returns the current page number for the file in slot f. */
long long pli_pageno(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return 0;
  return rt_pli_pages[slot];
}

/* PAGENO(f) = n (TR 123, ADR-201): set the page counter for slot f. */
void pli_set_pageno(long long slot, long long val) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  rt_pli_pages[slot] = val;
}

/* Edit-directed output (rules (108),(44)-(54)). All output routes through
 * rt_put_raw so STRING/FILE sources and sinks are honoured. */

/* Right-justify a formatted string into a field of width w: pad on the left
 * with blanks; if the content is wider than w, write it in full. */

