/* Render a piece to a WAV on a computer: render PIECE SECONDS OUT.wav [voices in hex, e.g. 0127 or 9abe] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	if (argc < 4) { fprintf(stderr, "render PIECE SECONDS OUT.wav [VOICES]\n"); return 2; }
	int piece = atoi(argv[1]);
	float secs = (float)atof(argv[2]);
	gl_init(1234);
	gl_patch p;
	gl_piece(piece, &p);
	if (argc > 4)
		for (int c = 0; c < GL_NCH && argv[4][c]; c++)
			p.voice[c] = (uint8_t)(argv[4][c] <= '9' ? argv[4][c] - '0' : (argv[4][c] | 0x20) - 'a' + 10);   /* hex: 0-9, a-f */
	if (argc > 5)   /* channel mask, e.g. 0100 keeps channel 2 only */
		for (int c = 0; c < GL_NCH && argv[5][c]; c++)
			if (argv[5][c] == '0')
				p.ch[c][CP_LEVEL] = 0.0f;
	if (getenv("DRY")) { p.g[GP_REVERB] = 0; p.g[GP_DELAY] = 0; for (int c = 0; c < GL_NCH; c++) p.ch[c][CP_VERB] = p.ch[c][CP_ECHO] = 0; }
	/* channel 1's activity, tone and character, for auditioning a voice */
	if (getenv("ACT"))
		p.ch[0][CP_ACTIVITY] = (float)atof(getenv("ACT"));
	if (getenv("TONE"))
		p.ch[0][CP_TONE] = (float)atof(getenv("TONE"));
	if (getenv("REG"))      /* channel 1's register step, -2..+2 */
		p.ch[0][CP_REGISTER] = ((float)atof(getenv("REG")) + 2.0f) * 0.25f;
	if (getenv("CHAR"))
		p.ch[0][CP_CHAR] = (float)atof(getenv("CHAR"));
	{	/* the piece's channel macros; MACRO=4444 (steps per channel) and MPER=seconds override */
		uint8_t st[GL_NCH];
		float per;
		gl_piece_macro(piece, st, &per);
		if (getenv("MACRO"))
			for (int c = 0; c < GL_NCH && getenv("MACRO")[c]; c++)
				st[c] = (uint8_t)(getenv("MACRO")[c] - '0');
		if (getenv("MPER"))
			per = (float)atof(getenv("MPER"));
		for (int c = 0; c < GL_NCH; c++)
			gl_macro(c, (float)st[c] / 4.0f, per);
	}
	gl_post_patch(&p);
	if (getenv("REVERB")) {   /* REVERB=type[,shimmer]: 0 Hall, 1 Chamber, 2 Shimmer, 3 Bloom */
		int t = 0, sh = 0;
		sscanf(getenv("REVERB"), "%d,%d", &t, &sh);
		p.space[GL_SP_REVERB] = (uint8_t)t;
		p.space[GL_SP_SHIMMER] = (uint8_t)sh;
	}
	if (getenv("NOVERB"))      /* the same render with nothing sent to the reverb */
		for (int c = 0; c < GL_NCH; c++) p.ch[c][CP_VERB] = 0;
	gl_post_patch(&p);
	if (getenv("DIST")) {   /* DIST=type,amount,post: e.g. 2,3,0 = fuzz, amount 3, before the effects */
		int t = 0, a = 0, po = 0;
		sscanf(getenv("DIST"), "%d,%d,%d", &t, &a, &po);
		gl_dist(t, a, po);
	}
	uint32_t frames = (uint32_t)(secs * 48000.0f) / 96u * 96u;
	FILE *f = fopen(argv[3], "wb");
	wav_header(f, frames);
	int16_t blk[192];
	double sum2 = 0; int clip = 0, peak = 0;
	int sweep_t = -1, sweep_p = 0;
	if (getenv("DISTSWEEP"))   /* DISTSWEEP=type,post: amounts 1, 2, 3, 4 across the render */
		sscanf(getenv("DISTSWEEP"), "%d,%d", &sweep_t, &sweep_p);
	float fz0 = -1.0f, fz1 = -1.0f;
	if (getenv("FREEZE"))      /* FREEZE=from,to in seconds */
		sscanf(getenv("FREEZE"), "%f,%f", &fz0, &fz1);
	for (uint32_t n = 0; n < frames; n += 96) {
		if (fz0 >= 0.0f)
			gl_freeze((float)n / 48000.0f >= fz0 && (float)n / 48000.0f < fz1);
		if (sweep_t >= 0)
			gl_dist(sweep_t, 1 + (int)((uint64_t)n * 4u / frames), sweep_p);
		gl_render(blk, 96, 0.8f);
		for (int i = 0; i < 192; i++) {
			int a = abs(blk[i]);
			if (a > peak) peak = a;
			if (a >= 31990) clip++;
			sum2 += (double)blk[i] * blk[i];
		}
		fwrite(blk, 2, 192, f);
	}
	fclose(f);
	printf("%s: piece %d (%s) rms %.1f dBFS peak %.1f dBFS clipped %d\n", argv[3], piece, gl_piece_name(piece),
	       10 * __builtin_log10(sum2 / (frames * 2.0) / (32768.0 * 32768.0) + 1e-20), 20 * __builtin_log10(peak / 32768.0 + 1e-9), clip);
	return 0;
}
