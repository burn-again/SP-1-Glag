/* SPDX-License-Identifier: MIT
 * Glag controls (glag_ui.h). Main thread only.
 */
#include "glag_ui.h"
#include <string.h>

/* ---- timing (ms) ---- */
#define TAP_MS 300u          /* a press shorter than this is a tap */
#define DOUBLE_MS 400u       /* two taps closer than this are a double tap */
#define FREEZE_HOLD_MS 400u  /* FFWD held longer than this freezes only while held */
#define SAVE_HOLD_MS 3000u   /* •• + RWD held this long saves the piece, its macros as its scene */
#define SCENE_HOLD_MS 3000u  /* •• + FFWD held this long: the piece's macro scene in */
#define SHOW_MS 1000u        /* how long the play row shows a value */
#define CATCH 0.02f          /* soft takeover: this close counts as reached */
#define MOVE 0.008f          /* a fader moved more than this is being used */
#define VOL_STEPS 16
#define MAC_STEPS 4          /* FFWD on a channel page: macro depth in quarters */
#define MAC_PERIOD 8.0f      /* seconds per glide, at first */
#define MAC_SLOWEST 64.0f
#define MAC_SLOW_MS 1500.0f  /* RWD held on a channel page: the glide takes twice as long every 1.5 s */

static gl_patch cur, undo_patch;
static int undo_valid, undo_piece;
static gl_patch saved[GL_NPIECES];
static uint8_t saved_valid[GL_NPIECES];
/* each piece's hidden macro scene: from the piece, overwritten by saving it (•• + RWD held 3 s) */
static uint8_t scene_mac[GL_NPIECES][4];
static float scene_per[GL_NPIECES][4];
static int piece;
static int dirty = 1;

static int page = PG_HOME, chan;
static int layer_shift;                       /* the faders were last mapped with •• held */
static float last_pos[4];
static uint8_t caught[4];
static int8_t side_at_entry[4];               /* which side of the value the fader started */

static uint16_t prev_btn;
static uint32_t down_at[GL_NBTN];
static uint8_t used_with_fn;                  /* something else was pressed during this •• hold */
static uint32_t fn_tap_at;
static int fn_taps;
static int play_taps;                         /* •• + PLAY taps waiting to be counted */
static uint32_t play_tap_at;
static int freeze_latched, freeze_on, slow_on, playing = 1;
static uint8_t rwd_shift, rwd_saved, ffwd_froze, ffwd_shift, ffwd_done, started;
/* distortion: type, amount (0 off .. 4), after the effects; and the •• + VOL presses in flight */
#define DIST_HOLD_MS 3000u
static int dist_type, dist_amt, dist_post;
static uint8_t vol_shift[2], vol_done[2];                /* [0] VOL-, [1] VOL+ */
static int vol = 11;
/* the channel macro, per channel: depth in steps, and how slow its glide is */
static int mac_step[4];
static float mac_period[4] = {MAC_PERIOD, MAC_PERIOD, MAC_PERIOD, MAC_PERIOD};
static float rwd_from;                          /* the period when RWD went down */
static uint8_t rwd_mac;                         /* this RWD press is the macro's (channel pages) */
static uint32_t rwd_tap_at;
static int rwd_taps;
static uint32_t seed = 0x2545F491u;

/* what the play row is showing */
enum { SH_MUSIC, SH_VALUE, SH_FIND, SH_INDEX, SH_SWEEP, SH_FLASH, SH_DIST, SH_HOLD, SH_REVERB, SH_MACROS };

/* ---- Playworld ---- */
#define WORLD_HOLD_MS 2000u
static int w_song, w_arp, w_pump, w_tex, w_focus = -1;
static float wv_song[4][4] = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f, 0.5f},
                              {0.0f, 0.5f, 0.7f, 0.6f}, {0.0f, 0.0f, 0.0f, 0.0f}};
static float wv_pump[2][4] = {{0.6f, 1.0f / 3.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
static float wv_tex[5][4] = {{0}, {0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f},
                             {0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};
static float wv_arp[4] = {1.0f / 3.0f, 0.0f, 1.0f / 3.0f, 0.0f};   /* octaves, pattern, division, spread */
static float w_sink;                            /* the faders' target when nothing is focused */
static uint32_t w_anim_t0;
static int w_anim_from, w_anim_to, w_anim_kind;  /* 0 none, 1 glide to a page, 2 fade out, 3 not yet */
static uint8_t play_world_done;
/* ---- demo mode: the album ---- */
#define DEMO_HOLD_MS 10000u
#define SHUF_HOLD_MS 20000u
#ifdef GL_TEST_HOOKS
static uint32_t demo_piece_ms = 120000u;        /* (the hardware test plays shorter tracks) */
#define DEMO_PIECE_MS demo_piece_ms
#else
#define DEMO_PIECE_MS 120000u
#endif
#define DEMO_FADE_MS 16000u                     /* each side of a track: out, the switch in the quiet, in */
#define DEMO_SKIP_MS 1500u                      /* FFWD / RWD: out this quickly ... */
#define DEMO_SKIPIN_MS 4000u                    /* ... and the next in over this (and the first track) */
#define DEMO_COUNT 8                            /* for now the AMB-1 pieces */
#define DEMO_SEED 1234u                         /* the seed the clips were rendered with */
static int demo_on, demo_idx, demo_n, play_prev_page;   /* demo_on: 1 the album, 2 shuffle */
static uint8_t demo_list[GL_NPIECES];
static uint32_t demo_t0;
static float demo_gain = 1.0f;
static uint32_t demo_in = DEMO_SKIPIN_MS, demo_out = DEMO_FADE_MS, demo_end;   /* this track's fades; when it ends */
static uint8_t demo_skipped;
static int show = SH_MUSIC;
static uint32_t show_until;
static float show_val;
static int show_idx, show_dir;
static uint32_t flash_t0;

/* ---------------------------------------------------------------- the fader map */

typedef struct {
	float *val;
	int steps;          /* 0 smooth, else positions */
	int centred;
} target;

static target map(int f, int shift)
{
	target t = {0, 0, 0};
	if (shift) {                                  /* the mixer, everywhere */
		t.val = &cur.ch[f][CP_LEVEL];
		return t;
	}
	switch (page) {
	case PG_HOME: {
		static const int G[4] = {GP_TEMPO, GP_DELAY, GP_REVERB, GP_FILTER};
		t.val = &cur.g[G[f]];
		t.steps = gl_gp_steps(G[f]);
		t.centred = gl_gp_centred(G[f]);
		break;
	}
	case PG_CH: {
		static const int C[4] = {CP_CHAR, CP_TONE, CP_LENGTH, CP_REGISTER};
		t.val = &cur.ch[chan][C[f]];
		t.steps = gl_cp_steps(C[f]);
		break;
	}
	case PG_EXT: {
		static const int C[4] = {CP_ECHO, CP_VERB, CP_PAN, CP_ACTIVITY};
		t.val = &cur.ch[chan][C[f]];
		t.steps = gl_cp_steps(C[f]);
		t.centred = gl_cp_centred(C[f]);
		break;
	}
	case PG_HARMONY: {
		static const int G[4] = {GP_ROOT, GP_MODE, GP_DRIFT, GP_MOTION};
		t.val = &cur.g[G[f]];
		t.steps = gl_gp_steps(G[f]);
		break;
	}
	case PG_WORLD:
		t.val = &w_sink;
		if (w_focus == 0 && w_song) {
			t.val = &wv_song[w_song - 1][f];
			if (w_song == 2)
				t.steps = 5;                          /* loops: x1/4 .. x4 */
			else if (w_song == 3 && f < 2)
				t.steps = f == 0 ? 3 : 5;             /* arrangement; length 4..32 bars */
		} else if (w_focus == 1 && w_arp) {
			t.val = &wv_arp[f];
			t.steps = f < 3 ? 4 : 0;                  /* 1-4 octaves; up, down, up-down, random; 16th .. quarter */
		} else if (w_focus == 2 && w_pump) {
			t.val = &wv_pump[w_pump - 1][f];
			if (w_pump == 2 && f == 1)
				t.steps = 4;                          /* the rhythm */
		} else if (w_focus == 3 && w_tex)
			t.val = &wv_tex[w_tex][f];
		break;
	}
	return t;
}

static void uncatch_all(const gl_input *in);
/* Playworld: one fader's value to the engine */
static void world_apply(int sec, int pg, int f)
{
	if (sec == 0) {
		float v = wv_song[pg][f];
		static const float BARS[5] = {1.0f, 2.0f, 4.0f, 6.0f, 8.0f};   /* x4 per cycle: 4..32 bars */
		if (pg == 0)
			gl_world_ch(GWC_STAGGER, f, v);
		else if (pg == 1)
			gl_world_ch(GWC_LOOP, f, (float)(int)(v * 4.0f + 0.5f));
		else if (pg == 2) {
			if (f == 0)
				gl_world(GW_FORM_KIND, (float)(int)(v * 2.0f + 0.5f));
			else if (f == 1)
				gl_world(GW_FORM_BARS, BARS[(int)(v * 4.0f + 0.5f)]);
			else
				gl_world(f == 2 ? GW_FILLS : GW_FULL, v);
		} else {
			static const int R[4] = {GW_RND_SET, GW_RND_MAC, GW_RND_KEY, GW_RND_STRAY};
			gl_world(R[f], v);
		}
	} else if (sec == 1) {
		static const int A[4] = {GW_ARP_OCT, GW_ARP_PAT, GW_ARP_DIV, GW_ARP_SPREAD};
		gl_world(A[f], wv_arp[f]);
	} else if (sec == 2) {
		float v = wv_pump[pg][f];
		static const int P0[4] = {GW_PUMP, GW_PUMP_REL, GW_PUMP_BEND, GW_PUMP_DRUMS};
		static const int P1[4] = {GW_COMP, GW_PUMP_RHYTHM, GW_PUMP_SPACE, GW_PUMP_SPARE};
		if (pg == 1 && f == 1)
			gl_world(GW_PUMP_RHYTHM, (float)(int)(v * 3.0f + 0.5f));
		else
			gl_world(pg ? P1[f] : P0[f], v);
	} else if (sec == 3 && w_tex) {
		if (f == 0)
			gl_world(GW_TEX_LEVEL, wv_tex[w_tex][0]);
		else
			gl_world_ch(GWC_TEXP, f - 1, wv_tex[w_tex][f]);
	}
}
static void world_apply_all(int sec)
{
	if (sec == 0)
		for (int pg = 0; pg < 4; pg++)
			for (int f = 0; f < 4; f++)
				world_apply(0, pg, f);
	else if (sec == 2)
		for (int pg = 0; pg < 2; pg++)
			for (int f = 0; f < 4; f++)
				world_apply(2, pg, f);
	else if (sec == 3)
		for (int f = 0; f < 4; f++)
			world_apply(3, 0, f);
	else if (sec == 1)
		for (int f = 0; f < 4; f++)
			world_apply(1, 0, f);
}
/* the light that shows a section's page (its own T on its first page) */
static int world_page_light(int sec)
{
	if (sec == 0)
		return w_song > 1 ? w_song - 1 : 0;
	if (sec == 2)
		return w_pump == 2 ? 1 : 2;
	if (sec == 3)
		return w_tex > 1 ? 4 - w_tex : 3;
	return sec;
}
static void world_anim(int kind, int from, int to, uint32_t now)
{
	w_anim_kind = kind;
	w_anim_from = from;
	w_anim_to = to;
	w_anim_t0 = now;
}
static int world_any_on(void) { return w_song || w_arp || w_pump || w_tex; }
/* T1..T4 on Playworld */
static void world_press(int t, uint32_t now, const gl_input *in)
{
	int *st = t == 0 ? &w_song : t == 1 ? &w_arp : t == 2 ? &w_pump : &w_tex;
	int n = t == 0 ? 5 : t == 1 ? 2 : t == 2 ? 3 : 5;
	if (*st && w_focus != t) {                       /* on already: just bring the faders here */
		w_focus = t;
		world_anim(1, t, world_page_light(t), now);
		uncatch_all(in);
		return;
	}
	*st = (*st + 1) % n;
	if (t == 3 && *st == 2)
		*st = 3;                                     /* (the vinyl's place: removed) */
	if (t == 0)
		gl_world(GW_SONG, w_song ? 1.0f : 0.0f);
	else if (t == 1)
		gl_world(GW_ARP, w_arp ? 1.0f : 0.0f);      /* (taken on the next bar) */
	else if (t == 2 && !w_pump)
		gl_world(GW_PUMP, 0.0f);
	else if (t == 3 && !w_tex)
		gl_world(GW_TEXTURE, 0.0f);
	if (t == 3 && w_tex)
		gl_world(GW_TEXTURE, (float)w_tex);
	if (*st && (*st == 1 || t == 3))
		world_apply_all(t);                          /* on (or a new texture): its values */
	if (*st) {
		w_focus = t;
		world_anim(1, t, world_page_light(t), now);
	} else {
		world_anim(2, t, t, now);
		w_focus = w_song ? 0 : w_arp ? 1 : w_pump ? 2 : w_tex ? 3 : -1;
	}
	uncatch_all(in);
}

static float snap(const target *t, float pos)
{
	if (t->steps > 1) {
		int k = (int)(pos * (float)(t->steps - 1) + 0.5f);
		return (float)k / (float)(t->steps - 1);
	}
	if (t->centred && pos > 0.46f && pos < 0.54f)
		return 0.5f;                              /* centre detent */
	return pos;
}

static void uncatch_all(const gl_input *in)
{
	for (int f = 0; f < 4; f++) {
		target t = map(f, layer_shift);
		float v = *t.val, p = snap(&t, in->fader[f]);
		caught[f] = (p - v < CATCH && v - p < CATCH);
		side_at_entry[f] = (int8_t)(p < v ? -1 : 1);
	}
}

static void go(int pg, int ch, const gl_input *in)
{
	page = pg;
	chan = ch;
	uncatch_all(in);
}

/* ---------------------------------------------------------------- pieces */

static void macro_post(int c);
static void say(int what, uint32_t now, uint32_t ms);
/* Harmony's rocker: the echo a step longer or shorter (1/8 .. 2 bars, in order) */
static void echo_step(int d, uint32_t now)
{
	int at = 0;
	for (int i = 0; i < GL_NECHO; i++)
		if (GL_ECHO_ORDER[i] == cur.space[GL_SP_ECHO])
			at = i;
	at += d;
	at = at < 0 ? 0 : at >= GL_NECHO ? GL_NECHO - 1 : at;
	cur.space[GL_SP_ECHO] = GL_ECHO_ORDER[at];
	dirty = 1;
	show_idx = at;
	say(SH_INDEX, now, 1200);
}
static void load_piece(int i)
{
	undo_patch = cur;
	undo_piece = piece;
	undo_valid = 1;
	piece = (i % GL_NPIECES + GL_NPIECES) % GL_NPIECES;
	if (saved_valid[piece])
		cur = saved[piece];
	else
		gl_piece(piece, &cur);
	{	/* the macros as the piece loads (most: off); its scene waits for •• + FFWD held 3 s */
		uint8_t st[4];
		float per;
		gl_piece_macro(piece, st, &per);
		for (int c = 0; c < 4; c++) {
			mac_step[c] = st[c];
			mac_period[c] = per;
			macro_post(c);
		}
	}
	dirty = 1;
}

void glui_enter(void)
{
	started = 0;
	prev_btn = 0;
	fn_taps = play_taps = 0;
	if (freeze_on || slow_on) {               /* nothing stays held across a power cycle */
		freeze_on = freeze_latched = slow_on = 0;
		gl_freeze(0);
		gl_slow(0);
	}
}

void glui_set_saved(int i, const gl_patch *p)
{
	if ((unsigned)i >= GL_NPIECES)
		return;
	saved_valid[i] = p != 0;
	if (p)
		saved[i] = *p;

}
const gl_patch *glui_saved(int i) { return (unsigned)i < GL_NPIECES && saved_valid[i] ? &saved[i] : 0; }
const gl_patch *glui_patch(void) { return &cur; }
float glui_target(int f, int *c)
{
	target t = map(f & 3, layer_shift);
	if (c)
		*c = caught[f & 3];
	return *t.val;
}

void glui_init(void)
{
	for (int i = 0; i < GL_NPIECES; i++) {
		float per;
		gl_piece_scene(i, scene_mac[i], &per);
		for (int c = 0; c < 4; c++)
			scene_per[i][c] = per;
	}
	piece = 0;
	if (saved_valid[0])
		cur = saved[0];
	else
		gl_piece(0, &cur);
	undo_valid = 0;
	page = PG_HOME;
	show = SH_MUSIC;
	dirty = 1;
	memset(caught, 0, sizeof caught);
	started = 0;
	playing = 1;
	freeze_latched = freeze_on = slow_on = 0;
	vol = 11;
	dist_type = dist_amt = dist_post = 0;
	gl_dist(0, 0, 0);
	for (int c = 0; c < 4; c++) {
		mac_step[c] = 0;
		mac_period[c] = MAC_PERIOD;
		gl_macro(c, 0.0f, MAC_PERIOD);
	}
	rwd_mac = 0;
	rwd_taps = 0;
}

static int on_channel(void) { return page == PG_CH || page == PG_EXT; }
static void macro_post(int c) { gl_macro(c, (float)mac_step[c] / MAC_STEPS, mac_period[c]); }

/* ---------------------------------------------------------------- play row pictures */

static void bar(uint8_t *p, float v)           /* a bar filling from the •• end */
{
	for (int i = 0; i < 4; i++) {
		float x = v * 4.0f - (float)i;
		p[i] = (uint8_t)(x >= 1.0f ? 255 : x <= 0.0f ? 0 : (int)(x * 255.0f));
	}
}

/* A number 1..16 on the play row: the LED (1-4) is idx % 4, and how it lights is the group of
 * four: steady (1-4), slow blink (5-8), fast blink (9-12), breathing (13-16). */
static void index_pic(uint8_t *p, int idx, uint32_t now)
{
	memset(p, 0, 4);
	uint8_t v = 255;
	if (idx >= 16) {                       /* 17-20: a double flash; 21-24: a triple; 25-28: four */
		uint32_t t = now % 900u;
		int on = t < 90u || (t >= 180u && t < 270u) || (idx >= 20 && t >= 360u && t < 450u) ||
		         (idx >= 24 && t >= 540u && t < 630u);
		p[idx & 3] = on ? 255 : 20;
		return;
	}
	switch ((idx >> 2) & 3) {
	case 1: v = (now / 300u) & 1u ? 25 : 255; break;
	case 2: v = (now / 90u) & 1u ? 25 : 255; break;
	case 3: {
		uint32_t t = now % 1200u;
		v = (uint8_t)(30 + (t < 600u ? t : 1200u - t) * 225u / 600u);
		break;
	}
	default: break;
	}
	p[idx & 3] = v;
}

static void say(int what, uint32_t now, uint32_t ms)
{
	show = what;
	show_until = now + ms;
	if (what == SH_FLASH || what == SH_SWEEP)
		flash_t0 = now;
}

/* ---------------------------------------------------------------- the tick */

#define DOWN(b) (btn & (1u << (b)))
#define PRESSED(b) ((btn & ~prev_btn) & (1u << (b)))
#define RELEASED(b) ((~btn & prev_btn) & (1u << (b)))

/* ---- shuffle: every piece (but the labs) in a random order, each with random settings ---- */
static float clampf_ui(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static float urand(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (float)(seed >> 8) * (1.0f / 16777216.0f);
}
static int irand(int n) { int k = (int)(urand() * (float)n); return k >= n ? n - 1 : k; }
static void shuffle_list(uint32_t now)
{
	seed ^= now * 2654435761u;
	demo_n = 0;
	for (int i = 0; i < GL_NPIECES; i++) {
		const char *nm = gl_piece_name(i);
		if (!(nm[0] == 'L' && nm[1] == 'a' && nm[2] == 'b' && nm[3] == ' '))
			demo_list[demo_n++] = (uint8_t)i;
	}
	for (int i = demo_n - 1; i > 0; i--) {             /* Fisher-Yates */
		int j = irand(i + 1);
		uint8_t t = demo_list[i];
		demo_list[i] = demo_list[j];
		demo_list[j] = t;
	}
}
static uint8_t shuf_was[4];             /* Playworld's modes before shuffle: song, arp, pump, texture */
/* what shuffle may add to a piece, in the engine's estimate (gl_cost_est, model points): 68 is
 * about 88 % of the device, room left for the faders and the macros */
#define SHUF_BUDGET 68.0f
static void shuffle_settings(void)
{
	const uint8_t rv_own = cur.space[GL_SP_REVERB];
	/* song mode, always, with an arrangement of its own */
	for (int c = 0; c < 4; c++) {
		wv_song[0][c] = 0.6f * urand();
		wv_song[1][c] = (float)(1 + irand(3)) * 0.25f;   /* x1/2, x1, x2 */
	}
	wv_song[2][0] = (float)irand(3) * 0.5f;
	wv_song[2][1] = (float)(1 + irand(3)) * 0.25f;        /* 8, 16 or 24 bars a quarter */
	wv_song[2][2] = urand();
	wv_song[2][3] = 0.4f + 0.6f * urand();
	wv_song[3][0] = 0.6f * urand();
	wv_song[3][1] = 0.6f * urand();
	wv_song[3][2] = urand() < 0.4f ? 0.2f + 0.6f * urand() : 0.0f;
	wv_song[3][3] = 0.5f * urand();
	w_song = 1;
	gl_world(GW_SONG, 1.0f);
	world_apply_all(0);
	/* the pump, now and then (a quarter of those without drums) */
	if (urand() < 0.35f) {
		float p0[4] = {0.3f + 0.6f * urand(), urand(), urand() < 0.5f ? 0.8f * urand() : 0.0f, urand() < 0.25f ? 0.0f : 1.0f};
		float p1[4] = {0.6f * urand(), (float)irand(4) / 3.0f, 0.5f + 0.5f * urand(), 0.6f * urand()};
		for (int f = 0; f < 4; f++) {
			wv_pump[0][f] = p0[f];
			wv_pump[1][f] = p1[f];
		}
		w_pump = 1;
		world_apply_all(2);
	} else {
		w_pump = 0;
		gl_world(GW_PUMP, 0.0f);
	}
	/* arp mode, once in a while */
	if (urand() < 0.15f) {
		wv_arp[0] = (float)irand(4) / 3.0f;
		wv_arp[1] = (float)irand(4) / 3.0f;
		wv_arp[2] = (float)irand(3) / 3.0f;
		wv_arp[3] = urand();
		w_arp = 1;
		gl_world(GW_ARP, 1.0f);
		world_apply_all(1);
	} else {
		w_arp = 0;
		gl_world(GW_ARP, 0.0f);
	}
	/* a texture, now and then */
	if (urand() < 0.3f) {
		static const uint8_t TX[3] = {1, 3, 4};   /* rain, tape hiss or surf (the vinyl removed) */
		w_tex = TX[irand(3)];
		wv_tex[w_tex][0] = 0.25f + 0.35f * urand();
		for (int f = 1; f < 4; f++)
			wv_tex[w_tex][f] = 0.2f + 0.6f * urand();
		gl_world(GW_TEXTURE, (float)w_tex);
		world_apply_all(3);
	} else {
		w_tex = 0;
		gl_world(GW_TEXTURE, 0.0f);
	}
	w_focus = 0;
	/* distortion, now and then, mostly gentle */
	if (urand() < 0.3f) {
		dist_type = irand(GD_COUNT);
		dist_amt = urand() < 0.75f ? 1 + irand(2) : 3;
		dist_post = irand(4);
	} else
		dist_amt = 0;
	gl_dist(dist_type, dist_amt, dist_post);
	/* the space: another reverb, reverse, ping-pong, an echo time, how much of each */
	if (urand() < 0.5f) {
		cur.space[GL_SP_REVERB] = (uint8_t)irand(GR_COUNT);
		cur.space[GL_SP_SHIMMER] = (uint8_t)irand(GS_COUNT);
	}
	uint8_t fl = cur.space[GL_SP_FLAGS] & (uint8_t)~(GL_FLAG_REVERSE | GL_FLAG_PINGPONG);
	if (urand() < 0.25f)
		fl |= GL_FLAG_REVERSE;
	if (urand() < 0.3f)
		fl |= GL_FLAG_PINGPONG;
	cur.space[GL_SP_FLAGS] = fl;
	if (urand() < 0.5f)
		cur.space[GL_SP_ECHO] = (uint8_t)irand(GL_NECHO);
	cur.g[GP_DELAY] = clampf_ui(cur.g[GP_DELAY] + 0.3f * (urand() - 0.5f));
	cur.g[GP_REVERB] = clampf_ui(cur.g[GP_REVERB] + 0.3f * (urand() - 0.5f));
	gl_world(GW_AUTOF, urand() < 0.2f ? 0.3f + 0.5f * urand() : 0.0f);
	/* past the budget the heaviest extras go again: the texture, the distortion, the reverse
	 * swell, the pump, then a reverb heavier than the piece's own. (On a piece already past it,
	 * nothing is added at all.) */
	for (int k = 0; k < 5 && gl_cost_est(&cur, dist_amt, w_tex, w_pump) > SHUF_BUDGET; k++)
		if (k == 0 && w_tex) {
			w_tex = 0;
			gl_world(GW_TEXTURE, 0.0f);
		} else if (k == 1 && dist_amt) {
			dist_amt = 0;
			gl_dist(dist_type, 0, dist_post);
		} else if (k == 2)
			cur.space[GL_SP_FLAGS] &= (uint8_t)~GL_FLAG_REVERSE;
		else if (k == 3 && w_pump) {
			w_pump = 0;
			gl_world(GW_PUMP, 0.0f);
		} else if (k == 4) {
			gl_patch own = cur;
			own.space[GL_SP_REVERB] = rv_own;
			if (gl_cost_est(&own, dist_amt, w_tex, w_pump) < gl_cost_est(&cur, dist_amt, w_tex, w_pump))
				cur.space[GL_SP_REVERB] = rv_own;
		}
	/* macros: about half the channels, at their own depth and pace */
	for (int c = 0; c < 4; c++) {
		mac_step[c] = urand() < 0.5f ? 1 + irand(MAC_STEPS) : 0;
		mac_period[c] = 4.0f + 20.0f * urand();
		macro_post(c);
	}
}
/* the next track: its piece afresh (the album's as recorded; shuffle's with its own dice and
 * settings) */
static uint32_t trk_ui_seed, trk_fresh_seed;
static void demo_track(uint32_t now)
{
	trk_ui_seed = seed;                   /* (kept, so a track can be replayed exactly) */
	load_piece(demo_list[demo_idx]);
	if (demo_on == 2)
		shuffle_settings();
	gl_post_patch(&cur);
	dirty = 0;
	trk_fresh_seed = demo_on == 1 ? DEMO_SEED : now * 2654435761u + 1u;
	gl_fresh(trk_fresh_seed);
}
/* the album (1) or shuffle (2) from its first track */
static void demo_start(int mode, uint32_t now, const gl_input *in)
{
	demo_on = mode;
	if (demo_on == 2) {                      /* what shuffle will change, to put back after */
		shuf_was[0] = (uint8_t)w_song; shuf_was[1] = (uint8_t)w_arp;
		shuf_was[2] = (uint8_t)w_pump; shuf_was[3] = (uint8_t)w_tex;
	}
	demo_idx = 0;
	if (demo_on == 1) {                          /* the album: the AMB-1 pieces, each in its key */
		demo_n = DEMO_COUNT;
		for (int i = 0; i < demo_n; i++)
			demo_list[i] = (uint8_t)i;
		gl_world(GW_KEY_HOLD, 1.0f);
	} else {
		shuffle_list(now);
		gl_world(GW_KEY_HOLD, 0.0f);
	}
	demo_track(now);
	uncatch_all(in);
	demo_t0 = now;
	demo_gain = 0.0f;
	demo_in = DEMO_SKIPIN_MS;
	demo_out = DEMO_FADE_MS;
	demo_end = DEMO_PIECE_MS;
	demo_skipped = 0;
	if (!playing) {
		playing = 1;
		gl_play(1);
	}
}
/* the track playing in the album or shuffle: its piece, mode (0 none, 1 album, 2 shuffle) and the
 * two seeds that make it (the UI's dice before the deal, the engine's take) */
void glui_track(int *piece, int *mode, uint32_t *ui_seed, uint32_t *fresh_seed)
{
	*piece = demo_on ? demo_list[demo_idx] : -1;
	*mode = demo_on;
	*ui_seed = trk_ui_seed;
	*fresh_seed = trk_fresh_seed;
}

/* ---- MIDI in: controllers, programs, tempo ---- */
static uint8_t midi_recatch;
void glui_midi_cc(int ch, int cc, int value)
{
	if ((unsigned)ch >= 4u)
		return;
	float v = (float)(value < 0 ? 0 : value > 127 ? 127 : value) * (1.0f / 127.0f);
	int p = -1;
	switch (cc) {
	case 1:
		mac_step[ch] = (int)(v * MAC_STEPS + 0.5f);
		gl_macro(ch, v, mac_period[ch]);
		return;
	case 7: p = CP_LEVEL; break;
	case 10: p = CP_PAN; break;
	case 16: p = CP_REGISTER; v = (float)(int)(v * 4.0f + 0.5f) * 0.25f; break;
	case 71: p = CP_CHAR; break;
	case 72: p = CP_LENGTH; break;
	case 73: p = CP_ACTIVITY; break;
	case 74: p = CP_TONE; break;
	case 91: p = CP_VERB; break;
	case 93: p = CP_ECHO; break;
	default: return;
	}
	cur.ch[ch][p] = v;
	dirty = 1;
	midi_recatch = 1;
}
void glui_midi_program(int program)
{
	load_piece(program);
	midi_recatch = 1;
}
void glui_midi_tempo(float bpm)
{
	float v = (bpm - 20.0f) * 0.01f;
	cur.g[GP_TEMPO] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
	dirty = 1;
	midi_recatch = 1;
}

void glui_tick(uint32_t now, const gl_input *in, gl_ui_out *out)
{
	uint16_t btn = in->buttons;
	int fn = DOWN(GB_FN) != 0;
	out->save_slot = 0;
	if (!started) {                               /* the faders are wherever they are */
		started = 1;
		for (int f = 0; f < 4; f++)
			last_pos[f] = in->fader[f];
		layer_shift = fn;
		uncatch_all(in);
		prev_btn = btn;
	}

	for (int b = 0; b < GL_NBTN; b++)
		if (PRESSED(b))
			down_at[b] = now;
	if (PRESSED(GB_FN))
		used_with_fn = 0;

	/* ---- demo mode: its own rocker; any other press ends it (and does nothing else) ---- */
	if (demo_on) {
		uint32_t t = now - demo_t0;
		if ((PRESSED(GB_FFWD) || PRESSED(GB_RWD)) && t + DEMO_SKIP_MS < demo_end) {
			if (PRESSED(GB_RWD))
				demo_idx = (demo_idx + 2 * demo_n - 2) % demo_n;   /* the one before (the switch adds one) */
			demo_end = t + DEMO_SKIP_MS;                  /* to the next: out quickly, from where it is */
			demo_out = DEMO_SKIP_MS;
			demo_skipped = 1;
		}
		int stop = 0;
		for (int b = GB_T1; b <= GB_PLAY; b++)
			stop |= PRESSED(b) != 0;
		if (stop) {
			if (demo_on == 2) {
				/* shuffle switched Playworld's modes on and off at random: back as they were */
				w_song = shuf_was[0]; gl_world(GW_SONG, w_song ? 1.0f : 0.0f);
				w_arp = shuf_was[1]; gl_world(GW_ARP, w_arp ? 1.0f : 0.0f);
				w_pump = shuf_was[2]; gl_world(GW_PUMP, w_pump ? 1.0f : 0.0f);
				w_tex = shuf_was[3]; gl_world(GW_TEXTURE, (float)w_tex);
				gl_world(GW_AUTOF, 0.0f);
				if (w_song)
					world_apply_all(0);
				if (w_arp)
					world_apply_all(1);
				if (w_pump)
					world_apply_all(2);
				if (w_tex)
					world_apply_all(3);
			}
			demo_on = 0;
			gl_world(GW_KEY_HOLD, 0.0f);
			if (PRESSED(GB_PLAY))
				play_world_done = 1;                 /* this press is spent: no rest, no Playworld */
		}
		prev_btn |= (uint16_t)(btn & ((1u << GB_T1) | (1u << GB_T2) | (1u << GB_T3) | (1u << GB_T4) |
		                              (1u << GB_PLAY) | (1u << GB_FFWD) | (1u << GB_RWD)));
		if (demo_on) {
			t = now - demo_t0;
			if (t >= demo_end) {                      /* the switch, in the quiet */
				demo_idx = (demo_idx + 1) % demo_n;
				if (demo_on == 2 && demo_idx == 0)
					shuffle_list(now);               /* the whole list played: a new order */
				demo_track(now);
				uncatch_all(in);
				demo_t0 = now;
				t = 0;
				demo_in = demo_skipped ? DEMO_SKIPIN_MS : DEMO_FADE_MS;
				demo_out = DEMO_FADE_MS;
				demo_end = DEMO_PIECE_MS;
				demo_skipped = 0;
			}
			/* in, and out toward the end: whichever is lower (a skip mid-fade carries on from there) */
			const float gi = t < demo_in ? (float)t / (float)demo_in : 1.0f;
			const float go = t >= demo_end ? 0.0f : demo_end - t < demo_out ? (float)(demo_end - t) / (float)demo_out : 1.0f;
			const float g = gi < go ? gi : go;
			demo_gain = g * g * (3.0f - 2.0f * g);
		}
	}
	if (!demo_on && demo_gain < 1.0f) {               /* ended mid-fade: back up in a second */
		demo_gain += 0.005f;
		demo_gain = demo_gain > 1.0f ? 1.0f : demo_gain;
	}

	if (midi_recatch) {                           /* MIDI moved values: the faders pick them up anew */
		midi_recatch = 0;
		uncatch_all(in);
	}
	/* the faders' layer follows ••; changing it re-arms the soft takeover */
	if (fn != layer_shift) {
		layer_shift = fn;
		uncatch_all(in);
	}

	/* ---- •• : taps (double = HOME); holds are the firmware's power gesture ---- */
	if (RELEASED(GB_FN)) {
		if (!used_with_fn && now - down_at[GB_FN] <= TAP_MS) {
			if (fn_taps && now - fn_tap_at <= DOUBLE_MS) {
				fn_taps = 0;
				go(PG_HOME, chan, in);
			} else {
				fn_taps = 1;
				fn_tap_at = now;
			}
		} else
			fn_taps = 0;
	}

	/* ---- T1..T4 ---- */
	for (int t = 0; t < 4; t++) {
		if (!PRESSED(GB_T1 + t))
			continue;
		if (fn) {
			used_with_fn = 1;
			if (page == PG_HOME) {                     /* rotate this channel's voice */
				cur.voice[t] = (uint8_t)((cur.voice[t] + 1) % GV_COUNT);
				dirty = 1;
				show_idx = cur.voice[t];
				say(SH_INDEX, now, 1200);
			} else if (page == PG_HARMONY) {
				/* the reverb: T1 Hall, T2 Chamber, T3 Shimmer, T4 Bloom; Shimmer's T again
				 * steps its interval (octave, fifth, seventh) */
				/* the reverb already chosen, again: Shimmer steps its interval; the others turn
				 * their reverse swells on or off (saved with the piece) */
				if (t == cur.space[GL_SP_REVERB] && t == GR_SHIMMER)
					cur.space[GL_SP_SHIMMER] = (uint8_t)((cur.space[GL_SP_SHIMMER] + 1) % GS_COUNT);
				else if (t == cur.space[GL_SP_REVERB])
					cur.space[GL_SP_FLAGS] ^= GL_FLAG_REVERSE;
				else
					cur.space[GL_SP_REVERB] = (uint8_t)t;
				dirty = 1;
				say(SH_REVERB, now, 1500);
			} else if ((page == PG_CH || page == PG_EXT) && t == chan) {
				go(page == PG_CH ? PG_EXT : PG_CH, t, in);
			} else {
				go(PG_EXT, t, in);
			}
		} else if (page == PG_WORLD) {
			world_press(t, now, in);
		} else {
			go(PG_CH, t, in);
		}
	}

	/* ---- PLAY ---- */
	if (PRESSED(GB_PLAY)) {
		if (fn) {
			used_with_fn = 1;
			if (play_taps && now - play_tap_at <= DOUBLE_MS) {
				play_taps = 0;
				load_piece(piece + 1);
				uncatch_all(in);
				show_idx = piece;
				say(SH_INDEX, now, 1500);
			} else {
				play_taps = 1;
				play_tap_at = now;
			}
		} else
			play_world_done = 0;                     /* a tap rests or grows (on release); held 2 s: Playworld */
	}
	if (DOWN(GB_PLAY) && !fn && !play_world_done) {
		uint32_t held = now - down_at[GB_PLAY];
		if (held >= WORLD_HOLD_MS) {
			play_world_done = 1;
			play_prev_page = page;
			if (page == PG_WORLD)
				go(PG_HOME, chan, in);
			else {
				go(PG_WORLD, chan, in);
				world_anim(1, 0, 3, now);            /* a sweep across: you are in */
			}
		} else if (held >= TAP_MS) {
			show_val = (float)held / (float)WORLD_HOLD_MS;   /* the hold filling up */
			say(SH_VALUE, now, 100);
		}
	}
	if (RELEASED(GB_PLAY) && !fn && !play_world_done && now - down_at[GB_PLAY] < WORLD_HOLD_MS &&
	    !(prev_btn & (1u << GB_FN))) {
		playing = !playing;
		gl_play(playing);
	}
	if (DOWN(GB_PLAY) && !fn && play_world_done == 1 && !demo_on) {
		/* still held past Playworld: the play row filling again (blinking slowly) toward the
		 * album; at 10 s it flashes (let go: the album) and fills again, blinking fast, toward
		 * shuffle; at 20 s a light runs round and round (let go: shuffle) */
		uint32_t held = now - down_at[GB_PLAY];
		if (held >= SHUF_HOLD_MS) {
			show_dir = 1;
			say(SH_SWEEP, now, 100);
		} else if (held >= DEMO_HOLD_MS) {
			show_val = (now / 600u) & 1u ? 1.0f : (float)(held - DEMO_HOLD_MS) / (float)(SHUF_HOLD_MS - DEMO_HOLD_MS);
			say((now / 120u) & 1u ? SH_VALUE : SH_FIND, now, 100);
		} else if (held >= WORLD_HOLD_MS + 500u) {
			show_val = (float)(held - WORLD_HOLD_MS) / (float)(DEMO_HOLD_MS - WORLD_HOLD_MS);
			say((now / 400u) & 1u ? SH_VALUE : SH_FIND, now, 100);
		}
	}
	if (RELEASED(GB_PLAY) && play_world_done == 1 && !(prev_btn & (1u << GB_FN)) &&
	    now - down_at[GB_PLAY] >= DEMO_HOLD_MS && !demo_on) {
		go(play_prev_page, chan, in);                 /* (the Playworld step on the way is undone) */
		demo_start(now - down_at[GB_PLAY] >= SHUF_HOLD_MS ? 2 : 1, now, in);
	}
	if (RELEASED(GB_PLAY))
		play_world_done = 0;
	if (play_taps && now - play_tap_at > DOUBLE_MS) {  /* one •• + PLAY: the harmony page */
		play_taps = 0;
		go(PG_HARMONY, chan, in);
	}

	/* ---- FFWD: freeze (tap latches, hold is momentary) on Home and Harmony; on a channel page,
	 * one step more of that channel's macro; •• + FFWD: randomize, everywhere ---- */
	if (PRESSED(GB_FFWD)) {
		if (fn) {
			/* Home only: a tap randomizes when it lets go; held 3 s brings in the piece's
			 * macro scene. Elsewhere •• + FFWD is kept for later */
			used_with_fn = 1;
			ffwd_shift = page == PG_HOME;
			ffwd_done = 0;
			if (page == PG_HARMONY) {           /* the key's travel: off, fifths, lifts, relative, drops */
				uint8_t f = cur.space[GL_SP_FLAGS], k = (uint8_t)((GL_FLAG_KEY(f) + 1u) % GL_NKEYWAY);
				cur.space[GL_SP_FLAGS] = (uint8_t)((f & ~(7u << 2)) | (unsigned)(k << 2));
				dirty = 1;
				show_idx = k;
				say(SH_INDEX, now, 1200);
			} else if (on_channel() && cur.voice[chan] == GV_KIT) {   /* the Kit: its groove */
				uint8_t x = cur.chx[chan], v = (uint8_t)((GL_CHX_GRV(x) + 1u) % GL_NGROOVE);
				cur.chx[chan] = (uint8_t)((x & ~(15u << 2)) | (unsigned)(v << 2));
				dirty = 1;
				show_idx = v;
				say(SH_INDEX, now, 1200);
			} else if (on_channel()) {          /* this channel's interval: none, 3rd, 5th, 7th, 9th */
				uint8_t x = cur.chx[chan], v = (uint8_t)((GL_CHX_IVL(x) + 1u) % GL_NIVL);
				cur.chx[chan] = (uint8_t)((x & ~(15u << 2)) | (unsigned)(v << 2));
				dirty = 1;
				show_idx = v;
				say(SH_INDEX, now, 1200);
			}
		} else if (page == PG_HARMONY) {        /* the echo: longer (glides there, tape-like) */
			echo_step(1, now);
		} else if (on_channel()) {
			if (mac_step[chan] < MAC_STEPS)
				mac_step[chan]++;
			macro_post(chan);
			show_val = (float)mac_step[chan] / MAC_STEPS;
			say(SH_VALUE, now, SHOW_MS);
		} else if (page == PG_HOME) {
			freeze_on = 1;
			gl_freeze(1);
			ffwd_froze = 1;
		}
	}
	if (DOWN(GB_FFWD) && ffwd_shift && !ffwd_done) {
		uint32_t held = now - down_at[GB_FFWD];
		if (held >= SCENE_HOLD_MS) {
			ffwd_done = 1;
			for (int c = 0; c < 4; c++) {
				mac_step[c] = scene_mac[piece][c];
				mac_period[c] = scene_per[piece][c];
				macro_post(c);
			}
			say(SH_MACROS, now, 1500);
		} else if (held >= TAP_MS) {
			show_val = (float)held / (float)SCENE_HOLD_MS;   /* the hold filling up */
			say(SH_VALUE, now, 100);
		}
	}
	if (RELEASED(GB_FFWD) && ffwd_shift) {
		ffwd_shift = 0;
		if (!ffwd_done && now - down_at[GB_FFWD] < TAP_MS) {
			undo_patch = cur;
			undo_piece = piece;
			undo_valid = 1;
			seed = seed * 1664525u + 1013904223u + now;
			gl_randomize(&cur, seed);
			dirty = 1;
			uncatch_all(in);
			show_dir = 1;
			say(SH_SWEEP, now, 400);
		}
	}
	if (RELEASED(GB_FFWD) && ffwd_froze) {
		uint32_t held = now - down_at[GB_FFWD];
		ffwd_froze = 0;
		if (held < FREEZE_HOLD_MS) {
			/* a tap: toggles the latch (a tap while latched lets go) */
			freeze_latched = !freeze_latched;
			freeze_on = freeze_latched;
		} else {
			freeze_latched = 0;
			freeze_on = 0;
		}
		gl_freeze(freeze_on);
	}

	/* ---- RWD: the slowdown on Home and Harmony; on a channel page, held slows that channel's
	 * macro and a double tap turns it off; •• + RWD: undo (tap) or save (held 2 s) ---- */
	if (PRESSED(GB_RWD)) {
		rwd_mac = 0;
		if (fn) {
			/* Home only: a tap undoes, held 3 s saves. Elsewhere kept for later */
			used_with_fn = 1;
			rwd_shift = page == PG_HOME;
			rwd_saved = 0;
			if (page == PG_HARMONY) {           /* the macros' shape: glide, bar steps, long rise */
				uint8_t f = cur.space[GL_SP_FLAGS], k = (uint8_t)((GL_FLAG_SHAPE(f) + 1u) % GL_NSHAPE);
				cur.space[GL_SP_FLAGS] = (uint8_t)((f & ~(3u << 5)) | (unsigned)(k << 5));
				dirty = 1;
				show_idx = k;
				say(SH_INDEX, now, 1200);
			} else if (on_channel() && cur.voice[chan] == GV_KIT) {   /* the Kit: its tuning */
				uint8_t v = (uint8_t)((cur.space[GL_SP_KIT] + 1u) % (unsigned)gl_kit_ntunings());
				cur.space[GL_SP_KIT] = v;
				dirty = 1;
				show_idx = v;
				say(SH_INDEX, now, 1200);
			} else if (on_channel()) {          /* this channel's time division: 1, 1/2, 1/3, 1/4 */
				uint8_t x = cur.chx[chan], v = (uint8_t)((GL_CHX_DIV(x) + 1u) & 3u);
				cur.chx[chan] = (uint8_t)((x & ~3u) | v);
				dirty = 1;
				show_idx = v;
				say(SH_INDEX, now, 1200);
			}
		} else if (on_channel()) {
			rwd_shift = 0;
			rwd_mac = 1;
			rwd_from = mac_period[chan];
		} else if (page == PG_HOME && (!playing || freeze_on)) {
			rwd_shift = 0;
			gl_crank();                          /* resting or frozen: a turn of the crank */
		} else if (page == PG_HOME) {
			rwd_shift = 0;
			slow_on = 1;
			gl_slow(1);
		} else {
			rwd_shift = 0;
			if (page == PG_HARMONY)             /* the echo: shorter */
				echo_step(-1, now);
		}
	}
	if (DOWN(GB_RWD) && rwd_mac && now - down_at[GB_RWD] >= TAP_MS) {
		/* held: the glide doubles in length every 1.5 s, down to one every 64 s */
		float k = (float)(now - down_at[GB_RWD] - TAP_MS) / MAC_SLOW_MS, p = rwd_from;
		for (; k >= 1.0f && p < MAC_SLOWEST; k -= 1.0f)
			p *= 2.0f;
		p *= 1.0f + k;                        /* (roughly 2^k between doublings) */
		p = p > MAC_SLOWEST ? MAC_SLOWEST : p;
		if (p != mac_period[chan]) {
			mac_period[chan] = p;
			macro_post(chan);
		}
		/* how slow, as a bar: 8 s empty .. 64 s full, a third per doubling */
		float lv = 0.0f, q = MAC_PERIOD;
		for (; q * 2.0f <= p; q *= 2.0f)
			lv += 1.0f;
		show_val = (lv + p / q - 1.0f) / 3.0f;
		say(SH_VALUE, now, SHOW_MS);
	}
	if (DOWN(GB_RWD) && rwd_shift && !rwd_saved && now - down_at[GB_RWD] >= SAVE_HOLD_MS) {
		rwd_saved = 1;
		saved[piece] = cur;
		saved_valid[piece] = 1;
		for (int c = 0; c < 4; c++) {           /* the macros as they are: its scene */
			scene_mac[piece][c] = mac_step[c];
			scene_per[piece][c] = mac_period[c];
		}
		out->save_slot = (uint8_t)(piece + 1);
		say(SH_FLASH, now, 900);
	}
	if (RELEASED(GB_RWD)) {
		if (rwd_shift) {
			if (!rwd_saved && undo_valid) {         /* undo, and undo again is redo */
				gl_patch t = cur;
				int tp = piece;
				cur = undo_patch;
				piece = undo_piece;
				undo_patch = t;
				undo_piece = tp;
				dirty = 1;
				uncatch_all(in);
				show_dir = -1;
				say(SH_SWEEP, now, 400);
			}
			rwd_shift = 0;
		} else if (rwd_mac) {
			rwd_mac = 0;
			if (now - down_at[GB_RWD] < TAP_MS) {
				if (rwd_taps && now - rwd_tap_at <= DOUBLE_MS) {
					rwd_taps = 0;                   /* double tap: the macro off */
					mac_step[chan] = 0;
					mac_period[chan] = MAC_PERIOD;
					macro_post(chan);
					show_val = 0.0f;
					say(SH_VALUE, now, SHOW_MS);
				} else {
					rwd_taps = 1;
					rwd_tap_at = now;
				}
			} else
				rwd_taps = 0;
		} else if (slow_on) {
			slow_on = 0;
			gl_slow(0);
		}
	}

	/* ---- VOL: the level; with •• the distortion ----
	 * •• + VOL- tap: amount (off, 1-4); •• + VOL+ tap: type. Held 3 s: VOL- turns it off and
	 * puts the whole stack back to default, VOL+ moves it before / after the delay and reverb.
	 * Taps act on release, so a hold is never also a tap. */
	for (int k = 0; k < 2; k++) {
		const int b = k ? GB_VOLUP : GB_VOLDN;
		if (PRESSED(b)) {
			if (fn) {
				used_with_fn = 1;
				vol_shift[k] = 1;
				vol_done[k] = 0;
			} else {
				vol_shift[k] = 0;
				vol += k ? 1 : -1;
				vol = vol < 0 ? 0 : vol > VOL_STEPS ? VOL_STEPS : vol;
				show_val = (float)vol / VOL_STEPS;
				say(SH_VALUE, now, SHOW_MS);
			}
		}
		if (DOWN(b) && vol_shift[k] && !vol_done[k]) {
			uint32_t held = now - down_at[b];
			if (held >= DIST_HOLD_MS) {
				vol_done[k] = 1;
				if (k) {
					dist_post = (dist_post + 1) % 4;   /* before, after, the reverb only, the drums only */
				} else {
					dist_type = 0;
					dist_amt = 0;
					dist_post = 0;
				}
				gl_dist(dist_type, dist_amt, dist_post);
				say(SH_DIST, now, 1500);
			} else if (held >= TAP_MS) {
				show_val = (float)held / (float)DIST_HOLD_MS;   /* the hold filling up */
				say(SH_HOLD, now, 100);
			}
		}
		if (RELEASED(b) && vol_shift[k]) {
			if (!vol_done[k]) {
				if (k)
					dist_type = (dist_type + 1) % GD_COUNT;
				else
					dist_amt = (dist_amt + 1) % 5;
				gl_dist(dist_type, dist_amt, dist_post);
				say(SH_DIST, now, 1500);
			}
			vol_shift[k] = 0;
		}
	}

	/* ---- faders ---- */
	for (int f = 0; f < 4; f++) {
		float pos = in->fader[f];
		float d = pos - last_pos[f];
		int moved = d > MOVE || d < -MOVE;
		if (!moved)
			continue;
		last_pos[f] = pos;
		if (fn)
			used_with_fn = 1;
		target t = map(f, layer_shift);
		float p = snap(&t, pos), v = *t.val;
		if (!caught[f]) {
			int side = p < v ? -1 : 1;
			if ((p - v < CATCH && v - p < CATCH) || side != side_at_entry[f])
				caught[f] = 1;                         /* reached it, or passed through it */
		}
		if (caught[f]) {
			if (*t.val != p) {
				*t.val = p;
				if (page == PG_WORLD && !layer_shift) {
					if (t.val != &w_sink)
						world_apply(w_focus, w_focus == 0 ? w_song - 1 : w_focus == 2 ? w_pump - 1 : 0, f);   /* (arp, textures: one page) */
				} else
					dirty = 1;
			}
			show_val = p;
			say(SH_VALUE, now, SHOW_MS);
		} else {
			show_val = v;                              /* where to go */
			say(SH_FIND, now, SHOW_MS);
		}
	}

	if (dirty) {
		gl_post_patch(&cur);
		dirty = 0;
	}

	/* ---- LEDs ---- */
	uint32_t tri = now % 2000u;                    /* a slow breath for HOME */
	uint8_t breath = (uint8_t)(20 + (tri < 1000u ? tri : 2000u - tri) * 60u / 1000u);
	for (int t = 0; t < 4; t++) {
		uint8_t v = 0;
		switch (page) {
		case PG_HOME: v = breath; break;
		case PG_CH: v = t == chan ? 255 : 0; break;
		case PG_EXT: v = t == chan && ((now / 250u) & 1u) ? 255 : 0; break;
		case PG_HARMONY: v = (uint8_t)(t == (int)((now / 300u) & 3u) ? 255 : 40); break;
		case PG_WORLD: {
			int on = (t == 0 && w_song) || (t == 1 && w_arp) || (t == 2 && w_pump) || (t == 3 && w_tex);
			v = on ? 255 : 0;
			if (w_focus >= 0 && t == world_page_light(w_focus) && (w_focus != t || on)) {
				uint32_t ph = now % 800u, tri2 = ph < 400u ? ph : 800u - ph;
				if (t != w_focus)                  /* the page: a dim pulse (over a bright one, a deeper one) */
					v = on ? (uint8_t)(90 + tri2 * 165u / 400u) : (uint8_t)(20 + tri2 * 60u / 400u);
			}
			break;
		}
		}
		out->led_t[t] = v;
	}
	if (demo_on && show == SH_MUSIC)                  /* the demo: the piece's number on the track row */
		index_pic(out->led_t, piece, now);
	if (page == PG_WORLD && w_anim_kind && now - w_anim_t0 < 1000u) {
		/* a second showing what the press did: a light gliding from the T to its page, a light
		 * fading out, or (arp: not yet) three quick blinks */
		float x = (float)(now - w_anim_t0) * (1.0f / 1000.0f), sx = x * x * (3.0f - 2.0f * x);
		for (int t = 0; t < 4; t++) {
			float v = 0.0f;
			if (w_anim_kind == 1) {
				float pos = (float)w_anim_from + (float)(w_anim_to - w_anim_from) * sx, d = (float)t - pos;
				v = 1.0f - (d < 0.0f ? -d : d) * 1.3f;
			} else if (w_anim_kind == 2)
				v = t == w_anim_from ? 1.0f - sx : 0.0f;
			else
				v = t == w_anim_from && ((now - w_anim_t0) / 110u) % 2u == 0u && x < 0.66f ? 1.0f : 0.0f;
			out->led_t[t] = (uint8_t)(v > 0.0f ? (v > 1.0f ? 255.0f : 255.0f * v) : 0.0f);
		}
	} else if (w_anim_kind && now - w_anim_t0 >= 1000u)
		w_anim_kind = 0;

	if (show != SH_MUSIC && now >= show_until)
		show = SH_MUSIC;
	switch (show) {
	case SH_VALUE:
		bar(out->led_p, show_val);
		break;
	case SH_FIND:                                   /* blink the value you are looking for */
		bar(out->led_p, show_val);
		if ((now / 120u) & 1u)
			memset(out->led_p, 0, 4);
		break;
	case SH_INDEX:
		index_pic(out->led_p, show_idx, now);
		break;
	case SH_SWEEP: {                                /* randomize sweeps up, undo sweeps down */
		int k = (int)((now - flash_t0) / 100u) & 3;
		memset(out->led_p, 0, 4);
		out->led_p[show_dir > 0 ? k : 3 - k] = 255;
		break;
	}
	case SH_DIST: {
		/* both positions: the type on the track row (the number picture), the amount as a
		 * bar on the play row, filling from the •• end before the effects and from the PLAY
		 * end after them (off: a dim LED at that end) */
		index_pic(out->led_t, dist_type, now);
		uint8_t p[4];
		if (dist_amt)
			bar(p, (float)dist_amt * 0.25f);
		else {
			memset(p, 0, 4);
			p[0] = 30;
		}
		for (int i = 0; i < 4; i++)
			out->led_p[i] = dist_post == 1 ? p[3 - i] : p[i];
		if ((dist_post == 2 && ((now / 250u) & 1u)) || (dist_post == 3 && ((now / 90u) & 1u)))   /* the reverb only: the bar blinks; the drums only: fast */
			memset(out->led_p, 0, 4);
		break;
	}
	case SH_REVERB:                                 /* the reverb on the track row, Shimmer's interval below */
		memset(out->led_t, 0, 4);
		out->led_t[cur.space[GL_SP_REVERB] & 3] = 255;
		memset(out->led_p, 0, 4);
		if (cur.space[GL_SP_REVERB] == GR_SHIMMER)
			index_pic(out->led_p, cur.space[GL_SP_SHIMMER], now);
		break;
	case SH_HOLD:                                   /* •• + VOL held: filling toward 3 s */
		index_pic(out->led_t, dist_type, now);
		bar(out->led_p, show_val);
		break;
	case SH_MACROS:                                 /* the macro scene in or stored: each channel's step */
		for (int c = 0; c < 4; c++) {
			out->led_t[c] = ((now - flash_t0) / 150u) & 1u ? 0 : 255;
			out->led_p[c] = (uint8_t)(mac_step[c] * 63u);
		}
		break;
	case SH_FLASH: {                                /* saved: three flashes on all eight */
		uint8_t on = ((now - flash_t0) / 150u) & 1u ? 0 : 255;
		memset(out->led_p, on, 4);
		memset(out->led_t, on, 4);
		break;
	}
	default:
		if (gl_frozen) {
			memset(out->led_p, 70, 4);              /* frozen: a still glow */
		} else {
			float sp = gl_speed;
			for (int c = 0; c < 4; c++) {
				float g = gl_glow[c];
				int v = (int)(g * 255.0f * sp);
				out->led_p[c] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v);
			}
		}
		break;
	}

	out->volume = (float)vol / VOL_STEPS;
	out->volume *= out->volume;                     /* steps that sound even */
	out->volume *= demo_gain;                       /* the demo's fades */
	out->fade = demo_gain;
	out->page = (uint8_t)page;
	out->channel = (uint8_t)chan;
	out->piece = (uint8_t)piece;
	out->dist_type = (uint8_t)dist_type;
	out->dist_amt = (uint8_t)dist_amt;
	out->dist_post = (uint8_t)dist_post;
	out->reverb = cur.space[GL_SP_REVERB];
	out->shimmer = cur.space[GL_SP_SHIMMER];
	for (int c = 0; c < 4; c++) {
		out->macro[c] = (uint8_t)mac_step[c];
		out->macro_period[c] = mac_period[c];
	}
	out->world_song = (uint8_t)w_song;
	out->world_arp = (uint8_t)w_arp;
	out->world_pump = (uint8_t)w_pump;
	out->world_tex = (uint8_t)w_tex;
	out->world_focus = (int8_t)w_focus;
	out->demo = (uint8_t)demo_on;
	out->demo_ms = demo_on ? now - demo_t0 : 0;
	(void)world_any_on;
	prev_btn = btn;
}

/* As at power-on, on piece i: the album and shuffle, Playworld, the distortion, freeze, slow and
 * rest all off, the piece as stock (or as saved), Home. The saved pieces stay. The firmware calls
 * it on every ON: with USB in, "off" is only standby (the chip cannot power off on USB), and every
 * change made would otherwise carry over, where on battery off is off. */
void glui_restart(int i)
{
	demo_on = 0;
	demo_gain = 1.0f;
	w_song = w_arp = w_pump = w_tex = 0;
	w_focus = -1;
	gl_world(GW_SONG, 0.0f);
	gl_world(GW_ARP, 0.0f);
	gl_world(GW_PUMP, 0.0f);
	gl_world(GW_TEXTURE, 0.0f);
	gl_world(GW_AUTOF, 0.0f);
	gl_world(GW_KEY_HOLD, 0.0f);
	dist_type = dist_amt = dist_post = 0;
	gl_dist(0, 0, 0);
	freeze_on = freeze_latched = slow_on = 0;
	gl_freeze(0);
	gl_slow(0);
	if (!playing) {
		playing = 1;
		gl_play(1);
	}
	load_piece(i);
	undo_valid = 0;
	page = PG_HOME;
	show = SH_MUSIC;
	dirty = 1;
}

/* The SP-1's faders come raw from its ADC, and the controls take a change of 0.8 % as a hand
 * (a value shown, a light flashing). Each is smoothed (~25 ms) and held still until it moves more
 * than 0.4 %: noise never gets through, a hand does at once. `hold`: keep them where they are (the
 * firmware's readings while FFWD or RWD is held are corrected for the rail's sag, not clean). */
void glui_faders_condition(gl_input *in, int hold)
{
	static float y[4], o[4];
	static int have;
	for (int f = 0; f < 4; f++) {
		if (!have)
			y[f] = o[f] = in->fader[f];
		else if (!hold) {
			y[f] += 0.35f * (in->fader[f] - y[f]);
			if (y[f] - o[f] > 0.004f || o[f] - y[f] > 0.004f)
				o[f] = y[f];
		}
		in->fader[f] = o[f];
	}
	have = 1;
}

#ifdef GL_TEST_HOOKS
/* ---- the hardware test's hooks (glag_test.h): each scenario starts from a known state, set the
 * way the gestures set it. The test build and its host twin only; nothing here saves. ---- */
static void test_world(int sec, int st)
{
	int *w = sec == 0 ? &w_song : sec == 1 ? &w_arp : sec == 2 ? &w_pump : &w_tex;
	*w = st;
	if (sec == 0)
		gl_world(GW_SONG, st ? 1.0f : 0.0f);
	else if (sec == 1)
		gl_world(GW_ARP, st ? 1.0f : 0.0f);
	else if (sec == 2 && !st)
		gl_world(GW_PUMP, 0.0f);
	else if (sec == 3)
		gl_world(GW_TEXTURE, (float)st);
	if (st) {
		world_apply_all(sec);
		w_focus = sec;
	} else
		w_focus = w_song ? 0 : w_arp ? 1 : w_pump ? 2 : w_tex ? 3 : -1;
}
void glui_test(int op, int a, int b, float v, uint32_t now)
{
	gl_input in;
	for (int f = 0; f < 4; f++)
		in.fader[f] = last_pos[f];
	in.buttons = 0;
	switch (op) {
	case GT_RESET:                                   /* as at power-on, on piece a */
		demo_piece_ms = 120000u;
		glui_restart(a);
		go(PG_HOME, 0, &in);
		break;
	case GT_PIECE: load_piece(a); break;
	case GT_VOICE: cur.voice[a & 3] = (uint8_t)((unsigned)b % GV_COUNT); dirty = 1; break;
	case GT_CH: cur.ch[a & 3][b] = clampf_ui(v); dirty = 1; break;
	case GT_G: cur.g[b] = clampf_ui(v); dirty = 1; break;
	case GT_SPACE: cur.space[a] = (uint8_t)b; dirty = 1; break;
	case GT_DIST:
		dist_type = a;
		dist_amt = b;
		dist_post = (int)v;
		gl_dist(dist_type, dist_amt, dist_post);
		break;
	case GT_WORLD: test_world(a & 3, b); break;
	case GT_WORLDV: {                                /* a: the section, b: page * 4 + fader */
		const int pg = (b >> 2) & 3, f = b & 3;
		float *p = a == 0 ? &wv_song[pg][f] : a == 1 ? &wv_arp[f] : a == 2 ? &wv_pump[pg & 1][f] : &wv_tex[w_tex][f];
		*p = clampf_ui(v);
		world_apply(a, pg, f);
		break;
	}
	case GT_FREEZE: freeze_on = freeze_latched = a != 0; gl_freeze(a != 0); break;
	case GT_SLOW: slow_on = a != 0; gl_slow(a != 0); break;
	case GT_MACRO: mac_step[a & 3] = b; mac_period[a & 3] = v; macro_post(a & 3); break;
	case GT_DEMO:                                    /* a: 1 the album, 2 shuffle; b: each track, ms */
		demo_piece_ms = (uint32_t)b;
		demo_start(a, now, &in);
		break;
	case GT_PAGE:                                    /* a: the page, b: the channel (Playworld: the section) */
		if (a == PG_WORLD)
			w_focus = b;
		go(a, a == PG_WORLD ? chan : b & 3, &in);
		break;
	case GT_SEED: seed = (uint32_t)a; break;
	}
}
#endif
