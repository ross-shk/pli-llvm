/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* runtime/platform/posix/pli_rt_plat.h — POSIX backend types. */
#ifndef PLI_RT_PLAT_POSIX_H
#define PLI_RT_PLAT_POSIX_H

#include <pthread.h>

typedef pthread_mutex_t pli_mutex;
typedef pthread_cond_t pli_cond;
typedef pthread_once_t pli_once;
typedef pthread_t pli_thread;
#define PLI_ONCE_INIT PTHREAD_ONCE_INIT

#endif /* PLI_RT_PLAT_POSIX_H */
