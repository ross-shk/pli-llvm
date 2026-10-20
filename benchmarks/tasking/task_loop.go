// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
// ADR-234 benchmark W1b (Go baseline): the task lifecycle, one task at a
// time. Start a task that does nothing and join it, N times over. The
// same work runs in the PL/I and the C baselines (task_loop.pli,
// task_loop_c.c).
package main

import (
	"fmt"
	"sync"
)

const N = 20000

func main() {
	for i := 0; i < N; i++ {
		var wg sync.WaitGroup
		wg.Add(1)
		go func() {
			wg.Done()
		}()
		wg.Wait()
	}
	fmt.Println("done")
}
