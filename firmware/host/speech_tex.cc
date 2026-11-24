// Audition: three speech textures from the Plaits speech engine (Emilie Gillet, MIT; the copy the
// Wakes base carries), played into a Glag piece through the engine's texture slot (GL_EXT_TEX).
//   speech_tex PIECE KIND VERB SECS OUT.wav     KIND 0 none, 1 choir, 2 SAM, 3 radio words; VERB 0 dry, 1 into the reverb
// choir: three formant voices on the chord's notes, their vowels drifting; SAM: two voices on
// the root and fifth an octave down, articulating now and then; radio: a word every few seconds
// from Plaits' word banks, band-passed like a far-off radio.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
extern "C" {
#include "glag_engine.h"
extern void (*gl_ext_tex)(float *HL, float *HR, float *V, uint32_t n, const int *chord, int nchord);
}
#include "plaits/dsp/speech/naive_speech_synth.h"
#include "plaits/dsp/speech/sam_speech_synth.h"
#include "plaits/dsp/speech/lpc_speech_synth_controller.h"
#include "plaits/dsp/speech/lpc_speech_synth_words.h"
#include "stmlib/utils/buffer_allocator.h"

using namespace plaits;

static int kind, verb;
static NaiveSpeechSynth choir[3];
static SAMSpeechSynth sam[2];
static LPCSpeechSynthWordBank bank;
static LPCSpeechSynthController lpc;
static char arena[32768];
static float f_now[3], t_s;            // gliding pitches (normalized at 48 kHz); seconds
static uint32_t rng = 12345;
static float urand() { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0f; }
static float next_word = 2.0f, bp1[2], bp2[2], word_lvl;
static float mtof48(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f) / 48000.0f; }
static float sam_click_at = 1.0f;

static void tex(float *HL, float *HR, float *V, uint32_t n, const int *chord, int nchord)
{
	// 1 ms: 48 samples at 48 kHz (four Plaits blocks), averaged in pairs to the slot's 24 kHz
	float out[3][48], tmp[48], exc[48];
	const float dt = 0.001f;
	t_s += dt;
	memset(out, 0, sizeof out);
	if (kind == 1) {
		static const float base = 52.0f;     // the voices around E3..E4
		for (int v = 0; v < 3; v++) {
			int pc = chord[v % nchord];
			float m = base + (float)((pc - (int)base % 12 + 24) % 12) + (v == 2 ? 12.0f : 0.0f);
			float tgt = mtof48(m);
			f_now[v] = f_now[v] ? f_now[v] + (tgt - f_now[v]) * 0.004f : tgt;   // a slow glide to new chords
			float vowel = 0.5f + 0.5f * sinf(6.2832f * t_s / (23.0f + 8.0f * v) + v);
			for (int b = 0; b < 4; b++)
				choir[v].Render(false, f_now[v], vowel, 0.35f, tmp, exc, &out[v][b * 12], 12);
		}
	} else if (kind == 2) {
		for (int v = 0; v < 2; v++) {
			int pc = chord[v ? (nchord > 2 ? 2 : 1) : 0];
			float m = 45.0f + (float)((pc - 45 % 12 + 24) % 12);
			float tgt = mtof48(m);
			f_now[v] = f_now[v] ? f_now[v] + (tgt - f_now[v]) * 0.004f : tgt;
			float vowel = 0.5f + 0.5f * sinf(6.2832f * t_s / (11.0f + 6.0f * v));
			bool click = false;
			if (v == 0 && t_s >= sam_click_at) { click = true; sam_click_at = t_s + 2.5f + 3.0f * urand(); }
			for (int b = 0; b < 4; b++)
				sam[v].Render(click && b == 0, f_now[v], vowel, 0.45f, exc, &out[v][b * 12], 12);
		}
	} else if (kind == 3) {
		bool trig = false;
		static int bk = 0;
		static float addr = 0.0f;
		if (t_s >= next_word) {
			trig = true;
			next_word = t_s + 3.0f + 4.0f * urand();
			bk = (int)(urand() * LPC_SPEECH_SYNTH_NUM_WORD_BANKS);
			addr = urand();
			word_lvl = 0.6f + 0.4f * urand();
		}
		float m = 48.0f + (float)((chord[0] - 48 % 12 + 24) % 12);
		for (int b = 0; b < 4; b++)
			lpc.Render(false, trig && b == 0, bk, mtof48(m), 0.4f, 0.0f, addr, 0.5f, 1.0f, exc, &out[0][b * 12], 12);
		for (int i = 0; i < 48; i++) {            // the radio: ~350 Hz to ~2.5 kHz
			float x = out[0][i] * word_lvl;
			bp1[0] += 0.045f * (x - bp1[0]);
			float hp = x - bp1[0];
			bp2[0] += 0.28f * (hp - bp2[0]);
			bp2[1] += 0.28f * (bp2[0] - bp2[1]);
			out[0][i] = bp2[1] * 1.6f;
		}
	}
	if (!kind)
		return;
	static const float PAN[3] = {0.3f, 0.7f, 0.5f};
	const float lvl = kind == 1 ? 0.04f : kind == 2 ? 0.08f : 0.11f;
	const float dry = verb ? 0.55f : 1.0f, send = verb ? 1.2f : 0.0f;
	const int nv = kind == 1 ? 3 : kind == 2 ? 2 : 1;
	for (uint32_t i = 0; i < n; i++) {
		float l = 0.0f, r = 0.0f, m = 0.0f;
		for (int v = 0; v < nv; v++) {
			float x = 0.5f * (out[v][2 * i] + out[v][2 * i + 1]) * lvl;
			float p = nv == 1 ? 0.5f : PAN[v];
			l += x * (1.0f - p);
			r += x * p;
			m += x;
		}
		HL[i] += l * dry;
		HR[i] += r * dry;
		V[i] += m * send;
	}
}

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
	if (argc < 6) { fprintf(stderr, "speech_tex PIECE KIND VERB SECS OUT.wav\n"); return 2; }
	int piece = atoi(argv[1]);
	kind = atoi(argv[2]);
	verb = atoi(argv[3]);
	float secs = (float)atof(argv[4]);
	for (auto &c : choir) c.Init();
	for (auto &s : sam) s.Init();
	stmlib::BufferAllocator alloc(arena, sizeof arena);
	bank.Init(word_banks_, LPC_SPEECH_SYNTH_NUM_WORD_BANKS, &alloc);
	lpc.Init(&bank);
	gl_init(1234);
	gl_patch p;
	gl_piece(piece, &p);
	gl_post_patch(&p);
	gl_ext_tex = tex;
	uint32_t frames = (uint32_t)(secs * 48000.0f);
	FILE *f = fopen(argv[5], "wb");
	wav_header(f, frames);
	int16_t blk[192];
	for (uint32_t i = 0; i < frames; i += 96) {
		gl_render(blk, 96, 0.8f);
		fwrite(blk, 2, 192, f);
	}
	fclose(f);
	return 0;
}
