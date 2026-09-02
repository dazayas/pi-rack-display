/******
Demo for ssd1306 i2c driver for  Raspberry Pi
******/
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "st7735.h"
#include "rpiInfo.h"

/* lcd_display() takes a symbol of 0..3, and the four are NOT interchangeable:
   screen 0 is the only one that calls lcd_fill_screen() and draws the IP
   header, and screens 1-3 repaint just the band in the middle over whatever it
   left behind. So a rotation has to START at 0 -- which is what the alignment
   below anchors, and why joining mid-cycle is not allowed. */
#define PAGE_COUNT 4

/* How often to re-ask whether the clock has synchronised, while running
   unaligned because it had not. Not a config value: it is a retry interval for
   a case that should not last, and one popen every half minute is invisible
   next to a screen redraw. */
#define NTP_RECHECK_SECONDS 30.0

/* THE LOOP KEEPS TIME IN ONE OF TWO WAYS, and which is right depends on
   whether this host's wall clock can be trusted.

   ALIGNED (the default). Four Pis in one rack each start whenever their
   service happened to start, so their transitions land at random offsets from
   each other -- every display correct, the rack as a whole looking broken.
   The fix is not to synchronise the hosts WITH each other (no leader, no
   broker, no shared state) but to align each one independently against a
   reference they already share: the wall clock. Each waits for the next
   quarter-minute boundary, then runs a cycle of exactly UCTRONICS_CYCLE_SECONDS
   with each page drawn at an absolute instant computed from the clock. Every
   host lands on the same boundaries forever after with no communication at
   all, and one that reboots rejoins within a cycle.

   FREE-RUNNING (align_seconds: 0, or a clock that never synchronised). The
   original behaviour, kept because a host with no working time source is
   better off rotating than sitting on a placeholder: a fixed dwell, timed from
   the end of each draw.

   FIX (free-running dwell): the loop drew a screen then slept a flat two
   seconds, and the sleep was the only thing keeping time. Two problems
   followed.

   The rotation sped up whenever drawing did -- with the I2C bus at 400 kHz
   instead of the 100 kHz default, identical trays in one rack cycled at
   visibly different rates.

   And screens are not equally expensive. Only the CPU screen calls
   lcd_fill_screen(), repainting all 160x80 pixels before it draws anything, so
   it takes markedly longer to render than the other three. A screen is
   replaced the moment the next one starts drawing, so its readable time is the
   sleep MINUS its own draw -- and the CPU screen was visibly the shortest.

   The dwell now starts when the draw finishes, so every screen is fully
   readable for exactly UCTRONICS_DWELL_SECONDS (addon option: dwell_seconds)
   regardless of what it cost to render or how fast the bus is. */

/* Configuration comes from the environment so it can be changed without
   rebuilding: run.sh exports the addon's options, and a systemd unit sets the
   same names directly. Values are clamped rather than trusted -- a dwell of 0
   would repaint the display continuously, with nothing on screen to say why. */
static double env_seconds(const char *name, double def, double lo, double hi)
{
	const char *s = getenv(name);
	double v = (s && *s) ? atof(s) : def;

	if (v < lo) v = lo;
	if (v > hi) v = hi;

	return v;
}

static long long nsec_of(double seconds)
{
	return (long long)(seconds * 1000000000.0);
}

/* Seconds a screen stays readable when free-running. Ignored while aligned,
   where the slot is cycle / PAGE_COUNT instead. */
static long long dwell_nsec(void)
{
	return nsec_of(env_seconds("UCTRONICS_DWELL_SECONDS", 3.0, 0.5, 60.0));
}

/* One full rotation of all four screens, divided evenly between them: the
   default 15 s gives each screen 3.75 s. */
static long long cycle_nsec(void)
{
	return nsec_of(env_seconds("UCTRONICS_CYCLE_SECONDS", 15.0, 2.0, 3600.0));
}

/* The wall-clock grid a cycle starts on. 15 s means :00, :15, :30 and :45 of
   every minute -- epoch second zero was itself a minute boundary, so "epoch
   seconds divisible by 15" and "quarter-minute on the wall clock" name the
   same instants, for any interval that divides 60.

   Normally equal to the cycle: one cycle per boundary. Making it a separate
   value costs nothing and answers the case where a longer rotation should
   still re-pin to the clock periodically. 0 disables alignment entirely and
   free-runs on the dwell. */
static long long align_nsec(void)
{
	return nsec_of(env_seconds("UCTRONICS_ALIGN_SECONDS", 15.0, 0.0, 3600.0));
}

/* How far this host sits off the boundary. 0 -- all four transitioning
   together -- is the point of the exercise, but the same mechanism gives a
   deliberate stagger, a wave down the rack, by setting each host to its rack
   position times a fraction of the cycle. It is a config value while it is 0
   precisely so that trying the wave is an options change and not a patch. */
static long long align_offset_nsec(void)
{
	return nsec_of(env_seconds("UCTRONICS_ALIGN_OFFSET_SECONDS", 0.0, 0.0, 3600.0));
}

/* A draw that starts this far past its boundary is logged. It is the signal
   for a host under load or with a broken time source, and without it the one
   display that is visibly out of step looks like failing hardware. 0 silences
   it. UCTRONICS_LATE_WARN_MS, in milliseconds because that is the scale of the
   thing being measured. */
static long long late_warn_nsec(void)
{
	return nsec_of(env_seconds("UCTRONICS_LATE_WARN_MS", 200.0, 0.0, 60000.0) / 1000.0);
}

/* How long to wait for the clock before giving up and free-running. */
static double ntp_wait_seconds(void)
{
	return env_seconds("UCTRONICS_NTP_WAIT_SECONDS", 120.0, 0.0, 3600.0);
}

static long long realtime_nsec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Sleep until an absolute instant on the wall clock.

   CLOCK_REALTIME deliberately, where the dwell below uses CLOCK_MONOTONIC:
   alignment is only meaningful against the clock the other three Pis also
   read, and CLOCK_MONOTONIC's zero is different on every host and moves on
   every reboot. The cost is that a step change in the clock moves the target
   under us -- which is exactly why the loop waits for NTP before it starts,
   since the step that matters is the one at boot. */
static void sleep_until_realtime(long long when)
{
	struct timespec ts;

	ts.tv_sec  = (time_t)(when / 1000000000LL);
	ts.tv_nsec = (long)(when % 1000000000LL);

	/* clock_nanosleep() returns the error rather than setting errno, and under
	   TIMER_ABSTIME an interrupted sleep is resumed by asking for the same
	   absolute instant again -- there is no remaining time to carry. */
	while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL) == EINTR)
	{
		;
	}
}

/* The next instant a page should be drawn, and which page that is.

   NEVER sleep(slot). A relative sleep adds the draw time and the scheduler's
   jitter to every pass, so four hosts drift apart over hours -- slowly enough
   to look like the idea failed rather than the implementation. Recomputing an
   absolute target from the clock every iteration self-corrects instead: a late
   draw is followed by a shorter sleep and the error does not accumulate.

   A draw must fit INSIDE its slot rather than extending it. Because the target
   is always the next boundary STRICTLY AFTER now, a draw that overruns simply
   loses the slot it ran into: one dropped page, and the rotation stays on the
   boundary. Catching up would leave this host permanently offset from the
   other three, which is the failure this whole exercise exists to prevent.

   The page index is derived from the clock as well, rather than from a
   counter, so a dropped page cannot knock the rotation out of phase either --
   at any given instant all four hosts are showing the same page number.

   Callers must pass align > 0. */
static void next_page(long long now, long long align, long long cycle,
                      long long offset, int cycle_start_only,
                      long long *target, uint8_t *page)
{
	long long slot   = cycle / PAGE_COUNT;
	long long anchor = ((now - offset) / align) * align + offset;
	long long t;

	if (cycle_start_only)
	{
		/* Joining the rotation: wait for a cycle START, because page 0 is the
		   only one that repaints the whole screen. Entering mid-cycle would
		   leave the placeholder's text framing a band of live statistics. */
		t = anchor + align;
	}
	else
	{
		t = anchor + ((now - anchor) / slot + 1) * slot;
		if (t >= anchor + align)
		{
			/* The grid re-anchors here; the next cycle starts. */
			t = anchor + align;
		}
	}

	*target = t;
	*page   = (uint8_t)((((t - offset) % align) / slot) % PAGE_COUNT);
}

/* Wait for the clock to be worth aligning to.

   These Pis have no RTC. Until systemd-timesyncd corrects it, the clock at
   boot reads roughly whenever the filesystem was last written -- align to that
   and the rotation is pinned to a fiction, then jumps the moment the
   correction lands. This fleet has been bitten by an unsynchronised first-boot
   clock before; homelab-bootstrap's docs/raspberry-pi.md has the account.

   Three outcomes. Synchronised: align. Nothing to ask (no timedatectl, which
   is the addon container -- no systemd in there): say so and align anyway,
   because there is no answer coming and Home Assistant OS keeps its own clock.
   Still unsynchronised after the timeout -- a Pi whose network is not up yet:
   return 0, and let the caller free-run and retry rather than leave a
   placeholder on screen indefinitely. */
static int wait_for_clock(double timeout_seconds)
{
	double waited = 0.0;
	int state;

	for (;;)
	{
		state = get_ntp_synchronised();
		if (state > 0)
		{
			return 1;
		}
		if (state < 0)
		{
			fprintf(stderr, "display: cannot query time sync (no timedatectl); aligning anyway\n");
			return 1;
		}
		if (waited >= timeout_seconds)
		{
			return 0;
		}
		sleep(1);
		waited += 1.0;
	}
}

int main(void)
{
	uint8_t symbol = 0;
	long long dwell  = dwell_nsec();
	long long align  = align_nsec();
	long long cycle  = cycle_nsec();
	long long offset = align_offset_nsec();
	long long warn   = late_warn_nsec();
	long long last_target = 0;
	int aligned = 0;
	int joining = 1;
	double unaligned_for = 0.0;
	struct timespec next;

	if(lcd_begin())      //LCD Screen initialization
	{
		/* FIX: this returned 0, so failing to open the I2C bus exited
		   SUCCESSFULLY -- under systemd the journal reads "Deactivated
		   successfully" while the unit restarts forever, and under the addon
		   the container just exits quietly. */
		fprintf(stderr, "lcd_begin failed\n");
		return 1;
	}

	if (align > 0)
	{
		/* An offset is a position on the grid, so anything beyond one interval
		   is the same position again. */
		offset %= align;

		if (align < cycle || align % cycle != 0)
		{
			/* The grid re-anchors on every boundary, so an interval that is
			   not a whole number of cycles truncates the last rotation before
			   it. Legal, and occasionally what someone wants; never accidental
			   in a way worth staying quiet about. */
			fprintf(stderr, "display: align_seconds (%.2f) is not a whole number of "
			                "cycle_seconds (%.2f); the last rotation before each boundary "
			                "will be cut short\n",
			        (double)align / 1000000000.0, (double)cycle / 1000000000.0);
		}

		/* Something on screen for the wait, which is up to one interval. A
		   blank panel reads as a dead display, and starting a real cycle that
		   is about to be interrupted by the first boundary would visibly
		   stutter. The host's name is the one thing a rack of four identical
		   displays can never show too often. */
		lcd_display_placeholder("SYNC");

		aligned = wait_for_clock(ntp_wait_seconds());
		if (!aligned)
		{
			fprintf(stderr, "display: clock still unsynchronised; free-running and retrying "
			                "every %.0fs\n", NTP_RECHECK_SECONDS);
		}
	}

	while(1)
	{
		if (aligned)
		{
			long long target, lag;
			uint8_t page;

			next_page(realtime_nsec(), align, cycle, offset, joining, &target, &page);
			sleep_until_realtime(target);

			lag = realtime_nsec() - target;
			if (warn > 0 && lag > warn)
			{
				fprintf(stderr, "display: page %u started %.0f ms after its boundary\n",
				        (unsigned)page, (double)lag / 1000000.0);
			}
			if (!joining && target > last_target + cycle / PAGE_COUNT)
			{
				/* The only way to miss a boundary is to still have been
				   drawing when it passed. Skipping is the intended response
				   (see next_page), but a host doing it repeatedly is a host
				   whose I2C bus or CPU is not keeping up. Measured in time
				   rather than in page numbers, so that a cycle legitimately
				   cut short by the boundary is not reported as an overrun. */
				fprintf(stderr, "display: draw overran its slot by %.0f ms; skipped to page %u\n",
				        (double)(target - last_target - cycle / PAGE_COUNT) / 1000000.0,
				        (unsigned)page);
			}

			lcd_display(page);
			last_target = target;
			/* Kept in step with the aligned page so that if this host ever
			   drops back to free-running, the rotation carries on from where
			   the screen already is instead of jumping. */
			symbol      = page;
			joining     = 0;
		}
		else
		{
			lcd_display(symbol);

			/* Timed from HERE, after the draw, so the dwell is the time the
			   screen is actually readable rather than that plus however long
			   it took. */
			clock_gettime(CLOCK_MONOTONIC, &next);
			next.tv_sec  += dwell / 1000000000LL;
			next.tv_nsec += dwell % 1000000000LL;
			if (next.tv_nsec >= 1000000000L)
			{
				next.tv_nsec -= 1000000000L;
				next.tv_sec++;
			}
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

			symbol++;
			if(symbol==PAGE_COUNT)
			{
				symbol=0;
			}

			/* Alignment was wanted but the clock was not ready. Keep asking,
			   so a Pi that booted before its network came up joins the rack on
			   its own rather than waiting for someone to notice. */
			if (align > 0)
			{
				unaligned_for += (double)dwell / 1000000000.0;
				if (unaligned_for >= NTP_RECHECK_SECONDS)
				{
					unaligned_for = 0.0;
					if (get_ntp_synchronised() != 0)
					{
						fprintf(stderr, "display: clock synchronised; joining the aligned rotation\n");
						lcd_display_placeholder("SYNC");
						aligned = 1;
						joining = 1;
					}
				}
			}
		}
	}
	return 0;
}
