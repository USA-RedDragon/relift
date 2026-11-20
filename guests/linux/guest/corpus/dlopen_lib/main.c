#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
    void *h = dlopen("libdlopen_lib.so", RTLD_NOW);
    if (!h) {
        puts("dlopen failed");
        return 1;
    }
    const char *(*name)(void) = (const char *(*)(void))dlsym(h, "plugin_name");
    int (*compute)(int) = (int (*)(int))dlsym(h, "plugin_compute");
    if (!name || !compute) {
        puts("dlsym failed");
        return 2;
    }
    printf("plugin %s\n", name());
    printf("compute %d %d\n", compute(6), compute(7));
    int (*puts_again)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "puts");
    puts_again("puts found through RTLD_DEFAULT");
    printf("missing symbol: %s\n", dlsym(h, "plugin_absent") ? "found" : "not found");
    dlclose(h);
    puts("closed");
    return 0;
}
