/* Glag: the Kit, a second drum voice. Included by glag_engine.c after the Pulse.
 *
 * Seven sounds rebuilt after the 1997 drum recipes (Autechre's Chiastic Slide / Envane /
 * Cichlisuite era), each synthesised here at 24 kHz, each with the four controls its recipe
 * has, driven by the channel's controls:
 *   canvas kick      five membrane modes, a slack glide, a fingertip attack
 *   R-8 snare        two falling sines and noise, R-8 "feel" (each hit a little different),
 *                    phono-input crunch
 *   metal hat        808-style six squares, band-passed, then frequency-shifted (an IIR Hilbert
 *                    pair: the shift slides as it rings); long when LENGTH is up, open on
 *                    the off-beats past half CHARACTER
 *   DX100 clank      two operators with feedback, 12-bit grit
 *   tissue paper     crackle through a wandering band, handling rumble, a stroke envelope
 *   Nord sync hit    a hard-synced saw sweeping down onto a triangle, FM, a resonant filter
 *                    sweep (run at twice the rate, so the sync does not alias)
 *   EPS crunch roll  the snare retriggered in an accelerating roll, its pitch stepping,
 *                    through sample-rate and bit reduction
 * CHARACTER splits the kit: low, the core (kick, snare, hat, clank) tight and clean; up, the
 * tissue strokes and the Nord hits come in, then rolls and open hats, and every sound's own
 * controls move with it (crunch, shift, sweep, bits). TONE is the brightness, LENGTH the decays,
 * REGISTER the pitch, ACTIVITY the hits. The lanes are of different lengths (16, 16, 12, 7, 10,
 * 13) and drift against each other. */

/* The Kit's own rate: KIT_KSUB samples a millisecond (12, 16 or 24: 12, 16 or 24 kHz; the default
 * 16, where the tunings were made), raised to the channel's 24 kHz at the end
 * (kit_render). KP(a) carries a one-pole's coefficient, written for 24 kHz, to the Kit's rate
 * (folded by the compiler). */
#ifndef KIT_KSUB
#define KIT_KSUB 16
#endif
#define KSUB ((uint32_t)KIT_KSUB)
#define KSR (VSR * (float)KIT_KSUB / (float)VSUB)
#define KRAT ((float)VSUB / (float)KIT_KSUB)
#define KP(a) (1.0f - __builtin_powf(1.0f - (a), KRAT))
/* the same for a coefficient only known at run time (a tuning's): no powf in the firmware, so by
 * the engine's own exp2 and log2 */
static inline float kpv(float a)
{
	float b = 1.0f - a;
	return b <= 0.0f ? 1.0f : 1.0f - fexp2(KRAT * flog2(b));
}
static inline uint32_t kinc(float hz) { return (uint32_t)(hz * (4294967296.0f / KSR)); }
static uint32_t kit_r = 0x6D2B79F5u;
static inline float kn(void)
{
	kit_r = kit_r * 1664525u + 1013904223u;            /* (an LCG: its top bits are plenty for noise) */
	return (float)(int32_t)kit_r * (1.0f / 2147483648.0f);
}
static inline float ku(void) { return 0.5f + 0.5f * kn(); }
static inline float ksat(float x) { x = clampf(x, -3.0f, 3.0f); return x * (27.0f + x * x) / (27.0f + 9.0f * x * x); }
static inline float kexp(float t_s) { return t_s > 0.0f ? fexp2(-6.644f / (t_s * KSR)) : 0.0f; }   /* down 40 dB in t seconds */

/* a trapezoidal state-variable filter's coefficients (stable up to the top of the band):
 * g = tan(pi fc / sr), k = 1 / Q */
static inline float ktan(float f_over_sr)
{
	uint32_t p = (uint32_t)(clampf(f_over_sr, 0.0005f, 0.24f) * 2147483648.0f);
	return sin_u(p) / sin_u(p + 0x40000000u);
}
typedef struct { float a1, a2, a3, k; } ksvf;
static inline ksvf ksvf_set(float g, float k)
{
	ksvf f;
	f.a1 = 1.0f / (1.0f + g * (g + k));
	f.a2 = g * f.a1;
	f.a3 = g * f.a2;
	f.k = k;
	return f;
}
/* one sample: s1, s2 the states; returns the band, the low in *lp */
static inline float ksvf_run(const ksvf *f, float *s1, float *s2, float x, float *lp)
{
	float v3 = x - *s2, v1 = f->a1 * *s1 + f->a2 * v3, v2 = *s2 + f->a2 * *s1 + f->a3 * v3;
	*s1 = 2.0f * v1 - *s1;
	*s2 = 2.0f * v2 - *s2;
	*lp = v2;
	return v1;
}

static const float K_MEM[5] = {1.0f, 1.594f, 2.136f, 2.296f, 2.653f};
static const float K_MEMA[5] = {1.0f, 0.6f, 0.45f, 0.35f, 0.3f};
/* the 808's six squares (Hz), as phase steps at the Kit's rate */
#define KM(f) ((uint32_t)((f) * 4294967296.0 / (24000.0 * KIT_KSUB / 24.0) + 0.5))
static const uint32_t K_METAL[6] = {KM(205.3), KM(304.4), KM(369.6), KM(522.7), KM(540.0), KM(800.0)};
static const float K_RAT[4] = {1.41f, 3.5f, 7.07f, 0.5f};
/* the Hilbert pair: two chains of all-passes (c - z^-2) / (1 - c z^-2), the first a sample later,
 * 90 degrees apart within 0.5 degree from 600 Hz to 11.5 kHz (the other sideband 48 dB down):
 * two sections each are enough for the hat's band (fitted for 24 kHz) */
static const float K_HA[2] = {0.4111137f, 0.9009403f};
static const float K_HB[2] = {0.1288429f, 0.6854797f};

/* the Lich snare's delay line: the end of the channel's string buffer */
#define KSD_OFF 2816
#define KSD_MAX 255
static void kit_snare_hit(kit_snare *s, float semis, float dec, float nu, float drive, float vel)
{
	float k = fexp2(semis * (1.0f / 12.0f));
	s->e1 = 185.0f * k * (6.2832f / KSR);
	s->e2 = 330.0f * k * (6.2832f / KSR);
	if (s->x1 * s->x1 + s->y1 * s->y1 < 0.5f)
		s->x1 = 0.0f, s->y1 = 1.0f;
	if (s->x2 * s->x2 + s->y2 * s->y2 < 0.5f)
		s->x2 = 0.0f, s->y2 = 1.0f;
	s->drop = 0.4f;
	s->dk = fexp2(-1.4427f / (0.008f * 1000.0f));       /* (once a millisecond) */
	s->b = vel;
	s->bk = kexp(0.06f + dec * 0.15f);
	s->n = vel;
	s->nk = kexp(0.12f + dec * 0.5f);
	s->lpk = clampf((3000.0f + nu * 800.0f) * (6.2832f / KSR), 0.05f, 0.95f);
	s->nl = (0.3f + nu * (0.8f / 15.0f)) * 1.6f;
	s->drive = drive;
}
/* a block of the snare, added into A at gain g */
static void kit_snare_block(kit_snare *s, float *A, float g, const float *eg)
{
	if (s->b < 1e-3f && s->n < 1e-3f)
		return;
	const float d = 1.0f + s->drop, e1 = s->e1 * d, e2 = s->e2 * d;
	s->drop *= s->dk;
	float x1 = s->x1, y1 = s->y1, x2 = s->x2, y2 = s->y2, b = s->b, n = s->n, hp = s->hp, l1 = s->l1, l2 = s->l2, lo = s->lo;
	const float bk = s->bk, nk = s->nk, lpk = s->lpk, nl = s->nl, dr = s->drive;
	const float dg = 1.0f + 5.0f * dr, lg = 4.0f * dr, og = g / (1.0f + 0.7f * dr);
	for (uint32_t i = 0; i < KSUB; i++) {
		x1 += e1 * y1;
		y1 -= e1 * x1;
		x2 += e2 * y2;
		y2 -= e2 * x2;
		float body = (x1 * 0.7f + x2 * 0.45f) * b;
		float w = kn();
		hp += KP(0.33f) * (w - hp);                         /* ~1.5 kHz high-pass */
		l1 += lpk * (w - hp - l1);
		l2 += lpk * (l1 - l2);
		float y = body + l2 * n * nl;
		b *= bk;
		n *= nk;
		if (dr > 0.0f) {                                /* the phono input: lows up, then crunch */
			lo += KP(0.028f) * (y - lo);
			float u = clampf((y + lo * lg) * dg, -1.5f, 1.5f);
			A[i] += (u - 0.148148f * u * u * u) * og * (eg ? eg[i] : 1.0f);   /* (a cubic: flat at 1.5) */
		} else
			A[i] += y * g * (eg ? eg[i] : 1.0f);
	}
	s->x1 = x1; s->y1 = y1; s->x2 = x2; s->y2 = y2; s->b = b; s->n = n; s->hp = hp; s->l1 = l1; s->l2 = l2; s->lo = lo;
}

/* the key: semitones above the root of the scale's degree d (any integer, octaves included) */
static int kit_deg(int d)
{
	int n = scale_n > 0 ? scale_n : 7, o = d >= 0 ? d / n : -((-d + n - 1) / n), i = d - o * n;
	int rel = scale_n > 0 ? (scale_pc[i] - root_pc + 12) % 12 : 2 * i;
	return rel + 12 * o;
}
static float kit_hz(int midi) { return 440.0f * fexp2((float)(midi - 69) * (1.0f / 12.0f)); }

/* ---- the tuning: every number the Kit tuning page reaches (gl_kit_tune), at the firmware's
 * defaults (written by tune/gen_tunings.py from tune/params.py: edit that) ---- */
enum {
	KT_L_KICK,
	KT_L_SNARE,
	KT_L_HAT,
	KT_L_CLANK,
	KT_L_TISSUE,
	KT_L_NORD,
	KT_L_ROLL,
	KT_L_TOMS,
	KT_OUT,
	KT_K_PITCH,
	KT_K_TUNED,
	KT_K_DECAY,
	KT_K_SLACK,
	KT_K_CLICK,
	KT_S_PITCH,
	KT_S_TUNED,
	KT_S_DECAY,
	KT_S_NOISE,
	KT_S_CRUNCH,
	KT_S_GHOSTS,
	KT_S_SQUASH,
	KT_H_DECAY,
	KT_H_BAND,
	KT_H_SHIFT,
	KT_H_DARK,
	KT_H_OPEN,
	KT_C_DECAY,
	KT_C_INDEX,
	KT_C_OCT,
	KT_C_RATE,
	KT_T_LEN,
	KT_T_DENS,
	KT_N_DECAY,
	KT_N_SWEEP,
	KT_N_OCT,
	KT_N_CUT,
	KT_R_SPEED,
	KT_R_HITS,
	KT_R_CRUSH,
	KT_TOM_DECAY,
	KT_TOM_FILL,
	KT_F_PRESS,
	KT_F_LONG,
	KT_F_HATACC,
	KT_F_THREES,
	KT_F_SHORT,
	KT_F_LONGX,
	KT_F_ACCX,
	KT_F_DYN,
	KT_COL,
	KT_SOG_BASE,
	KT_SOG_REACH,
	KT_SOG_CLOSE,
	KT_SOG_RES,
	KT_SAG,
	KT_CH_DRIVE,
	KT_CH_T2,
	KT_CH_T3,
	KT_LFO_DEPTH,
	KT_LFO_RATE,
	KT_COMB_FB,
	KT_COMB_DAMP,
	KT_LOW_HZ,
	KT_K_MODES,
	KT_K_SPREAD,
	KT_E_KICK_A,
	KT_E_KICK_D,
	KT_E_KICK_S,
	KT_E_KICK_G,
	KT_E_KICK_R,
	KT_E_SNARE_A,
	KT_E_SNARE_D,
	KT_E_SNARE_S,
	KT_E_SNARE_G,
	KT_E_SNARE_R,
	KT_E_HAT_A,
	KT_E_HAT_D,
	KT_E_HAT_S,
	KT_E_HAT_G,
	KT_E_HAT_R,
	KT_E_CLANK_A,
	KT_E_CLANK_D,
	KT_E_CLANK_S,
	KT_E_CLANK_G,
	KT_E_CLANK_R,
	KT_E_TISSUE_A,
	KT_E_TISSUE_D,
	KT_E_TISSUE_S,
	KT_E_TISSUE_G,
	KT_E_TISSUE_R,
	KT_E_NORD_A,
	KT_E_NORD_D,
	KT_E_NORD_S,
	KT_E_NORD_G,
	KT_E_NORD_R,
	KT_E_ROLL_A,
	KT_E_ROLL_D,
	KT_E_ROLL_S,
	KT_E_ROLL_G,
	KT_E_ROLL_R,
	KT_E_TOMS_A,
	KT_E_TOMS_D,
	KT_E_TOMS_S,
	KT_E_TOMS_G,
	KT_E_TOMS_R,
	KT_LICH_KICK,
	KT_LICH_SNARE,
	KT_LICH_HAT,
	KT_LICH_CLANK,
	KT_LICH_TISSUE,
	KT_LICH_NORD,
	KT_LICH_RATE,
	KT_LICH_ROUGH,
	KT_LICH_STRETCH,
	KT_LICH_BEADS,
	KT_LICH_RUNG,
	KT_LICH_FALL,
	KT_RES_MIX,
	KT_EV_AMT,
	KT_EV_MACRO,
	KT_EV_FORM,
	KT_EV_LICH,
	KT_EV_METAL,
	KT_VEL_RAND,
	KT_VEL_GROOVE,
	KT_VEL_GDEPTH,
	KT_ARC_AMT,
	KT_ARC_WEIRD,
	KT_MAP_ON,
	KT_MAP_FIX,
	KT_MAP_X,
	KT_MAP_Y,
	KT_THROWS,
	KT_DROPOUTS,
	KT_PUMPS,
	KT_EMORPH,
	KT_RING_MIX,
	KT_RING_SEMI,
	KT_SPICE_MAC,
	KT_K_FIT,
	KT_WET_AMT,
	KT_WET_OCT,
	KT_WET_LP,
	KT_OCT_MOVE,
	KT_IV_HITS,
	KT_IV_P35,
	KT_IV_PALL,
	KT_IV_SET,
	KT_CHO_MIX,
	KT_CHO_RATE,
	KT_CHO_DEPTH,
	KT_CHO_WIDE,
	KT_CHO_SWELL,
	KT_CHO_DUCK,
	KT_VTHROW,
	KT_FILL_RATE,
	KT_FILL_TYPE,
	KT_NORD_FILLX,
	KT_FILL_SET,
	KT_COUNT
};
static float KT[KT_COUNT] = {
	1.5f,   /* L_KICK: Levels Kick */
	0.35f,   /* L_SNARE: Levels Snare */
	1.0f,   /* L_HAT: Levels Hats */
	0.8f,   /* L_CLANK: Levels Clank */
	0.65f,   /* L_TISSUE: Levels Tissue */
	0.7f,   /* L_NORD: Levels Nord */
	0.22f,   /* L_ROLL: Levels Roll */
	1.2f,   /* L_TOMS: Levels Toms */
	0.9f,   /* OUT: Levels Whole kit */
	-2.5f,   /* K_PITCH: Kick Pitch */
	1.0f,   /* K_TUNED: Kick Follow the chord */
	3.0f,   /* K_DECAY: Kick Decay */
	1.0f,   /* K_SLACK: Kick Slack (pitch bend) */
	0.9f,   /* K_CLICK: Kick Click */
	-15.0f,   /* S_PITCH: Snare Pitch */
	1.0f,   /* S_TUNED: Snare Follow the chord */
	1.0f,   /* S_DECAY: Snare Decay */
	1.1f,   /* S_NOISE: Snare Noise (wires) */
	0.6f,   /* S_CRUNCH: Snare Crunch */
	2.5f,   /* S_GHOSTS: Snare Ghost notes */
	2.0f,   /* S_SQUASH: Snare Squash chance */
	1.0f,   /* H_DECAY: Hats Decay */
	1.5f,   /* H_BAND: Hats Band */
	1.0f,   /* H_SHIFT: Hats Frequency shift */
	0.9f,   /* H_DARK: Hats Long hat darkening */
	0.25f,   /* H_OPEN: Hats Open hat chance */
	2.5f,   /* C_DECAY: Clank Decay */
	0.8f,   /* C_INDEX: Clank FM index */
	0.0f,   /* C_OCT: Clank Octave */
	1.0f,   /* C_RATE: Clank How often */
	1.0f,   /* T_LEN: Tissue Stroke length */
	1.5f,   /* T_DENS: Tissue Crackle density */
	1.5f,   /* N_DECAY: Nord Decay */
	2.0f,   /* N_SWEEP: Nord Sync sweep */
	0.0f,   /* N_OCT: Nord Octave */
	0.9f,   /* N_CUT: Nord Cutoff */
	1.3f,   /* R_SPEED: Roll Speed */
	1.3f,   /* R_HITS: Roll Hits */
	1.2f,   /* R_CRUSH: Roll Crush */
	1.0f,   /* TOM_DECAY: Toms Decay */
	3.0f,   /* TOM_FILL: Toms Fill chance */
	3.0f,   /* F_PRESS: Feel Press every */
	3.0f,   /* F_LONG: Feel Long snare every */
	3.0f,   /* F_HATACC: Feel Hat accent every */
	1.0f,   /* F_THREES: Feel Bar-4 kick in threes */
	0.5f,   /* F_SHORT: Feel Normal hit length */
	1.1f,   /* F_LONGX: Feel Long hit length */
	1.5f,   /* F_ACCX: Feel Accent length */
	2.0f,   /* F_DYN: Feel Macro dynamics */
	2.0f,   /* COL: Colour Colour */
	250.0f,   /* SOG_BASE: Colour Soggy: resting cutoff */
	4.5f,   /* SOG_REACH: Colour Soggy: how far it opens */
	30.0f,   /* SOG_CLOSE: Colour Soggy: close time */
	0.6f,   /* SOG_RES: Colour Soggy: damping */
	1.0f,   /* SAG: Colour Soggy: pitch sag */
	1.6f,   /* CH_DRIVE: Colour Cheby: drive */
	0.3f,   /* CH_T2: Colour Cheby: 2nd harmonic */
	0.22f,   /* CH_T3: Colour Cheby: 3rd harmonic */
	1.0f,   /* LFO_DEPTH: Colour Filter LFO depth */
	1.0f,   /* LFO_RATE: Colour Filter LFO speed */
	0.62f,   /* COMB_FB: Colour Comb-soggy: feedback */
	0.35f,   /* COMB_DAMP: Colour Comb-soggy: brightness */
	250.0f,   /* LOW_HZ: Colour Comb-soggy: low split */
	0.33f,   /* K_MODES: Kick Skin modes */
	1.0f,   /* K_SPREAD: Kick Mode spread */
	0.0f,   /* E_KICK_A: Kick Env attack */
	0.0f,   /* E_KICK_D: Kick Env decay */
	1.0f,   /* E_KICK_S: Kick Env sustain */
	2000.0f,   /* E_KICK_G: Kick Env gate */
	20.0f,   /* E_KICK_R: Kick Env release */
	17.0f,   /* E_SNARE_A: Snare Env attack */
	220.0f,   /* E_SNARE_D: Snare Env decay */
	0.5f,   /* E_SNARE_S: Snare Env sustain */
	1100.0f,   /* E_SNARE_G: Snare Env gate */
	1200.0f,   /* E_SNARE_R: Snare Env release */
	100.0f,   /* E_HAT_A: Hats Env attack */
	0.0f,   /* E_HAT_D: Hats Env decay */
	1.0f,   /* E_HAT_S: Hats Env sustain */
	2000.0f,   /* E_HAT_G: Hats Env gate */
	20.0f,   /* E_HAT_R: Hats Env release */
	150.0f,   /* E_CLANK_A: Clank Env attack */
	0.0f,   /* E_CLANK_D: Clank Env decay */
	1.0f,   /* E_CLANK_S: Clank Env sustain */
	2000.0f,   /* E_CLANK_G: Clank Env gate */
	20.0f,   /* E_CLANK_R: Clank Env release */
	100.0f,   /* E_TISSUE_A: Tissue Env attack */
	0.0f,   /* E_TISSUE_D: Tissue Env decay */
	1.0f,   /* E_TISSUE_S: Tissue Env sustain */
	2000.0f,   /* E_TISSUE_G: Tissue Env gate */
	20.0f,   /* E_TISSUE_R: Tissue Env release */
	0.0f,   /* E_NORD_A: Nord Env attack */
	0.0f,   /* E_NORD_D: Nord Env decay */
	1.0f,   /* E_NORD_S: Nord Env sustain */
	2000.0f,   /* E_NORD_G: Nord Env gate */
	20.0f,   /* E_NORD_R: Nord Env release */
	100.0f,   /* E_ROLL_A: Roll Env attack */
	0.0f,   /* E_ROLL_D: Roll Env decay */
	1.0f,   /* E_ROLL_S: Roll Env sustain */
	440.0f,   /* E_ROLL_G: Roll Env gate */
	20.0f,   /* E_ROLL_R: Roll Env release */
	0.0f,   /* E_TOMS_A: Toms Env attack */
	0.0f,   /* E_TOMS_D: Toms Env decay */
	1.0f,   /* E_TOMS_S: Toms Env sustain */
	2000.0f,   /* E_TOMS_G: Toms Env gate */
	20.0f,   /* E_TOMS_R: Toms Env release */
	0.0f,   /* LICH_KICK: Lich Kick: NES triangle */
	0.0f,   /* LICH_SNARE: Lich Snare: Karplus-Strong drum */
	0.0f,   /* LICH_HAT: Lich Hats: LFSR noise */
	0.0f,   /* LICH_CLANK: Lich Clank: 1-bit */
	0.0f,   /* LICH_TISSUE: Lich Tissue: PhISEM shaker */
	0.0f,   /* LICH_NORD: Lich Nord: rungler */
	2.64f,   /* LICH_RATE: Lich LFSR clock */
	0.45f,   /* LICH_ROUGH: Lich KS drum roughness */
	2.51f,   /* LICH_STRETCH: Lich KS drum ring */
	1.27f,   /* LICH_BEADS: Lich Shaker beads */
	3.0f,   /* LICH_RUNG: Lich Rungler depth */
	2.78f,   /* LICH_FALL: Lich Triangle kick fall */
	0.7f,   /* RES_MIX: Colour Resonance level */
	0.0f,   /* EV_AMT: Evolve Weird, always */
	0.8f,   /* EV_MACRO: Evolve Weird with the macro */
	0.0f,   /* EV_FORM: Evolve Form */
	1.0f,   /* EV_LICH: Evolve Weird swaps to Lich */
	1.0f,   /* EV_METAL: Evolve Weird gets metallic */
	0.15f,   /* VEL_RAND: Velocity Randomness */
	1.0f,   /* VEL_GROOVE: Velocity Groove */
	0.5f,   /* VEL_GDEPTH: Velocity Groove depth */
	0.6f,   /* ARC_AMT: Arc Arc */
	0.6f,   /* ARC_WEIRD: Arc Arc into weird */
	1.0f,   /* MAP_ON: Arc Wander the map */
	0.0f,   /* MAP_FIX: Arc Hold the map at */
	0.0f,   /* MAP_X: Arc Map X */
	0.0f,   /* MAP_Y: Arc Map Y */
	1.2f,   /* THROWS: Arc Delay throws */
	1.0f,   /* DROPOUTS: Arc Dropouts */
	1.0f,   /* PUMPS: Arc Pumping */
	1.0f,   /* EMORPH: Arc Euclid morph */
	0.4f,   /* RING_MIX: Colour Ring mod: amount */
	12.0f,   /* RING_SEMI: Colour Ring mod: note */
	0.75f,   /* SPICE_MAC: Arc Macro follows the spice */
	0.7f,   /* K_FIT: Kick Fit the gap */
	0.6f,   /* WET_AMT: Restraint Colour amount */
	-12.0f,   /* WET_OCT: Restraint Colour octave */
	4000.0f,   /* WET_LP: Restraint Colour cutoff */
	0.3f,   /* OCT_MOVE: Restraint Colour octave moves */
	0.4f,   /* IV_HITS: Restraint Colour intervals */
	0.3f,   /* IV_P35: Restraint Phrases with 3rd + 5th */
	0.4f,   /* IV_PALL: Restraint Phrases with all four */
	15.0f,   /* IV_SET: Restraint Which intervals */
	0.5f,   /* CHO_MIX: Restraint Chorus */
	0.35f,   /* CHO_RATE: Restraint Chorus speed */
	2.5f,   /* CHO_DEPTH: Restraint Chorus depth */
	0.8f,   /* CHO_WIDE: Restraint Chorus width */
	0.25f,   /* CHO_SWELL: Restraint Chorus width swell */
	0.06f,   /* CHO_DUCK: Restraint Chorus duck */
	0.0f,   /* VTHROW: Arc Reverb throws */
	1.0f,   /* FILL_RATE: Arc Fills */
	0.0f,   /* FILL_TYPE: Arc Fill */
	2.0f,   /* NORD_FILLX: Arc Fill Nord length */
	31127.0f,   /* FILL_SET: Arc Fills to pick from */
};
/* @@KIT_TUNINGS (written by tune/gen_tunings.py from tune/tunings.py: edit those) */
#define KIT_NTUNE 15
static const char *const KIT_TUNE_NAME[KIT_NTUNE] = {"Glag", "Cumbia Bell", "Brush Boom", "Brush Comb", "Brush Comb Wet", "Drill Shimmer", "Soggy", "Chip Vespers", "String Drum Mass", "Rungler Rain", "Bead Machine", "1-bit Funeral", "Rule of Three", "Lich Canvas", "Brush Ring"};
static const float KIT_TUNE[KIT_NTUNE][KT_COUNT] = {
	{ /* Glag */
		1.5f, 0.35f, 1.0f, 0.8f, 0.65f, 0.7f, 0.22f, 1.2f, 0.9f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 2.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 17.0f, 220.0f, 0.5f, 1100.0f, 1200.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.64f, 0.45f, 2.51f, 1.27f, 3.0f, 2.78f, 0.7f, 0.0f, 0.8f, 0.0f, 1.0f, 1.0f, 0.15f, 1.0f, 0.5f, 0.6f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.7f, 0.6f, -12.0f, 4000.0f, 0.3f, 0.4f, 0.3f, 0.4f, 15.0f, 0.5f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Cumbia Bell */
		2.81f, 0.22f, 0.68f, 1.11f, 0.78f, 1.26f, 1.0f, 0.72f, 0.73f, -9.1f, 1.0f, 2.08f, 1.27f, 1.0f, -16.0f, 1.0f, 1.0f, 1.0f, 1.0f, 10.0f, 4.15f, 2.43f, 1.41f, 0.74f, 1.0f, 0.25f, 2.13f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 2.48f, 5.02f, 3.0f, 0.72f, 1.36f, 2.29f, 2.13f, 1.07f, 7.61f, 2.0f, 3.0f, 1.0f, 1.0f, 0.3f, 5.0f, 0.57f, 5.73f, 2.0f, 426.0f, 3.4f, 134.0f, 0.15f, 5.74f, 0.49f, 1.87f, 0.08f, 0.64f, 16.86f, 0.06f, 1.0f, 189.0f, 0.37f, 1.02f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.99f, 2000.0f, 221.0f, 75.0f, 1795.0f, 1.0f, 2000.0f, 16.0f, 0.0f, 0.0f, 0.4f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 14.0f, 0.0f, 0.08f, 540.0f, 1261.0f, 137.0f, 186.0f, 1.0f, 2000.0f, 20.0f, 310.0f, 1693.0f, 0.93f, 2000.0f, 1487.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Brush Boom */
		4.0f, 0.17f, 0.68f, 1.11f, 0.78f, 1.26f, 0.09f, 0.72f, 1.03f, -2.6f, 1.0f, 6.8f, 1.36f, 0.0f, -12.5f, 1.0f, 5.23f, 0.04f, 2.56f, 9.61f, 8.56f, 7.89f, 1.41f, 0.74f, 1.0f, 0.25f, 2.13f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 2.48f, 5.02f, 3.0f, 7.2f, 1.36f, 2.29f, 2.13f, 1.07f, 7.61f, 1.0f, 9.0f, 7.0f, 1.0f, 0.46f, 3.19f, 1.11f, 3.74f, 0.0f, 255.0f, 0.95f, 892.0f, 2.0f, 5.74f, 6.44f, 0.28f, 1.74f, 4.99f, 2.59f, 0.94f, 0.19f, 1560.0f, 0.22f, 1.07f, 58.0f, 1697.0f, 0.79f, 191.0f, 1031.0f, 336.0f, 1000.0f, 0.31f, 365.0f, 1471.0f, 75.0f, 1795.0f, 1.0f, 2000.0f, 1475.0f, 0.0f, 0.0f, 0.4f, 2000.0f, 1816.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 1372.0f, 14.0f, 0.0f, 0.08f, 540.0f, 1261.0f, 137.0f, 186.0f, 1.0f, 2000.0f, 1479.0f, 310.0f, 1693.0f, 0.93f, 2000.0f, 1487.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Brush Comb */
		3.34f, 0.19f, 0.23f, 0.09f, 0.41f, 0.35f, 0.2f, 0.33f, 0.87f, -2.1f, 1.0f, 6.67f, 0.83f, 0.93f, -18.2f, 1.0f, 4.93f, 1.0f, 0.98f, 2.41f, 2.78f, 1.0f, 1.09f, 1.0f, 0.9f, 0.25f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.7f, 1.26f, 1.0f, 0.0f, 0.89f, 1.25f, 1.0f, 0.8f, 0.91f, 1.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 1.72f, 7.0f, 3000.0f, 1.85f, 1000.0f, 0.69f, 3.55f, 0.38f, 0.07f, 0.24f, 6.0f, 6.72f, 0.31f, 0.85f, 672.0f, 0.33f, 1.0f, 32.0f, 0.0f, 1.0f, 2000.0f, 205.0f, 0.0f, 0.0f, 0.94f, 2000.0f, 16.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 106.0f, 268.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.92f, 2000.0f, 320.0f, 99.0f, 0.0f, 1.0f, 436.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 18.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Brush Comb Wet */
		3.34f, 0.19f, 0.23f, 0.09f, 0.41f, 0.35f, 0.2f, 0.33f, 0.87f, -2.1f, 1.0f, 6.67f, 0.83f, 0.93f, -18.2f, 1.0f, 4.93f, 1.0f, 0.98f, 2.41f, 2.78f, 1.0f, 1.09f, 1.0f, 0.9f, 0.25f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.7f, 1.26f, 1.0f, 0.0f, 0.89f, 1.25f, 1.0f, 0.8f, 0.91f, 1.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 1.72f, 7.0f, 443.0f, 6.8f, 766.0f, 0.34f, 6.0f, 0.43f, 0.52f, 0.27f, 6.0f, 20.0f, 0.85f, 0.19f, 745.0f, 0.33f, 1.0f, 32.0f, 0.0f, 1.0f, 2000.0f, 205.0f, 0.0f, 0.0f, 0.94f, 2000.0f, 16.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 106.0f, 268.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.92f, 2000.0f, 320.0f, 99.0f, 0.0f, 1.0f, 436.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 18.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Drill Shimmer */
		1.28f, 0.32f, 1.08f, 0.76f, 0.62f, 0.56f, 0.36f, 1.77f, 0.32f, -2.1f, 1.0f, 6.67f, 0.83f, 0.93f, -18.9f, 1.0f, 0.57f, 1.27f, 0.39f, 2.69f, 1.2f, 0.66f, 1.89f, 1.1f, 0.9f, 0.25f, 4.67f, 0.65f, -1.0f, 1.0f, 1.0f, 1.7f, 1.26f, 1.0f, 0.0f, 0.89f, 1.25f, 1.0f, 0.8f, 0.91f, 1.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 1.72f, 2.0f, 1191.0f, 6.8f, 766.0f, 0.34f, 6.0f, 0.43f, 0.52f, 0.27f, 6.0f, 0.28f, 0.21f, 1.0f, 745.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 426.0f, 17.0f, 219.0f, 0.48f, 1138.0f, 1228.0f, 127.0f, 1444.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 1580.0f, 1.0f, 2000.0f, 20.0f, 106.0f, 268.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.92f, 2000.0f, 320.0f, 99.0f, 0.0f, 1.0f, 436.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 18.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Soggy */
		1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.25f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.6f, 1.1f, 1.5f, 1.0f, 6.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.35f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Chip Vespers */
		1.2f, 0.35f, 1.0f, 0.6f, 0.65f, 0.7f, 0.35f, 1.2f, 0.7f, -2.5f, 1.0f, 2.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.5f, 0.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 17.0f, 220.0f, 0.5f, 1100.0f, 1200.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 1.0f, 0.0f, 2.0f, 1.0f, 0.0f, 0.0f, 1.3f, 0.5f, 1.0f, 1.0f, 1.0f, 1.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* String Drum Mass */
		1.5f, 0.6f, 1.0f, 0.8f, 0.65f, 0.7f, 0.35f, 1.5f, 0.7f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 5.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 2.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.25f, 2.5f, 1.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Rungler Rain */
		1.5f, 0.35f, 1.0f, 0.8f, 0.65f, 1.4f, 0.35f, 1.2f, 0.7f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 3.0f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 7.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 2.0f, 1.0f, 0.75f, 0.25f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 17.0f, 220.0f, 0.5f, 1100.0f, 1200.0f, 180.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.7f, 0.5f, 1.0f, 1.0f, 2.2f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Bead Machine */
		1.5f, 0.35f, 1.0f, 0.8f, 1.6f, 0.7f, 0.35f, 1.2f, 0.7f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.5f, 2.0f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 4.0f, 600.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.7f, 1.0f, 2.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* 1-bit Funeral */
		1.8f, 0.35f, 1.0f, 0.8f, 0.65f, 0.7f, 0.35f, 1.2f, 0.7f, -5.0f, 1.0f, 4.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 6.0f, 300.0f, 4.5f, 30.0f, 0.6f, 1.0f, 2.5f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.45f, 1.0f, 1.0f, 1.5f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Rule of Three */
		1.5f, 0.35f, 1.0f, 0.8f, 0.65f, 0.7f, 0.35f, 1.2f, 0.7f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.6f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 3.0f, 1.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.62f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 17.0f, 220.0f, 0.5f, 1100.0f, 1200.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 1.0f, 0.8f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Lich Canvas */
		1.5f, 0.35f, 1.0f, 0.8f, 0.65f, 0.7f, 0.35f, 1.2f, 0.7f, -2.5f, 1.0f, 3.0f, 1.0f, 0.9f, -15.0f, 1.0f, 1.0f, 1.1f, 0.6f, 2.5f, 2.0f, 1.0f, 1.5f, 1.0f, 0.9f, 0.25f, 2.5f, 0.8f, 0.0f, 1.0f, 1.0f, 1.5f, 1.5f, 2.0f, 0.0f, 0.9f, 1.3f, 1.3f, 1.2f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 2.0f, 0.0f, 250.0f, 4.5f, 30.0f, 0.6f, 1.0f, 1.6f, 0.3f, 0.22f, 1.0f, 1.0f, 0.16f, 0.35f, 250.0f, 0.33f, 1.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 17.0f, 220.0f, 0.5f, 1100.0f, 1200.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 150.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 100.0f, 0.0f, 1.0f, 440.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 2.64f, 0.45f, 2.51f, 1.27f, 3.0f, 2.78f, 0.7f, 0.0f, 0.8f, 0.0f, 1.0f, 1.0f, 0.15f, 1.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.4f, 12.0f, 0.75f, 0.0f, 1.0f, 0.0f, 8000.0f, 0.0f, 0.0f, 0.3f, 0.4f, 15.0f, 0.0f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
	{ /* Brush Ring */
		3.34f, 0.19f, 0.23f, 0.09f, 0.41f, 0.35f, 0.2f, 0.33f, 0.34f, -2.1f, 1.0f, 6.67f, 0.83f, 0.93f, -18.2f, 1.0f, 4.93f, 1.0f, 0.98f, 2.41f, 2.78f, 1.0f, 1.09f, 1.0f, 0.9f, 0.25f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.7f, 1.26f, 1.0f, 0.0f, 0.89f, 1.25f, 1.0f, 0.8f, 0.91f, 1.0f, 3.0f, 3.0f, 3.0f, 1.0f, 0.5f, 1.1f, 1.5f, 1.72f, 8.0f, 443.0f, 6.8f, 766.0f, 0.34f, 6.0f, 0.43f, 0.52f, 0.27f, 6.0f, 20.0f, 0.85f, 0.19f, 745.0f, 0.33f, 1.0f, 32.0f, 0.0f, 1.0f, 2000.0f, 205.0f, 0.0f, 0.0f, 0.94f, 2000.0f, 16.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 20.0f, 106.0f, 268.0f, 1.0f, 2000.0f, 20.0f, 0.0f, 0.0f, 0.92f, 2000.0f, 320.0f, 99.0f, 0.0f, 1.0f, 436.0f, 20.0f, 0.0f, 0.0f, 1.0f, 2000.0f, 18.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.6f, 1.0f, 0.0f, 0.0f, 0.0f, 1.2f, 1.0f, 1.0f, 1.0f, 0.2f, 12.0f, 0.75f, 0.7f, 0.6f, -12.0f, 4000.0f, 0.3f, 0.4f, 0.3f, 0.4f, 15.0f, 0.5f, 0.35f, 2.5f, 0.8f, 0.25f, 0.06f, 0.0f, 1.0f, 0.0f, 2.0f, 31127.0f},
};
/* where the macro takes the Kit channel at each tuning: activity, tone, length, register,
 * character (-1: busier, darker, longer, fuller than the piece has it) */
static const float KIT_TUNE_TO[KIT_NTUNE][5] = {{1.0f, 0.0f, 1.0f, 0.83f, 1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {1.0f, 0.0f, 1.0f, 0.83f, 1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {0.8f, 0.2f, 1.0f, 0.75f, 1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}, {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f}};
/* the Kit map's corners (tunings): (0,0) (1,0) (0,1) (1,1), and each one's level */
static const uint8_t KIT_MAP[4] = {0, 5, 13, 14};
static const float KIT_MAP_GAIN[4] = {1.0f, 2.184f, 1.12f, 0.869f};
/* which numbers the map blends (not the evolving, velocity and arc ones), which it switches */
static const uint8_t KT_MAPPED[KT_COUNT] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
/* @@END_KIT_TUNINGS */
static int kit_tune_cur, kit_tune_lock;          /* (KT starts at tuning 0's values) */
/* KT as the tuning has it (KTB); KT itself is that with the macro's morph on a few numbers */
static float KTB[KT_COUNT];
static uint8_t ktb_ok;
static void ktb_init(void)
{
	if (!ktb_ok) {
		for (int i = 0; i < KT_COUNT; i++)
			KTB[i] = KT[i];
		ktb_ok = 1;
	}
}
static void kit_tuning(int t)
{
	if (t < 0 || t >= KIT_NTUNE)
		t = 0;
	for (int i = 0; i < KT_COUNT; i++)
		KT[i] = KTB[i] = KIT_TUNE[t][i];
	ktb_ok = 1;
	kit_tune_cur = t;
}
/* the macro's morph (0 the tuning as it is, 1 its far end): the numbers that move with it, all
 * together with the Kit channel's controls (kit_macro): the kick, the hats, the clank and the Nord
 * ring longer, more ghosts and tom fills, the hats darker and swelling in */
/* and the weird windows (kit_step's evolve: w 0 restrained .. 1 fully weird, times EV_METAL): the hats'
 * shift and their ring wider, open hats, the roll crushed, the clank's FM up, the Nord's sweep, more
 * ghosts, the resonance up */
static const struct { uint8_t id; float mul, add, wmul, wadd; } KMORPH[] = {
	{KT_K_DECAY, 0.5f, 0.0f, 0.0f, 0.0f}, {KT_H_DECAY, 1.0f, 0.0f, 1.5f, 0.0f}, {KT_C_DECAY, 1.0f, 0.0f, 0.5f, 0.0f},
	{KT_N_DECAY, 0.6f, 0.0f, 0.0f, 0.0f}, {KT_S_GHOSTS, 1.0f, 0.0f, 1.5f, 0.0f}, {KT_TOM_FILL, 1.5f, 0.0f, 0.0f, 0.0f},
	{KT_H_BAND, -0.25f, 0.0f, 0.0f, 0.0f}, {KT_E_HAT_A, 0.0f, 120.0f, 0.0f, 0.0f}, {KT_H_SHIFT, 0.0f, 0.0f, 2.5f, 0.0f},
	{KT_H_OPEN, 0.0f, 0.0f, 0.0f, 0.5f}, {KT_R_CRUSH, 0.0f, 0.0f, 1.0f, 0.0f}, {KT_C_INDEX, 0.0f, 0.0f, 1.5f, 0.0f},
	{KT_N_SWEEP, 0.0f, 0.0f, 1.0f, 0.0f}, {KT_RES_MIX, 0.0f, 0.0f, 0.6f, 0.0f},
};
static void kit_morph(float m, float w)
{
	ktb_init();
	w *= KTB[KT_EV_METAL];
	for (unsigned i = 0; i < sizeof KMORPH / sizeof KMORPH[0]; i++)
		KT[KMORPH[i].id] = KTB[KMORPH[i].id] * (1.0f + KMORPH[i].mul * m) * (1.0f + KMORPH[i].wmul * w) +
		                   KMORPH[i].add * m + KMORPH[i].wadd * w;
}
/* a Lich model for this hit: switched on by the tuning, or now and then in a weird window */
static inline int kit_lich(const kit_state *k, int id) { return KT[id] >= 0.5f || ku() < k->weird * KT[KT_EV_LICH]; }
/* the velocity groove: the accents of each sixteenth, three ways */
static const float KVG[3][16] = {
	{1.0f, 0.55f, 0.7f, 0.6f, 0.9f, 0.55f, 0.7f, 0.6f, 0.95f, 0.55f, 0.7f, 0.6f, 0.9f, 0.55f, 0.7f, 0.65f},   /* the beat */
	{0.8f, 0.5f, 1.0f, 0.55f, 0.75f, 0.5f, 1.0f, 0.6f, 0.8f, 0.5f, 1.0f, 0.55f, 0.75f, 0.5f, 1.0f, 0.7f},    /* push */
	{1.0f, 0.4f, 0.6f, 0.85f, 0.7f, 0.4f, 0.6f, 0.9f, 0.95f, 0.4f, 0.6f, 0.85f, 0.7f, 0.45f, 0.65f, 1.0f},  /* lilt */
};
static uint8_t kit_from_midi;                     /* (a MIDI hit keeps its own velocity) */

/* ---- the director: keeps the Kit alive over a piece without losing the thread. Each section (8 bars)
 * reads an intensity off the piece's arc (its order turned differently each piece); each phrase (4 bars)
 * builds a spice from nothing to its peak at the fourth bar (kit_step): a glide on the Kit map away from
 * the piece's home near the trad kit (four corners: Glag, Drill Shimmer, Lich Canvas, Brush Ring), the
 * weird windows, throws into the echo, the band pumping on the kick or swelling against it, the
 * euclidean rings gaining or losing hits; a dropout and a rotation late in the phrase ---- */
static float kit_pump, kit_pump_t;                /* read by the pump (glag_play.h): -1 anti .. 1 pumping */
static uint8_t kit_map_reset;
/* the colour's pitch, beyond its tuning: a bar's octave move and a hit's interval (semitones) */
static float kit_boct, kit_iv;
static uint8_t kit_fill_t, kit_hat_mute, kit_nord_pat;   /* (the phrase's fill; the hats held for a hat fill's bar) */
static uint32_t kit_r2 = 0x9e3779b9u;              /* (their own dice: the rest of the kit's stay as they were) */
static inline float kr2(void) { kit_r2 = kit_r2 * 1664525u + 1013904223u; return (float)(kit_r2 >> 8) * (1.0f / 16777216.0f); }
static uint8_t kit_vthr_done, kit_ivmode;           /* (a phrase's intervals: 0 none, 1 the 3rd and 5th, 2 the set) */
static uint8_t kit_dir_force;                     /* the tuning page: let the map drive all the same */
void gl_kit_director(int on) { kit_dir_force = (uint8_t)(on != 0); }
static struct {
	float mx, my, tx, ty, bx, nov, throw_p, thr_base, pump_sel, pk, env, S;
	int32_t sec;
	uint32_t last;
	uint8_t drop_bar, drop_mask, bar, seeded, turn, shape, plain;
	int8_t em[3];
} KD;
static const float KARC[8] = {0.25f, 0.45f, 0.7f, 1.0f, 0.5f, 0.85f, 0.35f, 0.6f};
/* the map: the numbers blended between the four corners (the switches from the nearest), into KTB */
static void kit_map(float x, float y)
{
	const float w[4] = {(1.0f - x) * (1.0f - y), x * (1.0f - y), (1.0f - x) * y, x * y};
	int near = 0;
	for (int j = 1; j < 4; j++)
		near = w[j] > w[near] ? j : near;
	for (int i = 0; i < KT_COUNT; i++) {
		if (!KT_MAPPED[i])
			continue;
		if (KT_MAPPED[i] == 2)
			KTB[i] = KIT_TUNE[KIT_MAP[near]][i];
		else
			KTB[i] = w[0] * KIT_TUNE[KIT_MAP[0]][i] + w[1] * KIT_TUNE[KIT_MAP[1]][i] + w[2] * KIT_TUNE[KIT_MAP[2]][i] + w[3] * KIT_TUNE[KIT_MAP[3]][i];
	}
	KTB[KT_OUT] *= w[0] * KIT_MAP_GAIN[0] + w[1] * KIT_MAP_GAIN[1] + w[2] * KIT_MAP_GAIN[2] + w[3] * KIT_MAP_GAIN[3];
	/* into KT too: the morph (kit_morph, every block) redoes its own few from KTB, the rest are these */
	for (int i = 0; i < KT_COUNT; i++)
		if (KT_MAPPED[i])
			KT[i] = KTB[i];
	KT[KT_OUT] = KTB[KT_OUT];
}
/* the macro on a Kit channel (from macro_tick): one movement, not several. Its glide (u, -1..1) at
 * its depth d gives the morph m; the channel's activity, tone, length, register and character move
 * together toward the tuning's far end (KIT_TUNE_TO), and the numbers above with them */
static void kit_macro(int c, float d, float u)
{
	/* (most of it waits for the phrase's build: SPICE_MAC of it rides the director's spice) */
	const float sp = KTB[KT_SPICE_MAC];
	const float m = d * (0.5f + 0.5f * u) * (1.0f - sp + sp * KD.env);
	const float *to = KIT_TUNE_TO[kit_tune_cur];
	static const uint8_t CPS[5] = {CP_ACTIVITY, CP_TONE, CP_LENGTH, CP_REGISTER, CP_CHAR};
	static const float REL[5] = {0.35f, -0.35f, 0.45f, 0.0f, 0.5f};
	for (int i = 0; i < 5; i++) {
		const float b = Pb.ch[c][CPS[i]], t = to[i] >= 0.0f ? to[i] : clampf(b + REL[i], 0.0f, 1.0f);
		P.ch[c][CPS[i]] = b + (t - b) * m;
	}
	CH[c].kt.morph = m;
}
int gl_kit_ntunings(void) { return KIT_NTUNE; }
const char *gl_kit_tuning_name(int t) { return KIT_TUNE_NAME[(unsigned)t < KIT_NTUNE ? t : 0]; }
void gl_kit_tuning_lock(int on) { kit_tune_lock = on; }
void gl_kit_tune(int id, float v) { if ((unsigned)id < KT_COUNT) { ktb_init(); KTB[id] = KT[id] = v; } }
int gl_kit_tunes(void) { return KT_COUNT; }

/* audition: only these lanes sound (and each one's own pattern runs whatever CHARACTER is) */
static uint32_t kit_solo;
void gl_kit_solo(int lanes) { kit_solo = (uint32_t)lanes; }
#define KSOLO(l) (kit_solo & (1u << (l)))

static int kit_rot_always;
void gl_kit_rotate(int always) { kit_rot_always = always; }
/* the rotation: the kick's hits played by the snare, the snare's by the hat, the hat's by the
 * kick (turned once, or twice) */
static int kit_rot(int lane, int r)
{
	for (; r > 0; r--)
		lane = lane == 0 ? 1 : lane == 1 || lane == 9 ? 2 : lane == 7 ? 2 : lane == 2 || lane == 6 ? 0 : lane;
	return lane;
}

/* ---- a hit on a lane, its sound set from the channel's controls ---- */
static void kit_fire(chan *c, int ch, int lane, float vel)
{
	kit_state *k = &c->kt;
	int nord_down = 0;
	if (lane == 20 || lane == 21)                   /* (a fill's Nord: 21 the first hit, 20 the second) */
		nord_down = lane == 20 ? 1 : -1, lane = 5;
	/* song mode peeling the Kit off: the extras first, then the snare, the hats, the kick last */
	if (!kit_from_midi && w_peel[ch] < 0.999f) {
		const float pl = w_peel[ch];
		const int keep = lane == 0 ? pl > 0.03f : lane == 2 ? pl > 0.4f : (lane == 1 || lane == 7) ? pl > 0.62f : pl > 0.84f;
		if (!keep)
			return;
	}
	const int fillhat = lane == 22;
	if (fillhat)
		lane = 2;
	else if (kit_hat_mute && !kit_from_midi && (lane == 2 || lane == 6))
		return;                                     /* (a hat fill's bar: the groove's hats held) */
	if (k->rot_r && k->rot_r < 4)                 /* (4: armed, not turning yet) */
		lane = kit_rot(lane, k->rot_r);
	if (kit_solo && !KSOLO(lane))
		return;
	/* the director's dropout bar: the kick, the hats or all but the hats left out */
	if (!kit_from_midi && KD.drop_mask && KD.bar == KD.drop_bar) {
		const unsigned bit = lane == 7 || lane == 9 ? 2u : 1u << lane;
		if (KD.drop_mask & bit)
			return;
	}
	/* the colour's interval: now and then a hit (not the kick or a closed hat) moves the colour up the
	 * chord's 3rd, 5th, 7th or 9th; the next such hit brings it home */
	int ivh = 0;
	if (lane != 0 && lane != 2 && lane != 7) {
		kit_iv = 0.0f;
		const int set = kit_ivmode == 1 ? 3 : kit_ivmode == 2 ? (int)(KT[KT_IV_SET] + 0.5f) : 0;
		if (set && KT[KT_IV_HITS] > 0.0f && kr2() < KT[KT_IV_HITS]) {
			int pick;
			do pick = (int)(kr2() * 4.0f); while (!(set >> pick & 1));
			const int third = (chord_pc[1] - chord_pc[0] + 12) % 12;
			static const int8_t IV[4] = {0, 7, 0, 14};
			kit_iv = (float)(pick == 0 ? third : pick == 2 ? (third == 3 ? 10 : 11) : IV[pick]);
			ivh = 1;
		}
	}
	/* a delay throw: now and then an accent, a clank, a Nord, an open hat, a roll or a tom goes into the
	 * echo (likelier on a hit that moved the colour: a tuned drum's echo) */
	if (!kit_from_midi && ((lane == 3 || lane == 5 || lane == 6 || lane == 8 || lane == 9 || (lane == 1 && vel > 0.9f)) ?
	        ku() < KD.throw_p * (ivh ? 1.5f : 1.0f) : lane >= 10 && KD.throw_p > 0.0f && kr2() < KD.throw_p * 1.5f)) {
		P.ch[ch][CP_ECHO] = clampf(Pb.ch[ch][CP_ECHO] + 0.5f, 0.0f, 1.0f);
		k->thr = 3;
	}
	/* a reverb throw: bar 2's last beat, one hit (not the kick) into the reverb */
	if (!kit_from_midi && lane != 0 && !kit_vthr_done && (KD.last & 63u) >= 28u && (KD.last & 63u) < 32u &&
	    KTB[KT_ARC_AMT] > 0.0f) {
		kit_vthr_done = 1;
		if (kr2() < KTB[KT_VTHROW]) {
			P.ch[ch][CP_VERB] = clampf(Pb.ch[ch][CP_VERB] + 0.6f, 0.0f, 1.0f);
			k->vthr = 2;
		}
	}
	/* the velocity groove and scatter (the sequencer's hits; the groove deeper in a weird window) */
	if (!kit_from_midi && vel < 0.99f) {
		const int g = (int)(KT[KT_VEL_GROOVE] + 0.5f);
		if (g >= 1 && g <= 3) {
			const float d = clampf(KT[KT_VEL_GDEPTH] + 0.3f * k->weird, 0.0f, 1.0f);
			vel *= 1.0f - d + d * KVG[g - 1][k->pos16 & 15];
		}
		vel = clampf(vel * (1.0f + KT[KT_VEL_RAND] * 0.6f * kn()), 0.05f, 0.98f);
	}
	/* the macro's dynamics: the deeper it goes, the more each hit's velocity spreads into its level,
	 * its brightness and its length (hard hits louder, brighter, longer; soft ones duller, shorter) */
	const float dyn = mac_depth[ch] * KT[KT_F_DYN], dv = vel - 0.75f;
	const float t = clampf(eff_tone(ch) + dyn * 1.6f * dv, 0.0f, 1.0f), l = P.ch[ch][CP_LENGTH], x = P.ch[ch][CP_CHAR];
	const float dyn_d = clampf(1.0f + dyn * 3.0f * dv, 0.35f, 2.0f);
	if (vel < 0.99f)
		vel = clampf(vel * (1.0f + dyn * 1.8f * dv), 0.05f, 0.98f);
	const float reg = (float)(stepped(P.ch[ch][CP_REGISTER], 5) - 2), oct = fexp2(reg * 0.5f);
	notes_fired++;
	c->glow = vel;
	/* short by default; an accent (velocity 1: the phrase's downbeat, the odd tissue stroke, a
	 * hard MIDI hit) rings long */
	const int big = vel >= 0.99f;
	/* the rule of threes: every third kick pressed (a hand pushing into the head: its pitch
	 * rising, damped, muffled), every third snare left to ring (its soggy filter closing slowly),
	 * every third hat leaned on; and now and then a pressed hit anywhere */
	int press = 0, lng = big;
	if (lane == 0) {
		k->n_k++;
		press = (KT[KT_F_PRESS] >= 1.0f && k->n_k % (unsigned)KT[KT_F_PRESS] == 0u) || ku() < 0.1f;
	} else if (lane == 1) {
		k->n_s++;
		lng |= KT[KT_F_LONG] >= 1.0f && k->n_s % (unsigned)KT[KT_F_LONG] == 0u;
		press = !lng && ku() < 0.12f;
	} else if (lane == 2) {
		k->n_h++;
		if (KT[KT_F_HATACC] >= 1.0f && k->n_h % (unsigned)KT[KT_F_HATACC] == 0u && vel < 0.95f)
			vel = 0.95f;
	}
	{	/* this sound's envelope starts again */
		static const int8_t ES[14] = {0, 1, 2, 3, 4, 5, 2, 1, 6, 1, 7, 7, 7, 7};
		if ((unsigned)lane < 14u) {
			k->e_st[ES[lane]] = 1;
			k->e_t[ES[lane]] = 0.0f;
		}
	}
	if (lane == 0 || lane == 1 || lane == 9) {
		k->c_relk = press ? 0.005f : lng ? 0.00035f : 0.042f / (KT[KT_SOG_CLOSE] < 2.0f ? 2.0f : KT[KT_SOG_CLOSE]);   /* the soggy filter: shut in 10, 120 or 30 ms */
		k->c_ceil = KT[KT_SOG_REACH] * (press ? 0.667f : lng ? 1.067f : 1.0f);             /* how far it opens */
	}
	const float dm = (big ? KT[KT_F_ACCX] : lng ? KT[KT_F_LONGX] : KT[KT_F_SHORT]) * dyn_d;
	const float root = (float)chord_pc[0];              /* the clank plays the chord's root */
	switch (lane) {
	case 0: {   /* canvas kick: tension (REGISTER), damping (LENGTH), firmness (TONE), slack (CHARACTER) */
		/* tuned: the chord's root, the one nearest D2 (73 Hz) */
		float f0 = clampf((KT[KT_K_TUNED] >= 0.5f ? kit_hz(38 + ((chord_pc[0] - 38 + 66) % 12) - 6) : 73.4f) * oct * fexp2(KT[KT_K_PITCH] * (1.0f / 12.0f)), 30.0f, 200.0f) * (1.0f + 0.004f * kn());
		float damp = 0.8f - 0.6f * l, firm = 0.2f + 0.6f * t, slack = 0.1f + 0.7f * x;
		float tau1 = (0.7f - damp * 0.6f) * (big ? 1.3f : 0.8f) * (press ? 0.55f : 1.0f) * KT[KT_K_DECAY];
		/* fitted to the gap since the last kick: four on the floor rings short, a lone kick booms */
		if (k->k_since > 0.05f && k->k_since < 2.0f && KT[KT_K_FIT] > 0.0f)
			tau1 = 1.0f / (1.0f / tau1 + KT[KT_K_FIT] / k->k_since);
		k->k_since = 0.0f;
		if (press)
			f0 *= 1.15f;
		for (int m = 0; m < 5; m++) {
			k->k_e[m] = f0 * (1.0f + (K_MEM[m] - 1.0f) * KT[KT_K_SPREAD]) * (6.2832f / KSR);
			if (k->k_x[m] * k->k_x[m] + k->k_y[m] * k->k_y[m] < 0.5f)
				k->k_x[m] = 0.0f, k->k_y[m] = 1.0f;
			k->k_amp[m] = vel * KT[KT_L_KICK] * (m ? KT[KT_K_MODES] : 1.0f) * K_MEMA[m] * fexp2(-(float)m * (1.0f - firm) * 0.72f) * (1.0f + 0.3f * kn());
			k->k_dec[m] = kexp(tau1 / fexp2(0.8f * flog2(K_MEM[m])));
		}
		k->k_glide = press ? -0.18f : slack * 0.25f * KT[KT_K_SLACK];   /* (pressed: from below, rising) */
		k->k_gk = fexp2(-1.4427f / (0.03f * 1000.0f));    /* (once a millisecond) */
		k->k_att = 0.0f;
		k->k_ai = 1.0f / ((0.008f - firm * 0.007f) * KSR);
		k->k_nz = vel * (press ? 0.35f : 0.75f) * KT[KT_K_CLICK] * KT[KT_L_KICK];
		k->k_nzk = kexp(0.014f - firm * 0.011f);
		k->k_lpk = clampf((1500.0f + firm * 4000.0f) * (6.2832f / KSR), 0.05f, 0.9f);
		pump_tgt = 1.0f;                                   /* Playworld's pump follows the kick */
		/* the Lich: a 4-bit triangle (the NES's), its pitch falling from far above onto the kick's */
		k->k_lich = KT[KT_LICH_KICK] >= 0.5f;   /* (the kick stays itself in a weird window: the anchor) */
		if (k->k_lich) {
			k->kl_f0 = f0;
			k->kl_fall = 5.0f * KT[KT_LICH_FALL];
		}
		break;
	}
	case 1:     /* R-8 snare: pitch, decay, nuance, feel; crunch rising with CHARACTER */
	case 7:     /* (7: a ghost, quieter and shorter) */
	case 9: {   /* (9: squashed: an accent pitched down 4 to 7 semitones, driven hard) */
		float feel = 2.0f + 4.0f * x;
		/* tuned: its body on the chord's third or fifth, the one nearest F#3 (185 Hz) */
		int tn = chord_pc[ku() < 0.5f ? 1 : 2];
		float semis = (KT[KT_S_TUNED] >= 0.5f ? (float)(54 + ((tn - 54 + 66) % 12) - 6 - 54) : 0.0f) + KT[KT_S_PITCH] + reg * 12.0f * 0.25f + kn() * feel * 0.02f;
		float dec = clampf((0.1f + 0.6f * l) * dm * KT[KT_S_DECAY] * (1.0f + kn() * feel * 0.03f), 0.0f, 4.0f);
		float nu = clampf(4.0f + 11.0f * t + kn() * feel * 0.4f, 0.0f, 15.0f);
		if (lane == 9)
			kit_snare_hit(&k->sn, semis - 4.0f - 3.0f * ku(), clampf(dec * 1.4f, 0.0f, 1.0f), nu * 0.6f, clampf(KT[KT_S_CRUNCH], 0.0f, 2.0f), vel);
		else
			kit_snare_hit(&k->sn, semis, lane == 7 ? dec * 0.4f : press ? dec * 0.5f : dec, nu, 0.8f * x * KT[KT_S_CRUNCH], lane == 7 ? vel * 0.35f : vel);
		k->sn.nl *= KT[KT_S_NOISE];
		/* the Lich: a Karplus-Strong drum in its place (a burst of noise round a delay line a period
		 * of the snare's pitch long, its sign flipped at random: none a tom, half the time a snare) */
		if (kit_lich(k, KT_LICH_SNARE)) {
			k->sn.b = k->sn.n = 0.0f;
			int16_t *kb = c->ks + KSD_OFF;
			const float f = 185.0f * fexp2(semis * (1.0f / 12.0f)) * (lane == 9 ? 0.75f : 1.0f);
			uint16_t n = (uint16_t)clampf(KSR / f, 8.0f, (float)KSD_MAX);
			const float a = (lane == 7 ? 0.35f : 1.0f) * vel * 9000.0f;
			for (uint16_t i = 0; i < n; i++)
				kb[i] = (int16_t)(kn() * a);
			k->ksd_n = n;
			k->ksd_i = 0;
			k->ksd_on = 1;
			k->ksd_env = 1.0f;
			k->ksd_rough = KT[KT_LICH_ROUGH];
			/* the loop's loss: the ring (LENGTH, the decay, the stretch) as time to -40 dB */
			const float ring = clampf((0.05f + 0.4f * dec) * KT[KT_LICH_STRETCH] * (lane == 7 ? 0.4f : 1.0f), 0.01f, 6.0f);
			k->ksd_g = fexp2(-6.644f * (float)n / (ring * KSR));
		}
		if (press && lane == 1) {                         /* pressed: rising from below, muffled */
			k->sn.drop = -0.2f;
			k->sn.dk = fexp2(-1.4427f / 25.0f);
			k->sn.n *= 0.5f;
		}
		break;
	}
	case 2:     /* metal hat: decay (LENGTH; long when open), shift at the hit and at the end, tone */
	case 6: {   /* (6: open) */
		/* (CHARACTER reaches only half the shift it had: past that it turned sour) */
		const float xs = x * 0.5f, open = lane == 6 ? 1.0f : 0.0f;
		float dec = expmap(l, 0.025f, 0.3f) * dm * KT[KT_H_DECAY] * (lane == 6 ? 3.0f : 1.0f);
		dec = dec > 1.2f * KT[KT_H_DECAY] ? 1.2f * KT[KT_H_DECAY] : dec;   /* (the cap moves with the tuning) */
		/* the longer it rings, the darker and quieter: a long hat sits back, muted */
		float dark = clampf((dec - 0.05f) * 5.0f * KT[KT_H_DARK], 0.0f, 1.0f);
		k->h_a = vel * (1.0f - 0.35f * dark) * KT[KT_L_HAT];
		k->h_k = kexp(dec);
		/* every hit its own: the squares' phases scattered, the band placed anew and swept down
		 * from above it in the first 15 ms (as a struck cymbal's top falls away), the shift's start
		 * moved a little */
		for (int m = 0; m < 6; m++)
			k->h_ph[m] += (uint32_t)(int32_t)(kn() * 2147483647.0f);
		k->h_fc0 = (4000.0f + 6000.0f * t) * (0.8f + 0.45f * ku()) * KT[KT_H_BAND];
		k->h_fenv = 0.5f + 0.7f * ku();
		k->h_s0 = (60.0f + 240.0f * xs) * (0.8f + 0.4f * ku()) * KT[KT_H_SHIFT];
		k->h_s1 = (-60.0f * xs + 20.0f * kn()) * KT[KT_H_SHIFT];
		if (dark > 0.0f) {
			/* a long hat: bright at first, then darker; its shift slides onto a note of the key
			 * (between 65 and 123 Hz, up or down) */
			int d = (int)(ku() * 7.0f);
			float hz = kit_hz(36 + root_pc + kit_deg(d) % 12);
			k->h_s1 = (kn() < 0.0f ? -hz : hz) * (0.5f + 0.5f * dark);
		}
		k->h_t = 0.0f;
		k->h_len = dec;
		k->h_f = 1.0f - 0.45f * dark;                    /* (the band's place at the end, a fraction) */
		k->h_grit = (0.2f + 0.5f * xs) * (1.0f - 0.6f * open);
		k->h_lpk = 1.0f;                                  /* the low-pass after the shift: open at first */
		k->h_lpe = 1.0f - 0.75f * dark;                   /* and where it closes to */
		/* the Lich: the NES's noise register (15 bits, or its short 7-bit loop: metallic), clocked
		 * higher with TONE */
		k->h_lich = (uint8_t)(KT[KT_LICH_HAT] + 0.5f);
		if (!k->h_lich && ku() < k->weird * KT[KT_EV_LICH])
			k->h_lich = 1;                                /* (weird: the 15-bit noise) */
		if (k->h_lich) {
			if (!k->lf_reg)
				k->lf_reg = 1;
			k->lf_per = KSR / ((1500.0f + 9000.0f * t) * KT[KT_LICH_RATE] * (lane == 6 ? 0.7f : 1.0f));
			if (k->lf_per < 1.0f)
				k->lf_per = 1.0f;
		}
		break;
	}
	case 3: {   /* DX100 clank: ratio (CHARACTER), carrier (REGISTER), index (TONE), decay (LENGTH) */
		float fc = 440.0f * fexp2((36.0f + 12.0f * KT[KT_C_OCT] + root - 69.0f) / 12.0f) * oct * (1.0f + 0.002f * kn());   /* C2-B2 */
		k->d_ic = kinc(fc);
		k->d_im = kinc(fc * K_RAT[stepped(x, 4)]);
		k->d_index = (1.5f + 5.0f * t) * IDX_SCALE * KT[KT_C_INDEX];
		k->d_ik = kexp((0.05f + 0.3f * l) * dm * 0.35f * KT[KT_C_DECAY]);
		k->d_a = vel * KT[KT_L_CLANK];
		k->d_k = kexp((0.06f + 0.35f * l) * dm * KT[KT_C_DECAY]);
		k->d_fb = (0.05f + 0.25f * x) * 3.1416f;
		k->d_grit = x > 0.5f;
		k->d_lich = (uint8_t)kit_lich(k, KT_LICH_CLANK);              /* the Lich: the two as 1-bit pulses, XORed */
		break;
	}
	case 4: {   /* tissue: stroke length (LENGTH), crackle density (CHARACTER), brightness (TONE), rumble */
		k->t_len = (big ? 0.25f + 0.6f * l : 0.04f + 0.12f * l) * KT[KT_T_LEN];   /* (held over MIDI: until let go) */
		k->t_t = 0.0f;
		k->t_a = vel * KT[KT_L_TISSUE];
		k->t_prob = (300.0f + 1200.0f * x) * KT[KT_T_DENS] / KSR;
		k->t_fc = 1500.0f + 5500.0f * t;
		k->t_prox = 0.2f + 0.4f * x;
		k->t_w0 = 2.0f + 3.0f * ku();
		k->t_w1 = 0.7f + ku();
		k->t_ph0 = ku();
		/* the Lich: a PhISEM shaker (beads in a gourd: the shake's energy decays, collisions come
		 * as often as it allows, each rings the gourd's band) */
		k->t_lich = KT[KT_LICH_TISSUE] >= 0.5f;
		if (k->t_lich) {
			k->sh_e = vel;
			k->sh_p = 64.0f * KT[KT_LICH_BEADS] * (1.0f / KSR) * 40.0f;
		}
		break;
	}
	case 5: {   /* Nord sync hit: pitch (REGISTER), sync sweep and FM (CHARACTER), cutoff (TONE), decay (LENGTH) */
		/* a note of the key in the C2 octave: the chord's root or fifth most often, any degree now
		 * and then */
		{
			float u = ku();
			int d = u < 0.45f ? 0 : u < 0.7f ? 4 : u < 0.85f ? 2 : (int)(ku() * 7.0f);
			int at = 0;
			for (int i = 0; i < scale_n; i++)
				if (scale_pc[i] == chord_pc[0])
					at = i;
			/* the C1 octave (33-62 Hz), now and then the C2 */
			const int nb = (ku() < 0.3f ? 36 : 24) + 12 * (int)KT[KT_N_OCT] + root_pc;
			k->n_f1 = kit_hz(nb + kit_deg(at + d)) * oct;
			float mul = oct;
			if (k->n_f1 > 125.0f * oct * fexp2(KT[KT_N_OCT]))
				k->n_f1 *= 0.5f, mul *= 0.5f;
			/* a fill's two Nord hits, the same note: at its place then an octave down, or (every
			 * other time) the other way round */
			static float last;
			if (nord_down > 0)
				k->n_f1 = last * (kit_nord_pat ? 0.5f : 1.0f);
			else if (nord_down < 0) {
				last = k->n_f1;
				k->n_f1 *= kit_nord_pat ? 1.0f : 0.5f;
			}
			(void)mul;
		}
		k->n_sweep = (12.0f + 24.0f * x) * (1.0f + kn() * (0.1f + 0.2f * x) * 0.35f) * KT[KT_N_SWEEP];
		k->n_fm = (0.08f + 0.25f * x) * (1.0f + kn() * 0.1f);
		k->n_md = fexp2(-1.4427f / (0.16f * 1000.0f));   /* a slow sweep, 160 ms (once a millisecond) */
		k->n_menv = 1.0f;
		k->n_cut = 350.0f * fexp2(3.0f * t) * KT[KT_N_CUT];
		k->n_a = vel;
		k->n_ak = kexp((0.4f + 1.0f * l) * dm * KT[KT_N_DECAY] * (nord_down ? KTB[KT_NORD_FILLX] : 1.0f));   /* (a fill's Nord rings longer) */
		k->n_att = 0.0f;
		k->n_p1 = k->n_p2 = 0.0f;
		/* the Lich: a rungler (two oscillators: B clocks an 8-bit register that A's state feeds; the
		 * register's low three bits bend both); a few octaves above the Nord's note */
		k->n_lich = (uint8_t)kit_lich(k, KT_LICH_NORD);
		if (k->n_lich) {
			k->rg_fa = k->n_f1 * 4.0f;
			k->rg_fb = k->n_f1 * 4.0f * 1.37f;
			k->rg_depth = KT[KT_LICH_RUNG];
			if (!k->rg_reg)
				k->rg_reg = (uint8_t)(1u + (kit_r >> 24));
		}
		break;
	}
	case 10:    /* toms: the roll's snare tuned to the chord, its body ringing, little noise: */
	case 11:    /* 10 the root, 11 the third, 12 the fifth (G2 to F#3), 13 the root an octave up */
	case 12:
	case 13: {
		if (k->r_i < k->r_n)
			break;                                        /* (a roll is playing) */
		int tn = chord_pc[lane == 13 ? 0 : lane - 10];
		float semis = (float)(43 + ((tn - 43 + 48) % 12) - 54) + (lane == 13 ? 12.0f : 0.0f);
		kit_snare_hit(&k->rs, semis, clampf(0.6f * dm, 0.2f, 1.0f), 1.0f, 0.0f, vel);
		k->rs.n *= 0.1f;
		k->rs.drop = 0.25f;                               /* (a tom's bend) */
		k->rs.bk = kexp((0.25f + 0.4f * l) * KT[KT_TOM_DECAY]);
		k->r_tom = 1;
		k->r_hold = 1.0f;
		k->r_q = 32768.0f;                                /* (the sampler out of the way) */
		break;
	}
	case 8: {   /* EPS crunch roll: retriggers, acceleration, pitch step; rate and bits from CHARACTER */
		float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		k->r_n = (uint8_t)clampf((4.0f + 8.0f * x) * KT[KT_R_HITS], 2.0f, 40.0f);
		k->r_tom = 0;
		k->r_i = 0;
		k->r_gap = step_s * 0.5f / KT[KT_R_SPEED];
		k->r_accel = 0.3f + 0.5f * x;
		k->r_t = 0.0f;
		k->r_vel = vel;
		k->r_hold = clampf(KSR / (14000.0f - 10000.0f * clampf(x * KT[KT_R_CRUSH], 0.0f, 1.3f)), 1.0f, 4.0f);
		k->r_q = fexp2(12.0f - 6.0f * clampf(x * KT[KT_R_CRUSH], 0.0f, 1.6f));
		/* tuned: the snare's body on the key's root (near 185 Hz), each hit a step of the scale */
		k->r_semis = (float)(((root_pc - 6 + 18) % 12) - 6);
		/* four kinds: a fall (as it was), a build that swells into the beat (a rush in reverse),
		 * one split by a pause, a harp's glissando of ringing tuned hits */
		{
			int m = alt_m[GV_KIT];
			if (m < 1 || m > 4) {
				float u = ku();
				m = u < 0.35f ? 1 : u < 0.6f ? 2 : u < 0.85f ? 3 : 4;
			}
			k->r_mode = (uint8_t)(m - 1);
		}
		k->r_dir = (int8_t)(k->r_mode == 1 || k->r_mode == 3 ? 1 : ku() < 0.6f ? 1 : -1);
		k->r_split = (uint8_t)(k->r_mode == 2 ? k->r_n / 2 : 255);
		if (ku() < 0.25f || alt_m[GV_KIT] >= 5) {         /* now and then at half speed */
			k->r_gap *= 2.0f;
			k->r_n = (uint8_t)(k->r_n > 6 ? k->r_n * 2 / 3 : k->r_n);
		}
		if (k->r_mode == 1)
			k->r_accel = 0.7f;
		if (k->r_mode == 3) {
			k->r_gap = step_s * 0.4f;
			k->r_accel = 0.2f;
		}
		break;
	}
	default:
		break;
	}
}

/* MIDI: a tissue stroke lasts as long as its key is held */
static void kit_hold(chan *c, int lane, int on)
{
	if (lane == 4)
		c->kt.t_len = on ? 4.0f : (c->kt.t_t < c->kt.t_len ? c->kt.t_t : c->kt.t_len);
}

/* a hit a moment from now (off the grid: the odd slots, swing, triplets) */
static void kit_later(chan *c, int ch, int lane, float vel, float t_s)
{
	if (t_s <= 0.0005f) {
		kit_fire(c, ch, lane, vel);
		return;
	}
	kit_state *k = &c->kt;
	for (int i = 0; i < 16; i++)
		if (k->q_t[i] <= 0.0f) {
			k->q_t[i] = t_s;
			k->q_v[i] = vel;
			k->q_l[i] = (uint8_t)lane;
			return;
		}
}
static void kit_trip(chan *c, int ch, int n, float step_s, float vel);
/* a phrase's closing fill (its last beat), one of sixteen: FILL_TYPE, or any of FILL_SET */
static int kit_fill_pick(int ch)
{
	int t = (int)(KTB[KT_FILL_TYPE] + 0.5f);
	if (t < 1 || t > 18) {
		uint32_t set = (uint32_t)(KTB[KT_FILL_SET] + 0.5f) & 0x3ffffu;
		if (GL_CHX_GRV(P.chx[ch]) == 5)
			set &= ~0x37c04u;                       /* (the house groove: no triplets, 3 and 11 to 18) */
		if (!set)
			return 0;
		do t = 1 + (int)(kr2() * 18.0f); while (!(set >> (t - 1) & 1u));
	}
	return t;
}
static void kit_fill(chan *c, int ch, int t, float s)
{
	switch (t) {
	case 1:     /* toms down the chord: the root up high, the fifth, the third, the root */
		for (int i = 0; i < 4; i++)
			kit_later(c, ch, 13 - i, 0.85f - 0.05f * (float)i, (float)i * s);
		break;
	case 2:     /* toms up the chord */
		for (int i = 0; i < 4; i++)
			kit_later(c, ch, 10 + i, 0.7f + 0.06f * (float)i, (float)i * s);
		break;
	case 3:     /* the low tom in triplets, the high one to land (a fifth under the others) */
		for (int i = 0; i < 3; i++)
			kit_later(c, ch, 10, 0.6f + 0.04f * (float)i, (float)i * s * 1.3333f);
		kit_later(c, ch, 13, 0.48f, s * 3.6667f);
		break;
	case 11:    /* the fourth bar's hats alone: slow, in quarter-note triplets (the groove's hats held) */
		for (int i = 0; i < 6; i++)
			kit_later(c, ch, 22, (i & 1) ? 0.62f : 0.75f, (float)i * s * 2.6667f);
		break;
	case 12:    /* the snare in triplets, rising */
	case 13:    /* the clank in triplets */
	case 14:    /* tissue strokes in triplets */
		{
			static const uint8_t TLN[3] = {1, 3, 4};
			for (int i = 0; i < 3; i++)
				kit_later(c, ch, TLN[t - 12], 0.55f + 0.12f * (float)i, (float)i * s * 1.3333f);
		}
		break;
	case 18: {  /* the Nord in triplets */
		static const uint8_t TLN[7] = {1, 3, 4, 0, 0, 0, 5};
		for (int i = 0; i < 3; i++)
			kit_later(c, ch, TLN[t - 12], 0.55f + 0.12f * (float)i, (float)i * s * 1.3333f);
		break;
	}
	case 15:    /* the Nord twice, two triplet beats apart: its own place then an octave down, and the
		 * next time the other way round (an octave down, then its place) */
		kit_nord_pat ^= 1u;
		kit_later(c, ch, 21, 0.8f, 0.0f);
		kit_later(c, ch, 20, 0.85f, s * 2.6667f);
		break;
	case 17: {  /* the fourth bar's hats alone, slowing down: each gap a fifth longer (the groove's hats held) */
		float at = 0.0f, gap = 1.5f;
		for (int i = 0; i < 7 && at < 15.5f; i++) {
			kit_later(c, ch, 22, 0.78f - 0.04f * (float)i, at * s);
			at += gap;
			gap *= 1.2f;
		}
		break;
	}
	case 16:    /* kick, snare, hat in triplets */
		kit_later(c, ch, 0, 0.8f, 0.0f);
		kit_later(c, ch, 1, 0.7f, s * 1.3333f);
		kit_later(c, ch, 2, 0.75f, s * 2.6667f);
		break;
	case 4:     /* call and answer: high, low, high, low, swung */
		kit_later(c, ch, 12, 0.8f, 0.0f);
		kit_later(c, ch, 10, 0.85f, s * 1.5f);
		kit_later(c, ch, 12, 0.65f, s * 2.5f);
		kit_later(c, ch, 10, 0.9f, s * 3.0f);
		break;
	case 5:     /* snare and toms */
		kit_later(c, ch, 1, 0.6f, 0.0f);
		kit_later(c, ch, 12, 0.8f, s);
		kit_later(c, ch, 1, 0.7f, s * 2.0f);
		kit_later(c, ch, 10, 0.9f, s * 3.0f);
		break;
	case 6:     /* the clank and the Nord: pitched, metallic */
		kit_later(c, ch, 3, 0.8f, 0.0f);
		kit_later(c, ch, 5, 0.75f, s * 1.5f);
		kit_later(c, ch, 3, 0.9f, s * 3.0f);
		break;
	case 7:     /* one low tom, left to ring */
		kit_later(c, ch, 10, 0.97f, s * 2.0f);
		break;
	case 8:     /* a crunch roll into the low tom */
		kit_later(c, ch, 8, 0.8f, 0.0f);
		kit_later(c, ch, 10, 0.9f, s * 3.0f);
		break;
	case 9:     /* half-time, up: the third, then the root up high */
		kit_later(c, ch, 11, 0.8f, 0.0f);
		kit_later(c, ch, 13, 0.85f, s * 2.0f);
		break;
	default:    /* a ripple of ghosts, then the high tom */
		for (int i = 0; i < 5; i++)
			kit_later(c, ch, 7, 0.4f + 0.06f * (float)i, (float)i * s * 0.5f);
		kit_later(c, ch, 13, 0.75f, s * 3.0f);
		break;
	}
}
/* hats in triplets: n of them, three to every two sixteenths, from now */
static void kit_trip(chan *c, int ch, int n, float step_s, float vel)
{
	for (int i = 0; i < n; i++)
		kit_later(c, ch, 2, vel * (i == 0 ? 1.0f : 0.75f + 0.15f * ku()), (float)i * step_s * (2.0f / 3.0f));
}

/* ---- the grooves (•• + FFWD on a Kit channel): one bar of the device in 32 slots, half a step
 * each, for the kick, the snare and the hats. x strong, X an accent (rings long), o a hit,
 * g a ghost (more of them with ACTIVITY), s a squashed snare, O an open hat, t hats in triplets.
 * Each has its tempo in mind: two-step at 65 (130 felt), drill at 70 (140), jungle at 85 (170),
 * downtempo at 85 in sixteenths (unit 2), house at 120, thrash at 95 (190), reggaeton's dembow and
 * cumbia at 95, jazz at 70 (a ride swung in triplets, 140 felt), IDM at 90 (glitch: ratchets
 * and skipped slots). Swing delays the off slots. ---- */
typedef struct { const char *k, *s, *h; float swing; uint8_t unit, glitch; } kit_groove;
static const kit_groove KG[GL_NGROOVE - 1] = {
	{"x.........o.....x.....o...o.....", "....x.......x.......x.......x..g",
	 "..o.g.o...o.g.o...o.g.o.g.o...O.", 0.35f, 1, 0},   /* two-step */
	{"x......o..o.....x.....o...o..o..", "........x.............g.s.....g.",
	 "o.o.o.t.o.o.o.o.o.o.t.o.o.o.t.o.", 0.00f, 1, 0},   /* drill */
	{"x.x.......xx....x.x.......x.....", "....x..g.x..x..g....x..g.x....s.",
	 "o.o.o.o.o.o.o.o.o.o.o.o.o.o.o.O.", 0.10f, 1, 0},   /* jungle */
	{"x...........o...........o.......", "........x...............x.....g.",
	 "o...o...o...o...o...o...o...o.O.", 0.30f, 2, 0},   /* downtempo */
	{"x.......x.......x.......x.......", "........x...............x.......",
	 "g...O...g...O...g...O...g...O.g.", 0.12f, 2, 0},   /* house 120 */
	{"x...x...x...x...x...x...x.x.xxxx", "..x...x...x...x...x...x...x.x.s.",
	 "o.o.o.o.o.o.o.o.o.o.o.o.o.o.o.O.", 0.00f, 1, 0},   /* thrash 95 (190) */
	{"x.......x.......x.......x.......", "......x.....x.........x.....x...",
	 "o...o...o...o...o...o...o...o.g.", 0.05f, 2, 0},   /* reggaeton 95 */
	{"x...............x...............", "....g...x.....g.....g...x.....g.",
	 "o.g.g.o.o.g.g...o.g.g.o.o.g.g...", 0.08f, 2, 0},   /* cumbia 95 */
	{"g...............o...............", "......g.....g.........g...g.s...",
	 "o...o.o.o...o.o.o...o.o.o...o.o.", 0.67f, 2, 0},   /* jazz 70 (140) */
	{"x..x.....x.x..x...x.....x..x....", "......x.....g..x.......xg...s...",
	 "oooo..o.o..ooo.oo.o..oooo.o.ooo.", 0.00f, 1, 1},   /* idm 90 */
};
static void kit_slot(chan *c, int ch, char e, int lane, float vel, float t_s, float h, int lean)
{
	switch (e) {
	case 'X': kit_later(c, ch, lane, 1.0f, t_s); break;
	case 'x': kit_later(c, ch, lane, 0.95f, t_s); break;
	case 'o': kit_later(c, ch, lane, vel, t_s); break;
	case 'g':
		if (!lean && ku() < 0.3f + 0.7f * h)
			kit_later(c, ch, lane == 1 ? 7 : lane, lane == 1 ? 0.8f : 0.4f, t_s);
		break;
	case 's': kit_later(c, ch, ku() < 0.5f + 0.4f * h ? 9 : lane, 0.95f, t_s); break;
	case 'O': kit_later(c, ch, 6, 0.85f, t_s); break;
	default: break;
	}
}

/* ---- audition: a pattern typed in by hand (the tuning page's step editor) in place of the Kit's own
 * lanes: per row, a bit for each of up to 32 sixteenths and a bit for its accent; the feel rules,
 * the rotation and the colour still apply ---- */
static const uint8_t KPAT_LANE[14] = {0, 1, 7, 9, 2, 6, 3, 4, 5, 8, 10, 11, 12, 13};
static uint32_t kpat_on[14], kpat_acc[14];
static uint8_t kpat_mode, kpat_len = 16, kpat_pos;
static float kpat_swing;
void gl_kit_pattern(int row, int step, int v)
{
	if ((unsigned)row >= 14u || (unsigned)step >= 32u)
		return;
	uint32_t b = 1u << step;
	kpat_on[row] = v ? kpat_on[row] | b : kpat_on[row] & ~b;
	kpat_acc[row] = v == 2 ? kpat_acc[row] | b : kpat_acc[row] & ~b;
}
void gl_kit_pattern_mode(int on, int len, float swing)
{
	kpat_mode = (uint8_t)(on != 0);
	kpat_len = (uint8_t)(len < 1 ? 1 : len > 32 ? 32 : len);
	kpat_swing = swing < 0.0f ? 0.0f : swing > 0.75f ? 0.75f : swing;
}
int gl_kit_pattern_pos(void) { return kpat_pos; }

/* ---- the lanes ---- */
static void kit_step(chan *c, int ch, uint32_t step)
{
	const float h = eff_activity(ch), x = P.ch[ch][CP_CHAR];
	const int s16 = (int)(step % 16u);
	/* the load governor stepping in (a block past 88 %): the core plays on, the colour waits */
	const int lean = gov_cap < FM_CAP && !kit_solo;
	const int phrase = (int)(step % 64u);                 /* four bars: the accents land here */
	const float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]) * (float)(GL_CHX_DIV(P.chx[ch]) + 1u);
	/* the rotation: at a phrase's start, at full macro (now and then), each segment a bar, half
	 * a bar or a third, turning kick -> snare -> hat -> kick, until it comes round to the start */
	kit_state *k = &c->kt;
	k->pos16 = (uint8_t)s16;
	k->k_since += step_s;                             /* (the kick fits its ring to this) */
	if (w_exit[ch] && (s16 & 3u) == 0u) {               /* song mode's drums leaving: a fill into the reverb */
		w_exit[ch] = 0;
		const int t = kit_fill_pick(ch);
		kit_fill(c, ch, t ? t : 1, step_s);
		P.ch[ch][CP_VERB] = clampf(Pb.ch[ch][CP_VERB] + 0.7f, 0.0f, 1.0f);
		k->vthr = 6;
	}
	if (k->vthr && !--k->vthr)
		P.ch[ch][CP_VERB] = Pb.ch[ch][CP_VERB];       /* the reverb throw over */
	if ((step & 63u) == 0u) {
		kit_vthr_done = 0;
		const float u = KT[KT_IV_HITS] > 0.0f ? kr2() : 1.0f;
		kit_ivmode = (uint8_t)(u < KT[KT_IV_P35] ? 1 : u < KT[KT_IV_P35] + KT[KT_IV_PALL] ? 2 : 0);
	}
	if ((step & 15u) == 0u)                           /* a bar: now and then the colour an octave further down, or up */
		kit_boct = KT[KT_OCT_MOVE] > 0.0f && kr2() < KT[KT_OCT_MOVE] ? (kr2() < 0.5f ? -12.0f : 12.0f) : 0.0f;
	if (k->thr && !--k->thr)
		P.ch[ch][CP_ECHO] = Pb.ch[ch][CP_ECHO];       /* the throw over */
	/* the director. A piece has an arc, its sections (8 bars) more or less intense; a section's phrases
	 * (4 bars) each build a spice from nothing at the downbeat to its peak at the fourth bar's start,
	 * then hold it through that bar: a shape and a peak of their own each time (and now and then a
	 * plain phrase). Everything that is spice rides on it: the map's excursion from the piece's home
	 * (near the trad kit), the weird windows, the throws, the pumping, the euclidean morph and most of
	 * the macro's morph; the dropouts and the rotations fall late in the phrase */
	{
		const float md = mac_depth[ch], arc = KTB[KT_ARC_AMT];
		const int32_t sec = (int32_t)(step / 128u);
		if (!KD.seeded || step < KD.last) {           /* a new piece: its own home and its own arc */
			kit_map_reset = 1;
			KD.bx = 0.25f * ku();
			KD.mx = KD.tx = KD.bx;
			KD.my = KD.ty = 0.0f;
			KD.turn = (uint8_t)(ku() * 8.0f);
			KD.sec = -1;
			KD.seeded = 1;
			KD.plain = 1;                             /* (the first phrase plain: the piece starts as itself) */
			KD.drop_mask = 0;
			KD.thr_base = 0.0f;
		}
		KD.last = step;
		KD.bar = (uint8_t)((step >> 4) & 3u);
		if (sec != KD.sec) {
			KD.sec = sec;
			const float I = KARC[(sec + KD.turn) & 7];
			KD.nov = clampf(arc * (0.3f + 0.7f * I) + 0.5f * md, 0.0f, 1.0f);
			KD.pump_sel = 0.0f;
			if (arc > 0.0f && KD.nov > 0.4f && ku() < 0.5f)
				KD.pump_sel = (ku() < 0.5f ? -1.0f : 1.0f) * (0.25f + 0.45f * KD.nov) * KTB[KT_PUMPS];
		}
		if (phrase == 0 && step > 0) {                /* a phrase: its shape, its peak, where it heads */
			const float N = KD.nov;
			KD.plain = (uint8_t)(arc <= 0.0f && md <= 0.0f ? 1 : ku() < 0.1f + 0.25f * (1.0f - md));
			KD.shape = (uint8_t)(ku() * 4.0f);
			KD.pk = clampf((0.45f * N + 0.55f * md) * (0.75f + 0.35f * ku()), 0.0f, 1.0f);
			KD.tx = ku();
			KD.ty = clampf(N * (0.5f + 0.8f * ku()), 0.0f, 1.0f);
			KD.drop_mask = 0;
			if (arc > 0.0f && !KD.plain && ku() < 0.3f * KD.pk * KTB[KT_DROPOUTS]) {
				const float u = ku();
				KD.drop_bar = (uint8_t)(ku() < 0.75f ? 3 : 2);
				KD.drop_mask = (uint8_t)(u < 0.4f ? 1u : u < 0.75f ? (4u | 64u) : (uint8_t)~(4u | 64u));
			}
			KD.thr_base = arc > 0.0f ? (0.04f + 0.2f * N) * KTB[KT_THROWS] * arc : 0.0f;
			for (int j = 0; j < 3; j++)
				KD.em[j] = (int8_t)((ku() * 3.0f - 1.0f) * KD.pk * KTB[KT_EMORPH] + 0.5f);
			if (arc > 0.0f && !KD.plain && !k->rot_r && ku() < 0.3f * KD.pk)
				k->rot_r = 4;                             /* (armed: it turns in the fourth bar) */
		}
		{
			const float x = phrase >= 48 ? 1.0f : (float)phrase / 48.0f;
			float e;
			switch (KD.shape) {
			case 1: e = x * x * x; break;                                       /* late */
			case 2: e = (float)(phrase >> 4) * (1.0f / 3.0f); break;            /* a step each bar */
			case 3: e = x > 0.5f ? (x - 0.5f) * 2.0f : 0.0f; break;             /* held off, then a sweep */
			default: e = x; break;
			}
			KD.env = KD.plain ? 0.0f : e;
			KD.S = KD.env * KD.pk;
		}
		KD.throw_p = KD.thr_base * KD.S * 1.5f;
		kit_pump = KD.pump_sel * KD.S;
		kit_pump_t = 1.0f;
		/* the map: on tuning 0 (the tuned kits stay themselves), unless the tuning page holds the numbers */
		if ((!kit_tune_lock || kit_dir_force) && kit_tune_cur == 0 && KTB[KT_MAP_ON] >= 0.5f) {
			if (KTB[KT_MAP_FIX] >= 0.5f)
				KD.mx = KTB[KT_MAP_X], KD.my = KTB[KT_MAP_Y];
			else {
				const float gx = KD.bx + (KD.tx - KD.bx) * KD.S, gy = KD.ty * KD.S;
				KD.mx += (gx - KD.mx) * 0.12f;
				KD.my += (gy - KD.my) * 0.12f;
			}
			/* (blended again only every fourth step, and only once it has moved: the blend is the
			 * director's one heavy job) */
			static float lx = -1.0f, ly = -1.0f;
			if (kit_map_reset)
				lx = ly = -1.0f, kit_map_reset = 0;
			const float dx = KD.mx - lx, dy = KD.my - ly;
			if ((step & 3u) == 0u && dx * dx + dy * dy > 4e-6f) {
				lx = KD.mx;
				ly = KD.my;
				kit_map(KD.mx, KD.my);
			}
		}
	}
	/* evolving: how weird this step is. The phrase's form: three bars restrained and one weird; or that
	 * within a bar (three beats and one), or within half a bar (six sixteenths and two); or a build,
	 * weirder and weirder over the phrase, then quiet again. The macro picks a form each phrase (more
	 * often the small ones the deeper it goes) and sets how weird the windows get */
	{
		const float md = mac_depth[ch];
		if (phrase == 0) {
			const int f = (int)(KT[KT_EV_FORM] + 0.5f);
			if (f >= 1 && f <= 4)
				k->ev_form = (uint8_t)f;
			else {
				const float u = ku();
				k->ev_form = (uint8_t)(u < 0.45f - 0.2f * md ? 1 : u < 0.65f ? 4 : u < 0.85f ? 2 : 3);
			}
		}
		/* (all of it inside the phrase's build: nothing at the downbeat, the most in the fourth bar) */
		const float e = KD.env;
		float w;
		switch (k->ev_form) {
		case 2: w = e * ((s16 >> 2) == 3 ? 1.0f : 0.3f); break;
		case 3: w = e * ((s16 & 7) >= 6 ? 1.0f : 0.3f); break;
		case 4: w = phrase >= 60 ? 0.0f : e; break;
		default: w = phrase >= 48 ? e : 0.3f * e; break;
		}
		const float arcw = KTB[KT_ARC_WEIRD] * (KD.nov > 0.3f ? (KD.nov - 0.3f) * 1.4f : 0.0f) * KTB[KT_ARC_AMT] / 0.6f;
		k->weird = w * clampf(KT[KT_EV_AMT] + KT[KT_EV_MACRO] * md + arcw, 0.0f, 1.0f);
	}
	if (k->rot_r == 4) {                               /* armed by the director: a turn each third of the fourth bar */
		if (phrase == 48) {
			k->rot_r = 1;
			k->rot_i = 0;
			k->rot_seg = 3;
			k->rot_left = 5;
		}
	} else if (k->rot_r) {
		if (k->rot_left)
			k->rot_left--;
		if (!k->rot_left) {
			k->rot_r = (uint8_t)((k->rot_r + 1u) % 3u);
			k->rot_i++;
			k->rot_left = k->rot_seg == 3 ? (uint8_t)(k->rot_i % 3u == 2u ? 6 : 5) : k->rot_seg == 2 ? 8 : 16;
		}
	} else if (kit_rot_always ? phrase == 0 : phrase == 48 && !KD.plain && ku() < (mac_depth[ch] - 0.4f) * 0.9f) {   /* (from 0.4 of the macro up, likelier as it goes: the fourth bar) */
		k->rot_r = 1;
		k->rot_i = 0;
		k->rot_seg = kit_rot_always ? (uint8_t)(1 + (int)(ku() * 3.0f)) : 3;
		k->rot_left = k->rot_seg == 3 ? 5 : k->rot_seg == 2 ? 8 : 16;
	}
	/* the macro turns the euclidean rings: at each phrase (more often as it goes deeper) the kick's,
	 * the snare's and the hats' rings move to new places, so the pattern keeps shifting */
	if (phrase == 0 && ku() < mac_depth[ch] * 1.2f) {
		k->eo_k = (uint8_t)(ku() * 16.0f);
		k->eo_s = (uint8_t)(ku() * 16.0f);
		k->eo_h = (uint8_t)(ku() * 12.0f);
	} else if (phrase == 0 && mac_depth[ch] <= 0.0f)
		k->eo_k = k->eo_s = k->eo_h = 0;
	if (kpat_mode) {
		const int sp = (int)(step % kpat_len);
		kpat_pos = (uint8_t)sp;
		const float t_s = (sp & 1) ? kpat_swing * step_s : 0.0f;
		for (int r = 0; r < 14; r++)
			if (kpat_on[r] >> sp & 1u)
				kit_later(c, ch, KPAT_LANE[r], kpat_acc[r] >> sp & 1u ? 0.97f : 0.72f + 0.06f * ku(), t_s);
		return;
	}
	const int gv = (int)GL_CHX_GRV(P.chx[ch]);
	if (gv >= 1 && gv < GL_NGROOVE) {
		/* a groove: this step's two slots */
		const kit_groove *g = &KG[gv - 1];
		if (g->glitch && !lean && ku() < 0.12f + 0.2f * h) {
			/* IDM: a ratchet (hats or the snare, 3 to 6 in the space of a step or two) */
			int n = 3 + (int)(ku() * 4.0f), ln = ku() < 0.6f ? 2 : 7;
			float span = step_s * (ku() < 0.5f ? 1.0f : 2.0f);
			for (int i = 0; i < n; i++)
				kit_later(c, ch, ln, (ln == 2 ? 0.6f : 0.8f) * (1.0f - 0.08f * (float)i), span * (float)i / (float)n);
		}
		for (int j = 0; j < 2; j++) {
			if (g->glitch && ku() < 0.12f)
				continue;                                 /* IDM: a slot skipped */
			int sl = s16 * 2 + j;
			float t_s = 0.0f;
			if (g->unit == 1) {
				if (j)
					t_s = 0.5f * step_s * (1.0f + g->swing);
			} else {
				if (j)
					continue;
				if (s16 & 1)
					t_s = g->swing * step_s * 0.5f;
				sl = (s16 * 2) % 32;
			}
			float hv = (j || (s16 & 1) ? 0.55f : 0.75f) * (0.85f + 0.12f * ku());
			char ek = g->k[sl], es = g->s[sl], eh = g->h[sl];
			if (ek == 'x' && phrase == 0 && j == 0)
				ek = 'X';
			kit_slot(c, ch, ek, 0, 0.8f * (0.85f + 0.12f * ku()), t_s, h, lean);
			if (es == 'x' && phrase >= 56 && ku() < 0.35f * KT[KT_S_SQUASH])
				es = 's';                                 /* the phrase's end: now and then squashed */
			kit_slot(c, ch, es, 1, 0.85f, t_s, h, lean);
			if (eh == 't')
				kit_trip(c, ch, 3, step_s * (g->unit == 1 ? 0.5f : 1.0f), hv);
			else
				kit_slot(c, ch, eh, 2, hv, t_s, h, lean);
		}
	} else {
		/* euclidean rings */
		float acc = s16 == 0 ? 0.95f : (s16 & 3) == 0 ? 0.8f : 0.6f;
		const int emo = KD.S > 0.35f;                  /* (the morph only once the phrase has built) */
		int kk = 1 + (int)(h * 3.0f + 0.5f) + KD.em[0] * emo, sk = 1 + (int)(h * 2.0f + 0.5f) + KD.em[1] * emo, hk = 3 + (int)(h * 6.0f + 0.5f) + KD.em[2] * emo;
		kk = kk < 1 ? 1 : kk > 8 ? 8 : kk;              /* (the director's morph, kept to sense) */
		sk = sk < 1 ? 1 : sk > 4 ? 4 : sk;
		hk = hk < 1 ? 1 : hk > 11 ? 11 : hk;
		/* the phrase's fourth bar: the kick in threes across it (0, 3, 6, 9, 12, 15) */
		const int eon = phrase >= 48 || kit_rot_always;   /* (the rings turned in the fourth bar) */
		int ek = (s16 + k->eo_k * eon) % 16, es = (s16 + 12 + k->eo_s * eon) % 16, eh = (int)((step + k->eo_h * eon) % 12u);
		int kick = phrase >= 48 && !lean && KT[KT_F_THREES] >= 0.5f ? s16 % 3 == 0 : euclid_hit(kk, 16, ek);
		if (kick)
			kit_fire(c, ch, 0, phrase == 0 ? 1.0f : acc * (0.85f + 0.12f * ku()));
		if (euclid_hit(sk, 16, es)) {
			/* the phrase's last snare an accent, often squashed; now and then one on the 4 */
			int sq = (phrase == 60 && ku() < 0.5f * KT[KT_S_SQUASH]) || (s16 == 12 && ku() < 0.12f * KT[KT_S_SQUASH]);
			kit_fire(c, ch, sq ? 9 : 1, sq ? 0.95f : phrase == 60 ? 1.0f : 0.82f + 0.13f * ku());
		} else if (!lean && h > 0.25f && (s16 & 1) && ku() < 0.22f * h * KT[KT_S_GHOSTS])
			kit_fire(c, ch, 7, 0.8f);                    /* a ghost note */
		int s12 = eh;
		if (!lean && s16 == 12 && ((step >> 4) & 1u) && ku() < 0.35f * h)
			kit_trip(c, ch, 6, step_s, 0.6f);            /* the bar's last beat in triplets */
		else if (euclid_hit(hk, 12, s12)) {
			/* the open hat: past 0.55, once in a while, on the off-beat */
			int op = s12 == 6 && (KSOLO(6) || (x > 0.55f && ku() < KT[KT_H_OPEN]));
			kit_fire(c, ch, op ? 6 : 2, op ? 0.9f : (s12 & 1 ? 0.55f : 0.8f) * (0.85f + 0.12f * ku()));
		}
	}
	/* a tom fill down the chord into the next phrase (now and then; always on Playworld's fill):
	 * the root up high, the fifth, the third, the root */
	if (phrase == 0)
		kit_hat_mute = 0;
	if (!lean && phrase == 48) {                       /* the fourth bar: will this phrase end in a fill, and which */
		kit_fill_t = (uint8_t)((form_fill || kr2() < (0.05f + 0.05f * h) * KT[KT_TOM_FILL] * KTB[KT_FILL_RATE]) ? kit_fill_pick(ch) : 0);
		if (kit_fill_t && KD.drop_bar >= 3)
			KD.drop_mask = 0;                          /* (a fill and a dropout don't share the bar) */
		if (kit_fill_t == 11 || kit_fill_t == 17) {    /* the hat fills take the whole bar */
			kit_hat_mute = 1;
			kit_fill(c, ch, kit_fill_t, step_s);
			kit_fill_t = 0;
		}
	}
	if ((KSOLO(10) && s16 == 12) || (!lean && phrase == 60 && kit_fill_t)) {
		kit_fill(c, ch, KSOLO(10) && !kit_fill_t ? 1 : kit_fill_t, step_s);
		kit_fill_t = 0;
	} else if (form_fill && s16 >= 12)
		kit_fire(c, ch, 1, 0.7f + 0.2f * ku());          /* Playworld's fill */
	if (lean)
		return;
	/* a weird window's extras: hat ratchets, and a crushed roll as a three-and-one window closes */
	if (k->weird > 0.05f) {
		if (ku() < 0.25f * k->weird)
			kit_trip(c, ch, 3 + (int)(ku() * 3.0f), step_s * (ku() < 0.5f ? 0.5f : 1.0f), 0.55f + 0.3f * k->weird);
		if (k->ev_form == 1 && phrase == 59 && ku() < k->weird)
			kit_fire(c, ch, 8, 0.85f);
	}
	int ck = 1 + (int)(h * 3.0f + 0.5f);
	if (KT[KT_C_RATE] > 0.0f && euclid_hit((int)((float)ck * KT[KT_C_RATE] + 0.5f), 7, (int)((step + 2u) % 7u)))
		kit_fire(c, ch, 3, 0.6f + 0.3f * ku());
	if ((x > 0.33f || KSOLO(4)) && euclid_hit(h > 0.5f ? 2 : 1, 10, (int)((step + 5u) % 10u)))
		kit_fire(c, ch, 4, ku() < 0.15f ? 1.0f : 0.7f + 0.25f * ku());   /* now and then a long stroke */
	if ((x > 0.4f || KSOLO(5)) && euclid_hit(h > 0.6f ? 2 : 1, 13, (int)((step + 9u) % 13u)))
		kit_fire(c, ch, 5, 0.7f + 0.25f * ku());
	if ((x > 0.6f || KSOLO(8)) && (step % (KSOLO(8) ? 16u : 64u)) == (KSOLO(8) ? 4u : 56u) && ku() < 0.5f + 0.5f * h)
		kit_fire(c, ch, 8, 0.8f);                        /* a roll into the next phrase */
}

/* ---- the colour: after the seven sounds, something to tie them to the track ----
 *   1 comb      a comb on a chord tone (C3 octave), gliding between the chord's root, fifth
 *               and third every few seconds, a slow wobble on it: drums that sing the chord
 *   2 resonate  three resonators on the chord's tones (C5 octave), struck by every hit: shimmer
 *   3 tzfm      a sine on the chord's root (C4) whose frequency the drums push through zero
 *               (it runs backwards when they swing negative): a clang that follows each hit
 *   4 soggy     a resonant low-pass that opens on each hit and closes in 30 ms, a little
 *               saturation, and the pitch sagging as the hit dies (a short delay stretching)
 *   5 metal     two combs a tritone apart on the root (C4), one inverted: a hollow, bell-like ring
 *   7 comb-sog  soggy by a comb instead: a short comb on the chord's root (C4), fed back inverted
 *               through a dark one-pole (boxy, hollow, damp), its delay wandering on an LFO, the
 *               dry dulled under it; then a Chebyshev shaper on the low end only (below ~250 Hz),
 *               the top left clean. A pressed hit tightens the comb, a long one feeds it more
 *   6 sog+cheby soggy, then a Chebyshev shaper (second and third harmonics): the default. Each hit
 *               sets how the soggy filter behaves: a pressed one shuts it fast and opens it less,
 *               a long one lets it close slowly
 * The combs live in the channel's string buffer (the Kit has no strings). */
static inline float kabs(float x) { return x < 0.0f ? -x : x; }
void gl_kit_color(int method) { KT[KT_COL] = (float)(method < 0 ? 0 : method > 8 ? 8 : method); }
#define KC_N 1536                                     /* each comb's line: 128 ms at 12 kHz */
static inline float kc_read(const int16_t *b, uint16_t w, float d)
{
	float pos = (float)w - d;
	pos += pos < 0.0f ? (float)KC_N : 0.0f;
	int i0 = (int)pos, i1 = i0 + 1 >= KC_N ? 0 : i0 + 1;
	float f = pos - (float)i0;
	return ((float)b[i0] + f * (float)(b[i1] - b[i0])) * (1.0f / 4096.0f);
}
static inline int16_t kc_w(float x)
{
	x *= 4096.0f;
	return (int16_t)(x > 32767.0f ? 32767 : x < -32767.0f ? -32767 : (int)x);
}
/* the chorus at the very start of the kit's chain: two taps (around 9 and 14 ms) swaying in opposite
 * phase, a slow LFO; mixed in under the dry kit (shared by every Kit channel: one line) */
#define KCHO_N 512                                 /* (a power of two: wrapped by a mask) */
static float kit_cho[KCHO_N];
static uint32_t kit_cho_w;
static float kit_cho_ph;
static float kit_side[KSUB];                       /* the chorus's two taps apart: left plus, right minus */
static uint8_t kit_side_on;
static float kit_cho_duck = 1.0f;
static void kit_chorus(float *A)
{
	const float mx = KT[KT_CHO_MIX], dry = 1.0f - 0.35f * mx, wet = 0.5f * mx;
	const float wd = KT[KT_CHO_WIDE] * (1.0f + KT[KT_CHO_SWELL] * KD.S);   /* (wider as the phrase builds) */
	const float sw = 0.5f * mx * wd;
	kit_cho_duck = 1.0f - KT[KT_CHO_DUCK] * mx * 2.0f * (wd < 1.5f ? wd : 1.5f);   /* (and a touch quieter, as the spread sounds louder) */
	const float dp = KT[KT_CHO_DEPTH] * 0.001f * KSR, d1 = 0.009f * KSR, d2 = 0.014f * KSR;
	kit_cho_ph += KT[KT_CHO_RATE] * (float)KSUB / KSR;
	kit_cho_ph -= kit_cho_ph >= 1.0f ? 1.0f : 0.0f;
	const float sn = sin_u((uint32_t)(kit_cho_ph * 4294967296.0f));
	/* the taps' delays, set once a block (the sway is slow): whole samples and a fraction each */
	const float t1 = d1 + dp * sn, t2 = d2 - dp * sn;
	const uint32_t n1 = (uint32_t)t1, n2 = (uint32_t)t2;
	const float f1 = t1 - (float)n1, f2 = t2 - (float)n2;
	uint32_t w = kit_cho_w;
	for (uint32_t i = 0; i < KSUB; i++) {
		const float x = A[i];
		kit_cho[w] = x;
		/* (a delay of n + f: between the samples n and n + 1 back) */
		const float a0 = kit_cho[(w - n1) & (KCHO_N - 1)], a1 = kit_cho[(w - n1 - 1u) & (KCHO_N - 1)];
		const float b0 = kit_cho[(w - n2) & (KCHO_N - 1)], b1 = kit_cho[(w - n2 - 1u) & (KCHO_N - 1)];
		const float y1 = a0 + f1 * (a1 - a0), y2 = b0 + f2 * (b1 - b0);
		A[i] = x * dry + (y1 + y2) * wet;
		kit_side[i] = (y1 - y2) * sw;
		w = (w + 1u) & (KCHO_N - 1);
	}
	kit_cho_w = w;
}
static void kit_color(chan *c, float *A)
{
	kit_state *k = &c->kt;
	const int m = (int)(KT[KT_COL] + 0.5f);
	int16_t *b1 = c->ks, *b2 = c->ks + KC_N;
	const float dt = (float)KSUB / KSR;
	float tail = 0.0f;
	if (m == 1) {
		/* which chord tone: root, fifth, third, a new one every ~3 s */
		k->c_t += dt;
		if (k->c_t > 3.0f) {
			k->c_t = 0.0f;
			k->c_sel = (uint8_t)((k->c_sel + 1u) % 3u);
		}
		static const uint8_t SEL[3] = {0, 2, 1};
		float f = kit_hz(48 + chord_pc[SEL[k->c_sel]]);
		k->c_lfo += dt * 0.3f;
		k->c_lfo -= k->c_lfo >= 1.0f ? 1.0f : 0.0f;
		float tgt = KSR / f * (1.0f + 0.006f * sin_u((uint32_t)(k->c_lfo * 4294967296.0f)));
		if (k->c_d1 < 8.0f)
			k->c_d1 = tgt;
		float d = k->c_d1, lp = k->c_lp1;
		uint16_t w = k->c_w;
		for (uint32_t i = 0; i < KSUB; i++) {
			d += (tgt - d) * KP(0.0008f);                      /* (it glides to the next tone) */
			float y = kc_read(b1, w, d);
			lp += KP(0.45f) * (y - lp);                        /* (darker each time round) */
			float in = A[i] + 0.9f * lp;
			b1[w] = kc_w(in);
			w = (uint16_t)(w + 1u >= KC_N ? 0 : w + 1u);
			A[i] = A[i] * 0.55f + lp * 0.75f * KT[KT_RES_MIX];
			tail = tail > kabs(lp) ? tail : kabs(lp);
		}
		k->c_d1 = d; k->c_lp1 = lp; k->c_w = w;
	} else if (m == 2) {
		/* three two-pole resonators (y = a y1 - b y2 + g x) on the chord's tones in the C5 octave,
		 * each ringing a bandwidth of f/167 (as the state-variable band-passes they replace), its peak
		 * gain 6; set again only when the chord changes */
		const uint16_t key = (uint16_t)(chord_pc[0] | chord_pc[1] << 4 | chord_pc[2] << 8);
		const float wofs = clampf(KT[KT_WET_OCT] + kit_boct, -24.0f, 12.0f) + kit_iv;
		if (key != k->c_rmask || k->c_rg[0] == 0.0f || k->c_roct != wofs) {
			k->c_rmask = key;
			k->c_roct = wofs;
			for (int j = 0; j < 3; j++) {
				const float f = kit_hz(72 + chord_pc[j] + (int)k->c_roct), w = 6.2832f * f / KSR;
				const float r = fexp2(-1.4427f * 3.1416f * f * 0.006f / KSR);       /* e^(-pi bw / sr) */
				const float cw = sin_u((uint32_t)(w * (1.0f / 6.2832f) * 4294967296.0f) + 0x40000000u);
				k->c_ra[j] = 2.0f * r * cw;
				k->c_rb[j] = r * r;
				/* |H| at the tone: 1 / ((1 - r) sqrt(1 - 2 r cos 2w + r^2)) */
				const float c2 = 2.0f * cw * cw - 1.0f, q = 1.0f - 2.0f * r * c2 + r * r;
				float sq = q > 0.0f ? q : 0.0f, y = 1.0f;
				for (int it = 0; it < 6; it++)
					y = 0.5f * (y + sq / y);                 /* (its square root) */
				k->c_rg[j] = 6.0f * (1.0f - r) * y;
			}
		}
		float y1[3], y2[3];
		for (int j = 0; j < 3; j++)
			y1[j] = k->c_r1[j], y2[j] = k->c_r2[j];
		const float a0 = k->c_ra[0], a1 = k->c_ra[1], a2 = k->c_ra[2], b0 = k->c_rb[0], b1 = k->c_rb[1], b2 = k->c_rb[2];
		const float g0 = k->c_rg[0], g1 = k->c_rg[1], g2 = k->c_rg[2];
		const float wg = 0.9f * KT[KT_RES_MIX] * KT[KT_WET_AMT];
		const float lk = KT[KT_WET_LP] >= 7900.0f ? 1.0f : 1.0f - fexp2(-9.06f * KT[KT_WET_LP] / KSR);   /* (one pole: 1 - e^(-2 pi fc / sr)) */
		float wl = k->c_wl;
		for (uint32_t i = 0; i < KSUB; i++) {
			const float x = A[i];
			float n0 = a0 * y1[0] - b0 * y2[0] + g0 * x;
			float n1 = a1 * y1[1] - b1 * y2[1] + g1 * x;
			float n2 = a2 * y1[2] - b2 * y2[2] + g2 * x;
			y2[0] = y1[0]; y1[0] = n0;
			y2[1] = y1[1]; y1[1] = n1;
			y2[2] = y1[2]; y1[2] = n2;
			const float sum = n0 + n1 + n2;
			wl += lk * (sum - wl);
			A[i] = x * 0.75f + wl * wg;
			tail = tail > kabs(sum) ? tail : kabs(sum);
		}
		k->c_wl = wl;
		for (int j = 0; j < 3; j++)
			k->c_r1[j] = y1[j], k->c_r2[j] = y2[j];
	} else if (m == 3) {
		const float inc0 = kit_hz(60 + chord_pc[0]) / KSR;
		float ph = k->c_ph, e = k->c_env;
		for (uint32_t i = 0; i < KSUB; i++) {
			float x = A[i], ax = kabs(x);
			e += (ax > e ? KP(0.3f) : KP(0.0017f)) * (ax - e);     /* (in at once, out over ~25 ms) */
			ph += inc0 * (1.0f + 9.0f * x);                /* through zero when the drums swing low */
			ph -= (float)(int)ph;
			ph += ph < 0.0f ? 1.0f : 0.0f;
			float y = sin_u((uint32_t)(ph * 4294967296.0f)) * e * 0.8f;
			A[i] = x * 0.6f + y;
			tail = tail > e ? tail : e;
		}
		k->c_ph = ph; k->c_env = e;
	} else if (m == 4 || m == 6) {
		float e = k->c_env, fa = k->c_fast, s1 = k->c_s1, s2 = k->c_s2, d = k->c_d1, dc = k->c_dc;
		const float relk = k->c_relk > 0.0f ? k->c_relk : 0.0014f, ceil = k->c_ceil > 0.0f ? k->c_ceil : KT[KT_SOG_REACH];
		/* three slow LFOs (14, 8 and 5 s round, deeper with the macro) move where it rests, how far
		 * it opens and how much it rings: the texture shifts over the phrase */
		const int ch = (int)(c - CH);
		const float lw = (0.6f + 0.4f * mac_depth[ch]) * KT[KT_LFO_DEPTH], lr = KT[KT_LFO_RATE];
		k->c_lfo += dt * 0.07f * lr; k->c_lfo -= k->c_lfo >= 1.0f ? 1.0f : 0.0f;
		k->c_l2 += dt * 0.125f * lr; k->c_l2 -= k->c_l2 >= 1.0f ? 1.0f : 0.0f;
		k->c_l3 += dt * 0.2f * lr; k->c_l3 -= k->c_l3 >= 1.0f ? 1.0f : 0.0f;
		const float o1 = sin_u((uint32_t)(k->c_lfo * 4294967296.0f)) * lw, o2 = sin_u((uint32_t)(k->c_l2 * 4294967296.0f)) * lw;
		const float o3 = sin_u((uint32_t)(k->c_l3 * 4294967296.0f)) * lw;
		/* the low-pass's place, once a block: ~250 Hz shut (110-570 as the LFO goes), up to ~5 kHz on the hit */
		const float base = KT[KT_SOG_BASE] * fexp2(1.2f * o1), cl = ceil + 0.8f * o2;
		const ksvf lpf = ksvf_set(ktan(clampf(base * fexp2(cl * clampf(fa * 3.0f, 0.0f, 1.0f)), 60.0f, 0.45f * KSR) / KSR), clampf(KT[KT_SOG_RES] - 0.25f * o3, 0.05f, 2.0f));
		uint16_t w = k->c_w;
		for (uint32_t i = 0; i < KSUB; i++) {
			float x = A[i], ax = kabs(x);
			fa += (ax > fa ? KP(0.5f) : KRAT * relk) * (ax - fa);     /* (30 ms; 10 pressed, 120 long) */
			e += (ax > e ? KP(0.2f) : KP(0.00035f)) * (ax - e);     /* (120 ms) */
			float lp;
			ksvf_run(&lpf, &s1, &s2, x, &lp);
			float u = clampf(lp * 1.8f, -1.5f, 1.5f);
			b1[w] = kc_w(u - 0.148148f * u * u * u);
			/* the read point drifts back as the hit dies: its pitch sags */
			float tgt = (48.0f + 140.0f * KT[KT_SAG] * clampf(1.0f - e * 4.0f, 0.0f, 1.0f)) / KRAT;
			d += (tgt - d) * 0.004f;
			float o = kc_read(b1, w, d < 2.0f ? 2.0f : d) * 0.75f;
			if (m == 6) {                                  /* Chebyshev: T2 and T3 on top */
				float v = clampf(o * KT[KT_CH_DRIVE], -1.0f, 1.0f);
				float y = v + KT[KT_CH_T2] * (2.0f * v * v) + KT[KT_CH_T3] * (4.0f * v * v * v - 3.0f * v);
				dc += KP(0.002f) * (y - dc);
				o = (y - dc) * 0.62f;
			}
			A[i] = o;
			w = (uint16_t)(w + 1u >= KC_N ? 0 : w + 1u);
			tail = tail > e ? tail : e;
		}
		k->c_env = e; k->c_fast = fa; k->c_s1 = s1; k->c_s2 = s2; k->c_d1 = d; k->c_w = w; k->c_dc = dc;
		tail += kabs(A[KSUB - 1]);
	} else if (m == 7) {
		const int ch = (int)(c - CH);
		k->c_lfo += dt * 0.11f;
		k->c_lfo -= k->c_lfo >= 1.0f ? 1.0f : 0.0f;
		const float lw = 0.5f + 0.5f * mac_depth[ch];
		/* the comb: a period of the chord's root (C4 octave), a pressed hit an octave up (tighter),
		 * wandering 3 % on the LFO; a long hit feeds it more */
		const float press = k->c_relk > 0.004f ? 1.0f : 0.0f, lng = k->c_relk > 0.0f && k->c_relk < 0.001f ? 1.0f : 0.0f;
		float tgt = KSR / (kit_hz(60 + chord_pc[0]) * (press ? 2.0f : 1.0f));
		tgt *= 1.0f + 0.03f * lw * sin_u((uint32_t)(k->c_lfo * 4294967296.0f));
		if (k->c_d1 < 2.0f)
			k->c_d1 = tgt;
		const float fb = -clampf(KT[KT_COMB_FB] + 0.2f * lng, 0.0f, 0.97f), damp = kpv(KT[KT_COMB_DAMP]), dull = KP(0.25f), lowk = kpv(clampf(KT[KT_LOW_HZ] * (6.2832f / 24000.0f), 0.005f, 0.5f));
		float d = k->c_d1, l1 = k->c_lp1, l2 = k->c_lp2, lo = k->c_s1, dc = k->c_dc;
		uint16_t w = k->c_w;
		for (uint32_t i = 0; i < KSUB; i++) {
			d += (tgt - d) * KP(0.003f);
			float x = A[i], y = kc_read(b1, w, d);
			l1 += damp * (y - l1);                         /* (dark each time round) */
			b1[w] = kc_w(x + fb * l1);
			w = (uint16_t)(w + 1u >= KC_N ? 0 : w + 1u);
			l2 += dull * (x - l2);                         /* (the dry, dulled) */
			float o = l2 * 0.7f + l1 * 0.8f;
			/* Chebyshev below ~250 Hz: the low band shaped, the rest as it was */
			lo += lowk * (o - lo);
			float v = clampf(lo * KT[KT_CH_DRIVE] * 1.125f, -1.0f, 1.0f);
			float sh = v + KT[KT_CH_T2] * 1.17f * (2.0f * v * v) + KT[KT_CH_T3] * 1.14f * (4.0f * v * v * v - 3.0f * v);
			dc += KP(0.002f) * (sh - dc);
			A[i] = (o - lo) + (sh - dc) * 0.62f;
			tail = tail > kabs(l1) + kabs(lo) ? tail : kabs(l1) + kabs(lo);
		}
		k->c_d1 = d; k->c_lp1 = l1; k->c_lp2 = l2; k->c_s1 = lo; k->c_dc = dc; k->c_w = w;
	} else if (m == 8) {
		/* ring mod: the drums times a chord tone (the root, then the fifth, a new one every 2 s),
		 * mixed in under the dry hit: a bell inside each sound, gone when the sound is */
		k->c_t += dt;
		if (k->c_t > 2.0f) {
			k->c_t = 0.0f;
			k->c_sel = (uint8_t)(k->c_sel ? 0 : 2);
		}
		const float inc = kit_hz(60 + chord_pc[k->c_sel] + (int)(KT[KT_RING_SEMI] + clampf(KT[KT_WET_OCT] + kit_boct, -24.0f, 12.0f) + kit_iv + 0.5f)) / KSR;
		const float mx = KT[KT_RING_MIX] * KT[KT_WET_AMT], dry = 1.0f - 0.6f * mx, wet = 1.5f * mx;
		const float lk = KT[KT_WET_LP] >= 7900.0f ? 1.0f : 1.0f - fexp2(-9.06f * KT[KT_WET_LP] / KSR);
		float ph = k->c_ph, wl = k->c_wl;
		for (uint32_t i = 0; i < KSUB; i++) {
			ph += inc;
			ph -= (float)(int)ph;
			wl += lk * (A[i] * sin_u((uint32_t)(ph * 4294967296.0f)) - wl);
			A[i] = A[i] * dry + wl * wet;
		}
		k->c_ph = ph; k->c_wl = wl;
	} else if (m == 5) {
		const float f = kit_hz(60 + chord_pc[0]);
		const float d1 = KSR / f, d2 = KSR / (f * 1.4142f);
		float l1 = k->c_lp1, l2 = k->c_lp2;
		uint16_t w = k->c_w;
		for (uint32_t i = 0; i < KSUB; i++) {
			float y1 = kc_read(b1, w, d1), y2 = kc_read(b2, w, d2);
			l1 += KP(0.6f) * (y1 - l1);
			l2 += KP(0.6f) * (y2 - l2);
			b1[w] = kc_w(A[i] * 0.5f + 0.93f * l1);
			b2[w] = kc_w(A[i] * 0.5f - 0.92f * l2);       /* (inverted: odd harmonics, hollow) */
			w = (uint16_t)(w + 1u >= KC_N ? 0 : w + 1u);
			float o = (l1 + l2) * 0.6f * KT[KT_RES_MIX] * KT[KT_WET_AMT];
			A[i] = A[i] * 0.6f + o;
			tail = tail > kabs(o) ? tail : kabs(o);
		}
		k->c_lp1 = l1; k->c_lp2 = l2; k->c_w = w;
	}
	k->c_tail = tail;
}

/* ---- each sound's envelope (attack, decay to sustain, a gate, release), the tuning page's: off
 * (and costing nothing) while it is at its defaults. Returns whether it is in use. ---- */
static int kit_env(kit_state *k, int s, float *eg)
{
	const float *e = &KT[KT_E_KICK_A + 5 * s];
	const float A = e[0] * 0.001f, D = e[1] * 0.001f, S = e[2], G = e[3] * 0.001f, Rr = e[4] * 0.001f;
	if (A <= 0.0f && S >= 0.999f && G >= 1.999f)
		return 0;
	/* its stages move once a millisecond; the level in between is a straight line */
	float lvl = k->e_lvl[s], t = k->e_t[s];
	uint8_t st = k->e_st[s];
	const float dt = 0.001f, l0 = lvl;
	if (st >= 1 && st <= 3 && t >= G)
		st = 4;                                           /* the gate shuts: release */
	switch (st) {
	case 1: lvl += A > 0.0005f ? dt / A : 2.0f; if (lvl >= 1.0f) { lvl = 1.0f; st = 2; } break;
	case 2: lvl -= D > 0.0005f ? (1.0f - S) * dt / D : 2.0f; if (lvl <= S) { lvl = S; st = 3; } break;
	case 3: lvl = S; break;
	case 4: lvl -= dt / (Rr > 0.0005f ? Rr : 0.0005f); if (lvl <= 0.0f) { lvl = 0.0f; st = 5; } break;
	default: break;
	}
	t += dt;
	const float step = (lvl - l0) * (1.0f / (float)KSUB);
	float v = l0;
	for (uint32_t i = 0; i < KSUB; i++) {
		v += step;
		eg[i] = v;
	}
	k->e_lvl[s] = lvl; k->e_t[s] = t; k->e_st[s] = st;
	return 1;
}
#define KE(s) (ea >> (s) & 1u ? eg[s][i] : 1.0f)

/* ---- the sound, 12 kHz, mono ---- */
static int kit_render12(chan *c, float *A)
{
	kit_state *k = &c->kt;
	int any = 0;
	float eg[8][KSUB];
	unsigned ea = 0;
	kit_morph(k->morph, k->weird);                       /* (the macro's morph and the weird window, this channel's) */
	for (int s = 0; s < 8; s++)
		ea |= (unsigned)kit_env(k, s, eg[s]) << s;
	/* the hits waiting off the grid */
	for (int i = 0; i < 16; i++)
		if (k->q_t[i] > 0.0f) {
			k->q_t[i] -= (float)KSUB / KSR;
			if (k->q_t[i] <= 0.0f) {
				k->q_t[i] = 0.0f;
				kit_fire(c, (int)(c - CH), k->q_l[i], k->q_v[i]);
			}
		}
	/* the Lich kick: the 4-bit triangle, its pitch falling */
	if (k->k_lich && (k->k_amp[0] > 1e-3f || k->k_nz > 1e-3f)) {
		any = 1;
		const float f = k->kl_f0 * (1.0f + k->kl_fall);
		k->kl_fall *= 0.94f;                               /* (16 ms a fall of e) */
		const uint32_t inc = kinc(f);
		uint32_t ph = k->kl_ph;
		float a = k->k_amp[0] * 1.6f;
		const float dk = k->k_dec[0];
		for (uint32_t i = 0; i < KSUB; i++) {
			ph += inc;
			int v = (int)(ph >> 28);                       /* 16 steps */
			v = v < 8 ? v : 15 - v;
			A[i] += a * ((float)v - 3.5f) * (1.0f / 3.5f) * KE(0);
			a *= dk;
		}
		k->kl_ph = ph;
		k->k_amp[0] = a * (1.0f / 1.6f);
		for (int m = 1; m < 5; m++)
			k->k_amp[m] = 0.0f;
		k->k_nz *= 0.5f;
	}
	/* canvas kick */
	if (!k->k_lich && (k->k_amp[0] > 1e-3f || k->k_nz > 1e-3f)) {
		any = 1;
		/* the five modes, recursive sines (the slack's glide once a block), into A */
		const float g = 1.0f + k->k_glide;
		k->k_glide *= k->k_gk;
		for (int m = 0; m < 5; m++) {
			if (k->k_amp[m] < 1e-3f)
				continue;
			float x = k->k_x[m], y = k->k_y[m], a = k->k_amp[m] * 1.3f;
			const float e = k->k_e[m] * g, dk = k->k_dec[m];
			for (uint32_t i = 0; i < KSUB; i++) {
				x += e * y;
				y -= e * x;
				A[i] += a * x * KE(0);
				a *= dk;
			}
			k->k_x[m] = x; k->k_y[m] = y; k->k_amp[m] = a * (1.0f / 1.3f);
		}
		/* the fingertip: the attack's noise (the modes' attack ramp is in the noise's shadow) */
		if (k->k_nz > 1e-3f) {
			float lp = k->k_lp, nz = k->k_nz;
			const float lk = k->k_lpk, nzk = k->k_nzk;
			for (uint32_t i = 0; i < KSUB; i++) {
				lp += lk * (kn() * nz - lp);
				nz *= nzk;
				A[i] += lp * 0.6f * KE(0);
			}
			k->k_lp = lp; k->k_nz = nz;
		}
	}
	/* the Lich snare: the Karplus-Strong drum */
	if (k->ksd_on) {
		any = 1;
		int16_t *kb = c->ks + KSD_OFF;
		const uint16_t n = k->ksd_n;
		uint16_t j = k->ksd_i;
		const float g = 0.5f * k->ksd_g, rough = k->ksd_rough, out = 5.4f * KT[KT_L_SNARE] / 9000.0f;   /* (its averaging loses level: made up) */
		float peak = 0.0f;
		for (uint32_t i = 0; i < KSUB; i++) {
			const uint16_t j2 = (uint16_t)(j + 1u >= n ? 0 : j + 1u);
			float v = ((float)kb[j] + (float)kb[j2]) * g;
			if (ku() < rough)
				v = -v;                                    /* (the drum's roughness) */
			kb[j] = (int16_t)v;
			A[i] += v * out * KE(1);
			peak = peak > kabs(v) ? peak : kabs(v);
			j = j2;
		}
		k->ksd_i = j;
		if (peak < 4.0f)
			k->ksd_on = 0;
	}
	/* R-8 snare (and its ghosts) */
	if (k->sn.b > 1e-3f || k->sn.n > 1e-3f) {
		any = 1;
		kit_snare_block(&k->sn, A, 0.9f * KT[KT_L_SNARE], ea >> 1 & 1u ? eg[1] : 0);
	}
	/* the Lich hats: the noise register */
	if (k->h_lich && k->h_a > 1e-3f) {
		any = 1;
		const int sh = k->h_lich == 2 ? 6 : 1;               /* (the short loop taps bit 6) */
		const float per = k->lf_per, hk = k->h_k, g = 1.4f;
		float cnt = k->lf_cnt, ha = k->h_a, hp = k->lh_hp;
		uint16_t reg = k->lf_reg;
		for (uint32_t i = 0; i < KSUB; i++) {
			cnt += 1.0f;
			while (cnt >= per) {
				cnt -= per;
				uint16_t bit = (uint16_t)((reg ^ (reg >> sh)) & 1u);
				reg = (uint16_t)((reg >> 1) | (bit << 14));
			}
			const float y = (reg & 1u ? 1.0f : -1.0f) * ha;
			hp += 0.3f * (y - hp);                         /* (its bottom taken off) */
			A[i] += (y - hp) * g * KE(2);
			ha *= hk;
		}
		k->lf_cnt = cnt; k->lf_reg = reg; k->h_a = ha; k->lh_hp = hp;
	}
	/* metal hat, frequency-shifted */
	if (!k->h_lich && k->h_a > 1e-3f) {
		any = 1;
		const float dt = 1.0f / KSR;
		/* the band: placed per hit, swept down from above in the first ms, settling lower on a
		 * long hat; the low-pass after the shift closes as it rings */
		const float prog = k->h_len > 0.0f ? clampf(k->h_t / k->h_len, 0.0f, 1.0f) : 1.0f;
		const ksvf hf = ksvf_set(ktan(k->h_fc0 * (1.0f + k->h_fenv) * (1.0f - (1.0f - k->h_f) * prog) / KSR), 0.8f);
		k->h_fenv *= 0.936f;                              /* (15 ms) */
		k->h_lpk += (k->h_lpe - k->h_lpk) * 0.025f;        /* (about 40 ms) */
		/* the shift's frequency (it slides over the ring), as a turn per sample, once a block */
		float f = k->h_t < k->h_len ? k->h_s0 + (k->h_s1 - k->h_s0) * k->h_t / k->h_len : k->h_s1;
		k->h_t += dt * KSUB;
		uint32_t rp = (uint32_t)(int32_t)(f * dt * 4294967296.0f);
		const float cr = sin_u(rp + 0x40000000u), sr = sin_u(rp);
		float pc = k->h_c, ps = k->h_s, nrm = pc * pc + ps * ps;
		nrm = nrm < 0.25f ? 0.0f : 1.5f - 0.5f * nrm;   /* keep the phasor on the circle */
		if (nrm == 0.0f)
			pc = 1.0f, ps = 0.0f;
		else
			pc *= nrm, ps *= nrm;
		uint32_t ph0 = k->h_ph[0], ph1 = k->h_ph[1], ph2 = k->h_ph[2], ph3 = k->h_ph[3], ph4 = k->h_ph[4], ph5 = k->h_ph[5];
		float bp1 = k->h_bp, lo = k->h_lo, hh = k->h_hp, l2 = k->h_l2, l3 = k->h_l3, ha = k->h_a, ad = k->h_ad;
		const float hk = k->h_k, grit = k->h_grit, hlk = k->h_lpk;
		float hl = k->h_lp;
		for (uint32_t i = 0; i < KSUB; i++) {
			/* the squares: each one's top bit, counted */
			ph0 += K_METAL[0]; ph1 += K_METAL[1]; ph2 += K_METAL[2];
			ph3 += K_METAL[3]; ph4 += K_METAL[4]; ph5 += K_METAL[5];
			int n = (int)((ph0 >> 31) + (ph1 >> 31) + (ph2 >> 31) + (ph3 >> 31) + (ph4 >> 31) + (ph5 >> 31));
			float s = (float)(3 - n) * (1.0f / 3.0f);
			/* the high band (its body) and a little of the low metal */
			float lp, bp = ksvf_run(&hf, &bp1, &lo, s, &lp);
			hh += KP(0.6f) * (bp - hh);
			l2 += KP(0.36f) * (s - l2);
			l3 += KP(0.36f) * (l2 - l3);
			float x0 = ((bp - hh) * 2.0f + (l2 - l3) * grit) * ha;
			ha *= hk;
			/* the two all-pass chains (in z^-2: y = c x + v[n-2], v = c y - x; the two
			 * memories taken in turn, odd and even samples) */
			const int p = (int)(i & 1u);
			float a = x0, b = x0;
			for (int j = 0; j < 2; j++) {
				float ya = K_HA[j] * a + k->ha_v[j][p];
				k->ha_v[j][p] = K_HA[j] * ya - a;
				a = ya;
				float yb = K_HB[j] * b + k->hb_v[j][p];
				k->hb_v[j][p] = K_HB[j] * yb - b;
				b = yb;
			}
			float re = ad, im = b;
			ad = a;                                       /* (chain A one sample later) */
			hl += hlk * ((re * pc + im * ps) - hl);         /* (shifted up for a positive f) */
			A[i] += hl * 2.8f * KE(2);
			float nc = pc * cr - ps * sr;                 /* the phasor turns */
			ps = pc * sr + ps * cr;
			pc = nc;
		}
		k->h_ph[0] = ph0; k->h_ph[1] = ph1; k->h_ph[2] = ph2; k->h_ph[3] = ph3; k->h_ph[4] = ph4; k->h_ph[5] = ph5;
		k->h_bp = bp1; k->h_lo = lo; k->h_hp = hh; k->h_l2 = l2; k->h_l3 = l3; k->h_a = ha; k->h_ad = ad;
		k->h_c = pc; k->h_s = ps; k->h_lp = hl;
	}
	/* the Lich clank: two pulses, XORed, 1-bit */
	if (k->d_lich && k->d_a > 1e-3f) {
		any = 1;
		uint32_t pc = k->d_pc, pm = k->d_pm;
		float a = k->d_a;
		const uint32_t ic = k->d_ic, im = k->d_im;
		const float dk = k->d_k;
		for (uint32_t i = 0; i < KSUB; i++) {
			pc += ic;
			pm += im;
			A[i] += ((pc ^ pm) >> 31 ? 0.35f : -0.35f) * a * KE(3);
			a *= dk;
		}
		k->d_pc = pc; k->d_pm = pm; k->d_a = a;
	}
	/* DX100 clank */
	if (!k->d_lich && k->d_a > 1e-3f) {
		any = 1;
		for (uint32_t i = 0; i < KSUB; i++) {
			float mo = sin_u(k->d_pm + (uint32_t)(int32_t)(k->d_fb * (k->d_m1 + k->d_m2) * 0.5f * 683565275.6f));
			k->d_m2 = k->d_m1;
			k->d_m1 = mo;
			float y = sin_u(k->d_pc + (uint32_t)(int32_t)(k->d_index * mo)) * k->d_a;
			if (k->d_grit)
				y = (float)(int32_t)(y * 2048.0f) * (1.0f / 2048.0f);
			k->d_pc += k->d_ic;
			k->d_pm += k->d_im;
			k->d_a *= k->d_k;
			k->d_index *= k->d_ik;
			A[i] += y * 0.7f * KE(3);
		}
	}
	/* the Lich shaker: PhISEM */
	if (k->t_lich && k->t_a > 1e-4f && (k->sh_e > 1e-3f || k->sh_lvl > 1e-4f)) {
		any = 1;
		const ksvf bf = ksvf_set(ktan(clampf((2400.0f + 3000.0f * eff_tone((int)(c - CH))) / KSR, 0.0f, 0.24f)), 0.12f);
		float e = k->sh_e, lvl = k->sh_lvl, s1 = k->t_b, s2 = k->t_lo;
		const float p = k->sh_p, ga = 0.5f * k->t_a;
		const float ed = fexp2(-6.644f / (clampf(k->t_len, 0.05f, 4.0f) * KSR));   /* (the shake: its stroke's length) */
		for (uint32_t i = 0; i < KSUB; i++) {
			e *= ed;
			if (ku() < p * e)
				lvl += 0.4f * e;                           /* a collision */
			lvl *= 0.93f;
			float lp, bp = ksvf_run(&bf, &s1, &s2, lvl * kn(), &lp);
			A[i] += bp * ga * KE(4);
		}
		k->sh_e = e; k->sh_lvl = lvl; k->t_b = s1; k->t_lo = s2;
	}
	/* tissue paper over a mic */
	if (!k->t_lich && k->t_a > 1e-4f && k->t_t < k->t_len + 0.08f) {
		any = 1;
		const float dt = 1.0f / KSR;
		/* the stroke's envelope and the hand's pressure, once a block: in over 30 ms, then fading
		 * (a long stroke to a fifth of itself), and out; after the first 30 ms its band falls,
		 * an octave and a half over the stroke */
		float t = k->t_t, e, fall = 0.0f;
		if (t < 0.03f)
			e = t * (1.0f / 0.03f);
		else {
			float u = (t - 0.03f) / (k->t_len > 0.06f ? k->t_len - 0.03f : 0.03f);
			u = u > 1.0f ? 1.0f : u;
			fall = u;
			e = 1.0f - 0.8f * u;
			if (t > k->t_len)
				e *= 1.0f - (t - k->t_len) * (1.0f / 0.06f);
		}
		e = e < 0.0f ? 0.0f : e;
		const ksvf tf = ksvf_set(ktan(k->t_fc * fexp2(-1.5f * fall) / KSR), 1.25f);
		float press = 0.6f + 0.4f * sin_u((uint32_t)((k->t_w0 * t + k->t_ph0) * 4294967296.0f)) * sin_u((uint32_t)((k->t_w1 * t) * 4294967296.0f));
		k->t_t += dt * KSUB;
		const float ep = e * press, pr = k->t_prob * press, ga = 1.5f * k->t_a, gr = k->t_prox * 4.0f * k->t_a;   /* (quieter) */
		float tc = k->t_c, tb = k->t_b, tl = k->t_lo, th = k->t_hp, r1 = k->t_r1, r2 = k->t_r2;
		for (uint32_t i = 0; i < KSUB; i++) {
			float w = kn(), u = 0.5f + 0.5f * w;
			if (u < pr) {                                 /* a crackle: its size and sign from the same draw */
				float a = u / pr;
				tc += (kit_r & 1u ? -1.0f : 1.0f) * a * a * a;
			}
			tc *= 1.0f - KP(0.1f);                                   /* ~0.4 ms */
			float lp, bp = ksvf_run(&tf, &tb, &tl, (tc * 1.2f + w * 0.12f) * ep, &lp);
			th += KP(0.1f) * (bp - th);
			r1 += KP(0.0235f) * (w * ep - r1);
			r2 += KP(0.0235f) * (r1 - r2);
			A[i] += ((bp - th) * ga + r2 * gr) * KE(4);
		}
		k->t_c = tc; k->t_b = tb; k->t_lo = tl; k->t_hp = th; k->t_r1 = r1; k->t_r2 = r2;
	} else if (!k->t_lich)
		k->t_a = 0.0f;
	/* the Lich Nord: the rungler */
	if (k->n_lich && k->n_a > 1e-3f) {
		any = 1;
		float pa = k->rg_pa, pb = k->rg_pb, lp = k->rg_lp, na = k->n_a;
		uint8_t reg = k->rg_reg;
		const float dt = 1.0f / KSR, d = k->rg_depth;
		float dac = (float)(reg & 7u) * (1.0f / 7.0f);
		for (uint32_t i = 0; i < KSUB; i++) {
			pa += k->rg_fa * (1.0f + d * 2.0f * dac) * dt;
			pa -= pa >= 1.0f ? 1.0f : 0.0f;
			pb += k->rg_fb * (1.0f + d * dac) * dt;
			if (pb >= 1.0f) {                              /* B clocks the register; A's state is the data */
				pb -= 1.0f;
				const uint8_t bit = (uint8_t)((pa < 0.5f) ^ (reg >> 7));
				reg = (uint8_t)((reg << 1) | bit);
				dac = (float)(reg & 7u) * (1.0f / 7.0f);
			}
			const float y = ((pa < 0.5f) ^ (pb < 0.5f)) ? 1.0f : -1.0f;
			lp += 0.35f * (y - lp);
			A[i] += lp * na * 0.3f * KT[KT_L_NORD] * KE(5);
			na *= k->n_ak;
		}
		k->rg_pa = pa; k->rg_pb = pb; k->rg_lp = lp; k->rg_reg = reg; k->n_a = na;
	}
	/* Nord sync / FM hit, at twice the rate */
	if (!k->n_lich && k->n_a > 1e-3f) {
		any = 1;
		const int os = k->n_menv > 0.03f ? 2 : 1;          /* twice the rate while the sync sweeps */
		const float dt2 = 1.0f / ((float)os * KSR);
		/* the sweep and the filter move once a block (1 ms) */
		const float f2 = k->n_f1 * fexp2(k->n_sweep * k->n_menv * (1.0f / 12.0f));
		const float fc = clampf(k->n_cut * fexp2(2.0f * k->n_menv), 60.0f, 0.21f * (float)os * KSR);
		const ksvf nf = ksvf_set(ktan(fc * dt2), 0.7f);
		k->n_menv *= k->n_md;
		const float fm4 = k->n_fm * (0.3f + 0.7f * k->n_menv) * 4.0f, f1 = k->n_f1 * dt2, i2 = f2 * dt2;
		float p1 = k->n_p1, p2 = k->n_p2, s1 = k->n_bp1, s2 = k->n_lo1;
		float na = k->n_a, att = k->n_att, dc = k->n_dc;
		for (uint32_t i = 0; i < KSUB; i++) {
			float acc = 0.0f;
			for (int o = 0; o < os; o++) {
				float o2 = 2.0f * p2 - 1.0f;
				p1 += f1 * (1.0f + fm4 * o2);
				if (p1 >= 1.0f) {
					p1 -= (float)(int)p1;
					p2 = 0.0f;                                /* the sync */
				} else {
					p1 += p1 < 0.0f ? 1.0f : 0.0f;
					p2 += i2;
					p2 -= p2 >= 1.0f ? 1.0f : 0.0f;
				}
				float tri = p1 < 0.5f ? 4.0f * p1 - 1.0f : 3.0f - 4.0f * p1;
				float l1;
				ksvf_run(&nf, &s1, &s2, 0.55f * o2 + 0.45f * tri, &l1);
				acc += l1;
			}
			att = att < 1.0f ? att + 0.004f * KRAT : 1.0f;          /* (a 10 ms attack) */
			float y = acc * na * att * (os == 2 ? 0.4f : 0.8f) * KT[KT_L_NORD];
			dc += KP(0.008f) * (y - dc);                      /* its offset (the FM runs it backwards) */
			A[i] += (y - dc) * KE(5);
			na *= k->n_ak;
		}
		k->n_p1 = p1; k->n_p2 = p2; k->n_bp1 = s1; k->n_lo1 = s2;
		k->n_a = na; k->n_att = att; k->n_dc = dc;
	}
	/* EPS crunch roll (its retriggers land on the millisecond) */
	if (k->r_i < k->r_n || k->rs.b > 1e-3f || k->rs.n > 1e-3f) {
		any = 1;
		if (k->r_i < k->r_n) {
			k->r_t -= (float)KSUB / KSR;
			if (k->r_t <= 0.0f) {
				int h = k->r_i++;
				const float fr = (float)h / (float)(k->r_n > 1 ? k->r_n - 1 : 1);
				float gain = k->r_mode == 1 ? 0.2f + 0.8f * fr * fr : k->r_mode == 3 ? 0.8f - 0.3f * fr : 0.55f + 0.45f * (1.0f - fr);
				float semis = k->r_semis + (float)kit_deg(k->r_dir * h * (k->r_mode == 3 ? 2 : 1));
				while (semis > 14.0f)
					semis -= 12.0f;                           /* (folded back into the snare's range) */
				while (semis < -10.0f)
					semis += 12.0f;
				if (k->r_mode == 3)                           /* the harp: ringing, little noise, clean */
				{
					kit_snare_hit(&k->rs, semis, 1.0f, 1.0f, 0.0f, k->r_vel * gain);
					k->rs.n *= 0.12f;                         /* (mostly its body: tuned) */
					k->rs.bk = kexp(0.5f);
				}
				else
					kit_snare_hit(&k->rs, semis, 0.3f, 10.0f, 0.3f, k->r_vel * gain);
				float gap = k->r_gap * (1.0f - k->r_accel * 0.8f * fr);
				if (h + 1 == k->r_split)
					gap *= 3.0f;                              /* the pause that splits it */
				k->r_t += gap;
			}
		}
		float buf[KSUB] = {0};
		kit_snare_block(&k->rs, buf, 1.0f, 0);
		float acc = k->r_acc, held = k->r_held;
		const float hold = k->r_hold, q = k->r_q, iq = 1.0f / k->r_q;
		for (uint32_t i = 0; i < KSUB; i++) {
			acc += 1.0f;
			if (acc >= hold) {                            /* the sampler's rate and bits */
				acc -= hold;
				held = (float)(int32_t)(buf[i] * q) * iq;
			}
			A[i] += held * 0.6f * (k->r_tom ? KT[KT_L_TOMS] * KE(7) : KT[KT_L_ROLL] * KE(6));
		}
		k->r_acc = acc; k->r_held = held;
	}
	/* (the chorus sits out with the colour under the load governor) */
	const int cho = KT[KT_CHO_MIX] > 0.0f && (gov_cap >= FM_CAP || kit_solo);
	kit_side_on = (uint8_t)(cho && KT[KT_CHO_WIDE] > 0.0f);
	if (cho)
		kit_chorus(A);
	if (KT[KT_COL] >= 0.5f && (any || k->c_tail > 1e-4f)) {
		kit_color(c, A);
		any = 1;
	}
	return any;
}

/* ---- into the channel: the kit at its rate raised to 24 kHz (straight lines between its samples,
 * a gentle low-pass taking the edge off the images) ---- */
static int kit_render(chan *c, float *A, float *B)
{
	float b[KSUB];
	for (uint32_t i = 0; i < KSUB; i++)
		b[i] = 0.0f;
	if (!kit_render12(c, b)) {
		c->kt.up_prev = 0.0f;
		return 0;
	}
	for (uint32_t i = 0; i < KSUB; i++)
		b[i] *= KT[KT_OUT];
#if KIT_KSUB == 24
	for (uint32_t i = 0; i < VSUB; i++)
		A[i] += b[i];
#elif KIT_KSUB * 2 == 24
	float p = c->kt.up_prev, lp = c->kt.up_lp;
	for (uint32_t i = 0; i < KSUB; i++) {
		lp += 0.62f * (0.5f * (p + b[i]) - lp);
		A[2 * i] += lp;
		lp += 0.62f * (b[i] - lp);
		A[2 * i + 1] += lp;
		p = b[i];
	}
	c->kt.up_prev = p;
	c->kt.up_lp = lp;
#elif KIT_KSUB * 3 == 48
	/* 16 to 24 kHz: three out for every two in (at 2/3 and 1/3 of the way, then on the sample) */
	float p = c->kt.up_prev, lp = c->kt.up_lp;
	for (uint32_t i = 0, j = 0; i < KSUB; i += 2, j += 3) {
		const float b0 = b[i], b1 = b[i + 1];
		lp += 0.62f * (p + 0.6667f * (b0 - p) - lp);
		A[j] += lp;
		lp += 0.62f * (b0 + 0.3333f * (b1 - b0) - lp);
		A[j + 1] += lp;
		lp += 0.62f * (b1 - lp);
		A[j + 2] += lp;
		p = b1;
	}
	c->kt.up_prev = p;
	c->kt.up_lp = lp;
#else
	float p = c->kt.up_prev, lp = c->kt.up_lp;
	for (uint32_t j = 0; j < VSUB; j++) {
		float pos = (float)(j + 1u) * ((float)KSUB / (float)VSUB) - 1.0f;   /* (in b; -1 is the last block's) */
		int i0 = pos < 0.0f ? -1 : (int)pos;
		float f = pos - (float)i0, a0 = i0 < 0 ? p : b[i0], a1 = b[i0 + 1 < (int)KSUB ? i0 + 1 : (int)KSUB - 1];
		lp += 0.62f * (a0 + f * (a1 - a0) - lp);
		A[j] += lp;
	}
	c->kt.up_prev = b[KSUB - 1];
	c->kt.up_lp = lp;
#endif
	/* the two sides: the kit in both at half (a stereo voice's level), the chorus's spread added to
	 * the left and taken from the right (brought up from the kit's rate like the rest) */
	static float sp_prev;
	if (kit_side_on) {
		const float o = 0.5f * KT[KT_OUT];
		float sd[VSUB];
#if KIT_KSUB * 3 == 48
		float p = sp_prev;                         /* (16 to 24 kHz: three for every two, as above) */
		for (uint32_t i = 0, j = 0; i < KSUB; i += 2, j += 3) {
			const float b0 = kit_side[i], b1 = kit_side[i + 1];
			sd[j] = p + 0.6667f * (b0 - p);
			sd[j + 1] = b0 + 0.3333f * (b1 - b0);
			sd[j + 2] = b1;
			p = b1;
		}
#else
		for (uint32_t j = 0; j < VSUB; j++)
			sd[j] = kit_side[j * KSUB / VSUB];
#endif
		const float dk = kit_cho_duck;
		for (uint32_t j = 0; j < VSUB; j++) {
			const float m = 0.5f * dk * A[j], d = sd[j] * o * dk;
			A[j] = m + d;
			B[j] = m - d;
		}
		sp_prev = kit_side[KSUB - 1];
	} else {
		for (uint32_t j = 0; j < VSUB; j++)
			B[j] = A[j] = 0.5f * A[j];
		sp_prev = 0.0f;
	}
	return 1;
}
