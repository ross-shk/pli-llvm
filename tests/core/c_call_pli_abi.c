/* SPDX-License-Identifier: Apache-2.0 */
 /* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
 /* C companion for c_call_pli_abi.pli: C calls exported PL/I functions. */

 #include <stdint.h>
 #include <stddef.h>

 struct pli_rec {
   int32_t tag;
   unsigned char flag;
   int32_t code;
 };

 /* --- Exported PL/I functions (C calls PL/I) --- */
 /* PL/I scalar parameters are passed by reference (pointer). */
 extern int32_t PLI_ADD_ONE(int32_t* x);
 extern void    PLI_INC_INT(int32_t* x);

 /* Structure-return: hidden result buffer pointer is first arg. */
 extern void    PLI_MAKE_REC(struct pli_rec* out, int32_t* x);

 /* Structure parameter: PL/I receives pointer to struct. */
 extern void    PLI_MUTATE_REC(struct pli_rec* r, int32_t* x);

 int c_call_pli_abi_run(void) {
   /* 1) Scalar function result */
   int32_t x = 41;
   int32_t r = PLI_ADD_ONE(&x);
   if (r != 42) return 0;

   /* 2) Scalar by-reference update */
   PLI_INC_INT(&x);
   if (x != 141) return 0;   /* 41 + 100 = 141 */

   /* 3) Structure-valued function return via hidden result buffer */
   int32_t y = 7;
   struct pli_rec out = {0};
   PLI_MAKE_REC(&out, &y);
   if (out.tag != 7) return 0;
   if (out.flag != 1) return 0;          /* expect '1'b stored as 1 */
   if (out.code != 14) return 0;

   /* 4) Structure parameter by reference mutation */
   int32_t z = 9;
   struct pli_rec rec = {0};
   PLI_MUTATE_REC(&rec, &z);
   if (rec.tag != 19) return 0;         /* z + 10 */
   if (rec.flag != 0) return 0;
   if (rec.code != 29) return 0;        /* z + 20 */

   return 1;
 }