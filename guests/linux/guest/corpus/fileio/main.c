#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void)
{
    const char *base = getenv("TMPDIR");
    char dir[256];
    snprintf(dir, sizeof dir, "%s/relift-fileio-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(dir)) {
        printf("mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    char path[300];
    snprintf(path, sizeof path, "%s/a.txt", dir);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < 100; i++)
        fprintf(f, "line %03d %s\n", i, "abcdefghij");
    fclose(f);
    struct stat st;
    stat(path, &st);
    printf("size=%lld regular=%d\n", (long long)st.st_size, S_ISREG(st.st_mode));
    f = fopen(path, "r");
    char line[64];
    int n = 0;
    while (fgets(line, sizeof line, f))
        n++;
    printf("lines=%d\n", n);
    fseek(f, 5 * 20L, SEEK_SET);
    fgets(line, sizeof line, f);
    printf("line5=%s", line);
    printf("tell=%ld\n", ftell(f));
    fclose(f);
    int fd = open(path, O_RDWR);
    lseek(fd, 0, SEEK_END);
    write(fd, "tail\n", 5);
    off_t end = lseek(fd, 0, SEEK_CUR);
    char buf[8] = { 0 };
    pread(fd, buf, 4, end - 5);
    printf("end=%lld tail=%s\n", (long long)end, buf);
    ftruncate(fd, 38);
    fstat(fd, &st);
    printf("truncated=%lld\n", (long long)st.st_size);
    close(fd);
    char path2[300];
    snprintf(path2, sizeof path2, "%s/b.txt", dir);
    rename(path, path2);
    printf("old exists=%d new exists=%d\n", access(path, F_OK) == 0, access(path2, F_OK) == 0);
    char sub[300];
    snprintf(sub, sizeof sub, "%s/sub", dir);
    mkdir(sub, 0755);
    for (int i = 0; i < 3; i++) {
        char p[320];
        snprintf(p, sizeof p, "%s/sub/f%d", dir, 2 - i);
        close(open(p, O_CREAT | O_WRONLY, 0644));
    }
    DIR *d = opendir(sub);
    char *names[8];
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            names[cnt++] = strdup(e->d_name);
    closedir(d);
    qsort(names, cnt, sizeof names[0], cmp);
    for (int i = 0; i < cnt; i++)
        printf("entry %s\n", names[i]);
    int fd2 = open("/nonexistent/relift", O_RDONLY);
    printf("open missing: %d errno=%d (%s)\n", fd2, errno, strerror(errno));
    for (int i = 0; i < 3; i++) {
        char p[320];
        snprintf(p, sizeof p, "%s/sub/f%d", dir, i);
        unlink(p);
    }
    rmdir(sub);
    unlink(path2);
    printf("rmdir=%d\n", rmdir(dir));
    return 0;
}
