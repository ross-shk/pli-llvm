/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_file.c — PL/I runtime library (pli_runtime): named files + SEQUENTIAL RECORD. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
/* Record/stream files route through pli_plat_fopen/pli_plat_fclose/
 * pli_plat_fread/pli_plat_fwrite (the platform seam). */

void pli_string_put_open(char *buf, long long cap) {
  rt_out_buf = buf;
  rt_out_cap = (size_t)cap;
  rt_out_len = 0;
  rt_items_on_line = 0;
}

/* End a STRING PUT: blank-pad the unused tail, then return to SYSPRINT.
 * Returns the number of characters written. */
long long pli_string_put_close(char *buf, long long cap) {
  (void)buf;
  size_t len = rt_out_len;
  if (rt_out_buf && rt_out_len < (size_t)cap)
    pli_memset(rt_out_buf + rt_out_len, ' ', (size_t)cap - rt_out_len);
  rt_out_buf = NULL;
  rt_out_cap = rt_out_len = 0;
  return (long long)len;
}

/* STRING (rule 105) GET: read list-directed input from buf (len bytes). */
void pli_string_get_open(char *buf, long long len) {
  rt_in_buf = buf;
  rt_in_len = (size_t)len;
  rt_in_pos = 0;
  rt_in_col = 0;
}

/* End a STRING GET: return to SYSIN. */
void pli_string_get_close(void) {
  rt_in_buf = NULL;
  rt_in_len = rt_in_pos = 0;
  rt_in_col = 0;
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
  if (rt_pli_files[slot])
    pli_plat_fclose(rt_pli_files[slot]);
  if (nonprint) {
    rt_pli_files[slot] = NULL;
    rt_pli_nonprint[slot] = 1;
  } else {
    rt_pli_files[slot] = pli_plat_fopen(buf, mode ? "w" : "r");
    rt_pli_nonprint[slot] = 0;
    /* A failed OPEN must not be silent: the slot stays NULL, and the output
     * routing would fall back to SYSPRINT, sending the file's data to the
     * console instead of the named file. */
    if (!rt_pli_files[slot])
      pli_signal_error("OPEN failed: the file could not be opened");
  }
}

/* CLOSE (rule 102): close the file in `slot` and release the slot. */
void pli_file_close(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  if (rt_pli_files[slot]) {
    pli_plat_fclose(rt_pli_files[slot]);
    rt_pli_files[slot] = NULL;
  }
}

/* FILE ( f ) output routing (rule 105): rt_put_raw writes to the named file until
 * the matching unselect returns to SYSPRINT. */
void pli_put_select(long long slot) {
  /* ADR-220: drain any buffered stdout before routing to a named file so
   * output ordering stays correct. */
  rt_flush_stdout();
  if (slot >= 0 && slot < PLI_MAX_FILES && !rt_pli_nonprint[slot])
    rt_out_f = rt_pli_files[slot];
  else
    rt_out_f = NULL;  /* NONPRINT: discard output (ADR-204) */
  rt_out_discard = (slot >= 0 && slot < PLI_MAX_FILES && rt_pli_nonprint[slot]);
  rt_col = 0;
  rt_items_on_line = 0;
}

void pli_put_unselect(void) {
  rt_out_f = NULL;
  rt_out_discard = 0;
}

/* FILE ( f ) input routing (rule 105): rt_next_char reads from the named file. */
void pli_get_select(long long slot) {
  rt_in_f = (slot >= 0 && slot < PLI_MAX_FILES) ? rt_pli_files[slot] : NULL;
  rt_in_col = 0;
}

void pli_get_unselect(void) {
  rt_in_f = NULL;
  rt_in_col = 0;
}

/* SEQUENTIAL RECORD files (rules (112),(113), ADR-101): fixed-size binary
 * records on the same slot table as stream files. Each WRITE appends one
 * record, each READ consumes one; the byte size comes from the caller's
 * type (FIXED 8, FLOAT 8, BIT 1, CHAR n). A use of a closed slot, a failed
 * transfer, or a short READ (EOF) raises ERROR, since ON ENDFILE stays
 * diagnosed. Images are host byte order (implementation-defined).
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
  if (rt_pli_files[slot])
    pli_plat_fclose(rt_pli_files[slot]);
  if (nonprint) {
    rt_pli_files[slot] = NULL;
    rt_pli_nonprint[slot] = 1;
    return;
  }
  rt_pli_nonprint[slot] = 0;
  rt_pli_files[slot] = pli_plat_fopen(buf, mode ? "wb" : "rb");
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
  if (!f)
    return;
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_float(long long slot, double v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f)
    return;
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f || len <= 0)
    return;
  if (pli_plat_fwrite(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_bit(long long slot, unsigned char v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f)
    return;
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

long long pli_record_read_fixed(long long slot) {
  FILE *f = rec_file(slot, "READ");
  long long v = 0;
  if (!f)
    return 0;
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
}

double pli_record_read_float(long long slot) {
  FILE *f = rec_file(slot, "READ");
  double v = 0;
  if (!f)
    return 0;
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
}

void pli_record_read_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "READ");
  if (!f || len <= 0)
    return;
  if (pli_plat_fread(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("ENDFILE on record file: no more records");
}

unsigned char pli_record_read_bit(long long slot) {
  FILE *f = rec_file(slot, "READ");
  unsigned char v = 0;
  if (!f)
    return 0;
  if (pli_plat_fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
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

