/* dbg.h - machine readable debug telemetry.
 *
 * When TELEMETRY is defined, a strip of black/white cells is drawn over the
 * frame.  Each row is one 32 bit value (white cell = 1) and
 * tools/decode_telemetry.py reads the cells back out of a screenshot, so an
 * emulator capture can be checked with exact numbers instead of eyeballing.
 */
#ifndef DBG_H_
#define DBG_H_

#include <gccore.h>

#define DBG_ROWS 38

void dbg_draw_values(const u32 *values, int count);

#endif
