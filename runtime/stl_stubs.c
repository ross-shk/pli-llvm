/*
 * MSVC STL helper function stubs for clang-cl + prebuilt LLVM 23 on Windows.
 *
 * The prebuilt LLVM 23 Windows libraries were compiled against a NEWER MSVC
 * STL than the local VS 2022 BuildTools 14.39 provide. The newer STL
 * vectorizes additional <algorithm> operations (__std_replace_4,
 * __std_min_element_f, ...) and uses revised signatures for several helpers:
 * values are passed BY VALUE (not by pointer) and ranges as (first, count).
 * The signatures below were verified against the microsoft/STL headers
 * (xutility/algorithm, late-2024 vintage matching the LLVM build) and against
 * disassembled call sites in the prebuilt LLVM libraries.
 *
 * Only symbols MISSING from the 14.39 system libraries are defined here.
 * Everything the system already provides (find_trivial_*, count_trivial_*,
 * min/max_element_*, minmax_element_*, reverse_*, init_once_*, system_error_*,
 * type_info_compare, ...) must NOT be redefined: the system objects that
 * provide them may be pulled in for other symbols, which would cause
 * duplicate-symbol link errors.
 *
 * On x86-64 __stdcall and __cdecl coincide, so plain C declarations match.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- __std_remove_N: compact out elements == val, return new end ----
 * New STL passes the value BY VALUE (verified: WinEHPrepare passes the
 * 8-byte pointer bits directly in R8). */
void *__std_remove_1(void *first, void *last, uint8_t val) {
    uint8_t *f = (uint8_t *)first, *l = (uint8_t *)last;
    uint8_t *result = f;
    for (uint8_t *it = f; it < l; ++it) {
        if (*it != val) {
            *result++ = *it;
        }
    }
    return result;
}
void *__std_remove_8(void *first, void *last, uint64_t val) {
    uint64_t *f = (uint64_t *)first, *l = (uint64_t *)last;
    uint64_t *result = f;
    for (uint64_t *it = f; it < l; ++it) {
        if (*it != val) {
            *result++ = *it;
        }
    }
    return result;
}

/* ---- __std_replace_N: replace elements == oldval with newval ----
 * New STL passes both values BY VALUE (verified: X86ISelLowering passes
 * 32-bit values in R8D/R9D). */
void __std_replace_4(void *first, void *last, uint32_t old_val, uint32_t new_val) {
    uint32_t *f = (uint32_t *)first, *l = (uint32_t *)last;
    while (f < l) {
        if (*f == old_val) {
            *f = new_val;
        }
        ++f;
    }
}
void __std_replace_8(void *first, void *last, uint64_t old_val, uint64_t new_val) {
    uint64_t *f = (uint64_t *)first, *l = (uint64_t *)last;
    while (f < l) {
        if (*f == old_val) {
            *f = new_val;
        }
        ++f;
    }
}

/* ---- __std_mismatch_N: return ELEMENT index of first difference ----
 * New STL takes (first1, first2, count) and returns an element index
 * (verified: GlobalMergeFunctions compares RAX against an element count and
 * adds it to element pointers). Returns count when fully equal. */
size_t __std_mismatch_1(const void *first1, const void *first2, size_t count) {
    const uint8_t *a = (const uint8_t *)first1, *b = (const uint8_t *)first2;
    size_t i = 0;
    while (i < count) {
        if (a[i] != b[i]) {
            return i;
        }
        ++i;
    }
    return count;
}
size_t __std_mismatch_4(const void *first1, const void *first2, size_t count) {
    const uint32_t *a = (const uint32_t *)first1, *b = (const uint32_t *)first2;
    size_t i = 0;
    while (i < count) {
        if (a[i] != b[i]) {
            return i;
        }
        ++i;
    }
    return count;
}
size_t __std_mismatch_8(const void *first1, const void *first2, size_t count) {
    const uint64_t *a = (const uint64_t *)first1, *b = (const uint64_t *)first2;
    size_t i = 0;
    while (i < count) {
        if (a[i] != b[i]) {
            return i;
        }
        ++i;
    }
    return count;
}

/* ---- __std_search_1: first occurrence of needle, or last1 ----
 * New STL takes (first1, last1, first2, count2): the needle length is a
 * COUNT, not an end pointer. */
const void *__std_search_1(const void *first1, const void *last1,
                            const void *first2, size_t count2) {
    const uint8_t *f1 = (const uint8_t *)first1, *l1 = (const uint8_t *)last1;
    const uint8_t *f2 = (const uint8_t *)first2;
    size_t len1 = (size_t)(l1 - f1);
    if (count2 == 0) {
        return first1;
    }
    if (count2 > len1) {
        return last1;
    }
    for (size_t i = 0; i + count2 <= len1; ++i) {
        if (memcmp(f1 + i, f2, count2) == 0) {
            return f1 + i;
        }
    }
    return last1;
}

/* ---- __std_find_end_1: last occurrence of needle, or last1 ----
 * Same count-based shape as search (verified: StringRef::rfind passes the
 * needle length in R9). An empty needle yields last1. */
const void *__std_find_end_1(const void *first1, const void *last1,
                              const void *first2, size_t count2) {
    const uint8_t *f1 = (const uint8_t *)first1, *l1 = (const uint8_t *)last1;
    const uint8_t *f2 = (const uint8_t *)first2;
    size_t len1 = (size_t)(l1 - f1);
    const void *result = last1;
    if (count2 == 0) {
        return last1;
    }
    if (count2 > len1) {
        return last1;
    }
    for (size_t i = 0; i + count2 <= len1; ++i) {
        if (memcmp(f1 + i, f2, count2) == 0) {
            result = f1 + i;
        }
    }
    return result;
}

/* ---- __std_find_first_of_trivial_8: first element also in set ----
 * 4-pointer shape, unchanged across STL generations. */
const void *__std_find_first_of_trivial_8(const void *first1, const void *last1,
                                           const void *first2, const void *last2) {
    const uint64_t *f1 = (const uint64_t *)first1, *l1 = (const uint64_t *)last1;
    const uint64_t *f2 = (const uint64_t *)first2, *l2 = (const uint64_t *)last2;
    while (f1 < l1) {
        for (const uint64_t *it = f2; it < l2; ++it) {
            if (*f1 == *it) {
                return f1;
            }
        }
        ++f1;
    }
    return (const void *)l1;
}

/* ---- __std_find_first_of_trivial_pos_1: byte offset or -1 ----
 * Used by char_traits find_first_of. Takes (first, count, needle, ncount)
 * and returns the byte POSITION of the first haystack byte present in the
 * needle, or (size_t)-1 when absent (verified by disassembly of the
 * _Traits_find_first_of wrapper: RDX holds an integer count, RAX is added
 * to a base pointer and compared against -1). */
size_t __std_find_first_of_trivial_pos_1(const void *first, size_t count,
                                          const void *needle, size_t ncount) {
    const uint8_t *f = (const uint8_t *)first;
    const uint8_t *n = (const uint8_t *)needle;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < ncount; ++j) {
            if (f[i] == n[j]) {
                return i;
            }
        }
    }
    return (size_t)-1;
}

/* ---- __std_min/max value over a range: return the extremal VALUE ----
 * New STL reduces (first, last) to a scalar (verified: TextDiagnostic
 * passes two pointers and uses 32-bit EAX). Callers guarantee non-empty. */
int32_t __std_min_4i(const void *first, const void *last) {
    const int32_t *f = (const int32_t *)first, *l = (const int32_t *)last;
    int32_t best = *f++;
    while (f < l) {
        if (*f < best) {
            best = *f;
        }
        ++f;
    }
    return best;
}
uint32_t __std_min_4u(const void *first, const void *last) {
    const uint32_t *f = (const uint32_t *)first, *l = (const uint32_t *)last;
    uint32_t best = *f++;
    while (f < l) {
        if (*f < best) {
            best = *f;
        }
        ++f;
    }
    return best;
}
int64_t __std_min_8i(const void *first, const void *last) {
    const int64_t *f = (const int64_t *)first, *l = (const int64_t *)last;
    int64_t best = *f++;
    while (f < l) {
        if (*f < best) {
            best = *f;
        }
        ++f;
    }
    return best;
}
int32_t __std_max_4i(const void *first, const void *last) {
    const int32_t *f = (const int32_t *)first, *l = (const int32_t *)last;
    int32_t best = *f++;
    while (f < l) {
        if (*f > best) {
            best = *f;
        }
        ++f;
    }
    return best;
}
uint32_t __std_max_4u(const void *first, const void *last) {
    const uint32_t *f = (const uint32_t *)first, *l = (const uint32_t *)last;
    uint32_t best = *f++;
    while (f < l) {
        if (*f > best) {
            best = *f;
        }
        ++f;
    }
    return best;
}
uint64_t __std_max_8u(const void *first, const void *last) {
    const uint64_t *f = (const uint64_t *)first, *l = (const uint64_t *)last;
    uint64_t best = *f++;
    while (f < l) {
        if (*f > best) {
            best = *f;
        }
        ++f;
    }
    return best;
}

/* ---- __std_min_element_f/d: pointer to first minimal float/double ----
 * The bool flag is unused for floating point (the STL passes false;
 * verified: RegAllocPBQP zeroes R8D). Scalar `<` replicates min_element
 * semantics including NaN handling. */
const void *__std_min_element_f(const void *first, const void *last, int unused) {
    (void)unused;
    const float *f = (const float *)first, *l = (const float *)last;
    const float *best = f;
    if (f < l) {
        ++f;
        while (f < l) {
            if (*f < *best) {
                best = f;
            }
            ++f;
        }
    }
    return best;
}
const void *__std_min_element_d(const void *first, const void *last, int unused) {
    (void)unused;
    const double *f = (const double *)first, *l = (const double *)last;
    const double *best = f;
    if (f < l) {
        ++f;
        while (f < l) {
            if (*f < *best) {
                best = f;
            }
            ++f;
        }
    }
    return best;
}
