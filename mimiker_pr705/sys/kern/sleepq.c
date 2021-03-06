#define KL_LOG KL_SLEEPQ
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/queue.h>
#include <sys/libkern.h>
#include <sys/sleepq.h>
#include <sys/pool.h>
#include <sys/sched.h>
#include <sys/spinlock.h>
#include <sys/thread.h>
#include <sys/interrupt.h>
#include <sys/errno.h>
#include <sys/callout.h>
#include <sys/sysinit.h>

#define SC_TABLESIZE 256 /* Must be power of 2. */
#define SC_MASK (SC_TABLESIZE - 1)
#define SC_SHIFT 8
#define SC_HASH(wc)                                                            \
  ((((uintptr_t)(wc) >> SC_SHIFT) ^ (uintptr_t)(wc)) & SC_MASK)
#define SC_LOOKUP(wc) &sleepq_chains[SC_HASH(wc)]

/*! \brief bucket of sleep queues */
typedef struct sleepq_chain {
  spin_t sc_lock;
  TAILQ_HEAD(, sleepq) sc_queues; /*!< list of sleep queues */
} sleepq_chain_t;

/*! \brief Sleeping mode.
 *
 * Used to select sleeping mode in `_sleepq_wait`. For sleeping purposes given
 * mode implies former modes, i.e. sleep with timeout (`SLP_TIMED`) is also
 * interruptible (`SLP_INTR`).
 */
typedef enum { SLP_NORMAL = 0, SLP_INTR = 1, SLP_TIMED = 2 } sleep_t;

static sleepq_chain_t sleepq_chains[SC_TABLESIZE];

static sleepq_chain_t *sc_acquire(void *wchan) {
  sleepq_chain_t *sc = SC_LOOKUP(wchan);
  spin_lock(&sc->sc_lock);
  return sc;
}

static bool sc_owned(sleepq_chain_t *sc) {
  return spin_owned(&sc->sc_lock);
}

static void sc_release(sleepq_chain_t *sc) {
  spin_unlock(&sc->sc_lock);
}

/*! \brief stores all threads sleeping on the same resource */
typedef struct sleepq {
  spin_t sq_lock;
  TAILQ_ENTRY(sleepq) sq_entry;    /*!< link on sleepq_chain */
  TAILQ_HEAD(, sleepq) sq_free;    /*!< unused sleep queue records */
  TAILQ_HEAD(, thread) sq_blocked; /*!< blocked threads */
  unsigned sq_nblocked;            /*!< number of blocked threads */
  void *sq_wchan;                  /*!< associated waiting channel */
} sleepq_t;

static void sq_acquire(sleepq_t *sq) {
  spin_lock(&sq->sq_lock);
}

static bool sq_owned(sleepq_t *sq) {
  return spin_owned(&sq->sq_lock);
}

static void sq_release(sleepq_t *sq) {
  spin_unlock(&sq->sq_lock);
}

static void sq_ctor(sleepq_t *sq) {
  TAILQ_INIT(&sq->sq_blocked);
  TAILQ_INIT(&sq->sq_free);
  sq->sq_nblocked = 0;
  sq->sq_wchan = NULL;
  sq->sq_lock = SPIN_INITIALIZER(0);
}

static void sleepq_init(void) {
  memset(sleepq_chains, 0, sizeof(sleepq_chains));

  for (int i = 0; i < SC_TABLESIZE; i++) {
    sleepq_chain_t *sc = &sleepq_chains[i];
    sc->sc_lock = SPIN_INITIALIZER(0);
    TAILQ_INIT(&sc->sc_queues);
  }

  /* Thread Zero has no sleepqueue, add it here. */
  thread_self()->td_sleepqueue = sleepq_alloc();
}

static POOL_DEFINE(P_SLEEPQ, "sleepq", sizeof(sleepq_t));

sleepq_t *sleepq_alloc(void) {
  sleepq_t *sq = pool_alloc(P_SLEEPQ, M_ZERO);
  sq_ctor(sq);
  return sq;
}

void sleepq_destroy(sleepq_t *sq) {
  pool_free(P_SLEEPQ, sq);
}

/*! \brief Lookup the sleep queue associated with \a wchan.
 *
 * \return NULL if no queue is found
 *
 * \warning returned sleep queue is locked!
 */
static sleepq_t *sq_lookup(sleepq_chain_t *sc, void *wchan) {
  assert(sc_owned(sc));

  sleepq_t *sq;
  TAILQ_FOREACH (sq, &sc->sc_queues, sq_entry) {
    if (sq->sq_wchan == wchan) {
      sq_acquire(sq);
      return sq;
    }
  }

  return NULL;
}

static void sq_enter(thread_t *td, void *wchan, const void *waitpt,
                     sleep_t sleep) {
  klog("Thread %ld goes to sleep on %p at pc=%p", td->td_tid, wchan, waitpt);

  assert(td->td_wchan == NULL);
  assert(td->td_waitpt == NULL);
  assert(td->td_sleepqueue != NULL);

  sleepq_t *td_sq = td->td_sleepqueue;

  assert(TAILQ_EMPTY(&td_sq->sq_blocked));
  assert(TAILQ_EMPTY(&td_sq->sq_free));
  assert(td_sq->sq_nblocked == 0);

  sleepq_chain_t *sc = sc_acquire(wchan);
  sleepq_t *sq = sq_lookup(sc, wchan);

  if (sq == NULL) {
    /* No sleep queue for this waiting channel.
     * We take current thread's sleep queue and use it for that purpose. */
    sq = td_sq;
    sq_acquire(sq);
    sq->sq_wchan = wchan;
    TAILQ_INSERT_HEAD(&sc->sc_queues, sq, sq_entry);
  } else {
    /* A sleep queue for the waiting channel already exists!
     * We add this thread's sleepqueue to the free list. */
    TAILQ_INSERT_HEAD(&sq->sq_free, td->td_sleepqueue, sq_entry);
  }

  TAILQ_INSERT_TAIL(&sq->sq_blocked, td, td_sleepq);
  sq->sq_nblocked++;

  WITH_SPIN_LOCK (&td->td_spin) {
    td->td_wchan = wchan;
    td->td_waitpt = waitpt;
    td->td_sleepqueue = NULL;

    /* The thread is about to fall asleep, but it still needs to reach
     * sched_switch - it may get interrupted on the way, so mark our intent. */
    td->td_flags |= TDF_SLEEPY;

    if (sleep >= SLP_INTR)
      td->td_flags |= TDF_SLPINTR;
    if (sleep >= SLP_TIMED)
      td->td_flags |= TDF_SLPTIMED;
  }

  sq_release(sq);
  sc_release(sc);
}

static void sq_leave(thread_t *td, sleepq_chain_t *sc, sleepq_t *sq) {
  klog("Wakeup thread %ld from %p at pc=%p", td->td_tid, td->td_wchan,
       td->td_waitpt);

  assert(sc_owned(sc));
  assert(sq_owned(sq));

  assert(td->td_wchan != NULL);
  assert(td->td_sleepqueue == NULL);
  assert(sq->sq_nblocked >= 1);

  TAILQ_REMOVE(&sq->sq_blocked, td, td_sleepq);
  sq->sq_nblocked--;

  if (TAILQ_EMPTY(&sq->sq_blocked)) {
    /* If it was the last thread on this sleep queue, \a td gets it. */
    assert(sq->sq_nblocked == 0);
    sq->sq_wchan = NULL;
    /* Remove the sleep queue from the chain. */
    TAILQ_REMOVE(&sc->sc_queues, sq, sq_entry);
  } else {
    /* Otherwise \a td gets a sleep queue from the free list. */
    assert(sq->sq_nblocked > 0);
    assert(!TAILQ_EMPTY(&sq->sq_free));
    sq = TAILQ_FIRST(&sq->sq_free);
    /* Remove the sleep queue from the free list. */
    TAILQ_REMOVE(&sq->sq_free, sq, sq_entry);
  }

  WITH_SPIN_LOCK (&td->td_spin) {
    td->td_wchan = NULL;
    td->td_waitpt = NULL;
    td->td_sleepqueue = sq;
  }
}

static inline bool _sleepq_interrupted_early(thread_t *td, sleep_t sleep) {
  return (td->td_flags & TDF_NEEDSIGCHK) != 0 && sleep == SLP_INTR;
}

static int _sleepq_wait(void *wchan, const void *waitpt, sleep_t sleep) {
  thread_t *td = thread_self();
  int status = 0;

  /* If there are pending signals, interrupt the sleep immediately. */
  WITH_SPIN_LOCK (&td->td_spin) {
    if (_sleepq_interrupted_early(td, sleep))
      return EINTR;
  }

  if (waitpt == NULL)
    waitpt = __caller(0);

  sq_enter(td, wchan, waitpt, sleep);

  /* The code can be interrupted in here.
   * A race is avoided by clever use of TDF_SLEEPY flag.
   * We can also get a signal in here -- interrupt early if we got one.
   * The first signal check is an optimization that saves us the call
   * to sq_enter. */

  WITH_SPIN_LOCK (&td->td_spin) {
    if (td->td_flags & TDF_SLEEPY) {
      td->td_flags &= ~TDF_SLEEPY;
      if (_sleepq_interrupted_early(td, sleep)) {
        td->td_flags &= ~(TDF_SLPINTR | TDF_SLPTIMED);
        status = EINTR;
      } else {
        td->td_state = TDS_SLEEPING;
        sched_switch();
      }
    }
    /* After wakeup, only one of the following flags may be set:
     *  - TDF_SLPINTR if sleep was aborted,
     *  - TDF_SLPTIMED if sleep has timed out. */
    if (td->td_flags & TDF_SLPINTR) {
      td->td_flags &= ~TDF_SLPINTR;
      status = EINTR;
    } else if (td->td_flags & TDF_SLPTIMED) {
      td->td_flags &= ~TDF_SLPTIMED;
      status = ETIMEDOUT;
    }
  }

  return status;
}

/* Remove a thread from the sleep queue and resume it. */
static bool sq_wakeup(thread_t *td, sleepq_chain_t *sc, sleepq_t *sq,
                      int wakeup) {
  /* Only sq_enter sets TDF_SLP... flags and it holds the same sleepq spinlock
   * as sq_wakeup. Hence it's safe to read flags without holding thread's
   * spinlock. */

  /* Do not try to abort thread's sleep if it's not prepared for that. */
  if ((wakeup == EINTR) && !(td->td_flags & TDF_SLPINTR))
    return false;
  if ((wakeup == ETIMEDOUT) && !(td->td_flags & TDF_SLPTIMED))
    return false;

  sq_leave(td, sc, sq);

  WITH_SPIN_LOCK (&td->td_spin) {
    /* Clear TDF_SLPINTR flag if thread's sleep was not aborted. */
    if (wakeup != EINTR)
      td->td_flags &= ~TDF_SLPINTR;
    if (wakeup != ETIMEDOUT)
      td->td_flags &= ~TDF_SLPTIMED;
    /* Do not try to wake up a thread that is sleepy but did not fall asleep! */
    if (td->td_flags & TDF_SLEEPY) {
      td->td_flags &= ~TDF_SLEEPY;
    } else {
      sched_wakeup(td, 0);
    }
  }
  return true;
}

bool sleepq_signal(void *wchan) {
  sleepq_chain_t *sc = sc_acquire(wchan);
  sleepq_t *sq = sq_lookup(sc, wchan);

  if (sq == NULL) {
    sc_release(sc);
    return false;
  }

  thread_t *td, *best_td = TAILQ_FIRST(&sq->sq_blocked);
  TAILQ_FOREACH (td, &sq->sq_blocked, td_sleepq) {
    /* Search for thread with highest priority */
    if (prio_gt(td->td_prio, best_td->td_prio))
      best_td = td;
  }

  sq_wakeup(best_td, sc, sq, SLP_NORMAL);
  sq_release(sq);
  sc_release(sc);

  sched_maybe_preempt();
  return true;
}

bool sleepq_broadcast(void *wchan) {
  sleepq_chain_t *sc = sc_acquire(wchan);
  sleepq_t *sq = sq_lookup(sc, wchan);

  if (sq == NULL) {
    sc_release(sc);
    return false;
  }

  thread_t *td;
  TAILQ_FOREACH (td, &sq->sq_blocked, td_sleepq)
    sq_wakeup(td, sc, sq, SLP_NORMAL);
  sq_release(sq);
  sc_release(sc);

  sched_maybe_preempt();
  return true;
}

static bool _sleepq_abort(thread_t *td, int reason) {
  sleepq_chain_t *sc = sc_acquire(td->td_wchan);
  sleepq_t *sq = sq_lookup(sc, td->td_wchan);
  bool aborted = false;

  if (sq != NULL) {
    aborted = sq_wakeup(td, sc, sq, reason);
    sq_release(sq);
  }
  sc_release(sc);

  /* If we woke up higher priority thread, we should switch to it immediately.
   * This is useful if `_sleepq_abort` gets called in thread context and
   * preemption is enabled. */
  sched_maybe_preempt();
  return aborted;
}

bool sleepq_abort(thread_t *td) {
  return _sleepq_abort(td, EINTR);
}

void sleepq_wait(void *wchan, const void *waitpt) {
  int status = _sleepq_wait(wchan, waitpt, 0);
  assert(status == 0);
}

static void sq_timeout(thread_t *td) {
  _sleepq_abort(td, ETIMEDOUT);
}

int sleepq_wait_timed(void *wchan, const void *waitpt, systime_t timeout) {
  thread_t *td = thread_self();
  int status = 0;
  WITH_INTR_DISABLED {
    if (timeout > 0)
      callout_setup_relative(&td->td_slpcallout, timeout, (timeout_t)sq_timeout,
                             thread_self());
    status = _sleepq_wait(wchan, waitpt, timeout ? SLP_TIMED : SLP_INTR);
  }
  if (timeout > 0)
    callout_stop(&td->td_slpcallout);
  return status;
}

SYSINIT_ADD(sleepq, sleepq_init, DEPS("sched"));
