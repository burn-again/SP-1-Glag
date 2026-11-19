/* Glag: Playworld, a prototype. Included by glag_engine.c, before control().
 *
 * Things that shape a piece over time, outside the patch unless noted, each off by default:
 *   form      the piece moves through sections (intro, build, full, breakdown, peak) in one of
 *             three arrangements; builds open the filter, swell the space and end on a drum fill
 *   key       every few minutes the key moves: up a fifth, half-step lifts, the relative
 *             major / minor, or drops (down a whole step, down to the fourth, home)
 *   pump      the Pulse's kick ducks everything else and the space; past 70 % it also bends
 *             the notes down as it cuts through
 *   shape     the channel macros' shape: the glide, steps on each bar, or a long rise
 *   pingpong  the tape's heads bounce left and right (also a flag in the patch: saved with it)
 *   texture   a bed under the piece: rain, vinyl (with a warble), tape hiss or surf
 *   reverse   the reverb's tail played backwards, swelling (also a flag in the patch)
 *   autof     the DJ filter sweeping on its own, once every two bars
 */

static struct {
	float form, form_bars;
	int form_kind, form_init;
	float key_min;
	int key_kind, key_i;
	float key_t;
	float pump;
	int tex;
	float tex_lvl;
	float rev;
	float autof;
	int pp;
	int key_forced, shape_forced;
} W = {0.0f, 16.0f, 0, 0, 0.0f, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0};

void gl_world(int what, float v)
{
	switch (what) {
	case GW_FORM: W.form = clampf(v, 0.0f, 1.0f); W.form_init = 0; break;
	case GW_FORM_BARS: W.form_bars = v < 1.0f ? 1.0f : v; break;
	case GW_FORM_KIND: W.form_kind = (int)v % 3; break;
	case GW_KEY_MIN: W.key_min = v; W.key_t = 0.0f; W.key_forced = 1; break;
	case GW_KEY_KIND: W.key_kind = (int)v; break;
	case GW_PUMP: W.pump = clampf(v, 0.0f, 1.0f); break;
	case GW_SHAPE:
		for (int c = 0; c < GL_NCH; c++)
			mac_shape[c] = (uint8_t)v;
		W.shape_forced = 1;
		break;
	case GW_PINGPONG: W.pp = v > 0.5f; break;
	case GW_TEXTURE: W.tex = (int)v; break;
	case GW_TEX_LEVEL: W.tex_lvl = clampf(v, 0.0f, 1.0f); break;
	case GW_REVERSE: W.rev = clampf(v, 0.0f, 1.0f); break;
	case GW_AUTOF: W.autof = clampf(v, 0.0f, 1.0f); break;
	default: break;
	}
}

/* ---- song form ----
 * One cycle of 4 x form_bars bars. Each track has its own arc, staggered by role: the held voices
 * are there throughout; the plucks come in, then the bass, then the drums, fading in over bars
 * while the whole grows busier. Then all four together; then they thin out in turn (drums, bass,
 * the plucks to a shadow) while the filter closes and the space opens; then a build, the tracks
 * back in one after another, the filter opening, the space swelling, a fill on its last bar; all
 * four again; and out, back to the held voices, where the cycle starts over.
 * Nothing jumps: every value is a smooth curve of where the song is, slewed again on top, the
 * density never goes past 100 % (no notes cut short to make room), and the filter stays one kind
 * of filter, gliding, for as long as the form is on. */
static inline float fminf_(float a, float b) { return a < b ? a : b; }
enum { ROLE_HELD, ROLE_PLUCK, ROLE_BASS, ROLE_DRUMS };
static int voice_role(int v)
{
	if (v == GV_PULSE)
		return ROLE_DRUMS;
	if (v == GV_BASS)
		return ROLE_BASS;
	if (v == GV_PAD || v == GV_DRONE || v == GV_FLUTE || v == GV_SWELL || v == GV_GONG)
		return ROLE_HELD;
	return ROLE_PLUCK;
}
/* keyframes (position 0..1, value), eased between with a smoothstep */
typedef struct { float x, v; } kf;
static float kf_at(const kf *k, int n, float x)
{
	if (x <= k[0].x)
		return k[0].v;
	for (int i = 1; i < n; i++)
		if (x <= k[i].x) {
			float t = (x - k[i - 1].x) / (k[i].x - k[i - 1].x);
			t = t * t * (3.0f - 2.0f * t);
			return k[i - 1].v + (k[i].v - k[i - 1].v) * t;
		}
	return k[n - 1].v;
}
/* each role's level through the cycle (the held voices stay) */
static const kf ARC_PLUCK[] = {{0.08f, 0.0f}, {0.17f, 1.0f}, {0.57f, 1.0f}, {0.64f, 0.35f}, {0.70f, 0.35f},
                               {0.75f, 1.0f}, {0.92f, 1.0f}, {0.99f, 0.0f}};
static const kf ARC_BASS[] = {{0.18f, 0.0f}, {0.27f, 1.0f}, {0.55f, 1.0f}, {0.61f, 0.0f}, {0.73f, 0.0f},
                              {0.78f, 1.0f}, {0.94f, 1.0f}, {0.995f, 0.0f}};
static const kf ARC_DRUMS[] = {{0.28f, 0.0f}, {0.36f, 1.0f}, {0.53f, 1.0f}, {0.58f, 0.0f}, {0.76f, 0.0f},
                               {0.81f, 0.55f}, {0.85f, 1.0f}, {0.93f, 1.0f}, {0.98f, 0.0f}};
static const kf ARC_ACT[] = {{0.0f, 0.65f}, {0.40f, 1.0f}, {0.55f, 1.0f}, {0.63f, 0.7f}, {0.72f, 0.7f},
                             {0.85f, 1.0f}, {0.94f, 1.0f}, {1.0f, 0.65f}};
static const kf ARC_FILT[] = {{0.58f, 0.47f}, {0.67f, 0.30f}, {0.72f, 0.30f}, {0.85f, 0.47f}};   /* the DJ filter: open is 0.47 */
static const kf ARC_SPACE[] = {{0.58f, 1.0f}, {0.66f, 1.2f}, {0.72f, 1.2f}, {0.84f, 1.4f}, {0.90f, 1.0f}};
#define NK(a) ((int)(sizeof a / sizeof a[0]))
static float form_pos(void)
{
	float cyc = 4.0f * W.form_bars;
	return fmodf_pos(((float)step_n + step_acc) / 16.0f, cyc) / cyc;
}
static void form_tick(float dt)
{
	if (W.form <= 0.0f) {
		form_fill = 0;
		form_space = 1.0f;
		form_filt = -1.0f;
		for (int c = 0; c < GL_NCH; c++)
			w_lvl[c] = w_act[c] = 1.0f;
		W.form_init = 0;
		return;
	}
	float x = form_pos(), f = W.form;
	/* a second pluck (or bass...) channel comes in a little after the first */
	int seen[4] = {0, 0, 0, 0};
	float k = W.form_init ? 1.0f - dcoef(0.6f, dt) : 1.0f;   /* a slew on top of the curves */
	W.form_init = 1;
	float act = kf_at(ARC_ACT, NK(ARC_ACT), x);
	/* the anchor: a held voice stays throughout; with none (EP, Bass, Pulse, Kalimba...), the
	 * first pluck does, so the song never starts or ends in silence */
	int anchor = -1;
	for (int c = 0; c < GL_NCH && anchor < 0; c++)
		if (P.ch[c][CP_LEVEL] > 0.0f && voice_role(CH[c].want) == ROLE_HELD)
			anchor = c;
	for (int c = 0; c < GL_NCH && anchor < 0; c++)
		if (P.ch[c][CP_LEVEL] > 0.0f && voice_role(CH[c].want) == ROLE_PLUCK)
			anchor = c;
	for (int c = 0; c < GL_NCH; c++) {
		int r = voice_role(CH[c].want), r0 = voice_role(CH[c].voice);
		if (c == anchor)
			r = r0 = ROLE_HELD;
		float lvl = 1.0f, l0 = 1.0f, off = 0.035f * (float)seen[r]++;
		float xs = fmodf_pos(x - off, 1.0f);
		const kf *arc[4] = {0, ARC_PLUCK, ARC_BASS, ARC_DRUMS};
		const int an[4] = {0, NK(ARC_PLUCK), NK(ARC_BASS), NK(ARC_DRUMS)};
		if (r)
			lvl = kf_at(arc[r], an[r], xs);
		if (r0 && r0 != r)
			l0 = kf_at(arc[r0], an[r0], xs);                  /* crossfading: the quieter */
		float l = 1.0f + f * (fminf_(lvl, l0) - 1.0f);
		float a = c == anchor ? 1.0f + f * (0.5f * (act + 1.0f) - 1.0f) : 1.0f + f * (act - 1.0f);
		w_lvl[c] += (l - w_lvl[c]) * k;
		w_act[c] += (a - w_act[c]) * k;
	}
	float sp = 1.0f + f * (kf_at(ARC_SPACE, NK(ARC_SPACE), x) - 1.0f);
	form_space += (sp - form_space) * k;
	form_filt = 0.47f - f * (0.47f - kf_at(ARC_FILT, NK(ARC_FILT), x));
	/* the fill: the bar before the build lands (x = 0.85) */
	float cyc = 4.0f * W.form_bars, bar = x * cyc, land = 0.85f * cyc;
	form_fill = bar >= land - 1.0f && bar < land;
}

/* ---- key travel: on the bar, once the time is up ---- */
static void key_tick(float dt)
{
	if (W.key_min <= 0.0f) {
		if (key_off || mode_swap) {
			key_off = 0;
			mode_swap = 0;
			harmony_update();
		}
		return;
	}
	W.key_t += dt * speed;
	if (W.key_t < W.key_min * 60.0f || (step_n & 15u) != 0u)
		return;
	W.key_t = 0.0f;
	W.key_i++;
	if (W.key_kind == 0)
		key_off = (key_off + 7) % 12;                          /* up a fifth, round the circle */
	else if (W.key_kind == 1) {
		static const int8_t LIFT[4] = {0, 1, 2, 0};            /* a half step, another, home */
		key_off = LIFT[W.key_i & 3];
	} else if (W.key_kind == 2)
		mode_swap = !mode_swap;                                /* the relative major / minor */
	else {
		static const int8_t DROP[4] = {0, -2, -5, 0};          /* down a whole step, to the fourth below, home */
		key_off = DROP[W.key_i & 3];
	}
	harmony_update();
}

/* ---- pump: the kick ducks the rest, ~ a dotted eighth to come back. Deeper the higher it goes
 * (up to -30 dB at full); past 70 % the notes bend down with it, up to a semitone ---- */
static float pump_gain(float dt, float bpm)
{
	if (W.pump <= 0.0f) {
		pbend = 1.0f;
		return 1.0f;
	}
	float rel = 0.375f * 60.0f / bpm * (1.0f + W.pump);
	if (pump_tgt > pump_env)
		pump_env += (pump_tgt - pump_env) * 0.35f;             /* ~3 ms in: no click */
	else
		pump_env = pump_tgt;
	pump_tgt *= dcoef(rel * 3.0f, dt);                         /* (dcoef: the time to fall 60 dB) */
	float depth = 0.6f * W.pump + 0.37f * W.pump * W.pump * W.pump;
	float bend = W.pump > 0.7f ? (W.pump - 0.7f) / 0.3f : 0.0f;
	pbend = 1.0f - 0.0561f * bend * pump_env;                  /* 0.0561: a semitone down */
	return 1.0f - depth * pump_env;
}

/* ---- textures, at 24 kHz into the dry mix ---- */
static uint32_t tx_r = 0x2545F491u;
static float tx_s[12];
static float tx_drop, tx_dk, tx_dpan, tx_lfo;
static float rn_f[2] = {0.3f, 0.5f}, rn_ft[2] = {0.3f, 0.5f};            /* rain: two moving bands */
static float sf_env[2], sf_ph[2] = {0.0f, 0.55f}, sf_per[2] = {9.0f, 11.0f}, sf_pan[2];   /* surf: two waves */
static inline float tx_noise(void)
{
	tx_r ^= tx_r << 13;
	tx_r ^= tx_r >> 17;
	tx_r ^= tx_r << 5;
	return (float)(int32_t)tx_r * (1.0f / 2147483648.0f);
}
static void texture_block(float *HL, float *HR, uint32_t n, float sr)
{
	if (W.tex <= 0 || W.tex_lvl <= 0.0f)
		return;
	float g = W.tex_lvl * W.tex_lvl * 0.25f, bt = (float)n / sr;
	float *s = tx_s;
	tx_lfo += bt / 20.0f;                                      /* tape hiss: a breath every ~20 s */
	tx_lfo -= tx_lfo >= 1.0f ? 1.0f : 0.0f;
	if (W.tex == 1) {   /* rain: the two bands wander, each to a new place every few seconds */
		for (int k = 0; k < 2; k++) {
			if (tx_noise() > 0.9993f)
				rn_ft[k] = 0.12f + 0.5f * (0.5f + 0.5f * tx_noise());
			rn_f[k] += (rn_ft[k] - rn_f[k]) * bt * 0.4f;
		}
	}
	if (W.tex == 4) {   /* surf: waves of their own lengths, rising, then a long wash back */
		for (int k = 0; k < 2; k++) {
			sf_ph[k] += bt / sf_per[k];
			if (sf_ph[k] >= 1.0f) {
				sf_ph[k] -= 1.0f;
				sf_per[k] = 7.0f + 8.0f * (0.5f + 0.5f * tx_noise());
				sf_pan[k] = 0.5f + 0.45f * tx_noise();
			}
			float x = sf_ph[k], tgt = x < 0.3f ? 0.5f - 0.5f * sin_u((uint32_t)(x / 0.3f * 2147483648.0f) + 0x40000000u) : 0.0f;
			if (tgt > sf_env[k])
				sf_env[k] = tgt;
			else
				sf_env[k] *= dcoef(sf_per[k] * 1.3f, bt);          /* the wash outlasts the wave */
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		float a = tx_noise(), b = tx_noise(), l = 0.0f, r = 0.0f;
		switch (W.tex) {
		case 1: {   /* rain: distant hiss through two slowly wandering bands, and drops close by */
			for (int k = 0; k < 2; k++) {
				float f = rn_f[k], q = 0.9f, x = k ? b : a;    /* a state-variable band-pass */
				float lo = s[2 * k], bp = s[2 * k + 1];
				lo += f * bp;
				float hi = x - lo - q * bp;
				bp += f * hi;
				s[2 * k] = lo;
				s[2 * k + 1] = bp;
			}
			l = (s[1] * 0.8f + s[3] * 0.35f) * 0.6f;
			r = (s[3] * 0.8f + s[1] * 0.35f) * 0.6f;
			if (tx_drop < 0.01f && (tx_r & 0xFFFu) < 3u) {       /* ~18 drops a second */
				tx_drop = 0.3f + 0.7f * (float)((tx_r >> 12) & 255u) / 255.0f;
				tx_dk = 0.992f - 0.006f * (float)((tx_r >> 20) & 15u) / 15.0f;
				tx_dpan = (float)((tx_r >> 24) & 255u) / 255.0f;
			}
			s[4] += 0.6f * (a - s[4]);
			float d = (a - s[4]) * tx_drop;
			tx_drop *= tx_dk;
			l += d * (1.0f - tx_dpan);
			r += d * tx_dpan;
			break;
		}
		case 2: {   /* vinyl: crackle, and a quieter surface (the warble is on the whole mix) */
			float c = 0.0f;
			if ((tx_r & 0x7FFu) < 1u)                            /* ~12 pops a second */
				c = (a > 0.0f ? 1.0f : -1.0f) * (0.4f + 0.6f * b * b);
			s[0] += 0.5f * (c - s[0]);
			s[1] += 0.25f * (a - s[1]);
			s[2] += 0.004f * (s[1] - s[2]);
			float hiss = (s[1] - s[2]) * 0.045f;
			l = (c - s[0]) * 1.4f + hiss;
			r = (c - s[0]) * 1.2f + hiss;
			break;
		}
		case 3: {   /* tape hiss: soft, rolled off above ~4.5 kHz, breathing slowly */
			s[0] += 0.08f * (a - s[0]);
			s[1] += 0.08f * (b - s[1]);
			s[2] += 0.7f * ((a - s[0]) - s[2]);
			s[3] += 0.7f * (s[2] - s[3]);
			s[4] += 0.7f * ((b - s[1]) - s[4]);
			s[5] += 0.7f * (s[4] - s[5]);
			float br = 0.8f + 0.2f * sin_u((uint32_t)(tx_lfo * 4294967296.0f));
			l = s[3] * 0.26f * br;
			r = s[5] * 0.26f * br;
			break;
		}
		default: {  /* surf: low noise for each wave, brighter as it breaks, moving along the shore */
			for (int k = 0; k < 2; k++) {
				float e = sf_env[k], kf = 0.02f + 0.13f * e * e, x = k ? b : a;
				s[2 * k] += kf * (x - s[2 * k]);
				s[2 * k + 1] += kf * (s[2 * k] - s[2 * k + 1]);
				float v = s[2 * k + 1] * (0.15f + 1.8f * e);
				float pn = clampf(sf_pan[k] + 0.3f * (sf_ph[k] - 0.5f), 0.05f, 0.95f);
				l += v * (1.0f - pn) * 1.4f;
				r += v * pn * 1.4f;
			}
			break;
		}
		}
		HL[i] += l * g;
		HR[i] += r * g;
	}
}

/* ---- the vinyl's warble: the whole mix through a short delay that wavers (wow and flutter) ---- */
#define WB_N 1024u                                   /* 21 ms at 48 kHz */
static int16_t wb_l[WB_N], wb_r[WB_N];
static uint32_t wb_w;
static float wb_wow, wb_flut, wb_drift, wb_dt;
static void warble_block(float *L, float *R, uint32_t n)
{
	float amt = W.tex == 2 ? W.tex_lvl : 0.0f;
	if (amt <= 0.0f)
		return;
	for (uint32_t i = 0; i < n; i++) {
		wb_wow += 0.55f / GL_SR;
		wb_wow -= wb_wow >= 1.0f ? 1.0f : 0.0f;
		wb_flut += 6.3f / GL_SR;
		wb_flut -= wb_flut >= 1.0f ? 1.0f : 0.0f;
		if ((wb_w & 4095u) == 0u)
			wb_dt = tx_noise();
		wb_drift += (wb_dt - wb_drift) * 0.00002f;
		float d = 384.0f + amt * (70.0f * sin_u((uint32_t)(wb_wow * 4294967296.0f)) +
		                          6.0f * sin_u((uint32_t)(wb_flut * 4294967296.0f)) + 60.0f * wb_drift);
		wb_l[wb_w & (WB_N - 1u)] = to16(L[i]);
		wb_r[wb_w & (WB_N - 1u)] = to16(R[i]);
		uint32_t k = (uint32_t)d;
		float f = d - (float)k;
		uint32_t a = (wb_w - k) & (WB_N - 1u), b = (wb_w - k - 1u) & (WB_N - 1u);
		L[i] = from16(wb_l[a]) + (from16(wb_l[b]) - from16(wb_l[a])) * f;
		R[i] = from16(wb_r[a]) + (from16(wb_r[b]) - from16(wb_r[a])) * f;
		wb_w++;
	}
}

/* ---- reverse: grains of the reverb's last 0.7 s played backwards, swelling ---- */
#define RV_N 16384u                                  /* 1.4 s at 12 kHz */
static int16_t rvb[RV_N];                            /* 32 KB, 16-bit like the ring */
static uint32_t rvb_w;
static float rvb_g[2], rvb_k[2], rvb_on[2], rvb_pl, rvb_pr, rvb_ll, rvb_lr;
static const float RV_G = 8000.0f;                   /* a grain: ~0.67 s (read and write part 2 grains: < RV_N) */
static float rev_amount(void) { return (P.space[GL_SP_FLAGS] & GL_FLAG_REVERSE) && W.rev <= 0.0f ? 0.7f : W.rev; }
static void reverse_block(float *WL, float *WR, uint32_t n)
{
	float rv = rev_amount();
	if (rv <= 0.0f)
		return;
	for (uint32_t i = 0; i < n; i += 2) {
		rvb[rvb_w & (RV_N - 1u)] = to16(0.25f * (WL[i] + WR[i] + WL[i + 1] + WR[i + 1]));
		rvb_w++;
		float o[2] = {0.0f, 0.0f};
		for (int k = 0; k < 2; k++) {
			if (rvb_g[k] <= 0.0f) {                           /* a grain starts half a grain after the other */
				float other = rvb_g[k ^ 1];
				if (other <= 0.0f || other < RV_G * 0.5f) {
					rvb_g[k] = RV_G;
					rvb_k[k] = (float)rvb_w;                     /* read back from here */
					rvb_on[k] = tx_noise() < 0.2f * rv ? 0.0f : 1.0f;   /* now and then: some sit out */
				}
				continue;
			}
			float t = 1.0f - rvb_g[k] / RV_G;                 /* 0 .. 1 through the grain */
			uint32_t at = (uint32_t)(rvb_k[k] - t * RV_G);
			/* swelling, and a gentle end (a sudden one clicked, the buzz you heard) */
			float env = t * t * (t < 0.8f ? 1.0f : 0.5f + 0.5f * sin_u((uint32_t)((t - 0.8f) / 0.2f * 2147483648.0f) + 0x40000000u));
			o[k] = from16(rvb[at & (RV_N - 1u)]) * env * rvb_on[k];
			rvb_g[k] -= 1.0f;
		}
		float g = 2.2f * rv;
		float l = (o[0] * 0.8f + o[1] * 0.3f) * g, r = (o[0] * 0.3f + o[1] * 0.8f) * g;
		/* back up to 24 kHz each side on its own (the old way mixed the sides every other sample:
		 * a buzz near 12 kHz), then a pole at ~3.5 kHz for what the 12 kHz steps leave */
		float l0 = 0.5f * (l + rvb_pl), r0 = 0.5f * (r + rvb_pr);
		rvb_pl = l;
		rvb_pr = r;
		rvb_ll += 0.6f * (l0 - rvb_ll);
		rvb_lr += 0.6f * (r0 - rvb_lr);
		WL[i] += rvb_ll;
		WR[i] += rvb_lr;
		rvb_ll += 0.6f * (l - rvb_ll);
		rvb_lr += 0.6f * (r - rvb_lr);
		WL[i + 1] += rvb_ll;
		WR[i + 1] += rvb_lr;
	}
}

/* ---- auto filter (and the form's builds): the DJ filter moved on its own ---- */
static void autof_tick(void)
{
	static float was = -1.0f;
	float v = -1.0f, base = P.g[GP_FILTER];
	int centre = base > 0.47f && base < 0.53f;
	if (W.autof > 0.0f) {
		float ph = ((float)(step_n & 31u) + step_acc) / 32.0f;
		float sw = 0.5f - 0.5f * sin_u((uint32_t)(ph * 4294967296.0f) + 0x40000000u);
		v = (centre ? 0.47f : base) - W.autof * (centre ? 0.27f : 0.3f) * sw;   /* from open, down to ~500 Hz */
	}
	/* the form's filter only when the DJ filter is open (a high-pass of yours is left alone); it
	 * is a low-pass at 18 kHz when "open", so it glides and never changes kind */
	if (form_filt >= 0.0f && (centre || base < 0.47f))
		v = v < 0.0f ? fminf_(form_filt, centre ? 0.47f : base) : fminf_(v, form_filt);
	if (v < 0.0f) {
		if (was >= 0.0f) {
			filter_setup(base);
			was = -1.0f;
		}
		return;
	}
	if (v != was) {
		filter_setup(v);
		was = v;
	}
}

/* ---- the patch's flags, every control tick ---- */
static void world_tick(void)
{
	uint8_t f = P.space[GL_SP_FLAGS];
	w_pingpong = W.pp || (f & GL_FLAG_PINGPONG);
	if (!W.key_forced) {             /* the piece's key travel: a move every 3 minutes */
		unsigned k = GL_FLAG_KEY(f);
		float want = k ? 3.0f : 0.0f;
		if (want != W.key_min)
			W.key_t = 0.0f;
		W.key_min = want;
		W.key_kind = k ? (int)k - 1 : 0;
	}
	if (!W.shape_forced)
		for (int c = 0; c < GL_NCH; c++)
			mac_shape[c] = (uint8_t)GL_FLAG_SHAPE(f);
}
