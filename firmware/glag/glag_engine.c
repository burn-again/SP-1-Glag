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
/* The instruments run at half rate (24 kHz) and are brought back up to 48 kHz after the mix:
 * nearly half the cost of every voice, for nothing above 12 kHz (heard in round 2 of the
 * reverb audition; only a fully buzzing Shimmer aliased noticeably, dry). */
#ifndef VOICE_HALF
#define VOICE_HALF 1                     /* 0: full rate, for measuring the half rate's cost on a computer */
#endif
#define VSUB (GL_SUB >> VOICE_HALF)
#define VSR (GL_SR / (float)(1 << VOICE_HALF))
static inline uint32_t hz_inc(float hz) { return (uint32_t)(hz * (89478.485f * (float)(1 << VOICE_HALF))); }
static inline uint32_t hz_inc48(float hz) { return (uint32_t)(hz * 89478.485f); }   /* 2^32 / 48000 */
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
	return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;   /* exponent - 128 + (log2 m + 1) */
}
/* lo..hi exponentially */
static inline float expmap(float v, float lo, float hi) { return lo * fexp2(v * flog2(hi / lo)); }
/* a phase increment per sub-block for `hz`, at the current speed */
static inline uint32_t sub_inc(float hz, float spd) { return hz_inc48(hz * spd) * GL_SUB; }

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
                                             "EP", "Gong", "Pizz", "Shimmer", "Tails", "Pulse"};
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
float gl_tempo_bpm(float v) { return 20.0f + 100.0f * clampf(v, 0.0f, 1.0f); }
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
static const uint8_t STEREO[GV_COUNT] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1};
static const float SPREAD[GV_COUNT] = {0.3f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.0f, 0.25f,
                                       0.35f, 0.0f, 0.0f, 0.0f, 0.3f, 0.3f, 0.2f};

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
	/* pulse: kick (phase, pitch, its fall's end, level, decay), shaker, click; a swung hit waiting */
	float pk_ph, pk_f, pk_fend, pk_a, pk_k, ps_a, ps_k, ps_x, pc_ph, pc_inc, pc_a;
	float pend_t;
	uint8_t pend;
	/* bells: a note waiting to sound, per loop */
	float bp_t[4], bp_note[4], bp_vel[4], bp_ratio[4];
	uint8_t bp_on;
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
/* plucked or struck (bright into the reverb), rather than held */
static const uint8_t PLUCK[GV_COUNT] = {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0};
static const uint8_t HOME[GV_COUNT] = {57, 72, 38, 64, 72, 45, 72, 72, 67, 57, 60, 33, 67, 79, 64, 36};

/* ---- the channel macro: a slow random glide pushing each voice's own controls around ----
 * Per voice, how far the macro moves:   tone  char  trem  oct   len   act */
static const uint8_t MW[GV_COUNT][6] = {
	{60, 50, 40,  0, 30, 20},   /* pad: brightness, detune, breathing tremolo */
	{50, 70,  0, 50, 30, 30},   /* bells: metal, octave leaps */
	{40, 60,  0, 30, 30, 20},   /* bass: glide and fifths, the odd octave */
	{60, 60, 20, 40, 40, 30},   /* piano: tine, octaves */
	{50, 80,  0, 50, 30, 40},   /* marimba: ratio, octaves */
	{80, 30, 30,  0,  0, 30},   /* drone: the index opens wider, interval shifts */
	{60, 60,  0, 50, 40, 40},   /* kalimba */
	{50, 80, 30, 30, 30, 20},   /* flute: vibrato */
	{50, 60, 20, 60, 40, 30},   /* arp: pattern, octave range */
	{60, 70, 30,  0, 30, 20},   /* swell: detune, vibrato */
	{60, 60, 60, 30, 30, 30},   /* ep: suitcase tremolo, tine */
	{50, 80,  0, 20, 30, 20},   /* gong: metal */
	{50, 60,  0, 60, 40, 40},   /* pizz: octave jumps */
	{50, 90, 30, 30, 30, 20},   /* shimmer: buzz */
	{50, 90,  0, 40, 40, 30},   /* tails: tail shape */
	{50, 60,  0,  0, 40, 50},   /* pulse: swing, brightness, hits */
};
static float mac_depth[GL_NCH], mac_period[GL_NCH] = {8.0f, 8.0f, 8.0f, 8.0f};
static float mac_ph[GL_NCH][2], mac_from[GL_NCH][2], mac_to[GL_NCH][2], mac_u[GL_NCH][2];
static float mac_trem_ph[GL_NCH];
static gl_patch Pb;                    /* the patch as posted, before the macro moves it */
void gl_macro(int ch, float depth, float period_s)
{
	if ((unsigned)ch < GL_NCH) {
		mac_depth[ch] = depth;
		mac_period[ch] = period_s;
	}
}
static int reg_base(const chan *c, int ch)
{
	int jump = 0;
	float d = mac_depth[ch];
	float pj = d * (1.0f + d) * MW[c->voice][3] * 0.006f;   /* up to ~0.7 of notes leap at full */
	if (d > 0.0f && rnd() < pj) {
		jump = rnd() < 0.6f ? 12 : -12;
		if (d > 0.7f && rnd() < 0.4f)
			jump *= 2;                                       /* two octaves, near full */
	}
	return HOME[c->voice] + 12 * (stepped(P.ch[ch][CP_REGISTER], 5) - 2) + jump;
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
	/* the index starts from nothing: a higher note must not inherit a lower one's brightness
	 * (for its first millisecond it would alias, a grinding edge on every strike) */
	v->idx = v->iinc = v->idx2 = v->iinc2 = 0.0f;
	/* and its pitch is the new note's from the first sample (the cap above reads it) */
	float hz = mtof(note) * speed;
	v->ic = hz_inc(hz);
	v->im = hz_inc(hz * ratio);
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
/* Counted in half-voices: a 2-op voice is 2, the three-oscillator swell and EP are 3. The
 * distortion, when it is on, takes two voices' worth out of the budget. */
#ifndef FM_CAP
#define FM_CAP 18
#endif
#ifndef FM_DIST_CUT
#define FM_DIST_CUT 4
#endif
static int fm_cap = FM_CAP, fm_dist = FM_CAP;   /* the cap now; the cap the distortion leaves */
/* The load governor: the audio thread reports each block's share of its time (gl_load); past
 * 88 % the cap comes down a voice at a time (the quietest decaying notes give way), and after a
 * second under 72 % it goes back up. A safety net under the worst cases, never heard otherwise. */
static int gov_cap = FM_CAP, gov_cool;
void gl_load(float frac)
{
	if (frac > 0.88f) {
		gov_cap = gov_cap - 2 > 8 ? gov_cap - 2 : 8;
		gov_cool = 0;
	} else if (frac < 0.72f && gov_cap < FM_CAP && ++gov_cool >= (int)(GL_SR / 240.0f)) {
		gov_cool = 0;
		gov_cap += 2;
	}
	fm_cap = fm_dist < gov_cap ? fm_dist : gov_cap;
}
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
	case GV_PULSE: return 0;                  /* no FM voices: outside the budget */
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
		if (ch->voice == GV_KALIMBA)               /* a ringing string costs about a voice */
			for (int k = 0; k < KS_N; k++)
				n += ch->ks_len[k] ? 2 : 0;
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
	if (n + w > fm_cap && q) {
		q->stage = ST_IDLE;                    /* its amplitude ramps to 0 in the next ms */
		q->env = 0.0f;
	}
}
static void make_room(const nvoice *keep) { make_room_w(keep, 2); }

/* set this sub-block's ramps toward amplitude `a` and index `i`. A voice that has faded
 * is snapped to exactly zero, or float rounding would leave it rendering forever. */
/* An FM voice's partials reach about fc + (index + 1) fm (Carson). The voices run at 24 kHz,
 * so anything past 12 kHz would fold back down as aliasing (a grinding edge on high bells,
 * marimba, shimmer): cap each note's index so its partials stay under 10 kHz, with a sideband
 * to spare (Carson's rule leaves the next one only ~20 dB down). Low notes are never touched;
 * the highest, brightest ones lose only what would have aliased. */
#define HZ_PER_INC (1.0f / (89478.485f * (float)(1 << VOICE_HALF)))
#ifndef IDX_TOP
#define IDX_TOP (0.42f * VSR)                /* 10 kHz at half rate */
#endif
/* from the oscillators' increments (nv_trigger sets them for the new note at once, so a
 * strike never reads the last note's pitch); mult widens fm for a richer modulator */
static inline float idx_cap(const nvoice *v, float i, float mult)
{
	float fc = (float)v->ic * HZ_PER_INC, fm = (float)v->im * HZ_PER_INC * mult;
	if (fm < 1.0f)
		return i;
	float cap = ((IDX_TOP - fc) / fm - 2.0f) * IDX_SCALE;   /* Carson, plus a sideband to spare */
	cap = cap > 0.0f ? cap : 0.0f;
	return i < cap ? i : cap;
}

static inline void ramp(nvoice *v, float a, float i)
{
	i = idx_cap(v, i, 1.0f);
	if (a <= 0.0f && v->amp < 0.0002f && v->amp > -0.0002f)
		v->amp = v->ainc = 0.0f;
	else
		v->ainc = (a - v->amp) * (1.0f / VSUB);
	v->iinc = (i - v->idx) * (1.0f / VSUB);
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
	for (uint32_t i = 0; i < VSUB; i++) {
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
	for (uint32_t i = 0; i < VSUB; i++) {
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
	for (uint32_t i = 0; i < VSUB; i++) {
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
	for (uint32_t i = 0; i < VSUB; i++) {
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
		/* AMB-1's density: 0 to 5 notes a loop, swelled and thinned by the arc */
		int len = BELL_LEN[k], have = 0;
		int want = (int)clampf(act * 5.0f * (1.0f + 0.8f * arc) + rnd(), 0.0f, 5.0f);
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
	/* CHARACTER: its lower half the ratio set (harmonic, bell, glass) on the beat, its upper half
	 * the same three in sixteenths, busier */
	float chr = P.ch[ch][CP_CHAR];
	int zone = stepped(chr > 0.5f ? (chr - 0.5f) * 2.0f : chr * 2.0f, 3);
	for (int k = 0; k < 4; k++) {
		int len = BELL_LEN[k];
		int8_t d = c->loop[k][c->lpos[k]];
		if (d >= 0 && rnd() < 0.6f + 0.4f * eff_activity(ch)) {
			int oct = d / scale_n, deg = d % scale_n;
			int note = base + 12 * oct + ((scale_pc[deg] - root_pc + 12) % 12) + root_pc % 12;
			while (note < base)
				note += 12;
			/* a human hand: each bell up to 60 ms late, as AMB-1's (it also keeps four loops
			 * landing on one beat from all starting in the same millisecond) */
			c->bp_t[k] = 0.06f * rnd();
			c->bp_note[k] = (float)note;
			c->bp_vel[k] = 0.45f + 0.35f * rnd();
			c->bp_ratio[k] = BELL_RATIO[zone][k];
			c->bp_on |= (uint8_t)(1u << k);
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

/* ---- pulse: three euclidean rings of 16, 12 and 7 sixteenths ----
 * ACTIVITY the hits, TONE the shaker's and click's brightness, LENGTH the decays, REGISTER the
 * kick's pitch (half an octave a step), CHARACTER the swing (the off sixteenths up to half a
 * step late). The click's ring is turned a little against the others. */
static int euclid_hit(int k, int n, int i) { return k > 0 && ((i * k) % n) < k; }
static void pulse_fire(chan *c, int ch, int mask)
{
	float t = eff_tone(ch), l = P.ch[ch][CP_LENGTH];
	float kp = fexp2((float)(stepped(P.ch[ch][CP_REGISTER], 5) - 2) * 0.5f);
	if (mask & 1) {
		c->pk_a = (mask & 8 ? 1.0f : 0.7f) * (0.85f + 0.15f * rnd());
		c->pk_f = 120.0f * kp;
		c->pk_fend = 48.0f * kp;
		c->pk_ph = 0.0f;
		c->pk_k = fexp2(-9.9658f / ((0.12f + 0.33f * l) * VSR));
		c->glow = 1.0f;
	}
	if (mask & 2) {
		c->ps_a = (mask & 16 ? 0.6f : 0.35f) * (0.7f + 0.3f * rnd());
		c->ps_k = fexp2(-9.9658f / ((0.02f + 0.07f * l) * VSR));
	}
	if (mask & 4) {
		c->pc_a = 0.5f + 0.2f * rnd();
		c->pc_ph = 0.0f;
		c->pc_inc = (1100.0f + 1400.0f * t) / VSR;
	}
}
static void pulse_step(chan *c, int ch, uint32_t step)
{
	float h = eff_activity(ch);
	int kk = 1 + (int)(h * 3.0f + 0.5f), hk = 2 + (int)(h * 6.0f + 0.5f), ck = (int)(h * 3.0f + 0.5f);
	int s16 = (int)(step % 16u), s12 = (int)(step % 12u), s7 = (int)((step + 3u) % 7u);
	int mask = (euclid_hit(kk, 16, s16) ? 1 : 0) | (euclid_hit(hk, 12, s12) ? 2 : 0) |
	           (euclid_hit(ck, 7, s7) ? 4 : 0) | (s16 == 0 ? 8 : 0) | (s12 & 1 ? 0 : 16);
	if (!(mask & 7))
		return;
	float swing = 0.5f * P.ch[ch][CP_CHAR];
	if ((step & 1u) && swing > 0.01f) {      /* an off sixteenth: late */
		float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		c->pend = (uint8_t)mask;
		c->pend_t = swing * step_s;
		return;
	}
	pulse_fire(c, ch, mask);
}

/* ---- the sixteenth-note step: every voice's sequencer ---- */
static void chan_step(chan *c, int ch, uint32_t step)
{
	float act = eff_activity(ch);
	int base = reg_base(c, ch);
	switch (c->voice) {
	case GV_BELLS:
		if (P.ch[ch][CP_CHAR] > 0.5f || (step & 3) == 0)
			bells_beat(c, ch);            /* quarter notes, as AMB-1's; or sixteenths */
		break;
	case GV_PULSE:
		pulse_step(c, ch, step);
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
			if (!c->ks_len[k] && budget_used() + 2 > fm_cap)
				break;                       /* no room: this pluck is skipped */
			c->ks_next = (uint8_t)((k + 1) % KS_N);
			float hz = mtof((float)note) * speed;
			int len = (int)(VSR / hz);
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
			c->ks_g[k] = fexp2(-9.9658f * (float)len / (VSR * T));
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
	if (c->bp_on)                              /* bells waiting for their moment */
		for (int k = 0; k < 4; k++)
			if (c->bp_on & (1u << k)) {
				c->bp_t[k] -= sdt;
				if (c->bp_t[k] <= 0.0f) {
					c->bp_on &= (uint8_t)~(1u << k);
					if (!sounding(&c->v[k]))
						make_room(&c->v[k]);
					nv_trigger(&c->v[k], c->bp_note[k], c->bp_vel[k], c->bp_ratio[k], k);
					c->glow = 1.0f;
				}
			}
	switch (c->voice) {
	case GV_PULSE:
		if (c->pend) {
			c->pend_t -= sdt;
			if (c->pend_t <= 0.0f) {
				pulse_fire(c, ch, c->pend);
				c->pend = 0;
			}
		}
		c->glow *= 0.95f;
		break;
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
					    budget_used() + weight(c->voice) > fm_cap) {
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
			/* as AMB-1's: a soft strike when dark (~50 ms, a slowed piano), sharp when bright
			 * (4 ms); low bells ring longer; the brightness fades on its own, longer when bright */
			float att = 0.004f + 0.046f * clampf(1.0f - 2.0f * tone, 0.0f, 1.0f);
			float ring = clampf(1.0f + (60.0f - v->note) * 0.015f, 0.5f, 1.6f);
			nv_perc(v, att, expmap(len, 1.0f, 10.0f) * ring / speed, (0.35f + 1.5f * tone) / speed,
			        0.3f + 2.0f * tone, dt);
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
			/* self-feedback is as rich as a saw: high notes get less, or it folds back */
			float top = (IDX_TOP / (mtof(v->note) * speed) - 1.0f) * 0.25f;
			fbk = fbk < top ? fbk : (top > 0.0f ? top : 0.0f);
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
			float i2 = idx_cap(v, P.ch[ch][CP_CHAR] * 2.6f * v->menv2 * v->vel * IDX_SCALE, 14.0f / (v->ratio > 0.1f ? v->ratio : 1.0f));
			v->iinc2 = (i2 - v->idx2) * (1.0f / VSUB);
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
			float im = idx_cap(v, (0.4f + 2.4f * tone) * (0.25f + 0.75f * b) * IDX_SCALE, 1.0f);
			v->iinc = (im - v->idx) * (1.0f / VSUB);
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
			nv_perc(v, 0.0005f, expmap(len, 0.06f, 0.45f) / speed, 0.03f / speed, 0.6f + 2.0f * tone, dt);
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
			/* the buzzing modulator is as wide as several of its harmonics: cap as if it were */
			float fbn = v->idx2 * (1.0f / IDX_SCALE);
			ramp(v, v->env * v->vel * trem, idx_cap(v, (0.3f + 1.8f * tone) * (0.4f + 0.6f * v->menv) * IDX_SCALE,
			                                       1.0f + 2.0f * fbn));
			v->bend *= dcoef(0.15f, sdt);
			float vib = 1.0f + 0.004f * sin_u(lfo_vib + (uint32_t)k * 0x40000000u);
			float hz = mtof(v->note + v->bend) * speed * vib;
			/* the buzz is a self-modulated modulator, as rich as a saw: less of it up high,
			 * where its harmonics would fold back */
			v->idx2 = fbk * clampf((3000.0f - hz) * (1.0f / 2200.0f), 0.1f, 1.0f);
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
			float im = idx_cap(v, (0.1f + 2.6f * tone) * (0.08f + 0.92f * sh) * IDX_SCALE, 1.0f);
			v->iinc = (im - v->idx) * (1.0f / VSUB);
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
	case GV_PULSE: {
		/* each part only while it sounds */
		if (c->pk_a > 1e-4f) {                    /* the kick: a sine whose pitch falls */
			any = 1;
			float kph = c->pk_ph, kf = c->pk_f, ka = c->pk_a;
			const float fend = c->pk_fend, kk = c->pk_k;
			for (uint32_t i = 0; i < VSUB; i++) {
				kph += kf * (1.0f / VSR);
				kph -= kph >= 1.0f ? 1.0f : 0.0f;
				kf += (fend - kf) * 0.0012f;
				A[i] += sin_u((uint32_t)(kph * 4294967296.0f)) * ka * 2.6f;
				ka *= kk;
			}
			c->pk_ph = kph; c->pk_f = kf; c->pk_a = ka;
		}
		if (c->ps_a > 1e-4f) {                    /* the shaker: noise, its lows taken off */
			any = 1;
			float sa = c->ps_a, sx = c->ps_x;
			const float sk = c->ps_k, br = 0.3f + 0.7f * eff_tone((int)(c - CH));
			uint32_t r = rng;
			for (uint32_t i = 0; i < VSUB; i++) {
				r = r * 1664525u + 1013904223u;
				float w = (float)(int32_t)r * (1.0f / 2147483648.0f), sh = (w - br * sx) * sa;
				sx = w;
				A[i] += sh * 1.0f;
				B[i] += sh * 0.7f;
				sa *= sk;
			}
			rng ^= r;
			c->ps_a = sa; c->ps_x = sx;
		}
		if (c->pc_a > 1e-4f) {                    /* the click: ~15 ms of a high sine */
			any = 1;
			float cph = c->pc_ph, ca = c->pc_a;
			const float ci = c->pc_inc;
			for (uint32_t i = 0; i < VSUB; i++) {
				cph += ci;
				cph -= cph >= 1.0f ? 1.0f : 0.0f;
				B[i] += sin_u((uint32_t)(cph * 4294967296.0f)) * ca * 1.3f;
				ca *= 0.992f;
			}
			c->pc_ph = cph; c->pc_a = ca;
		}
		break;
	}
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
			for (uint32_t i = 0; i < VSUB; i++) {
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
		/* the fundamental, the sub-octave (less as TONE rises) and, as AMB-1's bass has, a little
		 * of the octave above, so it carries on small speakers; 4 dB more than before */
		float sub = v->ratio, fund = 1.0f - 0.5f * sub;
		uint32_t pc = v->pc, pm = v->pm, ic = v->ic, im = v->im;
		float amp = v->amp * 1.6f, ai = v->ainc * 1.6f, idx = v->idx, ii = v->iinc, fb = v->fb;
		for (uint32_t i = 0; i < VSUB; i++) {
			float o = sin_u(pc + (uint32_t)(int32_t)(fb * idx));
			fb = o;
			A[i] += (o * fund + SIN[pm >> TBL_SHIFT] * (0.9f * sub) + SIN[(pc << 1) >> TBL_SHIFT] * 0.15f) * amp;
			pc += ic;
			pm += im;
			amp += ai;
			idx += ii;
		}
		amp *= 1.0f / 1.6f;
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
				uint32_t m = c->ks_fill[k] < VSUB ? c->ks_fill[k] : VSUB;
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
			for (; n < VSUB; n++) {
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

/* ---- the space (echo and reverbs) is in glag_space.h, included after the distortion ---- */

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
static float d_drift = 60.0f;
static float k_freeze;
static void derive(void);
static void space_init(void);
static void space_settle(void);

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
	space_init();
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
	space_settle();                        /* the tape at its length: no slide at the start */
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

/* ================================================================ distortion
 * Five cheap waveshapers, a few operations a sample each. Driven harder at each amount, then
 * brought back down so a step changes the colour more than the loudness. */
const char *const GL_DIST_NAME[GD_COUNT] = {"Chebyshev", "Overdrive", "Fuzz", "Fold", "XOR"};
static volatile uint8_t cmd_dist_type, cmd_dist_amt, cmd_dist_post;
void gl_dist(int type, int amount, int post)
{
	cmd_dist_type = (uint8_t)((unsigned)type < GD_COUNT ? type : 0);
	cmd_dist_amt = (uint8_t)(amount < 0 ? 0 : amount > 4 ? 4 : amount);
	cmd_dist_post = (uint8_t)(post < 0 ? 0 : post > 2 ? 2 : post);
}

static struct {
	uint8_t type, amt, post;
	float g, mix, mk;          /* drive, how much of the shaped signal, make-up gain */
	int32_t prev[6];           /* XOR: the last crushed sample per path (L, R, echo, reverb, wet L, wet R) */
	float env[6];              /* Chebyshev: each path's level */
	float c2, c3;              /* Chebyshev: how much octave (T2) and twelfth (T3) */
} dist;

static void dist_setup(void)
{
	static const float DRIVE[GD_COUNT][4] = {
		{1.2f, 1.6f, 2.2f, 3.0f},      /* Chebyshev: T2 and T3 harmonics growing */
		{2.0f, 3.5f, 6.0f, 11.0f},     /* overdrive: soft saturation */
		{3.0f, 6.0f, 12.0f, 25.0f},    /* fuzz: biased hard clip */
		{1.5f, 2.3f, 3.4f, 5.0f},      /* fold: wave folding */
		{1.0f, 1.0f, 1.0f, 1.0f},      /* XOR: crushed and XORed bits */
	};
	static const float MAKEUP[GD_COUNT][4] = {
		{1.209f, 1.524f, 1.677f, 1.446f},
		{0.42f, 0.307f, 0.238f, 0.188f},
		{0.162f, 0.111f, 0.093f, 0.087f},
		{0.66f, 0.486f, 0.373f, 0.306f},
		{1.333f, 1.952f, 2.26f, 1.839f},
	};
	dist.type = cmd_dist_type;
	dist.amt = cmd_dist_amt;
	dist.post = cmd_dist_post;
	fm_dist = FM_CAP - (dist.amt ? FM_DIST_CUT : 0);
	fm_cap = fm_dist < gov_cap ? fm_dist : gov_cap;
	static const float C2[4] = {0.12f, 0.22f, 0.35f, 0.5f}, C3[4] = {0.15f, 0.3f, 0.5f, 0.75f};
	if (dist.amt) {
		dist.c2 = C2[dist.amt - 1];
		dist.c3 = C3[dist.amt - 1];
		dist.g = DRIVE[dist.type][dist.amt - 1];
		dist.mk = MAKEUP[dist.type][dist.amt - 1];
		dist.mix = 0.35f + 0.65f * (float)dist.amt * 0.25f;
	}
}

/* shape a buffer in place; `path` keeps each signal's own memory (L, R, echo, reverb). One
 * loop per type with everything in locals: the type is decided once per buffer, not per sample. */
static void dist_block(float *x, uint32_t n, int path)
{
	const float mk = dist.mk, g = dist.g, mix = dist.mix, dry = 1.0f - dist.mix;
	switch (dist.type) {
	case GD_CHEBYSHEV: {
		/* A harmonic exciter: the signal is brought to full scale by its own level (T_n makes
		 * clean harmonics only there), T2 adds the octave and T3 the twelfth, and the level is
		 * put back. T2's offset is taken out by the output's high-pass. */
		/* c2 T2(u) + c3 T3(u) = 4 c3 u^3 + 2 c2 u^2 - 3 c3 u - c2, in Horner form */
		/* the level first, so the signal never passes full scale (that would be a hard clip):
		 * this block's peak, or the last level falling over ~50 ms, whichever is higher */
		float pk = 0.0f;
		for (uint32_t i = 0; i < n; i++) {
			float a = x[i] > 0.0f ? x[i] : -x[i];
			pk = a > pk ? a : pk;
		}
		float d = dist.env[path] * 0.98f, e = pk > d ? pk : d;
		dist.env[path] = e;
		if (e < 0.0005f)
			break;                                      /* silence: nothing to excite */
		const float inv = 1.0f / e;
		const float pa = 4.0f * dist.c3 * mk, pb = 2.0f * dist.c2 * mk, pc = -3.0f * dist.c3 * mk, pd = -dist.c2 * mk;
		for (uint32_t i = 0; i < n; i++) {
			float v = x[i], u = v * inv;
			x[i] = v * mk + e * (((pa * u + pb) * u + pc) * u + pd);
		}
		break;
	}
	case GD_OVERDRIVE: {
		const float k = mk * 1.6f;
		for (uint32_t i = 0; i < n; i++) {
			float v = x[i] * g;
			x[i] = v / (1.0f + (v > 0.0f ? v : -v)) * k;
		}
		break;
	}
	case GD_FUZZ: {
		const float k = mk * 2.2f;
		for (uint32_t i = 0; i < n; i++)                /* biased: uneven halves, a buzz */
			x[i] = (clampf(x[i] * g + 0.15f, -0.55f, 0.85f) - 0.15f) * k;
		break;
	}
	case GD_FOLD: {
		/* a triangle fold: past the edge the wave turns back on itself */
		const float hg = g * 0.5f, wet = mix * 0.7f * mk, dk = dry * mk;
		for (uint32_t i = 0; i < n; i++) {
			float v = x[i] * hg + 0.25f + 64.0f;        /* kept positive: (int) is then floor */
			float t = v - (float)(int)v;
			float tri = 1.0f - 4.0f * (t > 0.5f ? t - 0.5f : 0.5f - t);
			x[i] = x[i] * dk + tri * wet;
		}
		break;
	}
	default: {
		/* crush to fewer bits, then XOR with the last crushed sample shifted down */
		static const uint8_t BITS[4] = {11, 9, 7, 5};
		static const float XMIX[4] = {0.2f, 0.4f, 0.65f, 0.9f};
		const int sh = 16 - BITS[dist.amt - 1];
		const float xm = XMIX[dist.amt - 1];
		const float wet = xm * mk * (1.0f / 32768.0f) * (float)(1 << sh), dk = (1.0f - xm) * mk;
		int32_t prev = dist.prev[path];
		for (uint32_t i = 0; i < n; i++) {
			int32_t q = (int32_t)(clampf(x[i], -1.0f, 1.0f) * 32767.0f) >> sh;
			int32_t y = q ^ (prev >> 1);
			prev = q;
			x[i] = x[i] * dk + (float)y * wet;
		}
		dist.prev[path] = prev;
		break;
	}
	}
}

#include "glag_space.h"

/* ================================================================ render */

static float lvl_ch[GL_NCH];

static void derive(void)
{
	const float dt = (float)GL_SUB / GL_SR;
	float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	d_drift = expmap(1.0f - P.g[GP_DRIFT], 10.0f, 240.0f);
	space_derive(bpm);
	k_freeze = 1.0f - dcoef(0.08f, dt);
	filter_setup(P.g[GP_FILTER]);
}

static void macro_tick(float dt)
{
	static uint8_t was_on[GL_NCH];
	for (int c = 0; c < GL_NCH; c++) {
		const uint8_t *w = MW[CH[c].voice];
		float d = mac_depth[c];
		if (d <= 0.0f) {                      /* off: put the controls back once, then cost nothing */
			if (was_on[c]) {
				for (int p = 0; p < GL_NCP; p++)
					P.ch[c][p] = Pb.ch[c][p];
				was_on[c] = 0;
			}
			continue;
		}
		was_on[c] = 1;
		for (int k = 0; k < 2; k++) {
			/* a cosine glide from one random value to the next, once per (slightly uneven) period */
			mac_ph[c][k] += dt / (mac_period[c] * (k ? 1.37f : 1.0f));
			if (mac_ph[c][k] >= 1.0f) {
				mac_ph[c][k] -= 1.0f;
				mac_from[c][k] = mac_to[c][k];
				mac_to[c][k] = 2.0f * rnd() - 1.0f;
			}
			float x = 0.5f - 0.5f * sin_u((uint32_t)(mac_ph[c][k] * 2147483648.0f) + 0x40000000u);
			mac_u[c][k] = mac_from[c][k] + (mac_to[c][k] - mac_from[c][k]) * x;
		}
		float u1 = mac_u[c][0], u2 = mac_u[c][1];
		d *= 1.0f + d;                       /* gentle at the first steps, twice as wide at full */
		P.ch[c][CP_TONE] = clampf(Pb.ch[c][CP_TONE] + d * w[0] * 0.006f * u1, 0.0f, 1.0f);
		P.ch[c][CP_CHAR] = clampf(Pb.ch[c][CP_CHAR] + d * w[1] * 0.007f * u2, 0.0f, 1.0f);
		P.ch[c][CP_LENGTH] = clampf(Pb.ch[c][CP_LENGTH] + d * w[4] * 0.005f * u2, 0.0f, 1.0f);
		P.ch[c][CP_ACTIVITY] = clampf(Pb.ch[c][CP_ACTIVITY] + d * w[5] * 0.005f * u1, 0.0f, 1.0f);
		mac_trem_ph[c] += dt * (3.0f + 2.0f * u2);   /* 1..5 Hz, wandering */
		if (mac_trem_ph[c] >= 1.0f)
			mac_trem_ph[c] -= 1.0f;
	}
}
static inline float macro_trem(int c)
{
	if (mac_depth[c] <= 0.0f)
		return 1.0f;
	float d = mac_depth[c] * (1.0f + mac_depth[c]) * 0.5f;
	float t = d * MW[CH[c].voice][2] * 0.01f;
	return 1.0f - t * 0.5f * (1.0f + sin_u((uint32_t)(mac_trem_ph[c] * 4294967295.0f)));
}

static void control(float dt)
{
	uint32_t s = post_seq;
	if (s != post_seen) {
		P = post_buf[s & 1u];
		Pb = P;
		post_seen = s;
		harmony_update();
		derive();
	}
	playing = cmd_play;
	int freeze_changed = frozen != cmd_freeze;
	frozen = cmd_freeze;
	if (cmd_dist_type != dist.type || cmd_dist_amt != dist.amt || cmd_dist_post != dist.post)
		dist_setup();
	slowing = cmd_slow;
	macro_tick(dt);
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

	/* the space (the patch-dependent parts are in derive()) */
	space_control(dt, freeze_changed);
	freeze_in += ((frozen ? 0.0f : 1.0f) - freeze_in) * k_freeze;
	gl_playing = playing;
	gl_frozen = frozen;
}

#if VOICE_HALF
/* the upsampler: a 12-tap half-band (Kaiser-windowed sinc), 27 dB down by 15 kHz and 42 by 16,
 * where the 4-point it replaces managed 13 and 16: the images of bright partials no longer ring */
static float hb_l[11], hb_r[11];
static const float HB[6] = {0.619623f, -0.166023f, 0.062933f, -0.021032f, 0.004843f, -0.000343f};
#endif
static float vd_s[2];                     /* the dark send */

void gl_render(int16_t *lr, uint32_t n, float gain)
{
	const float dt = (float)GL_SUB / GL_SR;
	float peak = gl_peak * 0.9f;
	for (uint32_t base = 0; base + GL_SUB <= n; base += GL_SUB) {
		control(dt);
		/* The instruments at 24 kHz: the dry mix (HL, HR) and the sends. Plucked and struck
		 * voices go into the reverb bright; held ones go in through a dark send (AMB-1 darkens its
		 * pad the same way), and less of them the higher they sit above C4. */
		float L[GL_SUB], R[GL_SUB], E[GL_SUB / 2], V[GL_SUB / 2], VD[GL_SUB / 2], HL[VSUB], HR[VSUB];
		for (uint32_t i = 0; i < GL_SUB / 2; i++)
			E[i] = V[i] = VD[i] = 0.0f;
		for (uint32_t i = 0; i < VSUB; i++)
			HL[i] = HR[i] = 0.0f;
		/* the sends run at 24 kHz: at full rate (a measuring build), pairs are averaged */
#define SI(i) ((i) >> (1 - VOICE_HALF))
		const float sk = VOICE_HALF ? 1.0f : 0.5f;
		for (int c = 0; c < GL_NCH; c++) {
			float g = lvl_ch[c] * 0.5f * macro_trem(c);
			if (g <= 0.0f)
				continue;                   /* silent: its envelopes still run in control() */
			int vo = CH[c].voice, st = STEREO[vo];
			float ca[VSUB], cb[VSUB];
			memset(ca, 0, sizeof ca);
			if (st)
				memset(cb, 0, sizeof cb);
			if (!chan_render(&CH[c], ca, cb))
				continue;
			float pan = P.ch[c][CP_PAN], es = P.ch[c][CP_ECHO] * g, vs = P.ch[c][CP_VERB] * g;
			float *VV = V;
			if (!PLUCK[vo]) {
				int hn = HOME[vo] + 12 * (stepped(P.ch[c][CP_REGISTER], 5) - 2);
				if (hn > 60)
					vs *= fexp2(-(float)(hn - 60) * (1.0f / 24.0f));   /* -3 dB an octave */
				VV = VD;
			}
			if (!st) {
				float al = (1.0f - pan) * g, ar = pan * g;
				for (uint32_t i = 0; i < VSUB; i++) {
					float a0 = ca[i];
					HL[i] += a0 * al;
					HR[i] += a0 * ar;
					E[SI(i)] += 4.0f * sk * a0 * es;   /* as loud in the sends as a stereo voice */
					VV[SI(i)] += 4.0f * sk * a0 * vs;
				}
				continue;
			}
			/* the two sides, spread either side of the channel's pan */
			float sp = SPREAD[vo];
			float pa = clampf(pan - sp, 0.0f, 1.0f), pb = clampf(pan + sp, 0.0f, 1.0f);
			float al = (1.0f - pa) * g, ar = pa * g, bl = (1.0f - pb) * g, br = pb * g;
			for (uint32_t i = 0; i < VSUB; i++) {
				float a0 = ca[i], b0 = cb[i];
				HL[i] += a0 * al + b0 * bl;
				HR[i] += a0 * ar + b0 * br;
				float m = 2.0f * sk * (a0 + b0);
				E[SI(i)] += m * es;
				VV[SI(i)] += m * vs;
			}
		}
		{	/* the dark send: 12 dB an octave from 1.5 kHz */
			const float k = 0.3245f;              /* 1 - exp(-2 pi 1500 / 24000) */
			float d1 = vd_s[0], d2 = vd_s[1];
			for (uint32_t i = 0; i < GL_SUB / 2; i++) {
				d1 += k * (VD[i] - d1);
				d2 += k * (d1 - d2);
				V[i] += d2;
			}
			vd_s[0] = d1;
			vd_s[1] = d2;
		}
		/* distortion before the effects: the dry mix and what goes into them, at 24 kHz. The
		 * middle of the image is distorted and the sides left clean: half the work, and
		 * instruments panned wide keep their spread. */
		if (dist.amt && dist.post == 0) {
			float M[VSUB];
			for (uint32_t i = 0; i < VSUB; i++)
				M[i] = 0.5f * (HL[i] + HR[i]);
			dist_block(M, VSUB, 0);
			for (uint32_t i = 0; i < VSUB; i++) {
				float sd = 0.5f * (HL[i] - HR[i]);
				HL[i] = M[i] + sd;
				HR[i] = M[i] - sd;
			}
			dist_block(E, GL_SUB / 2, 2);
			dist_block(V, GL_SUB / 2, 3);
		}
		/* back up to 48 kHz: each sample, and a half-band midpoint before it */
#if !VOICE_HALF
		for (uint32_t i = 0; i < GL_SUB; i++) {
			L[i] = HL[i];
			R[i] = HR[i];
		}
#else
		{	/* the last 11 samples and this block's 24, one window: no shifting */
			float wl[11 + VSUB], wr[11 + VSUB];
			for (int k = 0; k < 11; k++) {
				wl[k] = hb_l[k];
				wr[k] = hb_r[k];
			}
			for (uint32_t i = 0; i < VSUB; i++) {
				wl[11 + i] = HL[i];
				wr[11 + i] = HR[i];
			}
			for (uint32_t i = 0; i < VSUB; i++) {
				const float *a = wl + i, *b = wr + i;      /* a[0..11]: the 12 around this midpoint */
				L[2 * i] = HB[0] * (a[5] + a[6]) + HB[1] * (a[4] + a[7]) + HB[2] * (a[3] + a[8]) +
				           HB[3] * (a[2] + a[9]) + HB[4] * (a[1] + a[10]) + HB[5] * (a[0] + a[11]);
				R[2 * i] = HB[0] * (b[5] + b[6]) + HB[1] * (b[4] + b[7]) + HB[2] * (b[3] + b[8]) +
				           HB[3] * (b[2] + b[9]) + HB[4] * (b[1] + b[10]) + HB[5] * (b[0] + b[11]);
				L[2 * i + 1] = a[6];
				R[2 * i + 1] = b[6];
			}
			for (int k = 0; k < 11; k++) {
				hb_l[k] = wl[VSUB + k];
				hb_r[k] = wr[VSUB + k];
			}
		}
#endif
		/* the space, at half rate; its output interpolated back up (and, with distortion on
		 * the reverb only, that happens in there) */
		space_block(E, V, L, R);
		/* ...or after them, on everything (the DJ filter below can tame it) */
		if (dist.amt && dist.post == 1) {
			dist_block(L, GL_SUB, 0);
			dist_block(R, GL_SUB, 1);
		}
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

/* Sixteen recipes. Values are percent (0..100). Columns per channel:
 *   ACT TONE LEN REG ECHO VERB PAN CHAR LEVEL      REG: 0,25,50,75,100 = -2..+2 octaves
 * Globals: TEMPO DELAY REVERB FILTER ROOT(0..11) MODE(0..7) DRIFT MOTION */
typedef struct {
	const char *name;
	uint8_t voice[GL_NCH];
	uint8_t ch[GL_NCH][GL_NCP];
	uint8_t g[GL_NGP];
	uint8_t space[3];          /* reverb, shimmer interval, echo time (GL_ECHO_NAME) */
} recipe;

static const recipe PIECES[GL_NPIECES] = {
	/* ---- after AMB-1's eight stock pieces (Charles Vestal), in Glag's voices: its pad, bells,
	 * bass and pulse are channels 1 to 4, its numbers mapped onto Glag's controls (the pad's
	 * breaths as long as AMB-1's: LENGTH = 0.12 x Swell / 26) ---- */
	/* Garden: everything, gently */
	{"Garden", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 40, 28, 50, 27, 65, 50, 20, 60}, {40, 50, 55, 50, 45, 65, 50, 25, 65},
	  {35, 30, 85, 50, 9, 26, 50, 35, 45}, {35, 50, 40, 50, 14, 26, 50, 0, 40}},
	 {52, 45, 68, 50, 2, 1, 57, 52}, {GR_HALL, GS_OCTAVE, 3}},
	/* Departures: slowed bells an octave down over a quiet pad, as 1/1 */
	{"Departures", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 20, 25, 50, 18, 70, 50, 20, 35}, {35, 14, 55, 25, 30, 70, 50, 2, 75},
	  {35, 30, 85, 50, 6, 28, 50, 25, 0}, {0, 40, 40, 50, 9, 28, 50, 0, 0}},
	 {40, 30, 77, 50, 1, 0, 44, 45}, {GR_BLOOM, GS_OCTAVE, 3}},
	/* Voices: only the sung pad on unequal loops and a low bass, as 2/1 */
	{"Voices", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 67, 32, 50, 12, 80, 50, 70, 85}, {30, 34, 55, 50, 20, 80, 50, 15, 0},
	  {35, 30, 85, 25, 4, 32, 50, 30, 25}, {0, 40, 40, 50, 6, 32, 50, 0, 0}},
	 {36, 20, 85, 50, 5, 5, 36, 52}, {GR_SHIMMER, GS_FIFTH, 4}},
	/* Afternoon: a bright pad, busier bells, a light pulse */
	{"Afternoon", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 60, 23, 50, 30, 60, 50, 20, 50}, {55, 58, 55, 50, 50, 60, 50, 30, 65},
	  {35, 30, 85, 50, 10, 24, 50, 40, 35}, {30, 65, 40, 50, 15, 24, 50, 15, 25}},
	 {64, 50, 60, 50, 7, 3, 61, 60}, {GR_HALL, GS_OCTAVE, 2}},
	/* Night Bus: the pulse forward, metallic bells, short echoes */
	{"Night Bus", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 35, 18, 50, 33, 55, 50, 20, 45}, {45, 70, 55, 50, 55, 55, 50, 100, 55},
	  {35, 30, 85, 50, 11, 22, 50, 45, 55}, {55, 70, 40, 50, 16, 22, 50, 30, 60}},
	 {76, 55, 51, 50, 9, 5, 65, 70}, {GR_CHAMBER, GS_OCTAVE, 1}},
	/* Shoreline: dark, round bells lost in long echoes, as 2/2 */
	{"Shoreline", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 30, 37, 50, 51, 55, 50, 20, 40}, {30, 10, 55, 50, 85, 55, 50, 0, 60},
	  {35, 30, 85, 50, 17, 22, 50, 20, 30}, {0, 30, 40, 50, 26, 22, 50, 0, 0}},
	 {44, 85, 94, 50, 4, 6, 44, 72}, {GR_BLOOM, GS_OCTAVE, 4}},
	/* Glass: clangy bells an octave up, dense */
	{"Glass", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 55, 21, 50, 27, 65, 50, 20, 35}, {60, 82, 55, 75, 45, 65, 50, 45, 80},
	  {35, 30, 85, 75, 9, 26, 50, 30, 30}, {25, 85, 40, 50, 14, 26, 50, 10, 20}},
	 {56, 45, 68, 50, 3, 7, 57, 62}, {GR_SHIMMER, GS_OCTAVE, 2}},
	/* Tides: the most changeable */
	{"Tides", {GV_PAD, GV_BELLS, GV_BASS, GV_PULSE},
	 {{75, 60, 28, 50, 30, 65, 50, 70, 55}, {45, 54, 55, 50, 50, 65, 50, 28, 60},
	  {35, 30, 85, 50, 10, 26, 50, 55, 40}, {40, 50, 40, 50, 15, 26, 50, 10, 35}},
	 {50, 50, 77, 50, 10, 4, 70, 95}, {GR_HALL, GS_OCTAVE, 3}},
	/* ---- Glag's own ---- */
	{"Gate Seven", {GV_PAD, GV_PIANO, GV_DRONE, GV_BELLS},
	 {{40, 30, 75, 50, 20, 80, 50, 10, 60}, {35, 35, 60, 50, 55, 70, 45, 0, 65},
	  {40, 35, 30, 50, 0, 60, 50, 0, 45}, {25, 30, 70, 75, 50, 75, 50, 25, 50}},
	 {40, 50, 75, 50, 0, 0, 25, 40}, {GR_BLOOM, GS_OCTAVE, 3}},
	{"Long Wait", {GV_DRONE, GV_FLUTE, GV_PAD, GV_KALIMBA},
	 {{30, 45, 50, 50, 10, 70, 50, 0, 60}, {25, 35, 80, 50, 60, 80, 40, 40, 55},
	  {30, 25, 90, 75, 10, 80, 50, 60, 45}, {30, 40, 40, 50, 60, 60, 65, 50, 45}},
	 {30, 55, 85, 50, 9, 5, 15, 55}, {GR_BLOOM, GS_OCTAVE, 4}},
	{"Night Flight", {GV_PAD, GV_KALIMBA, GV_FLUTE, GV_DRONE},
	 {{45, 20, 85, 25, 20, 85, 50, 0, 60}, {40, 30, 60, 50, 65, 70, 35, 30, 50},
	  {20, 30, 90, 50, 60, 85, 65, 60, 50}, {25, 20, 40, 25, 0, 50, 50, 0, 40}},
	 {32, 60, 80, 50, 4, 7, 20, 45}, {GR_BLOOM, GS_OCTAVE, 3}},
	{"Lounge", {GV_PIANO, GV_MARIMBA, GV_PAD, GV_FLUTE},
	 {{45, 30, 70, 50, 40, 60, 40, 0, 60}, {25, 25, 45, 75, 50, 70, 65, 70, 40},
	  {35, 40, 70, 25, 20, 70, 50, 30, 55}, {25, 50, 60, 75, 55, 75, 50, 50, 45}},
	 {42, 45, 70, 50, 5, 0, 30, 50}, {GR_CHAMBER, GS_OCTAVE, 3}},
	{"Rain City", {GV_SWELL, GV_EP, GV_GONG, GV_SHIMMER},
	 {{45, 55, 75, 25, 25, 85, 50, 60, 65}, {35, 25, 55, 50, 50, 65, 40, 40, 60},
	  {30, 45, 70, 50, 10, 70, 50, 25, 55}, {25, 50, 60, 50, 70, 80, 60, 50, 45}},
	 {37, 60, 85, 50, 4, 5, 30, 55}, {GR_SHIMMER, GS_FIFTH, 3}},
	{"Deep Water", {GV_SHIMMER, GV_TAILS, GV_GONG, GV_PAD},
	 {{35, 45, 70, 50, 75, 85, 40, 30, 55}, {35, 55, 70, 50, 50, 75, 60, 50, 55},
	  {25, 40, 80, 50, 15, 80, 50, 75, 50}, {40, 25, 85, 25, 15, 85, 50, 20, 50}},
	 {28, 65, 90, 50, 2, 1, 25, 60}, {GR_SHIMMER, GS_OCTAVE, 4}},
	{"Lobby 84", {GV_EP, GV_PIZZ, GV_BASS, GV_SWELL},
	 {{45, 75, 55, 50, 40, 55, 45, 70, 75}, {45, 45, 30, 50, 45, 45, 60, 50, 70},
	  {40, 40, 50, 50, 5, 20, 50, 30, 65}, {40, 45, 60, 50, 20, 70, 50, 40, 60}},
	 {60, 40, 55, 50, 5, 0, 35, 45}, {GR_CHAMBER, GS_OCTAVE, 1}},
	{"Long Haul", {GV_PIZZ, GV_TAILS, GV_SWELL, GV_GONG},
	 {{55, 40, 25, 75, 60, 55, 40, 100, 70}, {30, 50, 75, 50, 40, 75, 60, 50, 68},
	  {40, 40, 80, 25, 20, 80, 50, 70, 62}, {25, 45, 75, 50, 10, 70, 50, 0, 65}},
	 {50, 55, 75, 50, 9, 7, 40, 60}, {GR_HALL, GS_OCTAVE, 3}},
#ifdef GL_DEMO_PIECES
	/* ---- audition only: six in styles (not in the firmware yet) ---- */
	/* Berlin school: a Pizz sequencer two octaves down, sixteenths up and down the chord */
	{"Sequencer Dawn", {GV_PIZZ, GV_SWELL, GV_ARP, GV_DRONE},
	 {{90, 45, 25, 0, 40, 40, 45, 0, 70}, {35, 40, 80, 25, 20, 75, 50, 40, 50},
	  {40, 55, 35, 75, 55, 60, 65, 33, 40}, {40, 35, 60, 25, 0, 50, 50, 0, 45}},
	 {80, 45, 70, 50, 9, 5, 30, 60}, {GR_HALL, GS_OCTAVE, 0}},
	/* krautrock: four on the floor, a driving bass */
	{"Motorik", {GV_PULSE, GV_BASS, GV_MARIMBA, GV_FLUTE},
	 {{100, 65, 35, 50, 10, 25, 50, 0, 70}, {95, 40, 40, 50, 5, 15, 50, 15, 65},
	  {70, 55, 40, 50, 45, 50, 35, 50, 50}, {35, 45, 70, 50, 50, 65, 65, 50, 40}},
	 {108, 35, 50, 50, 4, 4, 40, 45}, {GR_CHAMBER, GS_OCTAVE, 1}},
	/* Tangerine Dream's slow space: dark, climbing */
	{"Dark Orbit", {GV_PAD, GV_TAILS, GV_SHIMMER, GV_GONG},
	 {{50, 30, 80, 25, 20, 80, 50, 40, 60}, {35, 50, 75, 50, 50, 75, 60, 50, 50},
	  {30, 45, 60, 25, 60, 80, 40, 40, 40}, {30, 40, 80, 50, 15, 70, 50, 50, 55}},
	 {36, 55, 85, 50, 2, 2, 25, 75}, {GR_SHIMMER, GS_FIFTH, 4}},
	/* city pop: Rhodes, a busy bass, a swung pulse, brass swells */
	{"Night Drive", {GV_EP, GV_BASS, GV_PULSE, GV_SWELL},
	 {{55, 65, 55, 50, 35, 50, 45, 60, 70}, {85, 35, 45, 50, 5, 15, 50, 25, 65},
	  {60, 75, 40, 50, 10, 25, 50, 35, 50}, {45, 70, 50, 50, 25, 55, 55, 50, 40}},
	 {84, 30, 50, 50, 5, 3, 55, 45}, {GR_CHAMBER, GS_OCTAVE, 2}},
	/* lofi: a dark piano, a lazy swing, the DJ filter a little closed */
	{"Study Lamp", {GV_PIANO, GV_BASS, GV_PULSE, GV_KALIMBA},
	 {{45, 25, 60, 50, 35, 50, 45, 30, 70}, {70, 25, 60, 50, 5, 15, 50, 30, 65},
	  {55, 35, 45, 50, 15, 25, 50, 90, 55}, {35, 35, 55, 50, 50, 55, 65, 40, 40}},
	 {58, 40, 45, 38, 10, 1, 50, 40}, {GR_CHAMBER, GS_OCTAVE, 1}},
	/* experimental: glass bells in sixteenths, wandering intervals, octave leaps, all moving */
	{"Static Garden", {GV_BELLS, GV_DRONE, GV_TAILS, GV_PIZZ},
	 {{30, 70, 50, 75, 60, 65, 40, 100, 50}, {40, 60, 50, 25, 10, 60, 50, 100, 45},
	  {45, 60, 55, 50, 55, 65, 60, 100, 50}, {40, 50, 30, 75, 55, 55, 65, 100, 40}},
	 {52, 60, 75, 50, 1, 7, 80, 95}, {GR_BLOOM, GS_OCTAVE, 2}},
#endif
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
	for (int k = 0; k < 3; k++)
		out->space[k] = r->space[k];
	out->space[3] = 0;
#ifdef GL_FORCE_REVERB                  /* for measuring each reverb's cost */
	out->space[GL_SP_REVERB] = GL_FORCE_REVERB;
#endif
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
	p->g[GP_TEMPO] = 0.19f + 0.54f * rrnd();                 /* 39..93 BPM */
	p->g[GP_DELAY] = 0.2f + 0.5f * rrnd();
	p->g[GP_REVERB] = 0.4f + 0.5f * rrnd();
	p->g[GP_FILTER] = 0.5f;                               /* never randomized into silence */
	p->g[GP_ROOT] = (float)rrndi(12) / 11.0f;
	p->g[GP_MODE] = (float)rrndi(GL_NMODES) / (float)(GL_NMODES - 1);
	p->g[GP_DRIFT] = 0.1f + 0.6f * rrnd();
	p->g[GP_MOTION] = 0.2f + 0.6f * rrnd();
	}
