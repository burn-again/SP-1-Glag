/* SPDX-License-Identifier: MIT
 * The Kit tuning page's glue: the real engine compiled to WebAssembly, a piece playing, the Kit's
 * numbers (gl_kit_tune) and its channel's controls reachable from the page. */
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

/* powf, for the Kit's coefficients at 16 kHz (the firmware's compiler folds them; clang here may not):
 * x^y = e^(y ln x), ln by halving to [1, 2) and the atanh series, e^ by halving and the Taylor series */
float powf(float x, float y)
{
	if (x <= 0.0f)
		return 0.0f;
	double m = x, l = 0.0;
	int e = 0;
	while (m >= 2.0) { m *= 0.5; e++; }
	while (m < 1.0) { m *= 2.0; e--; }
	double z = (m - 1.0) / (m + 1.0), z2 = z * z, t = z;
	for (int k = 1; k < 30; k += 2) { l += t / k; t *= z2; }
	l = 2.0 * l + e * 0.69314718055994531;
	double a = y * l;
	int h = 0;
	while (a > 0.5 || a < -0.5) { a *= 0.5; h++; }
	double r = 1.0, term = 1.0;
	for (int k = 1; k < 20; k++) { term *= a / k; r += term; }
	while (h--) r *= r;
	return (float)r;
}

static gl_patch patch;
static uint8_t lvl_saved[GL_NCH];
static int solo, piece;
static int16_t pcm[2 * 1024];
static float out[2 * 1024];
static double acc_e;
static int acc_n;

static void post(void) { gl_post_patch(&patch); }

EXPORT void kt_init(uint32_t seed) { gl_init(seed); gl_kit_tuning_lock(1); }   /* (the page owns the Kit's numbers) */
/* which tuning a piece carries (0 the firmware's) */
EXPORT int kt_piece_tune(int i) { gl_patch p; gl_piece(i, &p); return p.space[GL_SP_KIT]; }
EXPORT int kt_tunes(void) { return gl_kit_tunes(); }
EXPORT void kt_set(int id, float v) { gl_kit_tune(id, v); }
/* a piece (its macros as written), the Kit on channel 1 */
EXPORT void kt_piece(int i)
{
	piece = i;
	gl_piece(i, &patch);
	for (int c = 0; c < GL_NCH; c++)
		lvl_saved[c] = (uint8_t)(patch.ch[c][CP_LEVEL] * 255.0f);
	if (solo)
		for (int c = 1; c < GL_NCH; c++)
			patch.ch[c][CP_LEVEL] = 0.0f;
	post();
	uint8_t st[GL_NCH];
	float per;
	gl_piece_macro(i, st, &per);
	for (int c = 0; c < GL_NCH; c++)
		gl_macro(c, (float)st[c] / 4.0f, per);
}
EXPORT void kt_solo(int on)
{
	solo = on;
	for (int c = 1; c < GL_NCH; c++)
		patch.ch[c][CP_LEVEL] = on ? 0.0f : (float)lvl_saved[c] / 255.0f;
	post();
}
/* the Kit channel's controls: 0 activity, 1 tone, 2 length, 3 register, 7 character, 8 level */
EXPORT void kt_ch(int cp, float v) { if (cp >= 0 && cp < GL_NCP) { patch.ch[0][cp] = v; post(); } }
EXPORT float kt_get_ch(int cp) { return cp >= 0 && cp < GL_NCP ? patch.ch[0][cp] : 0.0f; }
EXPORT void kt_groove(int g) { patch.chx[0] = (uint8_t)((patch.chx[0] & 3u) | ((unsigned)(g & 15) << 2)); post(); }
EXPORT int kt_get_groove(void) { return (int)GL_CHX_GRV(patch.chx[0]); }
EXPORT void kt_tempo(float v) { patch.g[GP_TEMPO] = v; post(); }
EXPORT float kt_get_tempo(void) { return patch.g[GP_TEMPO]; }
EXPORT void kt_macro(float depth) { gl_macro(0, depth, 8.0f); }
EXPORT void kt_dist(int type, int amt, int post_) { gl_dist(type, amt, post_); }
EXPORT void kt_lanes(int mask) { gl_kit_solo(mask); }
/* n frames at 48 kHz, interleaved floats: whole 1 ms blocks only (the engine renders 48 frames at
 * a time, and a part block would be left stale), so n is rounded down to a multiple of 48 */
EXPORT float *kt_render(int n)
{
	if (n > 1008)
		n = 1008;
	n -= n % 48;
	gl_render(pcm, (uint32_t)n, 0.9f);
	for (int i = 0; i < 2 * n; i++) {
		out[i] = (float)pcm[i] * (1.0f / 32768.0f);
		acc_e += out[i] * out[i];
	}
	acc_n += 2 * n;
	return out;
}
EXPORT float kt_peak(void) { return gl_peak; }
EXPORT void kt_pat(int row, int step, int v) { gl_kit_pattern(row, step, v); }
EXPORT void kt_pat_mode(int on, int len, float swing) { gl_kit_pattern_mode(on, len, swing); }
EXPORT int kt_pat_pos(void) { return gl_kit_pattern_pos(); }
EXPORT void kt_director(int on) { gl_kit_director(on); }
/* the mean power since the last call (a test's level tap) */
EXPORT float kt_power(void) { float p = acc_n ? (float)(acc_e / acc_n) : 0.0f; acc_e = 0; acc_n = 0; return p; }
