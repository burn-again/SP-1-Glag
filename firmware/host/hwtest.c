/* The hardware test's host twin: hwtest [ROUNDS] [OUT.wav] [SEED]
 * The test firmware's script (glag/glag_test.h) played here through the same controls and the
 * same engine, ticking as the device does (every 8 ms, four 2 ms blocks a tick), printing the same
 * "GLAG T3" lines, so a device log can be laid beside it: what the twin shows too is the music;
 * what only the device shows (load, stream, stacks) is the device. There is no cycle counter
 * here, so the load fields read 0. OUT.wav: everything it played (48 kHz stereo, ~180 MB a round).
 * ONLY=name: that scenario alone. SEED: the engine's (the device takes one from the clock at power-on, so its notes differ). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glag_engine.h"
#include "glag_ui.h"
#define GLT_PRINTF printf
#include "glag_test.h"

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
	const uint32_t rounds = argc > 1 ? (uint32_t)atoi(argv[1]) : 1u;
	FILE *w = argc > 2 ? fopen(argv[2], "wb") : 0;
	gl_init(argc > 3 ? (uint32_t)strtoul(argv[3], 0, 0) : 1234u);
	if (getenv("ONLY"))         /* ONLY=name: that scenario alone, ROUNDS times */
		for (int i = 0; i < GLT_N; i++)
			if (!strcmp(GLT_SCENES[i].name, getenv("ONLY")))
				glt_only = i;
	glui_init();
	gl_input rest = {{0.5f, 0.5f, 0.5f, 0.5f}, 0};   /* the faders where a hand left them */
	gl_ui_out out;
	if (w)
		wav_header(w, 0);
	uint32_t now = 0, frames = 0;
	int16_t blk[192];
	while (glt_round < rounds) {
		gl_input in = rest;
		glt_tick(now, &in, 0);
		glui_tick(now, &in, &out);
		for (int b = 0; b < 4; b++) {
			gl_render(blk, 96, 1.0f);
			if (w)
				fwrite(blk, 2, 192, w);
			frames += 96;
		}
		now += 8;
	}
	if (w) {
		fseek(w, 0, SEEK_SET);
		wav_header(w, frames);
		fclose(w);
	}
	return 0;
}
