#include <stdio.h>

int main(void) {
    int s = 0;
    for (int i = 1; i <= 50000; i++) {
        int q;
        int d = i % 100;
        if (d != 0)
            q = 1000000 / d;
        else
            q = 0;
        s += i;
        s += q % 10;
    }
    printf("%d\n", s);
    return 0;
}
