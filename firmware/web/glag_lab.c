/* SPDX-License-Identifier: MIT
 * Glag Lab's glue: the engine alone (no SP-1 controls), compiled to WebAssembly, for the page
 * that tries the light voices and the performance effects. Morph pads are offsets on the patch,
 * glided in and out here so a press never jumps. */
#include <stdint.h>
#include <string.h>
#include "glag_engine.h"

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

static int16_t pcm[2 * 480];
static float fbuf[2 * 480];
static gl_patch base, now;
/* the morph's offsets: what the page asks for, and where the glide has got to (each channel's
 * controls, then the globals) */
static float m_tgt[GL_NCH * GL_NCP + GL_NGP], m_cur[GL_NCH * GL_NCP + GL_NGP];
static uint32_t m_tick;
static int cur_piece;
static float vol = 0.8f;

static float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static void post_now(void)
{
	now = base;
	for (int c = 0; c < GL_NCH; c++)
		for (int p = 0; p < GL_NCP; p++)
			now.ch[c][p] = clamp01(base.ch[c][p] + m_cur[c * GL_NCP + p]);
	for (int g = 0; g < GL_NGP; g++)
		if (g != GP_ROOT && g != GP_MODE)
			now.g[g] = clamp01(base.g[g] + m_cur[GL_NCH * GL_NCP + g]);
	gl_post_patch(&now);
}

EXPORT void lab_init(uint32_t seed) { gl_init(seed); }
EXPORT int lab_npieces(void) { return GL_NPIECES; }
EXPORT const char *lab_name(int i) { return gl_piece_name(i); }
EXPORT const char *lab_voice_name(int v) { return (unsigned)v < GV_COUNT ? GL_VOICE_NAME[v] : ""; }
EXPORT int lab_voice(int c) { return base.voice[c & 3]; }
EXPORT float lab_tempo(void) { return gl_tempo_bpm(base.g[GP_TEMPO]); }
EXPORT int lab_reverb(void) { return base.space[GL_SP_REVERB]; }
EXPORT float lab_cost(void) { return gl_cost_est(&base, 0, 0, 0); }
EXPORT void lab_piece(int i, uint32_t seed)
{
	cur_piece = i;
	gl_piece(i, &base);
	for (int k = 0; k < GL_NCH * GL_NCP + GL_NGP; k++)
		m_tgt[k] = m_cur[k] = 0.0f;
	post_now();
	gl_fresh(seed);
	uint8_t st[4];
	float per;
	gl_piece_macro(i, st, &per);
	for (int c = 0; c < GL_NCH; c++)
		gl_macro(c, (float)st[c] / 4.0f, per);
	gl_play(1);
}
EXPORT void lab_perform(int what, float v) { gl_perform(what, v); }
EXPORT void lab_freeze(int on) { gl_freeze(on); }
EXPORT void lab_play(int on) { gl_play(on); }
EXPORT void lab_volume(float v) { vol = v; }
/* the morph: an offset for a channel's control (c 0..3) or a global (c 4) */
EXPORT void lab_morph(int c, int p, float d)
{
	if (c >= 0 && c < GL_NCH && p >= 0 && p < GL_NCP)
		m_tgt[c * GL_NCP + p] = d;
	else if (c == GL_NCH && p >= 0 && p < GL_NGP)
		m_tgt[GL_NCH * GL_NCP + p] = d;
}
EXPORT void lab_morph_clear(void)
{
	for (int k = 0; k < GL_NCH * GL_NCP + GL_NGP; k++)
		m_tgt[k] = 0.0f;
}
EXPORT float lab_glow(int c) { return gl_glow[c & 3]; }
EXPORT float lab_beat(void) { return gl_beat; }
EXPORT int lab_ducked(void) { return gl_ducked(); }
/* n frames (a multiple of 48, at most 480), as interleaved float; the morph glides here (~0.3 s) */
EXPORT float *lab_render(uint32_t n)
{
	if ((++m_tick & 3u) == 0u) {
		int moving = 0;
		for (int k = 0; k < GL_NCH * GL_NCP + GL_NGP; k++) {
			const float d = m_tgt[k] - m_cur[k];
			if (d > 0.002f || d < -0.002f) {
				m_cur[k] += d * 0.12f;
				moving = 1;
			} else if (d != 0.0f) {
				m_cur[k] = m_tgt[k];
				moving = 1;
			}
		}
		if (moving)
			post_now();
	}
	gl_render(pcm, n, vol);
	for (uint32_t i = 0; i < 2u * n; i++)
		fbuf[i] = (float)pcm[i] * (1.0f / 32768.0f);
	return fbuf;
}
