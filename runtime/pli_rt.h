/* pli_rt.h — PL/I runtime library (libpli), M0 subset.
 *
 * Prototypes are generated from runtime/pli_rt_abi.def, the single source of
 * truth for the pli_* ABI (names are the ABI: see docs/ARCHITECTURE.md
 * "Runtime interface"). Each PLI_FN line becomes one prototype; the .def also
 * drives irgen's LLVM declarations, so the two cannot drift.
 */
#ifndef PLI_RT_H
#define PLI_RT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Expand the .def's type tokens to C types. */
#define VOID    void
#define I64     long long
#define I32     int
#define I8      unsigned char
#define DOUBLE  double
#define PTR     char *
#define CPTR    const char *
#define PLI_FN(name, ret, args) ret name args;

#include "pli_rt_abi.def"

#undef PLI_FN
#undef CPTR
#undef PTR
#undef DOUBLE
#undef I8
#undef I32
#undef I64
#undef VOID

#ifdef __cplusplus
}
#endif
#endif /* PLI_RT_H */
