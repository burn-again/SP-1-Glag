/* SPDX-License-Identifier: MIT
 * Glag's space: the tape echo and the four reverbs. Included once, by glag_engine.c, after the
 * distortion; it uses the engine's helpers and state (P, frozen, freeze_in, dist, to16 ...).
 *
 * Both run at half rate (24 kHz) from the sends; the reverbs that run at 12 kHz average pairs
 * of input and interpolate their output back. All their memory is 16-bit.
 *
 * TAPE ECHO. A tape loop at 8 kHz, up to 4 s: one bar by default, or the piece's echo time
 * (a dotted quarter, a half, a dotted half, a bar, two bars), halved until it fits. Two heads,
 * the loop's length on the left and three quarters of it on the right; the feedback leans on the
 * first. Each repeat comes back darker and thinner, through a soft tape saturation, with a
 * little wow. Its output also feeds the reverb (Discreet Music's chain). A change of length
 * slides, as a tape would.
 *
 * REVERBS. Four, chosen on the Harmony page, sharing one 32 KB ring (only one runs at a time;
 * a change fades the wet out, clears the ring and fades the new one in). Every delay line is a
 * fixed region of the ring and there is one write pointer, moving down a step each tick, so a
 * read is an add, a mask and a load (the FV-1's trick).
 *   HALL     an eight-line FDN (Hadamard mixing) at 12 kHz: 40 ms predelay, the brightest,
 *            barely any wobble, lows ringing longer than highs
 *   CHAMBER  a four-line FDN (Householder) at 12 kHz: 8 ms predelay, mid-dark, a little wobble,
 *            the closest
 *   SHIMMER  the four-line FDN at 12 kHz with a pitch shifter in its loop: the tail climbs by an
 *            octave (open, after the Eventide and Strymon shimmers), a fifth or a minor seventh
 *   BLOOM    the four-line FDN at 12 kHz behind six long allpasses: notes swell in late, dark and
 *            wobbling, far away
 * Freeze holds any of them: the decay goes to 50 minutes and a slow servo holds the tail's level
 * (the damping would otherwise drain it), which also keeps Shimmer's climb from running away. */

/* ================================================================ tape echo */

#define TAPE_N 32768u                    /* 4.1 s at 8 kHz */
#define TAPE_SR 8000.0f
static int16_t tape[TAPE_N];
static struct {
	uint32_t w;
	int ph;                               /* 24 kHz samples into this 8 kHz one */
	float acc, len, len_want, fb, mix, lp, hp, wow, l0, r0, l1, r1;
} tp = {0, 0, 0.0f, 16000.0f, 16000.0f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
const char *const GL_ECHO_NAME[GL_NECHO] = {"1/4.", "1/2", "1/2.", "1 bar", "2 bars"};
static const float ECHO_BEATS[GL_NECHO] = {1.5f, 2.0f, 3.0f, 4.0f, 8.0f};

static inline float tape_rd(float d)     /* d 8 kHz samples ago, interpolated */
{
	uint32_t k = (uint32_t)d;
	float f = d - (float)k;
	float a = from16(tape[(tp.w - k) & (TAPE_N - 1u)]), b = from16(tape[(tp.w - k - 1u) & (TAPE_N - 1u)]);
	return a + (b - a) * f;
}

/* every 24 kHz sample: in, and the two heads out (interpolated up from 8 kHz) */
static inline void tape_tick(float in, float *el, float *er)
{
	tp.acc += in;
	if (++tp.ph >= 3) {
		tp.ph = 0;
		tp.wow += 0.4f / TAPE_SR;
		tp.wow -= tp.wow >= 1.0f ? 1.0f : 0.0f;
		float w = 1.5f * sin_u((uint32_t)(tp.wow * 4294967296.0f));
		float h1 = tape_rd(tp.len + w), h2 = tape_rd(0.75f * tp.len - w);
		float y = tp.acc * (1.0f / 3.0f) + tp.fb * (0.6f * h1 + 0.4f * h2);
		tp.lp += 0.75f * (y - tp.lp);                     /* darker each pass (~1.8 kHz) */
		tp.hp += 0.1f * (tp.lp - tp.hp);                  /* and thinner (~140 Hz) */
		y = tp.lp - tp.hp;
		y = y / (1.0f + (y > 0.0f ? y : -y) * 0.4f);     /* tape saturation */
		tp.w++;
		tape[tp.w & (TAPE_N - 1u)] = to16(y);
		tp.acc = 0.0f;
		tp.l0 = tp.l1;
		tp.r0 = tp.r1;
		tp.l1 = h1;
		tp.r1 = h2;
	}
	float f = (float)tp.ph * (1.0f / 3.0f);
	*el = tp.l0 + (tp.l1 - tp.l0) * f;
	*er = tp.r0 + (tp.r1 - tp.r0) * f;
}

static void tape_derive(float bpm, float dl, int echo)
{
	float b = ECHO_BEATS[(unsigned)echo < GL_NECHO ? echo : 3], t = b * 60.0f / bpm;
	while (t > 3.9f && b > 1.0f) {
		b *= 0.5f;
		t = b * 60.0f / bpm;
	}
	tp.len_want = clampf(t, 0.1f, 3.9f) * TAPE_SR;
	tp.fb = 0.45f + 0.4f * dl;
	tp.mix = dl < 0.02f ? 0.0f : 0.25f + 0.5f * dl;
}

/* ================================================================ reverbs */

const char *const GL_REVERB_NAME[GR_COUNT] = {"Hall", "Chamber", "Shimmer", "Bloom"};
const char *const GL_SHIMMER_NAME[GS_COUNT] = {"Octave", "Fifth", "Seventh"};

enum { ALG_FDN8, ALG_FDN4_24, ALG_FDN4_12 };
typedef struct {
	uint8_t alg, bloom, shimmer, twoband, pre_ms, fmod;
	uint16_t damp, bw;                    /* Hz: the lines' damping, the input's bandwidth */
	float lfo_hz, gain, rt_k;
} rvdef;
static const rvdef RVDEF[GR_COUNT] = {
	/* (the gains give each the same level across three pieces) */
	{ALG_FDN8, 0, 0, 1, 40, 2, 4200, 5500, 0.3f, 0.483f, 1.1f},     /* Hall */
	{ALG_FDN4_12, 0, 0, 0, 8, 3, 2800, 3500, 0.4f, 0.203f, 1.0f},   /* Chamber: its top is under 6 kHz, so 12 kHz loses nothing */
	{ALG_FDN4_12, 0, 1, 1, 20, 5, 3000, 4000, 0.4f, 0.192f, 1.0f},  /* Shimmer */
	{ALG_FDN4_12, 1, 0, 1, 30, 12, 1800, 2000, 0.25f, 0.183f, 1.0f},/* Bloom */
};
/* the shimmer's interval, and how much of each new octave's top its filter lets through */
static const float SH_RATIO[GS_COUNT] = {2.0f, 1.4983071f, 1.7817974f};
static const float SH_LPK[GS_COUNT] = {0.85f, 0.45f, 0.45f};
static const float SH_GAIN[GS_COUNT] = {1.0f, 1.057f, 1.094f};   /* the same level at each */
#define SH_W 1024                         /* the shifter's window: 85 ms at 12 kHz */

static const uint16_t L8[8] = {557, 683, 811, 947, 1069, 1213, 1361, 1511};
static const uint16_t L4_24[4] = {2311, 2803, 3343, 3761}, I4_24[4] = {115, 86, 306, 223};
static const uint16_t L4_12[4] = {1153, 1399, 1669, 1879}, I4_12[4] = {57, 43, 153, 111};
static const uint16_t BLOOM_L[6] = {613, 797, 1021, 1291, 1559, 1823};   /* 51..152 ms */
#define FMOD_ROOM 16                      /* modulated lines leave room past their length */

#define RBITS 14
#define RMASK ((1u << RBITS) - 1u)
static int16_t rring[1u << RBITS];
static uint32_t rw;
#define RRD(off) from16(rring[(rw + (uint32_t)(off)) & RMASK])
#define RWR(off, x) (rring[(rw + (uint32_t)(off)) & RMASK] = to16(x))

static struct {
	int cur, shim;                        /* the reverb running, its shimmer interval */
	float fade;                           /* the wet while switching: out, clear, in */
	const rvdef *d;
	int nl, pre;
	uint16_t len[8], ilen[4];
	uint32_t o_pre, o_in[4], o_l[8], o_b[6], o_sh;
	float g[8], gl[8], gh[8], lp[8], lo[8];
	float kd, kbw, kx, bw, mix, fmod;
	uint32_t lfo, lfo_inc;
	float sh_p, sh_lp, sh_lp2, sh_hp, sh_amt;
	float agc_env, agc_target, ag;
	float acc, ol, or_, pl, pr;
	int phase;
	float last_rv;
} rv;

static inline float rap(uint32_t off, int len, float x, float g)      /* allpass on a region */
{
	float z = RRD(off + (uint32_t)len), v = x - g * z;
	RWR(off, v);
	return z + g * v;
}
static inline float rrdf(uint32_t off, float k)                      /* interpolated read */
{
	uint32_t ki = (uint32_t)k;
	float f = k - (float)ki, a = RRD(off + ki), b = RRD(off + ki + 1u);
	return a + (b - a) * f;
}

/* lay the chosen reverb out in the ring */
static void rv_layout(int type)
{
	rv.cur = type;
	rv.d = &RVDEF[type];
	const rvdef *d = rv.d;
	int r12 = d->alg != ALG_FDN4_24;
	uint32_t o = 0;
	rv.o_pre = o;
	o += (r12 ? 480u : 960u) + 1u;       /* up to 40 ms */
	if (d->alg == ALG_FDN8) {
		rv.nl = 8;
		rv.ilen[0] = 57;
		rv.ilen[1] = 43;
		for (int k = 0; k < 2; k++) {
			rv.o_in[k] = o;
			o += rv.ilen[k] + 1u;
		}
		for (int k = 0; k < 8; k++)
			rv.len[k] = L8[k];
	} else {
		rv.nl = 4;
		if (d->bloom)
			for (int k = 0; k < 6; k++) {
				rv.o_b[k] = o;
				o += BLOOM_L[k] + 1u;
			}
		for (int k = 0; k < 4; k++) {
			rv.ilen[k] = r12 ? I4_12[k] : I4_24[k];
			rv.len[k] = r12 ? L4_12[k] : L4_24[k];
			rv.o_in[k] = o;
			o += rv.ilen[k] + 1u;
		}
	}
	for (int k = 0; k < rv.nl; k++) {
		rv.o_l[k] = o;
		o += rv.len[k] + FMOD_ROOM;
	}
	rv.o_sh = o;
	o += SH_W + 2u;
	/* (the biggest, Bloom's, takes 14.2 k of the ring's 16 k) */
	memset(rring, 0, sizeof rring);
	for (int k = 0; k < 8; k++)
		rv.lp[k] = rv.lo[k] = 0.0f;
	rv.bw = rv.acc = rv.ol = rv.or_ = rv.pl = rv.pr = 0.0f;
	rv.sh_p = rv.sh_lp = rv.sh_lp2 = rv.sh_hp = 0.0f;
	rv.agc_env = rv.agc_target = 0.0f;
	rv.ag = 1.0f;
	(void)o;
}

/* the decay and colour from REVERB (0..1), the frozen state and the shimmer interval */
static void rv_derive(float rvv)
{
	const rvdef *d = rv.d;
	rv.last_rv = rvv;
	float fr = d->alg == ALG_FDN4_24 ? 24000.0f : 12000.0f;
	float dec = clampf(0.75f + 0.22f * rvv, 0.0f, 0.975f);
	/* the same decay time as a plate whose ~0.725 s loop loses dec^4 */
	float rt = 3.0f * 0.725f / (-4.0f * flog2(dec) * 0.30103f) * d->rt_k;
	if (frozen)
		rt = 3000.0f;
	for (int k = 0; k < rv.nl; k++)
		rv.g[k] = fexp2(-9.9658f * (float)rv.len[k] / (rt * fr));
	rv.sh_amt = 0.0f;
	if (d->shimmer) {
		/* less shimmer as the tail grows longer, and room left in the loop for what it adds;
		 * frozen, the shifter takes over and the servo holds the level */
		rv.sh_amt = 0.35f * clampf((1.0f - rv.g[0]) * 12.0f, 0.0f, 1.0f);
		for (int k = 0; k < rv.nl; k++)
			rv.g[k] *= 1.0f - 0.15f * rv.sh_amt;
		if (frozen) {
			rv.sh_amt = 0.25f;
			for (int k = 0; k < rv.nl; k++)
				rv.g[k] = 0.998f;
		}
	}
	for (int k = 0; k < rv.nl; k++) {
		rv.gl[k] = rv.gh[k] = rv.g[k];
		if (d->twoband) {                /* lows outlast highs: 1.5x and 0.7x the decay */
			float base = fexp2(-9.9658f * (float)rv.len[k] / (rt * fr));
			rv.gl[k] = rv.g[k] * fexp2(-9.9658f * (float)rv.len[k] / (1.5f * rt * fr)) / base;
			rv.gh[k] = rv.g[k] * fexp2(-9.9658f * (float)rv.len[k] / (0.7f * rt * fr)) / base;
		}
	}
	float damp = frozen && d->shimmer ? 5500.0f : (float)d->damp;
	rv.kd = 1.0f - fexp2(-1.4427f * 6.2832f * clampf(damp, 100.0f, 0.45f * fr) / fr);
	rv.kbw = 1.0f - fexp2(-1.4427f * 6.2832f * clampf((float)d->bw, 100.0f, 0.45f * fr) / fr);
	rv.kx = 1.0f - fexp2(-1.4427f * 6.2832f * 400.0f / fr);
	rv.lfo_inc = (uint32_t)(d->lfo_hz / fr * 4294967296.0f);
	rv.pre = (int)((float)d->pre_ms * fr * 0.001f);
	rv.fmod = (float)d->fmod;
	rv.mix = d->gain * 2.0f * (d->shimmer ? SH_GAIN[rv.shim] : 1.0f);   /* the ring's 6 dB of headroom back */
}

/* the freeze servo: tracks the lines' energy; frozen, nudges the loop toward the level it had */
static inline float rv_servo(float e)
{
	rv.agc_env += 0.0004f * (e - rv.agc_env);
	float ag = 1.0f;
	if (frozen && rv.agc_target > 0.0f)
		ag = clampf(1.0f + 0.004f * (rv.agc_target - rv.agc_env) / rv.agc_target, 0.9f, 1.003f);
	else
		rv.agc_target = rv.agc_env;
	rv.ag = ag;
	return ag;
}

static inline __attribute__((always_inline)) void fdn4_tick(float in, float *ol, float *or_)
{
	RWR(rv.o_pre, in);
	float x = RRD(rv.o_pre + (uint32_t)rv.pre);
	rv.bw += rv.kbw * (x - rv.bw);
	x = rv.bw;
	if (rv.d->bloom)
		for (int k = 0; k < 6; k++)
			x = rap(rv.o_b[k], BLOOM_L[k], x, 0.7f);
	x = rap(rv.o_in[0], rv.ilen[0], x, 0.75f);
	x = rap(rv.o_in[1], rv.ilen[1], x, 0.75f);
	x = rap(rv.o_in[2], rv.ilen[2], x, 0.625f);
	x = rap(rv.o_in[3], rv.ilen[3], x, 0.625f);
	rv.lfo += rv.lfo_inc;
	float ms = 0.5f + 0.5f * sin_u(rv.lfo), mc = 0.5f + 0.5f * sin_u(rv.lfo + 0x40000000u);
	float d0 = RRD(rv.o_l[0] + rv.len[0]);
	float d1 = rrdf(rv.o_l[1], (float)rv.len[1] + rv.fmod * ms);
	float d2 = RRD(rv.o_l[2] + rv.len[2]);
	float d3 = rrdf(rv.o_l[3], (float)rv.len[3] + rv.fmod * mc);
	const float kd = rv.kd;
	rv.lp[0] += kd * (d0 - rv.lp[0]);
	rv.lp[1] += kd * (d1 - rv.lp[1]);
	rv.lp[2] += kd * (d2 - rv.lp[2]);
	rv.lp[3] += kd * (d3 - rv.lp[3]);
	float v[4];
	if (rv.d->twoband) {
		for (int k = 0; k < 4; k++) {
			rv.lo[k] += rv.kx * (rv.lp[k] - rv.lo[k]);
			v[k] = rv.gl[k] * rv.lo[k] + rv.gh[k] * (rv.lp[k] - rv.lo[k]);
		}
	} else {
		for (int k = 0; k < 4; k++)
			v[k] = rv.g[k] * rv.lp[k];
	}
	float h = 0.5f * (v[0] + v[1] + v[2] + v[3]);
	if (rv.d->shimmer) {
		/* the tail, shifted, back into the network: two taps sweeping a window half a window
		 * apart and crossfaded; a tap whose delay falls (ratio - 1) samples a sample plays that
		 * much faster. Two poles take the top off each new step, nothing below ~150 Hz goes
		 * back, and the servo reins it in while frozen. */
		RWR(rv.o_sh, 0.5f * (d0 + d1 + d2 + d3));
		const float r = SH_RATIO[rv.shim];
		float pa = rv.sh_p, pb = rv.sh_p + 0.5f;
		pb -= pb >= 1.0f ? 1.0f : 0.0f;
		float da = 1.0f + (float)(SH_W - 3) * (1.0f - pa), db = 1.0f + (float)(SH_W - 3) * (1.0f - pb);
		float wa = 1.0f - (pa > 0.5f ? 2.0f * pa - 1.0f : 1.0f - 2.0f * pa);
		float sh = wa * rrdf(rv.o_sh, da) + (1.0f - wa) * rrdf(rv.o_sh, db);
		rv.sh_p += (r - 1.0f) * (1.0f / (float)(SH_W - 3));
		rv.sh_p -= rv.sh_p >= 1.0f ? 1.0f : 0.0f;
		const float lk = SH_LPK[rv.shim];
		rv.sh_lp += lk * (sh - rv.sh_lp);
		rv.sh_lp2 += lk * (rv.sh_lp - rv.sh_lp2);
		rv.sh_hp += 0.08f * (rv.sh_lp2 - rv.sh_hp);
		float a4 = rv.ag * rv.ag;
		x += rv.sh_amt * a4 * a4 * (rv.sh_lp2 - rv.sh_hp);
	}
	float ag = rv_servo(d0 * d0 + d1 * d1 + d2 * d2 + d3 * d3);
	RWR(rv.o_l[0], x + ag * (v[0] - h));
	RWR(rv.o_l[1], x + ag * (v[1] - h));
	RWR(rv.o_l[2], -x + ag * (v[2] - h));
	RWR(rv.o_l[3], -x + ag * (v[3] - h));
	rw--;
	*ol = (d0 - d2 + 0.6f * d1 - 0.3f * d3) * rv.mix;
	*or_ = (d1 - d3 + 0.6f * d2 - 0.3f * d0) * rv.mix;
}

static inline __attribute__((always_inline)) void fdn8_tick(float in, float *ol, float *or_)
{
	RWR(rv.o_pre, in);
	float x = RRD(rv.o_pre + (uint32_t)rv.pre);
	rv.bw += rv.kbw * (x - rv.bw);
	x = rap(rv.o_in[0], rv.ilen[0], rv.bw, 0.75f);
	x = rap(rv.o_in[1], rv.ilen[1], x, 0.75f);
	rv.lfo += rv.lfo_inc;
	float ms = 0.5f + 0.5f * sin_u(rv.lfo), mc = 0.5f + 0.5f * sin_u(rv.lfo + 0x40000000u);
	float d[8], y[8], e = 0.0f;
#pragma GCC unroll 8
	for (int k = 0; k < 8; k++)
		d[k] = RRD(rv.o_l[k] + rv.len[k]);
	d[2] = rrdf(rv.o_l[2], (float)rv.len[2] + rv.fmod * ms);
	d[6] = rrdf(rv.o_l[6], (float)rv.len[6] + rv.fmod * mc);
#pragma GCC unroll 8
	for (int k = 0; k < 8; k++) {
		rv.lp[k] += rv.kd * (d[k] - rv.lp[k]);
		rv.lo[k] += rv.kx * (rv.lp[k] - rv.lo[k]);
		y[k] = rv.gl[k] * rv.lo[k] + rv.gh[k] * (rv.lp[k] - rv.lo[k]);
		e += d[k] * d[k];
	}
	/* Hadamard 8: three rounds of butterflies, scaled to keep the energy */
#pragma GCC unroll 8
	for (int h = 1; h < 8; h <<= 1)
		for (int i = 0; i < 8; i += h << 1)
			for (int j = i; j < i + h; j++) {
				float a = y[j], b = y[j + h];
				y[j] = a + b;
				y[j + h] = a - b;
			}
	float ag = rv_servo(0.5f * e) * 0.35355339f;
#pragma GCC unroll 8
	for (int k = 0; k < 8; k++)
		RWR(rv.o_l[k], ag * y[k] + ((k & 1) ? -0.5f : 0.5f) * x);
	rw--;
	*ol = 0.5f * (d[0] - d[2] + d[4] - d[6]) * rv.mix;
	*or_ = 0.5f * (d[1] - d[3] + d[5] - d[7]) * rv.mix;
}

/* every 24 kHz sample */
static inline __attribute__((always_inline)) void rv_tick(float in, float *ol, float *or_)
{
	in *= 0.5f;                                   /* headroom in the 16-bit ring */
	if (rv.d->alg == ALG_FDN4_24) {
		fdn4_tick(in, ol, or_);
		return;
	}
	rv.acc += in;                                 /* 12 kHz: every other call */
	if ((rv.phase ^= 1)) {
		*ol = rv.ol;
		*or_ = rv.or_;
		return;
	}
	rv.pl = rv.ol;
	rv.pr = rv.or_;
	if (rv.d->alg == ALG_FDN8)
		fdn8_tick(0.5f * rv.acc, &rv.ol, &rv.or_);
	else
		fdn4_tick(0.5f * rv.acc, &rv.ol, &rv.or_);
	rv.acc = 0.0f;
	*ol = 0.5f * (rv.pl + rv.ol);
	*or_ = 0.5f * (rv.pr + rv.or_);
}

/* ================================================================ the block */

static float sp_pl, sp_pr;                        /* the last output, for interpolating up */

static void space_init(void)
{
	memset(tape, 0, sizeof tape);
	tp.w = 0;
	tp.ph = 0;
	tp.acc = tp.lp = tp.hp = tp.l0 = tp.r0 = tp.l1 = tp.r1 = 0.0f;
	tp.len = tp.len_want;
	rv.shim = P.space[GL_SP_SHIMMER] % GS_COUNT;
	rv_layout(P.space[GL_SP_REVERB] % GR_COUNT);
	rv.fade = 1.0f;
	rv_derive(P.g[GP_REVERB]);
	sp_pl = sp_pr = 0.0f;
}

static void space_settle(void) { tp.len = tp.len_want; }

/* from derive(): the patch's tempo, delay, reverb value and choices */
static void space_derive(float bpm)
{
	tape_derive(bpm, P.g[GP_DELAY], P.space[GL_SP_ECHO]);
	if (rv.cur == P.space[GL_SP_REVERB] % GR_COUNT) {
		rv.shim = P.space[GL_SP_SHIMMER] % GS_COUNT;
		rv_derive(P.g[GP_REVERB]);
	}
}

/* every control tick: the tape's length slides; a new reverb fades the old out over 80 ms,
 * takes the ring and fades in; freezing changes the decay */
static void space_control(float dt, int freeze_changed)
{
	tp.len += (tp.len_want - tp.len) * (1.0f - dcoef(0.5f, dt));
	int want = P.space[GL_SP_REVERB] % GR_COUNT;
	if (want != rv.cur) {
		rv.fade -= dt / 0.08f;
		if (rv.fade <= 0.0f) {
			rv.fade = 0.0f;
			rv.shim = P.space[GL_SP_SHIMMER] % GS_COUNT;
			rv_layout(want);
			rv_derive(P.g[GP_REVERB]);
		}
	} else if (rv.fade < 1.0f) {
		rv.fade += dt / 0.08f;
		if (rv.fade > 1.0f)
			rv.fade = 1.0f;
	}
	if (freeze_changed)
		rv_derive(P.g[GP_REVERB]);
}

/* one sub-block: E and V hold one echo and reverb send per pair of samples (24 kHz); the
 * result is interpolated back up and added into L and R (48 kHz) */
static void space_block(const float *E, const float *V, float *L, float *R)
{
	float WL[GL_SUB / 2], WRr[GL_SUB / 2], EL[GL_SUB / 2], ER[GL_SUB / 2];
	const float fi = freeze_in, fade = rv.fade;
	for (uint32_t j = 0; j < GL_SUB / 2; j++) {
		float el, er, wl, wr;
		tape_tick(E[j] * 0.5f, &el, &er);
		rv_tick((0.5f * V[j] + (el + er) * 0.25f) * fi, &wl, &wr);
		WL[j] = wl * fade;
		WRr[j] = wr * fade;
		EL[j] = el;
		ER[j] = er;
	}
	if (dist.amt && dist.post == 2) {            /* dirty tails, clean instruments */
		dist_block(WL, GL_SUB / 2, 4);
		dist_block(WRr, GL_SUB / 2, 5);
	}
	const float emix = tp.mix;
	float pl = sp_pl, pr = sp_pr;
	for (uint32_t i = 0; i < GL_SUB; i += 2) {
		float ol = EL[i >> 1] * emix + WL[i >> 1], or_ = ER[i >> 1] * emix + WRr[i >> 1];
		L[i] += 0.5f * (pl + ol);
		R[i] += 0.5f * (pr + or_);
		L[i + 1] += ol;
		R[i + 1] += or_;
		pl = ol;
		pr = or_;
	}
	sp_pl = pl;
	sp_pr = pr;
}
