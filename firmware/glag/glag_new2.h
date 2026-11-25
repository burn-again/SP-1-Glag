/* SPDX-License-Identifier: MIT
 * Glag: more audition voices (after the nine of glag_new.h; appended to the enum, so no piece
 * changes). Included into glag_engine.c after glag_new.h; uses its helpers (nb_saw, nb_noise,
 * res_set, band_set). Costs are estimates, not measured.
 *
 * After AMB-1: the melodic ones don't solo. Each channel keeps a phrase on a loop of its own
 * length (19, 23, 29 or 31 beats, by channel), one to three long notes from the scale; the loops
 * drift against each other as Music for Airports' tapes do, and now and then a note moves a step.
 *
 *   Muted Trumpet  a saw, a slow swell, well low-passed, a Harmon mute's nasal band (CHARACTER: cup
 *                  .. harmon), its own late vibrato (est. 1)
 *   Timpani        two struck membrane modes from a mallet's pulse (CHARACTER: felt .. wood) (est. 1)
 *   Sax            a saw through a low-pass that opens slowly with the breath, breath noise riding
 *                  the reed (CHARACTER: how breathy), a late vibrato (est. 1)
 *   Cello          one bowed saw: a slow bow that swells and fades within a bow's length, a body,
 *                  darker as it climbs (CHARACTER: sul tasto .. ponticello) (est. 1.5)
 *   Upright        as AMB-1's bass: a sine and its octave (which carries on small speakers) and a
 *                  little twelfth, plucked: a finger's thump, the brightness falling away, a long
 *                  decay; the chord's root, slowly (CHARACTER: roots .. fifths .. passing notes) (est. 0.5)
 *   Haze           grains of the reverb's tail (audition builds only) (est. 1)
 *   Vibes          a vibraphone: a sine and its fourth harmonic (the bar's second mode) dying first,
 *                  a motor's tremolo (CHARACTER: off .. slow .. fast and deep), the pedal (LENGTH);
 *                  played on the bells' four loops of 13, 17, 23 and 29 beats (est. 1)
 *   Brushes        soft brushwork: circular sweeps, taps on two and four, a quiet ride, a felt
 *                  kick (ACTIVITY adds them in that order; CHARACTER the ride) (est. 0.5)
 */

/* ---- the phrase loops (horns, cello). A channel's loop is in its bells' memory (a voice that is
 * not the Bells): the degree at each beat in loop[0..1] (-1: none), its length in beats in
 * loop[2..3]; lpos[0] the beat ---- */
static const uint8_t LINE_LEN[GL_NCH] = {19, 23, 29, 31};
static int8_t *line_deg(chan *c) { return &c->loop[0][0]; }
static int8_t *line_dur(chan *c) { return &c->loop[2][0]; }
static void line_fill(chan *c, int ch)
{
	const int len = LINE_LEN[ch];
	int8_t *dg = line_deg(c), *du = line_dur(c);
	const int cello = c->voice == GV_CELLO;
	int have = 0;
	for (int s = 0; s < len; s++)
		have += dg[s] >= 0;
	const int want = (int)clampf(1.0f + P.ch[ch][CP_ACTIVITY] * 2.4f * (1.0f + 0.5f * arc) + 0.3f * rnd(), 1.0f, 3.0f);
	for (int tries = 0; have < want && tries < 16; tries++) {
		const int s = rndi(len);
		const int d = cello ? 4 + 2 * rndi(3) : 2 + rndi(4);        /* beats: cello 4, 6, 8; horns 2 to 5 */
		int clear = 1;                                 /* room: no other note within it, either way */
		for (int k = -6; k <= d + 1 && clear; k++)
			clear = dg[(s + k + len) % len] < 0;
		if (!clear)
			continue;
		/* near the last one placed: a step or two away in the scale */
		int prev = -100;
		for (int k = 1; k < len && prev == -100; k++)
			if (dg[(s - k + len) % len] >= 0)
				prev = dg[(s - k + len) % len];
		int deg = prev == -100 ? 2 + rndi(scale_n) : prev + (rnd() < 0.5f ? -1 : 1) * (1 + rndi(2));
		deg = deg < 0 ? 1 : deg > scale_n + 4 ? scale_n + 2 : deg;
		dg[s] = (int8_t)deg;
		du[s] = (int8_t)d;
		have++;
	}
	while (have > want) {
		const int s = rndi(len);
		if (dg[s] >= 0) {
			dg[s] = -1;
			have--;
		}
	}
}
static int line_note(int base, int deg)
{
	const int oct = deg / scale_n, k = deg % scale_n;
	int note = base + 12 * oct + ((scale_pc[k] - root_pc + 12) % 12) + root_pc % 12;
	while (note < base)
		note += 12;
	return note;
}
static void line_fire(chan *c, int ch, int note, float vel, float hold);
static void line_step(chan *c, int ch, uint32_t step, int base)
{
	if (step & 3u)
		return;                                   /* on the beat */
	const int len = LINE_LEN[ch];
	int8_t *dg = line_deg(c), *du = line_dur(c);
	if (c->lpos[0] >= len)
		c->lpos[0] = 0;
	if (c->lpos[0] == 0)
		line_fill(c, ch);
	const int s = c->lpos[0];
	if (dg[s] >= 0 && rnd() < 0.7f + 0.3f * eff_activity(ch)) {
		const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		float hold = (float)du[s] * beat * (0.55f + 0.5f * P.ch[ch][CP_LENGTH]);
		if (c->voice == GV_CELLO && hold > 6.0f)
			hold = 6.0f;                          /* a bow is only so long */
		line_fire(c, ch, line_note(base, dg[s]), 0.5f + 0.25f * rnd(), hold);
	}
	if (++c->lpos[0] >= len) {
		c->lpos[0] = 0;
		/* now and then a note moves a step (MOTION) */
		if (rnd() < 0.12f + 0.5f * P.g[GP_MOTION]) {
			const int k = rndi(len);
			if (dg[k] >= 0)
				dg[k] = (int8_t)clampf((float)(dg[k] + (rnd() < 0.5f ? -1 : 1)), 0.0f, (float)(scale_n + 4));
		}
	}
}
/* a line's note: one voice at a time. The same note again carries on in the same voice (a bow
 * change, no second saw beating against the first); another lets the last one go quickly */
static void line_fire(chan *c, int ch, int note, float vel, float hold)
{
	(void)ch;
	nvoice *same = 0;
	for (int i = 0; i < 3; i++) {
		nvoice *v = &c->v[i];
		if (!sounding(v))
			continue;
		if ((int)(v->note + 0.5f) == note && !v->mnote)
			same = v;
		else if (!v->mnote) {
			v->hold = -1.0f;                      /* (a quick release: the bow leaves it) */
			if (v->stage != ST_IDLE)
				v->stage = ST_REL;
		}
	}
	nvoice *v = same;
	if (!v) {
		v = alloc(c, 3);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel, 1.0f, 0);
	} else {
		v->stage = ST_ATT;
		v->t = 0.0f;
		v->vel = vel;
	}
	v->hold = hold;
	c->glow = vel;
}

/* a voice's own vibrato: its phase in pm2, 4.8 to 5.6 Hz (each voice a little different), coming
 * in after `delay` s over a second and a half */
static float own_vib(nvoice *v, int i, float sdt, float delay, float depth)
{
	v->pm2 += (uint32_t)(sdt * (4.8f + 0.25f * (float)i + 0.3f * v->vel) * 4294967296.0f);
	return 1.0f + depth * clampf((v->t - delay) * 0.66f, 0.0f, 1.0f) * sin_u(v->pm2);
}
/* key tracking for the low-passes: 1 at C3, falling an octave's worth for every two octaves up */
static inline float kt_dark(float note) { return clampf(fexp2(-(note - 48.0f) * (1.0f / 24.0f)), 0.35f, 1.3f); }

/* ---- the horns (Muted Trumpet, Sax): f1 f2 a low-pass's states, fk its coefficient; the
 * trumpet's mute: idx2 menv2 a band's states, fq fb its a and b, idx its gain; hold: seconds
 * left before the breath stops (-1: let go quickly); t: seconds since the note began ---- */
static void horn_control(chan *c, int ch, float dt)
{
	const int tpt = c->voice == GV_TRUMPET;
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	/* a slow swell in: a quarter to two thirds of a second (TONE quicker) */
	const float att = (tpt ? 0.22f : 0.28f) + 0.4f * (1.0f - tone), rel = 0.45f + 1.0f * len;
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
			v->env *= dcoef(v->hold < -0.5f ? 0.15f : rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float e2 = v->env * v->env;           /* (the swell eased in: no edge on the start) */
		/* a small scoop up into the note, then the vibrato, late */
		const float scoop = (tpt ? -0.2f : -0.4f) * fexp2(-v->t * 10.0f);
		const float hz = mtof(v->note + scoop) * speed * pbend * own_vib(v, i, sdt, 0.6f, tpt ? 0.0022f : 0.003f);
		v->ic = hz_inc(hz);
		/* the low-pass opens with the breath, never far: two to five harmonics in (TONE), fewer as
		 * it climbs, under 1.2 to 3.4 kHz whatever the note */
		const float bright = e2 * (0.6f + 0.4f * v->vel);
		float fc = hz * (1.1f + (1.0f + 3.0f * tone) * bright) * kt_dark(v->note);
		const float cap = (tpt ? 1400.0f : 1200.0f) + 2200.0f * tone;
		fc = clampf(fc < cap ? fc : cap, 60.0f, 6000.0f);
		v->fk = 1.0f - fexp2(-1.4427f * 6.2832f * fc / VSR);
		if (tpt) {
			/* the Harmon mute: a nasal band at 1 to 1.6 kHz, narrower toward harmon */
			const float fm = 1000.0f + 600.0f * chr;
			float g;
			band_set(fm, fm * (0.6f - 0.3f * chr), &v->fq, &v->fb, &g);
			v->idx = g * (1.2f + 1.2f * chr) * (0.4f + 0.6f * bright);
		} else
			v->idx = chr;                         /* the sax: how breathy */
		const float a = e2 * v->vel * (tpt ? 1.3f : 1.7f);
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
				const float z = ba * z1 - bb * z2 + bg * y2;
				z2 = z1; z1 = z;
				A[j] += (0.45f * y2 + z) * amp;
				pc += ic;
				amp += ai;
			}
			v->idx2 = z1; v->menv2 = z2;
		} else {
			const float br = v->idx * 0.45f;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				const float n = nb_noise(&v->pm) * br * (0.6f + 0.4f * s);   /* breath riding the reed */
				y1 += k * (s + n - y1);
				y2 += k * (y1 - y2);
				A[j] += (y2 + 0.25f * n * k) * amp;
				pc += ic;
				amp += ai;
			}
		}
		v->pc = pc; v->amp = amp; v->f1 = y1; v->f2 = y2;
	}
	return any;
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
 * idx2 menv2 its states, fq fb its a b, idx its gain; hold, t as the horns'. A bow swells in over
 * most of a second and fades again before it runs out (held by MIDI past a bow's length, the bow
 * changes: a dip, and on) ---- */
static void cello_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	const float att = 0.7f + 0.8f * (1.0f - tone), rel = 0.9f + 1.0f * len;
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
			/* the bow: a slow fall through its length; a bow change (dip) every 5 s held */
			const float bp = v->mnote ? v->t - 5.0f * (float)(int)(v->t * 0.2f) : 1.0f;
			const float want = v->mnote && v->t > 2.0f && bp < 0.4f ? 0.55f : 0.8f;
			v->env += (want - v->env) * (1.0f - dcoef(v->mnote ? 0.15f : 1.5f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f && !v->mnote)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(v->hold < -0.5f ? 0.25f : rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float e2 = v->env * v->env;
		const float hz = mtof(v->note) * speed * pbend * own_vib(v, i, sdt, 0.5f, 0.0028f);
		v->ic = hz_inc(hz);
		/* the low-pass: with the bow's pressure, three to eight harmonics, and fewer as it
		 * climbs (an octave's worth darker for every two up from C3); never past 2.5 kHz */
		float fc = hz * (1.6f + (1.5f + 5.0f * (0.3f + 0.7f * chr) * (0.4f + 0.6f * tone)) * e2) * kt_dark(v->note);
		fc = clampf(fc, 80.0f, 1000.0f + 1500.0f * chr);
		v->fk = 1.0f - fexp2(-1.4427f * 6.2832f * fc / VSR);
		float g;
		/* the body: a wide hump (tasto) or the bridge's edge (ponticello); wide, so a vibrato
		 * doesn't swing the level */
		const float fb = 450.0f + 1200.0f * chr;
		band_set(fb, 500.0f + 700.0f * chr, &v->fq, &v->fb, &g);
		v->idx = g * 1.2f;
		const float a = e2 * v->vel * 0.9f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int cello_render(chan *c, float *A, int ch)
{
	int any = 0;
	const float ros = 0.015f + 0.03f * P.ch[ch][CP_CHAR];
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
			const float z = ba * z1 - bb * z2 + bg * y2;
			z2 = z1; z1 = z;
			A[j] += (0.75f * y2 + z) * amp;
			pc += ic;
			amp += ai;
		}
		v->pc = pc; v->amp = amp; v->f1 = y1; v->f2 = y2; v->idx2 = z1; v->menv2 = z2;
	}
	return any;
}

/* ---- Upright, as AMB-1's bass: per voice a sine and its octave and twelfth (one phase, pc);
 * menv the pluck's brightness (falling over ~0.3 s), menv2 the finger's thump, f1 its low-pass
 * state; hold: seconds before the finger lets go (-1 now) ---- */
static nvoice *upright_note(chan *c, int ch, int note, float vel, float hold)
{
	(void)ch;
	for (int i = 0; i < 2; i++)                    /* the last note stopped by the finger */
		if (sounding(&c->v[i]) && !c->v[i].mnote) {
			c->v[i].hold = -1.0f;
			c->v[i].stage = ST_REL;
		}
	nvoice *v = alloc(c, 2);
	if (!sounding(v))
		make_room(v);
	nv_trigger(v, (float)note, vel, 1.0f, 0);
	v->hold = hold;
	v->menv2 = 1.0f;
	c->last_note = note;
	c->glow = vel;
	return v;
}
static void upright_control(chan *c, int ch, float dt)
{
	const float tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed, T = expmap(len, 1.2f, 5.0f);
	for (int i = 0; i < 2; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		v->t += sdt;
		if (v->stage == ST_ATT) {
			v->env += sdt / 0.008f;
			if (v->env >= 1.0f) {
				v->env = 1.0f;
				v->stage = ST_DEC;
			}
		} else if (v->stage == ST_DEC) {
			v->env *= dcoef(T, sdt);
			v->hold -= sdt;
			if ((v->hold <= 0.0f && !v->mnote) || v->env < 0.0005f)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(v->hold < -0.5f ? 0.08f : 0.25f, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->menv *= dcoef(0.25f + 0.3f * tone, sdt);   /* the pluck's brightness */
		/* a string plucked hard sits a little sharp for a moment */
		v->ic = hz_inc(mtof(v->note + 0.12f * v->vel * fexp2(-v->t * 18.0f)) * speed * pbend);
		v->fk = 0.42f + 0.25f * tone;               /* the octave: always there (it carries the note) */
		v->fq = (0.08f + 0.35f * tone) * (0.3f + 0.7f * v->menv);   /* the twelfth: mostly the pluck */
		v->bend = 1.0f - fexp2(-1.4427f * 6.2832f * 500.0f / VSR);   /* the thump's low-pass */
		const float a = v->env * v->vel * 0.62f;
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int upright_render(chan *c, float *A)
{
	int any = 0;
	for (int i = 0; i < 2; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		float amp = v->amp, th = v->menv2, y = v->f1;
		const float h2 = v->fk, h3 = v->fq, k = v->bend, ai = v->ainc;
		for (uint32_t j = 0; j < VSUB; j++) {
			y += k * (nb_noise(&v->pm) * th - y);
			th *= 0.996f;                          /* (the thump: ~20 ms) */
			A[j] += (sin_u(pc) + h2 * sin_u(2u * pc) + h3 * sin_u(3u * pc) + 1.5f * y) * amp;
			pc += ic;
			amp += ai;
		}
		v->pc = pc; v->amp = amp; v->menv2 = th; v->f1 = y;
	}
	return any;
}
/* AMB-1's bass, plucked: the chord's root on a new chord, held; more ACTIVITY, the bar's root
 * again and then the half bar; CHARACTER brings in the fifth, then a passing note now and then */
static void upright_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int b = (int)(step & 15u);
	const int lo = base - 5, hi = base + 7;
	const float chr = P.ch[ch][CP_CHAR];
	const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	if (b == 0 && (c->seen_chord != chord_count || rnd() < act * act)) {
		c->seen_chord = chord_count;
		upright_note(c, ch, place(chord_pc[0], (float)base, lo, hi), 0.75f + 0.15f * rnd(), 3.6f * beat);
	} else if (b == 8 && act > 0.45f && rnd() < act) {
		int note;
		if (rnd() < chr * 0.6f)
			note = place((chord_pc[0] + 7) % 12, (float)c->last_note, lo, hi);       /* the fifth */
		else if (rnd() < chr * 0.3f)
			note = pick_note(c, c->last_note - 3, c->last_note + 3, 0.4f);          /* a passing note */
		else
			note = place(chord_pc[0], (float)c->last_note, lo, hi);
		upright_note(c, ch, note, 0.55f + 0.15f * rnd(), 1.8f * beat);
	}
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

/* ---- Vibes: per voice a sine and its fourth harmonic (pc, pc2), menv the fourth's life; the
 * motor's tremolo for the whole channel in lfo2. Played on the bells' loops (bells_beat; its
 * pending notes land through chan_control) ---- */
static void vibes_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	/* the motor: off below a fifth of CHARACTER, then 3 to 7 Hz, deeper as it goes */
	const float rate = chr < 0.2f ? 0.0f : 3.0f + 4.0f * (chr - 0.2f) / 0.8f;
	c->lfo2 += (uint32_t)(sdt * rate * 4294967296.0f);
	c->swell = chr < 0.2f ? 0.0f : 0.15f + 0.4f * (chr - 0.2f) / 0.8f;   /* (its depth, kept for render) */
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		if (c->bp_fade & (1u << i))
			v->env *= dcoef(0.012f, dt);         /* making way for the next bar */
		/* a soft mallet (5 ms), the pedal down: 1.5 to 7 s, low bars longer */
		const float ring = clampf(1.0f + (60.0f - v->note) * 0.012f, 0.6f, 1.4f);
		nv_perc(v, 0.005f, expmap(len, 1.5f, 7.0f) * ring / speed, 0.35f / speed, 0.0f, dt);
		const float hz = mtof(v->note) * speed * pbend;
		v->ic = hz_inc(hz);
		v->ic2 = hz_inc(hz * 4.0f);
		v->fk = (0.08f + 0.3f * tone) * clampf(fexp2(-(v->note - 60.0f) * (1.0f / 18.0f)), 0.15f, 1.0f);
		v->hold = clampf(1.0f - (v->note - 64.0f) * 0.02f, 0.6f, 1.0f);   /* high bars a little softer */
	}
}
static int vibes_render(chan *c, float *A, float *B)
{
	int any = 0;
	const float tr = 1.0f - c->swell * 0.5f * (1.0f + sin_u(c->lfo2));
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t p1 = v->pc, p2 = v->pc2;
		const uint32_t i1 = v->ic, i2 = v->ic2;
		float amp = v->amp;
		const float l2 = v->menv * v->fk, ai = v->ainc, g = tr * v->hold * 0.8f;
		float *out = v->side ? B : A;
		for (uint32_t j = 0; j < VSUB; j++) {
			out[j] += (sin_u(p1) + l2 * sin_u(p2)) * amp * g;
			p1 += i1; p2 += i2;
			amp += ai;
		}
		v->pc = p1; v->pc2 = p2; v->amp = amp;
	}
	return any;
}

/* ---- Brushes: in the Kit's room ---- */
typedef struct {
	float sw_ph, sw_dt, sw_a, sw_y1, sw_y2, sw_ba, sw_bb, sw_bg;     /* the sweep: its place, rate, level, band */
	float tp_a, tp_k, tp_y1, tp_y2, tp_ba, tp_bb, tp_bg;            /* the tap */
	float rd_a, rd_k, rd_y1, rd_y2, rd_z1, rd_z2;                   /* the ride: two narrow bands */
	float rd_aa, rd_ab, rd_ag, rd_ba, rd_bb, rd_bg;
	float kk_ph, kk_f, kk_a, kk_k;                                  /* the felt kick */
	uint32_t rng;
} br_state;
_Static_assert(sizeof(br_state) <= sizeof(kit_state), "the Brushes live in the Kit's room");
#define BR(c) ((br_state *)(void *)&(c)->kt)
enum { BR_SWEEP = 1, BR_TAP = 2, BR_RIDE = 4, BR_KICK = 8 };
static void br_fire(chan *c, int ch, int m, float vel)
{
	br_state *S = BR(c);
	const float tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	if (S->rng == 0u)
		S->rng = 0x9E3779B9u;
	if (m & BR_SWEEP) {                           /* a circle of the brush: two beats, rising and falling */
		S->sw_ph = 0.0f;
		S->sw_dt = 1.0f / (1.8f * beat * VSR);
		S->sw_a = vel * (0.2f + 0.12f * tone);
		band_set(2200.0f + 1500.0f * tone, 2600.0f, &S->sw_ba, &S->sw_bb, &S->sw_bg);
	}
	if (m & BR_TAP) {                             /* the brush laid on the snare */
		S->tp_a = vel * 0.55f;
		S->tp_k = fexp2(-9.9658f / ((0.09f + 0.15f * len) * VSR));
		band_set(1800.0f + 1200.0f * tone, 2200.0f, &S->tp_ba, &S->tp_bb, &S->tp_bg);
	}
	if (m & BR_RIDE) {                            /* a quiet ride: its shimmer, long */
		S->rd_a = vel * 0.1f;
		S->rd_k = fexp2(-9.9658f / ((0.8f + 1.4f * len) * VSR));
		band_set(4300.0f + 900.0f * tone, 500.0f, &S->rd_aa, &S->rd_ab, &S->rd_ag);
		band_set(6100.0f + 1100.0f * tone, 700.0f, &S->rd_ba, &S->rd_bb, &S->rd_bg);
	}
	if (m & BR_KICK) {                            /* a felt beater, barely there */
		S->kk_ph = 0.0f;
		S->kk_f = 70.0f;
		S->kk_a = vel * 0.5f;
		S->kk_k = fexp2(-9.9658f / (0.35f * VSR));
	}
	notes_fired++;
	c->glow = vel * 0.6f;
}
static void br_step(chan *c, int ch, uint32_t step)
{
	const float act = eff_activity(ch), chr = P.ch[ch][CP_CHAR];
	const int s16 = (int)(step & 15u);
	int m = 0;
	float vel = 0.8f + 0.2f * rnd();
	if ((s16 == 0 || s16 == 8) && act > 0.08f)
		m |= BR_SWEEP;
	if ((s16 == 4 || s16 == 12) && act > 0.3f && rnd() < 0.6f + act)
		m |= BR_TAP;
	if (act > 0.5f && chr > 0.15f) {
		/* the ride: on the beats, and the swung note before two and four as it gets busier */
		if ((s16 & 3) == 0 && rnd() < 0.5f + 0.5f * chr)
			m |= BR_RIDE;
		if ((s16 == 7 || s16 == 15) && rnd() < (act - 0.5f) * 2.0f * chr)
			m |= BR_RIDE;
	}
	if (s16 == 0 && act > 0.7f && rnd() < 0.5f)
		m |= BR_KICK;
	if (!m && act > 0.4f && (s16 & 1) && rnd() < 0.03f * act) {
		m = BR_TAP;                                /* a ghost */
		vel = 0.3f;
	}
	if (m)
		br_fire(c, ch, m, vel);
}
static int br_render(chan *c, float *A)
{
	br_state *S = BR(c);
	if (S->sw_ph >= 1.0f && S->tp_a < 1e-4f && S->rd_a < 1e-4f && S->kk_a < 1e-4f)
		return 0;
	for (uint32_t j = 0; j < VSUB; j++) {
		const float n = nb_noise(&S->rng);
		float x = 0.0f;
		if (S->sw_ph < 1.0f) {
			const float e = sin_u((uint32_t)(S->sw_ph * 2147483648.0f));
			const float y = S->sw_ba * S->sw_y1 - S->sw_bb * S->sw_y2 + S->sw_bg * n * e * e;
			S->sw_y2 = S->sw_y1; S->sw_y1 = y;
			x += y * S->sw_a;
			S->sw_ph += S->sw_dt;
		}
		if (S->tp_a > 1e-4f) {
			const float y = S->tp_ba * S->tp_y1 - S->tp_bb * S->tp_y2 + S->tp_bg * n * S->tp_a;
			S->tp_y2 = S->tp_y1; S->tp_y1 = y;
			x += y;
			S->tp_a *= S->tp_k;
		}
		if (S->rd_a > 1e-4f) {
			const float y = S->rd_aa * S->rd_y1 - S->rd_ab * S->rd_y2 + S->rd_ag * n * S->rd_a;
			const float z = S->rd_ba * S->rd_z1 - S->rd_bb * S->rd_z2 + S->rd_bg * n * S->rd_a;
			S->rd_y2 = S->rd_y1; S->rd_y1 = y;
			S->rd_z2 = S->rd_z1; S->rd_z1 = z;
			x += 1.6f * (y + z);
			S->rd_a *= S->rd_k;
		}
		if (S->kk_a > 1e-4f) {
			x += S->kk_a * sin_u((uint32_t)(S->kk_ph * 4294967296.0f));
			S->kk_ph += S->kk_f * (1.0f / VSR);
			S->kk_ph -= S->kk_ph >= 1.0f ? 1.0f : 0.0f;
			S->kk_f += (46.0f - S->kk_f) * 0.0006f;
			S->kk_a *= S->kk_k;
		}
		A[j] += x;
	}
	return 1;
}
