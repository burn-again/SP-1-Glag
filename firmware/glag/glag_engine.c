/* SPDX-License-Identifier: MIT
 * Glag engine (glag_engine.h). One compilation unit; build it at -O2.
 *
 * Cost model (nRF52840, 64 MHz, 48 kHz: ~1333 cycles per stereo frame):
 *   - oscillators read a 1024-point sine table in RAM, phases are uint32 (wrap for free);
 *   - envelopes, indices and pitches are computed once per 24-sample sub-block and
 *     interpolated linearly inside it;
 *   - the echo runs at half rate and both delays store int16;
 *   - no double, no libm in the audio path.
 */
#include "glag_engine.h"
#include <string.h>

#define BARRIER() __asm__ volatile("" ::: "memory")

/* ================================================================ fast math */

/* 4096 points, read without interpolation: the error is harmonics ~60 dB down, and it is
 * three instructions instead of ten (16 KB of RAM well spent on this CPU). */
#define TBL 4096
#define TBL_SHIFT 20
static float SIN[TBL + 1];

static inline float sin_u(uint32_t ph) { return SIN[ph >> TBL_SHIFT]; }   /* full turn = 2^32 */

/* 2^x, |x| < 60. Round-to-nearest split, 5th-order series on [-0.5, 0.5]: exact at 0, so
 * coefficients just below 1 come out right (a decay must not stop or grow). */
static float fexp2(float x)
{
	int i = (int)(x + (x >= 0.0f ? 0.5f : -0.5f));
	float y = (x - (float)i) * 0.69314718f;
	float p = 1.0f + y * (1.0f + y * (0.5f + y * (0.16666667f + y * (0.04166667f + y * 0.00833333f))));
	union { float f; int32_t i; } u;
	u.f = p;
	u.i += i * (1 << 23);
	return u.f;
}
static inline float mtof(float m) { return 440.0f * fexp2((m - 69.0f) * (1.0f / 12.0f)); }
static inline uint32_t hz_inc(float hz) { return (uint32_t)(hz * 89478.485f); }   /* 2^32 / 48000 */
static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* coefficient that falls by 60 dB over t seconds, applied every `dt` seconds */
static inline float dcoef(float t, float dt) { return fexp2(-9.9658f * dt / t); }
/* log2(x), x > 0: exponent from the bits, a cubic for the mantissa (control rate only) */
static float flog2(float x)
{
	union { float f; int32_t i; } u;
	u.f = x;
	float e = (float)(((u.i >> 23) & 255) - 128);
	u.i = (u.i & 0x007FFFFF) | 0x3F800000;                 /* mantissa in [1, 2) */
	float m = u.f;
	return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f + 1.0f;
}
/* lo..hi exponentially */
static inline float expmap(float v, float lo, float hi) { return lo * fexp2(v * flog2(hi / lo)); }
/* a phase increment per sub-block for `hz`, at the current speed */
static inline uint32_t sub_inc(float hz, float spd) { return hz_inc(hz * spd) * GL_SUB; }

/* ================================================================ random */

static uint32_t rng = 0x9E3779B9u;
static inline uint32_t rnd32(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static inline float rnd(void) { return (float)(rnd32() >> 8) * (1.0f / 16777216.0f); }
static inline int rndi(int n) { return (int)((rnd32() >> 8) % (uint32_t)n); }

/* ================================================================ names, steps */

const char *const GL_VOICE_NAME[GV_COUNT] = {"Pad", "Bells", "Bass", "Piano", "Marimba",
                                             "Drone", "Kalimba", "Flute", "Arp", "Swell",
                                             "EP", "Gong", "Pizz", "Shimmer", "Tails"};
const char *const GL_MODE_NAME[GL_NMODES] = {"Ionian", "Dorian", "Phrygian", "Lydian",
                                             "Mixolydian", "Aeolian", "Pent maj", "Pent min"};
static const uint16_t MODE_POOL[GL_NMODES] = {
	0xAB5, /* 0 2 4 5 7 9 11 */  0x6AD, /* 0 2 3 5 7 9 10 */  0x5AB, /* 0 1 3 5 7 8 10 */
	0xAD5, /* 0 2 4 6 7 9 11 */  0x6B5, /* 0 2 4 5 7 9 10 */  0x5AD, /* 0 2 3 5 7 8 10 */
	0x295, /* 0 2 4 7 9 */       0x4A9, /* 0 3 5 7 10 */
};

int gl_cp_steps(int cp) { return cp == CP_REGISTER ? 5 : 0; }
int gl_gp_steps(int gp) { return gp == GP_ROOT ? 12 : gp == GP_MODE ? GL_NMODES : 0; }
int gl_cp_centred(int cp) { return cp == CP_PAN; }
int gl_gp_centred(int gp) { return gp == GP_FILTER; }
float gl_tempo_bpm(float v) { return 30.0f + 90.0f * clampf(v, 0.0f, 1.0f); }
static int stepped(float v, int n) { int k = (int)(v * (float)(n - 1) + 0.5f); return k < 0 ? 0 : k >= n ? n - 1 : k; }

int gl_scale(int mode, int *pcs)
{
	int n = 0;
	uint16_t pool = MODE_POOL[(unsigned)mode < GL_NMODES ? mode : 0];
	for (int i = 0; i < 12; i++)
		if (pool & (1u << i))
			pcs[n++] = i;
	return n;
}

/* ================================================================ state */

volatile float gl_glow[GL_NCH];
volatile float gl_beat;
volatile uint8_t gl_chord_root;
volatile uint8_t gl_playing = 1, gl_frozen;
volatile float gl_speed = 1.0f;
volatile float gl_peak;

/* one oscillator voice: 2-op FM (or 1-op with feedback, or two carriers for the bass) */
typedef struct {
	uint32_t pc, pm, ic, im;       /* carrier / modulator phase and increment */
	float amp, ainc;               /* per-sample amplitude and its ramp */
	float idx, iinc;               /* per-sample modulation index (phase units) and ramp */
	float env, menv;               /* envelope states, per sub-block */
	float note, vel, ratio;
	uint8_t side;                  /* which half of the channel's stereo pair it plays in */
	float hold;                    /* flute: seconds left at full breath */
	float fb;                      /* flute: feedback memory */
	uint8_t stage;                 /* 0 idle, 1 attack, 2 decay / sustain, 3 release */
	/* the newer voices: a second carrier (swell) or modulator (EP's tine), its index, the
	 * seconds since the note began (gong, shimmer, tails) and a pitch bend in semitones */
	uint32_t pc2, pm2, ic2, im2;
	float idx2, iinc2, menv2, t, bend;
} nvoice;
enum { ST_IDLE, ST_ATT, ST_DEC, ST_REL };

#define NV 4
#define KS_N 3
#define KS_LEN 1024
#define BELL_MAX 29
static const uint8_t BELL_LEN[4] = {13, 17, 23, 29};
static const float PAD_FACTOR[NV] = {1.0f, 1.31f, 1.62f, 1.93f};   /* unequal breaths */
/* Voices that spread their notes across two sides of the channel (pad, bells);
 * the rest are mono (their channel pan places them), which saves a buffer and half the mixing. SPREAD: how far each side sits
 * from the channel's pan. */
static const uint8_t STEREO[GV_COUNT] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1};
static const float SPREAD[GV_COUNT] = {0.3f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f, 0.25f,
                                       0.35f, 0.0f, 0.0f, 0.0f, 0.3f, 0.3f};

typedef struct {
	uint8_t voice, want, fading;
	float fade;                    /* 0..1 switch fade */
	float lvl, lvl_inc;            /* level, interpolated */
	nvoice v[NV];
	uint8_t vnext;
	int last_note;
	/* pad */
	float cyc[NV], gap[NV];
	/* bells */
	int8_t loop[4][BELL_MAX];
	uint8_t lpos[4];
	/* bass / drone */
	float glide_note, target_note, swell;
	/* arp */
	uint8_t arp_i, arp_dir;
	/* kalimba */
	int16_t ks[KS_N][KS_LEN];
	uint16_t ks_len[KS_N], ks_i[KS_N], ks_fill[KS_N];
	float ks_g[KS_N], ks_a[KS_N], ks_ea[KS_N], ks_elp[KS_N], ks_vel[KS_N];
	float ks_env[KS_N], ks_kd[KS_N];   /* an estimate of the string's level, for stopping it */
	uint8_t ks_next;
	float glow;
	uint32_t seen_chord;
	uint32_t lfo1, lfo2;           /* drone: index swell and breath */
	/* shimmer: the octave echo waiting to sound */
	float echo_at, echo_note, echo_vel;
} chan;

static gl_patch P;                         /* the patch the render plays */
static gl_patch post_buf[2];
static volatile uint32_t post_seq, post_seen = 0xFFFFFFFFu;

static volatile uint8_t cmd_play = 1, cmd_freeze, cmd_slow;
static volatile uint32_t cmd_next, cmd_panic;
static uint32_t seen_next, seen_panic;

/* harmony */
static int scale_pc[12], scale_n;          /* absolute pitch classes, ascending from the root */
static int root_pc, mode_cur = -1, root_cur = -1;
static int chord_deg, chord_pc[4], chord_n, chord_moves;
static uint32_t chord_count;
static uint16_t scale_mask, chord_mask;     /* bit pc: in the scale / in the chord */
static float chord_timer;
static uint8_t chord_pending;

/* time */
static float step_acc;                     /* 16ths */
static uint32_t step_n;
static float speed = 1.0f, arc;
static uint32_t lfo_vib, lfo_arc1, lfo_arc2, lfo_trem;
static uint8_t playing = 1, frozen, slowing;
static float freeze_in = 1.0f;             /* reverb input gain, ramps to 0 while frozen */

/* ================================================================ harmony */

static void harmony_update(void)
{
	int mode = stepped(P.g[GP_MODE], GL_NMODES);
	int root = stepped(P.g[GP_ROOT], 12);
	if (mode == mode_cur && root == root_cur)
		return;
	int rel[12];
	scale_n = gl_scale(mode, rel);
	scale_mask = 0;
	for (int i = 0; i < scale_n; i++) {
		scale_pc[i] = (rel[i] + root) % 12;
		scale_mask |= (uint16_t)(1u << scale_pc[i]);
	}
	mode_cur = mode;
	root_cur = root;
	root_pc = root;
	if (chord_deg >= scale_n)
		chord_deg = 0;
	chord_pending = 1;                         /* rebuild the chord on the new scale now */
}

static void chord_build(void)
{
	chord_n = (scale_n >= 7 && rnd() < 0.6f) ? 4 : 3;
	chord_mask = 0;
	for (int k = 0; k < chord_n; k++) {
		chord_pc[k] = scale_pc[(chord_deg + 2 * k) % scale_n];
		chord_mask |= (uint16_t)(1u << chord_pc[k]);
	}
	gl_chord_root = (uint8_t)chord_pc[0];
	chord_count++;
}

static void chord_next(void)
{
	/* a gentle walk: IV, V, vi, ii mostly, and home again after a few moves */
	static const int8_t MOVE[6] = {3, 4, 5, 1, -2, 2};
	static const uint8_t W[6] = {30, 22, 20, 14, 7, 7};
	if (chord_moves >= 3 && rnd() < 0.5f) {
		chord_deg = 0;
		chord_moves = 0;
	} else {
		int r = rndi(100), k = 0, acc = W[0];
		while (r >= acc && k < 5)
			acc += W[++k];
		chord_deg = (chord_deg + MOVE[k] + scale_n * 2) % scale_n;
		chord_moves = chord_deg ? chord_moves + 1 : 0;
	}
	chord_build();
}

static inline int in_chord(int pc) { return (chord_mask >> pc) & 1; }
static inline int in_scale(int pc) { return (scale_mask >> pc) & 1; }

/* a melodic note in lo..hi: a chord tone with probability cb, else any scale tone;
 * mostly near the last note, never the same note twice */
static int pick_note(chan *c, int lo, int hi, float cb)
{
	int cand[48], n = 0, want_chord = rnd() < cb;
	int near = c->last_note >= lo && c->last_note <= hi && rnd() < 0.7f;
	for (int m = lo; m <= hi && n < 48; m++) {
		int pc = m % 12;
		if (!(want_chord ? in_chord(pc) : in_scale(pc)) || m == c->last_note)
			continue;
		if (near && (m - c->last_note > 7 || c->last_note - m > 7))
			continue;
		cand[n++] = m;
	}
	if (!n) {
		for (int m = lo; m <= hi && n < 48; m++)
			if (in_scale(m % 12))
				cand[n++] = m;
		if (!n)
			return lo;
	}
	int m = cand[rndi(n)];
	c->last_note = m;
	return m;
}

/* pitch class pc placed nearest to `from`, inside lo..hi (lo >= 0; hi - lo >= 11) */
static int place(int pc, float from, int lo, int hi)
{
	int m = lo + (pc - lo % 12 + 12) % 12;          /* the lowest pc in range */
	int f = (int)from;
	while (m + 12 <= hi && (m + 12 - f) * (m + 12 - f) <= (m - f) * (m - f))
		m += 12;
	return m;
}

/* ================================================================ per-voice ranges */

/* home note of each voice; REGISTER moves it by octaves (-2..+2) */
static const uint8_t HOME[GV_COUNT] = {57, 72, 38, 64, 72, 45, 72, 72, 67, 57, 60, 33, 67, 79, 64};

static int reg_base(const chan *c, int ch)
{
	return HOME[c->voice] + 12 * (stepped(P.ch[ch][CP_REGISTER], 5) - 2);
}

static float eff_activity(int ch) { return clampf(P.ch[ch][CP_ACTIVITY] * (1.0f + 0.6f * arc), 0.0f, 1.0f); }
static float eff_tone(int ch) { return clampf(P.ch[ch][CP_TONE] + 0.15f * arc, 0.0f, 1.0f); }

/* ================================================================ note voices */

#define IDX_SCALE 683565275.6f            /* radians -> phase units */

static void nv_trigger(nvoice *v, float note, float vel, float ratio, int side)
{
	v->note = note;
	v->vel = vel;
	v->ratio = ratio;
	v->side = (uint8_t)(side & 1);
	v->stage = ST_ATT;
	v->menv = 1.0f;
	v->menv2 = 1.0f;
	v->t = 0.0f;
	v->bend = 0.0f;
	/* the attack starts from where the amplitude is: no click on a retrigger */
	v->env = v->amp > 0.0f ? v->amp / (vel > 0.01f ? vel : 0.01f) : 0.0f;
	if (v->env > 1.0f)
		v->env = 1.0f;
	if (v->env < 0.0f)
		v->env = 0.0f;
}

/* ---- the voice budget: at most FM_CAP oscillator voices sound at once, across channels.
 * A new note past the cap fades out the quietest decaying note anywhere (over 1 ms); the
 * pad, the drone and the bass are never taken, but they count. This bounds the worst case
 * whatever four voices are chosen. */
/* Counted in half-voices: a 2-op voice is 2, the three-oscillator swell and EP are 3. */
#define FM_CAP 18
static chan CH[GL_NCH];
static int nslots(int voice)
{
	switch (voice) {
	case GV_PAD: return 3;
	case GV_BELLS: return 4;
	case GV_BASS: return 1;
	case GV_DRONE: return 2;
	case GV_KALIMBA: return 0;
	case GV_GONG: return 2;
	case GV_SHIMMER: return 4;
	default: return 3;
	}
}
static int weight(int voice) { return voice == GV_SWELL || voice == GV_EP ? 3 : 2; }
static inline int sounding(const nvoice *v) { return v->stage != ST_IDLE || v->amp > 0.0001f; }
static int stealable(int voice)
{
	return voice == GV_BELLS || voice == GV_PIANO || voice == GV_MARIMBA || voice == GV_ARP ||
	       voice == GV_FLUTE || voice == GV_EP || voice == GV_GONG || voice == GV_PIZZ ||
	       voice == GV_SHIMMER || voice == GV_TAILS;
}
static int budget_used(void)
{
	int n = 0;
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		int ns = nslots(ch->voice), w = weight(ch->voice);
		for (int i = 0; i < ns; i++)
			if (sounding(&ch->v[i]))
				n += w;
	}
	return n;
}
/* a new note of weight w is about to sound in `keep`: fade the quietest stealable note out
 * if the budget would overflow */
static void make_room_w(const nvoice *keep, int w)
{
	int n = 0;
	nvoice *q = 0;
	float qa = 1e9f;
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		int ns = nslots(ch->voice), st = stealable(ch->voice), cw = weight(ch->voice);
		for (int i = 0; i < ns; i++) {
			nvoice *v = &ch->v[i];
			if (!sounding(v))
				continue;
			n += cw;
			if (st && v != keep && v->amp < qa) {
				qa = v->amp;
				q = v;
			}
		}
	}
	if (n + w > FM_CAP && q) {
		q->stage = ST_IDLE;                    /* its amplitude ramps to 0 in the next ms */
		q->env = 0.0f;
	}
}
static void make_room(const nvoice *keep) { make_room_w(keep, 2); }

/* set this sub-block's ramps toward amplitude `a` and index `i`. A voice that has faded
 * is snapped to exactly zero, or float rounding would leave it rendering forever. */
static inline void ramp(nvoice *v, float a, float i)
{
	if (a <= 0.0f && v->amp < 0.0002f && v->amp > -0.0002f)
		v->amp = v->ainc = 0.0f;
	else
		v->ainc = (a - v->amp) * (1.0f / GL_SUB);
	v->iinc = (i - v->idx) * (1.0f / GL_SUB);
}

static nvoice *alloc(chan *c, int n)
{
	nvoice *best = &c->v[0];
	for (int i = 0; i < n; i++)               /* an idle voice, else the quietest */
		if (c->v[i].stage == ST_IDLE)
			return &c->v[i];
	for (int i = 1; i < n; i++)
		if (c->v[i].amp < best->amp)
			best = &c->v[i];
	return best;
}

/* per sub-block: percussive envelope (attack `att` s, decay to -60 dB in `dec` s) */
static void nv_perc(nvoice *v, float att, float dec, float mdec, float imax, float dt)
{
	if (v->stage == ST_ATT) {
		v->env += dt / att;
		if (v->env >= 1.0f) {
			v->env = 1.0f;
			v->stage = ST_DEC;
		}
	} else if (v->stage == ST_DEC) {
		v->env *= dcoef(dec, dt);
		if (v->env < 0.0005f) {
			v->env = 0.0f;
			v->stage = ST_IDLE;
		}
	}
	v->menv *= dcoef(mdec, dt);
	float a = v->env * v->vel, i = clampf(imax * (0.15f + 0.85f * v->menv), 0.0f, 3.0f) * IDX_SCALE;
	ramp(v, a, i);
}

/* pitch for this sub-block (slowdown bends everything down together) */
static void nv_pitch(nvoice *v, float note)
{
	float hz = mtof(note) * speed;
	v->ic = hz_inc(hz);
	v->im = hz_inc(hz * v->ratio);
}

/* render one 2-op FM voice, adding into `out` (its side of the channel) */
static void nv_render(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc;
	for (uint32_t i = 0; i < GL_SUB; i++) {
		float m = SIN[pm >> TBL_SHIFT] * idx;
		out[i] += sin_u(pc + (uint32_t)(int32_t)m) * amp;
		pm += im;
		pc += ic;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx;
}

/* swell: one modulator shared by two detuned carriers (pc and pc2) */
static void nv_render_dual(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pc2 = v->pc2, pm = v->pm, ic = v->ic, ic2 = v->ic2, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc;
	for (uint32_t i = 0; i < GL_SUB; i++) {
		uint32_t m = (uint32_t)(int32_t)(SIN[pm >> TBL_SHIFT] * idx);
		out[i] += (sin_u(pc + m) + sin_u(pc2 + m)) * amp;
		pm += im;
		pc += ic;
		pc2 += ic2;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pc2 = pc2; v->pm = pm; v->amp = amp; v->idx = idx;
}

/* EP: the body modulator (1:1) and the tine (14:1) both modulating one carrier */
static void nv_render_ep(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, pm2 = v->pm2, ic = v->ic, im = v->im, im2 = v->im2;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, idx2 = v->idx2, ii2 = v->iinc2;
	for (uint32_t i = 0; i < GL_SUB; i++) {
		float m = SIN[pm >> TBL_SHIFT] * idx + SIN[pm2 >> TBL_SHIFT] * idx2;
		out[i] += sin_u(pc + (uint32_t)(int32_t)m) * amp;
		pm += im;
		pm2 += im2;
		pc += ic;
		amp += ai;
		idx += ii;
		idx2 += ii2;
	}
	v->pc = pc; v->pm = pm; v->pm2 = pm2; v->amp = amp; v->idx = idx; v->idx2 = idx2;
}

/* shimmer: a modulator with self-feedback (the buzz, idx2) driving the carrier */
static void nv_render_shimmer(nvoice *v, float *out)
{
	uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
	float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, fbk = v->idx2, fb = v->fb;
	for (uint32_t i = 0; i < GL_SUB; i++) {
		float mo = SIN[(pm + (uint32_t)(int32_t)(fb * fbk)) >> TBL_SHIFT];
		fb = mo;
		out[i] += sin_u(pc + (uint32_t)(int32_t)(mo * idx)) * amp;
		pm += im;
		pc += ic;
		amp += ai;
		idx += ii;
	}
	v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx; v->fb = fb;
}

/* an idle voice among v[from .. from+n-1], else the quietest of them */
static nvoice *alloc_from(chan *c, int from, int n)
{
	nvoice *best = &c->v[from];
	for (int i = from; i < from + n; i++)
		if (c->v[i].stage == ST_IDLE)
			return &c->v[i];
	for (int i = from + 1; i < from + n; i++)
		if (c->v[i].amp < best->amp)
			best = &c->v[i];
	return best;
}

/* ================================================================ channel: voice logic */

static void chan_reset(chan *c, int voice)
{
	memset(c, 0, sizeof *c);
	c->voice = c->want = (uint8_t)voice;
	c->fade = 0.0f;
	c->last_note = -1;
	for (int i = 0; i < NV; i++) {
		c->v[i].side = (uint8_t)(i & 1);
		c->v[i].ratio = 1.0f;
		c->v[i].pc = rnd32();
		c->v[i].pm = rnd32();
		c->v[i].pc2 = rnd32();
		c->v[i].pm2 = rnd32();
		c->gap[i] = 0.3f + 2.5f * (float)i * rnd();   /* the pad comes in one breath at a time */
	}
	c->glide_note = c->target_note = -1.0f;
	for (int k = 0; k < 4; k++)
		for (int s = 0; s < BELL_MAX; s++)
			c->loop[k][s] = -1;
}

/* ---- bells: four tape loops ---- */
static void bells_fill(chan *c, int ch)
{
	float act = P.ch[ch][CP_ACTIVITY];
	for (int k = 0; k < 4; k++) {
		int len = BELL_LEN[k], want = 1 + (int)(act * 0.4f * (float)len), have = 0;
		for (int s = 0; s < len; s++)
			have += c->loop[k][s] >= 0;
		while (have < want) {
			int s = rndi(len);
			if (c->loop[k][s] < 0) {
				c->loop[k][s] = (int8_t)rndi(scale_n + 3);
				have++;
			}
		}
		while (have > want) {
			int s = rndi(len);
			if (c->loop[k][s] >= 0) {
				c->loop[k][s] = -1;
				have--;
			}
		}
	}
}

static const float BELL_RATIO[3][4] = {
	{2.0f, 3.0f, 4.0f, 1.0f},      /* harmonic */
	{3.5f, 2.76f, 4.2f, 1.41f},    /* bell */
	{5.19f, 7.1f, 3.14f, 6.3f},    /* glass */
};

static void bells_beat(chan *c, int ch)
{
	int base = reg_base(c, ch) - 12;
	int zone = stepped(P.ch[ch][CP_CHAR], 3);
	for (int k = 0; k < 4; k++) {
		int len = BELL_LEN[k];
		int8_t d = c->loop[k][c->lpos[k]];
		if (d >= 0 && rnd() < 0.6f + 0.4f * eff_activity(ch)) {
			int oct = d / scale_n, deg = d % scale_n;
			int note = base + 12 * oct + ((scale_pc[deg] - root_pc + 12) % 12) + root_pc % 12;
			while (note < base)
				note += 12;
			if (!sounding(&c->v[k]))
				make_room(&c->v[k]);
			nv_trigger(&c->v[k], (float)note, 0.45f + 0.35f * rnd(),
			           BELL_RATIO[zone][k], k);
			c->glow = 1.0f;
		}
		if (++c->lpos[k] >= len) {
			c->lpos[k] = 0;
			/* evolve: now and then a loop changes one note */
			if (rnd() < 0.15f + 0.6f * P.g[GP_MOTION]) {
				int s = rndi(len);
				if (c->loop[k][s] >= 0)
					c->loop[k][s] = (int8_t)rndi(scale_n + 3);
			}
			bells_fill(c, ch);
		}
	}
}

/* ---- the sixteenth-note step: every voice's sequencer ---- */
static void chan_step(chan *c, int ch, uint32_t step)
{
	float act = eff_activity(ch);
	int base = reg_base(c, ch);
	switch (c->voice) {
	case GV_BELLS:
		if ((step & 3) == 0)
			bells_beat(c, ch);
		break;
	case GV_BASS: {
		int bar = (step & 15) == 0;
		int every = act < 0.25f ? 64 : act < 0.5f ? 32 : act < 0.75f ? 16 : 8;
		if ((bar && ((step % (uint32_t)every) == 0 || c->seen_chord != chord_count)) || c->target_note < 0.0f) {
			c->seen_chord = chord_count;
			int fifth = rnd() < 0.5f * P.ch[ch][CP_CHAR];
			int pc = fifth ? (chord_pc[0] + 7) % 12 : chord_pc[0];
			int note = place(pc, (float)base, base - 6, base + 6);
			c->target_note = (float)note;
			if (c->glide_note < 0.0f)
				c->glide_note = (float)note;
			nv_trigger(&c->v[0], (float)note, 0.8f, 0.5f, 0);
			c->glow = 1.0f;
		}
		break;
	}
	case GV_GONG: {
		/* a strike on each new chord's root (on the bar), and now and then between */
		int strike = (step & 15) == 0 && c->seen_chord != chord_count;
		if (!strike && rnd() >= act * act * 0.035f)
			break;
		c->seen_chord = chord_count;
		int pc = strike || rnd() < 0.6f ? chord_pc[0] : chord_pc[1 + rndi(chord_n - 1)];
		int note = place(pc, (float)base, base - 5, base + 7);
		nvoice *v = alloc(c, 2);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, 0.75f + 0.2f * rnd(), 1.4f, 0);
		c->glow = 1.0f;
		break;
	}
	case GV_PIZZ: {
		int div = act < 0.25f ? 4 : act < 0.5f ? 3 : act < 0.75f ? 2 : 1;
		if (step % (uint32_t)div)
			break;
		/* up and down the chord over two octaves; CHARACTER adds octave jumps */
		int notes[8], n = 0;
		for (int o = 0; o < 2; o++)
			for (int k = 0; k < chord_n; k++)
				notes[n++] = place(chord_pc[k], (float)(base + 12 * o + 2 * k), base + 12 * o - 1, base + 12 * o + 11);
		int per = 2 * n - 2, q = c->arp_i % per, i = q < n ? q : per - q;
		int zone = stepped(P.ch[ch][CP_CHAR], 3), oct = 0;
		if (zone == 1)
			oct = (c->arp_i & 1) ? 12 : 0;
		else if (zone == 2) {
			static const int8_t JUMP[6] = {-12, 0, 0, 12, 12, 24};
			oct = JUMP[rndi(6)];
		}
		c->arp_i++;
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)(notes[i] + oct), (((step & 3) == 0 ? 0.85f : 0.6f) + 0.15f * rnd()) * 1.5f, 1.0f, 0);
		c->glow = 0.7f;
		break;
	}
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_FLUTE:
	case GV_KALIMBA:
	case GV_EP:
	case GV_SHIMMER:
	case GV_TAILS: {
		static const float PMAX[GV_COUNT] = {0, 0, 0, 0.14f, 0.2f, 0, 0.28f, 0.07f, 0,
		                                     0, 0.14f, 0, 0, 0.07f, 0.08f};
		float p = act * act * PMAX[c->voice];
		if (rnd() >= p)
			break;
		int note = pick_note(c, base - 5, base + 12, 0.6f);
		float vel = 0.55f + 0.45f * rnd();
		int vp = (int)(rnd32() & 1u);
		if (c->voice == GV_KALIMBA) {
			int k = c->ks_next;
			c->ks_next = (uint8_t)((k + 1) % KS_N);
			float hz = mtof((float)note) * speed;
			int len = (int)(GL_SR / hz);
			if (len >= KS_LEN)
				len = KS_LEN - 1;
			if (len < 8)
				len = 8;
			/* excitation: a burst of noise through a one-pole low-pass (TONE), written into the
			 * string during its first period by the render, so a pluck costs no spike */
			c->ks_ea[k] = 0.15f + 0.8f * eff_tone(ch);
			c->ks_elp[k] = 0.0f;
			c->ks_vel[k] = vel;
			c->ks_fill[k] = (uint16_t)len;
			c->ks_len[k] = (uint16_t)len;
			c->ks_i[k] = 0;
			/* decay per period: -60 dB over LENGTH (0.4..6 s) */
			float T = expmap(P.ch[ch][CP_LENGTH], 0.4f, 6.0f);
			c->ks_g[k] = fexp2(-9.9658f * (float)len / (GL_SR * T));
			c->ks_env[k] = vel;
			c->ks_kd[k] = fexp2(-9.9658f * (float)GL_SUB / (GL_SR * T));
			c->ks_a[k] = 0.5f + 0.45f * P.ch[ch][CP_CHAR];
		} else if (c->voice == GV_SHIMMER) {
			/* a swoop up into the note, and its echo an octave higher a dotted eighth later */
			nvoice *v = alloc_from(c, 0, 2);
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)note, vel * 1.15f, 1.0f, 0);
			v->bend = -3.0f;
			c->echo_note = (float)(note + 12);
			c->echo_vel = vel * 0.55f;
			c->echo_at = 0.75f * 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		} else if (c->voice == GV_TAILS) {
			/* each tail its own ratio; now and then an octave up */
			static const float TR[6] = {1.0f, 2.0f, 1.5f, 3.5f, 0.5f, 2.5f};
			nvoice *v = alloc(c, 3);
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)(note + (rnd() < 0.2f ? 12 : 0)), vel * 0.9f, TR[rndi(6)], vp);
		} else if (c->voice == GV_EP) {
			nvoice *v = alloc(c, 3);
			if (!sounding(v))
				make_room_w(v, 3);
			nv_trigger(v, (float)note, vel, 1.0f, vp);
		} else {
			nvoice *v = alloc(c, 3);
			if (c->voice == GV_FLUTE && v->stage != ST_IDLE)
				break;                       /* a flute never cuts a breath short */
			float ratio = 1.0f;
			if (c->voice == GV_PIANO)
				ratio = (float)(1 + stepped(P.ch[ch][CP_CHAR], 3));
			else if (c->voice == GV_MARIMBA)
				ratio = 2.5f + 2.5f * P.ch[ch][CP_CHAR];
			if (!sounding(v))
				make_room(v);
			nv_trigger(v, (float)note, c->voice == GV_FLUTE ? vel : vel * 1.35f, ratio, vp);
			if (c->voice == GV_FLUTE)
				v->hold = expmap(P.ch[ch][CP_LENGTH], 0.8f, 7.0f);
		}
		c->glow = vel;
		break;
	}
	case GV_ARP: {
		int div = act < 0.25f ? 8 : act < 0.5f ? 4 : act < 0.75f ? 3 : 2;
		if (step % (uint32_t)div)
			break;
		/* the chord over two octaves, walked by the CHARACTER pattern */
		int notes[8], n = 0;
		for (int o = 0; o < 2; o++)
			for (int k = 0; k < chord_n; k++)
				notes[n++] = place(chord_pc[k], (float)(base + 12 * o + 2 * k), base + 12 * o - 1, base + 12 * o + 11);
		int pat = stepped(P.ch[ch][CP_CHAR], 4), i;
		if (pat == 0)
			i = c->arp_i % n;
		else if (pat == 1) {
			int per = 2 * n - 2, q = c->arp_i % per;
			i = q < n ? q : per - q;
		} else if (pat == 2)
			i = rndi(n);
		else
			i = (c->arp_i & 1) ? (c->arp_i / 2) % n : 0;
		c->arp_i++;
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)notes[i], 0.5f + 0.3f * rnd(), 3.5f, c->arp_i);
		c->glow = 0.7f;
		break;
	}
	default:
		break;
	}
}

/* ---- per sub-block: envelopes, indices, pitches ---- */
static void chan_control(chan *c, int ch, float dt)
{
	float tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	float sdt = dt * speed;                   /* time runs slower while slowing */
	int base = reg_base(c, ch);
	switch (c->voice) {
	case GV_PAD:
	case GV_SWELL: {
		/* the swell is a pad that breathes like brass: a slow rise, a hold, a long fall, its
		 * brightness following its loudness, vibrato arriving late */
		const int sw = c->voice == GV_SWELL;
		const float chr = P.ch[ch][CP_CHAR];
		int nv = 2 + stepped(P.ch[ch][CP_ACTIVITY], 2);   /* 2 or 3 voices breathing */
		float cycle = sw ? 6.0f + 24.0f * len : 4.0f + 26.0f * len;
		float ratio = sw ? 1.0f : (chr < 0.5f ? 1.0f : 2.0f);
		float detune = sw ? 0.0f : 0.12f * chr;
		float spread = fexp2((4.0f + 14.0f * chr) * (1.0f / 1200.0f));   /* swell's second carrier */
		for (int i = 0; i < NV; i++) {
			nvoice *v = &c->v[i];
			if (!frozen) {
				if (v->stage == ST_IDLE) {
					c->gap[i] -= sdt;
					if (c->gap[i] <= 0.0f && playing && i < nv &&
					    budget_used() + weight(c->voice) > FM_CAP) {
						c->gap[i] = 0.5f;                /* no room: breathe a little later */
					} else if (c->gap[i] <= 0.0f && playing && i < nv) {
						/* breathe in on this voice's tone of the chord */
						int pc = chord_pc[i % chord_n];
						float from = v->note > 0.0f ? v->note : (float)(base + 3 * i);
						v->note = (float)place(pc, from, base - 2, base + 14);
						v->stage = ST_DEC;
						c->cyc[i] = 0.0f;
						v->vel = 0.5f + 0.2f * rnd();
						v->ratio = ratio;
						c->glow = 0.6f;
					}
				} else {
					c->cyc[i] += sdt / (cycle * PAD_FACTOR[i]);
					if (c->cyc[i] >= 1.0f) {
						v->stage = ST_IDLE;
						c->cyc[i] = 0.0f;
						c->gap[i] = rnd() * (1.2f - P.ch[ch][CP_ACTIVITY]) * cycle * 0.5f;
					}
				}
			}
			float x = c->cyc[i], e, idx, vib = 1.0f;
			if (v->stage == ST_IDLE)
				e = 0.0f;
			else if (!sw) {
				float s = sin_u((uint32_t)(x * 2147483648.0f));
				e = s * s;
			} else if (x < 0.45f) {
				float u = x * (1.0f / 0.45f);
				e = u * u;
			} else if (x < 0.7f)
				e = 1.0f;
			else
				e = 0.5f + 0.5f * sin_u((uint32_t)((0.25f + (x - 0.7f) * (0.5f / 0.3f)) * 4294967296.0f));
			v->env = e;
			if (sw) {
				idx = (0.15f + 2.2f * tone) * e * (0.4f + 0.6f * e);
				float vd = clampf((x - 0.3f) * 3.0f, 0.0f, 1.0f) * (0.002f + 0.005f * chr);
				vib = 1.0f + vd * sin_u(lfo_vib + (uint32_t)i * 0x40000000u);
			} else
				idx = tone * 1.3f * (0.5f + 0.5f * e);
			ramp(v, e * v->vel * (sw ? 0.7f : 1.0f), clampf(idx, 0.0f, 3.0f) * IDX_SCALE);
			float hz = mtof(v->note + detune * (float)(i - 1)) * speed * vib;
			v->ic = hz_inc(hz);
			v->im = hz_inc(hz * ratio);
			v->ic2 = hz_inc(hz * spread);
		}
		break;
	}
	case GV_BELLS:
		for (int k = 0; k < 4; k++) {
			nvoice *v = &c->v[k];
			nv_perc(v, 0.004f, expmap(len, 1.0f, 10.0f) / speed, 0.6f / speed, 0.3f + 2.0f * tone, dt);
			nv_pitch(v, v->note);
		}
		break;
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_ARP: {
		float dec, mdec, imax;
		if (c->voice == GV_PIANO) {
			dec = expmap(len, 0.8f, 8.0f);
			mdec = 0.35f;
			imax = 0.4f + 2.4f * tone;
		} else if (c->voice == GV_MARIMBA) {
			dec = expmap(len, 0.2f, 2.5f);
			mdec = 0.08f;
			imax = 0.3f + 2.2f * tone;
		} else {
			dec = expmap(len, 0.3f, 4.0f);
			mdec = 0.15f;
			imax = 0.2f + 1.8f * tone;
		}
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			nv_perc(v, c->voice == GV_PIANO ? 0.002f : 0.001f, dec / speed, mdec / speed, imax, dt);
			nv_pitch(v, v->note);
		}
		break;
	}
	case GV_FLUTE:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			float att = 0.4f + 1.2f * len;
			if (v->stage == ST_ATT) {
				v->env += sdt / att;
				if (v->env >= 1.0f) {
					v->env = 1.0f;
					v->stage = ST_DEC;
				}
			} else if (v->stage == ST_DEC) {
				if (!frozen)
					v->hold -= sdt;
				if (v->hold <= 0.0f)
					v->stage = ST_REL;
			} else if (v->stage == ST_REL) {
				v->env *= dcoef(0.6f + 2.0f * len, sdt);
				if (v->env < 0.0005f) {
					v->env = 0.0f;
					v->stage = ST_IDLE;
				}
			}
			/* breath: the attack is breathier (less feedback) and brightens as it opens */
			float a = v->env * v->vel;
			float fbk = (0.1f + 1.1f * tone) * (0.4f + 0.6f * v->env);
			ramp(v, a, fbk * IDX_SCALE);
			float vib = 1.0f + (0.0015f + 0.006f * P.ch[ch][CP_CHAR]) * sin_u(lfo_vib + (uint32_t)i * 1000000000u);
			v->ic = hz_inc(mtof(v->note) * speed * vib);
		}
		break;
	case GV_BASS: {
		nvoice *v = &c->v[0];
		if (c->target_note >= 0.0f) {
			float g = 0.02f + 1.5f * P.ch[ch][CP_CHAR];          /* glide time */
			c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(g * 0.3f, sdt));
		}
		float dec = expmap(len, 1.5f, 30.0f);
		if (v->stage == ST_ATT) {
			v->env += sdt / 0.03f;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC && !frozen) {
			v->env *= dcoef(playing ? dec : 2.0f, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		float a = v->env * v->vel;
		/* TONE: the sub-octave fades out and a little self-modulation (grit) comes in */
		ramp(v, a, (0.05f + 0.6f * tone) * IDX_SCALE);
		v->ratio = 1.0f - tone;                                /* sub mix */
		float hz = mtof(c->glide_note < 0.0f ? (float)base : c->glide_note) * speed;
		v->ic = hz_inc(hz);
		v->im = hz_inc(hz * 0.5f);
		break;
	}
	case GV_DRONE: {
		/* root and its interval, gliding to each new chord */
		static const float IVL[4] = {7.0f, 5.0f, 12.0f, 14.0f};
		if (c->seen_chord != chord_count || c->last_note != base) {
			c->seen_chord = chord_count;
			c->last_note = base;
			c->target_note = (float)place(chord_pc[0], c->glide_note < 0.0f ? (float)base : c->glide_note, base - 5, base + 6);
		}
		if (c->glide_note < 0.0f)
			c->glide_note = c->target_note;
		c->glide_note += (c->target_note - c->glide_note) * (1.0f - dcoef(3.0f, sdt));
		float want = playing ? 1.0f : 0.0f;
		if (!frozen)
			c->swell += (want - c->swell) * (1.0f - dcoef(6.0f, sdt));
		float period = expmap(1.0f - P.ch[ch][CP_ACTIVITY], 12.0f, 80.0f);
		c->lfo1 += sub_inc(1.0f / period, speed);
		c->lfo2 += sub_inc(0.61f / period, speed);
		float breath = 1.0f - len * 0.6f * (0.5f - 0.5f * sin_u(c->lfo2));
		for (int k = 0; k < 2; k++) {
			nvoice *v = &c->v[k];
			float lfo = 0.5f - 0.5f * sin_u(c->lfo1 + (k ? 0x5EB851EBu : 0u));
			float idx = (0.2f + 2.3f * tone * lfo) * (k ? 0.75f : 1.0f);
			float a = c->swell * breath * (k ? 0.3f : 0.45f);
			ramp(v, a, clampf(idx, 0.0f, 3.0f) * IDX_SCALE);
			v->ratio = 2.0f + (k ? 0.0f : 0.003f);
			v->side = (uint8_t)k;
			nv_pitch(v, c->glide_note - 12.0f + (k ? IVL[stepped(P.ch[ch][CP_CHAR], 4)] : 0.0f));
			v->stage = c->swell > 0.0005f ? ST_DEC : ST_IDLE;
		}
		break;
	}
	case GV_EP:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = v->iinc2 = 0.0f;
				continue;
			}
			/* TONE: dull to bright, and harder notes are brighter, as on the DX7 */
			nv_perc(v, 0.002f, expmap(len, 1.0f, 10.0f) / speed, 0.6f / speed,
			        (0.3f + 2.2f * tone) * (0.5f + 0.6f * v->vel), dt);
			v->menv2 *= dcoef(0.09f / speed, dt);
			float i2 = P.ch[ch][CP_CHAR] * 2.6f * v->menv2 * v->vel * IDX_SCALE;
			v->iinc2 = (i2 - v->idx2) * (1.0f / GL_SUB);
			nv_pitch(v, v->note);
			v->im2 = hz_inc(mtof(v->note) * speed * 14.0f);
		}
		break;
	case GV_GONG: {
		static const float GR[4] = {1.4f, 1.73f, 2.76f, 3.14f};
		float ratio = GR[stepped(P.ch[ch][CP_CHAR], 4)];
		for (int k = 0; k < 2; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.003f, expmap(len, 4.0f, 25.0f) / speed, 1.0f, 1.0f, dt);
			/* the index blooms for 0.6 s after the strike, then fades over seconds */
			float b = v->t < 0.6f ? v->t * (1.0f / 0.6f)
			                      : fexp2(-clampf(v->t - 0.6f, 0.0f, 60.0f) * (1.4427f / 5.0f));
			float im = (0.4f + 2.4f * tone) * (0.25f + 0.75f * b) * IDX_SCALE;
			v->iinc = (im - v->idx) * (1.0f / GL_SUB);
			v->ratio = ratio;
			nv_pitch(v, v->note + 0.7f * fexp2(-clampf(v->t, 0.0f, 5.0f) * (1.4427f / 0.12f)));
		}
		break;
	}
	case GV_PIZZ:
		for (int i = 0; i < 3; i++) {
			nvoice *v = &c->v[i];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			nv_perc(v, 0.0005f, expmap(len, 0.08f, 0.7f) / speed, 0.03f / speed, 0.6f + 2.0f * tone, dt);
			nv_pitch(v, v->note);
		}
		break;
	case GV_SHIMMER: {
		if (c->echo_at > 0.0f && !frozen) {
			c->echo_at -= sdt;
			if (c->echo_at <= 0.0f && playing) {
				nvoice *v = alloc_from(c, 2, 2);
				if (!sounding(v))
					make_room(v);
				nv_trigger(v, c->echo_note, c->echo_vel, 1.0f, 1);
				v->bend = -2.0f;
				c->glow = 0.5f;
			}
		}
		float trem = 1.0f - 0.22f * (0.5f + 0.5f * sin_u(lfo_trem));
		float fbk = (0.4f + 1.4f * P.ch[ch][CP_CHAR]) * IDX_SCALE;
		for (int k = 0; k < 4; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.04f + 0.2f * len, expmap(len, 1.5f, 8.0f) / speed, 3.0f / speed, 1.0f, dt);
			ramp(v, v->env * v->vel * trem, (0.3f + 1.8f * tone) * (0.4f + 0.6f * v->menv) * IDX_SCALE);
			v->idx2 = fbk;
			v->bend *= dcoef(0.15f, sdt);
			float vib = 1.0f + 0.004f * sin_u(lfo_vib + (uint32_t)k * 0x40000000u);
			float hz = mtof(v->note + v->bend) * speed * vib;
			v->ic = hz_inc(hz);
			v->im = hz_inc(hz);
		}
		break;
	}
	case GV_TAILS: {
		int z = stepped(P.ch[ch][CP_CHAR], 3);
		float dec = expmap(len, 2.0f, 14.0f), tau = 0.35f * dec;
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (v->stage == ST_IDLE && v->amp == 0.0f) {
				v->ainc = v->iinc = 0.0f;
				continue;
			}
			v->t += sdt;
			nv_perc(v, 0.008f, dec / speed, 1.0f, 1.0f, dt);
			/* the index tail: falls (bright to pure), blooms (rises, then falls) or rises */
			float x = clampf(v->t / tau, 0.0f, 40.0f), sh;
			if (z == 0)
				sh = fexp2(-x * 1.4427f);
			else if (z == 1)
				sh = x < 0.5f ? x * 2.0f : fexp2(-(x - 0.5f) * 1.4427f * 1.5f);
			else
				sh = 1.0f - fexp2(-x * 1.4427f);
			float im = (0.1f + 2.6f * tone) * (0.08f + 0.92f * sh) * IDX_SCALE;
			v->iinc = (im - v->idx) * (1.0f / GL_SUB);
			nv_pitch(v, v->note);
		}
		break;
	}
	case GV_KALIMBA:
	default:
		break;
	}
	c->glow *= dcoef(0.4f, dt);
}

/* ---- per sub-block: audio ---- */
/* render the channel into its two sides, A and B; returns whether anything sounded */
static int chan_render(chan *c, float *A, float *B)
{
	int any = 0;
	if (!STEREO[c->voice])
		B = A;                                  /* mono voice: both sides are one buffer */
	switch (c->voice) {
	case GV_PAD:
	case GV_BELLS:
		for (int i = 0; i < NV; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_SWELL:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_dual(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_EP:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_ep(&c->v[i], A);
				any = 1;
			}
		break;
	case GV_SHIMMER:
		for (int i = 0; i < 4; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render_shimmer(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_GONG:
		for (int i = 0; i < 2; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f) {
				nv_render(&c->v[i], A);
				any = 1;
			}
		break;
	case GV_PIANO:
	case GV_MARIMBA:
	case GV_ARP:
	case GV_PIZZ:
	case GV_TAILS:
		for (int i = 0; i < 3; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_DRONE:
		for (int i = 0; i < 2; i++)
			if (c->v[i].amp > 0.0f || c->v[i].ainc > 0.0f)
				{
				nv_render(&c->v[i], c->v[i].side ? B : A);
				any = 1;
			}
		break;
	case GV_FLUTE:
		for (int k = 0; k < 3; k++) {
			nvoice *v = &c->v[k];
			if (!(v->amp > 0.0f || v->ainc > 0.0f))
				continue;
			any = 1;
			float *out = v->side ? B : A;
			uint32_t pc = v->pc, ic = v->ic;
			float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, fb = v->fb;
			for (uint32_t i = 0; i < GL_SUB; i++) {
				float o = sin_u(pc + (uint32_t)(int32_t)(fb * idx));
				fb = 0.5f * (fb + o);              /* the DX7's two-sample average */
				pc += ic;
				out[i] += o * amp;
				amp += ai;
				idx += ii;
			}
			v->pc = pc; v->amp = amp; v->idx = idx; v->fb = fb;
		}
		break;
	case GV_BASS: {
		nvoice *v = &c->v[0];
		if (!(v->amp > 0.0f || v->ainc > 0.0f))
			break;
		any = 1;
		float sub = v->ratio, fund = 1.0f - 0.5f * sub;
		uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
		float amp = v->amp, ai = v->ainc, idx = v->idx, ii = v->iinc, fb = v->fb;
		for (uint32_t i = 0; i < GL_SUB; i++) {
			float o = sin_u(pc + (uint32_t)(int32_t)(fb * idx));
			fb = o;
			A[i] += (o * fund + SIN[pm >> TBL_SHIFT] * sub) * amp;
			pc += ic;
			pm += im;
			amp += ai;
			idx += ii;
		}
		v->pc = pc; v->pm = pm; v->amp = amp; v->idx = idx; v->fb = fb;
		break;
	}
	case GV_KALIMBA:
		for (int k = 0; k < KS_N; k++) {
			int len = c->ks_len[k];
			if (!len)
				continue;
			any = 1;
			int16_t *b = c->ks[k];
			int i = c->ks_i[k];
			float g = c->ks_g[k] * (1.0f / 16000.0f), a = c->ks_a[k], na = 1.0f - a;
			float *out = (k & 1) ? B : A;
			uint32_t n = 0;
			if (c->ks_fill[k]) {
				float ea = c->ks_ea[k], lp = c->ks_elp[k], vel = c->ks_vel[k];
				uint32_t m = c->ks_fill[k] < GL_SUB ? c->ks_fill[k] : GL_SUB;
				for (; n < m; n++) {
					lp += ea * ((rnd() * 2.0f - 1.0f) - lp);
					float x = lp * vel;                      /* the string's units: 1.0 = 16000 */
					b[i] = (int16_t)(x * 16000.0f);
					out[n] += x * 1.8f;
					i = i + 1 == len ? 0 : i + 1;
				}
				c->ks_elp[k] = lp;
				c->ks_fill[k] = (uint16_t)(c->ks_fill[k] - m);
			}
			float y = (float)b[i];                     /* carried: one load per sample */
			for (; n < GL_SUB; n++) {
				int j = i + 1 == len ? 0 : i + 1;
				float yj = (float)b[j];
				float nx = (a * y + na * yj) * g;
				b[i] = (int16_t)(nx * 16000.0f);
				y = yj;
				i = j;
				out[n] += nx * 1.8f;
			}
			c->ks_i[k] = (uint16_t)i;
			/* the string's level, estimated once per block (its filter only makes it quieter
			 * sooner): below -70 dB it stops costing anything */
			c->ks_env[k] *= c->ks_kd[k];
			if (!c->ks_fill[k] && c->ks_env[k] < 0.0003f)
				c->ks_len[k] = 0;
		}
		break;
	default:
		break;
	}
	return any;
}

/* ================================================================ effects */

/* ---- the space: echo and reverb, both at half rate (24 kHz), int16 memory ----
 * Echo: two taps (L, and R at 1.5 x L), fed back darker through a soft tape saturation, then
 * into the reverb (Discreet Music's chain). Reverb: two input allpasses, then a four-line
 * feedback delay network with a Householder mix and a low-pass in every line.
 * Every line is a power of two long and they all share one write index, so a read is
 * (w - length) & mask: no wrap branches, nothing the compiler has to reload. */
#define ECHO_LEN 16384u                        /* 0.68 s at 24 kHz: L tap to 0.44 s, R to 0.66 s */
#define FDN_SZ 2048u
static int16_t echo_buf[ECHO_LEN];
static int16_t fdn_buf[4][FDN_SZ];
static const uint16_t FDN_LEN[4] = {1103, 1361, 1583, 1879};   /* 46..78 ms */
#define AP1 113
#define AP2 277
static float ap1[AP1], ap2[AP2];
static struct {
	uint32_t w;
	uint16_t a1, a2;
	float echo_d, echo_lp, lp[4], prev_l, prev_r;
} fx = {0, 0, 0, 3000.0f, 0, {0, 0, 0, 0}, 0, 0};

static inline int16_t to16(float x)
{
	x *= 16384.0f;
#if defined(__ARM_ARCH) && defined(__ARM_FEATURE_SAT)
	return (int16_t)__builtin_arm_ssat((int32_t)x, 16);    /* VCVT saturates, SSAT clamps */
#else
	return (int16_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x);
#endif
}
static inline float from16(int16_t v) { return (float)v * (1.0f / 16384.0f); }

/* ---- DJ filter: TPT state-variable, stereo ---- */
static float svf_s[2][2], svf_a1, svf_a2, svf_a3, svf_k = 1.2f;
static float svf_mode;                         /* <0 low-pass, >0 high-pass, 0 through */
static float dc_x[2], dc_y[2];                 /* DC blocker (feedback FM leaves an offset) */

static void filter_setup(float v)
{
	float fc;
	if (v < 0.47f) {
		fc = 18000.0f * fexp2(-(0.47f - v) / 0.47f * 9.0f);
		svf_mode = -1.0f;
	} else if (v > 0.53f) {
		fc = 18.0f * fexp2((v - 0.53f) / 0.47f * 9.6f);
		svf_mode = 1.0f;
	} else {
		fc = 18000.0f;
		svf_mode = 0.0f;
	}
	/* g = tan(pi fc / SR), from the sine table */
	uint32_t ph = (uint32_t)(fc * (0.5f / GL_SR) * 4294967296.0f);
	float g = sin_u(ph) / sin_u(ph + 0x40000000u);
	svf_k = svf_mode < 0.0f ? 1.1f : 1.3f;
	svf_a1 = 1.0f / (1.0f + g * (g + svf_k));
	svf_a2 = g * svf_a1;
	svf_a3 = g * svf_a2;
}

/* derived from the patch when it changes, not every millisecond (see derive()) */
static float d_drift = 60.0f, d_echo_want = 3000.0f, d_verb_g, d_verb_damp;
static float k_echo_slide, k_freeze;
static void derive(void);

/* ================================================================ main thread API */

void gl_init(uint32_t seed)
{
	for (int i = 0; i <= TBL; i++) {
		/* sine without libm: a short Taylor series per point, then exact quarter symmetry */
		float x = (float)(i % (TBL / 2)) * (6.2831853f / TBL);
		float y = x > 1.5707963f ? 3.1415927f - x : x, y2 = y * y;
		float s = y * (1.0f - y2 / 6.0f * (1.0f - y2 / 20.0f * (1.0f - y2 / 42.0f * (1.0f - y2 / 72.0f * (1.0f - y2 / 110.0f)))));
		SIN[i] = (i % TBL) < TBL / 2 ? s : -s;
	}
	SIN[TBL] = SIN[0];
	rng = seed ? seed : 0x9E3779B9u;
	gl_piece(0, &P);
	post_buf[0] = post_buf[1] = P;
	memset(echo_buf, 0, sizeof echo_buf);
	memset(fdn_buf, 0, sizeof fdn_buf);
	memset(ap1, 0, sizeof ap1); memset(ap2, 0, sizeof ap2);
	mode_cur = root_cur = -1;
	chord_deg = 0;
	harmony_update();
	chord_build();
	chord_pending = 0;
	for (int c = 0; c < GL_NCH; c++) {
		chan_reset(&CH[c], P.voice[c]);
		if (CH[c].voice == GV_BELLS)
			bells_fill(&CH[c], c);
	}
	derive();
	fx.echo_d = d_echo_want;
}

void gl_post_patch(const gl_patch *p)
{
	uint32_t s = post_seq + 1u;
	post_buf[s & 1u] = *p;
	BARRIER();
	post_seq = s;
}
void gl_play(int on) { cmd_play = (uint8_t)(on != 0); }
void gl_freeze(int on) { cmd_freeze = (uint8_t)(on != 0); }
void gl_slow(int on) { cmd_slow = (uint8_t)(on != 0); }
void gl_next_chord(void) { cmd_next++; }
void gl_panic(void) { cmd_panic++; }

/* ================================================================ render */

static float lvl_ch[GL_NCH], fx_echo_mix, fx_echo_fb, fx_verb_mix, fx_verb_g, fx_verb_damp;

static void derive(void)
{
	const float dt = (float)GL_SUB / GL_SR;
	float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	float dl = P.g[GP_DELAY], rv = P.g[GP_REVERB];
	d_drift = expmap(1.0f - P.g[GP_DRIFT], 10.0f, 240.0f);
	fx_echo_mix = dl < 0.02f ? 0.0f : 0.25f + 0.5f * dl;
	fx_echo_fb = 0.25f + 0.62f * dl;
	float t = 0.75f * 60.0f / bpm;            /* a dotted eighth, halved until it fits */
	while (t > 0.44f)
		t *= 0.5f;
	d_echo_want = t * (GL_SR * 0.5f);
	fx_verb_mix = (0.15f + 0.85f * rv) * 0.6f;
	d_verb_g = 0.55f + 0.42f * rv;
	d_verb_damp = 0.25f + 0.35f * (1.0f - rv);
	k_echo_slide = 1.0f - dcoef(0.5f, dt);
	k_freeze = 1.0f - dcoef(0.08f, dt);
	filter_setup(P.g[GP_FILTER]);
}

static void control(float dt)
{
	uint32_t s = post_seq;
	if (s != post_seen) {
		P = post_buf[s & 1u];
		post_seen = s;
		harmony_update();
		derive();
	}
	playing = cmd_play;
	frozen = cmd_freeze;
	slowing = cmd_slow;
	if (cmd_panic != seen_panic) {
		seen_panic = cmd_panic;
		for (int c = 0; c < GL_NCH; c++) {
			chan_reset(&CH[c], P.voice[c]);
			if (CH[c].voice == GV_BELLS)
				bells_fill(&CH[c], c);
		}
	}
	if (cmd_next != seen_next) {
		seen_next = cmd_next;
		chord_pending = 1;
	}

	/* the slowdown: down to 10 BPM over 10 s, then back with a spring */
	float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	float floor_ = 10.0f / bpm;
	if (slowing) {
		speed -= (1.0f - floor_) * dt / 10.0f;
		if (speed < floor_)
			speed = floor_;
	} else if (speed < 1.0f) {
		speed += (1.0f - speed) * (1.0f - dcoef(0.35f, dt)) + dt * 0.2f;
		if (speed > 1.0f)
			speed = 1.0f;
	}
	gl_speed = speed;
	lfo_vib += sub_inc(5.1f, speed);
	lfo_trem += sub_inc(7.3f, speed);
	lfo_arc1 += sub_inc(1.0f / 170.0f, speed);
	lfo_arc2 += sub_inc(1.0f / 47.0f, speed);

	/* the arc: a slow tide between sparse and full, as deep as MOTION */
	arc = P.g[GP_MOTION] * (0.65f * sin_u(lfo_arc1) + 0.35f * sin_u(lfo_arc2));

	/* chord drift: 10 s .. 4 min */
	if (!frozen && playing)
		chord_timer += dt * speed;
	if (chord_timer >= d_drift) {
		chord_timer = 0.0f;
		chord_pending = 2;
	}
	if (chord_pending == 1) {                  /* asked for: now */
		chord_pending = 0;
		chord_next();
	}

	/* voice changes: fade the channel out, swap, fade in */
	for (int c = 0; c < GL_NCH; c++) {
		chan *ch = &CH[c];
		if (P.voice[c] < GV_COUNT && P.voice[c] != ch->voice && !ch->fading)
			ch->fading = 1;
		if (ch->fading) {
			ch->fade -= dt / 0.03f;
			if (ch->fade <= 0.0f) {
				chan_reset(ch, P.voice[c] < GV_COUNT ? P.voice[c] : GV_PAD);
				if (ch->voice == GV_BELLS)
					bells_fill(ch, c);
			}
		} else if (ch->fade < 1.0f) {
			ch->fade += dt / 0.03f;
			if (ch->fade > 1.0f)
				ch->fade = 1.0f;
		}
	}

	/* the clock: sixteenths, slowed with everything else */
	step_acc += dt * speed * bpm * (4.0f / 60.0f);
	gl_beat = (float)(step_n & 3) * 0.25f + step_acc * 0.25f;
	while (step_acc >= 1.0f) {
		step_acc -= 1.0f;
		if ((step_n & 15) == 0 && chord_pending == 2) {   /* drifted: on the bar */
			chord_pending = 0;
			chord_next();
		}
		if (playing && !frozen)
			for (int c = 0; c < GL_NCH; c++)
				if (!CH[c].fading)
					chan_step(&CH[c], c, step_n);
		step_n++;
	}
	for (int c = 0; c < GL_NCH; c++) {
		chan_control(&CH[c], c, dt);
		gl_glow[c] = CH[c].glow;
		float l = P.ch[c][CP_LEVEL];
		lvl_ch[c] = l * l * CH[c].fade;
	}

	/* effects (the patch-dependent parts are in derive()) */
	fx.echo_d += (d_echo_want - fx.echo_d) * k_echo_slide;    /* a tape slide, not a jump */
	fx_verb_g = frozen ? 0.9995f : d_verb_g;
	fx_verb_damp = frozen ? 0.02f : d_verb_damp;
	freeze_in += ((frozen ? 0.0f : 1.0f) - freeze_in) * k_freeze;
	gl_playing = playing;
	gl_frozen = frozen;
}

/* the space for one sub-block, at half rate: E and V hold one echo and reverb send per pair
 * of samples; the result is interpolated back up and added into L and R. Everything lives in
 * locals for the loop: the int16 stores would otherwise force the compiler to reload every
 * global they might alias. */
static void fx_block(const float *E, const float *V, float *L, float *R)
{
	uint32_t w = fx.w;
	uint32_t a1 = fx.a1, a2 = fx.a2;
	float d = fx.echo_d, d15 = fx.echo_d * 1.5f, elp = fx.echo_lp;
	float lp0 = fx.lp[0], lp1 = fx.lp[1], lp2 = fx.lp[2], lp3 = fx.lp[3];
	float pl = fx.prev_l, pr = fx.prev_r;
	const float efb = fx_echo_fb, emix = fx_echo_mix, vmix = fx_verb_mix, fi = freeze_in * 0.3f;
	const float g = fx_verb_g, k = 1.0f - fx_verb_damp;
	int16_t *const eb = echo_buf;
	int16_t *const f0 = fdn_buf[0], *const f1 = fdn_buf[1], *const f2 = fdn_buf[2], *const f3 = fdn_buf[3];
	float *const p1 = ap1, *const p2 = ap2;
	for (uint32_t i = 0; i < GL_SUB; i += 2) {
		/* echo: two interpolated taps */
		float rp = (float)w - d + (float)ECHO_LEN;
		uint32_t j = (uint32_t)rp;
		float fr = rp - (float)j;
		float x0 = from16(eb[j & (ECHO_LEN - 1u)]), x1 = from16(eb[(j + 1u) & (ECHO_LEN - 1u)]);
		float el = x0 + (x1 - x0) * fr;
		rp = (float)w - d15 + (float)ECHO_LEN;
		j = (uint32_t)rp;
		fr = rp - (float)j;
		x0 = from16(eb[j & (ECHO_LEN - 1u)]);
		x1 = from16(eb[(j + 1u) & (ECHO_LEN - 1u)]);
		float er = x0 + (x1 - x0) * fr;
		elp += 0.45f * (el - elp);                          /* each repeat darker */
		float x = E[i >> 1] * 0.5f + elp * efb;
		x = x / (1.0f + (x > 0.0f ? x : -x) * 0.3f);       /* soft tape saturation */
		eb[w & (ECHO_LEN - 1u)] = to16(x);

		/* reverb: the sends and the echo, diffused, into the network */
		float y, o;
		x = (0.5f * V[i >> 1] + (el + er) * 0.25f) * fi;
		y = p1[a1]; o = y - 0.6f * x; p1[a1] = x + 0.6f * o;
		a1 = a1 + 1u == AP1 ? 0u : a1 + 1u;
		x = o;
		y = p2[a2]; o = y - 0.6f * x; p2[a2] = x + 0.6f * o;
		a2 = a2 + 1u == AP2 ? 0u : a2 + 1u;
		x = o;
		float d0 = from16(f0[(w - FDN_LEN[0]) & (FDN_SZ - 1u)]);
		float d1 = from16(f1[(w - FDN_LEN[1]) & (FDN_SZ - 1u)]);
		float d2 = from16(f2[(w - FDN_LEN[2]) & (FDN_SZ - 1u)]);
		float d3 = from16(f3[(w - FDN_LEN[3]) & (FDN_SZ - 1u)]);
		lp0 += k * (d0 - lp0);
		lp1 += k * (d1 - lp1);
		lp2 += k * (d2 - lp2);
		lp3 += k * (d3 - lp3);
		float h = 0.5f * (lp0 + lp1 + lp2 + lp3);
		uint32_t wi = w & (FDN_SZ - 1u);
		f0[wi] = to16(x + g * (lp0 - h));
		f1[wi] = to16(x + g * (lp1 - h));
		f2[wi] = to16(-x + g * (lp2 - h));
		f3[wi] = to16(-x + g * (lp3 - h));
		w++;
		float ol = el * emix + (d0 - d2 + 0.5f * d1) * vmix;
		float or_ = er * emix + (d1 - d3 + 0.5f * d2) * vmix;
		L[i] += 0.5f * (pl + ol);
		R[i] += 0.5f * (pr + or_);
		L[i + 1] += ol;
		R[i + 1] += or_;
		pl = ol;
		pr = or_;
	}
	fx.w = w;
	fx.a1 = (uint16_t)a1;
	fx.a2 = (uint16_t)a2;
	fx.echo_lp = elp;
	fx.lp[0] = lp0; fx.lp[1] = lp1; fx.lp[2] = lp2; fx.lp[3] = lp3;
	fx.prev_l = pl;
	fx.prev_r = pr;
}

void gl_render(int16_t *lr, uint32_t n, float gain)
{
	const float dt = (float)GL_SUB / GL_SR;
	float peak = gl_peak * 0.9f;
	for (uint32_t base = 0; base + GL_SUB <= n; base += GL_SUB) {
		control(dt);
		/* the dry mix at full rate; the echo and reverb sends at half rate (one per pair) */
		float L[GL_SUB], R[GL_SUB], E[GL_SUB / 2], V[GL_SUB / 2];
		for (uint32_t i = 0; i < GL_SUB; i++)
			L[i] = R[i] = 0.0f;
		for (uint32_t i = 0; i < GL_SUB / 2; i++)
			E[i] = V[i] = 0.0f;
		for (int c = 0; c < GL_NCH; c++) {
			float g = lvl_ch[c] * 0.5f;
			if (g <= 0.0f)
				continue;                   /* silent: its envelopes still run in control() */
			int st = STEREO[CH[c].voice];
			float ca[GL_SUB], cb[GL_SUB];
			memset(ca, 0, sizeof ca);
			if (st)
				memset(cb, 0, sizeof cb);
			if (!chan_render(&CH[c], ca, cb))
				continue;
			float pan = P.ch[c][CP_PAN], es = P.ch[c][CP_ECHO] * g, vs = P.ch[c][CP_VERB] * g;
			if (!st) {
				float al = (1.0f - pan) * g, ar = pan * g;
				for (uint32_t i = 0; i < GL_SUB; i += 2) {
					float a0 = ca[i], a1 = ca[i + 1];
					L[i] += a0 * al;
					R[i] += a0 * ar;
					L[i + 1] += a1 * al;
					R[i + 1] += a1 * ar;
					float m = 2.0f * (a0 + a1);         /* as loud in the sends as a stereo voice */
					E[i >> 1] += m * es;
					V[i >> 1] += m * vs;
				}
				continue;
			}
			/* the two sides, spread either side of the channel's pan */
			float sp = SPREAD[CH[c].voice];
			float pa = clampf(pan - sp, 0.0f, 1.0f), pb = clampf(pan + sp, 0.0f, 1.0f);
			float al = (1.0f - pa) * g, ar = pa * g, bl = (1.0f - pb) * g, br = pb * g;
			for (uint32_t i = 0; i < GL_SUB; i += 2) {
				float a0 = ca[i], b0 = cb[i], a1 = ca[i + 1], b1 = cb[i + 1];
				L[i] += a0 * al + b0 * bl;
				R[i] += a0 * ar + b0 * br;
				L[i + 1] += a1 * al + b1 * bl;
				R[i + 1] += a1 * ar + b1 * br;
				float m = (a0 + b0) + (a1 + b1);
				E[i >> 1] += m * es;
				V[i >> 1] += m * vs;
			}
		}
		/* the space, at half rate; its output interpolated back up */
		fx_block(E, V, L, R);
		/* DJ filter, DC blocker, soft clip, out */
		float a1 = svf_a1, a2 = svf_a2, a3 = svf_a3, k = svf_k, md = svf_mode;
		float s0l = svf_s[0][0], s1l = svf_s[0][1], s0r = svf_s[1][0], s1r = svf_s[1][1];
		float xl = dc_x[0], yl = dc_y[0], xr = dc_x[1], yr = dc_y[1];
		if (md != 0.0f) {
			for (uint32_t i = 0; i < GL_SUB; i++) {
				float l = L[i], r = R[i];
				float v3 = l - s1l, v1 = a1 * s0l + a2 * v3, v2 = s1l + a2 * s0l + a3 * v3;
				s0l = 2.0f * v1 - s0l;
				s1l = 2.0f * v2 - s1l;
				v3 = r - s1r;
				float w1 = a1 * s0r + a2 * v3, w2 = s1r + a2 * s0r + a3 * v3;
				s0r = 2.0f * w1 - s0r;
				s1r = 2.0f * w2 - s1r;
				L[i] = md < 0.0f ? v2 : l - k * v1 - v2;
				R[i] = md < 0.0f ? w2 : r - k * w1 - w2;
			}
		}
		int16_t *o = lr + 2u * base;
		for (uint32_t i = 0; i < GL_SUB; i++) {
			float l = L[i], r = R[i];
			yl = l - xl + 0.9987f * yl; xl = l;      /* ~10 Hz high-pass */
			yr = r - xr + 0.9987f * yr; xr = r;
			l = clampf(yl * gain, -1.0f, 1.0f);
			r = clampf(yr * gain, -1.0f, 1.0f);
			l *= 1.5f - 0.5f * l * l;                 /* soft clip, 1.0 at full scale */
			r *= 1.5f - 0.5f * r * r;
			o[2u * i] = (int16_t)(l * 32000.0f);
			o[2u * i + 1u] = (int16_t)(r * 32000.0f);
		}
		for (uint32_t i = 0; i < GL_SUB; i += 8) {   /* a peak for the LEDs, sampled */
			float a = L[i] > 0.0f ? L[i] : -L[i];
			if (a * gain > peak)
				peak = a * gain;
		}
		svf_s[0][0] = s0l; svf_s[0][1] = s1l; svf_s[1][0] = s0r; svf_s[1][1] = s1r;
		dc_x[0] = xl; dc_y[0] = yl; dc_x[1] = xr; dc_y[1] = yr;
	}
	gl_peak = peak;
}

/* ================================================================ pieces */

/* Eight recipes. Values are percent (0..100). Columns per channel:
 *   ACT TONE LEN REG ECHO VERB PAN CHAR LEVEL      REG: 0,25,50,75,100 = -2..+2 octaves
 * Globals: TEMPO DELAY REVERB FILTER ROOT(0..11) MODE(0..7) DRIFT MOTION */
typedef struct {
	const char *name;
	uint8_t voice[GL_NCH];
	uint8_t ch[GL_NCH][GL_NCP];
	uint8_t g[GL_NGP];
} recipe;

static const recipe PIECES[GL_NPIECES] = {
	{"Terminal", {GV_PAD, GV_BELLS, GV_BASS, GV_FLUTE},
	 {{50, 45, 55, 50, 30, 70, 50, 20, 70}, {40, 45, 55, 50, 45, 65, 50, 50, 65},
	  {30, 30, 60, 50, 10, 30, 50, 30, 55}, {35, 40, 50, 50, 50, 70, 60, 30, 55}},
	 {47, 45, 65, 50, 2, 1, 35, 50}},
	{"Gate Seven", {GV_PAD, GV_PIANO, GV_DRONE, GV_BELLS},
	 {{40, 30, 75, 50, 20, 80, 50, 10, 60}, {35, 35, 60, 50, 55, 70, 45, 0, 65},
	  {40, 35, 30, 50, 0, 60, 50, 0, 45}, {25, 30, 70, 75, 50, 75, 50, 50, 50}},
	 {33, 50, 75, 50, 0, 0, 25, 40}},
	{"Long Wait", {GV_DRONE, GV_FLUTE, GV_PAD, GV_KALIMBA},
	 {{30, 45, 50, 50, 10, 70, 50, 0, 60}, {25, 35, 80, 50, 60, 80, 40, 40, 55},
	  {30, 25, 90, 75, 10, 80, 50, 60, 45}, {30, 40, 40, 50, 60, 60, 65, 50, 45}},
	 {22, 55, 85, 50, 9, 5, 15, 55}},
	{"Jet Lag", {GV_ARP, GV_PAD, GV_BASS, GV_MARIMBA},
	 {{35, 30, 40, 50, 60, 60, 50, 25, 50}, {50, 50, 50, 50, 20, 70, 50, 70, 65},
	  {45, 40, 50, 50, 10, 25, 50, 40, 55}, {30, 40, 40, 75, 50, 60, 60, 40, 45}},
	 {55, 55, 60, 50, 7, 3, 30, 60}},
	{"Tarmac", {GV_BASS, GV_DRONE, GV_BELLS, GV_PIANO},
	 {{25, 50, 80, 25, 0, 30, 50, 60, 60}, {50, 55, 40, 50, 10, 60, 50, 25, 50},
	  {50, 60, 50, 50, 40, 60, 50, 90, 55}, {40, 50, 50, 50, 50, 65, 55, 35, 55}},
	 {40, 50, 60, 50, 5, 6, 40, 65}},
	{"Night Flight", {GV_PAD, GV_KALIMBA, GV_FLUTE, GV_DRONE},
	 {{45, 20, 85, 25, 20, 85, 50, 0, 60}, {40, 30, 60, 50, 65, 70, 35, 30, 50},
	  {20, 30, 90, 50, 60, 85, 65, 60, 50}, {25, 20, 40, 25, 0, 50, 50, 0, 40}},
	 {25, 60, 80, 50, 4, 7, 20, 45}},
	{"Arrivals", {GV_BELLS, GV_ARP, GV_PAD, GV_BASS},
	 {{60, 60, 50, 50, 40, 60, 50, 50, 60}, {50, 50, 30, 75, 45, 55, 50, 75, 40},
	  {60, 55, 45, 50, 20, 60, 50, 60, 60}, {50, 30, 40, 50, 0, 20, 50, 20, 55}},
	 {62, 40, 55, 50, 7, 3, 45, 70}},
	{"Lounge", {GV_PIANO, GV_MARIMBA, GV_PAD, GV_FLUTE},
	 {{45, 30, 70, 50, 40, 60, 40, 0, 60}, {25, 25, 45, 75, 50, 70, 65, 70, 40},
	  {35, 40, 70, 50, 20, 70, 50, 30, 55}, {25, 50, 60, 75, 55, 75, 50, 50, 45}},
	 {36, 45, 70, 50, 5, 0, 30, 50}},
	{"Rain City", {GV_SWELL, GV_EP, GV_GONG, GV_SHIMMER},
	 {{45, 55, 75, 50, 25, 85, 50, 60, 65}, {35, 25, 55, 50, 50, 65, 40, 40, 60},
	  {30, 45, 70, 50, 10, 70, 50, 25, 55}, {25, 50, 60, 50, 70, 80, 60, 50, 45}},
	 {30, 60, 85, 50, 4, 5, 30, 55}},
	{"Deep Water", {GV_SHIMMER, GV_TAILS, GV_GONG, GV_PAD},
	 {{35, 45, 70, 50, 75, 85, 40, 30, 55}, {35, 55, 70, 50, 50, 75, 60, 50, 55},
	  {25, 40, 80, 50, 15, 80, 50, 75, 50}, {40, 25, 85, 25, 15, 85, 50, 20, 50}},
	 {20, 65, 90, 50, 2, 1, 25, 60}},
	{"Lobby 84", {GV_EP, GV_PIZZ, GV_BASS, GV_SWELL},
	 {{45, 75, 55, 50, 40, 55, 45, 70, 75}, {45, 45, 30, 50, 45, 45, 60, 50, 70},
	  {40, 40, 50, 50, 5, 20, 50, 30, 65}, {40, 45, 60, 50, 20, 70, 50, 40, 60}},
	 {55, 40, 55, 50, 5, 0, 35, 45}},
	{"Long Haul", {GV_PIZZ, GV_TAILS, GV_SWELL, GV_GONG},
	 {{55, 40, 25, 75, 60, 55, 40, 100, 70}, {30, 50, 75, 50, 40, 75, 60, 50, 68},
	  {40, 40, 80, 50, 20, 80, 50, 70, 62}, {25, 45, 75, 50, 10, 70, 50, 0, 65}},
	 {45, 55, 75, 50, 9, 7, 40, 60}},
};

const char *gl_piece_name(int i) { return PIECES[(unsigned)i < GL_NPIECES ? i : 0].name; }

void gl_piece(int i, gl_patch *out)
{
	const recipe *r = &PIECES[(unsigned)i < GL_NPIECES ? i : 0];
	for (int c = 0; c < GL_NCH; c++) {
		out->voice[c] = r->voice[c];
		for (int p = 0; p < GL_NCP; p++)
			out->ch[c][p] = (float)r->ch[c][p] * 0.01f;
	}
	for (int p = 0; p < GL_NGP; p++)
		out->g[p] = (float)r->g[p] * 0.01f;
	out->g[GP_ROOT] = (float)r->g[GP_ROOT] / 11.0f;
	out->g[GP_MODE] = (float)r->g[GP_MODE] / (float)(GL_NMODES - 1);
}

/* main thread: its own generator, so it never races the render's */
static uint32_t rr;
static float rrnd(void)
{
	rr ^= rr << 13;
	rr ^= rr >> 17;
	rr ^= rr << 5;
	return (float)(rr >> 8) * (1.0f / 16777216.0f);
}
static int rrndi(int n) { return (int)(rrnd() * (float)n) % n; }

void gl_randomize(gl_patch *p, uint32_t seed)
{
	rr = seed ? seed : 0x1234567u;
	for (int c = 0; c < GL_NCH; c++) {
		p->ch[c][CP_ACTIVITY] = 0.15f + 0.65f * rrnd();
		p->ch[c][CP_TONE] = 0.1f + 0.7f * rrnd();
		p->ch[c][CP_LENGTH] = 0.15f + 0.8f * rrnd();
		p->ch[c][CP_REGISTER] = (float)(1 + rrndi(3)) * 0.25f;   /* -1..+1 octave */
		p->ch[c][CP_ECHO] = 0.7f * rrnd();
		p->ch[c][CP_VERB] = 0.3f + 0.6f * rrnd();
		p->ch[c][CP_PAN] = 0.2f + 0.6f * rrnd();
		p->ch[c][CP_CHAR] = rrnd();
		p->ch[c][CP_LEVEL] = 0.4f + 0.4f * rrnd();
	}
	p->g[GP_TEMPO] = 0.1f + 0.6f * rrnd();
	p->g[GP_DELAY] = 0.2f + 0.5f * rrnd();
	p->g[GP_REVERB] = 0.4f + 0.5f * rrnd();
	p->g[GP_FILTER] = 0.5f;                               /* never randomized into silence */
	p->g[GP_ROOT] = (float)rrndi(12) / 11.0f;
	p->g[GP_MODE] = (float)rrndi(GL_NMODES) / (float)(GL_NMODES - 1);
	p->g[GP_DRIFT] = 0.1f + 0.6f * rrnd();
	p->g[GP_MOTION] = 0.2f + 0.6f * rrnd();
	}
