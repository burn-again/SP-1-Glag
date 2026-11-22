/* Single hits into the space: hits OUT.wav REVERB [CUT]
 * Eight struck notes, ten seconds apart (harp, harpsichord, bells, piano, then kalimba, marimba,
 * glow and the Kit's snare), every channel's own music at rest, everything into the reverb, the
 * reverb at its longest and no echo, so each reverb's tail can be heard on its own. */
#include <stdio.h>
#include <stdlib.h>
#include "glag_engine.h"

static void wav_header(FILE *f, uint32_t frames)
{
	uint32_t ds = frames * 4u, u;
	uint16_t h;
	fwrite("RIFF", 1, 4, f); u = 36 + ds; fwrite(&u, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	u = 16; fwrite(&u, 4, 1, f); h = 1; fwrite(&h, 2, 1, f); h = 2; fwrite(&h, 2, 1, f);
	u = 48000; fwrite(&u, 4, 1, f); u = 192000; fwrite(&u, 4, 1, f); h = 4; fwrite(&h, 2, 1, f);
	h = 16; fwrite(&h, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "hits OUT.wav REVERB [CUT] [SILENT]\n"); return 2; }
	const int rv = atoi(argv[2]), cut = argc > 3 ? atoi(argv[3]) : 0, silent = argc > 4;
	gl_init(1234);
	gl_patch p;
	gl_piece(0, &p);
	static const uint8_t V1[4] = {GV_HARP, GV_HARPSI, GV_BELLS, GV_PIANO}, V2[4] = {GV_KALIMBA, GV_MARIMBA, GV_GLOW, GV_KIT};
	for (int c = 0; c < GL_NCH; c++) {
		p.voice[c] = V1[c];
		p.ch[c][CP_ACTIVITY] = 0.0f;
		p.ch[c][CP_LEVEL] = 0.8f;
		p.ch[c][CP_VERB] = 1.0f;
		p.ch[c][CP_ECHO] = 0.0f;
		p.ch[c][CP_PAN] = 0.5f;
		gl_macro(c, 0.0f, 8.0f);
	}
	p.g[GP_REVERB] = 1.0f;
	p.g[GP_DELAY] = 0.0f;
	p.g[GP_MOTION] = 0.0f;
	p.g[GP_ROOT] = 0.0f;
	p.space[GL_SP_REVERB] = (uint8_t)rv;
	gl_post_patch(&p);
	gl_rv_cut(cut);
	if (getenv("RVDRIFT"))
		gl_rv_drift(atoi(getenv("RVDRIFT")));
	if (getenv("SHRATIO"))
		gl_rv_shim_ratio((float)atof(getenv("SHRATIO")));
	gl_play(0);                               /* (resting: nothing of its own, only the hits) */
	/* hit k at 2 + 10k s on channel k % 4, held down 0.4 s (so each channel's own sequencer rests) */
	static const int NOTE[8] = {67, 60, 72, 60, 67, 60, 60, 62};   /* (the Kit: D, its snare) */
	const float secs = 82.0f;
	const uint32_t frames = (uint32_t)(secs * 48000.0f);
	FILE *f = fopen(argv[1], "wb");
	wav_header(f, frames);
	int16_t blk[192];
	int held[4] = {-1, -1, -1, -1};
	for (uint32_t n = 0; n < frames; n += 96) {
		const float t = (float)n / 48000.0f;
		if (t >= 39.0f && p.voice[0] != V2[0]) {
			for (int c = 0; c < GL_NCH; c++)
				p.voice[c] = V2[c];
			gl_post_patch(&p);
		}
		for (int k = 0; k < 8 && !silent; k++) {
			const float at = 2.0f + 10.0f * (float)k;
			const int c = k % 4;
			if (held[c] != k && t >= at && t < at + 0.4f) {
				gl_midi_note(c, NOTE[k], 110);
				held[c] = k;
			}
			if (held[c] == k && t >= at + 0.4f) {
				gl_midi_note(c, NOTE[k], 0);
				held[c] = -1 - k;
			}
		}
		gl_render(blk, 96, 0.8f);
		fwrite(blk, 2, 192, f);
	}
	fclose(f);
	return 0;
}
