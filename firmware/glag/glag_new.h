/*
 * Glag: nine light voices and the performance effects. AUDITION: built to be heard and tried, not
 * yet measured on the cycle model or covered by tests; the costs in the comments are estimates.
 * Included by glag_engine.c after the Kit; the voices are appended to enum gl_voice, so no
 * existing piece changes, and every effect at zero leaves the engine as it was.
 *
 *   LITE KIT  the Pulse's way (a few cheap lanes) with more of them: kick, snare, hats, a clap
 *             (three noise bursts through a band-pass), a rim / clave (a struck resonator on a
 *             chord tone), toms (a falling sine on the chord), a cowbell (two squares through a
 *             band-pass, on the chord's root). Five grooves (CHARACTER: four on the floor,
 *             breaks, clave, minimal, dub), ACTIVITY the lanes, LENGTH the decays, TONE the
 *             brightness; a director over four bars: a fill or a dropout now and then.
 *             (est. 2-4 points busy, against the Kit's 14.6)
 *   PERC      a struck resonator and a click (Nord Drum's body + click, in small): CHARACTER the
 *             material: woodblock, log drum, tabla (bending down into its note), glass (est. 0.5)
 *   ORGAN     four drawbars (8' 4' 2 2/3' 2') on the chord, CHARACTER the registration, a slow
 *             Leslie between the two sides (est. 1-1.5)
 *   MUSIC BOX a sine and two inharmonic partials (2.76x and 5.4x) that die first (est. 0.6)
 *   BREATH    noise through a narrow resonator at the note, with a little of the sine; CHARACTER
 *             tonal .. airy (est. 0.8)
 *   CHIP      a band-limited pulse, CHARACTER its width (1/8, 1/4, 1/2), past 3/4 a chip
 *             arpeggio through the chord (est. 0.5)
 *   CHOIR     three voices on the chord: a saw through two formant resonators, the vowels
 *             drifting (CHARACTER: oo/oh .. ah/eh), sliding into each new chord (est. 1.5-2.5)
 *   KOTO      the harp's tuned plucked strings with a one-sided bridge that buzzes while they ring
 *             loud (CHARACTER: koto .. sitar) (est. 1.5-2.5)
 *   GRAINS    the tape echo's own memory read back in short windows at 1/2, 1, 1.5 and 2x: it
 *             plays what the other channels sent to the echo (est. 0.6)
 */

/* ================================================================ performance (gl_perform) */

void gl_perform(int what, float v)
{
	switch (what) {
	case GF_ANSWER: pf.answer = clampf(v, 0.0f, 1.0f); break;
	case GF_MEMORY: pf.memory = clampf(v, 0.0f, 1.0f); break;
	case GF_BREATHE:
		/* importance thinning (gl_thin): past the first start in a step, only what matters
		 * enough plays; the threshold rises with the amount */
		pf.breathe = clampf(v, 0.0f, 1.0f);
		thin_cap = pf.breathe > 0.01f ? 1 : 0;
		thin_thr = 0.12f + 0.78f * pf.breathe;
		break;
	case GF_HOLD_ECHO: pf.hold_echo = (uint8_t)(v > 0.5f); break;
	case GF_HOLD_VERB: pf.hold_verb = (uint8_t)(v > 0.5f); break;
	case GF_HEAD: pf.head = (uint8_t)(v < 0.5f ? 0 : v < 1.5f ? 1 : v < 2.5f ? 2 : 3); break;
	case GF_SYMBIOSIS: pf.symb = clampf(v, 0.0f, 1.0f); break;
	case GF_REGISTER: pf.regc = clampf(v, 0.0f, 1.0f); break;
	default: break;
	}
}
static inline float pf_rnd(void)
{
	pf_rng ^= pf_rng << 13;
	pf_rng ^= pf_rng >> 17;
	pf_rng ^= pf_rng << 5;
	return (float)(pf_rng >> 8) * (1.0f / 16777216.0f);
}

/* ---- Answer: a drum channel's variation (a Kit or Lite Kit fill) is answered a beat later by
 * the plucked voices, the root and then the fifth, two steps apart ---- */
static uint32_t ans_at;
static uint8_t ans_left;
static void answer_call(void)
{
	if (pf.answer <= 0.0f || ans_left || pf_rnd() >= pf.answer)
		return;
	ans_at = step_n + 4u;
	ans_left = 2;
}
static int answers(int v)
{
	return v == GV_BELLS || v == GV_PIANO || v == GV_MARIMBA || v == GV_KALIMBA || v == GV_EP ||
	       v == GV_PIZZ || v == GV_HARP || v == GV_GLOW || v == GV_TAILS || v == GV_PERC ||
	       v == GV_MBOX || v == GV_CHIP || v == GV_KOTO;
}
static int ks_take(chan *c);
static void answer_hit(chan *c, int ch)
{
	const int base = reg_base(c, ch);
	const int pc = chord_pc[ans_left ? 0 : (chord_n > 2 ? 2 : 1)];
	const int note = place(pc, (float)(base + 7), base, base + 12);
	const float vel = 0.5f + 0.2f * pf_rnd();
	if (c->voice == GV_KALIMBA || c->voice == GV_KOTO || c->voice == GV_HARP) {
		int k;
		if (c->voice == GV_HARP) {
			k = ks_take(c);
			if (k < 0)
				return;
		} else {
			k = c->ks_next;
			c->ks_next = (uint8_t)((k + 1) % ks_count(c->voice));
		}
		const float hz = mtof(ivl_note(ch, (float)note)) * speed * pbend;
		ks_pluck(c, k, hz, vel, 0.3f + 0.6f * eff_tone(ch),
		         c->voice == GV_KALIMBA ? 0.5f + 0.45f * P.ch[ch][CP_CHAR] : 0.65f + 0.3f * eff_tone(ch),
		         expmap(P.ch[ch][CP_LENGTH], 0.4f, 4.0f), c->voice != GV_KALIMBA);
	} else if (c->voice == GV_BELLS) {
		int k = 0;
		for (int i = 1; i < 4; i++)
			if (c->v[i].amp < c->v[k].amp)
				k = i;
		nvoice *v = &c->v[k];
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel, c->bp_ratio[k] > 0.0f ? c->bp_ratio[k] : 2.0f, k);
	} else {
		nvoice *v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		float ratio = 1.0f;
		if (c->voice == GV_PIANO)
			ratio = (float)(1 + stepped(P.ch[ch][CP_CHAR], 3));
		else if (c->voice == GV_MARIMBA)
			ratio = 2.5f + 2.5f * P.ch[ch][CP_CHAR];
		nv_trigger(v, (float)note, vel * 1.3f, ratio, (int)(pf_rng & 1u));
	}
	c->glow = 1.0f;
}
/* (from clock_step, after the channels have stepped) */
static void answer_tick(void)
{
	if (!ans_left || step_n < ans_at)
		return;
	ans_left--;
	ans_at = step_n + 2u;
	for (int c = 0; c < GL_NCH; c++)
		if (answers(CH[c].voice) && P.ch[c][CP_LEVEL] > 0.01f)
			answer_hit(&CH[c], c);
}

/* ---- Memory (after Marbles' Deja Vu): a channel's last 32 decisions (whether a step plays and
 * its note); with the amount's probability a step takes the decision of the step a bar before
 * instead of rolling new dice. Near full a part repeats, a little lower it mutates slowly ---- */
static int8_t mem_note[GL_NCH][32];
static uint8_t mem_fire[GL_NCH][32], mem_pos[GL_NCH], mem_n[GL_NCH];
static int mem_recall(int ch, int *fire, int *note)
{
	if (mem_n[ch] < 16 || pf_rnd() >= pf.memory)
		return 0;
	const int i = (mem_pos[ch] - 16) & 31;
	*fire = mem_fire[ch][i];
	*note = mem_note[ch][i];
	return 1;
}
static void mem_store(int ch, int fire, int note)
{
	mem_fire[ch][mem_pos[ch]] = (uint8_t)fire;
	mem_note[ch][mem_pos[ch]] = (int8_t)note;
	mem_pos[ch] = (uint8_t)((mem_pos[ch] + 1u) & 31u);
	if (mem_n[ch] < 32)
		mem_n[ch]++;
}

/* ---- Symbiosis: the channels' slow macro glides pull toward each other's phase (Kuramoto) ---- */
static void macro_couple(float dt)
{
	if (pf.symb <= 0.0f)
		return;
	for (int k = 0; k < 2; k++) {
		float pull[GL_NCH] = {0.0f, 0.0f, 0.0f, 0.0f};
		int n = 0;
		for (int a = 0; a < GL_NCH; a++)
			n += mac_depth[a] > 0.0f;
		if (n < 2)
			continue;
		for (int a = 0; a < GL_NCH; a++) {
			if (mac_depth[a] <= 0.0f)
				continue;
			for (int b = 0; b < GL_NCH; b++) {
				if (b == a || mac_depth[b] <= 0.0f)
					continue;
				float d = mac_ph[b][k] - mac_ph[a][k];
				d -= (float)(int)d;
				d += d < 0.0f ? 1.0f : 0.0f;
				pull[a] += sin_u((uint32_t)(d * 4294967040.0f));
			}
		}
		for (int a = 0; a < GL_NCH; a++)
			if (mac_depth[a] > 0.0f) {
				float p = mac_ph[a][k] + pf.symb * 0.04f * dt * pull[a] / (float)(n - 1);
				p -= (float)(int)p;
				mac_ph[a][k] = p < 0.0f ? p + 1.0f : p;
			}
	}
}

/* ================================================================ the voices */

static inline float nb_saw(uint32_t ph, uint32_t inc)      /* a PolyBLEP saw, as blep_saw */
{
	float t = (float)ph * (1.0f / 4294967296.0f), dt = (float)inc * (1.0f / 4294967296.0f);
	float s = 2.0f * t - 1.0f;
	if (t < dt) {
		float x = t / dt;
		s -= x + x - x * x - 1.0f;
	} else if (t > 1.0f - dt) {
		float x = (t - 1.0f) / dt;
		s -= x * x + x + x + 1.0f;
	}
	return s;
}
static inline float nb_noise(uint32_t *r)
{
	uint32_t x = *r ? *r : 0x1234567u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*r = x;
	return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
/* a two-pole resonator's coefficients: a = 2 r cos w, b = r^2, from a frequency and a decay
 * (seconds to -60 dB) at the voices' rate; *g its input gain for an impulse response of 1 */
static void res_set(float hz, float t60, float *a, float *b, float *g)
{
	const float w = clampf(hz, 20.0f, 0.45f * VSR) * (6.2831853f / VSR);
	const float r = fexp2(-9.9658f / (t60 * VSR));
	const uint32_t ph = (uint32_t)(w * (1.0f / 6.2831853f) * 4294967296.0f);
	*a = 2.0f * r * sin_u(ph + 0x40000000u);
	*b = r * r;
	*g = sin_u(ph);
}
/* the same for a band (a formant, a breath): its bandwidth in Hz, gain for a level peak */
static void band_set(float hz, float bw, float *a, float *b, float *g)
{
	const float w = clampf(hz, 20.0f, 0.45f * VSR) * (6.2831853f / VSR);
	const float r = fexp2(-1.4427f * 3.1416f * bw / VSR);
	const uint32_t ph = (uint32_t)(w * (1.0f / 6.2831853f) * 4294967296.0f);
	const float cw = sin_u(ph + 0x40000000u), c2 = 2.0f * cw * cw - 1.0f;
	*a = 2.0f * r * cw;
	*b = r * r;
	float q = 1.0f - 2.0f * r * c2 + r * r, y = 1.0f;
	q = q > 0.0f ? q : 0.0f;
	for (int it = 0; it < 6; it++)
		y = 0.5f * (y + q / y);
	*g = (1.0f - r) * y;
}

/* ---- Perc: three struck resonators. Per voice: f1, f2 the body's states, fk, fq its a and b,
 * idx2, menv2 the second mode's states, hold, fb its a and b; menv the click; pm its noise ---- */
static const struct { float oct, t60, p2, m2, bend, click; } PERC_M[4] = {
	{12.0f, 0.09f, 2.31f, 0.25f, 0.0f, 0.8f},     /* woodblock: high, short, hollow */
	{-12.0f, 0.65f, 1.52f, 0.35f, 0.0f, 0.3f},    /* log drum: low, round, a tongue's second note */
	{0.0f, 0.42f, 2.0f, 0.4f, 5.0f, 0.5f},        /* tabla: bends down into its note */
	{12.0f, 1.6f, 2.76f, 0.55f, 0.0f, 0.15f},     /* glass: rings, an inharmonic second */
};
static void perc_control(chan *c, int ch, float dt)
{
	const int m = stepped(P.ch[ch][CP_CHAR], 4);
	const float len = 0.4f + 1.2f * P.ch[ch][CP_LENGTH], tone = eff_tone(ch);
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f)
			continue;
		if (v->stage == ST_ATT) {                 /* struck: the click and the modes set */
			v->stage = ST_DEC;
			v->t = 0.0f;
			v->menv = 1.0f;
			v->f1 = v->f2 = v->idx2 = v->menv2 = 0.0f;
		}
		v->t += dt * speed;
		const float bend = PERC_M[m].bend * fexp2(-v->t * 20.0f);   /* (semitones, gone in ~70 ms) */
		const float hz = mtof(v->note + PERC_M[m].oct + bend) * speed * pbend;
		float g1, g2;
		res_set(hz, PERC_M[m].t60 * len, &v->fk, &v->fq, &g1);
		res_set(hz * PERC_M[m].p2, PERC_M[m].t60 * len * 0.5f, &v->hold, &v->fb, &g2);
		v->ratio = g1;                            /* (the body's input gain, kept for render) */
		v->bend = g2 * PERC_M[m].m2;
		v->iinc = PERC_M[m].click * (0.4f + 0.6f * tone);
		if (v->t > PERC_M[m].t60 * len * 1.3f)
			v->stage = ST_IDLE;
		const float a = v->stage == ST_IDLE ? 0.0f : v->vel * 0.7f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int perc_render(chan *c, float *A)
{
	int any = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		float y1 = v->f1, y2 = v->f2, z1 = v->idx2, z2 = v->menv2, amp = v->amp, ck = v->menv;
		const float a1 = v->fk, b1 = v->fq, a2 = v->hold, b2 = v->fb, g1 = v->ratio, g2 = v->bend;
		const float ai = v->ainc, cl = v->iinc;
		for (uint32_t j = 0; j < VSUB; j++) {
			float x = ck * (ck > 0.99f ? 1.0f : 0.5f * nb_noise(&v->pm));   /* the strike: an impulse, then ~1 ms of noise */
			ck *= 0.8f;
			float n1 = a1 * y1 - b1 * y2 + g1 * x;
			float n2 = a2 * z1 - b2 * z2 + g2 * x;
			y2 = y1; y1 = n1;
			z2 = z1; z1 = n2;
			A[j] += (n1 + n2 + x * cl * 0.3f) * amp;
			amp += ai;
		}
		v->f1 = y1; v->f2 = y2; v->idx2 = z1; v->menv2 = z2; v->amp = amp; v->menv = ck;
	}
	return any;
}

/* ---- Music box: a sine and two partials (2.76x, 5.4x) dying faster; menv, menv2 their levels ---- */
static void mbox_control(chan *c, int ch, float dt)
{
	const float len = P.ch[ch][CP_LENGTH], tone = eff_tone(ch);
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		nv_perc(v, 0.002f, expmap(len, 0.5f, 4.0f) / speed, 0.4f, 0.0f, dt);
		const float hz = mtof(v->note) * speed * pbend;
		v->ic = hz_inc(hz);
		v->ic2 = hz_inc(hz * 2.756f);
		v->ic3 = hz_inc(hz * 5.404f);
		v->fk = 0.35f * tone + 0.1f;              /* the partials' levels (menv, falling over 0.4 s, sets their life) */
	}
}
static int mbox_render(chan *c, float *A, float *B)
{
	int any = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t p1 = v->pc, p2 = v->pc2, p3 = v->pc3;
		const uint32_t i1 = v->ic, i2 = v->ic2, i3 = v->ic3;
		float amp = v->amp;
		const float l2 = v->menv * v->fk, l3 = v->menv * v->menv * v->fk * 0.5f, ai = v->ainc;
		float *out = v->side ? B : A;
		for (uint32_t j = 0; j < VSUB; j++) {
			out[j] += (sin_u(p1) + l2 * sin_u(p2) + l3 * sin_u(p3)) * amp;
			p1 += i1; p2 += i2; p3 += i3;
			amp += ai;
		}
		v->pc = p1; v->pc2 = p2; v->pc3 = p3; v->amp = amp;
	}
	return any;
}

/* ---- Breath: noise through a narrow band at the note, a little sine under it; a flute's shape
 * (rise, hold, fall). f1, f2 the band's states, fk, fq, hold2 (bend) its a, b, g; pm its noise ---- */
static void breath_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float att = 0.06f + 0.4f * (1.0f - tone), rel = 0.3f + 1.5f * len;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		const float sdt = dt * speed;
		if (v->stage == ST_ATT) {
			v->env += sdt / att;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->hold -= sdt;
			if (v->hold <= 0.0f || !playing)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float hz = mtof(v->note) * speed * pbend;
		float g;
		band_set(hz, hz / (60.0f - 50.0f * chr), &v->fk, &v->fq, &g);
		v->bend = g * 3.2f / (1.0f + 2.0f * (1.0f - chr));   /* (a narrow band passes less noise) */
		v->ic = hz_inc(hz);
		v->ratio = 0.35f * (1.0f - chr);         /* the sine under it */
		const float a = v->env * v->vel * 1.2f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int breath_render(chan *c, float *A)
{
	int any = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		float y1 = v->f1, y2 = v->f2, amp = v->amp;
		const float a = v->fk, b = v->fq, g = v->bend, s = v->ratio, ai = v->ainc;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		for (uint32_t j = 0; j < VSUB; j++) {
			float y = a * y1 - b * y2 + g * nb_noise(&v->pm);
			y2 = y1; y1 = y;
			A[j] += (y + s * sin_u(pc)) * amp;
			pc += ic;
			amp += ai;
		}
		v->f1 = y1; v->f2 = y2; v->amp = amp; v->pc = pc;
	}
	return any;
}

/* ---- Chip: a band-limited pulse (two saws a width apart); past 3/4 of CHARACTER, a 50 Hz
 * arpeggio through the chord's tones above the note ---- */
static void chip_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], len = P.ch[ch][CP_LENGTH];
	static const float W[3] = {0.125f, 0.25f, 0.5f};
	const float w = W[stepped(chr < 0.75f ? chr / 0.75f : 1.0f, 3)];
	const int arp = chr >= 0.75f;
	{	/* the arpeggio's clock: a step each 20 ms */
		const uint32_t was = c->lfo1;
		c->lfo1 += (uint32_t)(dt * 50.0f * 4294967296.0f);
		c->arp_i = (uint8_t)(c->arp_i + (c->lfo1 < was));
	}
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		nv_perc(v, 0.002f, expmap(len, 0.15f, 2.0f) / speed, 1.0f, 0.0f, dt);
		float n = v->note;
		if (arp && chord_n > 1) {
			const int k = (int)(c->arp_i % (uint8_t)chord_n);
			n = (float)place(chord_pc[k], v->note, (int)v->note, (int)v->note + 11);
		}
		v->ic = hz_inc(mtof(n) * speed * pbend);
		v->fk = w;
	}
}
static int chip_render(chan *c, float *A)
{
	int any = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic, wd = (uint32_t)(v->fk * 4294967296.0f);
		float amp = v->amp;
		const float ai = v->ainc;
		for (uint32_t j = 0; j < VSUB; j++) {
			A[j] += 0.6f * (nb_saw(pc, ic) - nb_saw(pc + wd, ic)) * amp;
			pc += ic;
			amp += ai;
		}
		v->pc = pc; v->amp = amp;
	}
	return any;
}

/* ---- Organ and Choir: held chords. Each of three voices sits on a tone of the chord, glides
 * to the new chord's (the organ quickly, the choir slowly); the choir's voices also breathe in
 * and out on their own. cyc: the choir's vowel drift; gap: a voice resting ---- */
static void held_control(chan *c, int ch, float dt)
{
	const int organ = c->voice == GV_ORGAN;
	const int base = reg_base(c, ch);
	const float act = eff_activity(ch), len = P.ch[ch][CP_LENGTH], chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch);
	const float sdt = dt * speed;
	const int nv = organ ? 2 + (act > 0.35f) : 1 + (act > 0.25f) + (act > 0.6f);
	c->lfo1 += (uint32_t)(sdt * (0.75f + 0.3f * chr) * 4294967296.0f);   /* the Leslie, slow */
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (midi_live(ch)) {                      /* played: each voice a key, held while it is */
			if (v->mnote == 0 && (v->stage == ST_ATT || v->stage == ST_DEC) && !arp_live(ch))
				v->stage = ST_REL;
		} else {
		const int want = playing && i < nv && (organ || c->gap[i] <= 0.0f);
		const int pc = chord_pc[i % chord_n];
		const float from = v->note > 0.0f ? v->note : (float)(base + 4 * i);
		const float tgt = (float)ivl_note(ch, (float)place(pc, from, base - 2, base + 14));
		if (v->note <= 0.0f)
			v->note = v->ratio = tgt;
		v->note = tgt;
		v->ratio += (tgt - v->ratio) * (1.0f - dcoef(organ ? 0.03f : 0.35f, sdt));   /* (the gliding pitch) */
		if (want) {
			if (v->stage == ST_IDLE || v->stage == ST_REL) {
				v->stage = ST_ATT;
				v->vel = organ ? 0.55f : 0.45f + 0.15f * rnd();
				v->t = 0.0f;
				notes_fired++;
			}
		} else if (v->stage != ST_IDLE)
			v->stage = ST_REL;
		}
		const float att = organ ? 0.012f : 0.4f + 1.2f * len, rel = organ ? 0.08f : 0.8f + 2.0f * len;
		if (v->stage == ST_ATT) {
			v->env += sdt / att;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		if (!organ && midi_live(ch)) {
			c->cyc[i] += sdt / (9.0f + 5.0f * (float)i);           /* the vowel's drift, no breaths */
			c->cyc[i] -= c->cyc[i] >= 1.0f ? 1.0f : 0.0f;
		} else if (!organ) {
			/* the choir breathes: a phrase of 4 to 20 s (LENGTH), then a rest */
			v->t += sdt;
			if (v->stage == ST_DEC && v->t > 4.0f + 16.0f * len + 2.0f * (float)i) {
				v->stage = ST_REL;
				c->gap[i] = (0.5f + 3.0f * rnd()) * (1.2f - act);
			} else if (v->stage == ST_IDLE && c->gap[i] > 0.0f)
				c->gap[i] -= sdt;
			c->cyc[i] += sdt / (9.0f + 5.0f * (float)i);           /* the vowel's drift */
			c->cyc[i] -= c->cyc[i] >= 1.0f ? 1.0f : 0.0f;
		}
		const float vib = 1.0f + (organ ? 0.0015f : 0.004f) * sin_u(lfo_vib + (uint32_t)i * 0x55555555u) *
		                         (organ ? sin_u(c->lfo1) : 1.0f);
		const float hz = mtof(v->ratio) * speed * pbend * vib;
		v->ic = hz_inc(hz);
		if (organ) {
			/* the drawbars: 8' 4' 2 2/3' 2' (harmonics 1 to 4), from flute to full */
			v->fk = 0.25f + 0.55f * chr;          /* 4' */
			v->fq = chr * chr * 0.6f;             /* 2 2/3' */
			v->hold = clampf(chr - 0.4f, 0.0f, 0.6f) * (0.5f + 0.8f * tone);   /* 2' */
		} else {
			/* two formants, gliding between an oo/oh pair and an ah/eh pair as CHARACTER rises */
			const float u = 0.5f + 0.5f * sin_u((uint32_t)(c->cyc[i] * 4294967296.0f));
			const float f1 = (325.0f + 375.0f * chr) + (125.0f + 100.0f * chr) * u;
			const float f2 = (700.0f + 500.0f * chr) + (200.0f + 450.0f * chr) * u;
			float g1, g2;
			band_set(f1, 90.0f, &v->fk, &v->fq, &g1);
			band_set(f2, 120.0f, &v->hold, &v->fb, &g2);
			v->bend = g1 * 2.6f;
			v->idx = g2 * 1.8f;                   /* (a formant's gain; idx is free in the choir) */
		}
		const float a = v->env * v->vel * (organ ? 0.27f : 0.5f) * clampf(1.0f - (v->ratio - 72.0f) * 0.025f, 0.6f, 1.0f);
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int held_render(chan *c, float *A, float *B)
{
	int any = 0;
	const int organ = c->voice == GV_ORGAN;
	/* the Leslie: the organ's level swaps between the sides (end to end of the block) */
	const float l0 = sin_u(c->lfo1), lsp = 0.55f;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		float amp = v->amp;
		const float ai = v->ainc;
		if (organ) {
			const float d4 = v->fk, d3 = v->fq, d2 = v->hold;
			const float ga = 1.0f + lsp * l0, gb = 1.0f - lsp * l0;
			for (uint32_t j = 0; j < VSUB; j++) {
				float x = sin_u(pc) + d4 * sin_u(2u * pc) + d3 * sin_u(3u * pc) + d2 * sin_u(4u * pc);
				x *= amp;
				A[j] += x * ga;
				B[j] += x * gb;
				pc += ic;
				amp += ai;
			}
		} else {
			float y1 = v->f1, y2 = v->f2, z1 = v->idx2, z2 = v->menv2;
			const float a1 = v->fk, b1 = v->fq, g1 = v->bend, a2 = v->hold, b2 = v->fb, g2 = v->idx;
			float *out = (i & 1) ? B : A;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				const float n1 = a1 * y1 - b1 * y2 + g1 * s;
				const float n2 = a2 * z1 - b2 * z2 + g2 * s;
				y2 = y1; y1 = n1;
				z2 = z1; z1 = n2;
				out[j] += (n1 + n2) * amp;
				pc += ic;
				amp += ai;
			}
			v->f1 = y1; v->f2 = y2; v->idx2 = z1; v->menv2 = z2;
		}
		v->pc = pc; v->amp = amp;
	}
	return any;
}

/* ---- Grains: windows read from the tape echo's memory (glag_space.h). Per grain: pc its place
 * in the tape (whole samples), fk the fraction, fq its rate (tape samples a voice sample), t its
 * window's position, hold the window's step; vel its level ---- */
static uint32_t grain_w(void);
static float grain_read(uint32_t i, float f);
static void grains_step(chan *c, int ch, uint32_t step)
{
	(void)step;
	const float act = eff_activity(ch), chr = P.ch[ch][CP_CHAR], len = P.ch[ch][CP_LENGTH];
	if (rnd() >= act * act * 0.7f)
		return;
	int k = -1;
	for (int i = 0; i < 4 && k < 0; i++)
		if (c->v[i].stage == ST_IDLE)
			k = i;
	if (k < 0)
		return;                                   /* (four already sounding: none is cut) */
	nvoice *v = &c->v[k];
	static const float R[5] = {1.0f, 2.0f, 0.5f, 1.5f, 0.75f};
	const int ri = rnd() < 1.0f - chr ? 0 : 1 + rndi(4);
	const float dur = expmap(len, 0.04f, 0.45f);
	float back = (0.08f + 1.6f * rnd()) * 8000.0f;
	const float need = dur * 8000.0f * (R[ri] - 1.0f) + 200.0f;   /* (a fast grain must not overtake the head) */
	back = back > need ? back : need;
	v->pc = grain_w() - (uint32_t)back;
	v->fk = 0.0f;
	v->fq = R[ri] * (8000.0f / VSR) * (1.0f + 0.004f * (rnd() - 0.5f));
	v->t = 0.0f;
	v->hold = 1.0f / (dur * VSR);
	v->vel = 0.5f + 0.4f * rnd();
	v->side = (uint8_t)(rnd32() & 1u);
	v->stage = ST_DEC;
	notes_fired++;
	c->glow = v->vel;
}
/* keys held on a Grains channel (MIDI): a cloud of grains at that pitch while held */
static uint8_t gr_key[GL_NCH][4];
static void grains_note(chan *c, int ch, int note, float vel);
static void grains_keys(chan *c, int ch, float dt)
{
	for (int k = 0; k < 4; k++)
		if (gr_key[ch][k] && rnd() < dt * (6.0f + 30.0f * eff_activity(ch) * eff_activity(ch)))
			grains_note(c, ch, gr_key[ch][k] - 1, 0.7f);
}
/* a grain played by a key: read back at the key's distance from middle C (an octave each way at
 * most, as the fast grains must not overtake the tape's head) */
static void grains_note(chan *c, int ch, int note, float vel)
{
	int k = 0;
	for (int i = 0; i < 4; i++)
		if (c->v[i].stage == ST_IDLE) {
			k = i;
			break;
		} else if (c->v[i].t > c->v[k].t)
			k = i;                                    /* (else the oldest) */
	nvoice *v = &c->v[k];
	const float r = clampf(fexp2((float)(note - 60) * (1.0f / 12.0f)), 0.5f, 2.0f);
	const float dur = expmap(P.ch[ch][CP_LENGTH], 0.04f, 0.45f);
	float back = (0.08f + 1.6f * P.ch[ch][CP_CHAR] * rnd()) * 8000.0f;
	const float need = dur * 8000.0f * (r - 1.0f) + 200.0f;
	back = back > need ? back : need;
	v->pc = grain_w() - (uint32_t)back;
	v->fk = 0.0f;
	v->fq = r * (8000.0f / VSR);
	v->t = 0.0f;
	v->hold = 1.0f / (dur * VSR);
	v->vel = 2.5f * (0.4f + 0.5f * vel);           /* (the tape is quiet: keyed grains stand out of it) */
	v->side = (uint8_t)(rnd32() & 1u);
	v->stage = ST_DEC;
	notes_fired++;
	c->glow = v->vel;
}
static int grains_render(chan *c, float *A, float *B)
{
	int any = 0;
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE)
			continue;
		any = 1;
		uint32_t p = v->pc;
		float f = v->fk, t = v->t;
		const float r = v->fq, dt = v->hold, g = v->vel * 4.0f;
		float *out = v->side ? B : A;
		uint32_t j = 0;
		for (; j < VSUB && t < 1.0f; j++) {
			const float s = sin_u((uint32_t)(t * 2147483648.0f));
			out[j] += grain_read(p, f) * s * s * g;
			f += r;
			const uint32_t w = (uint32_t)f;
			p += w;
			f -= (float)w;
			t += dt;
		}
		v->pc = p; v->fk = f; v->t = t;
		if (t >= 1.0f)
			v->stage = ST_IDLE;
	}
	return any;
}

/* ================================================================ the Lite Kit */

typedef struct {
	float k_ph, k_f, k_fend, k_a, k_k;           /* kick: phase, pitch, its end, level, decay */
	float s_a, s_k, s_ph, s_t, s_hp;             /* snare: noise, its decay, the tone's phase and level, a high-pass */
	float h_a, h_k, h_lp, h_hp;                  /* hats: level, decay (closed / open), filters */
	float c_a, c_k, c_y1, c_y2;                  /* clap: level, decay, its band */
	float r_y1, r_y2, r_a, r_b, r_g, r_x;        /* rim / clave: the resonator, its strike */
	float t_ph, t_f, t_fend, t_a, t_k;           /* tom */
	float b_p1, b_p2, b_i1, b_i2, b_a, b_k, b_y1, b_y2;   /* cowbell: two squares, level, decay, a band */
	uint32_t rng;
	int c_left, c_n;                             /* the clap's bursts still to come, samples to the next */
	uint16_t pend;                               /* a swung hit waiting */
	float pend_t;
	uint8_t fill, drop, phrase_seen;
	float tone, len;
} lk_state;
_Static_assert(sizeof(lk_state) <= sizeof(kit_state), "the Lite Kit lives in the Kit's room");
#define LK(c) ((lk_state *)(void *)&(c)->kt)
/* lanes (bits): kick, snare, closed hat, open hat, clap, rim, tom, bell */
enum { LK_K = 1, LK_S = 2, LK_H = 4, LK_O = 8, LK_C = 16, LK_R = 32, LK_T = 64, LK_B = 128 };
/* five grooves, each lane's sixteen steps (bit n: step n), and its swing */
static const struct { uint16_t k, s, h, o, c, r, b; float swing; } LK_G[5] = {
	{0x1111, 0x0000, 0x5555, 0x4444, 0x1010, 0x0000, 0x0000, 0.0f},   /* four on the floor */
	{0x0425, 0x1010, 0x5555, 0x0000, 0x0000, 0x8080, 0x0000, 0.18f},  /* breaks */
	{0x1111, 0x0000, 0x5555, 0x0000, 0x0000, 0x0849, 0x2222, 0.0f},   /* clave: son 3-2, a bell */
	{0x0101, 0x0000, 0x2222, 0x0000, 0x1010, 0x4904, 0x0000, 0.1f},   /* minimal: rim, a clap */
	{0x0001, 0x0100, 0x4444, 0x0000, 0x0000, 0x0000, 0x0000, 0.25f},  /* dub: half time */
};
static void lk_fire(chan *c, int ch, int m, float vel)
{
	lk_state *L = LK(c);
	const float t = L->tone, len = L->len, kp = fexp2((float)(stepped(P.ch[ch][CP_REGISTER], 5) - 2) * 0.5f);
	if (m & LK_K) {
		L->k_ph = 0.0f;
		L->k_f = 160.0f * kp;
		L->k_fend = 46.0f * kp;
		L->k_a = vel;
		L->k_k = fexp2(-9.9658f / ((0.15f + 0.45f * len) * VSR));
		pump_tgt = 1.0f;
	}
	if (m & LK_S) {
		L->s_a = vel * 0.55f;
		L->s_k = fexp2(-9.9658f / ((0.1f + 0.25f * len) * VSR));
		L->s_t = vel * 0.6f;
		L->s_ph = 0.0f;
	}
	if (m & (LK_H | LK_O)) {
		L->h_a = vel * (0.25f + 0.15f * t);
		L->h_k = fexp2(-9.9658f / (((m & LK_O) ? 0.25f + 0.35f * len : 0.03f + 0.04f * len) * VSR));
	}
	if (m & LK_C) {
		L->c_left = 3;
		L->c_n = 0;
		L->c_a = 0.0f;
		L->c_k = fexp2(-9.9658f / (0.012f * VSR));
	}
	if (m & LK_R) {
		/* on a chord tone two octaves up: a clave's wood (CHARACTER's upper grooves) or a rim */
		const float hz = mtof((float)(84 + chord_pc[rndi(chord_n)])) * (L->tone > 0.5f ? 1.0f : 0.5f);
		res_set(hz, 0.03f + 0.05f * len, &L->r_a, &L->r_b, &L->r_g);
		L->r_x = vel * 0.7f;
	}
	if (m & LK_T) {
		const float hz = mtof((float)(43 + chord_pc[rndi(chord_n)]) + 12.0f * (rnd() < 0.5f));
		L->t_f = hz * 1.5f;
		L->t_fend = hz;
		L->t_a = vel * 0.8f;
		L->t_ph = 0.0f;
		L->t_k = fexp2(-9.9658f / ((0.2f + 0.4f * len) * VSR));
	}
	if (m & LK_B) {
		const float hz = mtof((float)(72 + chord_pc[0]));
		L->b_i1 = hz / VSR;
		L->b_i2 = hz * 1.48f / VSR;
		L->b_a = vel * 0.32f;
		L->b_k = fexp2(-9.9658f / ((0.12f + 0.3f * len) * VSR));
	}
	notes_fired++;
	c->glow = vel;
}
static void lk_step(chan *c, int ch, uint32_t step)
{
	lk_state *L = LK(c);
	const float h = eff_activity(ch);
	const int g = stepped(P.ch[ch][CP_CHAR], 5), s16 = (int)(step % 16u), bar = (int)((step / 16u) % 4u);
	L->tone = eff_tone(ch);
	L->len = P.ch[ch][CP_LENGTH];
	if (s16 == 0 && bar == 0) {
		/* the director, each four bars: a fill in the last half of the fourth bar, now and then
		 * a dropout (no kick) in the second; answered, if Answer is on */
		L->fill = rnd() < 0.35f + 0.4f * h;
		L->drop = rnd() < 0.2f;
	}
	int m = 0;
	const int bit = 1 << s16;
	if ((LK_G[g].k & bit) && !(L->drop && bar == 1))
		m |= LK_K;
	if (h > 0.2f && (LK_G[g].s & bit))
		m |= LK_S;
	if (h > 0.15f && (LK_G[g].h & bit))
		m |= LK_H;
	if (h > 0.6f && (LK_G[g].o & bit))
		m |= LK_O;
	if (h > 0.25f && (LK_G[g].c & bit))
		m |= LK_C;
	if (h > 0.3f && (LK_G[g].r & bit))
		m |= LK_R;
	if (h > 0.5f && (LK_G[g].b & bit))
		m |= LK_B;
	/* ghosts: a snare on an off sixteenth, a hat on a gap, more with ACTIVITY */
	if ((s16 & 1) && rnd() < 0.1f * h * h)
		m |= LK_S | 0x100;
	if (!(m & LK_H) && h > 0.5f && rnd() < 0.2f * h)
		m |= LK_H | 0x100;
	if (L->fill && bar == 3 && s16 >= 8) {
		m &= LK_K | LK_H;
		if (!(s16 & 1))
			m |= LK_T;
		if (s16 >= 12)
			m |= LK_S;
		if (s16 == 8)
			answer_call();
	}
	if (form_fill && s16 >= 8 && !(s16 & 1))
		m |= LK_S | LK_T;
	if (!(m & 0xFF))
		return;
	/* Breathe (importance thinning): the hits that matter least go first */
	if (thin_cap > 0) {
		const float what = (m & LK_K) ? 1.0f : (m & (LK_S | LK_C)) ? 0.8f : 0.45f;
		if (thin_drop(what * ((m & 0x100) ? 0.5f : 1.0f), 0.8f))
			m &= LK_K;
		if (!(m & 0xFF))
			return;
	}
	const float vel = ((m & 0x100) ? 0.35f : (s16 & 3) == 0 ? 1.0f : 0.8f) * (0.85f + 0.15f * rnd());
	if ((step & 1u) && LK_G[g].swing > 0.01f) {
		L->pend = (uint16_t)(m & 0xFF);
		L->pend_t = LK_G[g].swing * 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		c->pend_t = vel;                          /* (its velocity, kept) */
		return;
	}
	lk_fire(c, ch, m & 0xFF, vel);
}
static void lk_control(chan *c, int ch, float dt)
{
	lk_state *L = LK(c);
	if (L->pend) {
		L->pend_t -= dt * speed;
		if (L->pend_t <= 0.0f) {
			lk_fire(c, ch, L->pend, c->pend_t);
			L->pend = 0;
		}
	}
	c->glow *= 0.95f;
}
static int lk_render(chan *c, float *A, float *B)
{
	lk_state *L = LK(c);
	int any = 0;
	const float lpk = 0.25f + 0.5f * L->tone;
	for (uint32_t j = 0; j < VSUB; j++) {
		float mid = 0.0f, left = 0.0f, right = 0.0f;
		if (L->k_a > 1e-4f) {
			L->k_ph += L->k_f * (1.0f / VSR);
			L->k_ph -= L->k_ph >= 1.0f ? 1.0f : 0.0f;
			L->k_f += (L->k_fend - L->k_f) * 0.0016f;
			mid += sin_u((uint32_t)(L->k_ph * 4294967296.0f)) * L->k_a * 1.6f;
			L->k_a *= L->k_k;
			any = 1;
		}
		if (L->s_a > 1e-4f || L->s_t > 1e-4f) {
			const float n = nb_noise(&L->rng);
			L->s_hp += 0.3f * (n - L->s_hp);
			L->s_ph += 185.0f / VSR;
			L->s_ph -= L->s_ph >= 1.0f ? 1.0f : 0.0f;
			mid += (n - L->s_hp) * L->s_a + sin_u((uint32_t)(L->s_ph * 4294967296.0f)) * L->s_t;
			L->s_a *= L->s_k;
			L->s_t *= 0.9975f;
			any = 1;
		}
		if (L->h_a > 1e-4f) {
			const float n = nb_noise(&L->rng);
			L->h_lp += 0.45f * (n - L->h_lp);
			const float hp = n - L->h_lp;
			L->h_hp += lpk * (hp - L->h_hp);
			left += L->h_hp * L->h_a;
			L->h_a *= L->h_k;
			any = 1;
		}
		if (L->c_left > 0 || L->c_a > 1e-4f) {
			if (L->c_left > 0 && --L->c_n <= 0) {   /* the next of the three bursts, 9 ms apart */
				L->c_a = 0.5f;
				L->c_left--;
				L->c_n = (int)(0.009f * VSR);
				L->c_k = L->c_left ? fexp2(-9.9658f / (0.012f * VSR)) : fexp2(-9.9658f / ((0.08f + 0.15f * L->len) * VSR));
			}
			const float n = nb_noise(&L->rng);
			/* a band at ~1.2 kHz (r 0.93) */
			const float y = 1.769f * L->c_y1 - 0.8649f * L->c_y2 + 0.14f * n;   /* (2 r cos w, r^2; the gain for ~0.25 rms) */
			L->c_y2 = L->c_y1;
			L->c_y1 = y;
			right += y * L->c_a;
			L->c_a *= L->c_k;
			any = 1;
		}
		if (L->r_x > 0.0f || L->r_y1 * L->r_y1 + L->r_y2 * L->r_y2 > 1e-9f) {
			const float y = L->r_a * L->r_y1 - L->r_b * L->r_y2 + L->r_g * L->r_x;
			L->r_x = 0.0f;
			L->r_y2 = L->r_y1;
			L->r_y1 = y;
			right += y;
			any = 1;
		}
		if (L->t_a > 1e-4f) {
			L->t_ph += L->t_f * (1.0f / VSR);
			L->t_ph -= L->t_ph >= 1.0f ? 1.0f : 0.0f;
			L->t_f += (L->t_fend - L->t_f) * 0.0008f;
			mid += sin_u((uint32_t)(L->t_ph * 4294967296.0f)) * L->t_a;
			L->t_a *= L->t_k;
			any = 1;
		}
		if (L->b_a > 1e-4f) {
			L->b_p1 += L->b_i1;
			L->b_p1 -= L->b_p1 >= 1.0f ? 1.0f : 0.0f;
			L->b_p2 += L->b_i2;
			L->b_p2 -= L->b_p2 >= 1.0f ? 1.0f : 0.0f;
			const float x = (L->b_p1 < 0.5f ? 1.0f : -1.0f) + (L->b_p2 < 0.5f ? 1.0f : -1.0f);
			const float y = 1.78f * L->b_y1 - 0.828f * L->b_y2 + 0.04f * x;   /* (a band near 800 Hz, r 0.91, its peak at about 1) */
			L->b_y2 = L->b_y1;
			L->b_y1 = y;
			left += y * L->b_a;
			L->b_a *= L->b_k;
			any = 1;
		}
		A[j] += 0.62f * mid + 1.25f * left;
		B[j] += 0.62f * mid + 1.25f * right;
	}
	return any;
}
