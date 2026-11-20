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
	CHECK(p->voice[0] == GV_PAD && p->voice[1] == GV_BELLS && p->voice[2] == GV_BASS && p->voice[3] == GV_PULSE,
	      "default voices Pad Bells Bass Pulse (Garden)");

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

	/* F1 on a channel page is character; F4 on the extended page is activity */
	float chr = glui_patch()->ch[2][CP_CHAR];
	sweep(0, chr);
	sweep(0, 0.9f);
	CHECK(fabsf(glui_patch()->ch[2][CP_CHAR] - 0.9f) < 0.02f, "F1 on a channel page sets character (%.3f)", glui_patch()->ch[2][CP_CHAR]);
	down(GB_FN); tap(GB_T3); up(GB_FN);
	float act = glui_patch()->ch[2][CP_ACTIVITY];
	sweep(3, act);
	sweep(3, 0.1f);
	CHECK(fabsf(glui_patch()->ch[2][CP_ACTIVITY] - 0.1f) < 0.02f, "F4 on the extended page sets activity (%.3f)", glui_patch()->ch[2][CP_ACTIVITY]);
	down(GB_FN); tap(GB_T3); up(GB_FN);
	run(500);

	/* the rocker on a channel page is that channel's macro, not freeze or slow */
	tap(GB_FFWD); run(50);
	CHECK(!gl_frozen, "FFWD on a channel page does not freeze");
	CHECK(out.macro[2] == 1, "FFWD on a channel page: macro step 1 (%d)", out.macro[2]);
	for (int k = 0; k < 5; k++) { tap(GB_FFWD); run(100); }
	CHECK(out.macro[2] == 4, "the macro stops at its fourth step (%d)", out.macro[2]);
	CHECK(out.macro[0] == 0 && out.macro[1] == 0 && out.macro[3] == 0, "only this channel's macro moved");
	down(GB_RWD); run(3500);
	CHECK(gl_speed > 0.99f, "RWD held on a channel page does not slow the record (speed %.2f)", gl_speed);
	CHECK(out.macro_period[2] > 15.0f, "RWD held slows the macro's glide (%.1f s)", out.macro_period[2]);
	up(GB_RWD); run(100);
	float per = out.macro_period[2];
	run(1000);
	CHECK(out.macro_period[2] == per && out.macro[2] == 4, "letting go of RWD keeps the macro as it was");
	down(GB_FN); tap(GB_T3); up(GB_FN);
	tap(GB_FFWD); run(50);
	CHECK(!gl_frozen && out.macro[2] == 4, "the extended page works the macro too");
	tap(GB_RWD); run(80); tap(GB_RWD); run(50);
	CHECK(out.macro[2] == 0 && out.macro_period[2] == 8.0f, "RWD double tap turns the macro off (%d, %.1f s)", out.macro[2], out.macro_period[2]);
	down(GB_FN); tap(GB_T3); up(GB_FN);
	run(500);

	/* double tap •• : HOME */
	tap(GB_FN); run(60); tap(GB_FN);
	CHECK(out.page == PG_HOME, "•• double tap returns HOME");
	run(500);

	/* •• + T4 on HOME rotates channel 4's voice */
	down(GB_FN); tap(GB_T4); up(GB_FN);
	CHECK(glui_patch()->voice[3] == GV_STRINGS, "•• + T4: Pulse -> Strings (%d)", glui_patch()->voice[3]);
	down(GB_FN); tap(GB_T4); up(GB_FN);
	CHECK(glui_patch()->voice[3] == GV_GLOW, "•• + T4 again: Glow (%d)", glui_patch()->voice[3]);
	CHECK(out.page == PG_HOME, "rotating stays on HOME");
	for (int k = 0; k < GV_COUNT - 2; k++) {
		down(GB_FN); tap(GB_T4); up(GB_FN);
	}
	CHECK(glui_patch()->voice[3] == GV_PULSE, "all %d voices, back to Pulse (%d)", GV_COUNT, glui_patch()->voice[3]);
	run(500);

	/* •• + PLAY once: HARMONY */
	down(GB_FN); tap(GB_PLAY); up(GB_FN);
	run(500);
	CHECK(out.page == PG_HARMONY, "•• + PLAY once opens HARMONY (page %d)", out.page);
	float root = glui_patch()->g[GP_ROOT];
	sweep(0, root);
	sweep(0, 1.0f);
	CHECK(glui_patch()->g[GP_ROOT] == 1.0f, "root steps to B");

	/* the reverb, on HARMONY: •• + T1..T4, and Shimmer's interval with its T again */
	CHECK(glui_patch()->space[GL_SP_REVERB] == GR_HALL, "Garden starts with the Hall");
	down(GB_FN); tap(GB_T2); up(GB_FN);
	CHECK(out.page == PG_HARMONY && glui_patch()->space[GL_SP_REVERB] == GR_CHAMBER, "•• + T2 on HARMONY: Chamber, and stays on HARMONY");
	{
		unsigned f0 = glui_patch()->space[GL_SP_FLAGS] & GL_FLAG_REVERSE;
		down(GB_FN); tap(GB_T2); up(GB_FN); run(100);
		CHECK((glui_patch()->space[GL_SP_FLAGS] & GL_FLAG_REVERSE) != f0 && glui_patch()->space[GL_SP_REVERB] == GR_CHAMBER,
		      "•• + T2 again on Chamber: reverse swells toggle");
		down(GB_FN); tap(GB_T2); up(GB_FN); run(100);
		CHECK((glui_patch()->space[GL_SP_FLAGS] & GL_FLAG_REVERSE) == f0, "and again: back");
	}
	CHECK(out.led_t[1] == 255 && out.led_t[0] == 0, "the track row shows the reverb");
	down(GB_FN); tap(GB_T3); up(GB_FN);
	CHECK(glui_patch()->space[GL_SP_REVERB] == GR_SHIMMER && glui_patch()->space[GL_SP_SHIMMER] == GS_OCTAVE, "•• + T3: Shimmer, an octave");
	down(GB_FN); tap(GB_T3); up(GB_FN);
	CHECK(glui_patch()->space[GL_SP_SHIMMER] == GS_FIFTH, "•• + T3 again: a fifth");
	down(GB_FN); tap(GB_T3); up(GB_FN);
	down(GB_FN); tap(GB_T3); up(GB_FN);
	CHECK(glui_patch()->space[GL_SP_SHIMMER] == GS_OCTAVE, "seventh, then round to the octave");
	down(GB_FN); tap(GB_T1); up(GB_FN);
	CHECK(glui_patch()->space[GL_SP_REVERB] == GR_HALL, "•• + T1: back to the Hall");
	run(1600);
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

	/* ---- Playworld ---- */
	{
		int was_playing = gl_playing;
		down(GB_PLAY); run(2100); up(GB_PLAY); run(50);
		CHECK(out.page == PG_WORLD, "PLAY held 2 s opens Playworld (page %d)", out.page);
		CHECK(gl_playing == was_playing, "holding PLAY into Playworld neither rests nor grows");
		run(1100);
		CHECK(out.led_t[0] == 0 && out.led_t[1] == 0 && out.led_t[2] == 0 && out.led_t[3] == 0, "Playworld: no lights until something is on");
		tap(GB_T1); run(300);
		CHECK(out.world_song == 1 && out.world_focus == 0, "T1: song mode on, page 1 (%d, focus %d)", out.world_song, out.world_focus);
		run(1100);
		CHECK(out.led_t[0] == 255 && out.led_t[1] == 0, "song page 1: T1's light bright, the rest dark");
		tap(GB_T1); run(1100);
		CHECK(out.world_song == 2 && out.led_t[0] == 255 && out.led_t[1] > 0 && out.led_t[1] < 255, "T1 again: page 2, the second light dim (%d)", out.led_t[1]);
		/* page 2: F1 sets channel 1's loop; it waits for pickup first */
		float target = glui_target(0, 0);
		CHECK(fabsf(target - 0.5f) < 1e-6f, "song page 2 F1 is channel 1's loop (x1 = %.2f)", target);
		tap(GB_T4); run(1100);
		CHECK(out.world_tex == 1 && out.world_focus == 3 && out.world_song == 2, "T4: rain on, the faders follow it, song stays on");
		CHECK(out.led_t[0] == 255 && out.led_t[3] == 255, "both sections' lights bright (%d %d)", out.led_t[0], out.led_t[3]);
		tap(GB_T4); run(1100);
		CHECK(out.world_tex == 2 && out.led_t[2] > 0 && out.led_t[2] < 255, "T4 again: vinyl, light 3 dim (%d)", out.led_t[2]);
		tap(GB_T1); run(300);
		CHECK(out.world_song == 2 && out.world_focus == 0, "T1 while song is on elsewhere: just the focus back (page %d)", out.world_song);
		tap(GB_T3); run(300);
		CHECK(out.world_pump == 1 && out.world_focus == 2, "T3: the pump on");
		tap(GB_T3); tap(GB_T3); run(1100);
		CHECK(out.world_pump == 0 && out.world_focus >= 0, "T3 twice more: the pump off, focus to what's still on");
		tap(GB_T1); tap(GB_T1); tap(GB_T1); run(1100);
		CHECK(out.world_song == 0, "T1 past page 4: song mode off (%d)", out.world_song);
		tap(GB_T4); tap(GB_T4); tap(GB_T4); run(1100);
		CHECK(out.world_tex == 0, "T4 past surf: texture off (%d)", out.world_tex);
		tap(GB_T1); run(1100);
		down(GB_FN); run(30); up(GB_FN); run(30); down(GB_FN); run(30); up(GB_FN); run(50);
		CHECK(out.page == PG_HOME && out.world_song == 1, "•• double tap leaves Playworld, song mode stays on");
		down(GB_PLAY); run(2100); up(GB_PLAY); run(50);
		CHECK(out.page == PG_WORLD, "PLAY held again: back in Playworld");
		tap(GB_T1); tap(GB_T1); tap(GB_T1); tap(GB_T1); run(50);
		down(GB_PLAY); run(2100); up(GB_PLAY); run(50);
		CHECK(out.page == PG_HOME && out.world_song == 0, "PLAY held in Playworld: back to Home (song off now)");
	}

	/* PLAY: rest and grow */
	tap(GB_PLAY); run(50);
	CHECK(!gl_playing, "PLAY rests");
	tap(GB_PLAY); run(50);
	CHECK(gl_playing, "PLAY grows again");

	/* •• + RWD held 3 s: save (with a macro on, to see it kept as the scene) */
	tap(GB_T3); run(50);
	tap(GB_FFWD); run(100); tap(GB_FFWD); run(100);
	down(GB_FN); run(30); up(GB_FN); run(30); down(GB_FN); run(30); up(GB_FN); run(400);   /* back HOME */
	int saved_slot = 0;
	down(GB_FN); down(GB_RWD);
	for (int t = 0; t < 3300; t += 5) {
		glui_tick(now, &in, &out);
		if (out.save_slot)
			saved_slot = out.save_slot;
		now += 5;
	}
	up(GB_RWD); up(GB_FN);
	CHECK(saved_slot == 1, "•• + RWD held 3 s saves piece 1 (slot %d)", saved_slot);
	CHECK(glui_saved(0) != 0, "the saved copy is kept");

	/* a saved piece comes back when it is chosen again */
	gl_patch mine = *glui_patch();
	int mac_saved = out.macro[2];
	down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	CHECK(out.macro[2] == 0, "a stock piece sets its own macros: off (%d)", out.macro[2]);
	for (int k = 0; k < GL_NPIECES - 1; k++) {
		down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	}
	CHECK(out.piece == 0, "%d steps wrap to piece 1 (%d)", GL_NPIECES, out.piece);
	CHECK(memcmp(glui_patch(), &mine, sizeof mine) == 0, "piece 1 comes back as saved");
	CHECK(out.macro[2] == 0, "it loads with its macros off (%d)", out.macro[2]);
	down(GB_FN); down(GB_FFWD); run(3200); up(GB_FFWD); up(GB_FN); run(300);
	CHECK(mac_saved > 0 && out.macro[2] == mac_saved, "•• + FFWD held 3 s: the macros it was saved with (%d, saved %d)", out.macro[2], mac_saved);
	down(GB_FN); tap(GB_PLAY); run(500); up(GB_FN); run(600);
	CHECK(out.page == PG_HARMONY, "on HARMONY (%d)", out.page);
	tap(GB_FFWD); run(100);
	CHECK(!gl_frozen, "FFWD on HARMONY does not freeze");
	{
		for (int k = 0; k < 10; k++) { tap(GB_FFWD); run(100); }
		CHECK(glui_patch()->space[GL_SP_ECHO] == 4, "FFWD on HARMONY: the echo longer, up to 2 bars (%d)", glui_patch()->space[GL_SP_ECHO]);
		for (int k = 0; k < 10; k++) { tap(GB_RWD); run(100); }
		CHECK(glui_patch()->space[GL_SP_ECHO] == GL_ECHO_STRING && gl_speed > 0.99f, "RWD on HARMONY: shorter, down to the string, no slow-down (%d)", glui_patch()->space[GL_SP_ECHO]);
		down(GB_FN); tap(GB_FFWD); run(100); up(GB_FN); run(200);
		CHECK(GL_FLAG_KEY(glui_patch()->space[GL_SP_FLAGS]) == 1u, "•• + FFWD on HARMONY: key travel, up a fifth");
		down(GB_FN); tap(GB_RWD); run(100); up(GB_FN); run(200);
		CHECK(GL_FLAG_SHAPE(glui_patch()->space[GL_SP_FLAGS]) == 1u, "•• + RWD on HARMONY: the macros' shape, bar steps");
	}
	{	/* •• + the rocker is Home's only: on a channel page it does nothing (kept for later) */
		down(GB_FN); run(30); up(GB_FN); run(30); down(GB_FN); run(30); up(GB_FN); run(400);
		tap(GB_T2); run(100);
		gl_patch was = *glui_patch();
		down(GB_FN); tap(GB_FFWD); run(100); up(GB_FN); run(300);
		CHECK(memcmp(glui_patch()->ch, was.ch, sizeof was.ch) == 0 && GL_CHX_IVL(glui_patch()->chx[1]) == 1u,
		      "•• + FFWD on a channel page: not a randomize, the interval a third (%u)", GL_CHX_IVL(glui_patch()->chx[1]));
		for (int k = 0; k < 4; k++) { down(GB_FN); tap(GB_FFWD); run(100); up(GB_FN); run(200); }
		CHECK(GL_CHX_IVL(glui_patch()->chx[1]) == 0u, "five steps round: none again");
		int slot = 0;
		down(GB_FN); down(GB_RWD);
		for (int t = 0; t < 3300; t += 5) { glui_tick(now, &in, &out); if (out.save_slot) slot = out.save_slot; now += 5; }
		up(GB_RWD); up(GB_FN); run(300);
		CHECK(slot == 0, "•• + RWD held 3 s on a channel page does not save (slot %d)", slot);
		CHECK(GL_CHX_DIV(glui_patch()->chx[1]) == 1u, "•• + RWD on a channel page: its time division, 1/2 (%u)", GL_CHX_DIV(glui_patch()->chx[1]));
		down(GB_FN); tap(GB_RWD); run(100); up(GB_FN); run(200);
		down(GB_FN); tap(GB_RWD); run(100); up(GB_FN); run(200);
		down(GB_FN); tap(GB_RWD); run(100); up(GB_FN); run(200);
		CHECK(GL_CHX_DIV(glui_patch()->chx[1]) == 0u, "four steps round: 1 again");
	}
	down(GB_FN); run(30); up(GB_FN); run(30); down(GB_FN); run(30); up(GB_FN); run(400);

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
	down(GB_FN); down(GB_VOLUP); run(3200); up(GB_VOLUP); up(GB_FN);
	CHECK(out.dist_post == 2, "•• + VOL+ held again: on the reverb only");
	down(GB_FN); down(GB_VOLUP); run(3200); up(GB_VOLUP); up(GB_FN);
	CHECK(out.dist_post == 3, "and again: on the drums only");
	down(GB_FN); down(GB_VOLUP); run(3200); up(GB_VOLUP); up(GB_FN);
	CHECK(out.dist_post == 0, "and again: before the effects");
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

	/* the macro scene on Home: •• + FFWD held 3 s brings it in, •• + RWD held 3 s stores it */
	glui_init(); run(500);
	CHECK(out.page == PG_HOME && out.piece == 0 && out.macro[0] == 0, "back on Home, Garden, macros off");
	down(GB_FN); down(GB_FFWD); run(2000);
	CHECK(out.macro[0] == 0, "•• + FFWD held 2 s: not yet");
	run(1200); up(GB_FFWD); up(GB_FN); run(300);
	CHECK(out.macro[0] == 2 && out.macro[1] == 2 && out.macro[2] == 1 && out.macro[3] == 1,
	      "•• + FFWD held 3 s: Garden's scene, 2 2 1 1 (%d %d %d %d)", out.macro[0], out.macro[1], out.macro[2], out.macro[3]);
	gl_patch keep = *glui_patch();
	CHECK(memcmp(glui_patch(), &keep, sizeof keep) == 0, "and no randomize on letting go");
	tap(GB_T4); run(50); tap(GB_FFWD); run(100); tap(GB_FFWD); run(100); tap(GB_FFWD); run(100);
	down(GB_FN); run(30); up(GB_FN); run(30); down(GB_FN); run(30); up(GB_FN); run(400);
	CHECK(out.page == PG_HOME && out.macro[3] == 4, "channel 4 to full, back Home (%d)", out.macro[3]);
	down(GB_FN); down(GB_RWD); run(3200); up(GB_RWD); up(GB_FN); run(300);
	down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	CHECK(out.piece == 2 && out.macro[3] == 0, "two pieces on, macros off (%d)", out.macro[3]);
	for (int k = 0; k < GL_NPIECES - 2; k++) {
		down(GB_FN); tap(GB_PLAY); run(50); tap(GB_PLAY); up(GB_FN); run(500);
	}
	CHECK(out.piece == 0, "round to Garden again (%d)", out.piece);
	down(GB_FN); down(GB_FFWD); run(3200); up(GB_FFWD); up(GB_FN); run(300);
	CHECK(out.macro[0] == 2 && out.macro[3] == 4, "the stored scene comes back: channel 4 at full (%d %d)", out.macro[0], out.macro[3]);

	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
