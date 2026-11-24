#!/bin/sh
# Checks heapsample: a program that keeps 1 MiB more each round in leak_here and frees what it takes in churn must
# show leak_here as the site that grew between two dumps, and churn not at all. Usage: test.sh [build dir]
set -e
B=${1:-build/host}/heapsample
mkdir -p "$B"
D=$(dirname "$0")
cc -O2 -shared -fPIC -o "$B/heapsample.so" "$D/heapsample.c" -ldl -lpthread
cat > "$B/leaky.c" <<'EOF'
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
__attribute__((noinline)) void *leak_here(size_t n) { void *p = malloc(n); memset(p, 1, n); return p; }
__attribute__((noinline)) void churn(void) { for (int i = 0; i < 64; i++) { void *p = malloc(65536); memset(p, 2, 64); free(p); } }
int main(void)
{
    for (int i = 0; i < 8; i++) { leak_here(1 << 20); churn(); }
    kill(getpid(), SIGUSR2);
    for (int i = 0; i < 8; i++) { leak_here(1 << 20); churn(); }
    for (int i = 0; i < 40; i++) { leak_here(1 << 20); churn(); }
    kill(getpid(), SIGUSR2);
    leak_here(16);
    return 0;
}
EOF
cc -O1 -g -fno-omit-frame-pointer -o "$B/leaky" "$B/leaky.c"
rm -f "$B"/dump.*
RELIFT_HEAPSAMPLE_OUT="$B/dump" LD_PRELOAD="$B/heapsample.so" "$B/leaky"
out=$(python3 "$D/report.py" "$B/dump.0" "$B/dump.1" --top 1 --frames 2)
echo "$out"
echo "$out" | head -1 | grep -q '^grew 4[0-9]\.' || { echo "heapsample: the growth is not about 48 MiB"; exit 1; }
echo "$out" | grep -q leak_here || { echo "heapsample: leak_here is not the site that grew"; exit 1; }
if python3 "$D/report.py" "$B/dump.1" --top 20 --frames 2 | grep -q churn; then echo "heapsample: freed blocks stayed live"; exit 1; fi
echo "heapsample: ok"
