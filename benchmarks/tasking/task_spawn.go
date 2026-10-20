 // SPDX-License-Identifier: Apache-2.0
 // Copyright 2026 Ross S. - pli-llvm: a PL/I compiler targeting LLVM
 // ADR-234 benchmark W1 (Go baseline): fan-out / fan-in.
 // The lightest possible task: start N tasks that do nothing, then join
 // all N. The same work runs in the PL/I and the C baselines
 // (task_spawn.pli, task_spawn_c.c).
package main

import (
	"fmt"
	"sync"
)

const N = 20000

func main() {
	var wg sync.WaitGroup
	wg.Add(N)
	for i := 0; i < N; i++ {
		go func() {
			wg.Done()
		}()
	}
	wg.Wait()
	fmt.Println("done")
}
