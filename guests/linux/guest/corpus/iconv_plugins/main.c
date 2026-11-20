#include <iconv.h>
#include <stdio.h>
#include <string.h>

static int convert(const char *to, const char *from, const char *in)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        printf("%s <- %s: unavailable\n", to, from);
        return 1;
    }
    char out[64];
    char *ip = (char *)in, *op = out;
    size_t il = strlen(in), ol = sizeof out;
    size_t r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1) {
        printf("%s <- %s: conversion failed\n", to, from);
        return 1;
    }
    printf("%s <- %s:", to, from);
    for (char *p = out; p < op; p++)
        printf(" %02x", (unsigned char)*p);
    printf("\n");
    return 0;
}

int main(void)
{
    int failed = 0;
    failed += convert("ISO-8859-15", "UTF-8", "caf\xc3\xa9 \xe2\x82\xac");
    failed += convert("IBM850", "UTF-8", "na\xc3\xafve");
    failed += convert("UTF-16LE", "UTF-8", "h\xc3\xa9llo");
    failed += convert("SJIS", "UTF-8", "\xe6\x97\xa5\xe6\x9c\xac");
    failed += convert("NO-SUCH-CHARSET", "UTF-8", "x");
    return failed;
}
