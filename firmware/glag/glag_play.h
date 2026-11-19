/* Glag: Playworld, a prototype. Included by glag_engine.c, before control().
 *
 * Eight things that shape a piece over time, outside the patch, each off by default:
 *   form      the piece moves through sections: a sparse intro, full, a breakdown (drums and bass
 *             out), full again; each section form_bars long
 *   key       every few minutes the key moves: up a fifth, half-step lifts, or to the relative
 *             major / minor (the same notes, a new home)
 *   pump      the Pulse's kick ducks everything else, and the reverb
 *   shape     the channel macros' shape: the glide, steps on each bar, or a long rise that lets go
 *   pingpong  the tape's heads bounce left and right, half the echo time apart
 *   texture   a bed under the piece: rain, vinyl, tape hiss or surf
 *   reverse   the reverb's tail played backwards, swelling, now and then
 *   autof     the DJ filter sweeping on its own, once every two bars
 */

static struct {
	float form, form_bars;
	float key_min;
	int key_kind, key_i, key_mode;
	float key_t;
	float pump;
	int tex;
	float tex_lvl;
	float rev;
	float autof;
} W = {0.0f, 16.0f, 0.0f, 0, 0, -1, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f};

void gl_world(int what, float v)
{
	switch (what) {
	case GW_FORM: W.form = clampf(v, 0.0f, 1.0f); break;
	case GW_FORM_BARS: W.form_bars = v < 1.0f ? 1.0f : v; break;
	case GW_KEY_MIN: W.key_min = v; W.key_t = 0.0f; break;
	case GW_KEY_KIND: W.key_kind = (int)v; break;
	case GW_PUMP: W.pump = clampf(v, 0.0f, 1.0f); break;
	case GW_SHAPE:
		for (int c = 0; c < GL_NCH; c++)
			mac_shape[c] = (uint8_t)v;
		break;
	case GW_PINGPONG: w_pingpong = v > 0.5f; break;
	case GW_TEXTURE: W.tex = (int)v; break;
	case GW_TEX_LEVEL: W.tex_lvl = clampf(v, 0.0f, 1.0f); break;
	case GW_REVERSE: W.rev = clampf(v, 0.0f, 1.0f); break;
	case GW_AUTOF: W.autof = clampf(v, 0.0f, 1.0f); break;
	default: break;
	}
}

/* ---- form: which section, and each channel's level and activity in it ---- */
enum { ROLE_HELD, ROLE_PLUCK, ROLE_RHYTHM };
static int voice_role(int v)
{
	if (v == GV_PULSE || v == GV_BASS)
		return ROLE_RHYTHM;
	if (v == GV_PAD || v == GV_DRONE || v == GV_FLUTE || v == GV_SWELL || v == GV_GONG)
		return ROLE_HELD;
	return ROLE_PLUCK;
}
static void form_tick(float dt)
{
	static const float LV[3][4] = {{1.0f, 1.0f, 1.0f, 1.0f},      /* held: always */
	                               {0.45f, 1.0f, 0.7f, 1.0f},     /* plucks: thin, full, thinner */
	                               {0.0f, 1.0f, 0.0f, 1.0f}};     /* drums and bass: in for the full parts */
	static const float AC[3][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {0.5f, 1.0f, 0.6f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
	int sec = (int)((float)(step_n / 16u) / W.form_bars) & 3;
	float k = 1.0f - dcoef(4.0f, dt);                          /* a section fades in over ~4 s */
	for (int c = 0; c < GL_NCH; c++) {
		int r = voice_role(CH[c].voice);
		float l = 1.0f + W.form * (LV[r][sec] - 1.0f), a = 1.0f + W.form * (AC[r][sec] - 1.0f);
		w_lvl[c] += (l - w_lvl[c]) * k;
		w_act[c] += (a - w_act[c]) * k;
	}
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
	} else
		mode_swap = !mode_swap;                                /* the relative major / minor */
	harmony_update();
}

/* ---- pump: the kick ducks the rest; ~ a dotted eighth to come back ---- */
static float pump_gain(float dt, float bpm)
{
	if (W.pump <= 0.0f)
		return 1.0f;
	float rel = 0.375f * 60.0f / bpm;
	if (pump_tgt > pump_env)
		pump_env += (pump_tgt - pump_env) * 0.35f;             /* ~3 ms in: no click */
	else
		pump_env = pump_tgt;
	pump_tgt *= dcoef(rel * 3.0f, dt);                         /* (dcoef: the time to fall 60 dB) */
	return 1.0f - 0.8f * W.pump * pump_env;
}

/* ---- textures, at 24 kHz into the dry mix ---- */
static uint32_t tx_r = 0x2545F491u;
static float tx_s[8];
static float tx_drop, tx_dk, tx_dpan, tx_lfo;
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
	float g = W.tex_lvl * W.tex_lvl * 0.25f;
	float *s = tx_s;
	tx_lfo += (float)n / sr / 10.0f;                           /* surf: a wave every ~10 s */
	tx_lfo -= tx_lfo >= 1.0f ? 1.0f : 0.0f;
	for (uint32_t i = 0; i < n; i++) {
		float a = tx_noise(), b = tx_noise(), l = 0.0f, r = 0.0f;
		switch (W.tex) {
		case 1: {   /* rain: a soft hiss of distant rain, and drops close by */
			s[0] += 0.35f * (a - s[0]);                          /* ~1.7 kHz low-pass ... */
			s[1] += 0.02f * (s[0] - s[1]);                       /* ... less its lows */
			s[2] += 0.35f * (b - s[2]);
			s[3] += 0.02f * (s[2] - s[3]);
			l = (s[0] - s[1]) * 0.5f;
			r = (s[2] - s[3]) * 0.5f;
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
		case 2: {   /* vinyl: crackle and a little surface noise */
			float c = 0.0f;
			if ((tx_r & 0x7FFu) < 1u)                            /* ~12 pops a second */
				c = (a > 0.0f ? 1.0f : -1.0f) * (0.4f + 0.6f * b * b);
			s[0] += 0.5f * (c - s[0]);
			s[1] += 0.25f * (a - s[1]);
			s[2] += 0.004f * (s[1] - s[2]);
			float hiss = (s[1] - s[2]) * 0.12f;
			l = (c - s[0]) * 1.4f + hiss;
			r = (c - s[0]) * 1.2f + hiss;
			break;
		}
		case 3: {   /* tape hiss: soft, high, breathing a little */
			s[0] += 0.08f * (a - s[0]);
			s[1] += 0.08f * (b - s[1]);
			float br = 0.85f + 0.15f * sin_u((uint32_t)(tx_lfo * 4.0f * 4294967296.0f));
			l = (a - s[0]) * 0.18f * br;
			r = (b - s[1]) * 0.18f * br;
			break;
		}
		default: {  /* surf: low noise swelling in and drawing back */
			float w = 0.5f - 0.5f * sin_u((uint32_t)(tx_lfo * 4294967296.0f) + 0x40000000u);
			float sw = w * w, kf = 0.03f + 0.12f * sw;
			s[0] += kf * (a - s[0]);
			s[1] += kf * (s[0] - s[1]);
			s[2] += kf * (b - s[2]);
			s[3] += kf * (s[2] - s[3]);
			l = s[1] * (0.25f + 1.6f * sw);
			r = s[3] * (0.25f + 1.6f * sw);
			break;
		}
		}
		HL[i] += l * g;
		HR[i] += r * g;
	}
}

/* ---- reverse: grains of the reverb's last second, played backwards and swelling ---- */
#define RV_N 16384u                                  /* 1.4 s at 12 kHz */
static int16_t rvb[RV_N];                            /* 32 KB, 16-bit like the ring */
static uint32_t rvb_w;
static float rvb_g[2], rvb_k[2], rvb_on[2], rvb_last;
static const float RV_G = 8000.0f;                   /* a grain: ~0.67 s (read and write part 2 grains: < RV_N) */
static void reverse_block(float *WL, float *WR, uint32_t n)
{
	if (W.rev <= 0.0f)
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
					rvb_on[k] = tx_noise() < 0.2f * W.rev ? 0.0f : 1.0f;   /* now and then: some sit out */
				}
				continue;
			}
			float t = 1.0f - rvb_g[k] / RV_G;                 /* 0 .. 1 through the grain */
			uint32_t at = (uint32_t)(rvb_k[k] - t * RV_G);
			float env = t * t * (t < 0.92f ? 1.0f : (1.0f - t) / 0.08f);   /* swelling, a quick end */
			o[k] = from16(rvb[at & (RV_N - 1u)]) * env * rvb_on[k];
			rvb_g[k] -= 1.0f;
		}
		float g = 2.2f * W.rev;
		float l = (o[0] * 0.8f + o[1] * 0.3f) * g, r = (o[0] * 0.3f + o[1] * 0.8f) * g;
		WL[i] += 0.5f * (l + rvb_last);
		WR[i] += 0.5f * (r + rvb_last);
		WL[i + 1] += l;
		WR[i + 1] += r;
		rvb_last = 0.5f * (l + r);
	}
}

/* ---- auto filter: the DJ filter sweeping down into a low-pass and back, every two bars ---- */
static void autof_tick(void)
{
	static float was = -1.0f;
	if (W.autof <= 0.0f) {
		if (was >= 0.0f) {
			filter_setup(P.g[GP_FILTER]);
			was = -1.0f;
		}
		return;
	}
	float ph = ((float)(step_n & 31u) + step_acc) / 32.0f;
	float sw = 0.5f - 0.5f * sin_u((uint32_t)(ph * 4294967296.0f) + 0x40000000u);
	float v = P.g[GP_FILTER] - W.autof * 0.3f * sw;
	if (P.g[GP_FILTER] > 0.47f && P.g[GP_FILTER] < 0.53f)
		v = 0.47f - W.autof * 0.27f * sw;                      /* from open, down to ~500 Hz */
	if (v != was) {
		filter_setup(v);
		was = v;
	}
}
