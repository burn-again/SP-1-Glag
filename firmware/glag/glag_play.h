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
 *   texture   a bed under the piece: rain, tape hiss or surf (2, the vinyl, removed: ~20 % of the CPU on hardware)
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
	/* song mode: its controls as asked and as playing (the asked ones are taken on the bar) */
	float a_stag[4], stag[4], a_full, full, a_fills, fills, a_bars;
	int a_loop[4], loop[4], a_kind;
	float rnd_set, rnd_mac, rnd_key, stray, stray_off[4];
	int kind_eff, ending, last_bar, song_key;
	float end_t;
	/* the pump */
	float prel, pbend, pdrums, pdr_s, pspace, pspare;
	int prhythm;
	/* the textures' controls past their level (rain, -, tape hiss, surf) */
	float tp[5][3];
	int key_hold;
	/* arp mode */
	int arp_want, arp_on;
	float arp_oct, arp_pat, arp_div, arp_spread;
	uint32_t arp_pos[4];
} W = {0.0f, 16.0f, 0, 0, 0.0f, 0, 0, 0.0f, 0.0f, 0, 0.0f, 0.0f, 0.0f, 0, 0, 0,
       {0}, {0}, 0.6f, 0.6f, 0.7f, 0.7f, 16.0f, {2, 2, 2, 2}, {2, 2, 2, 2}, 0,
       0.0f, 0.0f, 0.0f, 0.0f, {0}, 0, 0, -1, 0, 0.0f,
       1.0f / 3.0f, -1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0,
       {{0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}}, 0,
       0, 0, 1.0f / 3.0f, 0.0f, 1.0f / 3.0f, 0.0f, {0, 0, 0, 0}};

void gl_world(int what, float v)
{
	switch (what) {
	case GW_FORM: W.form = clampf(v, 0.0f, 1.0f); W.form_init = 0; break;
	case GW_FORM_BARS: W.a_bars = W.form_bars = v < 1.0f ? 1.0f : v; break;
	case GW_FORM_KIND: W.a_kind = W.form_kind = W.kind_eff = (int)v % 3; break;
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
	case GW_COMP: comp_amt = clampf(v, 0.0f, 1.0f); break;
	case GW_SONG:
		if (v > 0.5f) {                          /* on (or back on during its ending) */
			if (W.form <= 0.0f || W.ending) {
				W.form = 1.0f;
				W.ending = 0;
				if (!W.form_init)
					W.form_bars = W.a_bars;
			}
		} else if (W.form > 0.0f && !W.ending) { /* off: an outro first */
			W.ending = 1;
			W.end_t = 0.0f;
		}
		break;
	case GW_FILLS: W.a_fills = clampf(v, 0.0f, 1.0f); break;
	case GW_FULL: W.a_full = clampf(v, 0.0f, 1.0f); break;
	case GW_RND_SET: W.rnd_set = clampf(v, 0.0f, 1.0f); break;
	case GW_RND_MAC: W.rnd_mac = clampf(v, 0.0f, 1.0f); break;
	case GW_RND_KEY: W.rnd_key = clampf(v, 0.0f, 1.0f); break;
	case GW_RND_STRAY: W.stray = clampf(v, 0.0f, 1.0f); break;
	case GW_PUMP_REL: W.prel = clampf(v, 0.0f, 1.0f); break;
	case GW_PUMP_BEND: W.pbend = clampf(v, 0.0f, 1.0f); break;
	case GW_PUMP_DRUMS: W.pdrums = clampf(v, 0.0f, 1.0f); break;
	case GW_PUMP_RHYTHM: W.prhythm = (int)v & 3; break;
	case GW_PUMP_SPACE: W.pspace = clampf(v, 0.0f, 1.0f); break;
	case GW_PUMP_SPARE: W.pspare = clampf(v, 0.0f, 1.0f); break;
	case GW_KEY_HOLD: W.key_hold = v > 0.5f; break;
	case GW_ARP: W.arp_want = v > 0.5f; break;
	case GW_ARP_OCT: W.arp_oct = clampf(v, 0.0f, 1.0f); break;
	case GW_ARP_PAT: W.arp_pat = clampf(v, 0.0f, 1.0f); break;
	case GW_ARP_DIV: W.arp_div = clampf(v, 0.0f, 1.0f); break;
	case GW_ARP_SPREAD: W.arp_spread = clampf(v, 0.0f, 1.0f); break;
	default: break;
	}
}
void gl_world_ch(int what, int i, float v)
{
	if ((unsigned)i >= 4u)
		return;
	if (what == GWC_STAGGER)
		W.a_stag[i] = clampf(v, 0.0f, 1.0f);
	else if (what == GWC_LOOP)
		W.a_loop[i] = (int)v < 0 ? 0 : (int)v > 4 ? 4 : (int)v;
	else if (what == GWC_TEXP && i < 3 && W.tex > 0 && W.tex <= 4)
		W.tp[W.tex][i] = clampf(v, 0.0f, 1.0f);
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
static inline float fmaxf_(float a, float b) { return a > b ? a : b; }
enum { ROLE_HELD, ROLE_PLUCK, ROLE_BASS, ROLE_DRUMS };
static int voice_role(int v)
{
	if (v == GV_PULSE || v == GV_KIT || v == GV_LITEKIT)
		return ROLE_DRUMS;
	if (v == GV_BASS || v == GV_FUNK)
		return ROLE_BASS;
	if (v == GV_PAD || v == GV_DRONE || v == GV_FLUTE || v == GV_SWELL || v == GV_GONG ||
	    v == GV_ORGAN || v == GV_CHOIR || v == GV_BREATH)
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
/* (the drums' longer arc: the exits and entries over about twice as long) */
static const kf ARC_DRUMS_L[] = {{0.25f, 0.0f}, {0.38f, 1.0f}, {0.50f, 1.0f}, {0.61f, 0.0f}, {0.73f, 0.0f},
                                 {0.80f, 0.55f}, {0.86f, 1.0f}, {0.92f, 1.0f}, {0.99f, 0.0f}};
static const kf ARC_ACT[] = {{0.0f, 0.65f}, {0.40f, 1.0f}, {0.55f, 1.0f}, {0.63f, 0.7f}, {0.72f, 0.7f},
                             {0.85f, 1.0f}, {0.94f, 1.0f}, {1.0f, 0.65f}};
static const kf ARC_FILT[] = {{0.58f, 0.47f}, {0.67f, 0.30f}, {0.72f, 0.30f}, {0.85f, 0.47f}};   /* the DJ filter: open is 0.47 */
static const kf ARC_SPACE[] = {{0.58f, 1.0f}, {0.66f, 1.2f}, {0.72f, 1.2f}, {0.84f, 1.4f}, {0.90f, 1.0f}};
#define NK(a) ((int)(sizeof a / sizeof a[0]))
/* the other two arrangements: waves (in, all together, out; no breakdown) and a slow build
 * (one long rise, everything in by two thirds, out at the end) */
static const kf ARC_PLUCK_W[] = {{0.06f, 0.0f}, {0.2f, 1.0f}, {0.9f, 1.0f}, {0.99f, 0.0f}};
static const kf ARC_BASS_W[] = {{0.15f, 0.0f}, {0.3f, 1.0f}, {0.88f, 1.0f}, {0.97f, 0.0f}};
static const kf ARC_DRUMS_W[] = {{0.25f, 0.0f}, {0.4f, 1.0f}, {0.85f, 1.0f}, {0.95f, 0.0f}};
static const kf ARC_PLUCK_B[] = {{0.0f, 0.0f}, {0.25f, 1.0f}, {0.95f, 1.0f}, {1.0f, 0.0f}};
static const kf ARC_BASS_B[] = {{0.2f, 0.0f}, {0.45f, 1.0f}, {0.95f, 1.0f}, {1.0f, 0.0f}};
static const kf ARC_DRUMS_B[] = {{0.4f, 0.0f}, {0.65f, 1.0f}, {0.93f, 1.0f}, {0.99f, 0.0f}};
static inline float tx_noise(void);         /* (the textures' own dice: the notes' stay untouched) */
static const float LOOP_MUL[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
static float song_bars(void) { return ((float)step_n + step_acc) / 16.0f; }
static float form_pos(void)
{
	float cyc = 4.0f * W.form_bars;
	return fmodf_pos(song_bars(), cyc) / cyc;
}
/* on the bar: the controls asked for are taken; the randomness moves */
static void song_bar(int bar)
{
	for (int c = 0; c < 4; c++) {
		W.stag[c] = W.a_stag[c];
		W.loop[c] = W.a_loop[c];
	}
	W.full = W.a_full;
	W.fills = W.a_fills;
	if (W.form_kind != W.a_kind)
		W.kind_eff = W.form_kind = W.a_kind;
	if (W.a_bars != W.form_bars)
		W.form_bars = W.a_bars;
	float cyc = 4.0f * W.form_bars;
	if (fmodf_pos((float)bar, cyc) < 0.5f) {         /* a new cycle: how far it strays */
		for (int c = 0; c < 4; c++)
			W.stray_off[c] = W.stray * 0.3f * (0.5f + 0.5f * tx_noise());
		W.kind_eff = W.stray > 0.0f && 0.5f + 0.5f * tx_noise() < 0.5f * W.stray ? (int)(1.5f + 1.5f * tx_noise()) % 3 : W.form_kind;
	}
	if ((bar & 7) == 0)                              /* every 8 bars, each macro's pace */
		for (int c = 0; c < 4; c++)
			mac_pmul[c] = fexp2(tx_noise() * 1.5f * W.rnd_mac);
	if (W.rnd_key > 0.05f) {                         /* key travel, sooner the higher */
		W.key_forced = W.song_key = 1;
		float want = 4.0f - 3.0f * W.rnd_key;
		if (W.key_min != want)
			W.key_min = want;
		W.key_kind = W.rnd_key > 0.5f ? (int)(2.0f + 2.0f * tx_noise()) & 3 : 1;
	} else if (W.song_key) {
		W.song_key = 0;
		W.key_forced = 0;                            /* the piece's own key travel again */
	}
}
static void form_tick(float dt)
{
	int on = W.form > 0.0f;
	for (int c = 0; c < GL_NCH; c++)                 /* the settings' randomness: macros at least this deep */
		mac_depth[c] = on && !W.ending && W.rnd_set * 0.8f > mac_user[c] ? W.rnd_set * 0.8f : mac_user[c];
	if (!on) {
		/* off (after its outro): the piece comes back over four bars, never with a jump */
		form_fill = 0;
		float k = 1.0f - dcoef(4.0f * 16.0f * 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]), dt);
		if (form_filt >= 0.0f) {                    /* the filter opens the rest of the way, gliding */
			form_filt += (0.4699f - form_filt) * (1.0f - dcoef(1.0f, dt));
			if (form_filt > 0.4695f)
				form_filt = -1.0f;
		}
		form_space += (1.0f - form_space) * k;
		for (int c = 0; c < GL_NCH; c++) {
			w_lvl[c] += (1.0f - w_lvl[c]) * k;
			w_act[c] += (1.0f - w_act[c]) * k;
			if (w_lvl[c] > 0.999f)
				w_lvl[c] = 1.0f;
			if (w_act[c] > 0.999f)
				w_act[c] = 1.0f;
		}
		W.form_init = 0;
		for (int c = 0; c < 4; c++)
			mac_pmul[c] = 1.0f;
		if (W.song_key) {
			W.song_key = 0;
			W.key_forced = 0;
		}
		return;
	}
	int bar = (int)(song_bars());
	if (bar != W.last_bar || !W.form_init) {
		W.last_bar = bar;
		song_bar(bar);
	}
	float x = form_pos(), f = W.form, bar_s = 16.0f * 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	if (W.ending) {                                  /* the outro: two bars to the anchor, then off */
		W.end_t += dt;
		if (W.end_t >= 2.0f * bar_s) {
			W.form = 0.0f;
			W.ending = 0;
			return;
		}
	}
	/* a second pluck (or bass...) channel comes in a little after the first */
	int seen[4] = {0, 0, 0, 0};
	float k = W.form_init ? 1.0f - dcoef(W.ending ? bar_s : 1.2f, dt) : 1.0f;   /* a slew on top of the curves */
	W.form_init = 1;
	float act = kf_at(ARC_ACT, NK(ARC_ACT), x);
	act = 1.0f - (1.0f - act) * (1.3f - 0.6f * W.full);      /* how full: the quiet parts fuller */
	act = act > 1.0f ? 1.0f : act;
	int anchor = -1;
	for (int c = 0; c < GL_NCH && anchor < 0; c++)
		if (P.ch[c][CP_LEVEL] > 0.0f && voice_role(CH[c].want) == ROLE_HELD)
			anchor = c;
	for (int c = 0; c < GL_NCH && anchor < 0; c++)
		if (P.ch[c][CP_LEVEL] > 0.0f && voice_role(CH[c].want) == ROLE_PLUCK)
			anchor = c;
	const int kd = W.kind_eff;
	const kf *arc[3][4] = {{0, ARC_PLUCK, ARC_BASS, ARC_DRUMS}, {0, ARC_PLUCK_W, ARC_BASS_W, ARC_DRUMS_W},
	                       {0, ARC_PLUCK_B, ARC_BASS_B, ARC_DRUMS_B}};
	const int an[3][4] = {{0, NK(ARC_PLUCK), NK(ARC_BASS), NK(ARC_DRUMS)}, {0, NK(ARC_PLUCK_W), NK(ARC_BASS_W), NK(ARC_DRUMS_W)},
	                      {0, NK(ARC_PLUCK_B), NK(ARC_BASS_B), NK(ARC_DRUMS_B)}};
	for (int c = 0; c < GL_NCH; c++) {
		int r = voice_role(CH[c].want), r0 = voice_role(CH[c].voice);
		if (c == anchor)
			r = r0 = ROLE_HELD;
		/* this channel's own loop (a multiple of the song's), its stagger, its stray */
		float cyc = 4.0f * W.form_bars * LOOP_MUL[W.loop[c]];
		float xc = fmodf_pos(song_bars(), cyc) / cyc;
		float lvl = 1.0f, l0 = 1.0f, off = 0.035f * (float)seen[r]++ + 0.3f * W.stag[c] + W.stray_off[c];
		float xs = fmodf_pos(xc - off, 1.0f);
		if (r)
			lvl = (r == ROLE_DRUMS && kd == 0 && song_drums) ? kf_at(ARC_DRUMS_L, NK(ARC_DRUMS_L), xs) : kf_at(arc[kd][r], an[kd][r], xs);
		if (r0 && r0 != r)
			l0 = kf_at(arc[kd][r0], an[kd][r0], xs);              /* crossfading: the quieter */
		float l = 1.0f + f * (fminf_(lvl, l0) - 1.0f);
		if (W.ending && c != anchor)
			l = 0.0f;                                            /* the outro: down to the anchor */
		float a = c == anchor ? 1.0f + f * (0.5f * (act + 1.0f) - 1.0f) : 1.0f + f * (act - 1.0f);
		/* (the drums' own slew, from style 1: at least a bar and a half, however short the song) */
		const float kd_ = (song_drums && r0 == ROLE_DRUMS && W.form_init) ? 1.0f - dcoef(fmaxf_(1.2f, 1.5f * bar_s), dt) : k;
		if (CH[c].voice == GV_KIT && song_drums >= 2) {
			/* the Kit peels off: its place in the arc picks which sounds play (kit_fire), its level
			 * held until only the kick is left, which then fades */
			const float was = w_peel[c];
			w_peel[c] += (l - w_peel[c]) * kd_;
			if (song_drums == 3 && was >= 0.97f && w_peel[c] < 0.97f && l < was)
				w_exit[c] = 1;                                  /* (leaving: a fill into the reverb) */
			const float g = w_peel[c] / 0.35f;
			w_lvl[c] = g > 1.0f ? 1.0f : g;
		} else {
			w_peel[c] = 1.0f;
			w_lvl[c] += (l - w_lvl[c]) * kd_;
		}
		w_act[c] += (a - w_act[c]) * k;
	}
	/* the breakdown's filter and space, and the fill, belong to the first arrangement; FILLS
	 * sets how deep they go (and whether the fill plays) */
	float sp = 1.0f, ft = 0.47f;
	if (kd == 0 && !W.ending) {                      /* (the outro opens them back up) */
		sp = 1.0f + W.fills * (kf_at(ARC_SPACE, NK(ARC_SPACE), x) - 1.0f);
		ft = 0.47f - W.fills * (0.47f - kf_at(ARC_FILT, NK(ARC_FILT), x));
	}
	sp = 1.0f + f * (sp - 1.0f);
	form_space += (sp - form_space) * k;
	float fwant = 0.47f - f * (0.47f - ft);
	form_filt = form_filt < 0.0f ? fwant : form_filt + (fwant - form_filt) * k;
	float cyc = 4.0f * W.form_bars, bx = x * cyc, land = 0.85f * cyc;
	form_fill = kd == 0 && W.fills > 0.3f && !W.ending && bx >= land - 1.0f && bx < land;
}

/* ---- key travel: on the bar, once the time is up ---- */
static void key_tick(float dt)
{
	if (W.key_min <= 0.0f || W.key_hold) {          /* off, or held to the piece's own key */
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
	/* the Kit's director can pump too (kit_pump: > 0 the band ducks on the kick, < 0 it swells into it,
	 * dipping between): when Playworld's pump is off, while a Kit is playing */
	static float kp;
	kit_pump_t -= dt;
	kp += ((kit_pump_t > 0.0f ? kit_pump : 0.0f) - kp) * (1.0f - dcoef(1.5f, dt));
	if (W.pump <= 0.0f && (kp > 0.02f || kp < -0.02f)) {
		const float a = kp < 0.0f ? -kp : kp;
		const float rel = 0.375f * 60.0f / bpm * (1.0f + a);
		if (pump_tgt > pump_env)
			pump_env += (pump_tgt - pump_env) * 0.25f;
		else
			pump_env = pump_tgt;
		pump_tgt *= dcoef(rel * 3.0f, dt);
		pbend = 1.0f;
		const float depth = 0.5f * a;
		return kp > 0.0f ? 1.0f - depth * pump_env : 1.0f - depth * (1.0f - pump_env);
	}
	if (W.pump <= 0.0f) {
		pbend = 1.0f;
		W.pdr_s += (1.0f - W.pdr_s) * (1.0f - dcoef(0.3f, dt));
		return 1.0f;
	}
	W.pdr_s += ((W.pump > 0.0f ? W.pdrums : 1.0f) - W.pdr_s) * (1.0f - dcoef(0.3f, dt));   /* drums in or out, smoothly */
	float rel = 0.375f * 60.0f / bpm * (1.0f + W.pump) * (0.5f + 1.5f * W.prel);
	if (pump_tgt > pump_env)
		pump_env += (pump_tgt - pump_env) * 0.25f;             /* ~5 ms in: a chop, not a click */
	else
		pump_env = pump_tgt;
	pump_tgt *= dcoef(rel * 3.0f, dt);                         /* (dcoef: the time to fall 60 dB) */
	float depth = 0.6f * W.pump + 0.37f * W.pump * W.pump * W.pump;
	float bend = W.pbend >= 0.0f ? W.pbend : W.pump > 0.7f ? (W.pump - 0.7f) / 0.3f : 0.0f;
	pbend = 1.0f - 0.0561f * bend * pump_env;                  /* 0.0561: a semitone down */
	return 1.0f - depth * pump_env;
}

/* the pump's own clock: on the beat, the eighth or the half bar, or (with no Pulse in the
 * piece) on the beat, so it pumps without drums */
static void pump_step(uint32_t step)
{
	if (W.pump <= 0.0f)
		return;
	int r = W.prhythm, has = 0;
	for (int c = 0; c < GL_NCH; c++)
		has |= (CH[c].voice == GV_PULSE || CH[c].voice == GV_KIT) && P.ch[c][CP_LEVEL] > 0.0f;
	if (r == 0 && has)
		return;                                       /* the kick does it */
	uint32_t every = r == 2 ? 2u : r == 3 ? 8u : 4u;
	if (step % every == 0u)
		pump_tgt = 1.0f;
}

/* ---- arp mode (T2): every channel but the drums played by one arp ----
 * Each channel keeps its own sound but gives up its own sequencer: the arp walks the chord over
 * its octaves in a pattern (up, down, up-down, random) at its division, and plays each note on
 * every channel through voice_note(), at the shortest decay and sustain (plucks plucked, pads and
 * strings struck short). SPREAD: 0, every channel on the same note at once (one huge stacked
 * arp); up, each channel a little further along the pattern, and past two thirds the odd ones
 * at half speed. Turning it on or off is taken on the next bar. */
static int arp_live(int ch) { return W.arp_on && CH[ch].voice != GV_PULSE && CH[ch].voice != GV_KIT; }
static int arp_held(int v)
{
	return v == GV_PAD || v == GV_SWELL || v == GV_DRONE || v == GV_SUPER || v == GV_STRINGS;
}
static void arp_bar(void)
{
	if (W.arp_on != W.arp_want) {
		W.arp_on = W.arp_want;
		for (int c = 0; c < 4; c++)
			W.arp_pos[c] = 0;
	}
}
/* one note on a channel, now, in its own voice: what arp mode (and MIDI, later) plays with */
static nvoice *voice_note(chan *c, int ch, int note, float vel, float gate)
{
	nvoice *v = 0;
	float tone = eff_tone(ch), chr = P.ch[ch][CP_CHAR];
	switch (c->voice) {
	case GV_PULSE:
	case GV_KIT:
		return 0;
	case GV_KALIMBA: {
		int k = c->ks_next;
		if (!c->ks_len[k] && budget_used() + 2 > fm_cap)
			return 0;
		c->ks_next = (uint8_t)((k + 1) % 3);
		ks_pluck(c, k, mtof(ivl_note(ch, (float)note)) * speed * pbend, vel, 0.15f + 0.8f * tone, 0.5f + 0.45f * chr, 0.5f, 0);
		break;
	}
	case GV_HARP:
		harp_pluck(c, ch, note, vel);
		break;
	case GV_HARPSI:
		harpsi_pluck(c, ch, note, vel, gate);
		break;
	case GV_BASS:
		v = &c->v[0];
		nv_trigger(v, (float)note, vel, 0.5f, 0);
		c->target_note = c->glide_note = v->note;      /* no glide: each note its own */
		break;
	case GV_FUNK:
		v = &c->v[0];
		nv_trigger(v, (float)note, vel, 1.0f, 0);
		v->hold = gate;
		v->bend = 0.0f;
		v->ratio = 1.0f;
		break;
	case GV_RIBBON:
		ribbon_blow(c, ch, note, vel, gate);
		v = &c->v[0];
		break;
	case GV_GLOW:
		while (note > 79)
			note -= 12;
		v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel * 1.35f, 1.0f, 0);
		break;
	case GV_FLUTE:
		v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel, 1.0f, 0);
		v->hold = gate;
		break;
	default: {
		/* the FM voices (and, in arp mode, the held ones struck short): each its own ratio */
		int n = c->voice == GV_BELLS || c->voice == GV_SHIMMER || c->voice == GV_PAD ? 4 :
		        c->voice == GV_DRONE || c->voice == GV_GONG ? 2 : 3;
		float ratio = 1.0f;
		switch (c->voice) {
		case GV_BELLS: ratio = BELL_RATIO[stepped(chr > 0.5f ? (chr - 0.5f) * 2.0f : chr * 2.0f, 3)][c->vnext & 3]; break;
		case GV_PIANO: ratio = (float)(1 + stepped(chr, 3)); break;
		case GV_MARIMBA: ratio = 2.5f + 2.5f * chr; break;
		case GV_ARP: ratio = 3.5f; break;
		case GV_GONG: ratio = 1.4f; break;
		case GV_PAD: ratio = chr < 0.5f ? 1.0f : 2.0f; break;
		case GV_DRONE: ratio = 2.0f; break;
		case GV_TAILS: ratio = 2.0f; break;
		default: break;
		}
		v = alloc(c, n);
		if (!sounding(v))
			make_room_w(v, weight(c->voice));
		nv_trigger(v, (float)note, vel * (c->voice == GV_PIZZ ? 1.5f : 1.2f), ratio, c->vnext++);
		break;
	}
	}
	c->glow = vel;
	return v;
}
static void arp_step(chan *c, int ch, uint32_t step)
{
	const float s = W.arp_spread;
	int div = 1 + (int)(W.arp_div * 3.0f + 0.5f);
	if (s > 0.66f && (ch & 1))
		div *= 2;
	if (step % (uint32_t)div)
		return;
	int oct = 1 + (int)(W.arp_oct * 3.0f + 0.5f), base = reg_base(c, ch) - (voice_role(c->voice) == ROLE_BASS ? 0 : 5);
	int notes[16], n = 0;
	for (int o = 0; o < oct; o++)
		for (int k = 0; k < chord_n && n < 16; k++)
			notes[n++] = place(chord_pc[k], (float)(base + 12 * o + 3 * k), base - 1 + 12 * o, base + 11 + 12 * o);
	for (int i = 1; i < n; i++)                      /* low to high */
		for (int j = i; j > 0 && notes[j] < notes[j - 1]; j--) {
			int t = notes[j];
			notes[j] = notes[j - 1];
			notes[j - 1] = t;
		}
	int q = (int)W.arp_pos[ch]++ + (int)(s * (float)ch * (float)n * 0.25f + 0.5f), idx;
	switch ((int)(W.arp_pat * 3.0f + 0.5f)) {
	case 0: idx = q % n; break;
	case 1: idx = n - 1 - q % n; break;
	case 2: { int per = n > 1 ? 2 * n - 2 : 1, r = q % per; idx = r < n ? r : per - r; break; }
	default: idx = rndi(n); break;
	}
	float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	voice_note(c, ch, notes[idx], (q % n == 0 ? 0.85f : 0.65f) + 0.1f * rnd(), (float)div * step_s * 0.6f);
}
/* ---- MIDI in ---- */
static uint8_t mq[64][3];
static volatile uint32_t mq_w, mq_r;
static float midi_t[4];                          /* seconds MIDI keeps the channel */
static uint8_t midi_n[4];                        /* notes held */
void gl_midi_note(int ch, int note, int vel)
{
	uint32_t w = mq_w;
	if ((unsigned)ch >= 4u || w - mq_r >= 64u)
		return;
	mq[w & 63u][0] = (uint8_t)ch;
	mq[w & 63u][1] = (uint8_t)(note & 127);
	mq[w & 63u][2] = (uint8_t)(vel & 127);
	mq_w = w + 1u;
}
static int midi_live(int ch) { return midi_t[ch] > 0.0f; }
static const int8_t KIT_LANE[12] = {0, 3, 1, 4, 5, 2, 2, 8, 2, 7, 6, 2};
static void midi_drum(chan *c, int ch, int note, float vel)
{
	/* by the note's name, in any octave (General MIDI's drum notes land the same way): C kick,
	 * D / E snare, F# / G# hat, A# open hat, C# / D# click, the rest a soft shaker */
	static const uint8_t MAP[12] = {1 | 8, 4, 32, 4, 32, 2, 2 | 16, 2, 2 | 16, 2, 2 | 64 | 16, 2};
	int mask = MAP[note % 12];
	if (vel < 0.5f)
		mask &= ~(8 | 16);                           /* soft: no accent */
	if (c->voice == GV_KIT) {
		/* the kit: C kick, C# clank, D / E snare (E the Nord hit), D# tissue, F# / G# hat,
		 * G roll, A ghost, A# open hat, the rest the hat */
		kit_from_midi = 1;
		kit_fire(c, ch, KIT_LANE[note % 12], vel >= 0.99f ? 1.0f : vel * 0.95f);
		kit_from_midi = 0;
		kit_hold(c, KIT_LANE[note % 12], 1);
		return;
	}
	pulse_fire(c, ch, mask);
}
static void midi_tick(float dt)
{
	while (mq_r != mq_w) {
		const uint8_t *e = mq[mq_r & 63u];
		int ch = e[0], note = e[1];
		float vel = (float)e[2] * (1.0f / 127.0f);
		chan *c = &CH[ch];
		if (e[2]) {
			midi_t[ch] = 4.0f;
			if (midi_n[ch] < 255)
				midi_n[ch]++;
			if (c->voice == GV_PULSE || c->voice == GV_KIT)
				midi_drum(c, ch, note, vel);
			else {
				nvoice *v = voice_note(c, ch, note, 0.3f + 0.7f * vel, 30.0f);
				if (v)
					v->mnote = (uint8_t)(note + 1);
			}
		} else {
			if (midi_n[ch])
				midi_n[ch]--;
			if (c->voice == GV_KIT)
				kit_hold(c, KIT_LANE[note % 12], 0);
			for (int i = 0; i < NV; i++) {        /* its voice lets go */
				nvoice *v = &c->v[i];
				if (v->mnote != note + 1)
					continue;
				v->mnote = 0;
				if (arp_held(c->voice))
					v->stage = ST_REL;
				else if (c->voice == GV_FUNK || c->voice == GV_RIBBON || c->voice == GV_FLUTE)
					v->hold = 0.0f;
			}
		}
		mq_r++;
	}
	for (int ch = 0; ch < 4; ch++) {
		if (midi_n[ch])
			midi_t[ch] = 4.0f;
		else if (midi_t[ch] > 0.0f)
			midi_t[ch] -= dt;                        /* 4 s after the last: its own music back */
	}
}

/* the held voices in arp mode: struck, falling away, each in its own render's terms (under MIDI:
 * held while the key is, an attack, a decay to a sustain, a release when it lets go) */
static void arp_held_control(chan *c, int ch, float dt, float tone, float len)
{
	int nv = c->voice == GV_DRONE ? 2 : c->voice == GV_PAD ? 4 : 3;
	float dec = expmap(len, 0.15f, 1.2f);
	for (int i = 0; i < nv; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp == 0.0f) {
			v->ainc = v->iinc = 0.0f;
			continue;
		}
		if (midi_live(ch) && !arp_live(ch)) {
			if (v->stage == ST_ATT) {
				v->env += dt / 0.012f;
				if (v->env >= 1.0f) {
					v->env = 1.0f;
					v->stage = ST_DEC;
				}
			} else if (v->stage == ST_DEC)
				v->env += (0.75f - v->env) * (1.0f - dcoef(0.5f, dt));
			else {
				v->env *= dcoef(0.3f + 2.5f * len, dt);
				if (v->env < 0.0005f) {
					v->env = 0.0f;
					v->stage = ST_IDLE;
				}
			}
			v->menv *= dcoef(0.5f, dt);
			ramp(v, v->env * v->vel, clampf((0.3f + 1.5f * tone) * (0.3f + 0.7f * v->menv), 0.0f, 3.0f) * IDX_SCALE);
		} else
			nv_perc(v, 0.004f, dec / speed, 0.15f / speed, 0.3f + 1.5f * tone, dt);
		float hz = mtof(v->note) * speed * pbend;
		switch (c->voice) {
		case GV_PAD:
		case GV_DRONE:
			nv_pitch(v, v->note);
			break;
		case GV_SWELL:
			nv_pitch(v, v->note);
			v->ic2 = hz_inc(hz * 1.004f);
			break;
		case GV_SUPER: {
			v->ic = hz_inc(hz * 1.003f);
			v->ic2 = hz_inc(hz / 1.003f);
			v->im = hz_inc(hz);
			v->ic3 = hz_inc(hz * 1.007f);
			v->ic4 = hz_inc(hz / 1.007f);
			float fc = clampf((400.0f + 5000.0f * tone * tone * (0.3f + 0.7f * v->menv)) * fexp2((v->note - 60.0f) * (1.0f / 48.0f)), 1.5f * hz, 5200.0f);
			v->fk = clampf(fc * (6.2832f / VSR), 0.02f, 0.9f);
			v->amp *= 1.0f;
			v->ainc *= 0.63f;                  /* (its five saws: a little under the rest) */
			break;
		}
		default: {                             /* strings: as its render (PWM by default) wants */
			int m = alt_m[GV_STRINGS];
			v->ic = hz_inc(hz);
			v->ic2 = m == 1 ? hz_inc(hz * 2.0f) : hz_inc(hz / 1.0035f);
			v->bend = 0.4f;
			float fc = clampf((350.0f + 2600.0f * tone * tone * (0.3f + 0.7f * v->menv)) * fexp2((v->note - 60.0f) * (1.0f / 24.0f)), 1.5f * hz, 5000.0f);
			v->fk = m == 1 ? clampf(fc * (6.2832f / VSR), 0.05f, 0.85f) : clampf(1.0f - fexp2(-fc * (9.0647f / VSR)), 0.01f, 0.8f);
			v->fq = 0.25f;
			v->ainc *= 0.5f;
			break;
		}
		}
	}
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
	/* (every texture 20 % quieter than it was, and its noise washes 20 % under that again; the
	 * drops and crackles keep their place over them) */
	float g = W.tex_lvl * W.tex_lvl * 0.25f * 0.8f, bt = (float)n / sr;
	const float wash = 0.8f;
	float *s = tx_s;
	const float *tp = W.tp[W.tex <= 4 ? W.tex : 0];           /* its three controls past the level */
	tx_lfo += bt / 20.0f * (W.tex == 3 ? 2.0f * tp[1] + 0.1f : 1.0f);   /* tape hiss: a breath every ~20 s */
	tx_lfo -= tx_lfo >= 1.0f ? 1.0f : 0.0f;
	if (W.tex == 1) {   /* rain: the two bands wander, each to a new place every few seconds */
		for (int k = 0; k < 2; k++) {
			if (tx_noise() > 1.0f - 0.0014f * tp[0])
				rn_ft[k] = (0.12f + 0.5f * (0.5f + 0.5f * tx_noise())) * (0.5f + tp[2]);
			rn_f[k] += (rn_ft[k] - rn_f[k]) * bt * 0.8f * tp[0];
		}
	}
	if (W.tex == 4) {   /* surf: waves of their own lengths, rising, then a long wash back */
		for (int k = 0; k < 2; k++) {
			sf_ph[k] += bt / sf_per[k];
			if (sf_ph[k] >= 1.0f) {
				sf_ph[k] -= 1.0f;
				sf_per[k] = (7.0f + 8.0f * (0.5f + 0.5f * tx_noise())) * fexp2((0.5f - tp[0]) * 2.0f);
				sf_pan[k] = 0.5f + 0.45f * tx_noise();
			}
			float x = sf_ph[k], tgt = x < 0.3f ? 0.5f - 0.5f * sin_u((uint32_t)(x / 0.3f * 2147483648.0f) + 0x40000000u) : 0.0f;
			if (tgt > sf_env[k])
				sf_env[k] = tgt;
			else
				sf_env[k] *= dcoef(sf_per[k] * 2.6f * tp[1] + 0.5f, bt);   /* the wash outlasts the wave */
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
			l = (s[1] * 0.8f + s[3] * 0.35f) * 0.6f * wash;
			r = (s[3] * 0.8f + s[1] * 0.35f) * 0.6f * wash;
			if (tx_drop < 0.01f && (tx_r & 0xFFFu) < (uint32_t)(6.0f * tp[1] + 0.5f)) {   /* ~18 drops a second */
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
		case 3: {   /* tape hiss: soft, rolled off above ~4.5 kHz, breathing slowly */
			s[0] += 0.08f * (a - s[0]);
			s[1] += 0.08f * (b - s[1]);
			const float tk = 0.3f + 0.8f * tp[2];
			s[2] += tk * ((a - s[0]) - s[2]);
			s[3] += tk * (s[2] - s[3]);
			s[4] += tk * ((b - s[1]) - s[4]);
			s[5] += tk * (s[4] - s[5]);
			float br = 1.0f - 0.4f * tp[0] + 0.4f * tp[0] * sin_u((uint32_t)(tx_lfo * 4294967296.0f));
			l = s[3] * 0.26f * br * wash;
			r = s[5] * 0.26f * br * wash;
			break;
		}
		default: {  /* surf: low noise for each wave, brighter as it breaks, moving along the shore */
			for (int k = 0; k < 2; k++) {
				float e = sf_env[k], kf = (0.02f + 0.13f * e * e) * (0.5f + tp[2]), x = k ? b : a;
				s[2 * k] += kf * (x - s[2 * k]);
				s[2 * k + 1] += kf * (s[2 * k] - s[2 * k + 1]);
				float v = s[2 * k + 1] * (0.15f + 1.8f * e) * wash;
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
	if (v >= 0.0f && v > 0.4699f)
		v = 0.4699f;          /* (0.47 itself would be "through": the filter's states left behind, a click as it closes) */
	if (v < 0.0f) {
		if (was >= 0.0f && centre)
			v = 0.4699f;      /* once it has moved, it stays a low-pass at 18 kHz: going back to "through" would click */
		else {
			if (was >= 0.0f) {
				filter_setup(base);
				was = -1.0f;
			}
			return;
		}
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
