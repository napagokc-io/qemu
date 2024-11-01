/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#include "qemu/thread.h"

/*
 * Valid transitions:
 * - free->set, when setting the event
 * - busy->set, when setting the event, followed by qemu_futex_wake_all
 * - set->free, when resetting the event
 * - free->busy, when waiting
 *
 * set->busy does not happen (it can be observed from the outside but
 * it really is set->free->busy).
 *
 * busy->free provably cannot happen; to enforce it, the set->free transition
 * is done with an OR, which becomes a no-op if the event has concurrently
 * transitioned to free or busy.
 */

#define EV_SET         0
#define EV_FREE        1
#define EV_BUSY       -1

void qemu_event_init(QemuEvent *ev, bool init)
{
#ifndef HAVE_FUTEX
    pthread_mutex_init(&ev->lock, NULL);
    pthread_cond_init(&ev->cond, NULL);
#endif

    ev->value = (init ? EV_SET : EV_FREE);
    ev->initialized = true;
}

void qemu_event_destroy(QemuEvent *ev)
{
    assert(ev->initialized);
    ev->initialized = false;
#ifndef HAVE_FUTEX
    pthread_mutex_destroy(&ev->lock);
    pthread_cond_destroy(&ev->cond);
#endif
}

void qemu_event_set(QemuEvent *ev)
{
    assert(ev->initialized);

#ifdef HAVE_FUTEX
    /*
     * Pairs with both qemu_event_reset() and qemu_event_wait().
     *
     * qemu_event_set has release semantics, but because it *loads*
     * ev->value we need a full memory barrier here.
     */
    smp_mb();
    if (qatomic_read(&ev->value) != EV_SET) {
        int old = qatomic_xchg(&ev->value, EV_SET);

        /* Pairs with memory barrier in kernel futex_wait system call.  */
        smp_mb__after_rmw();
        if (old == EV_BUSY) {
            /* There were waiters, wake them up.  */
            qemu_futex_wake_all(ev);
        }
    }
#else
    pthread_mutex_lock(&ev->lock);
    qatomic_set(&ev->value, EV_SET);
    pthread_cond_broadcast(&ev->cond);
    pthread_mutex_unlock(&ev->lock);
#endif
}

void qemu_event_reset(QemuEvent *ev)
{
    assert(ev->initialized);

    /*
     * If there was a concurrent reset (or even reset+wait),
     * do nothing.  Otherwise change EV_SET->EV_FREE.
     */
    qatomic_or(&ev->value, EV_FREE);

    /*
     * Order reset before checking the condition in the caller.
     * Pairs with the first memory barrier in qemu_event_set().
     */
    smp_mb__after_rmw();
}

void qemu_event_wait(QemuEvent *ev)
{
    assert(ev->initialized);

#ifdef HAVE_FUTEX
    while (true) {
        /*
         * qemu_event_wait must synchronize with qemu_event_set even if it does
         * not go down the slow path, so this load-acquire is needed that
         * synchronizes with the first memory barrier in qemu_event_set().
         *
         * If we do go down the slow path, there is no requirement at all: we
         * might miss a qemu_event_set() here but ultimately the memory barrier
         * in qemu_futex_wait() will ensure the check is done correctly.
         */
        unsigned value = qatomic_load_acquire(&ev->value);
        if (value == EV_SET) {
            break;
        }

        if (value == EV_FREE) {
            /*
             * Leave the event reset and tell qemu_event_set that there are
             * waiters.  No need to retry, because there cannot be a concurrent
             * busy->free transition.  After the CAS, the event will be either
             * set or busy.
             *
             * This cmpxchg doesn't have particular ordering requirements if it
             * succeeds (moving the store earlier can only cause
             * qemu_event_set() to issue _more_ wakeups), the failing case needs
             * acquire semantics like the load above.
             */
            if (qatomic_cmpxchg(&ev->value, EV_FREE, EV_BUSY) == EV_SET) {
                break;
            }
        }

        /*
         * This is the final check for a concurrent set, so it does need
         * a smp_mb() pairing with the second barrier of qemu_event_set().
         * The barrier is inside the FUTEX_WAIT system call.
         */
        qemu_futex_wait(ev, EV_BUSY);
    }
#else
    pthread_mutex_lock(&ev->lock);
    if (qatomic_read(&ev->value) != EV_SET) {
        pthread_cond_wait(&ev->cond, &ev->lock);
    }
    pthread_mutex_unlock(&ev->lock);
#endif
}
