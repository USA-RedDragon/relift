#ifndef SHARED_LIB_H
#define SHARED_LIB_H

extern int shared_global;

int shared_add(int a, int b);
int shared_apply(int (*f)(int), int x);
int shared_counter_next(void);
int shared_ctor_ran(void);
int shared_tls_bump(void);
const char *shared_name(void);

#endif
