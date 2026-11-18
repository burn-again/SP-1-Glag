/* Glag control tests: every gesture in glag_ui.h, driven through a fake clock.
 * Build and run: see host/run_tests.sh */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "glag_engine.h"
#include "glag_ui.h"

static gl_input in;
static gl_ui_out out;
static uint32_t now;
static int fails, checks;
static int16_t audio[240 * 2];

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* advance time in 5 ms ticks, rendering the same 5 ms of audio */
static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		glui_tick(now, &in, &out);
		gl_render(audio, 240, out.volume);
		now += 5;
	}
}
static void down(int b) { in.buttons |= (uint16_t)(1u << b); run(10); }
static void up(int b) { in.buttons &= (uint16_t)~(1u << b); run(10); }
static void tap(int b) { down(b); run(40); up(b); }
static void fader(int f, float v) { in.fader[f] = v; run(20); }
static void sweep(int f, float to)
{
	float from = in.fader[f];
	for (int k = 1; k <= 20; k++)
		fader(f, from + (to - from) * (float)k / 20.0f);
}

int main(void)
{
	gl_init(42);
	glui_init();
	for (int f = 0; f < 4; f++)
		in.fader[f] = 0.0f;
	run(100);

	/* boot */
	const gl_patch *p = glui_patch();
	CHECK(out.page == PG_HOME, "boots on HOME (page %d)", out.page);
	CHECK(out.piece == 0, "boots on piece 1");
	CHECK(p->voice[0] == GV_PAD && p->voice[1] == GV_BELLS && p->voice[2] == GV_BASS && p->voice[3] == GV_FLUTE,
	      "default voices Pad Bells Bass Flute");

	/* soft takeover on HOME: F1 is tempo (0.47 in the piece); the fader sits at 0 */
	float tempo = p->g[GP_TEMPO];
	sweep(0, 0.3f);
	CHECK(fabsf(glui_patch()->g[GP_TEMPO] - tempo) < 1e-6f, "F1 below the tempo does nothing yet");
	sweep(0, 0.6f);
	CHECK(fabsf(glui_patch()->g[GP_TEMPO] - 0.6f) < 0.02f, "F1 caught the tempo and took over (%.3f)", glui_patch()->g[GP_TEMPO]);

	/* DJ filter centre detent */
	sweep(3, 0.5f);           /* catches at its stored 0.5 */
	sweep(3, 0.52f);
	CHECK(glui_patch()->g[GP_FILTER] == 0.5f, "filter snaps to centre near 0.5 (%.3f)", glui_patch()->g[GP_FILTER]);
	sweep(3, 0.2f);
	CHECK(glui_patch()->g[GP_FILTER] < 0.25f, "filter goes low-pass");
	sweep(3, 0.5f);

	/* the mixer: •• held + F1..F4 = levels */
	down(GB_FN);
	float lvl = glui_patch()->ch[1][CP_LEVEL];
	sweep(1, lvl);
	sweep(1, 0.2f);
	CHECK(fabsf(glui_patch()->ch[1][CP_LEVEL] - 0.2f) < 0.02f, "•• + F2 sets channel 2's level (%.3f)", glui_patch()->ch[1][CP_LEVEL]);
	up(GB_FN);
	run(500);

	/* channel pages */
	tap(GB_T2);
	CHECK(out.page == PG_CH && out.channel == 1, "T2 opens channel 2");
	CHECK(out.led_t[1] == 255 && out.led_t[0] == 0, "T2's LED shows the page");
	down(GB_FN); tap(GB_T2); up(GB_FN);
	CHECK(out.page == PG_EXT && out.channel == 1, "•• + T2 on its page: extended");
	down(GB_FN); tap(GB_T2); up(GB_FN);
	CHECK(out.page == PG_CH, "•• + T2 again: back");
	tap(GB_T3);
	CHECK(out.page == PG_CH && out.channel == 2, "T3 jumps to channel 3");
	run(500);

	/* register is stepped: 5 octaves */
	float reg = glui_patch()->ch[2][CP_REGISTER];
	sweep(3, reg);
	sweep(3, 0.9f);
	CHECK(glui_patch()->ch[2][CP_REGISTER] == 1.0f, "register snaps to its top step (%.3f)", glui_patch()->ch[2][CP_REGISTER]);

	/* double tap •• : HOME */
	tap(GB_FN); run(60); tap(GB_FN);
	CHECK(out.page == PG_HOME, "•• double tap returns HOME");
	run(500);

	/* •• + T4 on HOME rotates channel 4's voice */
	down(GB_FN); tap(GB_T4); up(GB_FN);
	CHECK(glui_patch()->voice[3] == GV_ARP, "•• + T4: Flute -> Arp (%d)", glui_patch()->voice[3]);
	down(GB_FN); tap(GB_T4); up(GB_FN);
	CHECK(glui_patch()->voice[3] == GV_SWELL, "•• + T4 again: Swell (%d)", glui_patch()->voice[3]);
	CHECK(out.page == PG_HOME, "rotating stays on HOME");
	for (int k = 0; k < GV_COUNT - 2; k++) {
		down(GB_FN); tap(GB_T4); up(GB_FN);
	}
	CHECK(glui_patch()->voice[3] == GV_FLUTE, "all %d voices, back to Flute (%d)", GV_COUNT, glui_patch()->voice[3]);
	run(500);

	/* •• + PLAY once: HARMONY */
	down(GB_FN); tap(GB_PLAY); up(GB_FN);
	run(500);
	CHECK(out.page == PG_HARMONY, "•• + PLAY once opens HARMONY (page %d)", out.page);
	float root = glui_patch()->g[GP_ROOT];
	sweep(0, root);
	sweep(0, 1.0f);
	CHECK(glui_patch()->g[GP_ROOT] == 1.0f, "root steps to B");
	tap(GB_FN); run(60); tap(GB_FN);
	run(500);

	/* •• + PLAY twice: next piece; •• + RWD: undo back */
	gl_patch before = *glui_patch();
	down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN);
	run(500);
	CHECK(out.piece == 1, "•• + PLAY twice: piece 2 (%d)", out.piece);
	CHECK(out.page == PG_HOME, "...and no HARMONY page");
	down(GB_FN); tap(GB_RWD); up(GB_FN);
	CHECK(out.piece == 0 && memcmp(glui_patch(), &before, sizeof before) == 0, "•• + RWD undoes the piece change");
	run(500);

	/* •• + FFWD: randomize; •• + RWD: undo */
	before = *glui_patch();
	down(GB_FN); tap(GB_FFWD); up(GB_FN);
	CHECK(memcmp(glui_patch(), &before, sizeof before) != 0, "•• + FFWD randomizes");
	CHECK(glui_patch()->g[GP_FILTER] == 0.5f, "randomize keeps the filter open");
	for (int c = 0; c < 4; c++)
		CHECK(glui_patch()->voice[c] == before.voice[c], "randomize keeps the voices");
	CHECK(!gl_frozen, "•• + FFWD does not freeze");
	down(GB_FN); tap(GB_RWD); up(GB_FN);
	CHECK(memcmp(glui_patch(), &before, sizeof before) == 0, "•• + RWD undoes the randomize");
	run(500);

	/* FFWD: tap latches, tap again releases; hold is momentary */
	tap(GB_FFWD); run(50);
	CHECK(gl_frozen, "FFWD tap freezes");
	run(1000);
	CHECK(gl_frozen, "...and stays frozen");
	tap(GB_FFWD); run(50);
	CHECK(!gl_frozen, "FFWD tap again lets go");
	down(GB_FFWD); run(800);
	CHECK(gl_frozen, "FFWD held freezes");
	up(GB_FFWD); run(50);
	CHECK(!gl_frozen, "...until released");

	/* RWD held: slows; release springs back */
	down(GB_RWD); run(5000);
	CHECK(gl_speed < 0.75f, "RWD held 5 s slows down (speed %.2f)", gl_speed);
	run(6000);
	float bpm = gl_tempo_bpm(glui_patch()->g[GP_TEMPO]);
	CHECK(fabsf(gl_speed - 10.0f / bpm) < 0.01f, "after 10 s it sits at 10 BPM (speed %.3f, want %.3f)", gl_speed, 10.0f / bpm);
	up(GB_RWD); run(1500);
	CHECK(gl_speed > 0.99f, "release springs back (speed %.3f)", gl_speed);

	/* PLAY: rest and grow */
	tap(GB_PLAY); run(50);
	CHECK(!gl_playing, "PLAY rests");
	tap(GB_PLAY); run(50);
	CHECK(gl_playing, "PLAY grows again");

	/* •• + RWD held 2 s: save */
	int saved_slot = 0;
	down(GB_FN); down(GB_RWD);
	for (int t = 0; t < 2300; t += 5) {
		glui_tick(now, &in, &out);
		if (out.save_slot)
			saved_slot = out.save_slot;
		now += 5;
	}
	up(GB_RWD); up(GB_FN);
	CHECK(saved_slot == 1, "•• + RWD held 2 s saves piece 1 (slot %d)", saved_slot);
	CHECK(glui_saved(0) != 0, "the saved copy is kept");

	/* a saved piece comes back when it is chosen again */
	gl_patch mine = *glui_patch();
	down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	for (int k = 0; k < GL_NPIECES - 1; k++) {
		down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	}
	CHECK(out.piece == 0, "%d steps wrap to piece 1 (%d)", GL_NPIECES, out.piece);
	CHECK(memcmp(glui_patch(), &mine, sizeof mine) == 0, "piece 1 comes back as saved");

	/* •• + VOL: the distortion */
	CHECK(out.dist_amt == 0 && out.dist_type == 0 && out.dist_post == 0, "distortion starts off, Chebyshev, before the effects");
	int want_amt[5] = {1, 2, 3, 4, 0};
	for (int k = 0; k < 5; k++) {
		down(GB_FN); tap(GB_VOLDN); up(GB_FN);
		CHECK(out.dist_amt == want_amt[k], "•• + VOL- tap %d: amount %d (got %d)", k + 1, want_amt[k], out.dist_amt);
	}
	down(GB_FN); tap(GB_VOLDN); tap(GB_VOLDN); up(GB_FN);
	CHECK(out.dist_amt == 2, "amount 2 again");
	CHECK(out.led_p[0] == 255 && out.led_p[1] == 255 && out.led_p[3] == 0, "before the effects: the bar fills from the •• end");
	CHECK(out.led_t[0] == 255 && out.led_t[1] == 0, "the track row shows the type (1 = Chebyshev)");
	down(GB_FN); tap(GB_VOLUP); up(GB_FN);
	CHECK(out.dist_type == GD_OVERDRIVE, "•• + VOL+ tap: next type (got %d)", out.dist_type);
	CHECK(out.led_t[1] == 255 && out.led_t[0] == 0, "the track row shows type 2");
	for (int k = 0; k < GD_COUNT; k++) {
		down(GB_FN); tap(GB_VOLUP); up(GB_FN);
	}
	CHECK(out.dist_type == GD_OVERDRIVE, "five types, round again (got %d)", out.dist_type);
	int pg0 = out.page;
	down(GB_FN); down(GB_VOLUP); run(3200);
	CHECK(out.dist_post == 1, "•• + VOL+ held 3 s: after the effects");
	up(GB_VOLUP); up(GB_FN);
	CHECK(out.dist_type == GD_OVERDRIVE, "the hold is not also a tap (type unchanged)");
	CHECK(out.led_p[3] == 255 && out.led_p[2] == 255 && out.led_p[0] == 0, "after the effects: the bar fills from the PLAY end");
	CHECK(out.page == pg0, "•• held with VOL is not a •• tap");
	down(GB_FN); down(GB_VOLDN); run(1500);
	CHECK(out.led_p[0] > 0 && out.led_p[3] == 0, "holding: the play row fills toward 3 s");
	run(1700);
	up(GB_VOLDN); up(GB_FN);
	CHECK(out.dist_amt == 0 && out.dist_type == 0 && out.dist_post == 0, "•• + VOL- held 3 s: off, the stack back to default");
	run(2000);

	/* VOL */
	float v0 = out.volume;
	tap(GB_VOLUP);
	CHECK(out.volume > v0, "VOL+ louder");
	tap(GB_VOLDN); tap(GB_VOLDN);
	CHECK(out.volume < v0, "VOL- quieter");

	/* a long •• hold alone does nothing here (it is the firmware's power-off) */
	int pg = out.page;
	down(GB_FN); run(3500); up(GB_FN); run(500);
	CHECK(out.page == pg, "•• held alone changes nothing in the UI");

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
