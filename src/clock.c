/*
 * Copyright (c) 2002 Jon Atkins http://www.jonatkins.com/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "memory.h"

#include "clock.h"

#include "decode_msf.h"
#include "decode_dcf77.h"
#include "decode_wwvb.h"

#include "shm.h"
#include "logger.h"
#include "settings.h"

static clkInfoT* clkListHead;

void clk_dump_data(const clkInfoT* clock) {
    int i;

    loggerf(LOGGER_TRACE, "data(%d): ", clock->numdata);
    for (i = 0; i < clock->numdata; i++) {
        loggerf(LOGGER_TRACE, "%d,", clock->data[i]);
    }
    loggerf(LOGGER_TRACE, "\n");
}

clkInfoT* clk_create(int inverted, int shmunit, time_f fudgeoffset, int clocktype) {
    clkInfoT* clkinfo;

    clkinfo = safe_mallocz(sizeof(clkInfoT));
    clkinfo->next = clkListHead;
    clkListHead = clkinfo;

    clkinfo->inverted = inverted;
    clkinfo->fudgeoffset = fudgeoffset;

    clkinfo->numdata = 0;
    clkinfo->clocktype = clocktype;

    if (!debugLevel) {
        clkinfo->shm = shm_create(shmunit);
    }

    return clkinfo;
}

void clk_data_clear(clkInfoT* clock) {
    clock->numdata = 0;
}

int clk_pulse_length(time_f timef, int clocktype) {
    //only detect short pulses...
    if (timef > 2.0) {
        return -1;
    }

    //pulse/clear lengths for each radio clock
    time_f* lengths;

    switch (clocktype) {
        case CLOCKTYPE_DCF77:
            //  (note: these last 2 are to handle the missing second 59)
            lengths = (time_f[]) {0.1, 0.2, 0.8, 0.9, 1.8, 1.9, -1.0};
            break;
        case CLOCKTYPE_MSF:
            lengths = (time_f[]) {0.1, 0.2, 0.3, 0.5, 0.7, 0.8, 0.9, -1.0};
            break;
        case CLOCKTYPE_WWVB:
            lengths = (time_f[]) {0.2, 0.5, 0.8, -1.0};
            break;
    }

    int i;

    for (i = 0; lengths[i] > 0; i++) {
        if (timef > (lengths[i] - 0.040) && timef < (lengths[i] + 0.040)) {
            return (int) (lengths[i] * 10 + 0.5);
        }    //to convert to 10ths of a second
    }
    return -1;
}

void clk_process_status_change(clkInfoT* clock, int status, time_f timef) {
    time_f diff;
    int val;

    if (clock->inverted) {
        status = !status;
    }

    //	timersub ( tv, &clock->changetime, &diff );
    diff = timef - clock->changetime;

    if (!clock->status && status) {
        val = clk_pulse_length(diff, clock->clocktype);

        if (val < 0) {
            loggerf(LOGGER_TRACE, "warning: bad pulse length "TIMEF_FORMAT"\n", diff);

            clk_data_clear(clock);
        } else {
            //make sure we dont go off the end - unlikely but could happen with noise...
            if (clock->numdata >= 120) {
                clk_data_clear(clock);
            }

            if (clock->msf_skip_b && (val == 1)) {
                clock->msf_skip_b = 0;
                if (clock->numdata >= 1) {
                    clock->data[clock->numdata - 1] += 10;
                }
            } else {
                if (val == 5 && clock->clocktype == CLOCKTYPE_MSF)  //MSF minute marker...
                {
                    clk_dump_data(clock);
                    if (msf_decode(clock, clock->changetime) < 0) {
                        loggerf(LOGGER_DEBUG, "warning: failed to decode MSF time\n");
                    } else {
                        clk_send_time(clock);
                    }

                    clk_data_clear(clock);
                }
                if (val == 8 && clock->clocktype == CLOCKTYPE_WWVB && clock->numdata > 0 && clock->data[clock->numdata - 1] == 8) {
                    /*
                        WWVB's Minute marker is a pair of back-to-back 0.8 second pulses, where
                        the first one is the start of the new minute, and the previous one was the
                        end of the previous minute. Strangely, they send the On-Time-Marker, and
                        then the time.
                    */
                    clk_dump_data(clock);
                    if (wwvb_decode(clock, clock->changetime) < 0) {
                        loggerf(LOGGER_DEBUG, "warning: failed to decode WWVB time\n");
                    } else {
                        clk_send_time(clock);
                    }

                    clk_data_clear(clock);
                }

                clock->data[clock->numdata++] = val;
            }

            if (clock->numdata > 0) {
                loggerf(LOGGER_TRACE, "pulse end: length "TIMEF_FORMAT" - # Bits: %3d: Pulse Width (10ths): %d\n", diff, clock->numdata - 1,
                        clock->data[clock->numdata - 1]);
            }

        }

        clock->status = status;
        clock->changetime = timef;
    } else if (clock->status && !status) {
        loggerf(LOGGER_TRACE, "pulse start: at "TIMEF_FORMAT"\n", timef);

        val = clk_pulse_length(diff, clock->clocktype);

        if (val < 0) {
            loggerf(LOGGER_TRACE, "warning: bad clear length "TIMEF_FORMAT"\n", diff);

            clk_data_clear(clock);
        } else if ((clock->numdata > 1) && (clock->data[clock->numdata - 1] == 1) && (val == 1)) {
            //the MSF signal has a 2nd bit sometimes - flag it...
            clock->msf_skip_b = 1;
        } else if (val == 18 || val == 19) {
            clock->data[clock->numdata++] = 0;    //store the missing second 59 value

            clk_dump_data(clock);

            if (dcf77_decode(clock, timef) < 0) {
                loggerf(LOGGER_DEBUG, "Warning: failed to decode DCF77\n");
            } else {
                clk_send_time(clock);
            }

            clk_data_clear(clock);

        } else {
            //we have the start of a second here...

            //printf ( "\a" ); flush ( stdout );

            //TODO: PPS processing on this time
            //increment time by 1 second (if pulse is good - within 50 ms perhaps?)
            //and, keep a running average of the error from the current time
            clk_process_pps(clock, timef);

            //			clk_dump_PPS ( clock );
        }

        clock->status = status;
        clock->changetime = timef;
    }
    //else no change so ignore pulse (should never happen)
}

void clk_send_time(clkInfoT* clock) {
    time_f average, maxerr;

    if (clk_calculate_pps_average(clock, &average, &maxerr) < 0) {
        maxerr = 0.005;

        loggerf(LOGGER_DEBUG, "clock: radio time "TIMEF_FORMAT", pc time "TIMEF_FORMAT"\n", clock->radiotime, clock->pctime);

        if (!debugLevel) {
            shm_store(clock->shm, clock->radiotime, clock->pctime, maxerr, clock->radioleap);
        }
    } else {
        loggerf(LOGGER_DEBUG, "clock: radio time "TIMEF_FORMAT", average pctime "TIMEF_FORMAT", error +-"TIMEF_FORMAT"\n", clock->radiotime,
                clock->radiotime + average, maxerr);

        if (!debugLevel) {
            shm_store(clock->shm, clock->radiotime, clock->radiotime + average, maxerr, clock->radioleap);
        }
    }

}

void clk_process_pps(clkInfoT* clock, time_f timef) {
    //	time_f	average, maxerr;

    //cant process second pulses unless we have decoded the time...
    if (clock->radiotime == 0) {
        return;
    }

    clock->secondssincetime += 1.0;

    clock->ppslist[clock->ppsindex].pctime = timef;
    clock->ppslist[clock->ppsindex].radiotime = clock->radiotime + clock->secondssincetime;
    clock->ppsindex++;
    clock->ppsindex %= PPS_AVERAGE_COUNT;

    //	if ( clk_calculate_pps_average ( clock, &average, &maxerr ) >= 0 )
    //	{
    //
    //	}
}

static int sort_timef_compare(const void* a, const void* b) {
    time_f ta, tb;

    ta = *(time_f*) a;
    tb = *(time_f*) b;

    if (ta < tb) {
        return -1;
    } else if (ta > tb) {
        return +1;
    }
    return 0;
}

int clk_calculate_pps_average(clkInfoT* clock, time_f* paverage, time_f* pmaxerr) {
    int i;
    time_f err;
    time_f total_offset, average_offset;
    int total_count;
    time_f standard_deviation;
    time_f timediff[PPS_AVERAGE_COUNT] = {0.0};

    for (i = 0; i < PPS_AVERAGE_COUNT; i++) {
        if (clock->ppslist[i].pctime == 0 || clock->ppslist[i].radiotime == 0) {
            return -1;
        }

        err = clock->ppslist[i].pctime - clock->ppslist[i].radiotime;
        // If the time isn't close, don't bother tracking it...
        if (fabs(err) > 0.1) { // Within 100ms - more than this and ntpd will step the time soon
            return -1;
        }

        timediff[i] = err;
    }

    qsort(timediff, PPS_AVERAGE_COUNT, sizeof(time_f), sort_timef_compare);

    total_offset = 0;
    total_count = 0;
    for (i = PPS_AVERAGE_COUNT / 4; i < PPS_AVERAGE_COUNT * 3 / 4; i++) {
        err = timediff[i];

        total_offset += err;
        total_count++;

    }

    average_offset = total_offset / total_count;

    standard_deviation = 0;
    for (i = 0; i < PPS_AVERAGE_COUNT; i++) {
        if (clock->ppslist[i].pctime == 0 || clock->ppslist[i].radiotime == 0) {
            continue;
        }

        err = (clock->ppslist[i].pctime - clock->ppslist[i].radiotime) - average_offset;

        standard_deviation += err * err;
    }
    standard_deviation /= total_count;
    standard_deviation = sqrt(standard_deviation);

    *paverage = average_offset;
    *pmaxerr = standard_deviation;    //over50%   * 2.0;	//over 90% of the values should be in this range...

    return 0;
}

