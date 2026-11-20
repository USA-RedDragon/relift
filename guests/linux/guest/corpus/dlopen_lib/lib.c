#include <stdio.h>

static int calls;

__attribute__((constructor)) static void plugin_init(void) { puts("plugin: constructor"); }

__attribute__((destructor)) static void plugin_fini(void) { puts("plugin: destructor"); }

int plugin_compute(int x)
{
    calls++;
    return x * x + calls;
}

const char *plugin_name(void) { return "squarer"; }
