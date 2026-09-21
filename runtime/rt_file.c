/* rt_file.c — PL/I runtime library (libpli): named files + SEQUENTIAL RECORD. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"
#include <stdio.h>
#include <string.h>

void pli_string_put_open(char *buf, long long cap) {
  rt_out_buf = buf;
  rt_out_cap = (size_t)cap;
  rt_out_len = 0;
  rt_items_on_line = 0;
}

/* End a STRING PUT: blank-pad the unused tail, then return to SYSPRINT. */
void pli_string_put_close(char *buf, long long cap) {
  (void)buf;
  if (rt_out_buf && rt_out_len < (size_t)cap)
    memset(rt_out_buf + rt_out_len, ' ', (size_t)cap - rt_out_len);
  rt_out_buf = NULL;
  rt_out_cap = rt_out_len = 0;
}

/* STRING (rule 105) GET: read list-directed input from buf (len bytes). */
void pli_string_get_open(char *buf, long long len) {
  rt_in_buf = buf;
  rt_in_len = (size_t)len;
  rt_in_pos = 0;
}

/* End a STRING GET: return to SYSIN. */
void pli_string_get_close(void) {
  rt_in_buf = NULL;
  rt_in_len = rt_in_pos = 0;
}

/* OPEN (rules 100-103): open the file in `slot` with the given name (namelen
 * bytes, not nul-terminated). mode 0 = INPUT, 1 = OUTPUT. The FILE variable's
 * slot is a compile-time constant, so rt_pli_files[slot] is stable across the
 * OPEN/GET/PUT/CLOSE statements that name it. */
void pli_file_open(long long slot, const char *name, long long namelen, long long mode) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  memcpy(buf, name, n);
  buf[n] = '\0';
  if (rt_pli_files[slot])
    fclose(rt_pli_files[slot]);
  rt_pli_files[slot] = fopen(buf, mode ? "w" : "r");
}

/* CLOSE (rule 102): close the file in `slot` and release the slot. */
void pli_file_close(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  if (rt_pli_files[slot]) {
    fclose(rt_pli_files[slot]);
    rt_pli_files[slot] = NULL;
  }
}

/* FILE ( f ) output routing (rule 105): rt_put_raw writes to the named file until
 * the matching unselect returns to SYSPRINT. */
void pli_put_select(long long slot) {
  rt_out_f = (slot >= 0 && slot < PLI_MAX_FILES) ? rt_pli_files[slot] : NULL;
  rt_col = 0;
  rt_items_on_line = 0;
}

void pli_put_unselect(void) {
  rt_out_f = NULL;
}

/* FILE ( f ) input routing (rule 105): rt_next_char reads from the named file. */
void pli_get_select(long long slot) {
  rt_in_f = (slot >= 0 && slot < PLI_MAX_FILES) ? rt_pli_files[slot] : NULL;
}

void pli_get_unselect(void) {
  rt_in_f = NULL;
}

/* SEQUENTIAL RECORD files (rules (112),(113), ADR-101): fixed-size binary
 * records on the same slot table as stream files. Each WRITE appends one
 * record, each READ consumes one; the byte size comes from the caller's
 * type (FIXED 8, FLOAT 8, BIT 1, CHAR n). A use of a closed slot, a failed
 * transfer, or a short READ (EOF) raises ERROR, since ON ENDFILE stays
 * diagnosed. Images are host byte order (implementation-defined). */
void pli_file_open_record(long long slot, const char *name, long long namelen,
                          long long mode) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  memcpy(buf, name, n);
  buf[n] = '\0';
  if (rt_pli_files[slot])
    fclose(rt_pli_files[slot]);
  rt_pli_files[slot] = fopen(buf, mode ? "wb" : "rb");
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
  if (fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_float(long long slot, double v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f)
    return;
  if (fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f || len <= 0)
    return;
  if (fwrite(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("WRITE to record file failed");
}

void pli_record_write_bit(long long slot, unsigned char v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f)
    return;
  if (fwrite(&v, sizeof v, 1, f) != 1)
    pli_signal_error("WRITE to record file failed");
}

long long pli_record_read_fixed(long long slot) {
  FILE *f = rec_file(slot, "READ");
  long long v = 0;
  if (!f)
    return 0;
  if (fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
}

double pli_record_read_float(long long slot) {
  FILE *f = rec_file(slot, "READ");
  double v = 0;
  if (!f)
    return 0;
  if (fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
}

void pli_record_read_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "READ");
  if (!f || len <= 0)
    return;
  if (fread(p, 1, (size_t)len, f) != (size_t)len)
    pli_signal_error("ENDFILE on record file: no more records");
}

unsigned char pli_record_read_bit(long long slot) {
  FILE *f = rec_file(slot, "READ");
  unsigned char v = 0;
  if (!f)
    return 0;
  if (fread(&v, sizeof v, 1, f) != 1)
    pli_signal_error("ENDFILE on record file: no more records");
  return v;
}

/* Edit-directed output (rules (108),(44)-(54)). All output routes through
 * rt_put_raw so STRING/FILE sources and sinks are honoured. */

/* Right-justify a formatted string into a field of width w: pad on the left
 * with blanks; if the content is wider than w, write it in full. */

