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

static uint32_t kit_r = 0x6D2B79F5u;
static inline float kn(void)
{
	kit_r = kit_r * 1664525u + 1013904223u;            /* (an LCG: its top bits are plenty for noise) */
	return (float)(int32_t)kit_r * (1.0f / 2147483648.0f);
}
static inline float ku(void) { return 0.5f + 0.5f * kn(); }
static inline float ksat(float x) { x = clampf(x, -3.0f, 3.0f); return x * (27.0f + x * x) / (27.0f + 9.0f * x * x); }
static inline float kexp(float t_s) { return t_s > 0.0f ? fexp2(-6.644f / (t_s * VSR)) : 0.0f; }   /* down 40 dB in t seconds */

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
/* the 808's six squares (Hz), as phase steps at 24 kHz */
#define KM(f) ((uint32_t)((f) * 4294967296.0 / 24000.0 + 0.5))
static const uint32_t K_METAL[6] = {KM(205.3), KM(304.4), KM(369.6), KM(522.7), KM(540.0), KM(800.0)};
static const float K_RAT[4] = {1.41f, 3.5f, 7.07f, 0.5f};
/* the Hilbert pair: two chains of all-passes (c - z^-2) / (1 - c z^-2), the first a sample later,
 * 90 degrees apart within 0.5 degree from 600 Hz to 11.5 kHz (the other sideband 48 dB down):
 * two sections each are enough for the hat's band (fitted for 24 kHz) */
static const float K_HA[2] = {0.4111137f, 0.9009403f};
static const float K_HB[2] = {0.1288429f, 0.6854797f};

static void kit_snare_hit(kit_snare *s, float semis, float dec, float nu, float drive, float vel)
{
	float k = fexp2(semis * (1.0f / 12.0f));
	s->e1 = 185.0f * k * (6.2832f / VSR);
	s->e2 = 330.0f * k * (6.2832f / VSR);
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
	s->lpk = clampf((3000.0f + nu * 800.0f) * (6.2832f / VSR), 0.05f, 0.95f);
	s->nl = (0.3f + nu * (0.8f / 15.0f)) * 1.6f;
	s->drive = drive;
}
/* a block of the snare, added into A at gain g */
static void kit_snare_block(kit_snare *s, float *A, float g)
{
	if (s->b < 1e-3f && s->n < 1e-3f)
		return;
	const float d = 1.0f + s->drop, e1 = s->e1 * d, e2 = s->e2 * d;
	s->drop *= s->dk;
	float x1 = s->x1, y1 = s->y1, x2 = s->x2, y2 = s->y2, b = s->b, n = s->n, hp = s->hp, l1 = s->l1, l2 = s->l2, lo = s->lo;
	const float bk = s->bk, nk = s->nk, lpk = s->lpk, nl = s->nl, dr = s->drive;
	const float dg = 1.0f + 5.0f * dr, lg = 4.0f * dr, og = g / (1.0f + 0.7f * dr);
	for (uint32_t i = 0; i < VSUB; i++) {
		x1 += e1 * y1;
		y1 -= e1 * x1;
		x2 += e2 * y2;
		y2 -= e2 * x2;
		float body = (x1 * 0.7f + x2 * 0.45f) * b;
		float w = kn();
		hp += 0.33f * (w - hp);                         /* ~1.5 kHz high-pass */
		l1 += lpk * (w - hp - l1);
		l2 += lpk * (l1 - l2);
		float y = body + l2 * n * nl;
		b *= bk;
		n *= nk;
		if (dr > 0.0f) {                                /* the phono input: lows up, then crunch */
			lo += 0.028f * (y - lo);
			float u = clampf((y + lo * lg) * dg, -1.5f, 1.5f);
			A[i] += (u - 0.148148f * u * u * u) * og;   /* (a cubic: flat at 1.5) */
		} else
			A[i] += y * g;
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

/* audition: only these lanes sound (and each one's own pattern runs whatever CHARACTER is) */
static uint32_t kit_solo;
void gl_kit_solo(int lanes) { kit_solo = (uint32_t)lanes; }
#define KSOLO(l) (kit_solo & (1u << (l)))

/* ---- a hit on a lane, its sound set from the channel's controls ---- */
static void kit_fire(chan *c, int ch, int lane, float vel)
{
	if (kit_solo && !KSOLO(lane))
		return;
	kit_state *k = &c->kt;
	const float t = eff_tone(ch), l = P.ch[ch][CP_LENGTH], x = P.ch[ch][CP_CHAR];
	const float reg = (float)(stepped(P.ch[ch][CP_REGISTER], 5) - 2), oct = fexp2(reg * 0.5f);
	notes_fired++;
	c->glow = vel;
	/* short by default; an accent (velocity 1: the phrase's downbeat, the odd tissue stroke, a
	 * hard MIDI hit) rings long */
	const int big = vel >= 0.99f;
	const float dm = big ? 1.5f : 0.6f;
	const float root = (float)chord_pc[0];              /* the clank plays the chord's root */
	switch (lane) {
	case 0: {   /* canvas kick: tension (REGISTER), damping (LENGTH), firmness (TONE), slack (CHARACTER) */
		float f0 = clampf(72.0f * oct, 40.0f, 140.0f) * (1.0f + 0.02f * kn());
		float damp = 0.8f - 0.6f * l, firm = 0.2f + 0.6f * t, slack = 0.1f + 0.7f * x;
		float tau1 = (0.7f - damp * 0.6f) * (big ? 1.3f : 0.8f);
		for (int m = 0; m < 5; m++) {
			k->k_e[m] = f0 * K_MEM[m] * (6.2832f / VSR);
			if (k->k_x[m] * k->k_x[m] + k->k_y[m] * k->k_y[m] < 0.5f)
				k->k_x[m] = 0.0f, k->k_y[m] = 1.0f;
			k->k_amp[m] = vel * K_MEMA[m] * fexp2(-(float)m * (1.0f - firm) * 0.72f) * (1.0f + 0.3f * kn());
			k->k_dec[m] = kexp(tau1 / fexp2(0.8f * flog2(K_MEM[m])));
		}
		k->k_glide = slack * 0.25f;
		k->k_gk = fexp2(-1.4427f / (0.03f * 1000.0f));    /* (once a millisecond) */
		k->k_att = 0.0f;
		k->k_ai = 1.0f / ((0.008f - firm * 0.007f) * VSR);
		k->k_nz = vel * 0.75f;
		k->k_nzk = kexp(0.014f - firm * 0.011f);
		k->k_lpk = clampf((1500.0f + firm * 4000.0f) * (6.2832f / VSR), 0.05f, 0.9f);
		pump_tgt = 1.0f;                                   /* Playworld's pump follows the kick */
		break;
	}
	case 1:     /* R-8 snare: pitch, decay, nuance, feel; crunch rising with CHARACTER */
	case 7:     /* (7: a ghost, quieter and shorter) */
	case 9: {   /* (9: squashed: an accent pitched down 4 to 7 semitones, driven hard) */
		float feel = 2.0f + 4.0f * x;
		float semis = reg * 3.0f + kn() * feel * 0.06f, dec = clampf((0.1f + 0.6f * l) * dm * (1.0f + kn() * feel * 0.03f), 0.0f, 1.0f);
		float nu = clampf(4.0f + 11.0f * t + kn() * feel * 0.4f, 0.0f, 15.0f);
		if (lane == 9)
			kit_snare_hit(&k->sn, semis - 4.0f - 3.0f * ku(), clampf(dec * 1.4f, 0.0f, 1.0f), nu * 0.6f, 1.0f, vel);
		else
			kit_snare_hit(&k->sn, semis, lane == 7 ? dec * 0.4f : dec, nu, 0.8f * x, lane == 7 ? vel * 0.35f : vel);
		break;
	}
	case 2:     /* metal hat: decay (LENGTH; long when open), shift at the hit and at the end, tone */
	case 6: {   /* (6: open) */
		/* (CHARACTER reaches only half the shift it had: past that it turned sour) */
		const float xs = x * 0.5f, open = lane == 6 ? 1.0f : 0.0f;
		float dec = expmap(l, 0.025f, 0.3f) * dm * (lane == 6 ? 3.0f : 1.0f);
		dec = dec > 1.2f ? 1.2f : dec;
		/* the longer it rings, the darker and quieter: a long hat sits back, muted */
		float dark = clampf((dec - 0.05f) * 5.0f, 0.0f, 1.0f);
		k->h_a = vel * (1.0f - 0.35f * dark);
		k->h_k = kexp(dec);
		/* every hit its own: the squares' phases scattered, the band placed anew and swept down
		 * from above it in the first 15 ms (as a struck cymbal's top falls away), the shift's start
		 * moved a little */
		for (int m = 0; m < 6; m++)
			k->h_ph[m] += (uint32_t)(int32_t)(kn() * 2147483647.0f);
		k->h_fc0 = (4000.0f + 6000.0f * t) * (0.8f + 0.45f * ku());
		k->h_fenv = 0.5f + 0.7f * ku();
		k->h_s0 = (60.0f + 240.0f * xs) * (0.8f + 0.4f * ku());
		k->h_s1 = -60.0f * xs + 20.0f * kn();
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
		break;
	}
	case 3: {   /* DX100 clank: ratio (CHARACTER), carrier (REGISTER), index (TONE), decay (LENGTH) */
		float fc = 440.0f * fexp2((36.0f + root - 69.0f) / 12.0f) * oct * (1.0f + 0.002f * kn());   /* C2-B2 */
		k->d_ic = hz_inc(fc);
		k->d_im = hz_inc(fc * K_RAT[stepped(x, 4)]);
		k->d_index = (1.5f + 5.0f * t) * IDX_SCALE;
		k->d_ik = kexp((0.05f + 0.3f * l) * dm * 0.35f);
		k->d_a = vel;
		k->d_k = kexp((0.06f + 0.35f * l) * dm);
		k->d_fb = (0.05f + 0.25f * x) * 3.1416f;
		k->d_grit = x > 0.5f;
		break;
	}
	case 4: {   /* tissue: stroke length (LENGTH), crackle density (CHARACTER), brightness (TONE), rumble */
		k->t_len = big ? 0.25f + 0.6f * l : 0.04f + 0.12f * l;   /* (held over MIDI: until let go) */
		k->t_t = 0.0f;
		k->t_a = vel;
		k->t_prob = (300.0f + 1200.0f * x) / VSR;
		k->t_fc = 1500.0f + 5500.0f * t;
		k->t_prox = 0.2f + 0.4f * x;
		k->t_w0 = 2.0f + 3.0f * ku();
		k->t_w1 = 0.7f + ku();
		k->t_ph0 = ku();
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
			k->n_f1 = kit_hz(36 + root_pc + kit_deg(at + d)) * oct;
			if (k->n_f1 > 140.0f * oct)
				k->n_f1 *= 0.5f;
		}
		k->n_sweep = (12.0f + 24.0f * x) * (1.0f + kn() * (0.1f + 0.2f * x) * 0.35f);
		k->n_fm = (0.08f + 0.25f * x) * (1.0f + kn() * 0.1f);
		k->n_md = fexp2(-1.4427f / (0.04f * 1000.0f));   /* (once a millisecond) */
		k->n_menv = 1.0f;
		k->n_cut = 350.0f * fexp2(3.0f * t);
		k->n_a = vel;
		k->n_ak = kexp((0.25f + 0.9f * l) * dm);
		k->n_att = 0.0f;
		k->n_p1 = k->n_p2 = 0.0f;
		break;
	}
	case 8: {   /* EPS crunch roll: retriggers, acceleration, pitch step; rate and bits from CHARACTER */
		float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		k->r_n = (uint8_t)(4 + (int)(8.0f * x));
		k->r_i = 0;
		k->r_gap = step_s * 0.5f;
		k->r_accel = 0.3f + 0.5f * x;
		k->r_t = 0.0f;
		k->r_vel = vel;
		k->r_hold = clampf(VSR / (22000.0f - 14000.0f * x), 1.0f, 4.0f);
		k->r_q = fexp2(12.0f - 6.0f * x);
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
	for (int i = 0; i < 8; i++)
		if (k->q_t[i] <= 0.0f) {
			k->q_t[i] = t_s;
			k->q_v[i] = vel;
			k->q_l[i] = (uint8_t)lane;
			return;
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
 * downtempo at 85 in sixteenths (unit 2). Swing delays the off slots. ---- */
typedef struct { const char *k, *s, *h; float swing; uint8_t unit; } kit_groove;
static const kit_groove KG[4] = {
	{"x.........o.....x.....o...o.....", "....x.......x.......x.......x..g",
	 "..o.g.o...o.g.o...o.g.o.g.o...O.", 0.35f, 1},                              /* two-step */
	{"x......o..o.....x.....o...o..o..", "........x.............g.s.....g.",
	 "o.o.o.t.o.o.o.o.o.o.t.o.o.o.t.o.", 0.0f, 1},                               /* drill */
	{"x.x.......xx....x.x.......x.....", "....x..g.x..x..g....x..g.x....s.",
	 "o.o.o.o.o.o.o.o.o.o.o.o.o.o.o.O.", 0.1f, 1},                               /* jungle */
	{"x...........o...........o.......", "........x...............x.....g.",
	 "o...o...o...o...o...o...o...o.O.", 0.3f, 2},                               /* downtempo */
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

/* ---- the lanes ---- */
static void kit_step(chan *c, int ch, uint32_t step)
{
	const float h = eff_activity(ch), x = P.ch[ch][CP_CHAR];
	const int s16 = (int)(step % 16u);
	/* the load governor stepping in (a block past 88 %): the core plays on, the colour waits */
	const int lean = gov_cap < FM_CAP && !kit_solo;
	const int phrase = (int)(step % 64u);                 /* four bars: the accents land here */
	const float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]) * (float)(GL_CHX_DIV(P.chx[ch]) + 1u);
	const int gv = (int)GL_CHX_IVL(P.chx[ch]);
	if (gv >= 1 && gv <= 4) {
		/* a groove: this step's two slots */
		const kit_groove *g = &KG[gv - 1];
		for (int j = 0; j < 2; j++) {
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
			if (es == 'x' && phrase >= 56 && ku() < 0.35f)
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
		int kk = 1 + (int)(h * 3.0f + 0.5f), sk = 1 + (int)(h * 2.0f + 0.5f), hk = 3 + (int)(h * 6.0f + 0.5f);
		if (euclid_hit(kk, 16, s16))
			kit_fire(c, ch, 0, phrase == 0 ? 1.0f : acc * (0.85f + 0.12f * ku()));
		if (euclid_hit(sk, 16, (s16 + 12) % 16)) {
			/* the phrase's last snare an accent, often squashed; now and then one on the 4 */
			int sq = (phrase == 60 && ku() < 0.5f) || (s16 == 12 && ku() < 0.12f);
			kit_fire(c, ch, sq ? 9 : 1, sq ? 0.95f : phrase == 60 ? 1.0f : 0.82f + 0.13f * ku());
		} else if (!lean && h > 0.25f && (s16 & 1) && ku() < 0.22f * h)
			kit_fire(c, ch, 7, 0.8f);                    /* a ghost note */
		int s12 = (int)(step % 12u);
		if (!lean && s16 == 12 && ((step >> 4) & 1u) && ku() < 0.35f * h)
			kit_trip(c, ch, 6, step_s, 0.6f);            /* the bar's last beat in triplets */
		else if (euclid_hit(hk, 12, s12)) {
			/* the open hat: past 0.55, once in a while, on the off-beat */
			int op = s12 == 6 && (KSOLO(6) || (x > 0.55f && ku() < 0.25f));
			kit_fire(c, ch, op ? 6 : 2, op ? 0.9f : (s12 & 1 ? 0.55f : 0.8f) * (0.85f + 0.12f * ku()));
		}
	}
	if (form_fill && s16 >= 12)
		kit_fire(c, ch, 1, 0.7f + 0.2f * ku());          /* Playworld's fill */
	if (lean)
		return;
	int ck = 1 + (int)(h * 3.0f + 0.5f);
	if (euclid_hit(ck, 7, (int)((step + 2u) % 7u)))
		kit_fire(c, ch, 3, 0.6f + 0.3f * ku());
	if ((x > 0.33f || KSOLO(4)) && euclid_hit(h > 0.5f ? 2 : 1, 10, (int)((step + 5u) % 10u)))
		kit_fire(c, ch, 4, ku() < 0.15f ? 1.0f : 0.7f + 0.25f * ku());   /* now and then a long stroke */
	if ((x > 0.4f || KSOLO(5)) && euclid_hit(h > 0.6f ? 2 : 1, 13, (int)((step + 9u) % 13u)))
		kit_fire(c, ch, 5, 0.7f + 0.25f * ku());
	if ((x > 0.6f || KSOLO(8)) && (step % (KSOLO(8) ? 16u : 64u)) == (KSOLO(8) ? 4u : 56u) && ku() < 0.5f + 0.5f * h)
		kit_fire(c, ch, 8, 0.8f);                        /* a roll into the next phrase */
}

/* ---- the sound, 24 kHz, mono into the channel ---- */
static int kit_render(chan *c, float *A)
{
	kit_state *k = &c->kt;
	int any = 0;
	/* the hits waiting off the grid */
	for (int i = 0; i < 8; i++)
		if (k->q_t[i] > 0.0f) {
			k->q_t[i] -= (float)VSUB / VSR;
			if (k->q_t[i] <= 0.0f) {
				k->q_t[i] = 0.0f;
				kit_fire(c, (int)(c - CH), k->q_l[i], k->q_v[i]);
			}
		}
	/* canvas kick */
	if (k->k_amp[0] > 1e-3f || k->k_nz > 1e-3f) {
		any = 1;
		/* the five modes, recursive sines (the slack's glide once a block), into A */
		const float g = 1.0f + k->k_glide;
		k->k_glide *= k->k_gk;
		for (int m = 0; m < 5; m++) {
			if (k->k_amp[m] < 1e-3f)
				continue;
			float x = k->k_x[m], y = k->k_y[m], a = k->k_amp[m] * 1.3f;
			const float e = k->k_e[m] * g, dk = k->k_dec[m];
			for (uint32_t i = 0; i < VSUB; i++) {
				x += e * y;
				y -= e * x;
				A[i] += a * x;
				a *= dk;
			}
			k->k_x[m] = x; k->k_y[m] = y; k->k_amp[m] = a * (1.0f / 1.3f);
		}
		/* the fingertip: the attack's noise (the modes' attack ramp is in the noise's shadow) */
		if (k->k_nz > 1e-3f) {
			float lp = k->k_lp, nz = k->k_nz;
			const float lk = k->k_lpk, nzk = k->k_nzk;
			for (uint32_t i = 0; i < VSUB; i++) {
				lp += lk * (kn() * nz - lp);
				nz *= nzk;
				A[i] += lp * 0.6f;
			}
			k->k_lp = lp; k->k_nz = nz;
		}
	}
	/* R-8 snare (and its ghosts) */
	if (k->sn.b > 1e-3f || k->sn.n > 1e-3f) {
		any = 1;
		kit_snare_block(&k->sn, A, 0.9f);
	}
	/* metal hat, frequency-shifted */
	if (k->h_a > 1e-3f) {
		any = 1;
		const float dt = 1.0f / VSR;
		/* the band: placed per hit, swept down from above in the first ms, settling lower on a
		 * long hat; the low-pass after the shift closes as it rings */
		const float prog = k->h_len > 0.0f ? clampf(k->h_t / k->h_len, 0.0f, 1.0f) : 1.0f;
		const ksvf hf = ksvf_set(ktan(k->h_fc0 * (1.0f + k->h_fenv) * (1.0f - (1.0f - k->h_f) * prog) / VSR), 0.8f);
		k->h_fenv *= 0.936f;                              /* (15 ms) */
		k->h_lpk += (k->h_lpe - k->h_lpk) * 0.025f;        /* (about 40 ms) */
		/* the shift's frequency (it slides over the ring), as a turn per sample, once a block */
		float f = k->h_t < k->h_len ? k->h_s0 + (k->h_s1 - k->h_s0) * k->h_t / k->h_len : k->h_s1;
		k->h_t += dt * VSUB;
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
		for (uint32_t i = 0; i < VSUB; i++) {
			/* the squares: each one's top bit, counted */
			ph0 += K_METAL[0]; ph1 += K_METAL[1]; ph2 += K_METAL[2];
			ph3 += K_METAL[3]; ph4 += K_METAL[4]; ph5 += K_METAL[5];
			int n = (int)((ph0 >> 31) + (ph1 >> 31) + (ph2 >> 31) + (ph3 >> 31) + (ph4 >> 31) + (ph5 >> 31));
			float s = (float)(3 - n) * (1.0f / 3.0f);
			/* the high band (its body) and a little of the low metal */
			float lp, bp = ksvf_run(&hf, &bp1, &lo, s, &lp);
			hh += 0.6f * (bp - hh);
			l2 += 0.36f * (s - l2);
			l3 += 0.36f * (l2 - l3);
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
			A[i] += hl * 2.8f;
			float nc = pc * cr - ps * sr;                 /* the phasor turns */
			ps = pc * sr + ps * cr;
			pc = nc;
		}
		k->h_ph[0] = ph0; k->h_ph[1] = ph1; k->h_ph[2] = ph2; k->h_ph[3] = ph3; k->h_ph[4] = ph4; k->h_ph[5] = ph5;
		k->h_bp = bp1; k->h_lo = lo; k->h_hp = hh; k->h_l2 = l2; k->h_l3 = l3; k->h_a = ha; k->h_ad = ad;
		k->h_c = pc; k->h_s = ps; k->h_lp = hl;
	}
	/* DX100 clank */
	if (k->d_a > 1e-3f) {
		any = 1;
		for (uint32_t i = 0; i < VSUB; i++) {
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
			A[i] += y * 0.7f;
		}
	}
	/* tissue paper over a mic */
	if (k->t_a > 1e-4f && k->t_t < k->t_len + 0.08f) {
		any = 1;
		const float dt = 1.0f / VSR;
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
		const ksvf tf = ksvf_set(ktan(k->t_fc * fexp2(-1.5f * fall) / VSR), 1.25f);
		float press = 0.6f + 0.4f * sin_u((uint32_t)((k->t_w0 * t + k->t_ph0) * 4294967296.0f)) * sin_u((uint32_t)((k->t_w1 * t) * 4294967296.0f));
		k->t_t += dt * VSUB;
		const float ep = e * press, pr = k->t_prob * press, ga = 1.5f * k->t_a, gr = k->t_prox * 4.0f * k->t_a;   /* (quieter) */
		float tc = k->t_c, tb = k->t_b, tl = k->t_lo, th = k->t_hp, r1 = k->t_r1, r2 = k->t_r2;
		for (uint32_t i = 0; i < VSUB; i++) {
			float w = kn(), u = 0.5f + 0.5f * w;
			if (u < pr) {                                 /* a crackle: its size and sign from the same draw */
				float a = u / pr;
				tc += (kit_r & 1u ? -1.0f : 1.0f) * a * a * a;
			}
			tc *= 0.9f;                                   /* ~0.4 ms */
			float lp, bp = ksvf_run(&tf, &tb, &tl, (tc * 1.2f + w * 0.12f) * ep, &lp);
			th += 0.1f * (bp - th);
			r1 += 0.0235f * (w * ep - r1);
			r2 += 0.0235f * (r1 - r2);
			A[i] += (bp - th) * ga + r2 * gr;
		}
		k->t_c = tc; k->t_b = tb; k->t_lo = tl; k->t_hp = th; k->t_r1 = r1; k->t_r2 = r2;
	} else
		k->t_a = 0.0f;
	/* Nord sync / FM hit, at twice the rate */
	if (k->n_a > 1e-3f) {
		any = 1;
		const int os = k->n_menv > 0.03f ? 2 : 1;          /* twice the rate while the sync sweeps */
		const float dt2 = 1.0f / ((float)os * VSR);
		/* the sweep and the filter move once a block (1 ms) */
		const float f2 = k->n_f1 * fexp2(k->n_sweep * k->n_menv * (1.0f / 12.0f));
		const float fc = clampf(k->n_cut * fexp2(2.0f * k->n_menv), 60.0f, 0.21f * (float)os * VSR);
		const ksvf nf = ksvf_set(ktan(fc * dt2), 0.7f);
		k->n_menv *= k->n_md;
		const float fm4 = k->n_fm * (0.3f + 0.7f * k->n_menv) * 4.0f, f1 = k->n_f1 * dt2, i2 = f2 * dt2;
		float p1 = k->n_p1, p2 = k->n_p2, s1 = k->n_bp1, s2 = k->n_lo1;
		float na = k->n_a, att = k->n_att, dc = k->n_dc;
		for (uint32_t i = 0; i < VSUB; i++) {
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
			att = att < 1.0f ? att + 0.05f : 1.0f;
			float y = acc * na * att * (os == 2 ? 0.4f : 0.8f);
			dc += 0.008f * (y - dc);                      /* its offset (the FM runs it backwards) */
			A[i] += y - dc;
			na *= k->n_ak;
		}
		k->n_p1 = p1; k->n_p2 = p2; k->n_bp1 = s1; k->n_lo1 = s2;
		k->n_a = na; k->n_att = att; k->n_dc = dc;
	}
	/* EPS crunch roll (its retriggers land on the millisecond) */
	if (k->r_i < k->r_n || k->rs.b > 1e-3f || k->rs.n > 1e-3f) {
		any = 1;
		if (k->r_i < k->r_n) {
			k->r_t -= (float)VSUB / VSR;
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
		float buf[VSUB] = {0};
		kit_snare_block(&k->rs, buf, 1.0f);
		float acc = k->r_acc, held = k->r_held;
		const float hold = k->r_hold, q = k->r_q, iq = 1.0f / k->r_q;
		for (uint32_t i = 0; i < VSUB; i++) {
			acc += 1.0f;
			if (acc >= hold) {                            /* the sampler's rate and bits */
				acc -= hold;
				held = (float)(int32_t)(buf[i] * q) * iq;
			}
			A[i] += held * 0.6f;
		}
		k->r_acc = acc; k->r_held = held;
	}
	return any;
}
