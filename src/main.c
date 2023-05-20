#include <stdio.h>
#include "sfmm.h"

int main(int argc, char const *argv[]) {
    size_t sz = 41, align = 23;
    sf_malloc(sz);
    void *x = sf_memalign(sz,align);
    sf_show_heap();
    sf_free(x);
}