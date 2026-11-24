/* SPDX-License-Identifier: MIT
 * Glag's hardware test, v3: a script of scenarios played through the controls, the same one in the
 * test firmware (app/src/main.c, CONFIG_SP1_GLAG_CPU_TEST) and in its host twin (host/hwtest.c),
 * so the two logs line up: what the twin also shows is the music; what only the device shows is
 * the device.
 *
 * Include it once, after glag_ui.h, with GL_PROF and GL_TEST_HOOKS defined and GLT_PRINTF set to
 * the log's printf (printk on the device). Then, every UI tick, before glui_tick, with `in` as the
 * real controls read it:
 *
 *     int ev = glt_tick(now_ms, &in, glt_touch(&in));
 *
 * glt_touch: a hand on the controls (a button held, VOL aside; a fader moved well past its noise).
 * The script then stops at once (`in` passes as it came), says what was touched, and goes on with
 * the next scenario after 30 s untouched.
 * Otherwise it may set the faders in `in`, and of the buttons only FFWD and RWD, tapped or held:
 * never ••, VOL or PLAY (so never a power gesture, never a save). It sets state through
 * glui_test() and sends MIDI through glui_midi_*(), as the gestures and a MIDI cable would.
 * Returns GLT_END on the tick a scenario has ended and printed its summary (the caller then adds
 * its own lines: stream, stacks, buffers), GLT_RUN otherwise.
 *
 * The round: 14 scenarios, about 15 minutes, then again with the next seed (only shuffle's deal
 * changes with it). Every line starts "GLAG T3":
 *   > n/14 name (s)                a scenario starts
 *   - name what: ...               one of its steps (a voice, a reverb, a distortion type)
 *   = name: load ... / out ... / engine ...    its summary
 *   track ...                      a shuffle or album track: its piece and seeds (to replay it)
 */
#ifndef GLAG_TEST_H
#define GLAG_TEST_H

#include <math.h>
#include "glag_ui.h"

#ifndef GLT_PRINTF
#error "glag_test.h: define GLT_PRINTF (printk, printf)"
#endif

enum { GLT_RUN, GLT_END };
#define GLT_SAFE ((uint16_t)((1u << GB_FFWD) | (1u << GB_RWD)))   /* all the script ever presses */
#define GLT_QUIET_MS 30000u

static uint32_t glt_round = 0, glt_seed = 0x6A09E667u, glt_rng;
static int glt_i = -1;                   /* the scenario playing (-1: before the first) */
static uint32_t glt_t0, glt_prev, glt_now, glt_quiet_at;
static int glt_paused, glt_first, glt_have_fad;
static uint16_t glt_btn;                 /* the buttons the script holds this tick */
static float glt_fad[4];                 /* where the script holds the faders (they never jump) */
static gl_check_t glt_acc, glt_sub;      /* the scenario so far; its step so far */
static const char *glt_name = "";  /* the scenario's name */
static char glt_step[40];                /* the step playing ("" none) */
static uint32_t glt_fs_seen, glt_touch_said, glt_touch_why;
static gl_check_t glt_win;               /* since the last 5 s line (glt_window) */
static int glt_p_seen = -1;
static int glt_only = -1;              /* the host twin: this scenario alone, round after round */

static float glt_u(void) { glt_rng = glt_rng * 1664525u + 1013904223u; return (float)(glt_rng >> 8) * (1.0f / 16777216.0f); }
static int glt_ir(int n) { int k = (int)(glt_u() * (float)n); return k >= n ? n - 1 : k; }
/* in a scenario, t its ms: the moment t passes ms (once), every ms, and which period of ms it is in */
static int glt_at(uint32_t t, uint32_t ms) { return ms == 0u ? glt_first : !glt_first && glt_prev < ms && t >= ms; }
#define AT(ms) glt_at(t, (uint32_t)(ms))
#define EVERY(ms) (glt_first || glt_prev / (uint32_t)(ms) != t / (uint32_t)(ms))
#define STEP(ms) (t / (uint32_t)(ms))

static void glt_zero(gl_check_t *c) { const gl_check_t z = {.ig_min = 1.0f, .cap_min = 99}; *c = z; }
static void glt_merge(gl_check_t *a, const gl_check_t *c)
{
	a->loads += c->loads; a->over90 += c->over90; a->over100 += c->over100;
	a->over150 += c->over150; a->over200 += c->over200; a->load_sum += c->load_sum;
	a->load_worst = c->load_worst > a->load_worst ? c->load_worst : a->load_worst;
	a->lean_max = c->lean_max > a->lean_max ? c->lean_max : a->lean_max;
	a->cap_min = c->cap_min < a->cap_min ? c->cap_min : a->cap_min;
	a->blocks += c->blocks; a->samples += c->samples; a->clips += c->clips; a->jumps += c->jumps;
	a->jump_max = c->jump_max > a->jump_max ? c->jump_max : a->jump_max;
	a->peak = c->peak > a->peak ? c->peak : a->peak;
	a->silent_max = c->silent_max > a->silent_max ? c->silent_max : a->silent_max;
	a->nan += c->nan; a->dc[0] += c->dc[0]; a->dc[1] += c->dc[1]; a->ms += c->ms;
	a->steps += c->steps; a->notes += c->notes; a->steals += c->steals;
	a->step_notes_max = c->step_notes_max > a->step_notes_max ? c->step_notes_max : a->step_notes_max;
	a->beats += c->beats;
	a->ig_min = c->ig_min < a->ig_min ? c->ig_min : a->ig_min;
	a->cyc += c->cyc;
}
/* tenths, signed, as "%s%d.%d" (printk has no %f) */
static int glt_x10(float v) { return (int)(v * 10.0f + (v < 0.0f ? -0.5f : 0.5f)); }
static int glt_a10(float v) { int x = glt_x10(v); return x < 0 ? -x : x; }
#define T10(v) (glt_x10(v) < 0 ? "-" : ""), glt_a10(v) / 10, glt_a10(v) % 10
static float glt_db(float x) { return x > 1e-9f ? 20.0f * log10f(x) : -180.0f; }

/* the engine's counts since the last take, into the step and the 5 s window */
static void glt_take(void)
{
	gl_check_t c;
	gl_check_take(&c);
	glt_merge(&glt_sub, &c);
	glt_merge(&glt_win, &c);
}
/* the counts since the last call, script running or not (the device's 5 s line) */
static __attribute__((unused)) void glt_window(gl_check_t *o)
{
	glt_take();
	*o = glt_win;
	glt_zero(&glt_win);
}
/* the step that just ended: its line; then the next one's name */
static void glt_step_end(void)
{
	glt_take();
	if (glt_step[0]) {
		const gl_check_t *s = &glt_sub;
		const float avg = s->loads ? s->load_sum / (float)s->loads * 100.0f : 0.0f;
		GLT_PRINTF("GLAG T3 - %s %s: load %s%d.%d/%s%d.%d%% >100 %u lean %d cap %d | steals %u clip %u clicks %u (max %u) nan %u\n",
		           glt_name, glt_step, T10(avg), T10(s->load_worst * 100.0f), s->over100, s->lean_max, s->cap_min,
		           s->steals, s->clips, s->jumps, (uint32_t)sqrtf((float)s->jump_max), s->nan);
	}
	glt_merge(&glt_acc, &glt_sub);
	glt_zero(&glt_sub);
}
static void glt_mark(const char *a, const char *b)
{
	glt_step_end();
	int n = 0;
	for (const char *p = a; p && *p && n < 38; p++)
		glt_step[n++] = *p;
	if (b && *b && n < 38)
		glt_step[n++] = ' ';
	for (const char *p = b; p && *p && n < 39; p++)
		glt_step[n++] = *p;
	glt_step[n] = 0;
}

/* ---- the scenarios: t is ms into it; set state, the faders (in->fader) and glt_btn ---- */
static void glt_tap(uint32_t t, uint32_t at, int b)
{
	if (t >= at && t < at + 100u)
		glt_btn |= (uint16_t)(1u << b);
}
static void glt_hold(uint32_t t, uint32_t from, uint32_t to, int b)
{
	if (t >= from && t < to)
		glt_btn |= (uint16_t)(1u << b);
}

static void sc_idle(uint32_t t) { (void)t; }

/* every voice on all four channels at once, busiest and longest: 6 s each */
static void sc_voices(uint32_t t)
{
	if (EVERY(6000) && STEP(6000) < GV_COUNT) {
		const int v = (int)STEP(6000);
		glt_mark(GL_VOICE_NAME[v], "x4");
		for (int c = 0; c < 4; c++) {
			glui_test(GT_VOICE, c, v, 0.0f, glt_now);
			glui_test(GT_CH, c, CP_ACTIVITY, 1.0f, glt_now);
			glui_test(GT_CH, c, CP_LENGTH, 1.0f, glt_now);
		}
	}
}
/* each reverb all the way up from every channel, then with its reverse swells: 6 s each */
static void sc_space(uint32_t t)
{
	static const uint8_t RV[6] = {GR_HALL, GR_CHAMBER, GR_SHIMMER, GR_SHIMMER, GR_SHIMMER, GR_BLOOM};
	static const uint8_t SH[6] = {0, 0, GS_OCTAVE, GS_FIFTH, GS_SEVENTH, 0};
	if (glt_first) {
		glui_test(GT_G, 0, GP_REVERB, 1.0f, glt_now);
		for (int c = 0; c < 4; c++)
			glui_test(GT_CH, c, CP_VERB, 1.0f, glt_now);
	}
	if (EVERY(6000) && STEP(6000) < 12u) {
		const int k = (int)STEP(6000), j = k >> 1;
		char nm[24];
		int n = 0;
		for (const char *p = GL_REVERB_NAME[RV[j]]; *p && n < 12; p++)
			nm[n++] = *p;
		if (RV[j] == GR_SHIMMER) {
			nm[n++] = ' ';
			for (const char *p = GL_SHIMMER_NAME[SH[j]]; *p && n < 23; p++)
				nm[n++] = *p;
		}
		nm[n] = 0;
		glt_mark(nm, k & 1 ? "reverse" : "");
		glui_test(GT_SPACE, GL_SP_REVERB, RV[j], 0.0f, glt_now);
		glui_test(GT_SPACE, GL_SP_SHIMMER, SH[j], 0.0f, glt_now);
		glui_test(GT_SPACE, GL_SP_FLAGS, (k & 1) ? GL_FLAG_REVERSE : 0, 0.0f, glt_now);
	}
}
/* the echo and the reverb all the way up, the echo at each of its times (the string's too),
 * ping-pong every other: 4 s each */
static void sc_echo(uint32_t t)
{
	if (glt_first) {
		glui_test(GT_G, 0, GP_DELAY, 1.0f, glt_now);
		glui_test(GT_G, 0, GP_REVERB, 1.0f, glt_now);
		for (int c = 0; c < 4; c++) {
			glui_test(GT_CH, c, CP_ECHO, 1.0f, glt_now);
			glui_test(GT_CH, c, CP_VERB, 1.0f, glt_now);
		}
	}
	if (EVERY(4000) && STEP(4000) < GL_NECHO) {
		const int k = (int)STEP(4000);
		glt_mark(GL_ECHO_NAME[k], k & 1 ? "ping-pong" : "");
		glui_test(GT_SPACE, GL_SP_ECHO, k, 0.0f, glt_now);
		glui_test(GT_SPACE, GL_SP_FLAGS, (k & 1) ? GL_FLAG_PINGPONG : 0, 0.0f, glt_now);
	}
}
/* freeze, the crank and the slow record, by their own buttons (FFWD and RWD, on Home) */
static void sc_hold(uint32_t t)
{
	if (AT(0))
		glt_mark("freeze, the crank", "");
	glt_tap(t, 500, GB_FFWD);                    /* freeze, latched */
	glt_tap(t, 3000, GB_RWD);                    /* the crank, three times */
	glt_tap(t, 4000, GB_RWD);
	glt_tap(t, 5000, GB_RWD);
	glt_tap(t, 9000, GB_FFWD);                   /* thawed */
	if (AT(11000))
		glt_mark("slow", "");
	glt_hold(t, 11000, 23000, GB_RWD);           /* the record slows for 12 s, then springs back */
	if (AT(23000))
		glt_mark("back", "");
}
/* every distortion, at every amount, in every place: 2 s each, a line per type */
static void sc_dist(uint32_t t)
{
	if (EVERY(2000)) {
		const uint32_t k = STEP(2000);              /* type * 16 + amount * 4 + placement */
		if (k < GD_COUNT * 16u) {
			if ((k & 15u) == 0u)
				glt_mark(GL_DIST_NAME[k >> 4], "1-4, every place");
			glui_test(GT_DIST, (int)(k >> 4), 1 + (int)((k >> 2) & 3u), (float)(k & 3u), glt_now);
		} else if (k == GD_COUNT * 16u) {
			glt_mark("off", "");
			glui_test(GT_DIST, 0, 0, 0.0f, glt_now);
		}
	}
}
/* each texture at full, its own three controls swept: 8 s each */
static void sc_tex(uint32_t t)
{
	static const char *const TX[3] = {"rain", "tape hiss", "surf"};
	static const uint8_t TN[3] = {1, 3, 4};
	if (EVERY(8000) && STEP(8000) < 3u) {
		const int k = (int)STEP(8000);
		glt_mark(TX[k], "");
		glui_test(GT_WORLD, 3, TN[k], 0.0f, glt_now);
		glui_test(GT_WORLDV, 3, 0, 1.0f, glt_now);
	}
	if (EVERY(250)) {
		const float x = (float)(t % 8000u) * (1.0f / 8000.0f);
		for (int f = 1; f < 4; f++)
			glui_test(GT_WORLDV, 3, f, f == 2 ? 1.0f - x : x, glt_now);
	}
}
/* Playworld all at once: song mode at its most random, the arp through its patterns, the pump
 * and the compressor at full, tape hiss under it */
static void sc_world(uint32_t t)
{
	if (glt_first) {
		glui_test(GT_WORLD, 0, 1, 0.0f, glt_now);
		for (int f = 0; f < 4; f++) {
			glui_test(GT_WORLDV, 0, 4 + f, 1.0f, glt_now);       /* loops x4 */
			glui_test(GT_WORLDV, 0, 12 + f, 1.0f, glt_now);      /* randomness, all of it */
		}
		glui_test(GT_WORLDV, 0, 8 + 2, 1.0f, glt_now);          /* fills */
		glui_test(GT_WORLDV, 0, 8 + 3, 1.0f, glt_now);          /* full */
		glui_test(GT_WORLD, 2, 1, 0.0f, glt_now);
		glui_test(GT_WORLDV, 2, 0, 1.0f, glt_now);              /* the pump's depth */
		glui_test(GT_WORLDV, 2, 4, 1.0f, glt_now);              /* the compressor */
		glui_test(GT_WORLD, 3, 3, 0.0f, glt_now);               /* tape hiss */
		glui_test(GT_WORLD, 1, 1, 0.0f, glt_now);
		glui_test(GT_WORLDV, 1, 0, 1.0f, glt_now);              /* four octaves */
		glui_test(GT_WORLDV, 1, 2, 0.0f, glt_now);              /* sixteenths */
		glui_test(GT_WORLDV, 1, 3, 0.5f, glt_now);
	}
	if (EVERY(10000) && STEP(10000) < 4u) {
		static const char *const PT[4] = {"up", "down", "up-down", "random"};
		const int k = (int)STEP(10000);
		glt_mark("arp", PT[k]);
		glui_test(GT_WORLDV, 1, 1, (float)k / 3.0f, glt_now);
		glui_test(GT_WORLDV, 2, 5, (float)k / 3.0f, glt_now);   /* the pump's rhythm with it */
	}
}
/* every channel's macro at its deepest, quick, then slow */
static void sc_macros(uint32_t t)
{
	if (AT(0) || AT(20000)) {
		glt_mark("deepest,", t < 20000u ? "2 s glides" : "64 s glides");
		for (int c = 0; c < 4; c++)
			glui_test(GT_MACRO, c, 4, t < 20000u ? 2.0f : 64.0f, glt_now);
	}
}
/* the faders on every page, swept through their whole travel (each catches and takes over, as a
 * hand's would): 4 s a page */
static void sc_faders(uint32_t t)
{
	static const char *const PG[18] = {"home", "ch1", "ch2", "ch3", "ch4", "ext1", "ext2", "ext3", "ext4",
		"harmony", "song 1", "song 2", "song 3", "song 4", "arp", "pump 1", "pump 2", "texture"};
	if (EVERY(4000) && STEP(4000) < 18u) {
		const int k = (int)STEP(4000);
		glt_mark("page", PG[k]);
		if (k == 0)
			glui_test(GT_PAGE, PG_HOME, 0, 0.0f, glt_now);
		else if (k <= 4)
			glui_test(GT_PAGE, PG_CH, k - 1, 0.0f, glt_now);
		else if (k <= 8)
			glui_test(GT_PAGE, PG_EXT, k - 5, 0.0f, glt_now);
		else if (k == 9)
			glui_test(GT_PAGE, PG_HARMONY, 0, 0.0f, glt_now);
		else {
			const int sec = k <= 13 ? 0 : k == 14 ? 1 : k <= 16 ? 2 : 3;
			const int st = k <= 13 ? k - 9 : k == 14 ? 1 : k <= 16 ? k - 14 : 3;
			glui_test(GT_WORLD, sec, st, 0.0f, glt_now);
			glui_test(GT_PAGE, PG_WORLD, sec, 0.0f, glt_now);
		}
	}
	for (int f = 0; f < 4; f++) {                /* 0.4 .. 1.1 sweeps a second, each its own */
		const float hz = 0.4f + 0.23f * (float)f, ph = (float)(t % 100000u) * 0.001f * hz;
		glt_fad[f] = 0.5f - 0.5f * cosf(6.2831853f * ph);
	}
}
/* MIDI as a busy sequencer sends it: a flood of controllers, then programs, then tempo */
static void sc_midi(uint32_t t)
{
	static const uint8_t CC[10] = {1, 7, 10, 16, 71, 72, 73, 74, 91, 93};
	if (AT(0))
		glt_mark("750 CC/s", "");
	if (AT(10000))
		glt_mark("a program every 1.5 s", "");
	if (AT(22000))
		glt_mark("tempo every 200 ms", "");
	if (t < 10000u)
		for (int i = 0; i < 6; i++)
			glui_midi_cc(glt_ir(4), CC[glt_ir(10)], glt_ir(128));
	else if (t < 22000u) {
		if (EVERY(1500))
			glui_midi_program(glt_ir(GL_NPIECES));
	} else if (EVERY(200))
		glui_midi_tempo(40.0f + 80.0f * glt_u());
}
/* the changes that cost the most at the moment they land, in quick succession */
static void sc_spikes(uint32_t t)
{
	if (AT(0))
		glt_mark("piece every 400 ms", "");
	if (AT(6000))
		glt_mark("reverb every 250 ms", "");
	if (AT(10000))
		glt_mark("voice every 200 ms", "");
	if (AT(15000))
		glt_mark("distortion every 150 ms", "");
	if (AT(19000)) {
		glt_mark("echo time every 200 ms", "");
		glui_test(GT_DIST, 0, 0, 0.0f, glt_now);
	}
	if (AT(23000))
		glt_mark("settling", "");
	if (t < 6000u) {
		if (EVERY(400))
			glui_test(GT_PIECE, glt_ir(GL_NPIECES), 0, 0.0f, glt_now);
	} else if (t < 10000u) {
		if (EVERY(250)) {
			glui_test(GT_SPACE, GL_SP_REVERB, glt_ir(GR_COUNT), 0.0f, glt_now);
			glui_test(GT_SPACE, GL_SP_FLAGS, glt_ir(4), 0.0f, glt_now);
		}
	} else if (t < 15000u) {
		if (EVERY(200))
			glui_test(GT_VOICE, glt_ir(4), glt_ir(GV_COUNT), 0.0f, glt_now);
	} else if (t < 19000u) {
		if (EVERY(150))
			glui_test(GT_DIST, glt_ir(GD_COUNT), glt_ir(5), (float)glt_ir(4), glt_now);
	} else if (t < 23000u) {
		if (EVERY(200))
			glui_test(GT_SPACE, GL_SP_ECHO, glt_ir(GL_NECHO), 0.0f, glt_now);
	}
}
/* the album, 20 s tracks, one skipped by FFWD */
static void sc_album(uint32_t t)
{
	if (AT(0))
		glui_test(GT_DEMO, 1, 20000, 0.0f, glt_now);
	glt_tap(t, 50000, GB_FFWD);
}
/* shuffle, 16 s tracks, now and then one skipped by FFWD: a fuzz test, every deal logged */
static uint32_t glt_skip_at;
static void sc_shuffle(uint32_t t)
{
	if (AT(0)) {
		glui_test(GT_SEED, (int)glt_seed, 0, 0.0f, glt_now);
		glui_test(GT_DEMO, 2, 16000, 0.0f, glt_now);
		glt_skip_at = 0;
	}
	if (EVERY(16000) && glt_u() < 0.3f)
		glt_skip_at = t + 4000u + (uint32_t)glt_ir(8000);
	if (glt_skip_at)
		glt_tap(t, glt_skip_at, GB_FFWD);
}

typedef struct { const char *name; uint32_t secs; uint8_t piece; void (*fn)(uint32_t t); } glt_scene;
/* (pieces: 0 Garden, 1 Departures; 16-19 the four heaviest, as this build carries them. The faders
 * on Shifted Metal: it has no clicks of its own, so any the faders make stand out) */
static const glt_scene GLT_SCENES[] = {
	{"idle", 20, 0, sc_idle},
	{"voices", 6u * GV_COUNT, 0, sc_voices},
	{"space", 72, 0, sc_space},
	{"echo", 4u * GL_NECHO, 1, sc_echo},
	{"hold", 28, 0, sc_hold},
	{"dist", 2u * 16u * GD_COUNT + 4u, 16, sc_dist},
	{"textures", 24, 0, sc_tex},
	{"playworld", 40, 17, sc_world},
	{"macros", 30, 18, sc_macros},
	{"faders", 72, 16, sc_faders},
	{"midi", 28, 19, sc_midi},
	{"spikes", 28, 0, sc_spikes},
	{"album", 64, 0, sc_album},
	{"shuffle", 192, 0, sc_shuffle},
};
#define GLT_N ((int)(sizeof GLT_SCENES / sizeof GLT_SCENES[0]))

static void glt_summary(void)
{
	glt_step_end();
	const gl_check_t *a = &glt_acc;
	const char *nm = glt_name;
	const float avg = a->loads ? a->load_sum / (float)a->loads * 100.0f : 0.0f;
	GLT_PRINTF("GLAG T3 = %s: load avg %s%d.%d worst %s%d.%d%% | blocks %u >90 %u >100 %u >150 %u >200 %u | lean %d cap %d\n",
	           nm, T10(avg), T10(a->load_worst * 100.0f), a->loads, a->over90, a->over100, a->over150, a->over200,
	           a->lean_max, a->cap_min);
	const float nb = a->blocks ? (float)a->blocks : 1.0f;
	const float rms = glt_db(sqrtf(a->ms / nb) * (1.0f / 32768.0f));
	const float pk = glt_db((float)a->peak * (1.0f / 32768.0f));
	const float dl = a->dc[0] / nb * (1000.0f / 32768.0f), dr = a->dc[1] / nb * (1000.0f / 32768.0f);
	const uint32_t per = a->blocks ? a->samples / a->blocks : 0u;
	const uint32_t el = glt_now - glt_t0;
	GLT_PRINTF("GLAG T3 = %s: out peak %s%d.%d rms %s%d.%d dBFS dc %s%d.%d/%s%d.%d permille | clip %u clicks %u (max %u) | silent %u ms | nan %u | %u Hz\n",
	           nm, T10(pk), T10(rms), T10(dl), T10(dr), a->clips, a->jumps, (uint32_t)sqrtf((float)a->jump_max), a->silent_max * per / 48u,
	           a->nan, el ? (uint32_t)((uint64_t)a->samples * 1000u / el) : 0u);
	GLT_PRINTF("GLAG T3 = %s: steps %u of %u | notes %u, at most %u a step | steals %u | reverb guard %s%d.%d%% | checks %u cyc/blk\n",
	           nm, a->steps, (uint32_t)(a->beats * 4.0f + 0.5f), a->notes, a->step_notes_max, a->steals,
	           T10(a->ig_min * 100.0f), a->blocks ? a->cyc / a->blocks : 0u);
}

static void glt_start(int i, uint32_t now)
{
	glt_i = i;
	glt_name = GLT_SCENES[i].name;
	glt_t0 = glt_prev = now;
	glt_first = 1;
	glt_btn = 0;
	glt_step[0] = 0;
	glui_test(GT_RESET, GLT_SCENES[i].piece < GL_NPIECES ? GLT_SCENES[i].piece : 0, 0, 0.0f, now);
	glt_take();                                      /* (what came before is not this one's) */
	glt_zero(&glt_acc);
	glt_zero(&glt_sub);
	GLT_PRINTF("GLAG T3 > %d/%d %s (%u s) on %s, round %u seed 0x%08x\n", i + 1, GLT_N, GLT_SCENES[i].name,
	           GLT_SCENES[i].secs, gl_piece_name(GLT_SCENES[i].piece), glt_round, glt_seed);
}

/* A hand on the controls: the buttons held (bits 0-9, as gl_btn; VOL aside, it is the output
 * level), and the faders moved (bits 16-19). The faders come raw from the ADC, so no one reading
 * means anything: each is followed by a running average (~30 ms), its resting place taken once that
 * has settled, and a fader counts only when its average has sat more than 8 % from there for three
 * ticks running. Noise never does that; a hand moving a fader always does, within ~0.1 s. */
static float glt_avg[4], glt_ref[4];
static uint8_t glt_away[4], glt_warm;
static uint32_t glt_touch(const gl_input *in)
{
	uint32_t why = (uint32_t)(in->buttons & (uint16_t)~((1u << GB_VOLUP) | (1u << GB_VOLDN)));
	for (int f = 0; f < 4; f++)
		glt_avg[f] = glt_warm ? glt_avg[f] + 0.25f * (in->fader[f] - glt_avg[f]) : in->fader[f];
	if (glt_warm < 8u) {                             /* settling: where the faders rest */
		glt_warm++;
		for (int f = 0; f < 4; f++)
			glt_ref[f] = glt_avg[f];
		return why;
	}
	for (int f = 0; f < 4; f++) {
		const float d = glt_avg[f] - glt_ref[f];
		if (d < 0.08f && d > -0.08f)
			glt_away[f] = 0;
		else if (++glt_away[f] >= 3u) {
			why |= 1u << (16 + f);
			glt_ref[f] = glt_avg[f];
			glt_away[f] = 0;
		}
	}
	return why;
}

static int glt_tick(uint32_t now, gl_input *in, uint32_t real)
{
	glt_now = now;
	if (real) {                                      /* a hand on it: out of the way, at once */
		/* what it was: at once, then at most every 10 s (or when it is something else) */
		if (!glt_paused || real != glt_touch_why || now - glt_touch_said >= 10000u) {
			GLT_PRINTF("GLAG T3 %s: buttons 0x%03x faders 0x%x (on again %u s after the last touch)\n",
			           glt_paused ? "touched" : "paused", (unsigned)(real & 0xFFFFu), (unsigned)(real >> 16),
			           GLT_QUIET_MS / 1000u);
			glt_touch_said = now;
			glt_touch_why = real;
		}
		glt_paused = 1;
		glt_quiet_at = now;
		return GLT_RUN;
	}
	if (glt_paused) {
		if (now - glt_quiet_at < GLT_QUIET_MS)
			return GLT_RUN;
		glt_paused = 0;
		glt_have_fad = 0;                            /* (from where the hand left them) */
		GLT_PRINTF("GLAG T3 on again\n");
		glt_start(glt_i + 1 < GLT_N ? glt_i + 1 : 0, now);
	}
	if (!glt_have_fad) {
		glt_have_fad = 1;
		for (int f = 0; f < 4; f++)
			glt_fad[f] = in->fader[f];
	}
	int ev = GLT_RUN;
	if (glt_i < 0) {
		glt_rng = glt_seed;
		glt_start(glt_only >= 0 ? glt_only : 0, now);
	} else if (now - glt_t0 >= GLT_SCENES[glt_i].secs * 1000u) {
		glt_summary();
		ev = GLT_END;
		int next = glt_i + 1;
		if (next >= GLT_N || glt_only >= 0) {
			GLT_PRINTF("GLAG T3 round %u done\n", glt_round);
			glt_round++;
			glt_seed = glt_seed * 2654435761u + 0x9E3779B9u;
			glt_rng = glt_seed;
			next = glt_only >= 0 ? glt_only : 0;
		}
		glt_start(next, now);
	}
	glt_btn = 0;
	const uint32_t t = now - glt_t0;
	GLT_SCENES[glt_i].fn(t);
	glt_prev = t;
	glt_first = 0;
	for (int f = 0; f < 4; f++)
		in->fader[f] = glt_fad[f];
	in->buttons = (uint16_t)(glt_btn & GLT_SAFE);   /* FFWD and RWD, nothing else, ever */
	{	/* a new album or shuffle track: a step of its own, and the seeds that replay it */
		int p, m;
		uint32_t us, fs;
		glui_track(&p, &m, &us, &fs);
		if (m && (fs != glt_fs_seen || p != glt_p_seen)) {
			glt_fs_seen = fs;
			glt_p_seen = p;
			glt_mark(gl_piece_name(p), "");
			GLT_PRINTF("GLAG T3 track %s %d (%s) ui 0x%08x fresh 0x%08x\n", m == 2 ? "shuffle" : "album",
			           p + 1, gl_piece_name(p), us, fs);
		}
	}
	return ev;
}

#endif /* GLAG_TEST_H */
