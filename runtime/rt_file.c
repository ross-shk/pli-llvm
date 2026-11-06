/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_file.c — PL/I runtime library (pli_runtime): named files + SEQUENTIAL RECORD. */
/* Split from pli_rt.c; pli_rt.h + pli_rt_abi.def stay the single ABI source. */
#include "pli_rt.h"

int rt_pli_linesize[PLI_MAX_FILES] = {0};
int rt_pli_pagesize[PLI_MAX_FILES] = {0};
int rt_cur_file_slot = -1;
/* Record/stream files route through pli_plat_fopen/pli_plat_fclose/
 * pli_plat_fread/pli_plat_fwrite (the platform seam). */
/* Defined with the record state below; pli_file_close resets it too. */
static void rt_rec_reset(long long slot);
/* KEYED/DIRECT sidecar index helpers (defined after pli_file_open_record). */
static void kix_save(long long slot);
static void kix_reset(long long slot);

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

/* A10: FILE identity across separately compiled units. Each FILE variable has
 * a marker symbol in its unit; an EXTERNAL file's marker is a common symbol
 * the linker merges. The address is stable for the whole process, so the
 * runtime maps it to one slot on first use instead of trusting a per-unit
 * compile-time index (which made every unit start at slot 0). */
static struct {
  pli_mutex mu;
  pli_once once;
  const void *key[PLI_MAX_FILES];
  long long count;
} rt_file_reg;

static void rt_file_reg_init(void) {
  pli_mutex_init(&rt_file_reg.mu);
  rt_file_reg.count = 0;
}

long long pli_file_slot(const char *key) {
  pli_call_once(&rt_file_reg.once, rt_file_reg_init);
  pli_mutex_lock(&rt_file_reg.mu);
  for (long long i = 0; i < rt_file_reg.count; ++i) {
    if (rt_file_reg.key[i] == (const void *)key) {
      pli_mutex_unlock(&rt_file_reg.mu);
      return i;
    }
  }
  long long slot = rt_file_reg.count;
  if (slot < PLI_MAX_FILES) {
    rt_file_reg.key[slot] = (const void *)key;
    rt_file_reg.count = slot + 1;
  }
  pli_mutex_unlock(&rt_file_reg.mu);
  if (slot >= PLI_MAX_FILES) {
    pli_signal_error("too many FILE constants in one program");
    return -1;
  }
  return slot;
}

/* OPEN (rules 100-103): open the file in `slot` with the given name (namelen
 * bytes, not nul-terminated). mode 0 = INPUT, 1 = OUTPUT. The FILE variable's
 * slot comes from pli_file_slot, so rt_pli_files[slot] is stable across the
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

/* KEYED/DIRECT sidecar index state: key -> (offset, length) in the data file.
 * Loaded at open, held in memory, rewritten at close when dirty. */
struct kix_entry {
  char *key;       /* owned copy of keylen bytes */
  long long keylen;
  long long off;   /* offset of the record in the data file */
  long long len;   /* record byte length */
};
struct kix_state {
  struct kix_entry *ent;
  long long count;
  long long cap;
  int dirty;
  int is_keyed;    /* 1 when this slot was opened KEYED/DIRECT */
  char kixpath[512]; /* sidecar path, populated at open for save-on-close */
};
static struct kix_state rt_kix[PLI_MAX_FILES];

/* CLOSE (rule 102): close the file in `slot` and release the slot. */
void pli_file_close(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  rt_rec_reset(slot);
  if (rt_kix[slot].is_keyed)
    kix_save(slot);
  if (rt_pli_files[slot]) {
    rt_file_out_detach(rt_pli_files[slot]);
    pli_plat_fclose(rt_pli_files[slot]);
    rt_pli_files[slot] = NULL;
  }
  kix_reset(slot);
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
    rt_cur_file_slot = -1;
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
  rt_cur_file_slot = (int)slot;
  rt_out_unlock();
}

void pli_put_unselect(void) {
  rt_out_lock();
  rt_out_f = NULL;
  rt_out_discard = 0;
  rt_cur_file_slot = -1;
  rt_out_unlock();
}

void pli_file_set_linesize(long long slot, long long n) {
  if (slot >= 0 && slot < PLI_MAX_FILES && n > 0)
    rt_pli_linesize[slot] = (int)n;
}
void pli_file_set_pagesize(long long slot, long long n) {
  if (slot >= 0 && slot < PLI_MAX_FILES && n > 0)
    rt_pli_pagesize[slot] = (int)n;
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

static void kix_reset(long long slot) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  struct kix_state *st = &rt_kix[slot];
  for (long long i = 0; i < st->count; ++i)
    pli_plat_free(st->ent[i].key);
  pli_plat_free(st->ent);
  st->ent = NULL;
  st->count = st->cap = 0;
  st->dirty = 0;
  st->is_keyed = 0;
}

static long long kix_find(long long slot, char *key, long long keylen) {
  struct kix_state *st = &rt_kix[slot];
  for (long long i = 0; i < st->count; ++i)
    if (st->ent[i].keylen == keylen &&
        pli_memcmp(st->ent[i].key, key, (size_t)keylen) == 0)
      return i;
  return -1;
}

static void kix_insert(long long slot, char *key, long long keylen,
                       long long off, long long len) {
  struct kix_state *st = &rt_kix[slot];
  if (kix_find(slot, key, keylen) >= 0)
    return; /* duplicate: the caller signals ERROR before inserting */
  if (st->count == st->cap) {
    long long ncap = st->cap ? st->cap * 2 : 16;
    struct kix_entry *n = (struct kix_entry *)pli_plat_realloc(
        st->ent, (size_t)ncap * sizeof(struct kix_entry));
    if (!n) {
      pli_signal_error("keyed index out of memory");
      return;
    }
    st->ent = n;
    st->cap = ncap;
  }
  st->ent[st->count].key = (char *)pli_plat_malloc((size_t)keylen);
  if (!st->ent[st->count].key) {
    pli_signal_error("keyed index out of memory");
    return;
  }
  pli_memcpy(st->ent[st->count].key, key, (size_t)keylen);
  st->ent[st->count].keylen = keylen;
  st->ent[st->count].off = off;
  st->ent[st->count].len = len;
  st->count++;
  st->dirty = 1;
}

static void kix_remove(long long slot, long long idx) {
  struct kix_state *st = &rt_kix[slot];
  if (idx < 0 || idx >= st->count)
    return;
  pli_plat_free(st->ent[idx].key);
  for (long long i = idx + 1; i < st->count; ++i)
    st->ent[i - 1] = st->ent[i];
  st->count--;
  st->dirty = 1;
}

static int kix_cmp_off(const void *a, const void *b) {
  long long ao = ((const struct kix_entry *)a)->off;
  long long bo = ((const struct kix_entry *)b)->off;
  return ao < bo ? -1 : (ao > bo ? 1 : 0);
}

/* Sidecar file format: magic "PLIKIX", count (i64), then per entry:
 * keylen (i64), key bytes, off (i64), len (i64). */
#define KIX_MAGIC "PLIKIX"
#define KIX_MAGIC_LEN 6

static void kix_load(long long slot, const char *path) {
  struct kix_state *st = &rt_kix[slot];
  long long n = pli_strlen(path);
  if (n + 5 > (long long)sizeof st->kixpath - 1) n = (long long)sizeof st->kixpath - 5;
  pli_memcpy(st->kixpath, path, (size_t)n);
  st->kixpath[n] = '\0';
  pli_memcpy(st->kixpath + n, ".kix", 5);
  FILE *f = pli_plat_fopen(st->kixpath, "rb");
  if (!f) return; /* no sidecar yet: empty index */
  char magic[KIX_MAGIC_LEN];
  if (pli_plat_fread(magic, 1, KIX_MAGIC_LEN, f) != KIX_MAGIC_LEN ||
      pli_memcmp(magic, KIX_MAGIC, KIX_MAGIC_LEN) != 0) {
    pli_plat_fclose(f);
    return;
  }
  long long count = 0;
  if (pli_plat_fread(&count, sizeof count, 1, f) != 1) { pli_plat_fclose(f); return; }
  for (long long i = 0; i < count; ++i) {
    long long keylen = 0, off = 0, len = 0;
    if (pli_plat_fread(&keylen, sizeof keylen, 1, f) != 1 ||
        keylen < 0 || keylen > (long long)sizeof(((struct kix_entry *)0)->key) * 0 + 1024)
      break;
    char *key = (char *)pli_plat_malloc((size_t)keylen);
    if (!key) break;
    if (pli_plat_fread(key, 1, (size_t)keylen, f) != (size_t)keylen ||
        pli_plat_fread(&off, sizeof off, 1, f) != 1 ||
        pli_plat_fread(&len, sizeof len, 1, f) != 1) {
      pli_plat_free(key);
      break;
    }
    /* Append (unsorted); linear search is used for lookups. */
    if (st->count == st->cap) {
      long long ncap = st->cap ? st->cap * 2 : 16;
      struct kix_entry *m = (struct kix_entry *)pli_plat_realloc(
          st->ent, (size_t)ncap * sizeof(struct kix_entry));
      if (!m) { pli_plat_free(key); break; }
      st->ent = m; st->cap = ncap;
    }
    st->ent[st->count].key = key;
    st->ent[st->count].keylen = keylen;
    st->ent[st->count].off = off;
    st->ent[st->count].len = len;
    st->count++;
  }
  pli_plat_fclose(f);
}

static void kix_save(long long slot) {
  struct kix_state *st = &rt_kix[slot];
  if (!st->dirty) return;
  FILE *f = pli_plat_fopen(st->kixpath, "wb");
  if (!f) { pli_signal_error("keyed sidecar could not be written"); return; }
  if (pli_plat_fwrite(KIX_MAGIC, 1, KIX_MAGIC_LEN, f) != KIX_MAGIC_LEN ||
      pli_plat_fwrite(&st->count, sizeof st->count, 1, f) != 1) {
    pli_plat_fclose(f); return;
  }
  /* Sort by offset so the sidecar mirrors the data file layout. */
  struct kix_entry *copy = (struct kix_entry *)pli_plat_malloc(
      (size_t)st->count * sizeof(struct kix_entry));
  if (!copy) { pli_plat_fclose(f); return; }
  pli_memcpy(copy, st->ent, (size_t)st->count * sizeof(struct kix_entry));
  qsort(copy, (size_t)st->count, sizeof(struct kix_entry), kix_cmp_off);
  for (long long i = 0; i < st->count; ++i) {
    long long keylen = copy[i].keylen;
    if (pli_plat_fwrite(&keylen, sizeof keylen, 1, f) != 1 ||
        pli_plat_fwrite(copy[i].key, 1, (size_t)keylen, f) != (size_t)keylen ||
        pli_plat_fwrite(&copy[i].off, sizeof copy[i].off, 1, f) != 1 ||
        pli_plat_fwrite(&copy[i].len, sizeof copy[i].len, 1, f) != 1) {
      break;
    }
  }
  pli_plat_free(copy);
  pli_plat_fclose(f);
  st->dirty = 0;
}

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

/* KEYED/DIRECT OPEN: open the data file and load the <name>.kix sidecar index.
 * OUTPUT mode opens r+b when the file exists (read+write for REWRITE/DELETE)
 * and wb when it does not; INPUT opens rb; UPDATE opens r+b. */
void pli_file_open_keyed(long long slot, const char *name, long long namelen,
                         long long mode, long long nonprint, long long keyed) {
  if (slot < 0 || slot >= PLI_MAX_FILES)
    return;
  char buf[256];
  if (namelen < 0) namelen = 0;
  size_t n = namelen < (long long)(sizeof buf - 1) ? (size_t)namelen : sizeof buf - 1;
  pli_memcpy(buf, name, n);
  buf[n] = '\0';
  rt_rec_reset(slot);
  kix_reset(slot);
  rt_kix[slot].is_keyed = 1;
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
  /* Determine the fopen mode. OUTPUT creates/truncates (wb); UPDATE opens
   * read-write (r+b); INPUT opens read-only (rb). */
  const char *fmode;
  if (mode == 2)
    fmode = "r+b";
  else if (mode == 1)
    fmode = "wb";
  else
    fmode = "rb";
  rt_pli_files[slot] = pli_plat_fopen(buf, fmode);
  if (rt_pli_files[slot]) {
    /* OUTPUT truncates the data file: discard any stale sidecar index and do
     * not load the old .kix (it will be rewritten on close). */
    if (mode == 1) {
      kix_reset(slot);
      rt_kix[slot].is_keyed = 1;
      rt_kix[slot].dirty = 1;
      long long n = pli_strlen(buf);
      if (n + 5 > (long long)sizeof rt_kix[slot].kixpath - 1)
        n = (long long)sizeof rt_kix[slot].kixpath - 5;
      pli_memcpy(rt_kix[slot].kixpath, buf, (size_t)n);
      rt_kix[slot].kixpath[n] = '\0';
      pli_memcpy(rt_kix[slot].kixpath + n, ".kix", 5);
    } else
      kix_load(slot, buf);
  }
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
  rt_rec_read_lock();
  off = pli_plat_ftell(f);
  if (rt_fbroker_fread(&v, sizeof v, f) != sizeof v) {
    rt_rec_read_unlock();
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  rt_rec_read_unlock();
  return v;
}

double pli_record_read_float(long long slot) {
  FILE *f = rec_file(slot, "READ");
  double v = 0;
  long long off;
  if (!f)
    return 0;
  rt_rec_read_lock();
  off = pli_plat_ftell(f);
  if (rt_fbroker_fread(&v, sizeof v, f) != sizeof v) {
    rt_rec_read_unlock();
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  rt_rec_read_unlock();
  return v;
}

void pli_record_read_char(long long slot, char *p, long long len) {
  FILE *f = rec_file(slot, "READ");
  long long off;
  if (!f || len <= 0)
    return;
  rt_rec_read_lock();
  off = pli_plat_ftell(f);
  if (rt_fbroker_fread(p, (size_t)len, f) != (size_t)len) {
    rt_rec_read_unlock();
    rt_rec_endfile(slot);
    return;
  }
  rt_rec_set(slot, off, len);
  rt_rec_read_unlock();
}

unsigned char pli_record_read_bit(long long slot) {
  FILE *f = rec_file(slot, "READ");
  unsigned char v = 0;
  long long off;
  if (!f)
    return 0;
  rt_rec_read_lock();
  off = pli_plat_ftell(f);
  if (rt_fbroker_fread(&v, sizeof v, f) != sizeof v) {
    rt_rec_read_unlock();
    rt_rec_endfile(slot);
    return 0;
  }
  rt_rec_set(slot, off, (long long)sizeof v);
  rt_rec_read_unlock();
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

/* --- KEYED/DIRECT record I/O (rules (112),(113), ADR-366) --- */

/* Look up a key in the sidecar index; returns the entry index or -1. */
static long long kix_lookup(long long slot, char *key, long long keylen) {
  return kix_find(slot, key, keylen);
}

/* WRITE ... KEY(k): key must not already exist; append the record and index it. */
void pli_record_write_keyed_fixed(long long slot, char *key, long long keylen,
                                  long long v) {
  FILE *f = rec_file(slot, "WRITE");
  long long idx;
  if (!f || kix_lookup(slot, key, keylen) >= 0) {
    if (f) pli_signal_error("WRITE duplicates an existing key");
    return;
  }
  if (pli_plat_fseek(f, 0, SEEK_END) != 0) { pli_signal_error("WRITE to keyed file failed"); return; }
  long long off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("WRITE to keyed file failed"); return; }
  kix_insert(slot, key, keylen, off, (long long)sizeof v);
}

void pli_record_write_keyed_float(long long slot, char *key, long long keylen, double v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f || kix_lookup(slot, key, keylen) >= 0) {
    if (f) pli_signal_error("WRITE duplicates an existing key");
    return;
  }
  if (pli_plat_fseek(f, 0, SEEK_END) != 0) { pli_signal_error("WRITE to keyed file failed"); return; }
  long long off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("WRITE to keyed file failed"); return; }
  kix_insert(slot, key, keylen, off, (long long)sizeof v);
}

void pli_record_write_keyed_char(long long slot, char *key, long long keylen,
                                 char *p, long long len) {
  FILE *f = rec_file(slot, "WRITE");
  long long off;
  if (!f || len <= 0 || kix_lookup(slot, key, keylen) >= 0) {
    if (f) pli_signal_error("WRITE duplicates an existing key");
    return;
  }
  if (pli_plat_fseek(f, 0, SEEK_END) != 0) { pli_signal_error("WRITE to keyed file failed"); return; }
  off = pli_plat_ftell(f);
  if (pli_plat_fwrite(p, 1, (size_t)len, f) != (size_t)len) { pli_signal_error("WRITE to keyed file failed"); return; }
  kix_insert(slot, key, keylen, off, len);
}

void pli_record_write_keyed_bit(long long slot, char *key, long long keylen,
                                unsigned char v) {
  FILE *f = rec_file(slot, "WRITE");
  if (!f || kix_lookup(slot, key, keylen) >= 0) {
    if (f) pli_signal_error("WRITE duplicates an existing key");
    return;
  }
  if (pli_plat_fseek(f, 0, SEEK_END) != 0) { pli_signal_error("WRITE to keyed file failed"); return; }
  long long off = pli_plat_ftell(f);
  if (pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("WRITE to keyed file failed"); return; }
  kix_insert(slot, key, keylen, off, (long long)sizeof v);
}

/* READ ... KEY(k): look up the key and read the record into the buffer. */
long long pli_record_read_keyed_fixed(long long slot, char *key, long long keylen) {
  FILE *f = rec_file(slot, "READ");
  long long v = 0, idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("READ key not found");
    return 0;
  }
  if (pli_plat_fseek(f, rt_kix[slot].ent[idx].off, SEEK_SET) != 0 ||
      pli_plat_fread(&v, sizeof v, 1, f) != sizeof v) {
    pli_signal_error("READ from keyed file failed");
    return 0;
  }
  return v;
}

double pli_record_read_keyed_float(long long slot, char *key, long long keylen) {
  FILE *f = rec_file(slot, "READ");
  double v = 0; long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("READ key not found");
    return 0;
  }
  if (pli_plat_fseek(f, rt_kix[slot].ent[idx].off, SEEK_SET) != 0 ||
      pli_plat_fread(&v, sizeof v, 1, f) != sizeof v) {
    pli_signal_error("READ from keyed file failed");
    return 0;
  }
  return v;
}

void pli_record_read_keyed_char(long long slot, char *key, long long keylen,
                                char *p, long long len) {
  FILE *f = rec_file(slot, "READ");
  long long idx;
  if (!f || len <= 0 || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("READ key not found");
    return;
  }
  long long rlen = rt_kix[slot].ent[idx].len;
  if (pli_plat_fseek(f, rt_kix[slot].ent[idx].off, SEEK_SET) != 0 ||
      pli_plat_fread(p, 1, (size_t)(rlen < len ? rlen : len), f) != (size_t)(rlen < len ? rlen : len)) {
    pli_signal_error("READ from keyed file failed");
    return;
  }
}

unsigned char pli_record_read_keyed_bit(long long slot, char *key, long long keylen) {
  FILE *f = rec_file(slot, "READ");
  unsigned char v = 0; long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("READ key not found");
    return 0;
  }
  if (pli_plat_fseek(f, rt_kix[slot].ent[idx].off, SEEK_SET) != 0 ||
      pli_plat_fread(&v, sizeof v, 1, f) != sizeof v) {
    pli_signal_error("READ from keyed file failed");
    return 0;
  }
  return v;
}

/* REWRITE ... KEYFROM(k): look up the key and rewrite the record in place. */
void pli_record_rewrite_keyed_fixed(long long slot, char *key, long long keylen,
                                    long long v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("REWRITE key not found");
    return;
  }
  long long off = rt_kix[slot].ent[idx].off;
  long long rlen = rt_kix[slot].ent[idx].len;
  if (rlen != (long long)sizeof v) { pli_signal_error("REWRITE changes the record size"); return; }
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("REWRITE to keyed file failed"); return; }
}

void pli_record_rewrite_keyed_float(long long slot, char *key, long long keylen,
                                    double v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("REWRITE key not found");
    return;
  }
  long long off = rt_kix[slot].ent[idx].off;
  long long rlen = rt_kix[slot].ent[idx].len;
  if (rlen != (long long)sizeof v) { pli_signal_error("REWRITE changes the record size"); return; }
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("REWRITE to keyed file failed"); return; }
}

void pli_record_rewrite_keyed_char(long long slot, char *key, long long keylen,
                                   char *p, long long len) {
  FILE *f = rec_file(slot, "REWRITE");
  long long idx;
  if (!f || len <= 0 || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("REWRITE key not found");
    return;
  }
  long long off = rt_kix[slot].ent[idx].off;
  long long rlen = rt_kix[slot].ent[idx].len;
  if (rlen != len) { pli_signal_error("REWRITE changes the record size"); return; }
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(p, 1, (size_t)len, f) != (size_t)len) { pli_signal_error("REWRITE to keyed file failed"); return; }
}

void pli_record_rewrite_keyed_bit(long long slot, char *key, long long keylen,
                                  unsigned char v) {
  FILE *f = rec_file(slot, "REWRITE");
  long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("REWRITE key not found");
    return;
  }
  long long off = rt_kix[slot].ent[idx].off;
  long long rlen = rt_kix[slot].ent[idx].len;
  if (rlen != (long long)sizeof v) { pli_signal_error("REWRITE changes the record size"); return; }
  if (pli_plat_fseek(f, off, SEEK_SET) != 0 ||
      pli_plat_fwrite(&v, sizeof v, 1, f) != 1) { pli_signal_error("REWRITE to keyed file failed"); return; }
}

/* DELETE ... KEYFROM(k): look up the key, remove the record, shift the tail. */
void pli_record_delete_keyed(long long slot, char *key, long long keylen) {
  FILE *f = rec_file(slot, "DELETE");
  long long idx;
  if (!f || (idx = kix_lookup(slot, key, keylen)) < 0) {
    if (f) pli_signal_error("DELETE key not found");
    return;
  }
  long long off = rt_kix[slot].ent[idx].off;
  long long size = rt_kix[slot].ent[idx].len;
  if (pli_plat_fseek(f, 0, SEEK_END) != 0) { pli_signal_error("DELETE from keyed file failed"); return; }
  long long end = pli_plat_ftell(f);
  long long src = off + size;
  long long dst = off;
  char buf[8192];
  if (src > end) { pli_signal_error("DELETE from keyed file failed"); return; }
  while (src < end) {
    long long n = end - src;
    if (n > (long long)sizeof buf) n = (long long)sizeof buf;
    if (pli_plat_fseek(f, src, SEEK_SET) != 0 ||
        pli_plat_fread(buf, 1, (size_t)n, f) != (size_t)n ||
        pli_plat_fseek(f, dst, SEEK_SET) != 0 ||
        pli_plat_fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
      pli_signal_error("DELETE from keyed file failed");
      return;
    }
    src += n; dst += n;
  }
  if (pli_plat_ftruncate(f, dst) != 0) { pli_signal_error("DELETE from keyed file failed"); return; }
  /* Remove the deleted entry and re-key every following entry to its new offset. */
  kix_remove(slot, idx);
  for (long long i = 0; i < rt_kix[slot].count; ++i)
    if (rt_kix[slot].ent[i].off >= off + size)
      rt_kix[slot].ent[i].off -= size;
  rt_kix[slot].dirty = 1;
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

