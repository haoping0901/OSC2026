#ifndef __TIMER_H__
#define __TIMER_H__

#include "types.h"

/*
 * timer_init() – Enable the S-mode core timer interrupt.
 *
 * Reads the timebase-frequency from the DTB, enables both the
 * per-source (sie.STIE) and global (sstatus.SIE) interrupt switches.
 * The hardware timer itself is left un-armed until the first call
 * to add_timer(); with an empty timer queue there is nothing to fire.
 */
void timer_init(void);

/*
 * timer_handle_interrupt() – S-mode timer IRQ bottom half.
 *
 * Drains every timer node whose expire_tick has already passed (one
 * IRQ may service several due to dispatch latency), invokes their
 * callbacks, then reprograms the hardware timer to the new queue
 * head. When the queue is empty the timer is pushed infinitely far
 * into the future so no spurious IRQ follows.
 */
void timer_handle_interrupt(void);

/*
 * add_timer() – Register a one-shot callback fired @sec seconds from now.
 *
 * Allocates a timer node, inserts it into the queue ordered by absolute
 * expire time, and reprograms the hardware timer when the new node
 * becomes the earliest. Non-blocking. Silently drops the request on
 * allocation failure.
 *
 * @callback: function invoked in IRQ context when the timer fires
 * @arg:      opaque pointer handed verbatim to @callback
 * @sec:      delay in whole seconds (must be > 0)
 */
void add_timer(void (*callback)(void *), void *arg, int sec);

/*
 * Tick-conversion helpers – exposed so shell callbacks can translate
 * absolute ticks back into seconds for human-readable output.
 */
uint64_t timer_get_timebase_freq(void);
uint64_t timer_read_ticks(void);

#endif /* __TIMER_H__ */
