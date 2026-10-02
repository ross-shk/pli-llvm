/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Ross S. - plic: a PL/I compiler targeting LLVM */
/* C baseline for mandelbrot benchmark */
#include <stdio.h>

int main(void) {
    int w = 600, h = 400, max_iter = 100;
    int total = 0, i, j, iter;
    double cx, cy, zx, zy, tmp, mag;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            cx = (i * 3.6) / w - 2.6;
            cy = (j * 3.0) / h - 1.5;
            zx = 0.0;
            zy = 0.0;
            iter = 0;
            while (iter < max_iter) {
                tmp = zx * zx - zy * zy;
                zy = 2.0 * zx * zy + cy;
                zx = tmp + cx;
                mag = zx * zx + zy * zy;
                iter++;
                if (mag > 64.0) break;
            }
            total += iter;
        }
    }
    printf("%d\n", total);
    return 0;
}
