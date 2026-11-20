#include "lib.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int twice(int x) { return x * 2; }

static void *worker(void *arg)
{
    (void)arg;
    int a = shared_tls_bump();
    int b = shared_tls_bump();
    printf("thread tls %d %d\n", a, b);
    return NULL;
}

int main(void)
{
    printf("constructor ran: %d\n", shared_ctor_ran());
    printf("name %s\n", shared_name());
    printf("add %d\n", shared_add(2, 3));
    shared_global = 10;
    printf("add with global %d\n", shared_add(2, 3));
    printf("apply %d\n", shared_apply(twice, 5));
    printf("counter %d %d %d\n", shared_counter_next(), shared_counter_next(), shared_counter_next());
    printf("main tls %d\n", shared_tls_bump());
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_join(t, NULL);
    printf("main tls again %d\n", shared_tls_bump());
    return 4;
}
