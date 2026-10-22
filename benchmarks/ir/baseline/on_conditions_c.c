#include <stdio.h>

int main(void) {
    long long s = 0;
    for (int i = 1; i <= 60000000; i++) {
        int q;
        int d = i % 100;
        if (d != 0)
            q = 1000000 / d;
        else
            q = 0;
        s += i;
        s += q % 10;
    }
    printf("%lld\n", s);
    return 0;
}
