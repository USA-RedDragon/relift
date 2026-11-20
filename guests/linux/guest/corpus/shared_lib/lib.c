#include "lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int shared_global = 7;

static int ctor_ran;
static int counter;
static __thread int tls_value = 100;
static char *name;

__attribute__((constructor)) static void shared_init(void)
{
    ctor_ran = 1;
    name = strdup("shared");
    puts("shared_lib: constructor");
}

__attribute__((destructor)) static void shared_fini(void)
{
    puts("shared_lib: destructor");
    free(name);
}

int shared_add(int a, int b) { return a + b + shared_global - 7; }

int shared_apply(int (*f)(int), int x) { return f(f(x)); }

int shared_counter_next(void) { return ++counter; }

int shared_ctor_ran(void) { return ctor_ran; }

int shared_tls_bump(void) { return ++tls_value; }

const char *shared_name(void) { return name; }
