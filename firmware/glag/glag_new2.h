/* SPDX-License-Identifier: MIT
 * Glag: six more audition voices (after the nine of glag_new.h; appended to the enum, so no piece
 * changes). Included into glag_engine.c after glag_new.h; uses its helpers (nb_saw, nb_noise,
 * res_set, band_set). Costs are estimates, not measured.
 *
 *   Muted Trumpet  a saw through a Harmon mute: a narrow band (CHARACTER: cup .. harmon) over a
 *                  little of the open horn, a scoop into each note, a late vibrato; plays phrases
 *                  like a soloist and rests between them (est. 1)
 *   Timpani        two struck membrane modes (1, 1.5) from a soft mallet's pulse (CHARACTER: soft
 *                  felt .. hard wood), the pitch settling after a hard hit; strikes the chord's root,
 *                  rolls into the end of a phrase (est. 1)
 *   Sax            a saw through a low-pass that opens with the breath, breath noise riding the
 *                  waveform (CHARACTER: how breathy), a scoop and a vibrato; phrases (est. 1)
 *   Cello          one bowed saw: a slow bow, a body resonance (CHARACTER: sul tasto .. ponticello),
 *                  a little rosin, a late vibrato; long legato lines (est. 1.5)
 *   Upright        a plucked string (the Kalimba's, two of them), dark and round; walks the bar in
 *                  quarter notes (CHARACTER: roots and fifths .. walking with approach notes), each
 *                  note stopping the last (est. 1)
 *   Haze           grains of the reverb's tail: its last four seconds kept and replayed, a slice at
 *                  a time, late, slow, an octave down or reversed (CHARACTER: how far back and how
 *                  strange). The reverb's memory is only kept in the audition builds (est. 1)
 */

/* ---- the horns (Muted Trumpet, Sax): f1 f2 a low-pass's states, fk its coefficient; the
 * trumpet's mute: idx2 menv2 a band's states, fq fb its a and b, idx its gain; hold: seconds
 * left before the breath stops; t: seconds since the note began ---- */
static void horn_control(chan *c, int ch, float dt)
{
	const int tpt = c->voice == GV_TRUMPET;
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	const float att = tpt ? 0.035f : 0.06f, rel = 0.08f + 0.25f * len;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		v->t += sdt;
		if (v->stage == ST_ATT) {
			v->env += sdt / att;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->env += (0.82f - v->env) * (1.0f - dcoef(0.4f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f && !v->mnote)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		/* a scoop up into the note, then a vibrato that comes in late */
		const float scoop = (tpt ? -0.45f : -1.1f) * fexp2(-v->t * 30.0f);
		const float vd = clampf((v->t - 0.3f) * 2.0f, 0.0f, 1.0f) * (tpt ? 0.0025f : 0.0045f);
		const float hz = mtof(v->note + scoop) * speed * pbend * (1.0f + vd * sin_u(lfo_vib + (uint32_t)i * 0x3000000u));
		v->ic = hz_inc(hz);
		const float bright = v->env * (0.55f + 0.45f * v->vel);
		float fc = hz * (tpt ? 1.5f + (3.0f + 9.0f * tone) * bright : 1.2f + (2.0f + 7.0f * tone) * bright);
		fc = clampf(fc, 60.0f, 9000.0f);
		v->fk = 1.0f - fexp2(-1.4427f * 6.2832f * fc / VSR);
		if (tpt) {
			/* the Harmon mute: a nasal band at 1.1 to 2 kHz, narrower toward harmon */
			const float fm = 1100.0f + 900.0f * chr;
			float g;
			band_set(fm, fm * (0.55f - 0.3f * chr), &v->fq, &v->fb, &g);
			v->idx = g * (2.2f + 2.0f * chr);
		} else
			v->idx = chr;                         /* the sax: how breathy */
		const float a = v->env * v->vel * (tpt ? 0.5f : 0.85f);
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int horn_render(chan *c, float *A)
{
	int any = 0;
	const int tpt = c->voice == GV_TRUMPET;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		float amp = v->amp, y1 = v->f1, y2 = v->f2;
		const float k = v->fk, ai = v->ainc;
		if (tpt) {
			float z1 = v->idx2, z2 = v->menv2;
			const float ba = v->fq, bb = v->fb, bg = v->idx;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				y1 += k * (s - y1);
				y2 += k * (y1 - y2);
				const float z = ba * z1 - bb * z2 + bg * y1;
				z2 = z1; z1 = z;
				A[j] += (0.3f * y2 + z) * amp;
				pc += ic;
				amp += ai;
			}
			v->idx2 = z1; v->menv2 = z2;
		} else {
			const float br = v->idx * 0.5f;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				const float n = nb_noise(&v->pm) * br * (0.6f + 0.4f * s);   /* breath riding the reed */
				y1 += k * (s + n - y1);
				y2 += k * (y1 - y2);
				A[j] += (y2 + 0.5f * n) * amp;
				pc += ic;
				amp += ai;
			}
		}
		v->pc = pc; v->amp = amp; v->f1 = y1; v->f2 = y2;
	}
	return any;
}
/* a soloist: a phrase of two to eight notes moving mostly by step, the last one held, then a rest */
static void horn_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int div = act < 0.45f ? 4 : 2;
	if (step % (uint32_t)div)
		return;
	if (c->arp_i == 0) {
		if (c->arp_dir) {
			c->arp_dir--;
			return;
		}
		if (rnd() >= 0.2f + 0.5f * act)
			return;
		c->arp_i = (uint8_t)(2 + rndi(2 + (int)(5.0f * act)));
		c->last_note = pick_note(c, base - 3, base + 9, 0.7f);
	}
	const int last = c->arp_i == 1;
	int note = c->last_note;
	if (c->hq_n++)                                 /* (past the first: move by a step or two) */
		note = pick_note(c, c->last_note - 3, c->last_note + 4, last ? 0.9f : 0.35f);
	const float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	const float len = P.ch[ch][CP_LENGTH];
	nvoice *v = alloc(c, 2);
	if (!sounding(v))
		make_room(v);
	nv_trigger(v, (float)note, (0.6f + 0.3f * rnd()) * ((step & 7u) == 0 ? 1.0f : 0.85f), 1.0f, 0);
	v->hold = last ? expmap(len, 0.4f, 2.5f) : (float)div * step_s * (0.55f + 0.5f * len);
	c->glow = v->vel;
	if (--c->arp_i == 0) {
		c->hq_n = 0;
		c->arp_dir = (uint8_t)((4 + rndi(12)) * (1.3f - act));
	}
}

/* ---- Timpani: per voice, two modes: f1 f2 the first's states, fk fq its a b, ratio its gain;
 * idx2 menv2 the second's, hold fb its a b, bend its gain; menv the mallet's pulse, iinc its decay
 * a sample, idx the hard mallet's noise. A roll waits in hq_n (strikes left), hq_t, hq_dt, hq_vel ---- */
static void timp_strike(chan *c, int ch, int note, float vel)
{
	nvoice *v = &c->v[c->vnext++ % 3u];
	const float keep_f1 = v->f1, keep_f2 = v->f2, keep_z1 = v->idx2, keep_z2 = v->menv2;
	const int same = v->stage != ST_IDLE && (int)v->note == note;
	nv_trigger(v, (float)note, vel, 1.0f, 0);
	if (same) {                                   /* struck again while it rings: it adds */
		v->f1 = keep_f1; v->f2 = keep_f2; v->idx2 = keep_z1; v->menv2 = keep_z2;
	} else
		v->f1 = v->f2 = v->idx2 = v->menv2 = 0.0f;
	(void)ch;
	c->glow = vel;
}
static void timp_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	if (c->hq_n) {                                /* a roll: strikes every ~45 ms, growing */
		c->hq_t -= sdt;
		if (c->hq_t <= 0.0f) {
			timp_strike(c, ch, c->last_note, c->hq_vel);
			c->hq_vel = clampf(c->hq_vel * 1.07f, 0.0f, 0.9f);
			c->hq_t += c->hq_dt * (0.85f + 0.3f * rnd());
			c->hq_n--;
		}
	}
	const float t60 = 1.2f + 3.5f * len;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f)
			continue;
		if (v->stage == ST_ATT) {
			v->stage = ST_DEC;
			v->t = 0.0f;
			v->menv = 0.4f + 0.6f * v->vel;
		}
		v->t += sdt;
		/* a hard hit starts a little sharp and settles */
		const float hz = mtof(v->note + 0.25f * v->vel * chr * fexp2(-v->t * 12.0f)) * speed * pbend;
		float g1, g2;
		res_set(hz, t60, &v->fk, &v->fq, &g1);
		res_set(hz * 1.504f, t60 * 0.45f, &v->hold, &v->fb, &g2);
		v->ratio = g1;
		v->bend = g2 * (0.25f + 0.5f * chr);
		/* the mallet: its pulse dies in 6 ms (felt) to 1 ms (wood) */
		v->iinc = fexp2(-9.9658f / ((0.006f - 0.005f * chr) * VSR));
		v->idx = 0.35f * chr;
		if (v->t > t60 * 1.2f)
			v->stage = ST_IDLE;
		const float a = v->stage == ST_IDLE ? 0.0f : 0.9f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int timp_render(chan *c, float *A)
{
	int any = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		float y1 = v->f1, y2 = v->f2, z1 = v->idx2, z2 = v->menv2, amp = v->amp, m = v->menv;
		const float a1 = v->fk, b1 = v->fq, g1 = v->ratio, a2 = v->hold, b2 = v->fb, g2 = v->bend;
		const float mk = v->iinc, nz = v->idx, ai = v->ainc;
		const float xs = (1.0f - mk) * 1.1f;          /* (the pulse's area: the same strike whatever the mallet) */
		for (uint32_t j = 0; j < VSUB; j++) {
			const float x = m * xs * (1.0f + nz * nb_noise(&v->pm));
			m *= mk;
			const float n1 = a1 * y1 - b1 * y2 + g1 * x;
			const float n2 = a2 * z1 - b2 * z2 + g2 * x;
			y2 = y1; y1 = n1;
			z2 = z1; z1 = n2;
			A[j] += (n1 + n2) * amp;
			amp += ai;
		}
		v->f1 = y1; v->f2 = y2; v->idx2 = z1; v->menv2 = z2; v->amp = amp; v->menv = m;
	}
	return any;
}
static void timp_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int s64 = (int)(step % 64u);
	if ((step & 15u) == 0u && (c->seen_chord != chord_count || rnd() < 0.3f * act)) {
		c->seen_chord = chord_count;
		c->hq_n = 0;                              /* (a roll lands here) */
		c->last_note = place(chord_pc[0], (float)base, base - 5, base + 7);
		timp_strike(c, ch, c->last_note, 0.85f + 0.1f * rnd());
		return;
	}
	if ((step & 15u) == 8u && rnd() < 0.5f * act) {       /* the fifth, below, on beat three */
		const int n = place((chord_pc[0] + 7) % 12, (float)(base - 4), base - 9, base + 3);
		timp_strike(c, ch, n, 0.5f + 0.2f * rnd());
		return;
	}
	if (s64 == 56 && act > 0.45f && rnd() < act) {        /* a roll into the next phrase */
		c->hq_n = (uint8_t)(0.5f * 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]) * 2.0f / 0.045f);
		c->hq_n = c->hq_n > 60 ? 60 : c->hq_n;
		c->hq_t = 0.0f;
		c->hq_dt = 0.045f;
		c->hq_vel = 0.2f;
	}
}

/* ---- Cello: one bowed saw; f1 f2 the low-pass's states, fk its coefficient; the body:
 * idx2 menv2 its states, fq fb its a b, idx its gain; hold, t as the horns' ---- */
static void cello_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	const float att = 0.08f + 0.3f * (1.0f - tone), rel = 0.25f + 0.6f * len;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		v->t += sdt;
		if (v->stage == ST_ATT) {
			v->env += sdt / att;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->env += (0.85f - v->env) * (1.0f - dcoef(0.8f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f && !v->mnote)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float vd = clampf((v->t - 0.25f) * 1.5f, 0.0f, 1.0f) * 0.004f;
		const float hz = mtof(v->note) * speed * pbend * (1.0f + vd * sin_u(lfo_vib * 1u + (uint32_t)i * 0x5000000u));
		v->ic = hz_inc(hz);
		const float fc = clampf(hz * (2.0f + (3.0f + 8.0f * (0.3f + 0.7f * chr) * (0.4f + 0.6f * tone)) * v->env), 80.0f, 8000.0f);
		v->fk = 1.0f - fexp2(-1.4427f * 6.2832f * fc / VSR);
		float g;
		const float fb = 500.0f + 1800.0f * chr;     /* the body (tasto) or the bridge (ponticello) */
		band_set(fb, 250.0f + 400.0f * chr, &v->fq, &v->fb, &g);
		v->idx = g * 2.0f;
		const float a = v->env * v->vel * 0.5f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int cello_render(chan *c, float *A, int ch)
{
	int any = 0;
	const float ros = 0.02f + 0.04f * P.ch[ch][CP_CHAR];
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		float amp = v->amp, y1 = v->f1, y2 = v->f2, z1 = v->idx2, z2 = v->menv2;
		const float k = v->fk, ba = v->fq, bb = v->fb, bg = v->idx, ai = v->ainc;
		for (uint32_t j = 0; j < VSUB; j++) {
			const float s = nb_saw(pc, ic) + ros * nb_noise(&v->pm);
			y1 += k * (s - y1);
			y2 += k * (y1 - y2);
			const float z = ba * z1 - bb * z2 + bg * y1;
			z2 = z1; z1 = z;
			A[j] += (0.7f * y2 + z) * amp;
			pc += ic;
			amp += ai;
		}
		v->pc = pc; v->amp = amp; v->f1 = y1; v->f2 = y2; v->idx2 = z1; v->menv2 = z2;
	}
	return any;
}
/* long bowed notes on the chord, one into the next; busier, a pickup between */
static void cello_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int every = act < 0.35f ? 32 : act < 0.7f ? 16 : 8;
	const int on = (step % (uint32_t)every) == 0u || c->seen_chord != chord_count;
	const int pickup = act > 0.6f && (step & 15u) == 14u && rnd() < 0.4f * act;
	if (!on && !pickup)
		return;
	if (on && (step & 3u))
		return;                                   /* (a new chord: on its beat) */
	c->seen_chord = chord_count;
	const int note = pickup ? pick_note(c, c->last_note - 3, c->last_note + 3, 0.2f) : pick_note(c, base - 5, base + 7, 0.85f);
	const float step_s = 15.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	for (int i = 0; i < 3; i++)                    /* the bow changes: the last one lets go */
		if (c->v[i].stage == ST_DEC && !c->v[i].mnote)
			c->v[i].hold = 0.0f;
	nvoice *v = alloc(c, 3);
	if (!sounding(v))
		make_room(v);
	nv_trigger(v, (float)note, pickup ? 0.5f : 0.7f + 0.2f * rnd(), 1.0f, 0);
	v->hold = pickup ? 2.0f * step_s : (float)every * step_s * (0.6f + 0.6f * P.ch[ch][CP_LENGTH]);
	c->glow = v->vel;
}

/* ---- Upright: the Kalimba's plucked strings, two of them, dark; each note stops the last ---- */
static void upright_pluck(chan *c, int ch, int note, float vel)
{
	const float tone = eff_tone(ch);
	const int k = c->ks_next & 1, o = k ^ 1;
	if (c->ks_len[o]) {                           /* the last note stopped by the finger */
		c->ks_g[o] = fexp2(-9.9658f * (float)c->ks_len[o] / (VSR * 0.09f));
		c->ks_kd[o] = fexp2(-9.9658f * (float)GL_SUB / (GL_SR * 0.09f));
	}
	c->ks_next = (uint8_t)o;
	ks_pluck(c, k, mtof(ivl_note(ch, (float)note)) * speed * pbend, vel * 2.6f, 0.03f + 0.2f * tone, 0.5f + 0.12f * tone,
	         expmap(P.ch[ch][CP_LENGTH], 0.8f, 4.0f), 1);
	c->last_note = note;
	c->glow = vel;
}
static void upright_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int every = act < 0.35f ? 8 : 4;
	const int b = (int)(step & 15u);
	const int ghost = act > 0.8f && b == 14 && rnd() < 0.5f;
	if ((step % (uint32_t)every) && !ghost)
		return;
	const float chr = P.ch[ch][CP_CHAR];
	const int lo = base - 5, hi = base + 7;
	int note;
	if (ghost)
		note = c->last_note;
	else if (b == 0)
		note = place(chord_pc[0], (float)base, lo, hi);
	else if (b == 12 && rnd() < chr) {
		/* an approach: a step above or below the root it is heading back to */
		const int root = place(chord_pc[0], (float)c->last_note, lo, hi);
		const int d = rnd() < 0.5f * chr ? 1 : 2;
		note = root + (rnd() < 0.5f ? d : -d);
	} else if (rnd() < 1.0f - chr)
		note = place(rnd() < 0.6f ? chord_pc[0] : (chord_pc[0] + 7) % 12, (float)c->last_note, lo, hi);
	else
		note = pick_note(c, c->last_note - 4, c->last_note + 4, 0.6f);
	upright_pluck(c, ch, note, ghost ? 0.3f : (b == 0 || b == 8) ? 0.85f : 0.65f + 0.1f * rnd());
}

/* ---- Haze: grains of the reverb's tail. Its last four seconds at 8 kHz (written from the space,
 * only while a channel plays Haze, only in the audition builds). Per grain as the Grains': pc its
 * place, fk the fraction, fq its rate (negative: reversed), t its window, hold the window's step ---- */
#ifdef GL_DEMO_PIECES
#define HZ_N 32768u
static float hz_ring[HZ_N];
static uint32_t hz_ph;
static float hz_acc;
#else
#define HZ_N 1u
static float hz_ring[HZ_N];
#endif
static uint32_t hz_w;
static void haze_write(const float *L, const float *R, uint32_t n)
{
#ifdef GL_DEMO_PIECES
	if (CH[0].voice != GV_HAZE && CH[1].voice != GV_HAZE && CH[2].voice != GV_HAZE && CH[3].voice != GV_HAZE)
		return;
	for (uint32_t j = 0; j < n; j++) {
		hz_acc += L[j] + R[j];
		if (++hz_ph >= 3u) {                       /* 24 kHz to 8: three averaged */
			hz_ph = 0;
			hz_ring[++hz_w & (HZ_N - 1u)] = hz_acc * (1.0f / 6.0f);
			hz_acc = 0.0f;
		}
	}
#else
	(void)L; (void)R; (void)n;
#endif
}
static float haze_read(uint32_t i, float f)
{
	const float a = hz_ring[i & (HZ_N - 1u)], b = hz_ring[(i + 1u) & (HZ_N - 1u)];
	return a + (b - a) * f;
}
static void haze_grain(chan *c, int ch, float r, float vel)
{
	const float chr = P.ch[ch][CP_CHAR], len = P.ch[ch][CP_LENGTH];
	int k = 0;
	for (int i = 0; i < 4; i++)
		if (c->v[i].stage == ST_IDLE) {
			k = i;
			break;
		} else if (c->v[i].t > c->v[k].t)
			k = i;
	nvoice *v = &c->v[k];
	const float dur = expmap(len, 0.25f, 1.6f);
	const float span = dur * 8000.0f * (r < 0.0f ? -r : r);
	float back = (0.3f + 3.0f * chr * rnd()) * 8000.0f;
	if (r > 1.0f && back < dur * 8000.0f * (r - 1.0f) + 200.0f)
		back = dur * 8000.0f * (r - 1.0f) + 200.0f;      /* a fast grain must not overtake the head */
	if (back + span > 31000.0f)
		back = 31000.0f - span;
	if (back < 200.0f)
		back = 200.0f;
	v->pc = hz_w - (uint32_t)back;
	v->fk = 0.0f;
	v->fq = r * (8000.0f / VSR);
	v->t = 0.0f;
	v->hold = 1.0f / (dur * VSR);
	v->vel = vel;
	v->side = (uint8_t)(rnd32() & 1u);
	v->stage = ST_DEC;
	notes_fired++;
	c->glow = vel * 0.5f;
}
static void haze_step(chan *c, int ch, uint32_t step)
{
	(void)step;
	const float act = eff_activity(ch), chr = P.ch[ch][CP_CHAR];
	if (rnd() >= act * act * 0.35f)
		return;
	/* as it is, an octave down, reversed, and (stranger) a fifth up or reversed an octave down */
	static const float R[5] = {1.0f, 0.5f, -1.0f, 1.5f, -0.5f};
	const int ri = rnd() < 1.0f - chr ? (rnd() < 0.5f ? 0 : 1) : rndi(5);
	haze_grain(c, ch, R[ri], 0.6f + 0.4f * rnd());
}
static void haze_note(chan *c, int ch, int note, float vel)
{
	haze_grain(c, ch, clampf(fexp2((float)(note - 60) * (1.0f / 12.0f)), 0.25f, 2.0f), 0.5f + 0.5f * vel);
}
static int haze_render(chan *c, float *A, float *B)
{
	int any = 0;
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE)
			continue;
		any = 1;
		uint32_t p = v->pc;
		float f = v->fk, t = v->t;
		const float r = v->fq, dt = v->hold, g = v->vel * 3.0f;
		float *out = v->side ? B : A;
		for (uint32_t j = 0; j < VSUB && t < 1.0f; j++) {
			const float s = sin_u((uint32_t)(t * 2147483648.0f));
			out[j] += haze_read(p, f) * s * s * g;
			f += r;
			const int32_t w = (int32_t)(f >= 0.0f ? f : f - 1.0f);   /* (floor: reversed grains step back) */
			p += (uint32_t)w;
			f -= (float)w;
			t += dt;
		}
		v->pc = p; v->fk = f; v->t = t;
		if (t >= 1.0f)
			v->stage = ST_IDLE;
	}
	return any;
}
