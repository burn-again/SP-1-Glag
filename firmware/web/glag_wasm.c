/* SPDX-License-Identifier: MIT
 * The browser emulator's glue: the real engine and controls, compiled to WebAssembly.
 * The page's AudioWorklet calls wg_tick() and wg_render(); everything else is the firmware's
 * own code, unchanged. */
#include <stdint.h>
#include "glag_engine.h"
#include "glag_ui.h"
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

static gl_input in;
static gl_ui_out out;
static int16_t pcm[2 * 480];
static float fbuf[2 * 480];
/* the state the page shows: LEDs, page, values */
static struct {
	uint8_t led_t[4], led_p[4];
	uint8_t page, channel, piece, playing, frozen, save_slot, pad[2];
	float volume, speed, beat;
	float target[4];
	int32_t caught[4];
	float glow[4];
	uint8_t dist_type, dist_amt, dist_post, pad2;
	uint8_t macro[4];          /* offset 80 */
	float macro_period[4];     /* offset 84 */
	uint8_t reverb, shimmer, pad3[2];   /* offset 100 */
	uint8_t w_song, w_arp, w_pump, w_tex;   /* offset 104: Playworld */
	int8_t w_focus, pad4[3];               /* offset 108 */
	uint8_t demo, pad5[3];                 /* offset 112: demo mode */
	uint32_t demo_ms;                      /* offset 116 */
} st;

EXPORT void wg_init(uint32_t seed) { gl_init(seed); glui_init(); }
EXPORT void wg_input(float f1, float f2, float f3, float f4, uint32_t buttons)
{
	in.fader[0] = f1; in.fader[1] = f2; in.fader[2] = f3; in.fader[3] = f4;
	in.buttons = (uint16_t)buttons;
}
EXPORT void wg_tick(uint32_t now_ms)
{
	glui_tick(now_ms, &in, &out);
	for (int i = 0; i < 4; i++) {
		st.led_t[i] = out.led_t[i];
		st.led_p[i] = out.led_p[i];
		int c;
		st.target[i] = glui_target(i, &c);
		st.caught[i] = c;
		st.glow[i] = gl_glow[i];
	}
	st.page = out.page; st.channel = out.channel; st.piece = out.piece;
	st.playing = gl_playing; st.frozen = gl_frozen;
	if (out.save_slot)
		st.save_slot = out.save_slot;
	st.volume = out.volume; st.speed = gl_speed; st.beat = gl_beat;
	st.dist_type = out.dist_type; st.dist_amt = out.dist_amt; st.dist_post = out.dist_post;
	st.reverb = out.reverb;
	st.shimmer = out.shimmer;
	st.w_song = out.world_song; st.w_arp = out.world_arp; st.w_pump = out.world_pump; st.w_tex = out.world_tex;
	st.w_focus = out.world_focus;
	st.demo = out.demo; st.demo_ms = out.demo_ms;
	for (int i = 0; i < 4; i++) {
		st.macro[i] = out.macro[i];
		st.macro_period[i] = out.macro_period[i];
	}
}
EXPORT void *wg_state(void) { return &st; }
/* MIDI in: one message (channels 1-4 are Glag's channels; clock is counted on the page) */
EXPORT void wg_midi(int status, int d1, int d2)
{
	int kind = status & 0xF0, ch = status & 0x0F;
	if (ch > 3)
		return;
	if (kind == 0x90)
		gl_midi_note(ch, d1, d2);
	else if (kind == 0x80)
		gl_midi_note(ch, d1, 0);
	else if (kind == 0xB0)
		glui_midi_cc(ch, d1, d2);
	else if (kind == 0xC0)
		glui_midi_program(d1);
}
EXPORT void wg_tempo(float bpm) { glui_midi_tempo(bpm); }
EXPORT void *wg_patch(void) { return (void *)glui_patch(); }
EXPORT int wg_patch_size(void) { return (int)sizeof(gl_patch); }
/* n frames (a multiple of 48, at most 480), as interleaved float */
EXPORT float *wg_render(uint32_t n)
{
	gl_render(pcm, n, out.volume);
	for (uint32_t i = 0; i < 2u * n; i++)
		fbuf[i] = (float)pcm[i] * (1.0f / 32768.0f);
	return fbuf;
}
/* put a saved piece back (from the page's storage) and read one out */
EXPORT void *wg_scratch(void) { static gl_patch p; return &p; }
EXPORT void wg_set_saved(int piece, int valid) { glui_set_saved(piece, valid ? (const gl_patch *)wg_scratch() : 0); }
EXPORT void *wg_saved(int piece) { return (void *)glui_saved(piece); }
