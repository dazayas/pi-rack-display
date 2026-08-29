/******
Demo for ssd1306 i2c driver for  Raspberry Pi 
******/
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "st7735.h"

/* FIX: the loop drew a screen then slept a flat two seconds, and the sleep was
   the only thing keeping time. Two problems followed.

   The rotation sped up whenever drawing did -- with the I2C bus at 400 kHz
   instead of the 100 kHz default, identical trays in one rack cycled at
   visibly different rates.

   And screens are not equally expensive. Only the CPU screen calls
   lcd_fill_screen(), repainting all 160x80 pixels before it draws anything, so
   it takes markedly longer to render than the other three. A screen is
   replaced the moment the next one starts drawing, so its readable time is the
   sleep MINUS its own draw -- and the CPU screen was visibly the shortest.

   The sleep now starts when the draw finishes, so every screen is fully
   readable for exactly UCTRONICS_DWELL_SECONDS (addon option: dwell_seconds)
   regardless of what it cost to render or how fast the bus is. The cycle time
   still varies a little between machines, by the sum of their draw times --
   that is the transition, and it is the part the eye does not measure. */

int main(void) 
{
	uint8_t symbol = 0;
	long period = dwell_nsec();
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

	while(1)
	{
		lcd_display(symbol);

		/* Timed from HERE, after the draw, so the dwell is the time the screen
		   is actually readable rather than that plus however long it took. */
		clock_gettime(CLOCK_MONOTONIC, &next);
		next.tv_sec  += period / 1000000000L;
		next.tv_nsec += period % 1000000000L;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		symbol++;
		if(symbol==4)
		{
			symbol=0;
		}
	}
	return 0;
}
