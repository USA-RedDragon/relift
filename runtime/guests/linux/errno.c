/**
 * @file errno.c
 * @brief Host errno values to Linux's: the numbers differ between hosts, so
 * every host error goes through this table by its symbolic name.
 */
#include "linux_priv.h"

#include <errno.h>

int lx_errno(int e)
{
    switch (e) {
#define E(name) \
    case name:  \
        return L_##name;
        E(EPERM)
        E(ENOENT)
        E(ESRCH)
        E(EINTR)
        E(EIO)
        E(ENXIO)
        E(E2BIG)
        E(ENOEXEC)
        E(EBADF)
        E(ECHILD)
        E(ENOMEM)
        E(EACCES)
        E(EFAULT)
        E(EBUSY)
        E(EEXIST)
        E(EXDEV)
        E(ENODEV)
        E(ENOTDIR)
        E(EISDIR)
        E(EINVAL)
        E(ENFILE)
        E(EMFILE)
        E(ENOTTY)
        E(ETXTBSY)
        E(EFBIG)
        E(ENOSPC)
        E(ESPIPE)
        E(EROFS)
        E(EMLINK)
        E(EPIPE)
        E(EDOM)
        E(ERANGE)
        E(EDEADLK)
        E(ENAMETOOLONG)
        E(ENOSYS)
        E(ENOTEMPTY)
        E(ELOOP)
        E(EOVERFLOW)
        E(ETIMEDOUT)
#undef E
    }
    switch (e) {
#define E(name)     \
    case name:      \
        return L_##name;
#ifdef ENOTSOCK
        E(ENOTSOCK)
#endif
#ifdef EDESTADDRREQ
        E(EDESTADDRREQ)
#endif
#ifdef EMSGSIZE
        E(EMSGSIZE)
#endif
#ifdef EPROTOTYPE
        E(EPROTOTYPE)
#endif
#ifdef ENOPROTOOPT
        E(ENOPROTOOPT)
#endif
#ifdef EPROTONOSUPPORT
        E(EPROTONOSUPPORT)
#endif
#ifdef ESOCKTNOSUPPORT
        E(ESOCKTNOSUPPORT)
#endif
#ifdef EPFNOSUPPORT
        E(EPFNOSUPPORT)
#endif
#ifdef EADDRINUSE
        E(EADDRINUSE)
#endif
#ifdef EADDRNOTAVAIL
        E(EADDRNOTAVAIL)
#endif
#ifdef ENETDOWN
        E(ENETDOWN)
#endif
#ifdef ENETUNREACH
        E(ENETUNREACH)
#endif
#ifdef ENETRESET
        E(ENETRESET)
#endif
#ifdef ECONNABORTED
        E(ECONNABORTED)
#endif
#ifdef ECONNRESET
        E(ECONNRESET)
#endif
#ifdef ENOBUFS
        E(ENOBUFS)
#endif
#ifdef EISCONN
        E(EISCONN)
#endif
#ifdef ENOTCONN
        E(ENOTCONN)
#endif
#ifdef ESHUTDOWN
        E(ESHUTDOWN)
#endif
#ifdef ETOOMANYREFS
        E(ETOOMANYREFS)
#endif
#ifdef ECONNREFUSED
        E(ECONNREFUSED)
#endif
#ifdef EHOSTDOWN
        E(EHOSTDOWN)
#endif
#ifdef EHOSTUNREACH
        E(EHOSTUNREACH)
#endif
#ifdef EALREADY
        E(EALREADY)
#endif
#ifdef EINPROGRESS
        E(EINPROGRESS)
#endif
#ifdef EAFNOSUPPORT
        E(EAFNOSUPPORT)
#endif
#ifdef ENODATA
        E(ENODATA)
#endif
#if defined(ENOATTR) && (!defined(ENODATA) || ENOATTR != ENODATA)
    case ENOATTR:
        return L_ENODATA;
#endif
#undef E
    default:
        break;
    }
    if (e == EAGAIN || e == EWOULDBLOCK)
        return L_EAGAIN;
    if (e == ENOTSUP || e == EOPNOTSUPP)
        return L_ENOTSUP;
    return L_EIO;
}

int64_t lx_fail(void) { return -(int64_t)lx_errno(errno); }
