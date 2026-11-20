#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { WORKERS = 4, ROUNDS = 20000 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static long counter;
static int started;
static _Thread_local int mine;
static volatile int atomic_hits;

static void *worker(void *arg)
{
    long id = (long)arg;
    mine = (int)id * 10;
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        __atomic_fetch_add(&atomic_hits, 1, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_lock(&lock);
    started++;
    pthread_cond_broadcast(&ready);
    pthread_mutex_unlock(&lock);
    return (void *)(long)(mine + 1);
}

static void *sleeper(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (started < WORKERS)
        pthread_cond_wait(&ready, &lock);
    pthread_mutex_unlock(&lock);
    return (void *)99;
}

int main(void)
{
    pthread_t t[WORKERS], s;
    pthread_create(&s, NULL, sleeper, NULL);
    for (long i = 0; i < WORKERS; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    long sum = 0;
    for (int i = 0; i < WORKERS; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    void *r;
    pthread_join(s, &r);
    printf("counter=%ld atomic=%d sum=%ld sleeper=%ld main_tls=%d\n", counter, atomic_hits, sum, (long)r, mine);
    pthread_t late;
    pthread_create(&late, NULL, worker, (void *)7);
    pthread_join(late, &r);
    printf("late=%ld counter=%ld\n", (long)r, counter);
    return 0;
}
