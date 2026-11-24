// The speech textures' cost on the SP-1's Cortex-M4F, as the audition renders them (Plaits at
// 48 kHz, 48 samples a millisecond). mark: 1 a millisecond starts, 2 it ends, 3 done.
#include <stdint.h>
#include <string.h>
#include "plaits/dsp/speech/naive_speech_synth.h"
#include "plaits/dsp/speech/sam_speech_synth.h"
#include "plaits/dsp/speech/lpc_speech_synth_controller.h"
#include "plaits/dsp/speech/lpc_speech_synth_words.h"
#include "stmlib/utils/buffer_allocator.h"
using namespace plaits;
volatile uint32_t mark;
volatile int cfg_kind = 1;
volatile float sink;
static NaiveSpeechSynth choir[3];
static SAMSpeechSynth sam[2];
static LPCSpeechSynthWordBank bank;
static LPCSpeechSynthController lpc;
static char arena[32768];
extern "C" int __aeabi_atexit(void *, void (*)(void *), void *) { return 0; }
void *__dso_handle;
extern "C" void *memset(void *d, int c, size_t n) { uint8_t *p = (uint8_t *)d; while (n--) *p++ = (uint8_t)c; return d; }
extern "C" void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s; while (n--) *p++ = *q++; return d; }
int main()
{
	const int kind = cfg_kind;
	for (auto &c : choir) c.Init();
	for (auto &s : sam) s.Init();
	stmlib::BufferAllocator alloc(arena, sizeof arena);
	bank.Init(word_banks_, LPC_SPEECH_SYNTH_NUM_WORD_BANKS, &alloc);
	lpc.Init(&bank);
	static const float F[3] = {164.8f / 48000.0f, 207.7f / 48000.0f, 329.6f / 48000.0f};   /* E3 G#3 E4 */
	float out[3][48], tmp[48], exc[48], v0 = 0.3f, bp1 = 0, bp2a = 0, bp2b = 0;
	for (int ms = 0; ms < 6000; ms++) {
		mark = 1;
		if (kind == 1) {
			for (int v = 0; v < 3; v++)
				for (int b = 0; b < 4; b++)
					choir[v].Render(false, F[v], v0 + 0.1f * v, 0.35f, tmp, exc, &out[v][b * 12], 12);
		} else if (kind == 2) {
			for (int v = 0; v < 2; v++)
				for (int b = 0; b < 4; b++)
					sam[v].Render(ms % 3000 == 0 && b == 0 && v == 0, F[v] * 0.5f, v0, 0.45f, exc, &out[v][b * 12], 12);
		} else {
			for (int b = 0; b < 4; b++)
				lpc.Render(false, ms % 3000 == 0 && b == 0, (ms / 3000) % 5, F[0] * 0.8f, 0.4f, 0.0f, 0.37f, 0.5f, 1.0f, exc, &out[0][b * 12], 12);
			for (int i = 0; i < 48; i++) {
				float x = out[0][i];
				bp1 += 0.045f * (x - bp1);
				float hp = x - bp1;
				bp2a += 0.28f * (hp - bp2a);
				bp2b += 0.28f * (bp2a - bp2b);
				out[0][i] = bp2b;
			}
		}
		float acc = 0;                          /* the mix into the slot: 24 samples, panned */
		const int nv = kind == 1 ? 3 : kind == 2 ? 2 : 1;
		for (int i = 0; i < 24; i++)
			for (int v = 0; v < nv; v++)
				acc += 0.5f * (out[v][2 * i] + out[v][2 * i + 1]) * 0.04f;
		sink = acc;
		v0 += 0.0001f;
		if (v0 > 0.9f) v0 = 0.1f;
		mark = 2;
	}
	mark = 3;
	for (;;) { }
}
extern "C" {
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
void reset(void)
{
	for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
	for (uint32_t *d = &_sbss; d < &_ebss;) *d++ = 0;
	*(volatile uint32_t *)0xE000ED88 |= 0xF << 20;   /* FPU on */
	main();
}
__attribute__((section(".vectors"), used)) const void *vectors[2] = {&_estack, (void *)reset};
}
