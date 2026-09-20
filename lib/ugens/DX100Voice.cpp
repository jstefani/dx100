// DX100Voice — one 4-op FM voice as a single UGen.
// Waves, algorithms, feedback, envelopes in C++ so unused branches are
// not computed (SC Select cannot skip).
//
// Voice model follows the Yamaha OPP (YM2164) family as used in the
// DX100 / DX21 / DX27 / TX81Z:
//   - operator level, D1L, key scaling, velocity, AM and EG bias are all
//     attenuations in dB; the envelope runs in the dB domain (decays are
//     linear in dB, attack is an exponential approach to 0 dB), and the
//     dB -> amplitude conversion happens once per op per sample
//   - a level-99 modulator swings the carrier phase by 8 pi radians
//   - feedback averages the last two op4 samples, FB 7 = 4 pi
//   - operator phases reset on key-on
//   - the LFO is shared (one per engine, on a control bus); this UGen
//     only applies delay, PMS and AMS to the value it is handed
//
// Performance note: the inner loop must stay free of libm transcendentals.
// sinf/powf/expf on the norns CPU (Cortex-A53, no fast transcendental unit)
// cost 50-150 cycles each, so:
//   - sin comes from a 4096-entry linear-interpolated table  (-123 dB)
//   - 2^x comes from exponent-bit assembly + quintic mantissa fit
//     (max 0.0004 cents error)
//   - everything derived only from scalar-rate IN0 is hoisted to block rate

#include "SC_PlugIn.h"
#include <math.h>
#include <stdint.h>

static InterfaceTable* ft;

static const float kPi = 3.14159265358979323846f;
static const float kTwoPi = 6.28318530717958647692f;
// Phase swing of a full-level modulator. OPM-family arithmetic: 14-bit
// operator output shifted right once, added to a 10-bit phase, so a
// level-99 op moves the carrier by +-4 cycles.
static const float kModIndex = 8.f * kPi;
// Attenuation treated as silence. Hardware envelope range is 96 dB.
static const float kSilentDb = 96.f;
// dB -> 2^ exponent: 10^(-x/20) == 2^(-x/6.0206)
static const float kDbToExp2 = -1.f / 6.02059991f;
// Hardware envelope step (10 bits over 96 dB), used by the grit option.
static const float kEnvStepDb = 0.09375f;

// ---- sine table -------------------------------------------------------
#define kSineBits 12
#define kSineSize (1 << kSineBits)
#define kSineMask (kSineSize - 1)

struct SineTable {
	float v[kSineSize + 1];
	SineTable() {
		for (int i = 0; i <= kSineSize; ++i) {
			v[i] = sinf(kTwoPi * (float)i / (float)kSineSize);
		}
	}
};
static const SineTable gSineTable;
static const float* const gSine = gSineTable.v;

// phase in turns (0..1); wraps.
static inline float sinTab(float phase) {
	float p = phase - floorf(phase);
	float fi = p * (float)kSineSize;
	int i = (int)fi;
	float frac = fi - (float)i;
	i &= kSineMask;
	float a = gSine[i];
	return a + (gSine[i + 1] - a) * frac;
}

// Hardware-style lookup: 10-bit phase index, no interpolation.
static inline float sinTabGrit(float phase) {
	float p = phase - floorf(phase);
	int i = ((int)(p * 1024.f)) & 1023;
	return gSine[i << 2];
}

static inline float sinRad(float x) { return sinTab(x * (1.f / kTwoPi)); }

// ---- fast 2^x ---------------------------------------------------------
static inline float exp2f_fast(float x) {
	if (x > 126.f) x = 126.f;
	if (x < -126.f) x = -126.f;
	float xf = floorf(x);
	int xi = (int)xf;
	float f = x - xf;
	float m = 0.99999983f
		+ f * (0.69315474f
		+ f * (0.24014650f
		+ f * (0.05583597f
		+ f * (0.00898726f
		+ f * 0.00187538f))));
	union { float f; uint32_t u; } v;
	v.u = (uint32_t)((xi + 127) & 0xFF) << 23;
	return m * v.f;
}

static inline float expf_fast(float x) {
	return exp2f_fast(x * 1.4426950408889634f);
}

// ---- fast log2 --------------------------------------------------------
static inline float log2f_fast(float x) {
	if (x <= 1e-20f) return -66.f;
	union { float f; uint32_t u; } v;
	v.f = x;
	int e = (int)((v.u >> 23) & 0xFF) - 127;
	v.u = (v.u & 0x007FFFFF) | 0x3F800000;
	float m = v.f;
	float p = -3.02832497f
		+ m * (6.06585886f
		+ m * (-5.26415552f
		+ m * (3.21886981f
		+ m * (-1.23427990f
		+ m * (0.26686277f
		+ m * -0.02482598f)))));
	return p + (float)e;
}

enum {
	i_hz = 0,
	i_gate,
	i_vel,
	i_legato,
	i_algo,
	i_feedback,     // 0..7, hardware FB level
	i_dxFeedback,
	i_transpose,
	i_port,
	i_portMode,
	i_lfoIn,        // shared LFO, bipolar -1..1
	i_lfoDelay,
	i_lfoUni,
	i_pms,
	i_ams,
	i_alms,
	i_pr1, i_pr2, i_pr3,   // pitch EG rates 0..1
	i_pl1, i_pl2, i_pl3,   // pitch EG levels -1..1 (x48 semitones)
	i_breath,       // 0..1
	i_egBias,       // breath EG bias range 0..1
	i_r1, i_r2, i_r3, i_r4,   // ratio
	i_d1, i_d2, i_d3, i_d4,   // detune steps -3..3
	i_f1, i_f2, i_f3, i_f4,   // fixed hz
	i_x1, i_x2, i_x3, i_x4,   // fixed mode
	i_w1, i_w2, i_w3, i_w4,   // wave
	i_l1, i_l2, i_l3, i_l4,   // output level 0..1 (OL/99)
	i_a1, i_a2, i_a3, i_a4,   // AR 0..1
	i_b1, i_b2, i_b3, i_b4,   // D1R
	i_c1, i_c2, i_c3, i_c4,   // D1L
	i_e1, i_e2, i_e3, i_e4,   // D2R
	i_g1, i_g2, i_g3, i_g4,   // RR
	i_k1, i_k2, i_k3, i_k4,   // keyboard level scaling 0..1
	i_v1, i_v2, i_v3, i_v4,   // key velocity sensitivity 0..1
	i_m1, i_m2, i_m3, i_m4,   // AME on/off
	i_s1, i_s2, i_s3, i_s4,   // keyboard rate scaling 0..3
	i_z1, i_z2, i_z3, i_z4,   // EG bias sensitivity 0..1
	i_grit,
	i_oversample,
	i_numInputs
};

enum { o_snd = 0, o_env, o_numOutputs };

enum EnvStage { kIdle, kAtk, kD1, kD2, kSus, kRel };

// Envelope state is an attenuation in dB: 0 = full level, 96 = silent.
struct OpEnv {
	int stage;
	float att;
};

enum PegStage { kPegRest, kPegL1, kPegL2, kPegL3 };

struct DX100Voice : public Unit {
	float sr;
	float recSr;
	float opPhase[4];
	float fbLast, fbPrev;
	float freqLag;
	float prevGate;
	float lfoDelayPos;
	int pegStage;
	int pegInit;
	float pegLevel;     // semitones
	OpEnv env[4];
	float dz[2][2];
	int lastOs;
};

// Direct-form-II transposed biquad.
static inline float biquad(float x, const float* c, float* z) {
	float y = c[0] * x + z[0];
	z[0] = c[1] * x - c[3] * y + z[1];
	z[1] = c[2] * x - c[4] * y;
	return y;
}

static void makeLowpass(float fc, float sr, float coef[2][5]) {
	static const float qs[2] = { 0.54119610f, 1.30656296f };
	float w0 = 2.f * kPi * fc / sr;
	if (w0 > 3.0f) w0 = 3.0f;
	float cw = cosf(w0), sw = sinf(w0);
	for (int s = 0; s < 2; ++s) {
		float alpha = sw / (2.f * qs[s]);
		float b0 = (1.f - cw) * 0.5f, b1 = 1.f - cw, b2 = b0;
		float a0 = 1.f + alpha, a1 = -2.f * cw, a2 = 1.f - alpha;
		coef[s][0] = b0 / a0;
		coef[s][1] = b1 / a0;
		coef[s][2] = b2 / a0;
		coef[s][3] = a1 / a0;
		coef[s][4] = a2 / a0;
	}
}

static float clip01(float x) {
	if (x < 0.f) return 0.f;
	if (x > 1.f) return 1.f;
	return x;
}

static float wrap01(float p) {
	p -= floorf(p);
	if (p < 0.f) p += 1.f;
	return p;
}

// ---- hardware tables ----------------------------------------------------

// Operator output level 0..99 -> attenuation in dB. TL is 7 bits at
// 0.75 dB/step; OL >= 20 maps as OL+28, below that the steps widen.
static const uint8_t kOlTab[20] = {
	0, 5, 9, 13, 17, 20, 23, 25, 27, 29,
	31, 33, 35, 37, 39, 41, 42, 43, 45, 46
};
static float levelDb(float l01) {
	int ol = (int)(clip01(l01) * 99.f + 0.5f);
	int tl = ol < 20 ? kOlTab[ol] : ol + 28;
	if (tl > 127) tl = 127;
	return (float)(127 - tl) * 0.75f;
}

// D1L 0..1 -> dB. Hardware: 16 steps of 3 dB, 0 = off.
static float d1lDb(float c01) {
	if (c01 <= 0.f) return kSilentDb;
	return (1.f - clip01(c01)) * 45.f;
}

// 96 dB decay time for a 0..31 rate (r01 * 31). OPM rates: the time
// halves every 2 rate steps, ~0.7 ms at 31. Rate 0 = hold (returns 0).
static float decayTime(float r01) {
	if (r01 <= 0.f) return 0.f;
	float r = clip01(r01) * 31.f;
	return 0.0007f * exp2f((31.f - r) * 0.5f);
}

// Attack: time from 96 dB to ~0. 31 = instantaneous, 0 ~ 14 s (hardware
// AR 0 never rises; we keep it finite so a patch cannot go silent).
static float attackTime(float r01) {
	float r = clip01(r01) * 31.f;
	return 0.0003f * exp2f((31.f - r) * 0.5f);
}

// Release 0..15: internal rate 4*RR+2, 0 = slow decay (~23 s), 15 fastest.
static float releaseTime(float r01) {
	float rr = clip01(r01) * 15.f;
	return 0.0007f * exp2f(15.f - rr);
}

// Pitch EG rate 0..1 -> time for a full 96-semitone sweep.
static float pegTime(float r01) {
	return 0.002f * exp2f((1.f - clip01(r01)) * 12.f);
}

// phase in turns; pm in radians.
static inline float opWave(int wave, float phase, float pm, int grit) {
	float pmTurns = pm * (1.f / kTwoPi);
	float s = grit ? sinTabGrit(phase + pmTurns) : sinTab(phase + pmTurns);
	switch (wave) {
	case 0: return s;
	case 1: return s > 0.f ? s : 0.f;
	case 2: return fabsf(s);
	case 3: {
		float pulse = phase < 0.5f ? 1.f : 0.f;
		return (s > 0.f ? s : 0.f) * pulse;
	}
	case 4: {
		float s2 = sinTab(2.f * phase + pmTurns);
		return s2 * (phase < 0.5f ? 1.f : 0.f);
	}
	case 5: {
		float s2 = sinTab(2.f * phase + pmTurns);
		float alt = s2 * (phase < 0.5f ? 1.f : 0.f);
		return alt > 0.f ? alt : 0.f;
	}
	case 6: return fabsf(s) * s;
	case 7: {
		float s2 = sinTab(2.f * phase + pmTurns);
		float s3 = sinTab(3.f * phase + pmTurns);
		return (s + s2 * 0.5f + s3 * 0.333f) * 0.6f;
	}
	default: return s;
	}
}

// Routing for one algorithm. m[i][j] = how much op j modulates op i.
struct Routing {
	float mod3_4;
	float mod2_3, mod2_4;
	float mod1_2, mod1_3, mod1_4;
	float car1, car2, car3, car4;
};

// 0-7: the Yamaha 4-op set (DX100/DX21/DX27/TX81Z panel order).
// 8-15: extra wirings. Carriers sum raw, as on the chip.
static void makeRouting(int algo, Routing* r) {
	const float m = kModIndex;
	float m34 = 0, m23 = 0, m24 = 0, m12 = 0, m13 = 0, m14 = 0;
	float c1 = 1, c2 = 0, c3 = 0, c4 = 0;
	switch (algo) {
	case 0:  m34 = m; m23 = m; m12 = m; c1 = 1; break;
	case 1:  m23 = m; m24 = m; m12 = m; c1 = 1; break;
	case 2:  m23 = m; m12 = m; m14 = m; c1 = 1; break;
	case 3:  m34 = m; m13 = m; m12 = m; c1 = 1; break;
	case 4:  m34 = m; m12 = m; c1 = 1; c3 = 1; break;        // two pairs
	case 5:  m34 = m; m24 = m; m14 = m; c1 = 1; c2 = 1; c3 = 1; break;
	case 6:  m34 = m; c1 = 1; c2 = 1; c3 = 1; break;
	case 7:  c1 = 1; c2 = 1; c3 = 1; c4 = 1; break;
	case 8:  m34 = m; m24 = m; m13 = m; c1 = 1; c2 = 1; break; // Y split
	case 9:  m34 = m; m23 = m; c1 = 1; c2 = 1; break;
	case 10: m14 = m; m13 = m; m12 = m; c1 = 1; break;
	case 11: m34 = m; m24 = m; c1 = 1; c2 = 1; c3 = 1; break;
	case 12: m34 = m; m24 = m; m13 = m; m12 = m; c1 = 1; break;
	case 13: m34 = m; m23 = m; m12 = m; m14 = m; c1 = 1; break;
	case 14: m34 = m; m23 = m; m12 = m; m13 = m; c1 = 1; break;
	case 15: m34 = m; m23 = m; m14 = m; c1 = 1; c2 = 1; break;
	default: break;
	}
	r->mod3_4 = m34;
	r->mod2_3 = m23; r->mod2_4 = m24;
	r->mod1_2 = m12; r->mod1_3 = m13; r->mod1_4 = m14;
	r->car1 = c1; r->car2 = c2; r->car3 = c3; r->car4 = c4;
}

static void DX100Voice_next(DX100Voice* unit, int inNumSamples);

static void DX100Voice_Ctor(DX100Voice* unit) {
	unit->sr = (float)SAMPLERATE;
	unit->recSr = 1.f / unit->sr;
	for (int i = 0; i < 4; ++i) {
		unit->opPhase[i] = 0.f;
		unit->env[i].stage = kIdle;
		unit->env[i].att = kSilentDb;
	}
	unit->fbLast = 0.f;
	unit->fbPrev = 0.f;
	unit->freqLag = IN0(i_hz);
	unit->prevGate = 0.f;
	unit->lfoDelayPos = 0.f;
	unit->pegStage = kPegRest;
	unit->pegInit = 0;
	unit->pegLevel = 0.f;
	for (int s = 0; s < 2; ++s) unit->dz[s][0] = unit->dz[s][1] = 0.f;
	unit->lastOs = 0;
	SETCALC(DX100Voice_next);
	// Do not calc in Ctor: IN0 can be garbage before wires connect.
}

static void DX100Voice_next(DX100Voice* unit, int inNumSamples) {
	float* out = OUT(o_snd);
	float* envOut = OUT(o_env);
	const float recSr = unit->recSr;
	const float sr = unit->sr;

	float hzIn = IN0(i_hz);
	float gate = IN0(i_gate);
	float vel = clip01(IN0(i_vel));
	float legato = clip01(IN0(i_legato));
	int algo = (int)(IN0(i_algo) + 0.5f);
	if (algo < 0) algo = 0;
	if (algo > 15) algo = 15;
	float feedback = IN0(i_feedback);
	if (feedback < 0.f) feedback = 0.f;
	if (feedback > 7.f) feedback = 7.f;
	float dxFeedback = clip01(IN0(i_dxFeedback));
	float transpose = IN0(i_transpose);
	float port = IN0(i_port);
	float portMode = IN0(i_portMode);
	float lfoRaw = IN0(i_lfoIn);
	if (lfoRaw < -1.f) lfoRaw = -1.f;
	if (lfoRaw > 1.f) lfoRaw = 1.f;
	float lfoDelay = clip01(IN0(i_lfoDelay));
	int lfoUni = IN0(i_lfoUni) >= 0.5f;
	float pms = clip01(IN0(i_pms));
	float ams = clip01(IN0(i_ams));
	float alms = IN0(i_alms);
	float pegR[3] = { IN0(i_pr1), IN0(i_pr2), IN0(i_pr3) };
	float pegL[3] = { IN0(i_pl1), IN0(i_pl2), IN0(i_pl3) };
	float breath = clip01(IN0(i_breath));
	float egBias = clip01(IN0(i_egBias));

	float ratio[4] = { IN0(i_r1), IN0(i_r2), IN0(i_r3), IN0(i_r4) };
	float det[4] = { IN0(i_d1), IN0(i_d2), IN0(i_d3), IN0(i_d4) };
	float fixHz[4] = { IN0(i_f1), IN0(i_f2), IN0(i_f3), IN0(i_f4) };
	int fixed[4] = {
		IN0(i_x1) >= 0.5f, IN0(i_x2) >= 0.5f,
		IN0(i_x3) >= 0.5f, IN0(i_x4) >= 0.5f
	};
	int wave[4] = {
		(int)(IN0(i_w1) + 0.5f), (int)(IN0(i_w2) + 0.5f),
		(int)(IN0(i_w3) + 0.5f), (int)(IN0(i_w4) + 0.5f)
	};
	float level[4] = { IN0(i_l1), IN0(i_l2), IN0(i_l3), IN0(i_l4) };
	float atkR[4] = { IN0(i_a1), IN0(i_a2), IN0(i_a3), IN0(i_a4) };
	float d1R[4] = { IN0(i_b1), IN0(i_b2), IN0(i_b3), IN0(i_b4) };
	float d1L[4] = { IN0(i_c1), IN0(i_c2), IN0(i_c3), IN0(i_c4) };
	float d2R[4] = { IN0(i_e1), IN0(i_e2), IN0(i_e3), IN0(i_e4) };
	float relR[4] = { IN0(i_g1), IN0(i_g2), IN0(i_g3), IN0(i_g4) };
	float kls[4] = { IN0(i_k1), IN0(i_k2), IN0(i_k3), IN0(i_k4) };
	float kvs[4] = { IN0(i_v1), IN0(i_v2), IN0(i_v3), IN0(i_v4) };
	int ame[4] = {
		IN0(i_m1) >= 0.5f, IN0(i_m2) >= 0.5f,
		IN0(i_m3) >= 0.5f, IN0(i_m4) >= 0.5f
	};
	float krs[4] = { IN0(i_s1), IN0(i_s2), IN0(i_s3), IN0(i_s4) };
	float ebs[4] = { IN0(i_z1), IN0(i_z2), IN0(i_z3), IN0(i_z4) };
	const int grit = IN0(i_grit) >= 0.5f;
	for (int i = 0; i < 4; ++i) {
		if (wave[i] < 0) wave[i] = 0;
		if (wave[i] > 7) wave[i] = 7;
		if (krs[i] < 0.f) krs[i] = 0.f;
		if (krs[i] > 3.f) krs[i] = 3.f;
	}

	int gateOn = gate >= 0.5f;

	float slide = (portMode >= 0.5f) ? port : port * legato;
	if (slide < 0.f) slide = 0.f;
	if (slide > 5.f) slide = 5.f;

	// ---- block-rate hoists ---------------------------------------------
	const float transposeMul = exp2f_fast(transpose * (1.f / 12.f));
	const float portCoef = (slide <= 0.0005f)
		? 1.f : (1.f - expf_fast(-recSr / slide));
	// LFO delay: hold for D, then fade in over D. D = 10.7 s at max.
	const float delayD = 10.7f * lfoDelay * lfoDelay;
	const float recDelayD = delayD > 1e-4f ? 1.f / delayD : 0.f;
	// PMS 7 + PMD 99 = +-800 cents. Curve so the low range stays usable.
	const float pmsSemis = 8.f * pms * pms * pms;
	// AMS 3 + AMD 99 = 96 dB.
	const float amsDb = kSilentDb * ams * ams;
	// FB 7 = 4 pi on the two-sample average; halves per step.
	const float fbGain = feedback > 0.f
		? 4.f * kPi * exp2f_fast(feedback - 7.f) : 0.f;
	const int dxOn = dxFeedback > 1e-6f;
	const float dxPiAmt = dxFeedback * kPi;
	// Detune: +-3 steps = +-2.6 cents (manual).
	float detMul[4];
	for (int i = 0; i < 4; ++i) {
		float d = det[i];
		if (d < -3.f) d = -3.f;
		if (d > 3.f) d = 3.f;
		detMul[i] = exp2f_fast(d * (0.8667f / 1200.f));
	}

	// Pitch-dependent scaling, from the current (lagged) frequency.
	float scaleFreq = unit->freqLag * transposeMul;
	if (scaleFreq < 8.f) scaleFreq = 8.f;
	if (scaleFreq > 12000.f) scaleFreq = 12000.f;
	// Octaves above C1 (32.7 Hz): the chip's key code runs 0..31 in
	// quarter-octave steps from about there.
	float octC1 = log2f_fast(scaleFreq * (1.f / 32.7032f));
	if (octC1 < 0.f) octC1 = 0.f;
	// Level scaling: nothing below C2, kls * 8 dB per octave above.
	float octC2 = octC1 - 1.f;
	if (octC2 < 0.f) octC2 = 0.f;

	int os = (int)(IN0(i_oversample) + 0.5f);
	if (os < 1) os = 1;
	if (os > 4) os = 4;
	if (os == 3) os = 2;
	const float recSrOs = recSr / (float)os;
	float lpCoef[2][5];
	if (os > 1) {
		makeLowpass(sr * 0.45f, sr * (float)os, lpCoef);
	}
	if (os != unit->lastOs) {
		for (int s = 0; s < 2; ++s) unit->dz[s][0] = unit->dz[s][1] = 0.f;
		unit->lastOs = os;
	}

	// Per-op static attenuation (dB): output level, level scaling,
	// velocity, EG bias. Constant across the block.
	float staticAtt[4];
	int opLive[4];
	for (int i = 0; i < 4; ++i) {
		float a = levelDb(level[i]);
		a += clip01(kls[i]) * 8.f * octC2;
		a += clip01(kvs[i]) * 48.f * (1.f - vel);
		a += clip01(ebs[i]) * egBias * 48.f * (1.f - breath);
		staticAtt[i] = a;
		opLive[i] = a < 90.f;
	}

	// Envelope increments, per op, resolved once per block. Rate scaling:
	// the key code adds to the internal rate, KRS selecting how much
	// (>> 3-KRS). Times shrink by 2^(-oct * 2^(KRS-3)).
	float atkCoef[4], d1Inc[4], d2Inc[4], relInc[4], d1lAtt[4];
	for (int i = 0; i < 4; ++i) {
		float k = (float)((int)(krs[i] + 0.5f));
		float tmul = exp2f_fast(-octC1 * exp2f_fast(k - 3.f));
		float ta = attackTime(atkR[i]) * tmul;
		// exponential approach; T is the time from 96 dB to 0.75 dB
		float tau = ta * (1.f / 4.852f);
		atkCoef[i] = ta < 1e-4f ? 1.f : (1.f - expf_fast(-recSr / tau));
		float t1 = decayTime(d1R[i]);
		d1Inc[i] = t1 > 0.f ? kSilentDb * recSr / (t1 * tmul) : 0.f;
		float t2 = decayTime(d2R[i]);
		d2Inc[i] = t2 > 0.f ? kSilentDb * recSr / (t2 * tmul) : 0.f;
		float tr = releaseTime(relR[i]) * tmul;
		relInc[i] = kSilentDb * recSr / tr;
		d1lAtt[i] = d1lDb(d1L[i]);
	}

	// Pitch EG: semitones per sample toward each level.
	float pegInc[3], pegTarget[3];
	for (int s = 0; s < 3; ++s) {
		float l = pegL[s];
		if (l < -1.f) l = -1.f;
		if (l > 1.f) l = 1.f;
		pegTarget[s] = l * 48.f;
		pegInc[s] = 96.f * recSr / pegTime(pegR[s]);
	}
	if (!unit->pegInit) {
		unit->pegLevel = pegTarget[2];
		unit->pegInit = 1;
	}

	const int algoMod = alms > 1e-6f;
	Routing rt;
	makeRouting(algo, &rt);
	int lastAlgSel = algo;

	float targetHz = hzIn * transposeMul;
	if (targetHz < 8.f) targetHz = 8.f;
	if (targetHz > 12000.f) targetHz = 12000.f;

	for (int n = 0; n < inNumSamples; ++n) {
		int gateEdge = gateOn && unit->prevGate < 0.5f;
		unit->prevGate = gateOn ? 1.f : 0.f;

		if (gateEdge) {
			// Key-on: the chip resets every operator phase, so each
			// note starts with the same waveform alignment.
			for (int i = 0; i < 4; ++i) unit->opPhase[i] = 0.f;
			unit->fbLast = 0.f;
			unit->fbPrev = 0.f;
			unit->lfoDelayPos = 0.f;
			unit->pegStage = kPegL1;
		}
		if (!gateOn && unit->pegStage != kPegRest
				&& unit->pegStage != kPegL3) {
			unit->pegStage = kPegL3;
		}

		if (portCoef >= 1.f) {
			unit->freqLag = targetHz;
		} else {
			unit->freqLag += (targetHz - unit->freqLag) * portCoef;
		}
		float freq = unit->freqLag;

		// pitch EG
		if (unit->pegStage != kPegRest) {
			int s = unit->pegStage - 1;
			float tgt = pegTarget[s];
			float inc = pegInc[s];
			float lv = unit->pegLevel;
			if (lv < tgt) {
				lv += inc;
				if (lv >= tgt) lv = tgt;
			} else if (lv > tgt) {
				lv -= inc;
				if (lv <= tgt) lv = tgt;
			}
			unit->pegLevel = lv;
			if (lv == tgt) {
				if (unit->pegStage == kPegL1) unit->pegStage = kPegL2;
				else unit->pegStage = kPegRest;
			}
		}
		if (unit->pegLevel != 0.f) {
			freq *= exp2f_fast(unit->pegLevel * (1.f / 12.f));
		}

		// envelopes (dB domain)
		for (int i = 0; i < 4; ++i) {
			OpEnv* e = &unit->env[i];
			if (gateOn && (gateEdge || e->stage == kIdle)) {
				e->stage = kAtk;
			} else if (!gateOn && e->stage != kIdle && e->stage != kRel) {
				e->stage = kRel;
			}
			switch (e->stage) {
			case kAtk:
				e->att -= e->att * atkCoef[i];
				if (e->att < 0.1f) {
					e->att = 0.f;
					e->stage = kD1;
				}
				break;
			case kD1:
				e->att += d1Inc[i];
				if (e->att >= d1lAtt[i]) {
					e->att = d1lAtt[i];
					e->stage = kD2;
				}
				break;
			case kD2:
				e->att += d2Inc[i];
				if (e->att >= kSilentDb) {
					e->att = kSilentDb;
					e->stage = kSus;
				}
				break;
			case kRel:
				e->att += relInc[i];
				if (e->att >= kSilentDb) {
					e->att = kSilentDb;
					e->stage = kIdle;
				}
				break;
			default:
				break;
			}
		}

		// LFO: shared value in, per-voice delay/fade here.
		if (gateOn && unit->lfoDelayPos < 2.f) {
			unit->lfoDelayPos += recSr * recDelayD;
			if (unit->lfoDelayPos > 2.f) unit->lfoDelayPos = 2.f;
		}
		float lfoD;
		if (recDelayD == 0.f) {
			lfoD = 1.f;
		} else {
			lfoD = unit->lfoDelayPos - 1.f;
			if (lfoD < 0.f) lfoD = 0.f;
			if (lfoD > 1.f) lfoD = 1.f;
		}
		float lfo = (lfoUni ? lfoRaw * 0.5f + 0.5f : lfoRaw) * lfoD;
		// AM is an attenuation: 0 at the LFO trough, amsDb at its peak.
		float lfo01 = (lfoRaw * 0.5f + 0.5f) * lfoD;
		if (pmsSemis != 0.f) {
			freq *= exp2f_fast(lfo * pmsSemis * (1.f / 12.f));
		}
		float amAtt = lfo01 * amsDb;

		if (algoMod) {
			float a = (float)algo + lfo * alms * 15.f;
			int algSel = (int)floorf(a + 0.5f);
			algSel %= 16;
			if (algSel < 0) algSel += 16;
			if (algSel != lastAlgSel) {
				makeRouting(algSel, &rt);
				lastAlgSel = algSel;
			}
		}

		float oscF[4];
		for (int i = 0; i < 4; ++i) {
			float f = (fixed[i] ? fixHz[i] : freq * ratio[i]) * detMul[i];
			if (f < 0.f) f = 0.f;
			oscF[i] = f;
		}

		// dB -> amplitude, once per op per sample.
		float envS[4];
		for (int i = 0; i < 4; ++i) {
			float att = unit->env[i].att + staticAtt[i];
			if (ame[i]) att += amAtt;
			if (grit) att = floorf(att * (1.f / kEnvStepDb)) * kEnvStepDb;
			envS[i] = (att >= kSilentDb || !opLive[i])
				? 0.f : exp2f_fast(att * kDbToExp2);
		}

		float mix = 0.f;
		for (int k = 0; k < os; ++k) {
			// op4 (feedback source). Feedback uses the mean of the last
			// two outputs, as the chip does; that lowpass is what keeps
			// FB 7 a rough saw rather than noise.
			unit->opPhase[3] = wrap01(unit->opPhase[3] + oscF[3] * recSrOs);
			float fb = (unit->fbLast + unit->fbPrev) * 0.5f * fbGain;
			float pm4 = fb;
			if (dxOn) {
				float dxSeed = sinRad(kTwoPi * unit->opPhase[3] + fb);
				pm4 += dxPiAmt * sinRad(kTwoPi * unit->opPhase[3] + fb
					+ dxPiAmt * dxSeed);
			}
			float out4 = 0.f;
			if (envS[3] > 1e-6f) {
				out4 = opWave(wave[3], unit->opPhase[3], pm4, grit) * envS[3];
			}
			unit->fbPrev = unit->fbLast;
			unit->fbLast = out4;

			// Ops resolve deepest-first so modulation is same-sample.
			unit->opPhase[2] = wrap01(unit->opPhase[2] + oscF[2] * recSrOs);
			float out3 = 0.f;
			if (envS[2] > 1e-6f) {
				out3 = opWave(wave[2], unit->opPhase[2], rt.mod3_4 * out4, grit)
					* envS[2];
			}

			unit->opPhase[1] = wrap01(unit->opPhase[1] + oscF[1] * recSrOs);
			float out2 = 0.f;
			if (envS[1] > 1e-6f) {
				out2 = opWave(wave[1], unit->opPhase[1],
						rt.mod2_3 * out3 + rt.mod2_4 * out4, grit)
					* envS[1];
			}

			unit->opPhase[0] = wrap01(unit->opPhase[0] + oscF[0] * recSrOs);
			float out1 = 0.f;
			if (envS[0] > 1e-6f) {
				out1 = opWave(wave[0], unit->opPhase[0],
						rt.mod1_2 * out2 + rt.mod1_3 * out3
						+ rt.mod1_4 * out4, grit)
					* envS[0];
			}

			mix = rt.car1 * out1 + rt.car2 * out2
				+ rt.car3 * out3 + rt.car4 * out4;
			if (os > 1) {
				mix = biquad(mix, lpCoef[0], unit->dz[0]);
				mix = biquad(mix, lpCoef[1], unit->dz[1]);
			}
		}
		out[n] = mix;
	}

	// Linear sum of op amplitudes for the free test; A2K reads index 0.
	float envSum = 0.f;
	for (int i = 0; i < 4; ++i) {
		float att = unit->env[i].att + staticAtt[i];
		envSum += (att >= kSilentDb) ? 0.f : exp2f_fast(att * kDbToExp2);
	}
	for (int n = 0; n < inNumSamples; ++n) {
		envOut[n] = envSum;
	}
}

PluginLoad(DX100Voice) {
	ft = inTable;
	DefineSimpleUnit(DX100Voice);
}
