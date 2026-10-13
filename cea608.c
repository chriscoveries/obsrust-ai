#include "cea608.h"
#include <math.h>
#include <string.h>

/* ---------------- CEA-608 roll-up emitter (port of rust/obs-captions) ----
 * Mirrors the proven spike: EDM + RU2/RU3 + bottom-row PAC init, words
 * wrapped at column 32 with CR, control commands repeated, odd parity
 * baked into every pair. Unmappable chars reject the word, not truncate. */

static uint8_t c608_parity_byte(uint8_t d)
{
	d &= 0x7f;
	return d | ((__builtin_popcount(d) & 1) ? 0 : 0x80);
}
static uint16_t c608_parity(uint16_t d)
{
	return ((uint16_t)c608_parity_byte(d >> 8) << 8) |
	       c608_parity_byte(d);
}

/* basic North American glyphs, codes 0x20..0x7f (codepoints) */
static const uint32_t c608_na[96] = {
	' ', '!', '"', '#', '$', '%', '&', 0x2019, '(', ')', 0xE1, '+', ',',
	'-', '.', '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	':', ';', '<', '=', '>', '?', '@', 'A', 'B', 'C', 'D', 'E', 'F',
	'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
	'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', 0xE9, ']', 0xED, 0xF3,
	0xFA, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
	'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',
	'z', 0xE7, 0xF7, 0xD1, 0xF1, 0x2588,
};

static int c608_na_code(uint32_t cp)
{
	for (int i = 0; i < 96; i++)
		if (c608_na[i] == cp)
			return 0x20 + i;
	return -1;
}

/* decode one UTF-8 codepoint; returns bytes consumed (0 = invalid) */
static int c608_utf8(const char *s, uint32_t *cp)
{
	unsigned char c = (unsigned char)*s;
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xe0) == 0xc0) { *cp = c & 0x1f; goto seq; }
	if ((c & 0xf0) == 0xe0) { *cp = c & 0x0f; goto seq; }
	if ((c & 0xf8) == 0xf0) { *cp = c & 0x07; goto seq; }
	return 0;
seq:;
	int n = c < 0xf0 ? (c < 0xe0 ? 1 : 2) : 3;
	for (int i = 1; i <= n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		*cp = (*cp << 6) | (s[i] & 0x3f);
	}
	return n + 1;
}

static void c608_put(struct c608 *r, uint16_t data, uint32_t due)
{
	if (r->n >= C608_CAP)
		return;
	if (r->slot < due)
		r->slot = due;
	r->slot += (r->step - r->slot % r->step) % r->step;
	r->pairs[(r->head + r->n) % C608_CAP] = data;
	r->n++;
	r->slot += r->step;
}

static void c608_control(struct c608 *r, uint16_t cmd, uint32_t due)
{
	uint16_t code = c608_parity(cmd);
	c608_put(r, code, due);
	c608_put(r, code, due); /* controls are repeated */
}

static void c608_text(struct c608 *r, const char *s, size_t len, uint32_t due)
{
	/* caller guarantees mappable; pack two glyphs per pair */
	for (size_t i = 0; i < len;) {
		uint32_t a = 0, b = 0;
		size_t na = c608_utf8(s + i, &a);
		i += na;
		uint16_t code;
		r->col++;
		if (i < len) {
			size_t nb = c608_utf8(s + i, &b);
			i += nb;
			code = c608_parity(
				(c608_na_code(a) << 8) | c608_na_code(b));
			r->col++;
		} else {
			code = c608_parity(c608_na_code(a) << 8);
		}
		c608_put(r, code, due);
	}
}

static const uint16_t c608_row_map[16] = {2, 3, 4, 5, 10, 11, 12, 13,
					  14, 15, 0, 6, 7, 8, 9, 1};

static uint16_t c608_pac(int row, int col, bool underline)
{
	row = c608_row_map[row & 0x0f];
	uint16_t x = 0x10 | (uint16_t)(col / 4);
	return c608_parity(0x1040 | ((row << 7) & 0x0700) |
			   ((row << 5) & 0x0020)) |
	       ((x << 1) & 0x001e) | (underline ? 1 : 0);
}

void c608_init(struct c608 *r, uint32_t fps, int rows)
{
	memset(r, 0, sizeof(*r));
	r->fps = fps;
	r->step = fps == 60 ? 2 : 1;
	r->last_ts = -1.0;
	c608_control(r, 0x142c, 0);          /* EDM */
	c608_control(r, rows == 3 ? 0x1426 : 0x1425, 0); /* RU3/RU2 */
	uint16_t pac = c608_pac(C608_ROWS - 1, 0, false);
	c608_put(r, pac, 0);
	c608_put(r, pac, 0);
}

/* -1 = rejected (unmappable char / bad ts), 0 = queued */
int c608_word(struct c608 *r, double ts, const char *word)
{
	if (!isfinite(ts) || ts < 0.0 || ts < r->last_ts)
		return -1;
	/* mappability check over the whole word first (reject, not truncate) */
	uint32_t cps[128];
	size_t ncps = 0;
	const char *p = word;
	for (; *p && ncps < 128;) {
		uint32_t cp;
		int n = c608_utf8(p, &cp);
		if (n == 0 || c608_na_code(cp) < 0)
			return -1;
		cps[ncps++] = cp;
		p += n;
	}
	if (*p)
		return -1; /* exceeds the pre-check cap: reject, don't emit unchecked */
	r->last_ts = ts;
	uint32_t due = (uint32_t)((ts * r->fps - 1e-9) + 1);
	size_t n = strlen(word);
	const char *s = word;
	if (r->col != 0 && r->col + 1 + (int)ncps > C608_COLS) {
		c608_control(r, 0x142d, due); /* CR */
		r->col = 0;
	}
	if (r->col != 0)
		c608_text(r, " ", 1, due);
	while (n > 0) {
		/* count chars that fit this row (utf8-safe via codepoints) */
		size_t fit = 0, bytes = 0;
		for (const char *p = s; *p && fit < (size_t)(C608_COLS - r->col);) {
			uint32_t cp;
			int c = c608_utf8(p, &cp);
			if (!c)
				break;
			p += c;
			bytes += c;
			fit++;
		}
		c608_text(r, s, bytes, due);
		s += bytes;
		n -= bytes;
		if (n > 0) {
			c608_control(r, 0x142d, due);
			r->col = 0;
		}
	}
	return 0;
}

/* oldest scheduled pair, or false */
bool c608_pop(struct c608 *r, uint16_t *out)
{
	if (r->n <= 0)
		return false;
	*out = r->pairs[r->head];
	r->head = (r->head + 1) % C608_CAP;
	r->n--;
	return true;
}
