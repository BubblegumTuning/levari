#ifndef LEVARI_PEERCREDS_H
#define LEVARI_PEERCREDS_H
#ifndef _WIN32
/* Unix-domain socket peer-uid check. The spelling is platform-specific:
   SO_PEERCRED + struct ucred on Linux (needs _GNU_SOURCE before the glibc
   headers, which both call sites define), LOCAL_PEERCRED + struct xucred
   on Apple. Best effort like the original call sites: a failed query
   passes, since the socket file is already owner-only (umask 0077 +
   chmod 0600); the check is defense against a pre-bound impostor on the
   /tmp fallback path. */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>   /* struct xucred lives here, not in sys/un.h */
#endif
static inline int peer_is_self(int c) {
#ifdef __APPLE__
    struct xucred xc;
    socklen_t xl = sizeof xc;
    xc.cr_version = XUCRED_VERSION;
    if (getsockopt(c, SOL_LOCAL, LOCAL_PEERCRED, &xc, &xl) != 0) return 1;
    return xc.cr_uid == getuid();
#else
    struct ucred uc;
    socklen_t ul = sizeof uc;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &uc, &ul) != 0) return 1;
    return uc.uid == getuid();
#endif
}
#endif
#endif
