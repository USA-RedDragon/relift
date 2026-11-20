#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double poly(double x)
{
    double r = 0;
    for (int i = 9; i >= 0; i--)
        r = r * x + (double)(i + 1) / 7.0;
    return r;
}

int main(void)
{
    printf("%f %e %g\n", 3.14159265358979, 6.02214076e23, 1.0 / 3.0);
    printf("%.10f %.3e %10.4f|%-10.2f|\n", 2.718281828459045, 12345.6789, -1.5, 2.25);
    printf("%g %g %g %g\n", 100000.0, 1000000.0, 1e-5, 0.0001);
    printf("%.0f %.0f %.0f %.0f\n", 0.5, 1.5, 2.5, 3.5);
    printf("sqrt(2)=%.15f cbrt(10)=%.15f\n", sqrt(2.0), cbrt(10.0));
    printf("sin=%.12f cos=%.12f tan=%.12f\n", sin(1.0), cos(1.0), tan(1.0));
    printf("exp=%.12f log=%.12f pow=%.12f atan2=%.12f\n", exp(2.5), log(10.0), pow(2.0, 0.5), atan2(1.0, 2.0));
    printf("floor=%.1f ceil=%.1f trunc=%.1f round=%.1f fmod=%.3f\n", floor(-2.5), ceil(-2.5), trunc(-2.5), round(-2.5), fmod(10.5, 3.0));
    float f = 1.1f;
    printf("float %.9f %.9f %.9f\n", f, f * f, sqrtf(f));
    printf("poly=%.12f\n", poly(0.37));
    printf("inf=%f -inf=%f nan=%f\n", INFINITY, -INFINITY, NAN);
    printf("int conv: %d %d %ld %u\n", (int)3.99, (int)-3.99, (long)1e18, (unsigned)4e9);
    printf("strtod: %.6f %.6f\n", strtod("2.5e3", NULL), strtod("-0.000123", NULL));
    char buf[64];
    snprintf(buf, sizeof buf, "%08.3f|%+.2e|%#g", 3.14159, 12345.678, 2.0);
    puts(buf);
    volatile double acc = 0;
    for (int i = 1; i <= 1000; i++)
        acc += 1.0 / ((double)i * i);
    printf("basel=%.12f\n", acc);
    printf("hypot=%.9f ldexp=%.3f frexp-exp=", hypot(3.0, 4.0), ldexp(1.5, 4));
    int e;
    frexp(1234.5, &e);
    printf("%d\n", e);
    return 0;
}
