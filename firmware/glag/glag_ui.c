/* SPDX-License-Identifier: MIT
 * Glag controls (glag_ui.h). Main thread only.
 */
#include "glag_ui.h"
#include <string.h>

/* ---- timing (ms) ---- */
#define TAP_MS 300u          /* a press shorter than this is a tap */
#define DOUBLE_MS 400u       /* two taps closer than this are a double tap */
#define FREEZE_HOLD_MS 400u  /* FFWD held longer than this freezes only while held */
#define SAVE_HOLD_MS 2000u   /* •• + RWD held this long saves */
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
static uint8_t rwd_shift, rwd_saved, ffwd_froze, started;
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
enum { SH_MUSIC, SH_VALUE, SH_FIND, SH_INDEX, SH_SWEEP, SH_FLASH, SH_DIST, SH_HOLD };
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
	}
	return t;
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
			} else if ((page == PG_CH || page == PG_EXT) && t == chan) {
				go(page == PG_CH ? PG_EXT : PG_CH, t, in);
			} else {
				go(PG_EXT, t, in);
			}
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
		} else {
			playing = !playing;
			gl_play(playing);
		}
	}
	if (play_taps && now - play_tap_at > DOUBLE_MS) {  /* one •• + PLAY: the harmony page */
		play_taps = 0;
		go(PG_HARMONY, chan, in);
	}

	/* ---- FFWD: freeze (tap latches, hold is momentary) on Home and Harmony; on a channel page,
	 * one step more of that channel's macro; •• + FFWD: randomize, everywhere ---- */
	if (PRESSED(GB_FFWD)) {
		if (fn) {
			used_with_fn = 1;
			undo_patch = cur;
			undo_piece = piece;
			undo_valid = 1;
			seed = seed * 1664525u + 1013904223u + now;
			gl_randomize(&cur, seed);
			dirty = 1;
			uncatch_all(in);
			show_dir = 1;
			say(SH_SWEEP, now, 400);
		} else if (on_channel()) {
			if (mac_step[chan] < MAC_STEPS)
				mac_step[chan]++;
			macro_post(chan);
			show_val = (float)mac_step[chan] / MAC_STEPS;
			say(SH_VALUE, now, SHOW_MS);
		} else {
			freeze_on = 1;
			gl_freeze(1);
			ffwd_froze = 1;
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
			used_with_fn = 1;
			rwd_shift = 1;
			rwd_saved = 0;
		} else if (on_channel()) {
			rwd_shift = 0;
			rwd_mac = 1;
			rwd_from = mac_period[chan];
		} else {
			rwd_shift = 0;
			slow_on = 1;
			gl_slow(1);
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
					dist_post = !dist_post;
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
		}
		out->led_t[t] = v;
	}

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
			out->led_p[i] = dist_post ? p[3 - i] : p[i];
		break;
	}
	case SH_HOLD:                                   /* •• + VOL held: filling toward 3 s */
		index_pic(out->led_t, dist_type, now);
		bar(out->led_p, show_val);
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
	out->page = (uint8_t)page;
	out->channel = (uint8_t)chan;
	out->piece = (uint8_t)piece;
	out->dist_type = (uint8_t)dist_type;
	out->dist_amt = (uint8_t)dist_amt;
	out->dist_post = (uint8_t)dist_post;
	for (int c = 0; c < 4; c++) {
		out->macro[c] = (uint8_t)mac_step[c];
		out->macro_period[c] = mac_period[c];
	}
	prev_btn = btn;
}
