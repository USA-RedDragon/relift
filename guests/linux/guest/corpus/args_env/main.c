#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

int main(int argc, char **argv)
{
    printf("argc=%d\n", argc);
    for (int i = 1; i < argc; i++)
        printf("argv[%d]=<%s>\n", i, argv[i]);
    printf("argv[argc] is null: %d\n", argv[argc] == NULL);
    const char *v = getenv("RELIFT_TEST_ALPHA");
    printf("ALPHA=%s\n", v ? v : "(unset)");
    v = getenv("RELIFT_TEST_EMPTY");
    printf("EMPTY=%s\n", v ? (*v ? v : "(empty)") : "(unset)");
    printf("MISSING=%s\n", getenv("RELIFT_TEST_MISSING") ? "set" : "(unset)");
    int n = 0;
    for (char **e = environ; *e; e++)
        if (strncmp(*e, "RELIFT_TEST_", 12) == 0)
            n++;
    printf("RELIFT_TEST_ vars=%d\n", n);
    setenv("RELIFT_TEST_NEW", "added", 1);
    printf("NEW=%s\n", getenv("RELIFT_TEST_NEW"));
    return argc > 1 ? argc : 0;
}
