/* One shuffle track on its own: shuf PIECE SEED OUT.wav [SECS] [FRESH]
 * (PIECE, SEED and FRESH as the emulator's track report gives them: the track replayed exactly)
 * The UI's own shuffle (its dice seeded with SEED) dealt for PIECE, played through glui as the
 * emulator does; prints the settings it dealt. For hunting what one shuffled track did. */
#include <stdio.h>
#include <stdlib.h>
#include "../glag/glag_ui.c"

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
	if (argc < 4) { fprintf(stderr, "shuf PIECE SEED OUT.wav [SECS] [FRESH]\n"); return 2; }
	const int piece = atoi(argv[1]);
	const float secs = argc > 4 ? (float)atof(argv[4]) : 120.0f;
	gl_init(1234);
	glui_init();
	gl_input in = {0};
	gl_ui_out out;
	for (int i = 0; i < 4; i++) in.fader[i] = 0.5f;
	glui_tick(0, &in, &out);
	seed = (uint32_t)strtoul(argv[2], 0, 0);
	demo_on = getenv("ALBUM") ? 1 : 2;
	gl_scope_on = 1;
	demo_n = 1;
	demo_idx = 0;
	demo_list[0] = (uint8_t)piece;
	demo_t0 = 2;
	demo_track(2);
	if (argc > 5)
		gl_fresh((uint32_t)strtoul(argv[5], 0, 0));
	if (getenv("NODIST")) { dist_amt = 0; gl_dist(dist_type, 0, dist_post); }
	if (getenv("POST")) { dist_post = atoi(getenv("POST")); gl_dist(dist_type, dist_amt, dist_post); }
	if (getenv("SOLO")) { int k = atoi(getenv("SOLO")); for (int c = 0; c < 4; c++) if (c != k) cur.ch[c][CP_LEVEL] = 0.0f; gl_post_patch(&cur); }
	if (getenv("DRYONLY")) { cur.g[GP_REVERB] = 0.0f; cur.g[GP_DELAY] = 0.0f; for (int c = 0; c < 4; c++) cur.ch[c][CP_VERB] = cur.ch[c][CP_ECHO] = 0.0f; gl_post_patch(&cur); }
	if (getenv("MUTE")) { for (int c = 0; c < 4; c++) cur.ch[c][CP_LEVEL] = 0.0f; gl_post_patch(&cur); }
	if (getenv("NOWARBLE"))
		gl_world_ch(GWC_TEXP, 1, 0.0f);
	if (getenv("NOTEX")) { w_tex = 0; gl_world(GW_TEXTURE, 0.0f); }
	if (getenv("NOPUMP")) { w_pump = 0; gl_world(GW_PUMP, 0.0f); }
	if (getenv("FLAGS")) { cur.space[GL_SP_FLAGS] = (uint8_t)atoi(getenv("FLAGS")); gl_post_patch(&cur); }
	if (getenv("NOECHO")) { cur.g[GP_DELAY] = 0.0f; gl_post_patch(&cur); }
	if (w_tex) fprintf(stderr, "texture %d: level %.2f, its controls %.2f %.2f %.2f\n", w_tex, wv_tex[w_tex][0], wv_tex[w_tex][1], wv_tex[w_tex][2], wv_tex[w_tex][3]);
	fprintf(stderr, "dist type %d amt %d post %d | reverb %d shim %d flags %d echo %d delay %.2f reverb %.2f | pump %d arp %d tex %d | macros %d %d %d %d\n",
		dist_type, dist_amt, dist_post, cur.space[GL_SP_REVERB], cur.space[GL_SP_SHIMMER], cur.space[GL_SP_FLAGS],
		cur.space[GL_SP_ECHO], cur.g[GP_DELAY], cur.g[GP_REVERB], w_pump, w_arp, w_tex,
		mac_step[0], mac_step[1], mac_step[2], mac_step[3]);
	const uint32_t frames = (uint32_t)(secs * 48000.0f);
	FILE *f = fopen(argv[3], "wb");
	wav_header(f, frames);
	int16_t blk[192];
	for (uint32_t n = 0; n < frames; n += 96) {
		glui_tick(2 + n / 48, &in, &out);
		gl_render(blk, 96, out.volume);
		if (getenv("COMPLOG")) { static FILE *cf; if (!cf) cf = fopen(getenv("COMPLOG"), "w"); fprintf(cf, "%.6f\n", gl_scope.comp); }
		fwrite(blk, 2, 192, f);
	}
	fclose(f);
	fprintf(stderr, "at %.1f s: %u notes, chord %d, T1 level %.3f\n", gl_scope.t, (unsigned)gl_scope.notes, gl_scope.chord, gl_scope.ch[0].lvl);
	return 0;
}
