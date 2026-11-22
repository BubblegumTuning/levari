#ifndef LV_WIN98_WAKE_H
#define LV_WIN98_WAKE_H
/* win98 era wake primitive (LV_WIN9X only) — batch 21.

   Why: batch 20's hardware log settled the decision tree on branch 1.
   Every command post logged pstm AND pstmok (p->mtx lock/unlock and
   pthread_cond_signal all RETURN on this box — the poster side is
   healthy; the mutex is additionally hardware-proven from the UI
   side: every draw frame locks it), while pwake NEVER fired in two
   boots — not even for the quit broadcast at close. The parked
   player thread never observed any wake from winpthread's
   pthread_cond_wait: its internals ride our batch-3
   TryEnterCriticalSection emulation and its own semaphores —
   unproven metal. Fix per doctrine: replace only the cond; every
   primitive this replacement touches is 95-stock kernel32 already in
   the import table (CreateEventA/SetEvent/ResetEvent/
   WaitForSingleObject), so the ILT stays byte-identical.

   Contract (matches how player.c uses a cond):
   - signal runs on the poster side WHILE HOLDING the guard mutex;
     with exactly one waiter (the player thread), a manual-reset
     event is exactly a broadcast — the era stop path's broadcast and
     the post paths' signal become the same call.
   - wait runs with the mutex held after the predicate was (re-)
     checked true: it UNLOCKS the mutex (never wait holding
     p->mtx — the poster would block), waits INFINITE on the event,
     RESETS it, then RE-ACQUIRES the mutex before returning, so the
     caller's predicate loop sees plain cond semantics.
   - Lost edges are benign: every wake source stores its predicate
     (p->cmd / p->quit) under the mutex BEFORE signalling, and the
     predicate re-check after re-acquire catches any post that
     raced the reset — a missed kick with a satisfied predicate
     cannot strand the thread (the event is a level-kick, not the
     state).
   - If CreateEventA ever failed, waits degrade to a bounded 50 ms
     poll (predicate still correct, no hot spin, log names it). */
#if defined(LV_WIN9X) && defined(_WIN32)
#include <windows.h>
#include <pthread.h>
#include "../player/player.h"   /* lv_wake_t lives there (void*-shaped) */
#include "win98_diag.h"

static inline void lv_wake_init(lv_wake_t *w) {
    /* manual-reset (broadcast shape), initially unsignaled;
       HANDLE is void* — the player.h member stores it uncast */
    w->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!w->ev) lv98_tracef("wake:init FAIL err=%lu", GetLastError());
}

static inline void lv_wake_signal(lv_wake_t *w) {
    if (w->ev) SetEvent(w->ev);
}

static inline void lv_wake_wait(lv_wake_t *w, pthread_mutex_t *mtx) {
    if (!w->ev) { Sleep(50); return; }
    pthread_mutex_unlock(mtx);
    WaitForSingleObject(w->ev, INFINITE);
    ResetEvent(w->ev);
    pthread_mutex_lock(mtx);
}

#define LV_WAKE_T          lv_wake_t
#define LV_WAKE_INIT(w)    lv_wake_init(w)
#define LV_WAKE_SIGNAL(w)  lv_wake_signal(w)
#define LV_WAKE_BROADCAST(w) lv_wake_signal(w) /* one waiter: kick == broadcast */
#define LV_WAKE_WAIT(w, m) lv_wake_wait((w), (m))
#endif
#endif
