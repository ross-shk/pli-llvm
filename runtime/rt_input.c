/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM */
/* rt_input.c — the input broker (ADR-238).
 *
 * A task that reads stdin must not block the worker it runs on. A
 * blocking read holds the OS thread, and in the fiber model that stalls
 * every other task on that worker - the very thing the fiber model is
 * for. The read therefore happens on a dedicated thread, which fills a
 * queue of chunks; a task that finds the queue empty parks until the
 * reader thread has bytes for it.
 *
 * The parsing stays on the task's side, under the input lock (ADR-233):
 * a record is read as a unit and two tasks never split one. Only the raw
 * bytes cross the thread boundary.
 *
 * This broker serves standard input; stream file reads use the file broker
 * (rt_fbroker.c, ADR-291), and record READ still blocks.
 */
#include "pli_rt.h"
#include "rt_fiber.h"
#include "rt_sched.h"

/* A chunk of input bytes. The queue is a list of them: the reader thread
 * appends, the consumer takes the bytes off the front chunk and frees it
 * when it is empty. */
typedef struct RtChunk {
  struct RtChunk *next;
  int len;
  int pos;
  char data[1];
} RtChunk;

#define RT_INPUT_CHUNK 4096

static struct {
  pli_mutex mu;    /* guards the queue */
  pli_cond cv;     /* the reader thread waits here for a request */
  pli_cond ready;  /* a consumer that is not a fiber waits here */
  RtChunk *head;    /* the queue of chunks */
  RtChunk *tail;
  int eof;         /* the input has ended */
  int requested;    /* a consumer wants more input */
  struct pli_fiber *waiter; /* the fiber parked for input: at most one,
                             * because the reader holds the input lock
                             * (ADR-233) for the whole record */
  pli_thread th;
  pli_once once;
} rt_input;

/* The reader thread: it does the blocking read, so a task never does. */
static void *rt_input_thread(void *arg) {
  (void)arg;
  for (;;) {
    pli_mutex_lock(&rt_input.mu);
    while (!rt_input.requested && !rt_input.eof)
      pli_cond_wait(&rt_input.cv, &rt_input.mu);
    rt_input.requested = 0;
    pli_mutex_unlock(&rt_input.mu);

    char buf[RT_INPUT_CHUNK];
    int n = pli_plat_stdin_read(buf, (int)sizeof buf);
    if (n < 0)
      n = 0;

    pli_mutex_lock(&rt_input.mu);
    int done = 0;
    if (n > 0) {
      RtChunk *c = (RtChunk *)pli_plat_malloc(sizeof(RtChunk) + (size_t)n);
      if (c) {
        c->next = NULL;
        c->len = n;
        c->pos = 0;
        for (int i = 0; i < n; ++i)
          c->data[i] = buf[i];
        if (rt_input.tail)
          rt_input.tail->next = c;
        else
          rt_input.head = c;
        rt_input.tail = c;
      }
    } else {
      /* The input has ended: the reader thread is done. It must not read
       * again, or it would spin on the end of the file. */
      rt_input.eof = 1;
      done = 1;
    }
    /* Take the consumer that is waiting, if any. The consumer published
     * itself under this same lock, so it is either waiting or it has not
     * looked yet - and it will find the bytes in the queue. */
    struct pli_fiber *w = rt_input.waiter;
    rt_input.waiter = NULL;
    if (!w)
      pli_cond_signal(&rt_input.ready);
    pli_mutex_unlock(&rt_input.mu);
    /* Wake the task that parked for the input. A task parks rather than
     * blocking, so the worker stays free to run the other tasks. */
    if (w)
      rt_sched_unpark(w);
    if (done)
      return NULL;
  }
  return NULL;
}

static void rt_input_start(void) {
  pli_mutex_init(&rt_input.mu);
  pli_cond_init(&rt_input.cv);
  pli_cond_init(&rt_input.ready);
  /* A broker that never starts would leave every reader waiting for bytes
   * that cannot arrive: say so instead of hanging. */
  if (pli_spawn(&rt_input.th, rt_input_thread, NULL) != 0)
    pli_abort("TASK: could not start the input thread");
  pli_thread_detach(rt_input.th);
}

/* The next byte of standard input, or EOF. */
int rt_input_getc(void) {
  pli_call_once(&rt_input.once, rt_input_start);
  for (;;) {
    pli_mutex_lock(&rt_input.mu);
    RtChunk *c = rt_input.head;
    if (c) {
      int ch = (unsigned char)c->data[c->pos++];
      if (c->pos >= c->len) {
        rt_input.head = c->next;
        if (!rt_input.head)
          rt_input.tail = NULL;
        pli_plat_free(c);
      }
      pli_mutex_unlock(&rt_input.mu);
      return ch;
    }
    if (rt_input.eof) {
      pli_mutex_unlock(&rt_input.mu);
      return EOF;
    }
    /* No input yet: ask the reader thread for more and wait for it.
     *
     * The waiter is published under this lock before the request goes
     * out, so the reader thread cannot deliver the bytes and find nobody
     * waiting.
     *
     * The task's owner is recorded before it is published: the reader
     * thread may unpark the task before the task reaches its park, and an
     * unpark hands the task to its owner - the worker that is running it
     * now, which picks it up when the task yields. Without the owner the
     * task would go to the global queue, and another worker would run it
     * while it is still running. */
    struct pli_fiber *self = rt_sched_current();
    if (self) {
      self->home = rt_sched_current_worker();
      rt_input.waiter = self;
      rt_input.requested = 1;
      pli_cond_signal(&rt_input.cv);
      pli_mutex_unlock(&rt_input.mu);
      /* A task parks: the worker stays free to run the other tasks. */
      rt_sched_park();
      continue;
    }
    /* The legacy model: the thread blocks, which is what it always did. */
    rt_input.requested = 1;
    pli_cond_signal(&rt_input.cv);
    while (!rt_input.head && !rt_input.eof)
      pli_cond_wait(&rt_input.ready, &rt_input.mu);
    pli_mutex_unlock(&rt_input.mu);
  }
}
