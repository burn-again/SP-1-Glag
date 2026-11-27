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

/* (the noir side's phrasing and its arc, defined below; the brushes follow it) */
static uint8_t ph_style, ph_piece;
static uint8_t esh_on;                              /* (the shaped envelopes, below: on and a noir song) */
#define ESH_LIVE (esh_on && ph_piece)
/* shaped, a lead cut off by its next note lets go over 0.6 s, not in a snap */
#define ESH_LETGO(t) (ESH_LIVE ? 0.6f : (t))
static float c_int(void);

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
static void line_fire(chan *c, int ch, int note, float vel, float hold, float emph);
static void ph_range(int voice, int base, int *lo, int *hi);
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
		int note = line_note(base, dg[s]), lo, hi;
		ph_range(c->voice, base, &lo, &hi);               /* the instrument's middle only: nothing shrill */
		while (note > hi)
			note -= 12;
		while (note < lo)
			note += 12;
		line_fire(c, ch, note, 0.5f + 0.25f * rnd(), hold, hold >= 3.0f ? 1.0f : 0.0f);
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
static void line_fire(chan *c, int ch, int note, float vel, float hold, float emph)
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
			if (ESH_LIVE) {                       /* (shaped: a legato crossfade, the last note held a */
				if (v->stage != ST_REL && v->hold > 0.25f)   /* moment as the next swells in, then let go as written) */
					v->hold = 0.25f;
				continue;
			}
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
		nv_trigger(v, (float)note, vel, emph, 0);   /* (ratio: its emphasis) */
	} else {
		v->stage = ST_ATT;
		v->t = 0.0f;
		v->vel = vel;
		v->ratio = emph;
	}
	v->hold = hold;
	c->glow = vel;
}

/* a voice's own vibrato, as players use it: for emphasis. Only a note given it (ratio, its
 * emphasis: the long tones a phrase leans into; a key held), and only late in it: from half way
 * through the note (no sooner than 0.9 s; a held key after 1.2 s), growing over a second and a half
 * from nothing, its rate easing up from 4.2 to 5.3 Hz as it grows. Short notes are straight */
static float own_vib(nvoice *v, int i, float sdt, float depth)
{
	float on;
	if (v->mnote)
		on = 1.2f;
	else {
		const float whole = v->t + (v->hold > 0.0f ? v->hold : 0.0f);
		on = whole * 0.5f > 0.9f ? whole * 0.5f : 0.9f;
	}
	const float g = v->ratio * clampf((v->t - on) * 0.65f, 0.0f, 1.0f);
	if (g <= 0.0f)
		return 1.0f;
	v->pm2 += (uint32_t)(sdt * (4.2f + 1.1f * g + 0.12f * (float)i) * 4294967296.0f);
	return 1.0f + depth * g * sin_u(v->pm2);
}
/* the long tone leaned into: a gentle swell up and back over the note (emphasis), held keys flat */
static inline float lean(const nvoice *v)
{
	if (v->mnote || v->ratio <= 0.0f || v->hold <= 0.0f)
		return 0.0f;
	const float x = v->t / (v->t + v->hold);
	return v->ratio * sin_u((uint32_t)(x * 2147483648.0f));
}
/* key tracking for the low-passes: 1 at C3, falling an octave's worth for every two octaves up */
static inline float kt_dark(float note) { return clampf(fexp2(-(note - 48.0f) * (1.0f / 24.0f)), 0.35f, 1.3f); }

/* a phrase's note waiting its moment (a little behind the beat): echo_at the seconds left,
 * echo_note, echo_vel, hq_dt its hold, hq_vel its emphasis */
static void ph_pending(chan *c, int ch, float sdt);

/* ---- the horns (Trumpet, Sax): f1 f2 a low-pass's states, fk its coefficient; the trumpet's
 * mute: idx2 menv2 a band's states, fq fb its a and b, idx its gain; hold: seconds left before
 * the breath stops (-1: let go quickly); t: seconds since the note began; ratio its emphasis.
 * The trumpet's CHARACTER: open and dark (Chet Baker's) to a gentle Harmon (Miles's) ---- */
static void horn_control(chan *c, int ch, float dt)
{
	const int tpt = c->voice == GV_TRUMPET;
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed;
	ph_pending(c, ch, sdt);
	/* a slow swell in: a quarter to two thirds of a second (TONE quicker) */
	const float att = (tpt ? 0.22f : 0.28f) + 0.4f * (1.0f - tone), rel = (0.45f + 1.0f * len) * (ESH_LIVE ? 2.0f : 1.0f);   /* (shaped: the end of a phrase let go as gently as written: the level is the breath squared) */
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
			v->env += (0.8f + 0.15f * lean(v) - v->env) * (1.0f - dcoef(0.5f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f && !v->mnote)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(v->hold < -0.5f ? ESH_LETGO(0.15f) : rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float e2 = v->env * v->env;           /* (the swell eased in: no edge on the start) */
		/* a breath of a scoop into the note; the vibrato only on a note leaned into, late */
		const float scoop = (tpt ? -0.12f : -0.3f) * fexp2(-v->t * 9.0f);
		const float hz = mtof(v->note + scoop) * speed * pbend * own_vib(v, i, sdt, tpt ? 0.0016f : 0.0028f);
		v->ic = hz_inc(hz);
		/* the low-pass opens with the breath and never far: one to three harmonics in (TONE),
		 * fewer as it climbs, and under 0.95 to 2 kHz whatever the note: nothing shrill */
		const float bright = e2 * (0.6f + 0.4f * v->vel) * (1.0f + 0.15f * lean(v));
		float fc = hz * (1.05f + (0.8f + 2.2f * tone) * bright) * kt_dark(v->note);
		const float cap = (tpt ? 950.0f : 900.0f) + (tpt ? 1100.0f : 1200.0f) * tone;
		fc = clampf(fc < cap ? fc : cap, 60.0f, 3000.0f);
		v->fk = 1.0f - fexp2(-1.4427f * 6.2832f * fc / VSR);
		if (tpt) {
			/* toward the Harmon: a soft nasal hump at 0.9 to 1.3 kHz, wide, never a whistle */
			const float fm = 900.0f + 400.0f * chr;
			float g;
			band_set(fm, fm * 0.9f, &v->fq, &v->fb, &g);
			v->idx = g * 0.9f * chr * (0.4f + 0.6f * bright);
			v->bend = 1.0f - 0.35f * chr;          /* (the open horn's share) */
		} else
			v->idx = chr;                         /* the sax: how breathy */
		const float a = e2 * v->vel * (tpt ? 1.6f : 1.9f);
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
			const float ba = v->fq, bb = v->fb, bg = v->idx, op = v->bend;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				y1 += k * (s - y1);
				y2 += k * (y1 - y2);
				const float z = ba * z1 - bb * z2 + bg * y2;
				z2 = z1; z1 = z;
				A[j] += (op * y2 + z) * amp;
				pc += ic;
				amp += ai;
			}
			v->idx2 = z1; v->menv2 = z2;
		} else {
			/* the breath: less of it, and what comes out past the reed itself low-passed (~1.6 kHz,
			 * idx2 its state): straight out it was white noise, a hiss over the tone */
			const float br = v->idx * 0.36f;
			float nl = v->idx2;
			for (uint32_t j = 0; j < VSUB; j++) {
				const float s = nb_saw(pc, ic);
				const float n = nb_noise(&v->pm) * br * (0.6f + 0.4f * s);   /* breath riding the reed */
				nl += 0.35f * (n - nl);
				y1 += k * (s + n - y1);
				y2 += k * (y1 - y2);
				A[j] += (y2 + 0.2f * nl * k) * amp;
				pc += ic;
				amp += ai;
			}
			v->idx2 = nl;
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
		const float xs = (1.0f - mk) * 0.8f;          /* (the pulse's area: the same strike whatever the mallet) */
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
	ph_pending(c, ch, sdt);
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
			const float want = v->mnote && v->t > 2.0f && bp < 0.4f ? 0.55f : 0.78f + 0.15f * lean(v);
			v->env += (want - v->env) * (1.0f - dcoef(v->mnote ? 0.15f : 1.5f, sdt));
			v->hold -= sdt;
			if (v->hold <= 0.0f && !v->mnote)
				v->stage = ST_REL;
		} else if (v->stage == ST_REL) {
			v->env *= dcoef(v->hold < -0.5f ? ESH_LETGO(0.25f) : rel, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		const float e2 = v->env * v->env;
		const float hz = mtof(v->note) * speed * pbend * own_vib(v, i, sdt, 0.0026f);
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
#if defined(GL_DEMO_PIECES) && !defined(GL_NO_HAZE_RAM)
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
#if defined(GL_DEMO_PIECES) && !defined(GL_NO_HAZE_RAM)
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
#if defined(GL_NO_HAZE_RAM)
/* (on the device there is no room for the tail's four seconds: Haze reads the echo's tape, the same
 * size and rate, and keeps its own way of reading it, long, slow, low and reversed) */
static float haze_read(uint32_t i, float f) { return grain_read(i, f); }
#define HZ_W() grain_w()
#else
static float haze_read(uint32_t i, float f)
{
	const float a = hz_ring[i & (HZ_N - 1u)], b = hz_ring[(i + 1u) & (HZ_N - 1u)];
	return a + (b - a) * f;
}
#define HZ_W() hz_w
#endif
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
	v->pc = HZ_W() - (uint32_t)back;
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
	ph_pending(c, ch, sdt);
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

/* ---- Brushes: in the Kit's room. The sweep is the brush going round the head: as it circles it
 * crosses the stereo field and back, its colour rises and falls (nearer the rim, brighter), and
 * the pressure swells twice a circle; each circle a little different in speed, colour and where
 * it starts. A tap is the brush laid on the head, then dragged off it (a soft tail). Busier, a
 * faint stir carries on underneath, wandering in level. A quiet ride; a felt kick, barely there ---- */
typedef struct {
	float sw_ph, sw_dt, sw_a, sw_fc, sw_p0, sw_y1, sw_y2, sw_ba, sw_bb, sw_bg;   /* the sweep */
	float tp_a, tp_k, dr_a, dr_k, tp_y1, tp_y2, tp_ba, tp_bb, tp_bg;            /* the tap and its drag */
	float rd_a, rd_k, rd_y1, rd_y2, rd_z1, rd_z2;                               /* the ride: two narrow bands */
	float rd_aa, rd_ab, rd_ag, rd_ba, rd_bb, rd_bg;
	float kk_ph, kk_f, kk_a, kk_k;                                              /* the felt kick */
	float st_a, st_want, st_y;                                                  /* the stir */
	uint32_t rng;
} br_state;
_Static_assert(sizeof(br_state) <= sizeof(kit_state), "the Brushes live in the Kit's room");
#define BR(c) ((br_state *)(void *)&(c)->kt)
enum { BR_SWEEP = 1, BR_TAP = 2, BR_RIDE = 4, BR_KICK = 8 };
static float br_rnd(br_state *S) { return 0.5f + 0.5f * nb_noise(&S->rng); }
static void br_fire(chan *c, int ch, int m, float vel)
{
	br_state *S = BR(c);
	const float tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	if (S->rng == 0u)
		S->rng = 0x9E3779B9u;
	if (m & BR_SWEEP) {                           /* a circle of the brush: about two beats */
		S->sw_ph = 0.0f;
		S->sw_dt = 1.0f / ((1.6f + 0.5f * br_rnd(S)) * beat * VSR);
		S->sw_a = vel * (0.2f + 0.12f * tone) * (0.8f + 0.4f * br_rnd(S));
		S->sw_fc = (1500.0f + 1300.0f * tone) * (0.85f + 0.3f * br_rnd(S));
		S->sw_p0 = br_rnd(S);
	}
	if (m & BR_TAP) {                             /* laid on the head, then dragged off it */
		S->tp_a = vel * 0.32f;
		S->tp_k = fexp2(-9.9658f / ((0.05f + 0.08f * len) * VSR));
		S->dr_a = vel * 0.12f;
		S->dr_k = fexp2(-9.9658f / ((0.18f + 0.15f * len) * VSR));
		band_set(1700.0f + 1100.0f * tone, 2000.0f, &S->tp_ba, &S->tp_bb, &S->tp_bg);
	}
	if (m & BR_RIDE) {
		S->rd_a = vel * 0.08f;
		S->rd_k = fexp2(-9.9658f / ((0.8f + 1.4f * len) * VSR));
		band_set(4000.0f + 700.0f * tone, 500.0f, &S->rd_aa, &S->rd_ab, &S->rd_ag);
		band_set(5600.0f + 900.0f * tone, 700.0f, &S->rd_ba, &S->rd_bb, &S->rd_bg);
	}
	if (m & BR_KICK) {
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
	br_state *S = BR(c);
	float act = eff_activity(ch);
	const float chr = P.ch[ch][CP_CHAR];
	if (ph_style == 2 && ph_piece) {
		/* C: the brushes with the arc: taps coming in as it fills, fading almost out at its
		 * sparest and coming back */
		const float p = c_int();
		act *= 0.6f + 1.0f * p;
		const float want = 0.15f + 0.85f * clampf((p - 0.15f) * 2.2f, 0.0f, 1.0f);
		arc_d[ch] += (want - arc_d[ch]) * 0.02f;
	} else
		arc_d[ch] = 1.0f;
	const int s16 = (int)(step & 15u);
	int m = 0;
	float vel = 0.8f + 0.2f * rnd();
	if ((s16 == 0 || s16 == 8) && act > 0.08f)
		m |= BR_SWEEP;
	if ((s16 == 4 || s16 == 12) && act > 0.3f && rnd() < 0.6f + act)
		m |= BR_TAP;
	(void)chr;                                     /* (the ride is gone: its narrow bands sang like a kettle) */
	if (s16 == 0 && act > 0.7f && rnd() < 0.5f)
		m |= BR_KICK;
	if (!m && act > 0.4f && (s16 & 1) && rnd() < 0.03f * act) {
		m = BR_TAP;
		vel = 0.3f;
	}
	if ((s16 & 3) == 0)                            /* the stir: busier, a faint bed, wandering */
		S->st_want = act > 0.55f ? (act - 0.55f) * 0.05f * (0.4f + 0.6f * rnd()) : 0.0f;
	if (m)
		br_fire(c, ch, m, vel);
}
static int br_render(chan *c, float *A, float *B)
{
	br_state *S = BR(c);
	if (S->sw_ph >= 1.0f && S->tp_a < 1e-4f && S->dr_a < 1e-4f && S->rd_a < 1e-4f && S->kk_a < 1e-4f &&
	    S->st_a < 1e-5f && S->st_want <= 0.0f)
		return 0;
	/* the sweep's place in its circle, for this block: colour, pressure, where it is across */
	float swe = 0.0f, pan = 0.5f;
	if (S->sw_ph < 1.0f) {
		const uint32_t ph = (uint32_t)(S->sw_ph * 4294967296.0f);
		const uint32_t p0 = (uint32_t)(S->sw_p0 * 4294967296.0f);
		const float half = sin_u(ph >> 1);           /* 0 .. 1 .. 0 over the circle */
		swe = half * half * (0.72f + 0.28f * sin_u(2u * ph + 0x40000000u)) * S->sw_a;
		pan = 0.5f + 0.26f * sin_u(ph + p0);
		band_set(S->sw_fc * (1.0f + 0.3f * sin_u(ph + p0 + 0x40000000u)), S->sw_fc * 0.9f, &S->sw_ba, &S->sw_bb, &S->sw_bg);
	}
	S->st_a += (S->st_want - S->st_a) * 0.01f;
	const float gl = 1.3f * (1.0f - pan), gr = 1.3f * pan;
	for (uint32_t j = 0; j < VSUB; j++) {
		const float n = nb_noise(&S->rng);
		float x = 0.0f, l = 0.0f, r = 0.0f;
		if (swe > 0.0f) {
			const float y = S->sw_ba * S->sw_y1 - S->sw_bb * S->sw_y2 + S->sw_bg * n * swe;
			S->sw_y2 = S->sw_y1; S->sw_y1 = y;
			l += y * gl;
			r += y * gr;
		}
		if (S->tp_a > 1e-4f || S->dr_a > 1e-4f) {
			const float y = S->tp_ba * S->tp_y1 - S->tp_bb * S->tp_y2 + S->tp_bg * n * (S->tp_a + S->dr_a);
			S->tp_y2 = S->tp_y1; S->tp_y1 = y;
			x += y;
			S->tp_a *= S->tp_k;
			S->dr_a *= S->dr_k;
		}
		if (S->rd_a > 1e-4f) {
			const float y = S->rd_aa * S->rd_y1 - S->rd_ab * S->rd_y2 + S->rd_ag * n * S->rd_a;
			const float z = S->rd_ba * S->rd_z1 - S->rd_bb * S->rd_z2 + S->rd_bg * n * S->rd_a;
			S->rd_y2 = S->rd_y1; S->rd_y1 = y;
			S->rd_z2 = S->rd_z1; S->rd_z1 = z;
			l += 0.9f * (y + z);
			r += 1.5f * (y + z);                       /* (the ride a little to the right) */
		}
		if (S->kk_a > 1e-4f) {
			x += S->kk_a * sin_u((uint32_t)(S->kk_ph * 4294967296.0f));
			S->kk_ph += S->kk_f * (1.0f / VSR);
			S->kk_ph -= S->kk_ph >= 1.0f ? 1.0f : 0.0f;
			S->kk_f += (46.0f - S->kk_f) * 0.0006f;
			S->kk_a *= S->kk_k;
		}
		S->st_y += 0.2f * (n - S->st_y);                /* the stir: soft, low-passed */
		x += S->st_y * S->st_a;
		A[j] += x + l;
		B[j] += x + r;
	}
	if (S->sw_ph < 1.0f)
		S->sw_ph += S->sw_dt * (float)VSUB;
	return 1;
}

/* ================================================================ the noir side's harmony */
/* A progression's chord: its root in semitones above the key's, its notes above its root (up to
 * four), its colour tones (bits above its root: 2 the ninth, 5 the eleventh, 6 the sharp eleventh,
 * 9 the thirteenth, 11 the major seventh, 1 the flat ninth), and its bars. These follow what the
 * scores and the players' own accounts show, as approximations, with no one's melody in them */
typedef struct { int8_t root; uint8_t n; int8_t iv[4]; uint16_t col; uint8_t bars; } prog_step;
#define C9 (1u << 2)
#define C11 (1u << 5)
#define CS11 (1u << 6)
#define C13 (1u << 9)
#define CM7 (1u << 11)
#define CB9 (1u << 1)
/* 1 Ascenseur pour l'echafaud: the session as Pierre Michelot recalled it, D minor and C7, four
 * bars each, ad lib (in the key's Dorian: i m7, bVII7) */
static const prog_step PG_LIFT[] = {
	{0, 4, {0, 3, 7, 10}, C9 | C11, 4}, {10, 4, {0, 4, 7, 10}, C9 | C13, 4}};
/* 2 Blade Runner's love theme (after a transcription's reading): IV and I for eight bars, then
 * ii, III7, vi, vi dim, bVII, V7 and home */
static const prog_step PG_RAIN[] = {
	{5, 4, {0, 4, 7, 11}, C9 | CS11, 2}, {0, 4, {0, 4, 7, 11}, C9 | C13, 2},
	{5, 4, {0, 4, 7, 11}, C9 | CS11, 2}, {0, 4, {0, 4, 7, 11}, C9 | C13, 2},
	{2, 4, {0, 3, 7, 10}, C9 | C11, 1}, {4, 4, {0, 4, 7, 10}, CB9, 1}, {9, 4, {0, 3, 7, 10}, C9, 1},
	{9, 4, {0, 3, 6, 9}, 0, 1}, {10, 4, {0, 4, 7, 11}, C9, 1}, {7, 4, {0, 4, 7, 10}, C9 | C13, 1},
	{0, 4, {0, 4, 7, 11}, C9 | C13, 2}};
/* 3 Badalamenti: the Twin Peaks theme's own few chords (I, vi, IV, V, diatonic), coloured and
 * slowed: Imaj7, vi9, IVmaj7 with its sharp eleventh, V suspended, then V */
static const prog_step PG_PINES[] = {
	{0, 4, {0, 4, 7, 11}, C9, 2}, {9, 4, {0, 3, 7, 10}, C9, 2}, {5, 4, {0, 4, 7, 11}, C9 | CS11, 2},
	{7, 4, {0, 5, 7, 10}, C9, 1}, {7, 3, {0, 4, 7}, C9, 1}};
/* 4 Bohren: a few minor chords turning over like pages, each moving to a distant minor (i, the
 * minor bVI, iv, the minor v), four bars each */
static const prog_step PG_DOOM[] = {
	{0, 3, {0, 3, 7}, C9, 4}, {8, 3, {0, 3, 7}, C9, 4}, {5, 4, {0, 3, 7, 10}, C9, 4}, {7, 3, {0, 3, 7}, C9 | C11, 4}};
/* 5 the noir line cliche: the minor chord with its inner line falling a semitone at a time (root,
 * major seventh, seventh, sixth), then iv and V7 */
static const prog_step PG_CLICHE[] = {
	{0, 3, {0, 3, 7}, C9, 2}, {0, 4, {0, 3, 7, 11}, C9, 2}, {0, 4, {0, 3, 7, 10}, C9, 2}, {0, 4, {0, 3, 7, 9}, C9, 2},
	{5, 4, {0, 3, 7, 10}, C9, 2}, {7, 4, {0, 4, 7, 10}, CB9, 2}};
/* 6 Subterraneans: major and minor on the same ground, slow (I, bVI, bVII, i, iv) */
static const prog_step PG_WALL[] = {
	{0, 3, {0, 4, 7}, C9 | CM7, 4}, {8, 3, {0, 4, 7}, C9 | CM7, 4}, {10, 3, {0, 4, 7}, C9, 4},
	{0, 3, {0, 3, 7}, C9, 4}, {5, 3, {0, 3, 7}, C9, 4}};
/* 7 a ballad's changes, as Chet Baker played them: Imaj7, vi7, ii7, V7, then iii7, VI7, ii7, V7 */
static const prog_step PG_BALLAD[] = {
	{0, 4, {0, 4, 7, 11}, C9 | C13, 2}, {9, 4, {0, 3, 7, 10}, C9, 2}, {2, 4, {0, 3, 7, 10}, C9 | C11, 2},
	{7, 4, {0, 4, 7, 10}, C9 | C13, 2}, {4, 4, {0, 3, 7, 10}, C11, 2}, {9, 4, {0, 4, 7, 10}, C9, 2},
	{2, 4, {0, 3, 7, 10}, C9 | C11, 2}, {7, 4, {0, 4, 7, 10}, C9 | C13, 2}};
/* 8 (B) the line cliche once, then away from it: i held, bVI, iv and V before it comes round again */
static const prog_step PG_CLICHE2[] = {
	{0, 3, {0, 3, 7}, C9, 2}, {0, 4, {0, 3, 7, 11}, C9, 2}, {0, 4, {0, 3, 7, 10}, C9, 2}, {0, 4, {0, 3, 7, 9}, C9, 2},
	{5, 4, {0, 3, 7, 10}, C9, 2}, {7, 4, {0, 4, 7, 10}, CB9, 2},
	{0, 4, {0, 3, 7, 10}, C9 | C11, 4}, {8, 4, {0, 4, 7, 11}, C9, 4}, {5, 4, {0, 3, 7, 10}, C9, 2}, {7, 3, {0, 4, 7}, C9, 2}};
static const struct { const prog_step *s; uint8_t n; } PROGS[8] = {
	{PG_LIFT, 2}, {PG_RAIN, 11}, {PG_PINES, 5}, {PG_DOOM, 4}, {PG_CLICHE, 6}, {PG_WALL, 5}, {PG_BALLAD, 8}, {PG_CLICHE2, 10}};
static uint8_t ph_style = 1;                       /* the phrasing: 0 A (as first written), 1 B (after the listening notes) */
static uint8_t ph_piece;                           /* the noir piece playing (1..7, 138 on), for its manner */
void gl_phrase_style(int st) { ph_style = (uint8_t)st; }
static int prog_eff(void) { return ph_style && prog_x == 5 ? 8 : prog_x; }
/* the noir pieces (138 on) and their progressions */
static const uint8_t PIECE_PROG[7] = {4, 1, 3, 2, 7, 6, 5};
static int prog_len(void) { return prog_x ? PROGS[prog_eff() - 1].n : 1; }
static int prog_bars_of(void) { return prog_x ? PROGS[prog_eff() - 1].s[prog_i % PROGS[prog_eff() - 1].n].bars : 4; }
static void prog_chord(void)
{
	const prog_step *st = &PROGS[prog_eff() - 1].s[prog_i % PROGS[prog_eff() - 1].n];
	const int r = (root_pc + st->root) % 12;
	chord_n = st->n;
	chord_mask = 0;
	for (int k = 0; k < chord_n; k++) {
		chord_pc[k] = (r + st->iv[k]) % 12;
		chord_mask |= (uint16_t)(1u << chord_pc[k]);
	}
	colour_mask = 0;
	for (int b = 0; b < 12; b++)
		if (st->col & (1u << b))
			colour_mask |= (uint16_t)(1u << ((r + b) % 12));
	/* the melody's notes: the scale, the chord's own and its colours in place of whatever sits a
	 * semitone from them */
	uint16_t m = scale_mask;
	const uint16_t keep = chord_mask | colour_mask;
	for (int pc = 0; pc < 12; pc++)
		if ((keep >> pc) & 1u) {
			m |= (uint16_t)(1u << pc);
			const int a = (pc + 1) % 12, b = (pc + 11) % 12;
			if (!((keep >> a) & 1u))
				m &= (uint16_t)~(1u << a);
			if (!((keep >> b) & 1u))
				m &= (uint16_t)~(1u << b);
		}
	mel_mask = m ? m : scale_mask;
	gl_chord_root = (uint8_t)chord_pc[0];
	chord_count++;
}

/* ================================================================ the phrase */
/* Under a progression the melodic voices (Trumpet, Sax, Cello, Vibes) sing in phrases, as the
 * ballad players do: a motif of three to five notes leaning into a long tone (the chord's third,
 * seventh, fifth or a colour); the motif again, fitted to the chord it now meets; an answer
 * stepping down to rest on the root or the fifth; a breath of a bar or more; the motif again, and
 * after two or three times a new one. Each part starts on or just after the bar and a little
 * behind the beat (20 to 80 ms). Only the middle of each instrument: nothing shrill.
 * In the channel's loop memory: loop[0] the motif's rhythm (half-beats; [6] how many), loop[1]
 * its contour (steps through the melody's notes), loop[2] this part's notes ([6] how many, [7]
 * the statement's first), loop[3] their onsets in sixteenths ([6..11] their lengths); lpos[0] the
 * part (0 resting, 1 statement, 2 again, 3 answer), lpos[1] sixteenths into it (255: waiting for
 * the bar), lpos[2] the next note, lpos[3] times stated; arp_dir bars of rest; arp_i the lag in
 * sixteenths after the bar */
static const int8_t PH_RHY[6][6] = {
	{1, 1, 2, 6, 0}, {2, 2, 8, 0}, {1, 1, 1, 1, 8, 0}, {3, 1, 8, 0}, {2, 1, 1, 2, 6, 0}, {4, 2, 8, 0}};
static const int8_t PH_VRHY[3][6] = {{2, 2, 2, 6, 0}, {1, 1, 1, 1, 8, 0}, {3, 1, 4, 8, 0}};
static const int8_t PH_ANS[3][6] = {{2, 2, 8, 0}, {1, 1, 2, 8, 0}, {3, 1, 2, 8, 0}};
static void ph_range(int voice, int base, int *lo, int *hi)
{
	int l, h, home;
	switch (voice) {
	case GV_TRUMPET: l = 53; h = 69; break;            /* F3 to A4 */
	case GV_SAX: l = 46; h = 65; break;                /* B-flat 2 to F4: a tenor's middle */
	case GV_CELLO: l = 40; h = 60; break;              /* E2 to C4 */
	default: l = 53; h = 74; break;                    /* the vibes: F3 to D5 */
	}
	home = HOME[voice];
	int off = base - home;                             /* REGISTER: down, never up */
	off = off > 0 ? 0 : off;
	/* and never under the instrument's own floor (a tenor's low A-flat, a trumpet's E, a cello's C) */
	const int floor_ = voice == GV_TRUMPET ? 52 : voice == GV_SAX ? 44 : voice == GV_CELLO ? 36 : 53;
	*lo = l + off < floor_ ? floor_ : l + off;
	*hi = h + off < *lo + 12 ? *lo + 12 : h + off;
}
static int mel_step(int note, int k, uint16_t set)
{
	if (!set)
		set = scale_mask;
	const int d = k > 0 ? 1 : -1;
	for (int n = k > 0 ? k : -k; n > 0 && note > 12 && note < 115;) {
		note += d;
		if ((set >> (note % 12)) & 1u)
			n--;
	}
	return note;
}
static int nearest_in(int note, uint16_t set, int lo, int hi)
{
	for (int d = 0; d < 12; d++) {
		if (note - d >= lo && ((set >> ((note - d) % 12)) & 1u))
			return note - d;
		if (note + d <= hi && ((set >> ((note + d) % 12)) & 1u))
			return note + d;
	}
	return note < lo ? lo : note > hi ? hi : note;
}
static void ph_new(chan *c, int vib)
{
	int8_t *rh = c->loop[0], *ct = c->loop[1];
	const int8_t *r = vib ? PH_VRHY[rndi(3)] : PH_RHY[rndi(6)];
	int n = 0;
	while (n < 6 && r[n]) {
		rh[n] = r[n];
		n++;
	}
	rh[6] = (int8_t)n;
	/* its shape: an arch (a leap up, then stepping down into the long tone), a sigh (stepping
	 * down), or a turn (up, down, and down again into the tone); the vibes go up the chord */
	const int shape = rndi(3);
	ct[0] = 0;
	for (int k = 1; k < n; k++) {
		if (vib)
			ct[k] = 1;
		else if (shape == 0)
			ct[k] = (int8_t)(k == 1 ? 3 + rndi(2) : -1);
		else if (shape == 1)
			ct[k] = -1;
		else
			ct[k] = (int8_t)((k & 1) ? 1 : -1 - (k == n - 1));
	}
}
static void ph_fit(chan *c, int part, int lo, int hi, int vib)
{
	int8_t *rh = c->loop[0], *ct = c->loop[1], *nt = c->loop[2], *on = c->loop[3];
	const uint16_t set = vib ? (uint16_t)(chord_mask | colour_mask) : mel_mask;
	const int center = (lo + hi) / 2;
	int8_t dur[6], stp[6];
	int n = 0;
	if (part == 3) {                                   /* the answer: down to the root or the fifth */
		const int8_t *r = PH_ANS[rndi(3)];
		while (n < 5 && r[n]) {
			dur[n] = r[n];
			stp[n] = (int8_t)(n ? -1 : 0);
			n++;
		}
	} else {
		n = rh[6] > 0 ? rh[6] : 1;
		for (int k = 0; k < n; k++) {
			dur[k] = rh[k];
			stp[k] = ct[k];
		}
	}
	int start;
	if (part == 1)
		start = center - 3 + rndi(5);
	else if (part == 2)
		start = nt[7] > 0 ? nt[7] : center;                /* the same height, on the chord it meets */
	else
		start = (c->last_note > 0 ? c->last_note : center) + 2;
	int tmp[6];
	tmp[0] = nearest_in(start, chord_mask, lo, hi);
	for (int k = 1; k < n; k++)
		tmp[k] = mel_step(tmp[k - 1], stp[k], set);
	/* the long tone: one of the chord's beautiful notes (the answer: its root or fifth) */
	const uint16_t beauty = (uint16_t)((1u << chord_pc[1]) | (1u << chord_pc[2 % chord_n]) |
	                                   (chord_n > 3 ? (1u << chord_pc[3]) : 0u) | colour_mask);
	const uint16_t rest = (uint16_t)((1u << chord_pc[0]) | (1u << chord_pc[2 % chord_n]));
	if (part == 3) {                                   /* the answer settles: down to its rest, never up */
		int x = tmp[n - 1];
		while (x > tmp[n - 1] - 12 && !((rest >> (x % 12)) & 1u))
			x--;
		tmp[n - 1] = x;
	} else
		tmp[n - 1] = nearest_in(tmp[n - 1], beauty, tmp[n - 1] - 5, tmp[n - 1] + 5);
	/* into the range: the whole phrase an octave down (or up), then any stray note folded in */
	int mx = tmp[0], mn = tmp[0];
	for (int k = 1; k < n; k++) {
		mx = tmp[k] > mx ? tmp[k] : mx;
		mn = tmp[k] < mn ? tmp[k] : mn;
	}
	int sh = 0;
	while (mx + sh > hi && mn + sh - 12 >= lo - 2)
		sh -= 12;
	while (mn + sh < lo && mx + sh + 12 <= hi + 2)
		sh += 12;
	int acc = 0;
	for (int k = 0; k < n; k++) {
		int x = tmp[k] + sh;
		while (x > hi)
			x -= 12;
		while (x < lo)
			x += 12;
		nt[k] = (int8_t)x;
		on[k] = (int8_t)acc;
		on[6 + k] = dur[k];
		acc += 2 * dur[k];
	}
	nt[6] = (int8_t)n;
	if (part == 1)
		nt[7] = nt[0];
	if (part != 3)
		c->last_note = nt[n - 1];
}
/* ---- shaped envelopes (audition, the noir songs, off unless asked): the long notes breathe the
 * way players hold them, and listen. A lead's note (the sax, trumpet, cello, breath) takes a contour
 * from how it comes: rising, it is pushed out (a lift into it); falling, it relaxes (leaning down
 * through it, or sagging and coming back); a step, a sag, two dips or a bloom; the same note again,
 * re-articulated; the first after a breath, blooming from soft. The drones (pad, choir, swell, organ,
 * strings, drone) take a new contour at every chord and every note of their own; the brushes drift
 * through them a bar or two long. And they make room: another instrument coming in, the leads and
 * drones give way a little (the drones more for a lead), a lead pushing out a note of its own
 * comes back through (the bass and the brushes keep time: they are not made room for). Only ever down from the level as written (to ~-6 dB a lead, ~-7 the rest),
 * glided and slewed (never faster than ~9 dB a second), so no attack or decay gets sharper. The
 * loudest long notes reach their peak and then fade through the rest, and a held chord fades a
 * little after its peak. Shaped, a lead cut off by its next note lets go over 0.6 s (not in a
 * snap; a note changing, the last is held a quarter second as the next swells in, then let go
 * as written), and the swell's second carrier, beating against the first, is brought down to 0.4, each breath
 * beating at its own pace (as written or slower, to a third). And the leads and drones sit 4 dB
 * under their level as written. The
 * bass, upright, vibes and Rhodes are left as they are, but heard. Its own dice: the notes are the
 * same on or off ---- */
void gl_env_shape(int on) { esh_on = (uint8_t)(on != 0); }
enum { ES_FLAT, ES_SAG, ES_BLOOM, ES_TWIST, ES_LEAN, ES_PUSH };
static struct {
	float t, len, depth, dip, att;                 /* the contour: time in, its length, depth; the dip now; all it takes off */
	float room, room_t, room_amt;                  /* making room for another */
	float pg, fade, fpk;                           /* the glow a tick ago (a rise: a note); after its peak, the fade: how far, from where */
	int8_t last;                                   /* the lead's last note */
	uint8_t shape, chord;
} esh[GL_NCH];
static uint32_t esh_rng = 0x9E3779B9u;
static float esh_rnd(void)
{
	uint32_t x = esh_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	esh_rng = x;
	return (float)(x >> 8) * (1.0f / 16777216.0f);
}
static int esh_kind(int v)
{
	if (v == GV_SAX || v == GV_TRUMPET || v == GV_CELLO || v == GV_BREATH)
		return 1;                                  /* a lead: a contour per note */
	if (v == GV_PAD || v == GV_CHOIR || v == GV_DRONE || v == GV_SWELL || v == GV_ORGAN || v == GV_STRINGS)
		return 2;                                  /* a drone: a contour per chord, per note */
	return v == GV_BRUSH ? 3 : 0;
}
static float esh_s01(float x) { x = clampf(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
static float esh_cos(float u) { return sin_u((uint32_t)(u * 4294967296.0f) + 0x40000000u); }   /* cos(2 pi u) */
/* how far down (0 none, d the deepest) at u, 0..1 through the note or the stretch */
static float esh_curve(int shape, float u, float d)
{
	u = clampf(u, 0.0f, 1.0f);
	switch (shape) {
	case ES_SAG:   return d * (0.5f - 0.5f * esh_cos(u));                       /* down in the middle, back */
	case ES_BLOOM: return d * (1.0f - esh_s01(u * (1.0f / 0.6f))) + 0.45f * d * esh_s01((u - 0.6f) * 2.5f);
	case ES_TWIST: return d * (0.5f - 0.5f * esh_cos(2.0f * u)) * (u < 0.5f ? 1.0f : 0.6f);   /* two dips, the second less */
	case ES_LEAN:  return d * esh_s01((u - 0.1f) * (1.0f / 0.75f));              /* full, then leaning down */
	case ES_PUSH:  return 0.55f * d * (1.0f - esh_s01(u * 4.0f)) + 0.6f * d * esh_s01((u - 0.45f) * 1.8f);   /* lifted into, easing off */
	default:       return 0.0f;
	}
}
/* where each contour peaks (u): the fade, if any, starts there */
static const float ESH_PEAK[6] = {0.1f, 0.1f, 0.6f, 0.1f, 0.1f, 0.25f};
static void esh_set(int c, int shape, float len, float d)
{
	esh[c].shape = (uint8_t)shape;
	esh[c].len = len;
	esh[c].depth = d;
	esh[c].t = 0.0f;
	esh[c].fade = 0.0f;
	esh[c].fpk = ESH_PEAK[shape];
}
/* a lead's note, as the phrase plays it: hold its length in seconds */
static void esh_note(int c, int note, float vel, float hold)
{
	if (!esh_on || !ph_piece || esh_kind(CH[c].voice) != 1)
		return;
	const int iv = esh[c].last ? note - esh[c].last : 99;
	const float gap = esh[c].t - esh[c].len;            /* (past the last note's end: a breath, if long) */
	esh[c].last = (int8_t)note;
	esh[c].room_amt *= 0.3f;                            /* pushing out a note of its own: back through */
	if (hold < 0.6f) {                                  /* short notes as they are */
		esh_set(c, ES_FLAT, hold, 0.0f);
		return;
	}
	const float r = esh_rnd();
	int sh;
	if (iv == 99 || gap > 1.5f)
		sh = r < 0.6f ? ES_BLOOM : ES_PUSH;             /* after a breath: from soft */
	else if (iv > 2)
		sh = r < 0.5f ? ES_PUSH : r < 0.8f ? ES_BLOOM : ES_TWIST;   /* rising: pushed out */
	else if (iv < -2)
		sh = r < 0.45f ? ES_LEAN : r < 0.8f ? ES_SAG : ES_FLAT;     /* falling: relaxing */
	else if (iv == 0)
		sh = r < 0.5f ? ES_TWIST : ES_SAG;              /* the same note again: re-articulated */
	else
		sh = r < 0.4f ? ES_SAG : r < 0.7f ? ES_TWIST : ES_BLOOM;    /* a step */
	float d = 0.25f + 0.2f * esh_rnd() + (hold > 3.0f ? 0.05f : 0.0f) + 0.05f * (1.0f - vel);
	esh_set(c, sh, hold, clampf(d, 0.0f, 0.5f));
	/* the loudest long notes: their peak, then fading through the rest (to -2 .. -4.5 dB) */
	if (hold >= 1.5f && vel >= 0.6f)
		esh[c].fade = 0.2f + 0.2f * clampf((vel - 0.6f) * 2.5f, 0.0f, 1.0f);
}
static void esh_drone(int c, float bar, int kind)
{
	const float r = esh_rnd();
	if (kind == 2) {
		esh_set(c, r < 0.3f ? ES_BLOOM : r < 0.6f ? ES_SAG : r < 0.8f ? ES_TWIST : ES_LEAN,
		        bar * (2.0f + 2.0f * esh_rnd()), 0.3f + 0.25f * esh_rnd());
		esh[c].fade = 0.12f + 0.12f * esh_rnd();         /* a chord held: after its peak, a little fading */
	} else
		esh_set(c, r < 0.15f ? ES_FLAT : r < 0.6f ? ES_SAG : r < 0.8f ? ES_TWIST : ES_BLOOM,
		        bar * (1.0f + 1.5f * esh_rnd()), 0.25f + 0.2f * esh_rnd());
}
/* each control tick: who began a note; the contours move on; the dips go onto the channels' levels */
static void esh_control(float dt)
{
	sw_b = ESH_LIVE ? 0.4f : 1.0f;                      /* (the swell's beating second carrier: less of it) */
	sw_g = ESH_LIVE ? 1.3f : 1.0f;
	if (!esh_on || !ph_piece) {
		for (int c = 0; c < GL_NCH; c++) {
			esh[c].dip = esh[c].att = 0.0f;
			esh[c].pg = CH[c].glow;
		}
		return;
	}
	const float bar = 4.0f * 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	const float k = dt / (0.2f + dt), slew = 0.5f * dt;   /* (glided, and never faster than ~9 dB a second) */
	float on[GL_NCH];
	for (int c = 0; c < GL_NCH; c++) {
		on[c] = CH[c].glow > esh[c].pg + 0.03f ? clampf(CH[c].glow, 0.4f, 1.0f) : 0.0f;
		esh[c].pg = CH[c].glow;
	}
	const uint8_t chord = (uint8_t)chord_count;
	for (int c = 0; c < GL_NCH; c++) {
		const int kind = esh_kind(CH[c].voice);
		if (!kind) {
			esh[c].dip = esh[c].att = 0.0f;
			continue;
		}
		/* making room: another coming in (a lead, for a drone, more) */
		for (int o = 0; o < GL_NCH; o++) {
			if (o == c || on[o] <= 0.0f)
				continue;
			const int ko = esh_kind(CH[o].voice);
			if (ko == 3 || (!ko && CH[o].voice != GV_RHODES && CH[o].voice != GV_VIBES))
				continue;                              /* (the bass and the brushes keep time: no one gives way to them) */
			const float amt = on[o] * (kind == 2 && ko == 1 ? 0.3f : kind == 1 ? 0.18f : 0.14f);
			const float now = esh[c].room_amt * (esh[c].room_t < 0.3f ? 1.0f : fexp2(-(esh[c].room_t - 0.3f) * 0.96f));
			if (amt > now) {
				esh[c].room_amt = amt;
				esh[c].room_t = 0.0f;
			}
		}
		esh[c].room_t += dt;
		const float rt = esh[c].room_t;
		esh[c].room = esh[c].room_amt * (rt < 0.3f ? esh_s01(rt * (1.0f / 0.3f)) : fexp2(-(rt - 0.3f) * 0.96f));   /* in over 0.3 s, back over ~1.5 */
		/* the contour */
		esh[c].t += dt;
		if (kind == 2 && (chord != esh[c].chord || on[c] > 0.0f || esh[c].len <= 0.0f || esh[c].t >= esh[c].len)) {
			esh[c].chord = chord;                          /* a new chord, a note of its own: a new contour */
			esh_drone(c, bar, 2);
		} else if (kind == 3 && (esh[c].len <= 0.0f || esh[c].t >= esh[c].len))
			esh_drone(c, bar, 3);
		const float u = esh[c].t / (esh[c].len > 0.0f ? esh[c].len : 1.0f);
		const float sh = esh[c].t < esh[c].len ? esh_curve(esh[c].shape, u, esh[c].depth) : 0.0f;
		const float fd = esh[c].t < esh[c].len ? esh[c].fade * esh_s01((u - esh[c].fpk) / (1.0f - esh[c].fpk)) : 0.0f;
		const float want = clampf(1.0f - (1.0f - sh) * (1.0f - fd) * (1.0f - esh[c].room), 0.0f, 0.55f);   /* (all told, never past ~-7 dB) */
		esh[c].dip += clampf((want - esh[c].dip) * k, -slew, slew);
		/* the held: the leads and drones 4 dB under as written (the brushes, the rest, as they are) */
		esh[c].att = 1.0f - (1.0f - esh[c].dip) * (kind == 3 ? 1.0f : 0.631f);
	}
}
#ifdef GL_PH_TRACE
void ph_trace(int ch, int note, float hold);   /* (a test build's hook: each phrase note as it sounds) */
#endif
static void ph_fire_now(chan *c, int ch, int note, float vel, float hold, float emph)
{
	esh_note(ch, note, vel, hold);
#ifdef GL_PH_TRACE
	ph_trace(ch, note, hold);
#endif
	if (c->voice == GV_VIBES) {
		nvoice *v = alloc(c, 4);
		if (!sounding(v))
			make_room(v);
		nv_trigger(v, (float)note, vel, 1.0f, c->vnext++);
		c->glow = vel;
	} else
		line_fire(c, ch, note, vel, hold, emph);
}
static void ph_pending(chan *c, int ch, float sdt)
{
	if (c->echo_at <= 0.0f)
		return;
	c->echo_at -= sdt;
	if (c->echo_at <= 0.0f) {
		c->echo_at = 0.0f;
		ph_fire_now(c, ch, (int)c->echo_note, c->echo_vel, c->hq_dt, c->hq_vel);
	}
}
static void phrase_step_b(chan *c, int ch, uint32_t step, int base);
static void phrase_step(chan *c, int ch, uint32_t step, int base)
{
	if (ph_style) {
		phrase_step_b(c, ch, step, base);
		return;
	}
	const int vib = c->voice == GV_VIBES;
	int lo, hi;
	ph_range(c->voice, base, &lo, &hi);
	const int s16 = (int)(step & 15u);
	uint8_t *part = &c->lpos[0], *pos = &c->lpos[1], *idx = &c->lpos[2], *times = &c->lpos[3];
	const float act = eff_activity(ch);
	if (*part == 0) {                                  /* a breath between phrases */
		if (s16 != 0)
			return;
		if (c->arp_dir) {
			c->arp_dir--;
			return;
		}
		if (c->loop[0][6] <= 0 || *times >= 2 + rndi(2)) {
			ph_new(c, vib);
			*times = 0;
		}
		*part = 1;
		*pos = 255;
	}
	if (*pos == 255) {                                 /* waiting for the bar */
		if (s16 != 0)
			return;
		*pos = 0;
		*idx = 0;
		c->arp_i = (uint8_t)(rnd() < 0.5f ? 0 : 2 * (1 + rndi(2)));   /* on the bar, or an eighth or a beat after */
		ph_fit(c, *part, lo, hi, vib);
	}
	const int n = c->loop[2][6];
	const int off = c->arp_i;
	const float hb = 30.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	while (*idx < n && (int)*pos == off + c->loop[3][*idx]) {
		const int k = *idx, dur = c->loop[3][6 + k], last = k == n - 1;
		const float hold = (float)dur * hb * (last ? 0.92f : 1.04f);   /* legato, the last let go */
		float vel = 0.5f + 0.22f * (float)k / (float)n + (last ? 0.08f : 0.0f);   /* leaning into the tone */
		if (*part == 3)
			vel *= 0.85f;
		c->echo_note = (float)c->loop[2][k];
		c->echo_vel = vel * (0.95f + 0.1f * rnd());
		c->hq_dt = hold;
		c->hq_vel = dur >= 6 ? 1.0f : 0.0f;
		c->echo_at = 0.02f + 0.06f * rnd();             /* behind the beat */
		(*idx)++;
	}
	if (*pos < 250)
		(*pos)++;
	if (*idx >= n && (int)*pos >= off + c->loop[3][n - 1] + 2 * c->loop[3][6 + n - 1]) {
		if (*part == 1)
			*part = rnd() < 0.85f ? 2 : 3;
		else if (*part == 2)
			*part = rnd() < 0.75f ? 3 : 0;
		else
			*part = 0;
		if (*part == 0) {
			(*times)++;
			c->arp_dir = (uint8_t)((1 + rndi(3)) * (1.5f - act));
		}
		*pos = 255;
	}
}

/* ---- B: the phrasing after the first listening. What A did: every piece alike, a motif of two
 * to five notes (six rhythms, three shapes: arch, sigh, turn) played straight through, the same
 * again on the next chord, then an answer that always stepped down to the root or fifth: too many
 * notes too close, and the same falling close everywhere. B:
 *   a breath inside the phrase: the motif split after its first note (or two), the rest waiting
 *     two to six half-beats, at times a bar or two (glacial);
 *   fewer notes: one-, two- and three-note rhythms most of the time;
 *   the motif varied when it comes back (whole, cut short and held, a beat late, landing on another
 *     colour, or with a neighbour leaning into the tone), and changed a little between times
 *     rather than always replaced;
 *   answers of four kinds: down to the root (now the rarest), up to a third or a colour, one long
 *     tone, or none at all;
 *   each piece its own manner (PH_MANNER): how much space, how often glacial, how sparse, and
 *     whether its lead keeps one hook and comes home to it (Pine Lake: the hook returns, and on the
 *     home chord it resolves to the tonic) ---- */
static const struct { uint8_t space, glacial, sparse, hook; } PH_MANNER[8] = {
	{50, 20, 50, 0},   /* (another piece) */
	{75, 25, 60, 0},   /* Rain Window: pauses between the first notes and the next */
	{65, 30, 60, 0},   /* Elevator, Paris */
	{30, 10, 40, 1},   /* Pine Lake: the hook */
	{60, 25, 85, 0},   /* Neon Rain: fewer notes */
	{55, 20, 55, 0},   /* Last Ferry */
	{60, 25, 60, 0},   /* Under the Wall */
	{85, 50, 70, 0},   /* Velvet Doom: glacial */
};
static const int8_t PB_RHY[8][5] = {               /* half-beats */
	{6, 0}, {2, 6, 0}, {1, 7, 0}, {4, 8, 0},       /* one or two notes */
	{3, 1, 6, 0}, {1, 1, 6, 0}, {2, 2, 8, 0}, {1, 1, 2, 6, 0}};
static const int8_t PB_HOOK[5] = {2, 2, 4, 8, 0};  /* the hook's rhythm (vibes: up the chord, a step down into its tone) */
/* ---- C: between A and B, on purpose. Each piece's lead moves along an arc over four minutes:
 * sparse at the start, the conversation filling in, a pulling back, the fullest, then a coda. At the
 * top of the arc it phrases as A did (busier rhythms, few breaths, short rests); at the bottom, as B
 * or sparer (one- and two-note rhythms, breaths inside the phrase, glacial ones, long rests). And
 * call and response: when the lead finishes a phrase, its responder (the Rhodes; in Last Ferry the
 * cello) answers in the gap at the next bar with the lead's last notes, lower, and the lead waits a
 * bar for it. More answering the fuller it gets. The brushes follow the arc too: the taps come in
 * as it fills, and at its sparest they fade almost out before coming back */
static const int8_t C_LEAD[8] = {-1, 0, 0, 0, 1, 0, 2, 1};   /* by piece: the lead's channel */
static const int8_t C_RESP[8] = {-1, 1, 3, 3, 3, 1, -1, 2};  /* and its responder's (-1 none) */
static const uint8_t C_FLOOR[8] = {0, 0, 0, 20, 0, 0, 0, 45}; /* (Velvet Doom never quite empty; the hook never long gone) */
static float ph_p = 0.5f;                                    /* the arc, now (0 sparest .. 1 fullest) */
static struct { int8_t notes[6], rh[6], ct[6]; uint8_t n, pending; } conv;
static int ph_resp_start;
static float c_int(void)
{
	static const float KX[7] = {0.0f, 35.0f, 75.0f, 110.0f, 150.0f, 195.0f, 240.0f};
	static const float KY[7] = {0.2f, 0.45f, 0.85f, 0.5f, 0.95f, 0.35f, 0.2f};
	const float bpm = gl_tempo_bpm(P.g[GP_TEMPO]);
	float t = (float)step_n * (15.0f / bpm);
	t -= 240.0f * (float)(int)(t * (1.0f / 240.0f));
	int k = 1;
	while (k < 6 && t > KX[k])
		k++;
	float x = (t - KX[k - 1]) / (KX[k] - KX[k - 1]);
	x = x * x * (3.0f - 2.0f * x);
	const float p = KY[k - 1] + (KY[k] - KY[k - 1]) * x;
	const int m = ph_piece < 8 ? ph_piece : 0;
	const float fl = C_FLOOR[m] * 0.01f;
	return fl + (1.0f - fl) * p;
}
static void ph_new_b(chan *c, int vib)
{
	int8_t *rh = c->loop[0], *ct = c->loop[1];
	const int m = ph_piece < 8 ? ph_piece : 0;
	const float space = PH_MANNER[m].space * 0.01f, glac = PH_MANNER[m].glacial * 0.01f, sparse = PH_MANNER[m].sparse * 0.01f;
	const int hook = PH_MANNER[m].hook && vib;
	if (hook && rh[6] > 0)
		return;                                        /* the hook is kept */
	const int8_t *r;
	if (hook)
		r = PB_HOOK;
	else if (ph_style == 2) {                          /* C: the arc decides */
		if (ph_p > 0.62f) {
			static const uint8_t FULL[3] = {0, 2, 4};     /* toward the top: A's fullest rhythms (four and five notes) */
			r = PH_RHY[ph_p > 0.8f ? FULL[rndi(3)] : rndi(6)];
		}
		else if (ph_p < 0.35f)
			r = PB_RHY[rndi(4)];                       /* at the bottom: one or two notes */
		else
			r = rnd() < (ph_p - 0.35f) * 3.7f ? PB_RHY[4 + rndi(4)] : PB_RHY[rndi(4)];
	} else
		r = rnd() < sparse ? PB_RHY[rndi(4)] : PB_RHY[4 + rndi(4)];
	int n = 0;
	while (n < 5 && r[n]) {
		rh[n] = r[n];
		n++;
	}
	rh[6] = (int8_t)n;
	/* the breath inside it: mostly after the first note */
	rh[7] = rh[8] = 0;
	if (n >= 2 && !hook && rnd() < space) {
		rh[7] = (int8_t)(rnd() < 0.6f ? 1 : 1 + rndi(n - 1));
		rh[8] = (int8_t)(rnd() < glac ? 8 + rndi(9) : 2 + rndi(5));
	}
	/* its shape: a question (ending a step or two up), an arch, a turn, a repeated note then a
	 * leap, and the sigh (stepping down) now the least of them */
	const float u = rnd();
	const int shape = u < 0.25f ? 0 : u < 0.5f ? 1 : u < 0.65f ? 2 : u < 0.85f ? 3 : 4;
	ct[0] = 0;
	for (int k = 1; k < n; k++) {
		if (vib)
			ct[k] = (int8_t)(k == n - 1 && n > 2 ? -1 : 1);
		else if (shape == 0)
			ct[k] = (int8_t)(k == n - 1 ? 2 : 1);
		else if (shape == 1)
			ct[k] = (int8_t)(k == 1 ? 3 + rndi(2) : -1);
		else if (shape == 2)
			ct[k] = (int8_t)((k & 1) ? 1 : -2);
		else if (shape == 3)
			ct[k] = (int8_t)(k == 1 ? 0 : k == 2 ? 3 : -1);
		else
			ct[k] = -1;
	}
}
/* change it a little between times: one length, or one step */
static void ph_mutate(chan *c)
{
	int8_t *rh = c->loop[0], *ct = c->loop[1];
	const int n = rh[6];
	if (n < 1)
		return;
	const int k = rndi(n);
	if (rnd() < 0.5f && k < n - 1)
		rh[k] = (int8_t)(rh[k] == 1 ? 2 : rh[k] == 2 ? (rnd() < 0.5f ? 1 : 3) : 2);
	else if (k > 0)
		ct[k] = (int8_t)(ct[k] + (rnd() < 0.5f ? 1 : -1));
}
/* this part's notes; returns 0, or -1 for an answer left out */
static int ph_fit_b(chan *c, int part, int lo, int hi, int vib)
{
	int8_t *rh = c->loop[0], *ct = c->loop[1], *nt = c->loop[2], *on = c->loop[3];
	const int m = ph_piece < 8 ? ph_piece : 0;
	const int hook = PH_MANNER[m].hook && vib;
	const uint16_t set = vib ? (uint16_t)(chord_mask | colour_mask) : mel_mask;
	const int center = (lo + hi) / 2;
	const uint16_t beauty = (uint16_t)((1u << chord_pc[1]) | (1u << chord_pc[2 % chord_n]) |
	                                   (chord_n > 3 ? (1u << chord_pc[3]) : 0u) | colour_mask);
	const uint16_t homes = (uint16_t)((1u << chord_pc[0]) | (1u << chord_pc[2 % chord_n]));
	int8_t dur[6], stp[6];
	int n = 0, split = 0, gap = 0, start, land_up = 0;
	uint16_t land = beauty;
	if (part == 3) {
		/* the answer: home to the tonic on the home chord (the hook), else one of four */
		const int home = hook && chord_pc[0] == root_pc;
		const float u = rnd();
		const float none = ph_style == 2 ? 0.25f * (1.2f - ph_p) : 0.25f;
		const int form = home ? 0 : u < 0.2f ? 0 : u < 0.5f ? 1 : u < 1.0f - none ? 2 : 3;
		if (form == 3)
			return -1;                                 /* no answer: the space is the answer */
		static const int8_t AR[3][4] = {{2, 2, 8, 0}, {2, 8, 0, 0}, {8, 0, 0, 0}};
		const int8_t *r = home ? AR[0] : form == 0 ? AR[rndi(2)] : form == 1 ? AR[1] : AR[2];
		while (n < 4 && r[n]) {
			dur[n] = r[n];
			stp[n] = (int8_t)(n == 0 ? 0 : form == 1 ? 1 : -1);
			n++;
		}
		if (home)
			land = (uint16_t)(1u << root_pc);
		else if (form == 0)
			land = homes;
		else if (form == 1) {
			land = (uint16_t)((1u << chord_pc[1]) | colour_mask);
			land_up = 1;
		} else
			land = (uint16_t)((1u << chord_pc[1]) | (1u << chord_pc[2 % chord_n]));
		start = (c->last_note > 0 ? c->last_note : center) + (form == 1 ? -2 : 2);
	} else {
		n = rh[6] > 0 ? rh[6] : 1;
		for (int k = 0; k < n; k++) {
			dur[k] = rh[k];
			stp[k] = ct[k];
		}
		/* the breath, chosen afresh each time it is played: after the first note or the second,
		 * two to six half-beats, at times a bar or two */
		{
			float space = PH_MANNER[m].space * 0.01f, glac = PH_MANNER[m].glacial * 0.01f;
			if (ph_style == 2) {                          /* (C: the breaths with the arc: none near the top, always near the bottom) */
				space = ph_p > 0.7f ? 0.0f : ph_p < 0.35f ? 1.0f : space * (1.4f - ph_p);
				glac = ph_p < 0.35f ? glac * 1.5f : ph_p < 0.55f ? glac * 0.5f : 0.0f;
			}
			split = gap = 0;
			if (n >= 2 && !hook && rnd() < space) {
				split = n >= 3 && rnd() < 0.55f ? 2 : 1;
				gap = rnd() < glac ? 8 + rndi(9) : 3 + rndi(5);
			}
		}
		start = part == 1 ? (ph_resp_start ? ph_resp_start : center - 3 + rndi(5)) : (nt[7] > 0 ? nt[7] : center);
		if (part == 2) {
			/* the motif again, varied (the hook keeps its shape: whole, cut short, or late) */
			const float u = hook ? rnd() * 0.7f : rnd();
			if (u < 0.3f) {
				/* whole */
			} else if (u < 0.5f && n > 1) {
				n = split > 0 ? split : 1;               /* cut short, its last note held */
				dur[n - 1] = 8;
				split = 0;
			} else if (u < 0.7f) {
				c->arp_i = (uint8_t)(c->arp_i + 4);      /* a beat late */
			} else if (u < 0.85f) {
				const uint16_t other = (uint16_t)((chord_n > 3 ? (1u << chord_pc[3]) : 0u) | colour_mask);
				if (other)
					land = other;                        /* landing on another colour */
			} else if (n < 5) {
				dur[n] = dur[n - 1];                     /* a neighbour leaning into the tone */
				stp[n] = (int8_t)(rnd() < 0.5f ? 1 : -1);
				dur[n - 1] = 1;
				n++;
			}
		}
	}
	int tmp[6];
	tmp[0] = nearest_in(start, chord_mask, lo, hi);
	for (int k = 1; k < n; k++)
		tmp[k] = mel_step(tmp[k - 1], stp[k], set);
	{
		int x = tmp[n - 1];
		if (land_up) {
			while (x < tmp[n - 1] + 12 && !((land >> (x % 12)) & 1u))
				x++;
		} else if (part == 3 && land != beauty) {
			while (x > tmp[n - 1] - 12 && !((land >> (x % 12)) & 1u))
				x--;
		} else
			x = nearest_in(x, land, x - 5, x + 5);
		tmp[n - 1] = x;
	}
	int mx = tmp[0], mn = tmp[0];
	for (int k = 1; k < n; k++) {
		mx = tmp[k] > mx ? tmp[k] : mx;
		mn = tmp[k] < mn ? tmp[k] : mn;
	}
	int sh = 0;
	while (mx + sh > hi && mn + sh - 12 >= lo - 2)
		sh -= 12;
	while (mn + sh < lo && mx + sh + 12 <= hi + 2)
		sh += 12;
	int acc = 0;
	for (int k = 0; k < n; k++) {
		if (split && k == split)
			acc += 2 * gap;                              /* the breath inside the phrase */
		int x = tmp[k] + sh;
		while (x > hi)
			x -= 12;
		while (x < lo)
			x += 12;
		nt[k] = (int8_t)x;
		on[k] = (int8_t)acc;
		on[6 + k] = dur[k];
		acc += 2 * dur[k];
	}
	nt[6] = (int8_t)n;
	if (part == 1)
		nt[7] = nt[0];
	if (part != 3)
		c->last_note = nt[n - 1];
	return 0;
}
static void phrase_step_b(chan *c, int ch, uint32_t step, int base)
{
	const int vib = c->voice == GV_VIBES;
	int lo, hi;
	ph_range(c->voice, base, &lo, &hi);
	const int s16 = (int)(step & 15u);
	uint8_t *part = &c->lpos[0], *pos = &c->lpos[1], *idx = &c->lpos[2], *times = &c->lpos[3];
	const float act = eff_activity(ch);
	const int m = ph_piece < 8 ? ph_piece : 0;
	const int cmode = ph_style == 2 && m > 0;
	if (cmode)
		ph_p = c_int();
	if (cmode && ch == C_RESP[m]) {
		/* the responder: silent but to answer, at the bar after a call, with the call's own
		 * rhythm and shape (three notes at most), starting below where the call ended */
		if (*part == 0) {
			if (!conv.pending || s16 != 0)
				return;
			conv.pending = 0;
			const int n = conv.n < 3 ? conv.n : 3;
			for (int k = 0; k < n; k++) {
				c->loop[0][k] = conv.rh[k];
				c->loop[1][k] = conv.ct[k];
			}
			c->loop[0][n - 1] = (int8_t)(c->loop[0][n - 1] < 6 ? 6 : c->loop[0][n - 1]);
			c->loop[0][6] = (int8_t)n;
			*part = 1;
			*pos = 0;
			*idx = 0;
			*times = 200;                                 /* (an answer: once, then quiet) */
			c->arp_i = 2;
			int st = conv.notes[conv.n - 1] - 3 - rndi(6);
			while (st > hi) st -= 12;
			while (st < lo) st += 12;
			ph_resp_start = st;
			const float keep = ph_p;
			ph_p = 1.0f;                                  /* (no breath inside an answer) */
			ph_fit_b(c, 1, lo, hi, vib);
			ph_p = keep;
			ph_resp_start = 0;
		}
	} else if (*part == 0) {
		if (s16 != 0)
			return;
		if (c->arp_dir) {
			c->arp_dir--;
			return;
		}
		if (c->loop[0][6] <= 0)
			ph_new_b(c, vib);
		else if (*times >= 2 + rndi(3)) {
			if (rnd() < 0.5f)
				ph_mutate(c);                          /* the same idea, changed a little */
			else
				ph_new_b(c, vib);
			*times = 0;
		}
		*part = 1;
		*pos = 255;
	}
	if (*pos == 255) {
		if (s16 != 0)
			return;
		if (c->hq_n) {                                  /* (C: the lead waiting a bar for its answer) */
			c->hq_n--;
			return;
		}
		*pos = 0;
		*idx = 0;
		c->arp_i = (uint8_t)(rnd() < 0.5f ? 0 : 2 * (1 + rndi(2)));
		if (ph_fit_b(c, *part, lo, hi, vib) < 0) {      /* an answer left out: rest */
			*part = 0;
			(*times)++;
			c->arp_dir = (uint8_t)((1 + rndi(3)) * (1.5f - act) + (PH_MANNER[m].space > 60 ? rndi(2) : 0));
			if (cmode)
				c->arp_dir = (uint8_t)((float)c->arp_dir * (1.3f - ph_p));
			return;
		}
	}
	const int n = c->loop[2][6];
	const int off = c->arp_i;
	const float hb = 30.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	while (*idx < n && (int)*pos == off + c->loop[3][*idx]) {
		const int k = *idx, dur = c->loop[3][6 + k], last = k == n - 1;
		const int after_breath = k > 0 && c->loop[3][k] > c->loop[3][k - 1] + 2 * c->loop[3][6 + k - 1];
		const float hold = (float)dur * hb * (last ? 0.92f : 1.04f);
		float vel = 0.5f + 0.22f * (float)k / (float)n + (last ? 0.08f : 0.0f) + (after_breath ? 0.05f : 0.0f);
		if (*part == 3)
			vel *= 0.85f;
		c->echo_note = (float)c->loop[2][k];
		c->echo_vel = vel * (0.95f + 0.1f * rnd());
		c->hq_dt = hold;
		c->hq_vel = dur >= 6 ? 1.0f : 0.0f;
		c->echo_at = 0.02f + 0.06f * rnd();
		(*idx)++;
	}
	if (*pos < 250)
		(*pos)++;
	if (*idx >= n && (int)*pos >= off + c->loop[3][n - 1] + 2 * c->loop[3][6 + n - 1]) {
		if (*times == 200) {                            /* (the responder's answer, done) */
			*part = 0;
			*times = 0;
			c->arp_dir = 0;
			*pos = 255;
			return;
		}
		/* C: a call answered: what the lead just played, for its responder, and a bar's wait */
		if (cmode && ch == C_LEAD[m] && C_RESP[m] >= 0 && *part != 2 && !conv.pending &&
		    rnd() < (ph_p > 0.75f ? 0.25f : ph_p < 0.3f ? 0.3f : 0.85f)) {   /* (the conversation, most in the middle of the arc) */
			conv.n = (uint8_t)n;
			for (int k = 0; k < n && k < 6; k++) {
				conv.notes[k] = c->loop[2][k];
				conv.rh[k] = c->loop[3][6 + k];
				conv.ct[k] = (int8_t)(k ? (c->loop[2][k] > c->loop[2][k - 1] ? 1 : c->loop[2][k] < c->loop[2][k - 1] ? -1 : 0) : 0);
			}
			conv.pending = 1;
			c->hq_n = 1;
		}
		const float again = cmode ? (ph_p > 0.6f ? 1.0f : ph_p < 0.35f ? 0.35f : 0.75f) : 0.8f;
		const float answer = cmode ? (ph_p > 0.6f ? 1.0f : ph_p < 0.35f ? 0.3f : 0.7f) : 0.7f;
		if (*part == 1)
			*part = rnd() < again ? 2 : (rnd() < answer ? 3 : 0);
		else if (*part == 2)
			*part = rnd() < answer ? 3 : 0;
		else
			*part = 0;
		if (*part == 0) {
			(*times)++;
			c->arp_dir = (uint8_t)((1 + rndi(3)) * (1.5f - act) + (PH_MANNER[m].space > 60 ? rndi(2) : 0) +
			                       (PH_MANNER[m].hook ? 2 + rndi(3) : 0));   /* (a hook comes back after a breath) */
			if (cmode)                                  /* C: near the top a bar at most; at the bottom two to four */
				c->arp_dir = (uint8_t)(ph_p > 0.7f ? rndi(2) : ph_p < 0.35f ? 2 + rndi(3) : 1 + rndi(2));
		}
		*pos = 255;
	}
}

/* ================================================================ the Rhodes */
/* Warm, no tine: per voice a sine phase-modulated by itself (1:1, the bark, falling away over a
 * third of a second; TONE how much, harder notes more, less as it climbs) and a touch of its
 * octave; a quick fall to two thirds (menv2), then a long one (LENGTH, 2.5 to 9 s); the damper when
 * the chord moves. A suitcase tremolo across the two sides (CHARACTER). It comps: the chord without
 * its root (third, seventh, a colour; the fifth for a triad), each note led to the nearest of the
 * last voicing, rolled up from the bottom; now and then a softer touch on beat three.
 * Per voice: pc its phase, fk the bark's depth, hold the seconds to the damper (-1: now) ---- */
static void rhodes_strike(chan *c, int note, float vel)
{
	nvoice *v = alloc(c, 4);
	if (!sounding(v))
		make_room(v);
	nv_trigger(v, (float)note, vel, 1.0f, c->vnext++);
	v->hold = 30.0f;
	c->glow = vel;
}
static void rhodes_control(chan *c, int ch, float dt)
{
	const float chr = P.ch[ch][CP_CHAR], tone = eff_tone(ch), len = P.ch[ch][CP_LENGTH];
	const float sdt = dt * speed, T = expmap(len, 2.5f, 9.0f);
	c->lfo2 += (uint32_t)(sdt * 4.4f * 4294967296.0f);
	c->swell = 0.55f * chr;
	if (c->hq_i < c->hq_n) {                            /* the roll: the next note up */
		c->hq_t -= sdt;
		if (c->hq_t <= 0.0f) {
			rhodes_strike(c, c->hq[c->hq_i], c->hq_vel * (0.92f + 0.12f * rnd()));
			c->hq_i++;
			c->hq_t += c->hq_dt;
		}
	}
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->stage == ST_IDLE && v->amp <= 0.0f) {
			v->ainc = 0.0f;
			continue;
		}
		v->t += sdt;
		if (v->stage == ST_ATT) {
			v->env += sdt / 0.004f;
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
			v->env *= dcoef(0.3f, sdt);
			if (v->env < 0.0005f) {
				v->env = 0.0f;
				v->stage = ST_IDLE;
			}
		}
		v->menv *= dcoef(0.32f, sdt);
		v->menv2 *= dcoef(0.6f, sdt);
		v->ic = hz_inc(mtof(v->note) * speed * pbend);
		v->fk = (0.12f + 0.75f * tone * v->vel) * v->menv * kt_dark(v->note);
		const float a = v->env * v->vel * 0.75f * (0.62f + 0.38f * v->menv2) * clampf(1.0f - (v->note - 64.0f) * 0.02f, 0.6f, 1.0f);
		v->ainc = (a - v->amp) * ctl_inv;
		if (a <= 0.0f && v->amp < 0.0002f)
			v->amp = v->ainc = 0.0f;
	}
}
static int rhodes_render(chan *c, float *A, float *B)
{
	int any = 0;
	const float tr = c->swell * sin_u(c->lfo2), gl = 0.75f * (1.0f + tr), gr = 0.75f * (1.0f - tr);
	for (int i = 0; i < 4; i++) {
		nvoice *v = &c->v[i];
		if (v->amp <= 0.0f && v->ainc <= 0.0f)
			continue;
		any = 1;
		uint32_t pc = v->pc;
		const uint32_t ic = v->ic;
		float amp = v->amp;
		const float ai = v->ainc, I = v->fk * 683565275.6f;
		for (uint32_t j = 0; j < VSUB; j++) {
			const float x = sin_u(pc + (uint32_t)(int32_t)(I * sin_u(pc))) + 0.06f * sin_u(2u * pc);
			A[j] += x * amp * gl;
			B[j] += x * amp * gr;
			pc += ic;
			amp += ai;
		}
		v->pc = pc; v->amp = amp;
	}
	return any;
}
static void rhodes_voicing(chan *c, int base, float vel, int damp)
{
	int pcs[4], n = 0;
	pcs[n++] = chord_pc[1];
	if (chord_n > 3)
		pcs[n++] = chord_pc[3];
	pcs[n++] = chord_pc[2 % chord_n];
	/* a colour: the progression's own, else the ninth if the scale has it */
	int col = -1;
	for (int b = 0; b < 12 && col < 0; b++) {
		const int pc = (chord_pc[0] + b) % 12;
		if ((colour_mask >> pc) & 1u)
			col = pc;
	}
	if (col < 0 && prog_x == 0 && in_scale((chord_pc[0] + 2) % 12))
		col = (chord_pc[0] + 2) % 12;
	if (col >= 0 && n < 4)
		pcs[n++] = col;
	const int lo = base - 7, hi = base + 10;           /* D3 to G4 at home */
	const int center = c->glide_note > 0.0f ? (int)c->glide_note : base;
	int notes[4], sum = 0;
	for (int k = 0; k < n; k++) {
		notes[k] = place(pcs[k], (float)center, lo, hi > lo + 11 ? hi : lo + 11);
		sum += notes[k];
	}
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && notes[j] < notes[j - 1]; j--) {
			const int t = notes[j];
			notes[j] = notes[j - 1];
			notes[j - 1] = t;
		}
	c->glide_note = (float)sum / (float)n;             /* (the voicing's centre, for the next) */
	if (damp)
		for (int i = 0; i < 4; i++)
			if (sounding(&c->v[i]) && !c->v[i].mnote)
				c->v[i].hold = 0.05f * (float)i;          /* the damper, a breath behind the new chord */
	for (int k = 0; k < n; k++)
		c->hq[k] = (int8_t)notes[k];
	c->hq_n = (uint8_t)n;
	c->hq_i = 0;
	c->hq_t = 0.0f;
	c->hq_dt = 0.035f + 0.035f * rnd();
	c->hq_vel = vel;
}
static void rhodes_step(chan *c, int ch, uint32_t step, float act, int base)
{
	const int s16 = (int)(step & 15u);
	const int m = ph_piece < 8 ? ph_piece : 0;
	if (ph_style == 2 && m > 0 && ch == C_RESP[m] && conv.pending && s16 == 0 && c->hq_i >= c->hq_n) {
		/* C: the Rhodes answers the lead, single notes, the call's last ones an octave or two down,
		 * a beat apart, a little behind the bar */
		conv.pending = 0;
		const int n = conv.n < 3 ? conv.n : 3, from = conv.n - n;
		for (int k = 0; k < n; k++) {
			int x = conv.notes[from + k] - 12;
			while (x > base + 10) x -= 12;
			while (x < base - 7) x += 12;
			c->hq[k] = (int8_t)x;
		}
		c->hq_n = (uint8_t)n;
		c->hq_i = 0;
		c->hq_t = 0.08f;
		c->hq_dt = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		c->hq_vel = 0.42f;
		return;
	}
	if (s16 == 0 && (c->seen_chord != chord_count || rnd() < 0.15f * act)) {
		const int fresh = c->seen_chord != chord_count;
		c->seen_chord = chord_count;
		rhodes_voicing(c, base, 0.55f + 0.15f * rnd(), fresh);
	} else if (s16 == 8 && rnd() < 0.45f * act)
		rhodes_voicing(c, base, 0.35f + 0.1f * rnd(), 0);
	(void)ch;
}

/* ================================================================ the tide (the space pieces) */
/* Audition: a piece's own slow shape for its space. The hits never stop (every channel keeps at
 * least four fifths of its activity, so the echo's tape and the reverb always have something for
 * the grains); what moves is how loud each channel is dry, as song mode moves its levels: pulled
 * back, its echoes and the reverb's tail and the grains come forward. Sections, each so many bars:
 * every channel's dry level, activity and echo and reverb sends (percent of the piece's own), the
 * delay and reverb amounts (added), the echo's time walking a step longer or shorter every so many
 * bars as the Harmony page's FFWD and RWD do (the heads swooping to each new length), and something
 * at its start (a throw into the echo, the reverb or echo held, the tape repeating, stopping or
 * reversing for two beats). The first is a seed: two bars of hits thrown into the echo and reverb,
 * so the grains have something from the start. Each time round: the lengths by a bar or two, each
 * channel's share, the event now and then another, which channel is thrown. Channels by
 * convention: two struck voices, the Grains (the echo's tape), Haze (the reverb's tail) */
enum { TK_SEED, TK_HUSH, TK_STIR, TK_BUILD, TK_GAP, TK_PEAK, TK_FALL, TK_RING, TK_N };
enum { TE_NONE, TE_THROW, TE_HOLDV, TE_HOLDE, TE_REPEAT, TE_STOP, TE_REV };
const char *const GL_TIDE_KIND[TK_N] = {"seed", "hush", "stir", "build", "gap", "peak", "fall", "ring"};
typedef struct {
	uint8_t kind, bars, glide;
	uint8_t dry[4], act[4], echo[4], verb[4];
	int8_t dl, rv;
	int8_t walk;                           /* the echo's time: 0 still, 1 longer, -1 shorter, 2 back and forth */
	uint8_t every;                         /* (bars between its steps) */
	uint8_t ev;
} tide_sec;
#define E100 {100, 100, 100, 100}
#define SEED {TK_SEED, 2, 0, {100, 100, 30, 30}, {110, 110, 60, 60}, {160, 160, 100, 100}, {150, 150, 100, 100}, 10, 10, 0, 1, TE_THROW}
static const tide_sec TD_ARC[] = {        /* one long arc */
	SEED,
	{TK_HUSH, 8, 4, {35, 30, 95, 100}, {90, 85, 80, 100}, {130, 130, 100, 100}, {150, 150, 100, 100}, 5, 12, 1, 4, TE_NONE},
	{TK_STIR, 6, 4, {70, 60, 85, 90}, {100, 95, 95, 100}, {140, 140, 100, 100}, {120, 120, 100, 100}, 10, 5, 1, 2, TE_THROW},
	{TK_BUILD, 4, 3, {100, 90, 75, 80}, {115, 110, 110, 100}, {160, 150, 120, 100}, {95, 95, 100, 100}, 18, 0, 2, 1, TE_NONE},
	{TK_GAP, 1, 0, {12, 12, 100, 110}, {100, 100, 100, 100}, {160, 160, 100, 100}, {150, 150, 100, 120}, 18, 15, 0, 1, TE_HOLDV},
	{TK_PEAK, 4, 0, {110, 100, 90, 90}, {120, 115, 115, 110}, {170, 150, 130, 110}, E100, 25, 0, -1, 2, TE_THROW},
	{TK_FALL, 2, 0, {55, 55, 100, 110}, {90, 90, 100, 100}, {130, 130, 140, 120}, {150, 150, 120, 130}, 15, 20, 0, 1, TE_STOP},
	{TK_RING, 6, 2, {22, 18, 100, 110}, {85, 80, 80, 100}, {120, 120, 100, 100}, {170, 170, 120, 120}, 12, 25, -1, 2, TE_NONE}};
static const tide_sec TD_WAVES[] = {      /* two swells */
	SEED,
	{TK_HUSH, 6, 4, {35, 30, 90, 100}, {90, 85, 80, 100}, {130, 130, 100, 100}, {140, 140, 100, 100}, 5, 10, 1, 3, TE_NONE},
	{TK_STIR, 4, 3, {90, 80, 85, 90}, {105, 100, 95, 100}, {150, 140, 120, 100}, E100, 15, 0, 2, 1, TE_THROW},
	{TK_RING, 4, 2, {25, 20, 100, 110}, {85, 80, 85, 100}, {120, 120, 110, 100}, {160, 160, 110, 120}, 10, 20, -1, 2, TE_NONE},
	{TK_HUSH, 4, 3, {35, 30, 90, 100}, {90, 85, 80, 100}, {130, 130, 100, 100}, {140, 140, 100, 100}, 5, 10, 0, 1, TE_NONE},
	{TK_BUILD, 4, 3, {100, 90, 80, 85}, {115, 110, 110, 105}, {160, 150, 130, 100}, E100, 18, 0, 1, 1, TE_REPEAT},
	{TK_PEAK, 2, 0, {110, 100, 90, 90}, {120, 115, 115, 110}, {170, 150, 140, 110}, E100, 25, 0, 0, 1, TE_THROW},
	{TK_RING, 6, 1, {20, 15, 100, 110}, {85, 80, 80, 100}, {120, 120, 100, 100}, {170, 170, 120, 120}, 12, 25, -1, 2, TE_REV}};
static const tide_sec TD_STORM[] = {      /* a long build */
	SEED,
	{TK_HUSH, 8, 4, {30, 25, 90, 100}, {90, 85, 75, 100}, {130, 130, 100, 100}, {150, 150, 100, 100}, 5, 12, 0, 1, TE_NONE},
	{TK_STIR, 8, 6, {60, 50, 85, 95}, {100, 95, 90, 100}, {130, 125, 110, 100}, {120, 120, 100, 100}, 8, 5, 1, 4, TE_NONE},
	{TK_BUILD, 8, 6, {90, 80, 80, 90}, {110, 105, 105, 100}, {150, 130, 120, 100}, {100, 100, 100, 100}, 15, 0, -1, 2, TE_THROW},
	{TK_BUILD, 4, 3, {110, 100, 75, 85}, {120, 115, 120, 110}, {170, 150, 140, 100}, {90, 90, 100, 100}, 25, 0, 2, 1, TE_REPEAT},
	{TK_GAP, 1, 0, {10, 10, 100, 110}, {100, 100, 100, 100}, {170, 170, 100, 100}, {160, 160, 100, 120}, 25, 20, 0, 1, TE_HOLDV},
	{TK_PEAK, 6, 0, {115, 105, 90, 95}, {120, 120, 120, 115}, {180, 160, 150, 110}, E100, 28, 0, -1, 3, TE_THROW},
	{TK_FALL, 2, 0, {50, 50, 100, 110}, {90, 90, 100, 100}, E100, {160, 160, 120, 130}, 15, 22, 0, 1, TE_STOP},
	{TK_RING, 8, 2, {18, 15, 100, 110}, {85, 80, 80, 100}, {120, 120, 100, 100}, {180, 180, 120, 120}, 12, 28, 1, 3, TE_HOLDE}};
static const tide_sec TD_BREATH[] = {     /* swells out of the tails */
	SEED,
	{TK_HUSH, 4, 2, {30, 25, 95, 100}, {90, 85, 80, 100}, {130, 130, 100, 100}, {140, 140, 100, 100}, 5, 10, 0, 1, TE_NONE},
	{TK_PEAK, 2, 0, {110, 100, 85, 90}, {115, 110, 110, 105}, {160, 150, 130, 100}, E100, 20, 0, 1, 1, TE_THROW},
	{TK_RING, 4, 0, {20, 15, 100, 110}, {85, 80, 85, 100}, {120, 120, 100, 100}, {170, 170, 120, 120}, 10, 22, -1, 2, TE_HOLDV},
	{TK_HUSH, 4, 2, {35, 30, 95, 100}, {90, 85, 80, 100}, {130, 130, 100, 100}, {140, 140, 100, 100}, 5, 10, 0, 1, TE_NONE},
	{TK_STIR, 2, 1, {80, 70, 85, 95}, {105, 100, 95, 100}, {140, 130, 110, 100}, E100, 10, 0, 2, 1, TE_NONE},
	{TK_GAP, 1, 0, {12, 12, 100, 110}, {100, 100, 100, 100}, {160, 160, 100, 100}, {150, 150, 100, 120}, 15, 15, 0, 1, TE_HOLDV},
	{TK_PEAK, 2, 0, {110, 100, 90, 90}, {120, 115, 115, 110}, {170, 150, 140, 110}, E100, 25, 0, -1, 1, TE_REPEAT},
	{TK_RING, 6, 0, {20, 15, 100, 110}, {85, 80, 80, 100}, {120, 120, 100, 100}, {175, 175, 120, 120}, 12, 25, 1, 2, TE_STOP}};
static const struct { const tide_sec *s; uint8_t n; } TIDES[4] = {
	{TD_ARC, 8}, {TD_WAVES, 8}, {TD_STORM, 9}, {TD_BREATH, 9}};
/* the space pieces (145 on): their progression (0 the drift) and their tide */
static const uint8_t PIECE_SPACE[10][2] = {
	{0, 1}, {0, 2}, {1, 4}, {0, 3}, {0, 1}, {0, 2}, {0, 3}, {3, 4}, {0, 2}, {5, 1}};
static void space_derive(float bpm);
static uint8_t tide_on = 1, tide_s, tide_bars, tide_len, tide_held, tide_swap, tide_started;
static float tide_cur[4][4], tide_tgt[4][4], tide_cdl, tide_crv, tide_tdl, tide_trv, tide_k;
static int8_t tide_wdir = 1;
static float tide_f[4] = {1.0f, 1.0f, 1.0f, 1.0f}, tide_throw[4], tide_head_t, tide_dt_acc;
static uint8_t tide_echo_t = 255;
static float tide_holdv_t;
/* an accent: the section's first beat struck on a channel, in its own voice (a drum's kick or its
 * sweep; a struck voice a chord tone at its home) */
static nvoice *voice_note(chan *c, int ch, int note, float vel, float gate);
static void midi_drum(chan *c, int ch, int note, float vel);
static void tide_accent(int ch, float vel)
{
	chan *c = &CH[ch];
	if (P.ch[ch][CP_LEVEL] < 0.05f || c->fading)
		return;
	switch (c->voice) {
	case GV_GRAINS:
	case GV_HAZE:
		return;
	case GV_PULSE:
	case GV_KIT:
	case GV_LITEKIT:
		midi_drum(c, ch, 36, vel);
		if (c->voice == GV_LITEKIT)
			midi_drum(c, ch, 39, vel * 0.8f);      /* (and the clap) */
		return;
	case GV_BRUSH:
		midi_drum(c, ch, 41, vel);                 /* a sweep, and the felt kick */
		midi_drum(c, ch, 36, vel * 0.7f);
		return;
	default: {
		const int base = HOME[c->voice] + 12 * (stepped(P.ch[ch][CP_REGISTER], 5) - 2);
		const int k = rnd() < 0.6f ? 0 : 1 + rndi(chord_n - 1);
		voice_note(c, ch, place(chord_pc[k], (float)base, base - 5, base + 7), vel, 1.5f);
		return;
	}
	}
}
static void tide_release(void)
{
	if (tide_held & 1u)
		gl_perform(GF_HOLD_VERB, 0.0f);
	if (tide_held & 2u)
		gl_perform(GF_HOLD_ECHO, 0.0f);
	if (tide_held & 4u)
		gl_perform(GF_HEAD, 0.0f);
	tide_held = 0;
	tide_head_t = 0.0f;
}
static void tide_enter(int s, int now)
{
	const tide_sec *t = &TIDES[tide_x - 1].s[s];
	const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
	tide_s = (uint8_t)s;
	tide_bars = 0;
	tide_len = t->bars;
	if (t->bars >= 4)
		tide_len = (uint8_t)(t->bars - 1 + rndi(3));    /* a bar more or less */
	if (s <= 1) {                                     /* a new time round: new shares */
		for (int c = 0; c < 4; c++)
			tide_f[c] = 0.8f + 0.4f * rnd();
		tide_swap = rnd() < 0.3f;
	}
	for (int c = 0; c < 4; c++) {
		const int cc = tide_swap && c < 2 ? 1 - c : c;   /* (the first two trading the lead) */
		const float f = 0.85f + 0.15f * tide_f[c];   /* (each channel's share this time round) */
		tide_tgt[0][c] = (float)t->act[cc] * 0.01f * f;
		if (c < 2 && tide_tgt[0][c] < 0.8f)
			tide_tgt[0][c] = 0.8f;                       /* the hits never stop */
		tide_tgt[1][c] = (float)t->echo[c] * 0.01f;
		tide_tgt[2][c] = (float)t->verb[c] * 0.01f;
		tide_tgt[3][c] = (float)t->dry[cc] * 0.01f * (t->kind == TK_GAP ? 1.0f : tide_f[c]);
	}
	tide_tdl = (float)t->dl * 0.01f;
	tide_trv = (float)t->rv * 0.01f;
	const float gs = (float)t->glide * 4.0f * beat;
	/* a section that cuts in (a gap, a peak, a fall) still takes 40 ms: levels stepped at once
	 * clicked on whatever was ringing (Music Box Fog) */
	tide_k = gs > 0.01f ? gs : 0.04f;
	if (now) {
		for (int k = 0; k < 4; k++)
			for (int c = 0; c < 4; c++)
				tide_cur[k][c] = tide_tgt[k][c];
		tide_cdl = tide_tdl;
		tide_crv = tide_trv;
	}
	/* its event: now and then another than written */
	int ev = t->ev;
	if (!now && rnd() < 0.35f) {
		static const uint8_t ALT[TK_N][3] = {
			{TE_THROW, TE_THROW, TE_THROW},
			{TE_NONE, TE_NONE, TE_HOLDE}, {TE_THROW, TE_NONE, TE_REPEAT}, {TE_REPEAT, TE_THROW, TE_NONE},
			{TE_HOLDV, TE_HOLDV, TE_NONE}, {TE_THROW, TE_REPEAT, TE_NONE}, {TE_STOP, TE_REV, TE_HOLDE},
			{TE_NONE, TE_HOLDE, TE_REV}};
		ev = ALT[t->kind][rndi(3)];
	}
	tide_release();
	tide_holdv_t = 0.0f;
	/* the impact: a gap opens with a stab thrown into the echo, the reverb held a third of a beat
	 * later so the stab hangs in it; a peak lands on a struck downbeat; a fall on a last hit */
	if (!now && (t->kind == TK_GAP || t->kind == TK_PEAK || t->kind == TK_FALL)) {
		const float av = t->kind == TK_PEAK ? 0.95f : t->kind == TK_GAP ? 0.55f : 0.5f;
		tide_accent(0, av);
		tide_accent(1, av * 0.85f);
		if (t->kind == TK_GAP) {
			tide_throw[rndi(2)] = beat;                /* (one of them into the echo) */
			if (ev == TE_HOLDV) {
				tide_holdv_t = 0.35f * beat;
				ev = TE_NONE;
			}
		}
	}
	switch (now ? TE_NONE : ev) {
	case TE_THROW: {                                  /* one channel (not a grain) into the echo, a beat */
		int c = rndi(2);
		if (P.ch[c][CP_LEVEL] < 0.05f)
			c ^= 1;
		tide_throw[c] = beat;
		break;
	}
	case TE_HOLDV: gl_perform(GF_HOLD_VERB, 1.0f); tide_held |= 1u; break;
	case TE_HOLDE: gl_perform(GF_HOLD_ECHO, 1.0f); tide_held |= 2u; break;
	case TE_REPEAT: case TE_STOP: case TE_REV:
		gl_perform(GF_HEAD, ev == TE_REPEAT ? 1.0f : ev == TE_STOP ? 2.0f : 3.0f);
		tide_held |= 4u;
		tide_head_t = 2.0f * beat;
		break;
	default: break;
	}
}
static void tide_reset(void)
{
	tide_release();
	tide_started = 0;
	for (int c = 0; c < 4; c++) {
		tide_a[c] = 1.0f;
		tide_d[c] = 1.0f;
		arc_d[c] = 1.0f;
		tide_throw[c] = 0.0f;
	}
	tide_echo_t = 255;
	conv.pending = 0;
}
/* each bar */
static void tide_bar(void)
{
	if (!tide_x || !tide_on)
		return;
	if (!tide_started) {
		tide_started = 1;
		tide_enter(0, 1);
		return;
	}
	if (++tide_bars >= tide_len) {
		const int nx = (tide_s + 1) % TIDES[tide_x - 1].n;
		tide_enter(nx ? nx : 1, 0);                      /* (the seed only opens the piece) */
		return;
	}
	/* the echo's time walking, as FFWD and RWD on the Harmony page: a step through the lengths
	 * (an eighth to a bar), the heads swooping to each */
	const tide_sec *t = &TIDES[tide_x - 1].s[tide_s];
	if (t->walk && t->every && tide_bars % t->every == 0) {
		const uint8_t cur = tide_echo_t == 255 ? Pb.space[GL_SP_ECHO] : tide_echo_t;
		int pos = 4;
		for (int k = 1; k < GL_NECHO; k++)
			if (GL_ECHO_ORDER[k] == cur)
				pos = k;
		int d = t->walk == 2 ? tide_wdir : t->walk;
		if (pos + d < 1 || pos + d > GL_NECHO - 2) {
			d = -d;                                    /* (at the end of the lengths: back the other way) */
			tide_wdir = (int8_t)-tide_wdir;
		}
		if (t->walk == 2)
			tide_wdir = (int8_t)-tide_wdir;
		tide_echo_t = GL_ECHO_ORDER[pos + d];
	}
}
/* each control tick: glide, and lay it onto the playing patch (the posted one, Pb, is the centre) */
static void tide_control(float dt)
{
	if (!tide_x || !tide_on || !tide_started) {
		if (tide_a[0] != 1.0f || tide_a[1] != 1.0f || tide_a[2] != 1.0f || tide_a[3] != 1.0f ||
		    tide_d[0] != 1.0f || tide_d[1] != 1.0f || tide_d[2] != 1.0f || tide_d[3] != 1.0f) {
			tide_reset();                          /* (off: everything back as posted) */
			for (int c = 0; c < GL_NCH; c++) {
				P.ch[c][CP_ECHO] = Pb.ch[c][CP_ECHO];
				P.ch[c][CP_VERB] = Pb.ch[c][CP_VERB];
			}
			P.g[GP_DELAY] = Pb.g[GP_DELAY];
			P.g[GP_REVERB] = Pb.g[GP_REVERB];
			P.space[GL_SP_ECHO] = Pb.space[GL_SP_ECHO];
			space_derive(gl_tempo_bpm(P.g[GP_TEMPO]));
		}
		return;
	}
	const float k = tide_k > 0.0f ? 1.0f - fexp2(-1.4427f * 3.0f * dt / tide_k) : 1.0f;
	for (int j = 0; j < 4; j++)
		for (int c = 0; c < 4; c++)
			tide_cur[j][c] += (tide_tgt[j][c] - tide_cur[j][c]) * k;
	tide_cdl += (tide_tdl - tide_cdl) * k;
	tide_crv += (tide_trv - tide_crv) * k;
	for (int c = 0; c < GL_NCH; c++) {
		tide_a[c] = tide_cur[0][c] > 0.0005f ? tide_cur[0][c] : 0.0005f;
		tide_d[c] = tide_cur[3][c];
		float th = 0.0f;
		if (tide_throw[c] > 0.0f) {
			tide_throw[c] -= dt;
			th = 0.65f;
		}
		P.ch[c][CP_ECHO] = clampf(Pb.ch[c][CP_ECHO] * tide_cur[1][c] + th, 0.0f, 1.0f);
		P.ch[c][CP_VERB] = clampf(Pb.ch[c][CP_VERB] * tide_cur[2][c], 0.0f, 1.0f);
	}
	if (tide_holdv_t > 0.0f) {
		tide_holdv_t -= dt;
		if (tide_holdv_t <= 0.0f) {
			gl_perform(GF_HOLD_VERB, 1.0f);
			tide_held |= 1u;
		}
	}
	if (tide_head_t > 0.0f) {
		tide_head_t -= dt;
		if (tide_head_t <= 0.0f && (tide_held & 4u)) {
			gl_perform(GF_HEAD, 0.0f);
			tide_held &= (uint8_t)~4u;
		}
	}
	/* the delay and reverb amounts, the echo's time: re-derived every 30 ms as they move */
	tide_dt_acc += dt;
	if (tide_dt_acc >= 0.03f) {
		tide_dt_acc = 0.0f;
		const float dl = clampf(Pb.g[GP_DELAY] + tide_cdl, 0.0f, 0.95f), rv = clampf(Pb.g[GP_REVERB] + tide_crv, 0.0f, 0.97f);
		const uint8_t et = tide_echo_t == 255 ? Pb.space[GL_SP_ECHO] : tide_echo_t;
		const float ddl = dl - P.g[GP_DELAY], drv = rv - P.g[GP_REVERB];
		if (ddl > 0.004f || ddl < -0.004f || drv > 0.004f || drv < -0.004f || et != P.space[GL_SP_ECHO]) {
			P.g[GP_DELAY] = dl;
			P.g[GP_REVERB] = rv;
			P.space[GL_SP_ECHO] = et;
			space_derive(gl_tempo_bpm(P.g[GP_TEMPO]));
		}
	}
}
void gl_tide(int what, float v)
{
	switch (what) {
	case 0: tide_on = v > 0.5f; if (!tide_on) tide_release(); break;   /* the tide on (1) or held where it is (0) */
	case 1: {                                         /* a throw, now: channel v (-1 the first two) */
		const float beat = 60.0f / gl_tempo_bpm(P.g[GP_TEMPO]);
		const int c = (int)v;
		if (c >= 0 && c < GL_NCH)
			tide_throw[c] = beat;
		else
			tide_throw[0] = tide_throw[1] = beat;
		break;
	}
	case 2:                                           /* on to the next section */
		if (tide_x && tide_started)
			tide_enter((tide_s + 1) % TIDES[tide_x - 1].n, 0);
		break;
	default: break;
	}
}
int gl_tide_info(int *sec, int *nsec, int *kind, int *bar, int *len)
{
	if (!tide_x) {
		*sec = *nsec = *kind = *bar = *len = 0;
		return 0;
	}
	*sec = tide_s;
	*nsec = TIDES[tide_x - 1].n;
	*kind = TIDES[tide_x - 1].s[tide_s].kind;
	*bar = tide_bars;
	*len = tide_len;
	return tide_x;
}
int gl_tide_kind_at(int s) { return tide_x ? TIDES[tide_x - 1].s[s % TIDES[tide_x - 1].n].kind : 0; }
int gl_tide_bars_at(int s) { return tide_x ? TIDES[tide_x - 1].s[s % TIDES[tide_x - 1].n].bars : 0; }
/* what the space is doing now, for the page: 0 the delay amount, 1 the reverb's, 2 the echo's time,
 * 3..6 each channel's echo send, 7..10 its reverb send, 11 the holds (1 reverb, 2 echo, 4.. the
 * tape head), 12..15 each channel's activity under the tide */
float gl_space_now(int k)
{
	if (k == 0) return P.g[GP_DELAY];
	if (k == 1) return P.g[GP_REVERB];
	if (k == 2) return (float)P.space[GL_SP_ECHO];
	if (k >= 3 && k < 7) return P.ch[k - 3][CP_ECHO];
	if (k >= 7 && k < 11) return P.ch[k - 7][CP_VERB];
	if (k == 11) return (float)(pf.hold_verb | (pf.hold_echo << 1) | (pf.head << 2));
	if (k >= 12 && k < 16) return tide_a[k - 12];
	return 0.0f;
}
