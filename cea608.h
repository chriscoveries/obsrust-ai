/* CEA-608 roll-up emitter (port of rust/obs-captions).
 * Mirrors the proven spike: EDM + RU2/RU3 + bottom-row PAC init, words
 * wrapped at column 32 with CR, control commands repeated, odd parity
 * baked into every pair. Unmappable chars reject the word, not truncate. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define C608_COLS 32
#define C608_ROWS 15
#define C608_CAP 4096 /* ~1.5 h of pairs at 60/s */

struct c608 {
	uint16_t pairs[C608_CAP];
	int head, n;
	uint32_t slot, step, fps;
	int col;
	double last_ts;
};

void c608_init(struct c608 *r, uint32_t fps, int rows);
int c608_word(struct c608 *r, double ts, const char *word);
bool c608_pop(struct c608 *r, uint16_t *out);
