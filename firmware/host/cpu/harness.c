/* Bare-metal harness for counting the engine's cost on a Cortex-M4F (run under cpu_count.py).
 * The host writes the voices into `cfg` before starting; markers bracket each render. */
#include <stdint.h>
#include <string.h>
#include "glag_engine.h"

volatile uint8_t cfg_voices[4] = {0, 1, 2, 7};
volatile uint32_t cfg_blocks = 500;
volatile uint8_t cfg_mute = 0x80;
volatile uint8_t cfg_stress = 0x80;
volatile uint16_t cfg_dist = 0xFFFF;      /* type | amount << 4 | post << 8; 0xFFFF: none */      /* 0x81: every channel at full activity and length */           /* bit c: channel c at level 0 */
volatile uint8_t cfg_piece = 0xFF;       /* 0xFF: piece 0 with cfg_voices in it (the table's way); else that piece as it plays, macros and all */
volatile uint32_t mark;                  /* the host watches writes here */
static int16_t blk[192];

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n && ((uintptr_t)p & 3)) { *p++ = (uint8_t)c; n--; } uint32_t w = (uint8_t)c * 0x01010101u; while (n >= 4) { *(uint32_t *)p = w; p += 4; n -= 4; } while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }

int main(void)
{
	gl_init(1234);
	gl_patch p;
	const int whole = cfg_piece != 0xFF;
	gl_piece(whole ? cfg_piece : 0, &p);
#ifdef ALTV
	gl_alt(ALTV, ALTM);
#endif
#ifdef CHOSHARE
	gl_cho_share(CHOSHARE);
#endif
#ifdef RVCUT
	gl_rv_cut(RVCUT);
#endif
#ifdef DROPCAP
	gl_drop_cap(DROPCAP);
#endif
#ifdef THIN
	gl_thin(THIN, THIN_THR);
#endif
#ifdef STRTHIN
	gl_str_thin(STRTHIN);
#endif
#ifdef KCHEAP
	gl_kit_cheap(KCHEAP);
#endif
#ifdef KCOL
	gl_kit_color(KCOL);
#endif
#ifdef COMP
	gl_world(GW_COMP, 1.0f);
#endif
	if (!whole)
		for (int c = 0; c < 4; c++)
			p.voice[c] = cfg_voices[c];
	if (cfg_stress & 1)
		for (int c = 0; c < 4; c++)
			p.ch[c][CP_ACTIVITY] = p.ch[c][CP_LENGTH] = 1.0f;
#ifdef KCHAR
	for (int c = 0; c < 4; c++)
		p.ch[c][CP_CHAR] = KCHAR;       /* e.g. the kit's split: every lane at 1 */
#endif
	for (int c = 0; c < 4; c++)
		if (cfg_mute & (1u << c))
			p.ch[c][CP_LEVEL] = 0.0f;
#ifdef RVTYPE
	p.space[GL_SP_REVERB] = RVTYPE;   /* one of the four reverbs */
#endif
#ifdef KTUNE
	p.space[GL_SP_KIT] = KTUNE;      /* one of the Kit's tunings */
#endif
	gl_post_patch(&p);
	if (whole) {                          /* the piece's own channel macros, as the renderer starts them */
		uint8_t st[4];
		float per;
		gl_piece_macro(cfg_piece, st, &per);
		for (int c = 0; c < 4; c++)
			gl_macro(c, (float)st[c] / 4.0f, per);
	}
	if (cfg_dist != 0xFFFF)
		gl_dist(cfg_dist & 15, (cfg_dist >> 4) & 15, (cfg_dist >> 8) & 1);
	for (uint32_t b = 0; b < cfg_blocks; b++) {
		mark = 1;
		gl_render(blk, 96, 0.8f);
		mark = 2;
	}
	mark = 3;
	for (;;) {}
}

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
void reset(void)
{
	uint32_t *s = &_sidata, *d = &_sdata;
	while (d < &_edata) *d++ = *s++;
	for (d = &_sbss; d < &_ebss;) *d++ = 0;
	main();
}
__attribute__((section(".vectors"))) const void *vectors[2] = {&_estack, (void *)reset};
