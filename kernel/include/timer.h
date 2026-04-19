#ifndef __TIMER_H__
#define __TIMER_H__

/*
 * timer_init() – Enable the core timer interrupt.
 *
 * Reads the timebase frequency from the DTB, programs the first
 * timer interrupt for 2 seconds in the future, and enables both
 * the per-interrupt (sie.STIE) and global (sstatus.SIE) switches.
 */
void timer_init(void);

/*
 * timer_handle_interrupt() – S-mode timer interrupt handler.
 *
 * Prints the elapsed seconds since boot and reprograms the timer
 * for the next 2-second interval.
 */
void timer_handle_interrupt(void);

#endif /* __TIMER_H__ */
