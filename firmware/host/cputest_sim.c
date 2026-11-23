/* The device CPU test's round on a computer: cputest_sim OUT.wav
 * Garden, then the four heaviest pieces (GL_TEST_PIECES), each through the five settings 30 s
 * apiece, switched as app/src/main.c does; reports each window's level and peak, so a switch that
 * misbehaves (a burst, a dropout) shows. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "glag_engine.h"
#include "glag_ui.h"

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
	static const uint8_t PIECE[5] = {0u, 16u, 17u, 18u, 19u};
	gl_init(1234);
	glui_init();
	gl_input in = {0};
	gl_ui_out out;
	for (int i = 0; i < 4; i++) in.fader[i] = 0.5f;
	FILE *f = argc > 1 ? fopen(argv[1], "wb") : NULL;
	const uint32_t win = 30u * 48000u, frames = 25u * win;
	if (f) wav_header(f, frames);
	int16_t blk[192];
	int rv = -1;
	for (int step = 0; step < 25; step++) {
		const int cfg = step % 5;
		if (cfg == 0)
			glui_midi_program(PIECE[step / 5]);
		gl_cpu_opts(cfg >= 1, cfg >= 2 ? 2 : 1);
		gl_kit_cheap(cfg >= 3 ? 3 : 0);
		if ((cfg >= 4 ? 13 : 0) != rv) { rv = cfg >= 4 ? 13 : 0; gl_rv_cut(rv); }
		double s2 = 0.0; int pk = 0, clip = 0;
		for (uint32_t n = 0; n < win; n += 96) {
			glui_tick((uint32_t)((step * win + n) / 48u), &in, &out);
			gl_render(blk, 96, out.volume);
			for (int i = 0; i < 192; i++) {
				int a = blk[i] < 0 ? -blk[i] : blk[i];
				pk = a > pk ? a : pk;
				clip += a >= 32000;
				s2 += (double)blk[i] * blk[i];
			}
			if (f) fwrite(blk, 2, 192, f);
		}
		printf("%-17s cfg %d: rms %6.1f dBFS peak %6.1f dBFS clipped %d\n", gl_piece_name(PIECE[step / 5]), cfg,
		       10.0 * log10(s2 / (win * 2.0) / (32768.0 * 32768.0) + 1e-20), 20.0 * log10(pk / 32768.0 + 1e-9), clip);
	}
	if (f) fclose(f);
	return 0;
}
