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
#if defined(GL_FLOAT_SPACE) || defined(GL_FLOAT_TAPE)   /* host only: in float, to hear what 16 bits cost */
static float tape[TAPE_N];
#define TAPE_GET(i) (tape[(i)])
float dbg_tpeak;
static inline float tpk(float x) { float a = x > 0.0f ? x : -x; if (a > dbg_tpeak) dbg_tpeak = a; return x; }
#define TAPE_PUT(i, x) (tape[(i)] = tpk(x))
#else
static int16_t tape[TAPE_N];
/* stored at 3x: the tape never holds more than ~0.42 of the 16 bits' +-2, so this is ~10 dB
 * finer, with room to spare */
#define TAPE_GET(i) (from16(tape[(i)]) * (1.0f / 3.0f))
#define TAPE_PUT(i, x) (tape[(i)] = to16((x) * 3.0f))
#endif
static struct {
	uint32_t w;
	int ph;                               /* 24 kHz samples into this 8 kHz one */
	float acc, len, len_want, fb, mix, lp, hp, wow, l0, r0, l1, r1;
	int string;                           /* the echo tuned to the chord's root: a plucked string */
} tp = {0, 0, 0.0f, 16000.0f, 16000.0f, 0.6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0};
const char *const GL_ECHO_NAME[GL_NECHO] = {"1/4.", "1/2", "1/2.", "1 bar", "2 bars", "1/8", "1/8.", "1/4", "string"};
static const float ECHO_BEATS[GL_NECHO] = {1.5f, 2.0f, 3.0f, 4.0f, 8.0f, 0.5f, 0.75f, 1.0f, 0.5f};
const uint8_t GL_ECHO_ORDER[GL_NECHO] = {GL_ECHO_STRING, 5, 6, 7, 0, 1, 2, 3, 4};

/* Grains (glag_new.h) read the tape: where it is writing, and a sample (with a fraction) */
static uint32_t grain_w(void) { return tp.w; }
static float grain_read(uint32_t i, float f)
{
	const float a = TAPE_GET(i & (TAPE_N - 1u)), b = TAPE_GET((i + 1u) & (TAPE_N - 1u));
	return a + (b - a) * f;
}
/* the echo as a read head (GF_HEAD; audition): while on, the tape is not written and a third head
 * plays it: 1 the last echo-length slice round and round (its seam crossfaded), 2 a tape stop (the
 * rate falling to nothing over 0.7 s, the level with it), 3 reverse (the rate slewing through
 * zero to -1 over ~25 ms). Off, the live echo comes back over 25 ms and the tape runs on. */
static struct { float back, rate, xf, L; uint32_t w0; uint8_t mode; } rhd;
static float hd_read(float back)
{
	const uint32_t k = (uint32_t)back;
	const float f = back - (float)k;
	const float a = TAPE_GET((rhd.w0 - k) & (TAPE_N - 1u)), b = TAPE_GET((rhd.w0 - k - 1u) & (TAPE_N - 1u));
	return a + (b - a) * f;
}
static float hd_tick(int head, float len)          /* one 8 kHz sample of the head, or 0 */
{
	if (head && !rhd.mode) {                         /* engaged: from where the long head reads */
		rhd.w0 = tp.w;
		rhd.L = len < 400.0f ? 400.0f : len > 16000.0f ? 16000.0f : len;
		rhd.back = head == 1 ? rhd.L : len;
		rhd.rate = 1.0f;
	}
	if (head)
		rhd.mode = (uint8_t)head;
	rhd.xf += ((head ? 1.0f : 0.0f) - rhd.xf) * 0.005f;
	if (!head && rhd.xf < 0.001f) {
		rhd.xf = 0.0f;
		rhd.mode = 0;
		return 0.0f;
	}
	float y, g = 1.0f;
	if (rhd.mode == 1) {
		y = hd_read(rhd.back);
		if (rhd.back < 40.0f)                        /* the seam: into the slice's start */
			y = y * (rhd.back * (1.0f / 40.0f)) + hd_read(rhd.back + rhd.L) * (1.0f - rhd.back * (1.0f / 40.0f));
		rhd.back -= 1.0f;
		if (rhd.back <= 0.0f)
			rhd.back += rhd.L;
	} else {
		if (rhd.mode == 2) {
			rhd.rate -= 1.0f / (0.7f * TAPE_SR);
			rhd.rate = rhd.rate < 0.0f ? 0.0f : rhd.rate;
			g = rhd.rate;
		} else
			rhd.rate += (-1.0f - rhd.rate) * 0.005f;
		y = hd_read(rhd.back);
		rhd.back -= rhd.rate;
		if (rhd.back < 1.0f)
			rhd.back = 1.0f;
		if (rhd.back > (float)(TAPE_N - 64u)) {      /* never past the oldest sample */
			rhd.back = (float)(TAPE_N - 64u);
			rhd.rate = 0.0f;
		}
	}
	return y * g;
}

/* the tape over a block: every 24 kHz sample in, and the two heads out (interpolated up from 8 kHz),
 * its state held in locals (no call or reload per sample) */
static float tp_wsc = 1.0f;                       /* the wow fades out as the loop is held (~125 ms) */
static void tape_block(const float *X, float *EL, float *ER, uint32_t n)
{
	float acc = tp.acc, wow = tp.wow, lp = tp.lp, hp = tp.hp, l0 = tp.l0, r0 = tp.r0, l1 = tp.l1, r1 = tp.r1;
	float wsc = tp_wsc;
	int ph = tp.ph;
	uint32_t w = tp.w;
	const float len = tp.len, fb = tp.fb;
	const int hold = tp_hold, string = tp.string, pp = w_pingpong, head = pf.head;
	const int one = pp || string;
	const float lpk = string ? 0.9f : 0.75f, hpk = string ? 0.015f : 0.1f, hd = pp ? 0.5f : 0.75f;
	/* the wow, less on a short loop (on the string, a cycle's length, it would detune it) */
	const float wl = len < 400.0f ? len * (1.0f / 400.0f) : 1.0f;
#define TRD(d, out) {                                                                      \
		const float dd = (d);                                                              \
		const uint32_t k = (uint32_t)dd;                                                   \
		const float f = dd - (float)k;                                                     \
		const float a = TAPE_GET((w - k) & (TAPE_N - 1u)), b = TAPE_GET((w - k - 1u) & (TAPE_N - 1u)); \
		out = a + (b - a) * f; }
	for (uint32_t j = 0; j < n; j++) {
		acc += X[j];
		if (++ph >= 3) {
			ph = 0;
			wow += 0.4f / TAPE_SR;
			wow -= wow >= 1.0f ? 1.0f : 0.0f;
			wsc += ((hold ? 0.0f : 1.0f) - wsc) * 0.001f;
			const float wv = 1.5f * wl * wsc * sin_u((uint32_t)(wow * 4294967296.0f));
			float h1, h2;
			TRD(len + wv, h1)
			TRD(hd * len - wv, h2)
			if (head || rhd.xf > 0.0f) {                /* the read head (audition), crossfaded in and out */
				tp.w = w;
				const float y = hd_tick(head, len), x = rhd.xf;
				h1 = h1 * (1.0f - x) + y * x;
				h2 = h2 * (1.0f - x) + y * x;
			}
			if (head) {
				/* the read head is playing: the tape stays as it is */
			} else if (hold) {
				/* frozen: the loop goes round as it is, nothing in, nothing lost */
				w++;
				TAPE_PUT(w & (TAPE_N - 1u), h1);
			} else {
				/* ping-pong and the string: only the long head feeds back */
				float y = acc * (1.0f / 3.0f) + fb * (one ? h1 : 0.6f * h1 + 0.4f * h2);
				lp += lpk * (y - lp);                   /* darker each pass */
				hp += hpk * (lp - hp);                  /* and thinner */
				y = lp - hp;
				y = y / (1.0f + (y > 0.0f ? y : -y) * 0.4f);   /* tape saturation */
				w++;
				TAPE_PUT(w & (TAPE_N - 1u), y);
			}
			acc = 0.0f;
			l0 = l1;
			r0 = r1;
			l1 = h1;
			r1 = h2;
		}
		const float f = (float)ph * (1.0f / 3.0f);
		EL[j] = l0 + (l1 - l0) * f;
		ER[j] = r0 + (r1 - r0) * f;
	}
#undef TRD
	tp.acc = acc; tp.wow = wow; tp.lp = lp; tp.hp = hp; tp.l0 = l0; tp.r0 = r0; tp.l1 = l1; tp.r1 = r1;
	tp.ph = ph; tp.w = w;
	tp_wsc = wsc;
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
	tp.string = echo == GL_ECHO_STRING;
	if (tp.string) {
		/* Karplus-Strong: one cycle of the chord's root (C3 .. B3) round the loop, fed back hard,
		 * so whatever goes in rings at that pitch; DELAY sets how long it sustains */
		tp.len_want = TAPE_SR / mtof((float)(48 + chord_pc[0]));
		tp.fb = 0.86f + 0.12f * dl;
	}
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
static uint32_t rw;
#if defined(GL_FLOAT_SPACE) || defined(GL_FLOAT_RING)
static float rring[1u << RBITS];
float dbg_rpeak;                          /* the largest value each memory had to hold */
static inline float rpk(float x) { float a = x > 0.0f ? x : -x; if (a > dbg_rpeak) dbg_rpeak = a; return x; }
#define RRD(off) (rring[(rw + (uint32_t)(off)) & RMASK])
#define RWR(off, x) (rring[(rw + (uint32_t)(off)) & RMASK] = rpk(x))
#else
static int16_t rring[1u << RBITS];
#define RRD(off) from16(rring[(rw + (uint32_t)(off)) & RMASK])
#define RWR(off, x) (rring[(rw + (uint32_t)(off)) & RMASK] = to16(x))
#endif

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
	float ig;                             /* the input's gain: 1, less when the ring runs hot */
	float ez[6];                          /* the elliptic low-pass before 12 kHz */
	float sz[4];                          /* and before the shimmer's shifter */
	float tz[2];                          /* the reverbs' tone: a pole and a zero at 12 kHz */
	float ul[13], ur[13];                 /* the last 12 kHz outputs, for the half-band up */
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

static inline float sqrtf_fast(float x)    /* control rate only: two Newton steps from a bit guess */
{
	union { float f; int32_t i; } u;
	u.f = x;
	u.i = 0x1FBD1DF5 + (u.i >> 1);
	float y = u.f;
	y = 0.5f * (y + x / y);
	return 0.5f * (y + x / y);
}

/* audition (bits): 1 the Hall on four lines, 2 one damping band in every reverb (no lows outlasting
 * highs), 4 the four-line Hall with the Hall's own two short diffusers (the other two out), 8 and with
 * four of the Hall's own lines. A copy of the definition carries the change */
static uint8_t rv_cut, rv_hall4;
/* audition: the Hall's lines drifting in length as it rings (1 growing then shrinking, ten seconds each
 * way; 2 half growing, half shrinking; 3 all growing slowly; 4 all shrinking slowly), and the
 * Shimmer's interval set to any ratio (below 1: down) */
#define DROOM 700u                                /* (room each line may grow into: 58 ms at 12 kHz) */
static uint8_t rv_drift;
static float rv_dr, rv_dt, rv_shr;
void gl_rv_drift(int mode) { rv_drift = (uint8_t)mode; rv_dt = 0.0f; rv.cur = -1; }
void gl_rv_shim_ratio(float r) { rv_shr = r; }
/* audition: odd tails (1 crushed more the longer it rings; 2 crushed again on every pass, in the
 * Hall's loop; 3 and 4 an endless rise or fall in the Shimmer's loop, a semitone a pass with a copy an
 * octave away fading in; 5 a ring tone stepping up the scale; 6 two ring tones in contrary motion,
 * left rising, right falling; 7 and 8 a ring tone stepping the scale by Fibonacci numbers, up or down) */
static uint8_t rv_fx;
#ifndef SHEP_AMT
#define SHEP_AMT 0.5f
#define SHEP_G 1.0f
#endif
static float rvx_t, rvx_pk, rvx_env, rvx_q, rvx_h[2], rvx_hn, sh_p2;
static uint32_t rvx_ph[2], rvx_n, rvx_step, rvx_fa, rvx_fb;
void gl_rv_fx(int mode) { rv_fx = (uint8_t)mode; rvx_t = 99.0f; rvx_step = 0; rvx_fa = 0; rvx_fb = 1; }
static rvdef rv_alt;
static uint8_t rv_cut_user, rv_cut_lean;
void gl_rv_cut(int bits)                          /* (laid out again on the next block) */
{
	rv_cut_user = (uint8_t)bits;
	const uint8_t eff = (uint8_t)(rv_cut_user | rv_cut_lean);
	if (eff != rv_cut) { rv_cut = eff; rv.cur = -1; }
}
/* (the governor's: the Hall on four of its own lines; laid out again only if it is the Hall playing) */
static void rv_lean_set(int bits)
{
	rv_cut_lean = (uint8_t)bits;
	const uint8_t eff = (uint8_t)(rv_cut_user | rv_cut_lean);
	if (eff != rv_cut) {
		rv_cut = eff;
		if (rv.cur == GR_HALL)
			rv.cur = -1;
	}
}
/* lay the chosen reverb out in the ring */
static void rv_layout(int type)
{
	rv.cur = type;
	rv_alt = RVDEF[type];
	rv_hall4 = 0;
	if ((rv_cut & 1) && rv_alt.alg == ALG_FDN8) {
		rv_hall4 = 1;
		rv_alt.alg = ALG_FDN4_12;
		rv_alt.rt_k *= (rv_cut & 8) ? 0.65f : 0.5f;                     /* (four lines hold their energy longer: half the time, to end where the eight do) */
		rv_alt.gain *= (rv_cut & 8) ? 0.38f : 0.45f;                    /* (and ring louder: brought to about level) */
	}
	if (rv_cut & 2)
		rv_alt.twoband = 0;
	rv.d = &rv_alt;
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
			if (rv_hall4 && (rv_cut & 4) && k >= 2)
				rv.ilen[k] = 1;                         /* (audition: only the Hall's two short diffusers) */
			if (rv_hall4 && (rv_cut & 8))
				rv.len[k] = L8[2 * k + 1];               /* (audition: four of the Hall's own lines) */
			rv.o_in[k] = o;
			o += rv.ilen[k] + 1u;
		}
	}
	for (int k = 0; k < rv.nl; k++) {
		rv.o_l[k] = o;
		o += rv.len[k] + FMOD_ROOM + (rv_drift && rv.nl == 8 ? DROOM : 0u);
	}
	rv.o_sh = o;
	o += SH_W + 2u;
	/* (the biggest, Bloom's, takes 14.2 k of the ring's 16 k) */
	clear32(rring, sizeof rring);
	for (int k = 0; k < 8; k++)
		rv.lp[k] = rv.lo[k] = 0.0f;
	rv.bw = 0.0f;
	memset(rv.ez, 0, sizeof rv.ez);
	memset(rv.sz, 0, sizeof rv.sz);
	rv.tz[0] = rv.tz[1] = 0.0f;
	memset(rv.ul, 0, sizeof rv.ul);
	memset(rv.ur, 0, sizeof rv.ur);
	rv.sh_p = rv.sh_lp = rv.sh_lp2 = rv.sh_hp = 0.0f;
	rv.agc_env = rv.agc_target = 0.0f;
	rv.ag = 1.0f;
	rv.ig = 1.0f;
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
	if (rv_hold)
		rt = 3000.0f;
	for (int k = 0; k < rv.nl; k++)
		rv.g[k] = fexp2(-9.9658f * (float)rv.len[k] / (rt * fr));
	rv.sh_amt = 0.0f;
	if (d->shimmer) {
		/* less shimmer as the tail grows longer, and room left in the loop for what it adds;
		 * frozen, the shifter takes over and the servo holds the level */
		rv.sh_amt = 0.35f * clampf((1.0f - rv.g[0]) * 12.0f, 0.0f, 1.0f);
		if ((rv_shr > 0.0f || rv_fx == 3 || rv_fx == 4) && rv.sh_amt < 0.25f)
			rv.sh_amt = 0.25f;                       /* (audition: the shifted tail heard even at the longest) */
		if (rv_fx == 3 || rv_fx == 4) {              /* (audition: the shifter carries most of the loop) */
			rv.sh_amt = SHEP_AMT;
			for (int k = 0; k < rv.nl; k++)
				rv.g[k] *= SHEP_G;
		}
		for (int k = 0; k < rv.nl; k++)
			rv.g[k] *= 1.0f - 0.15f * rv.sh_amt;
		if (rv_hold) {
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
	float damp = rv_hold && d->shimmer ? 5500.0f : (float)d->damp;
	rv.kd = 1.0f - fexp2(-1.4427f * 6.2832f * clampf(damp, 100.0f, 0.45f * fr) / fr);
	rv.kbw = 1.0f - fexp2(-1.4427f * 6.2832f * clampf((float)d->bw, 100.0f, 0.45f * fr) / fr);
	rv.kx = 1.0f - fexp2(-1.4427f * 6.2832f * 400.0f / fr);
	rv.lfo_inc = (uint32_t)(d->lfo_hz / fr * 4294967296.0f);
	rv.pre = (int)((float)d->pre_ms * fr * 0.001f);
	rv.fmod = (float)d->fmod;
	rv.mix = d->gain * (d->shimmer ? SH_GAIN[rv.shim] : 1.0f);
}

/* the freeze servo: tracks the lines' energy; frozen, nudges the loop toward the level it had */
static inline float rv_servo(float e)
{
	rv.agc_env += 0.0004f * (e - rv.agc_env);
	float ag = 1.0f;
	if (rv_hold && rv.agc_target > 0.0f)
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
		 * much faster. Two more poles soften each new step, nothing below ~150 Hz goes
		 * back, and the servo reins it in while frozen. */
		/* what goes in is cut at 2.7 kHz first (elliptic, 4 poles: -47 dB at 4): shifted up an
		 * octave, anything over 3 kHz would pass 6 and fold back down off-pitch */
		{
			float in = rv_fx == 3 || rv_fx == 4 ? 0.5f * (d0 + d1 - d2 - d3) : 0.5f * (d0 + d1 + d2 + d3), *z = rv.sz;
			float y = 0.069917512f * in + z[0];
			z[0] = 0.125911025f * in + 0.686835095f * y + z[1];
			z[1] = 0.069917512f * in - 0.264994175f * y;
			float y2 = y + z[2];
			z[2] = 1.128872815f * y + 0.220904689f * y2 + z[3];
			z[3] = y - 0.744281625f * y2;
			RWR(rv.o_sh, y2);
		}
		const int shep = rv_fx == 3 || rv_fx == 4;
		const float r = shep ? (rv_fx == 3 ? 1.0594631f : 0.9438743f) : rv_shr > 0.0f ? rv_shr : SH_RATIO[rv.shim];
		float pa = rv.sh_p, pb = rv.sh_p + 0.5f;
		pb -= pb >= 1.0f ? 1.0f : 0.0f;
		float da = 1.0f + (float)(SH_W - 3) * (1.0f - pa), db = 1.0f + (float)(SH_W - 3) * (1.0f - pb);
		float wa = 1.0f - (pa > 0.5f ? 2.0f * pa - 1.0f : 1.0f - 2.0f * pa);
		float sh = wa * rrdf(rv.o_sh, da) + (1.0f - wa) * rrdf(rv.o_sh, db);
		rv.sh_p += (r - 1.0f) * (1.0f / (float)(SH_W - 3));
		rv.sh_p -= rv.sh_p >= 1.0f ? 1.0f : 0.0f;
		rv.sh_p += rv.sh_p < 0.0f ? 1.0f : 0.0f;          /* (a ratio below 1: the taps wrap the other way) */
		if (shep) {                                /* (the copy an octave away, fading in to replace it) */
			const float r2 = rv_fx == 3 ? 0.5297316f : 1.8877486f;
			float qa = sh_p2, qb = sh_p2 + 0.5f;
			qb -= qb >= 1.0f ? 1.0f : 0.0f;
			float ea = 1.0f + (float)(SH_W - 3) * (1.0f - qa), eb = 1.0f + (float)(SH_W - 3) * (1.0f - qb);
			float va = 1.0f - (qa > 0.5f ? 2.0f * qa - 1.0f : 1.0f - 2.0f * qa);
			sh += 0.5f * (va * rrdf(rv.o_sh, ea) + (1.0f - va) * rrdf(rv.o_sh, eb));
			sh_p2 += (r2 - 1.0f) * (1.0f / (float)(SH_W - 3));
			sh_p2 -= sh_p2 >= 1.0f ? 1.0f : 0.0f;
			sh_p2 += sh_p2 < 0.0f ? 1.0f : 0.0f;
		}
		const float lk = shep ? 0.3f : SH_LPK[rv.shim];
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
	if (rv_drift) {                                /* (audition: every line read where its drift has it) */
		rv_dt += 1.0f / 12000.0f;
		float a, b;
		if (rv_drift == 1) {                       /* growing ten seconds, shrinking ten */
			const float ph = rv_dt * 0.05f - (float)(int)(rv_dt * 0.05f);
			a = b = (float)(DROOM - 4u) * (ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph);
		} else if (rv_drift == 2) {                /* half one way, half the other, over 80 s */
			const float u = rv_dt < 80.0f ? rv_dt / 80.0f : 1.0f, c = 0.5f * (float)(DROOM - 4u);
			a = c + c * u;
			b = c - c * u;
		} else {                                   /* all growing (3) or shrinking (4), over 80 s */
			const float u = rv_dt < 80.0f ? rv_dt / 80.0f : 1.0f;
			a = b = (float)(DROOM - 4u) * (rv_drift == 3 ? u : 1.0f - u);
		}
		rv_dr = a;
		for (int k = 0; k < 8; k++)
			d[k] = rrdf(rv.o_l[k], (float)rv.len[k] + ((k & 1) ? b : a) + (k == 2 ? rv.fmod * ms : k == 6 ? rv.fmod * mc : 0.0f));
	}
#pragma GCC unroll 8
	for (int k = 0; k < 8; k++) {
		rv.lp[k] += rv.kd * (d[k] - rv.lp[k]);
		if (rv.d->twoband) {
			rv.lo[k] += rv.kx * (rv.lp[k] - rv.lo[k]);
			y[k] = rv.gl[k] * rv.lo[k] + rv.gh[k] * (rv.lp[k] - rv.lo[k]);
		} else
			y[k] = rv.g[k] * rv.lp[k];
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
	if (rv_fx == 2 && rvx_q > 0.0f) {              /* (audition: every pass rounded to a coarse step) */
		const float iq = 1.0f / rvx_q;
		for (int k = 0; k < 8; k++) {
			float u = y[k] * iq;
			y[k] = (float)(int)(u + (u >= 0.0f ? 0.5f : -0.5f)) * rvx_q;
		}
	}
#pragma GCC unroll 8
	for (int k = 0; k < 8; k++)
		RWR(rv.o_l[k], ag * y[k] + ((k & 1) ? -0.5f : 0.5f) * x);
	rw--;
	*ol = 0.5f * (d[0] - d[2] + d[4] - d[6]) * rv.mix;
	*or_ = 0.5f * (d[1] - d[3] + d[5] - d[7]) * rv.mix;
}

/* the half-band's odd taps, doubled (they sum to 0.5); its centre is 0.5 */
static const float RHB[7] = {0.62557792f, -0.18066706f, 0.08055428f, -0.03579357f,
	0.01376638f, -0.00389876f, 0.00046081f};

/* one sub-block of the reverb: X, the send at 24 kHz (used up), into WL and WR at 24 kHz */
static void rv_block(float *X, float *WL, float *WR)
{
	enum { N = GL_SUB / 2, M = GL_SUB / 4 };
	/* in at full scale (it used to be half: 6 dB coarser in the 16-bit ring, a grain on quiet
	 * tails), turned down by the guard only when the ring runs hot */
	const float g = rv.ig;
	if (rv.d->alg == ALG_FDN4_24) {
		for (uint32_t j = 0; j < N; j++)
			fdn4_tick(X[j] * g, &WL[j], &WR[j]);
		return;
	}
	/* 12 kHz: down through an elliptic low-pass (6 poles: flat to 4.5 kHz, -60 dB from 6), up
	 * through a half-band (27 taps: -72 dB from 8 kHz). Averaging pairs, as it was, let a
	 * note's upper harmonics fold back down as off-pitch tones (a 2093 Hz note's third
	 * harmonic came back at 5721 Hz): a faint hiss on every hit above ~C4 */
	float z0 = rv.ez[0], z1 = rv.ez[1], z2 = rv.ez[2], z3 = rv.ez[3], z4 = rv.ez[4], z5 = rv.ez[5];
	for (uint32_t j = 0; j < N; j++) {
		float in = X[j] * g;
		float y = 0.013726642f * in + z0;
		z0 = 0.022248823f * in + 1.165985614f * y + z1;
		z1 = 0.013726642f * in - 0.421639763f * y;
		float y2 = y + z2;
		z2 = 0.394244539f * y + 0.886805138f * y2 + z3;
		z3 = y - 0.679369305f * y2;
		float d = y2 + z4;
		z4 = -0.083017639f * y2 + 0.714510458f * d + z5;
		z5 = y2 - 0.907053722f * d;
		X[j] = d;
	}
	rv.ez[0] = z0; rv.ez[1] = z1; rv.ez[2] = z2; rv.ez[3] = z3; rv.ez[4] = z4; rv.ez[5] = z5;
	float A[13 + M], B[13 + M];
	for (uint32_t k = 0; k < 13; k++) {          /* (not memcpy: the M4's is byte by byte) */
		A[k] = rv.ul[k];
		B[k] = rv.ur[k];
	}
	/* the tone the reverbs were voiced with (-3.5 dB at 2 kHz, -11 at 4): what the filters
	 * around the old pair-averaging gave, as one pole and a zero at 12 kHz */
	float t0 = rv.tz[0], t1 = rv.tz[1];
	for (uint32_t m = 0; m < M; m++) {
		float x = X[2 * m + 1];
		t0 += 0.68f * (0.5f * (x + t1) - t0);
		t1 = x;
		X[2 * m + 1] = t0;
	}
	rv.tz[0] = t0; rv.tz[1] = t1;
	if (rv.d->alg == ALG_FDN8)
		for (uint32_t m = 0; m < M; m++)
			fdn8_tick(X[2 * m + 1], &A[13 + m], &B[13 + m]);
	else
		for (uint32_t m = 0; m < M; m++)
			fdn4_tick(X[2 * m + 1], &A[13 + m], &B[13 + m]);
	for (uint32_t m = 0; m < M; m++) {
		const float *a = &A[m + 13], *b = &B[m + 13];
		WL[2 * m] = a[-7];
		WR[2 * m] = b[-7];
		WL[2 * m + 1] = RHB[0] * (a[-7] + a[-6]) + RHB[1] * (a[-8] + a[-5]) + RHB[2] * (a[-9] + a[-4]) +
			RHB[3] * (a[-10] + a[-3]) + RHB[4] * (a[-11] + a[-2]) + RHB[5] * (a[-12] + a[-1]) +
			RHB[6] * (a[-13] + a[0]);
		WR[2 * m + 1] = RHB[0] * (b[-7] + b[-6]) + RHB[1] * (b[-8] + b[-5]) + RHB[2] * (b[-9] + b[-4]) +
			RHB[3] * (b[-10] + b[-3]) + RHB[4] * (b[-11] + b[-2]) + RHB[5] * (b[-12] + b[-1]) +
			RHB[6] * (b[-13] + b[0]);
	}
	for (uint32_t k = 0; k < 13; k++) {
		rv.ul[k] = A[M + k];
		rv.ur[k] = B[M + k];
	}
}

/* (audition) the odd tails after the reverb, at 24 kHz; X the send */
static float rvx_note(int deg)
{
	const int n = scale_n > 0 ? scale_n : 7, o = deg >= 0 ? deg / n : -((n - 1 - deg) / n);
	const int d = deg - o * n;
	return mtof((float)(48 + 12 * o + ((scale_pc[d] - root_pc + 12) % 12) + root_pc % 12));
}
static void rv_fx_block(const float *X, float *WL, float *WR, uint32_t n)
{
	float pk = 0.0f;
	for (uint32_t j = 0; j < n; j++)
		pk = X[j] > pk ? X[j] : -X[j] > pk ? -X[j] : pk;
	if (pk > 2.0f * rvx_pk + 0.002f)
		rvx_t = 0.0f;                              /* a new hit */
	rvx_pk += (pk - rvx_pk) * (pk > rvx_pk ? 0.5f : 0.02f);
	rvx_t += (float)n / 24000.0f;
	rvx_q = sqrtf_fast(rv.agc_env * 0.125f + 1e-12f) * 1.0f;
	if (rv_fx == 1) {                              /* fewer bits and fewer samples the longer it rings */
		const float t = rvx_t;
		const float bits = t < 5.0f ? 8.0f - 1.2f * t : 2.0f;
		const float hold = t < 5.0f ? 1.0f + 1.4f * t : 8.0f;
		for (uint32_t j = 0; j < n; j++) {
			float a = (WL[j] > 0.0f ? WL[j] : -WL[j]) + (WR[j] > 0.0f ? WR[j] : -WR[j]);
			rvx_env += (a - rvx_env) * (a > rvx_env ? 0.01f : 0.0003f);
			if ((rvx_hn += 1.0f) >= hold) {
				rvx_hn -= hold;
				const float q = (rvx_env + 1e-6f) * 2.0f * fexp2(-bits), iq = 1.0f / q;
				float u = WL[j] * iq, v = WR[j] * iq;
				rvx_h[0] = (float)(int)(u + (u >= 0.0f ? 0.5f : -0.5f)) * q;
				rvx_h[1] = (float)(int)(v + (v >= 0.0f ? 0.5f : -0.5f)) * q;
			}
			WL[j] = rvx_h[0];
			WR[j] = rvx_h[1];
		}
		return;
	}
	if (rv_fx < 5)
		return;
	/* the ring tones: a step every so often, each step a note of the song's scale from C3 */
	const uint32_t every = rv_fx == 6 ? 8000u : rv_fx >= 7 ? 6000u : 4000u;
	if ((rvx_n += n) >= every) {
		rvx_n -= every;
		rvx_step++;
		const uint32_t m = 2u * (uint32_t)(scale_n > 0 ? scale_n : 7), c = (rvx_fa + rvx_fb) % m;
		rvx_fa = rvx_fb;
		rvx_fb = c;
	}
	const int sn = scale_n > 0 ? scale_n : 7;
	float fa, fb;
	if (rv_fx == 5)
		fa = fb = rvx_note((int)(rvx_step % (uint32_t)(2 * sn)));
	else if (rv_fx == 6) {
		fa = rvx_note((int)(rvx_step % (uint32_t)(2 * sn)));
		fb = rvx_note(2 * sn - (int)(rvx_step % (uint32_t)(2 * sn)));
	} else
		fa = fb = rvx_note(rv_fx == 7 ? (int)rvx_fa : 2 * sn - (int)rvx_fa);
	const uint32_t ia = (uint32_t)(fa / 24000.0f * 4294967296.0f), ib = (uint32_t)(fb / 24000.0f * 4294967296.0f);
	for (uint32_t j = 0; j < n; j++) {
		rvx_ph[0] += ia;
		rvx_ph[1] += ib;
		WL[j] *= 0.3f + 0.9f * sin_u(rvx_ph[0]);
		WR[j] *= 0.3f + 0.9f * sin_u(rvx_ph[1]);
	}
}

/* ================================================================ the block */

static float sp_up[6];                            /* the last outputs, for interpolating up */
static float sp_lp[12];                           /* the filters around the tape */

/* the tape's elliptic low-pass, 2.4 kHz at 24 kHz, two sections (z: 4 states), over n samples */
static void tb4(float *out, const float *x, float k, float *z, uint32_t n)
{
	float z0 = z[0], z1 = z[1], z2 = z[2], z3 = z[3];
	for (uint32_t j = 0; j < n; j++) {
		float in = x[j];
		float y = 0.00863182f * in + z0;
		z0 = 0.0098654f * in + 1.44647804f * y + z1;
		z1 = 0.00863182f * in - 0.57178381f * y;
		float y2 = y + z2;
		z2 = -0.38875277f * y + 1.46156061f * y2 + z3;
		z3 = y - 0.83106997f * y2;
		out[j] = y2 * k;
	}
	z[0] = z0; z[1] = z1; z[2] = z2; z[3] = z3;
}

static void space_init(void)
{
	clear32(tape, sizeof tape);
	tp.w = 0;
	tp.ph = 0;
	tp.acc = tp.lp = tp.hp = tp.l0 = tp.r0 = tp.l1 = tp.r1 = 0.0f;
	tp.len = tp.len_want;
	rv.shim = P.space[GL_SP_SHIMMER] % GS_COUNT;
	rv_layout(P.space[GL_SP_REVERB] % GR_COUNT);
	rv.fade = 1.0f;
	rv_derive(P.g[GP_REVERB]);
	memset(sp_up, 0, sizeof sp_up);
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
	/* the string follows the chord: each new root, the loop retunes (gliding, like a bend) */
	if (tp.string)
		tp.len_want = TAPE_SR / mtof((float)(48 + chord_pc[0]));
	/* the heads glide to a new length like a tape slowing or speeding up: no click, a short swoop */
	tp.len += (tp.len_want - tp.len) * (1.0f - dcoef(1.6f, dt));
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
	/* the guard: the lines' level (rms) held under ~0.35 of the ring's 2.0, which no stock piece
	 * reaches; past it, the input comes down in ~100 ms and goes back up over ~2 s */
	if (!rv_hold) {
		float rms2 = rv.agc_env * (rv.nl == 8 ? 0.0625f : 0.25f);
		float want = rms2 > 0.1225f ? 0.35f / sqrtf_fast(rms2) : 1.0f;
		want = want < 0.2f ? 0.2f : want;
		rv.ig += (want - rv.ig) * (want < rv.ig ? 0.01f : 0.0005f);
	}
}

/* one sub-block: E and V hold one echo and reverb send per pair of samples (24 kHz); the result
 * is added into L and R: at 24 kHz into the voices' mix (GL_SPACE24: brought up to 48 kHz with
 * it, by its half-band), or interpolated up on its own and added at 48 kHz (as it was) */
static void space_block(const float *E, const float *V, float *L, float *R, int at24)
{
	float WL[GL_SUB / 2], WRr[GL_SUB / 2], EL[GL_SUB / 2], ER[GL_SUB / 2];
	const float fi = freeze_in, fade = rv.fade * sp_duck;
	/* The tape (8 kHz) between two elliptic low-passes (4 poles: flat to 2.4 kHz, -32 dB at 4,
	 * -55 from 5): before it, so a note's upper harmonics can't fold back as off-pitch tones;
	 * after it, for the mirror images its interpolation leaves (a 2093 Hz repeat had one at
	 * 5907). The reverb does its own. */
	float X[GL_SUB / 2];
	tb4(X, E, 0.5f, sp_lp, GL_SUB / 2);
	tape_block(X, EL, ER, GL_SUB / 2);
	tb4(EL, EL, 1.0f, sp_lp + 4, GL_SUB / 2);
	tb4(ER, ER, 1.0f, sp_lp + 8, GL_SUB / 2);
	for (uint32_t j = 0; j < GL_SUB / 2; j++)
		X[j] = (0.5f * V[j] + (EL[j] + ER[j]) * 0.25f) * fi;
	{
		float X0[GL_SUB / 2];
		if (rv_fx)
			for (uint32_t j = 0; j < GL_SUB / 2; j++)
				X0[j] = X[j];
		rv_block(X, WL, WRr);
		if (rv_fx)
			rv_fx_block(X0, WL, WRr, GL_SUB / 2);
	}
	reverse_block(WL, WRr, GL_SUB / 2);
	haze_write(WL, WRr, GL_SUB / 2);              /* (Haze: the tail kept, audition builds only) */
	if (dist.amt && dist.post == 2) {            /* dirty tails, clean instruments */
		dist_block(WL, GL_SUB / 2, 4);
		dist_block(WRr, GL_SUB / 2, 5);
	}
	const float emix = (rhd.xf > 0.0f && tp.mix < 0.55f ? tp.mix + (0.55f - tp.mix) * rhd.xf : tp.mix) * sp_duck;   /* (the read head heard even on a quiet echo) */
	if (at24) {
		for (uint32_t j = 0; j < GL_SUB / 2; j++) {
			L[j] += EL[j] * emix + WL[j] * fade;
			R[j] += ER[j] * emix + WRr[j] * fade;
		}
		return;
	}
	/* back up to 48 kHz as the voices are: each new sample, and a 4-point midpoint before it */
	float u0 = sp_up[0], u1 = sp_up[1], u2 = sp_up[2], u3 = sp_up[3], u4 = sp_up[4], u5 = sp_up[5];
	for (uint32_t i = 0; i < GL_SUB; i += 2) {
		float ol = EL[i >> 1] * emix + WL[i >> 1] * fade, or_ = ER[i >> 1] * emix + WRr[i >> 1] * fade;
		L[i] += (9.0f * (u1 + u2) - u0 - ol) * (1.0f / 16.0f);
		R[i] += (9.0f * (u4 + u5) - u3 - or_) * (1.0f / 16.0f);
		L[i + 1] += u2;
		R[i + 1] += u5;
		u0 = u1; u1 = u2; u2 = ol;
		u3 = u4; u4 = u5; u5 = or_;
	}
	sp_up[0] = u0; sp_up[1] = u1; sp_up[2] = u2; sp_up[3] = u3; sp_up[4] = u4; sp_up[5] = u5;
}
