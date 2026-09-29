/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* ADR-169 proof: an UNALIGNED PL/I structure has the same layout as a C
   `__attribute__((packed))` struct, while the default ALIGNED structure
   matches an ordinary C struct. */
#include <stddef.h>
#include <stdint.h>

struct ua_aligned {
  int32_t fd;
  unsigned char flag;
  int32_t code;
};

struct __attribute__((packed)) ua_packed {
  int32_t fd;
  unsigned char flag;
  int32_t code;
};

int ua_aligned_size(void) { return (int)sizeof(struct ua_aligned); }
int ua_aligned_off_code(void) { return (int)offsetof(struct ua_aligned, code); }
int ua_packed_size(void) { return (int)sizeof(struct ua_packed); }
int ua_packed_off_code(void) { return (int)offsetof(struct ua_packed, code); }

int ua_packed_read_fd(struct ua_packed *r) { return r->fd; }
int ua_packed_read_flag(struct ua_packed *r) { return r->flag; }
int ua_packed_read_code(struct ua_packed *r) { return r->code; }
void ua_packed_write_code(struct ua_packed *r, int v) { r->code = v; }

int ua_aligned_read_code(struct ua_aligned *r) { return r->code; }
