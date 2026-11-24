/* The Hall's rule (glag_ui.c, hall_ok): on HARMONY, •• + T1 gives the Hall to a piece written
 * with it or light enough for it, and leaves any other on its own room. Built with the test hooks. */
#include <stdio.h>
#include "glag_engine.h"
#include "glag_ui.h"

static gl_input in = {{0.5f, 0.5f, 0.5f, 0.5f}, 0};
static gl_ui_out out;
static uint32_t now;
static int fails;
static void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 8) { now += 8; glui_tick(now, &in, &out); } }
static void down(int b) { in.buttons |= (uint16_t)(1u << b); run(10); }
static void up(int b) { in.buttons &= (uint16_t)~(1u << b); run(10); }
static void tap(int b) { down(b); run(40); up(b); }
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("FAIL: %s\n", msg); } else printf("ok: %s\n", msg); } while (0)
static int t1_on(int piece)
{
	glui_test(GT_RESET, piece, 0, 0.0f, now);
	run(100);
	glui_test(GT_PAGE, PG_HARMONY, 0, 0.0f, now);
	run(100);
	down(GB_FN); tap(GB_T1); up(GB_FN); run(100);
	return glui_patch()->space[GL_SP_REVERB];
}
int main(void)
{
	gl_init(1);
	glui_init();
	run(100);
	CHECK(t1_on(0) == GR_HALL, "Garden (written with the Hall): T1 gives the Hall");
	CHECK(t1_on(11) == GR_HALL, "Lounge (light: 57 points with the Hall): T1 gives the Hall");
	CHECK(t1_on(6) == GR_SHIMMER, "Glass (64 points with the Hall): T1 leaves it on the Shimmer");
	CHECK(t1_on(15) == GR_HALL, "Long Haul (59.5 with the Hall: light): T1 gives the Hall");
	CHECK(t1_on(4) == GR_CHAMBER, "Night Bus (64 with the Hall): T1 leaves it on the Chamber");
	printf("%s\n", fails ? "hall rule: FAILED" : "hall rule: all passed");
	return fails != 0;
}
