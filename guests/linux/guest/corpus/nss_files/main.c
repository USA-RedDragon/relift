#include <arpa/inet.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/socket.h>

int main(void)
{
    struct passwd *pw = getpwnam("relift");
    if (pw)
        printf("pw %s uid=%d gid=%d gecos=%s home=%s shell=%s\n", pw->pw_name, (int)pw->pw_uid, (int)pw->pw_gid,
               pw->pw_gecos, pw->pw_dir, pw->pw_shell);
    else
        printf("pw relift: none\n");
    pw = getpwuid(0);
    printf("uid 0: %s\n", pw ? pw->pw_name : "none");
    pw = getpwnam("nobody-here");
    printf("nobody-here: %s\n", pw ? "found" : "none");
    struct group *gr = getgrnam("users");
    if (gr) {
        printf("gr %s gid=%d members:", gr->gr_name, (int)gr->gr_gid);
        for (char **m = gr->gr_mem; *m; m++)
            printf(" %s", *m);
        printf("\n");
    } else
        printf("gr users: none\n");
    gr = getgrgid(0);
    printf("gid 0: %s\n", gr ? gr->gr_name : "none");
    struct hostent *h = gethostbyname("box");
    if (h)
        printf("host %s %s\n", h->h_name, inet_ntoa(*(struct in_addr *)h->h_addr_list[0]));
    else
        printf("host box: none\n");
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo("localhost", "80", &hints, &res) == 0) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        printf("addr %s:%d\n", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
        freeaddrinfo(res);
    } else
        printf("addr localhost: none\n");
    return 0;
}
