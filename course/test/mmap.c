#include <sys/types.h>
#include <sys/mman.h>
#include <stdio.h>

int main(int argc, char **argv){
    void *p = mmap(0, 8192, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    printf("%p\n", p);
    munmap(p, 8192);
}