// DX100Voice — one 4-op FM voice as a single UGen.
// Waves, algorithms, feedback, envelopes, LFO in C++ so unused
// branches are not computed (SC Select cannot skip).
//
// Performance note: the inner loop must stay free of libm transcendentals.
// sinf/powf/expf on the norns CPU (Cortex-A53, no fast transcendental unit)
// cost 50-150 cycles each; the first version of this UGen ran ~28 of them
// per sample per voice, which is why it was slower than the SC graph it
// replaced (SinOsc is a table lookup, and the .kr parts of that graph ran
// once per 64 samples). So here:
//   - sin comes from a 4096-entry linear-interpolated table  (-123 dB)
//   - 2^x comes from exponent-bit assembly + quintic mantissa fit
//     (max 0.0004 cents error)
//   - envelope segments run as a one-multiply exponential recursion
//   - everything derived only from scalar-rate IN0 is hoisted to block rate

#include "SC_PlugIn.h"
#include <math.h>
#include <stdint.h>

static InterfaceTable* ft;

static const float kPi = 3.14159265358979323846f;
static const float kTwoPi = 6.28318530717958647692f;
static const float kModIndex = 7.0f;

// ---- sine table -------------------------------------------------------
#define kSineBits 12
#define kSineSize (1 << kSineBits)
#define kSineMask (kSineSize - 1)

// Filled by a static initializer, not by PluginLoad: the table must be
// valid for any translation unit that pulls this in (offline test harness
// included), and it must never depend on load-order.
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

// sin of a radian argument, for phase-modulated lookups.
static inline float sinRad(float x) { return sinTab(x * (1.f / kTwoPi)); }

// ---- fast 2^x ---------------------------------------------------------
// Assemble the exponent by hand, fit the mantissa on [0,1) with a quintic.
// Max 2.4e-7 relative == 0.0004 cents, far below audibility for pitch.
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
// Pull the exponent, fit log2 of the mantissa on [1,2). Max 5.7e-6 abs.
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
	i_ttrig,
	i_algo,
	i_feedback,
	i_dxFeedback,
	i_transpose,
	i_port,
	i_portMode,
	i_lfoRate,
	i_lfoWave,
	i_lfoDelay,
	i_lfoOneshot,
	i_lfoUni,
	i_pms,
	i_ams,
	i_alms,
	i_pegAmt,
	i_pegRate,
	i_pegLevel,
	i_r1, i_r2, i_r3, i_r4,
	i_d1, i_d2, i_d3, i_d4,
	i_f1, i_f2, i_f3, i_f4,
	i_x1, i_x2, i_x3, i_x4,
	i_w1, i_w2, i_w3, i_w4,
	i_l1, i_l2, i_l3, i_l4,
	i_a1, i_a2, i_a3, i_a4,
	i_b1, i_b2, i_b3, i_b4,
	i_c1, i_c2, i_c3, i_c4,
	i_e1, i_e2, i_e3, i_e4,
	i_g1, i_g2, i_g3, i_g4,
	i_k1, i_k2, i_k3, i_k4,
	i_v1, i_v2, i_v3, i_v4,
	i_rateScale,
	i_numInputs
};

enum { o_snd = 0, o_env, o_numOutputs };

enum EnvStage { kIdle, kAtk, kD1, kD2, kSus, kRel };

// A segment runs as an exponential recursion: `ep` decays from 1 toward 0
// by one multiply per sample, and level maps off it. This replaces the two
// expf calls that curvePos() used to make per op per sample.
//   linear   (curve ~ 0): level = start + (target-start) * pos
//   exponential        : level = target + (start-target) * ep
struct OpEnv {
	int stage;
	float level;
	float start;
	float target;
	float pos;      // 0..1 linear progress, drives segment-done test
	float posInc;   // per-sample increment of pos
	float ep;       // exponential state, 1 -> ~0 across the segment
	float epMul;    // per-sample multiplier for ep
	float epScale;  // 1/(1-epEnd), normalizes ep so the segment lands exactly
	float epEnd;    // ep value at segment end
	int isExp;
};

struct DX100Voice : public Unit {
	float sr;
	float recSr;
	float opPhase[4];
	float fbLast;
	float freqLag;
	float prevGate;
	float prevTrig;
	float lfoPhase;
	float lfoSh;
	float lfoDelayPos;
	float pegPos;
	float pegLevel;
	uint32_t rng;
	OpEnv env[4];
	int oneshotDone;
};

static float linexp(float x, float x0, float x1, float y0, float y1) {
	if (x <= x0) return y0;
	if (x >= x1) return y1;
	float t = (x - x0) / (x1 - x0);
	return y0 * powf(y1 / y0, t);
}

static float linlin(float x, float x0, float x1, float y0, float y1) {
	if (x <= x0) return y0;
	if (x >= x1) return y1;
	return y0 + (x - x0) * (y1 - y0) / (x1 - x0);
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

static float rateTime(float rate, float lo, float hi) {
	rate = clip01(rate);
	return linexp(rate, 0.f, 1.f, hi, lo);
}

// Called on segment boundaries only (a few times per note), so the libm
// calls here are fine — they are not in the per-sample path.
static void envStartSeg(OpEnv* e, float start, float target, float dur,
		float curve, float recSr) {
	e->start = start;
	e->target = target;
	e->level = start;
	e->pos = 0.f;
	if (dur < 1e-6f) dur = 1e-6f;
	e->posInc = recSr / dur;
	if (fabsf(curve) < 0.001f) {
		e->isExp = 0;
		e->ep = 1.f;
		e->epMul = 1.f;
		e->epScale = 1.f;
		e->epEnd = 0.f;
	} else {
		// SC's Env curve shape: (1 - exp(pos*curve)) / (1 - exp(curve)).
		// Equivalent to an exponential from start to target, so run
		// exp(pos*curve) as a recursion and normalize the endpoints.
		e->isExp = 1;
		e->ep = 1.f;
		e->epMul = expf(curve * e->posInc);
		e->epEnd = expf(curve);
		float denom = 1.f - e->epEnd;
		e->epScale = (fabsf(denom) < 1e-8f) ? 0.f : (1.f / denom);
	}
}

static inline float envTick(OpEnv* e) {
	if (e->stage == kIdle) {
		e->level = 0.f;
		return 0.f;
	}
	if (e->stage == kSus) {
		return e->level;
	}
	e->pos += e->posInc;
	if (e->pos >= 1.f) {
		e->pos = 1.f;
		e->level = e->target;
		return e->level;
	}
	if (e->isExp) {
		e->ep *= e->epMul;
		// shape = (1 - ep) / (1 - epEnd), 0 at segment start, 1 at end
		float shape = (1.f - e->ep) * e->epScale;
		e->level = e->start + (e->target - e->start) * shape;
	} else {
		e->level = e->start + (e->target - e->start) * e->pos;
	}
	return e->level;
}

static inline int envSegDone(const OpEnv* e) { return e->pos >= 1.f; }

// phase in turns; pm in radians.
static inline float opWave(int wave, float phase, float pm) {
	float pmTurns = pm * (1.f / kTwoPi);
	float s = sinTab(phase + pmTurns);
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

static inline float lfoRaw(int wave, float p, float sh) {
	switch (wave) {
	case 0: // tri
		if (p < 0.25f) return p * 4.f;
		if (p < 0.75f) return 2.f - p * 4.f;
		return p * 4.f - 4.f;
	case 1: return sinTab(p);
	case 2: return p < 0.5f ? 1.f : -1.f;
	case 3: return sh;
	case 4: return p * 2.f - 1.f;
	case 5: return 1.f - p * 2.f;
	default: return 0.f;
	}
}

// Routing for one algorithm, as coefficients rather than a switch run
// four times per sample. m[i][j] = how much op j modulates op i.
// car[i] = op i is a carrier. Resolved once per block (or per sample only
// when ALMS is sweeping the algorithm).
struct Routing {
	float mod3_4;              // op4 -> op3
	float mod2_3, mod2_4;      // -> op2
	float mod1_2, mod1_3, mod1_4;  // -> op1
	float car1, car2, car3, car4;
	float norm;                // 1/sqrt(number of carriers)
};

static const float kInvSqrt[5] = {
	1.f, 1.f, 0.70710678f, 0.57735027f, 0.5f
};

static void makeRouting(int algo, Routing* r) {
	const float m = kModIndex;
	float m34 = 0, m23 = 0, m24 = 0, m12 = 0, m13 = 0, m14 = 0;
	float c1 = 1, c2 = 0, c3 = 0, c4 = 0;
	int n = 1;
	switch (algo) {
	case 0:  m34 = m; m23 = m; m12 = m; c1 = 1; n = 1; break;
	case 1:  m23 = m; m24 = m; m12 = m; c1 = 1; n = 1; break;
	case 2:  m23 = m; m12 = m; m14 = m; c1 = 1; n = 1; break;
	case 3:  m34 = m; m13 = m; m12 = m; c1 = 1; n = 1; break;
	case 4:  m34 = m; m24 = m; m13 = m; c1 = 1; c2 = 1; n = 2; break;
	case 5:  m34 = m; m24 = m; m14 = m; c1 = 1; c2 = 1; c3 = 1; n = 3; break;
	case 6:  m34 = m; c1 = 1; c2 = 1; c3 = 1; n = 3; break;
	case 7:  c1 = 1; c2 = 1; c3 = 1; c4 = 1; n = 4; break;
	case 8:  m34 = m; m12 = m; c1 = 1; c3 = 1; n = 2; break;
	case 9:  m34 = m; m23 = m; c1 = 1; c2 = 1; n = 2; break;
	case 10: m14 = m; m13 = m; m12 = m; c1 = 1; n = 1; break;
	case 11: m34 = m; m24 = m; c1 = 1; c2 = 1; c3 = 1; n = 3; break;
	case 12: m34 = m; m24 = m; m13 = m; m12 = m; c1 = 1; n = 1; break;
	case 13: m34 = m; m23 = m; m12 = m; m14 = m; c1 = 1; n = 1; break;
	case 14: m34 = m; m23 = m; m12 = m; m13 = m; c1 = 1; n = 1; break;
	case 15: m34 = m; m23 = m; m14 = m; c1 = 1; c2 = 1; n = 2; break;
	default: break;
	}
	r->mod3_4 = m34;
	r->mod2_3 = m23; r->mod2_4 = m24;
	r->mod1_2 = m12; r->mod1_3 = m13; r->mod1_4 = m14;
	r->car1 = c1; r->car2 = c2; r->car3 = c3; r->car4 = c4;
	r->norm = kInvSqrt[n < 0 ? 0 : (n > 4 ? 4 : n)];
}

static void DX100Voice_next(DX100Voice* unit, int inNumSamples);

static void DX100Voice_Ctor(DX100Voice* unit) {
	unit->sr = (float)SAMPLERATE;
	unit->recSr = 1.f / unit->sr;
	for (int i = 0; i < 4; ++i) {
		unit->opPhase[i] = 0.f;
		unit->env[i].stage = kIdle;
		unit->env[i].level = 0.f;
		unit->env[i].pos = 1.f;
		unit->env[i].posInc = 1.f;
		unit->env[i].ep = 0.f;
		unit->env[i].epMul = 1.f;
		unit->env[i].epScale = 1.f;
		unit->env[i].epEnd = 0.f;
		unit->env[i].isExp = 0;
	}
	unit->fbLast = 0.f;
	unit->freqLag = IN0(i_hz);
	unit->prevGate = 0.f;
	unit->prevTrig = 0.f;
	unit->lfoPhase = 0.f;
	unit->lfoSh = 0.f;
	unit->lfoDelayPos = 0.f;
	unit->pegPos = 1.f;
	unit->pegLevel = 0.f;
	unit->rng = 1u;
	unit->oneshotDone = 0;
	SETCALC(DX100Voice_next);
	// Do not calc in Ctor: IN0 can be garbage before wires connect,
	// which left envelopes idle and every operator silent.
}

static void DX100Voice_next(DX100Voice* unit, int inNumSamples) {
	float* out = OUT(o_snd);
	float* envOut = OUT(o_env);
	const float recSr = unit->recSr;

	float hzIn = IN0(i_hz);
	float gate = IN0(i_gate);
	float vel = clip01(IN0(i_vel));
	float legato = clip01(IN0(i_legato));
	float ttrig = IN0(i_ttrig);
	int algo = (int)(IN0(i_algo) + 0.5f);
	if (algo < 0) algo = 0;
	if (algo > 15) algo = 15;
	float feedback = clip01(IN0(i_feedback));
	float dxFeedback = clip01(IN0(i_dxFeedback));
	float transpose = IN0(i_transpose);
	float port = IN0(i_port);
	float portMode = IN0(i_portMode);
	float lfoRate = IN0(i_lfoRate);
	if (lfoRate < 0.02f) lfoRate = 0.02f;
	if (lfoRate > 60.f) lfoRate = 60.f;
	int lfoWave = (int)(IN0(i_lfoWave) + 0.5f);
	if (lfoWave < 0) lfoWave = 0;
	if (lfoWave > 5) lfoWave = 5;
	float lfoDelay = clip01(IN0(i_lfoDelay));
	int lfoOneshot = IN0(i_lfoOneshot) >= 0.5f;
	int lfoUni = IN0(i_lfoUni) >= 0.5f;
	float pms = IN0(i_pms);
	float ams = IN0(i_ams);
	float alms = IN0(i_alms);
	float pegAmt = IN0(i_pegAmt);
	float pegRate = clip01(IN0(i_pegRate));
	float pegLevP = IN0(i_pegLevel);
	float rateScale = IN0(i_rateScale);

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
	float ks[4] = { IN0(i_k1), IN0(i_k2), IN0(i_k3), IN0(i_k4) };
	float vs[4] = { IN0(i_v1), IN0(i_v2), IN0(i_v3), IN0(i_v4) };
	for (int i = 0; i < 4; ++i) {
		if (wave[i] < 0) wave[i] = 0;
		if (wave[i] > 7) wave[i] = 7;
		level[i] = level[i] < 0.f ? 0.f : level[i];
		d1L[i] = clip01(d1L[i]);
	}

	int gateOn = gate >= 0.5f;
	int trigEdge = ttrig > 0.f && unit->prevTrig <= 0.f;
	unit->prevTrig = ttrig;

	float slide = (portMode >= 0.5f) ? port : port * legato;
	if (slide < 0.f) slide = 0.f;
	if (slide > 5.f) slide = 5.f;

	float delayTime = linlin(lfoDelay, 0.f, 1.f, 0.001f, 4.f);
	float pegDur = linexp(pegRate, 0.f, 1.f, 4.f, 0.004f);

	// ---- block-rate hoists ---------------------------------------------
	// All of these depend only on scalar-rate inputs, so they are constant
	// across the block. Computing them per sample was the bulk of the cost.
	const float transposeMul = exp2f_fast(transpose * (1.f / 12.f));
	const float portCoef = (slide <= 0.0005f)
		? 1.f : (1.f - expf_fast(-recSr / slide));
	const float recDelayTime = 1.f / delayTime;
	const float recPegDur = 1.f / pegDur;
	const float lfoInc = lfoRate * recSr;
	const float pmsScale = pms * 0.0833f;
	const float fbGain = feedback * 6.f;
	const int dxOn = dxFeedback > 1e-6f;
	const float dxPiAmt = dxFeedback * kPi;
	float detMul[4];
	for (int i = 0; i < 4; ++i) {
		detMul[i] = exp2f_fast(det[i] * (1.f / 1200.f));
	}
	float velScale[4];
	for (int i = 0; i < 4; ++i) {
		velScale[i] = 1.f - vs[i] * (1.f - vel);
	}
	// Which ops can produce sound at all this block. Skips both the
	// oscillator and its wave switch when an op is muted.
	int opLive[4];
	for (int i = 0; i < 4; ++i) {
		opLive[i] = level[i] > 1e-5f;
	}

	// Key scaling / rate scaling follow the note's pitch, which only moves
	// on portamento, PEG or LFO pitch mod. Resolve once per block from the
	// current lagged frequency: these are scaling coefficients, not audio,
	// and a 64-sample staircase on them is inaudible.
	float scaleFreq = unit->freqLag * transposeMul;
	if (scaleFreq < 8.f) scaleFreq = 8.f;
	if (scaleFreq > 12000.f) scaleFreq = 12000.f;
	const float keyTrack = log2f_fast(scaleFreq > 13.0815f
		? scaleFreq * (1.f / 261.63f) : 0.05f);
	const float scl = exp2f_fast(keyTrack * (-rateScale) * 0.5f);
	float kScale[4];
	for (int i = 0; i < 4; ++i) {
		kScale[i] = exp2f_fast(keyTrack * (-ks[i]));
	}
	// Envelope segment times, per op, resolved once per block.
	float atkT[4], d1T[4], d2T[4], relT[4];
	for (int i = 0; i < 4; ++i) {
		atkT[i] = rateTime(atkR[i], 0.0008f, 6.f) * scl;
		d1T[i] = rateTime(d1R[i], 0.004f, 12.f) * scl;
		d2T[i] = rateTime(d2R[i], 0.01f, 40.f) * scl;
		relT[i] = rateTime(relR[i], 0.004f, 10.f) * scl;
	}

	// Algorithm routing. Constant for the block unless ALMS sweeps it.
	const int algoMod = alms > 1e-6f;
	Routing rt;
	makeRouting(algo, &rt);
	int lastAlgSel = algo;

	if (gateOn && unit->prevGate < 0.5f) {
		unit->lfoDelayPos = 0.f;
		unit->pegPos = 0.f;
		if (lfoOneshot) {
			unit->lfoPhase = 0.f;
			unit->oneshotDone = 0;
			unit->rng = unit->rng * 1664525u + 1013904223u;
			unit->lfoSh = (unit->rng / 2147483648.f) - 1.f;
		}
	}
	if (trigEdge && lfoOneshot) {
		unit->lfoPhase = 0.f;
		unit->oneshotDone = 0;
		unit->rng = unit->rng * 1664525u + 1013904223u;
		unit->lfoSh = (unit->rng / 2147483648.f) - 1.f;
	}

	float targetHz = hzIn * transposeMul;
	if (targetHz < 8.f) targetHz = 8.f;
	if (targetHz > 12000.f) targetHz = 12000.f;

	for (int n = 0; n < inNumSamples; ++n) {
		int gateEdge = gateOn && unit->prevGate < 0.5f;
		unit->prevGate = gateOn ? 1.f : 0.f;

		if (portCoef >= 1.f) {
			unit->freqLag = targetHz;
		} else {
			unit->freqLag += (targetHz - unit->freqLag) * portCoef;
		}
		float freq = unit->freqLag;

		if (gateOn && unit->pegPos < 1.f) {
			unit->pegPos += recSr * recPegDur;
			if (unit->pegPos > 1.f) unit->pegPos = 1.f;
		}
		float peg = (1.f - unit->pegPos) * pegLevP;
		if (peg != 0.f && pegAmt != 0.f) {
			freq *= exp2f_fast(peg * pegAmt * 2.f);
		}

		// envelopes
		for (int i = 0; i < 4; ++i) {
			OpEnv* e = &unit->env[i];
			// gate held but still idle: missed the edge (Ctor IN0 garbage).
			if (gateOn && (gateEdge || e->stage == kIdle)) {
				e->stage = kAtk;
				envStartSeg(e, 0.f, 1.f, atkT[i], 0.f, recSr);
			} else if (!gateOn && e->stage != kIdle && e->stage != kRel) {
				e->stage = kRel;
				envStartSeg(e, e->level, 0.f, relT[i], -4.f, recSr);
			}
			envTick(e);
			if (envSegDone(e)) {
				if (e->stage == kAtk) {
					e->stage = kD1;
					envStartSeg(e, 1.f, d1L[i], d1T[i], -4.f, recSr);
				} else if (e->stage == kD1) {
					e->stage = kD2;
					envStartSeg(e, d1L[i], 0.f, d2T[i], -4.f, recSr);
				} else if (e->stage == kD2) {
					e->stage = kSus;
					e->level = 0.f;
				} else if (e->stage == kRel) {
					e->stage = kIdle;
					e->level = 0.f;
				}
			}
		}

		// LFO
		if (lfoOneshot) {
			if (!unit->oneshotDone) {
				unit->lfoPhase += lfoInc;
				if (unit->lfoPhase >= 1.f) {
					unit->lfoPhase = 1.f;
					unit->oneshotDone = 1;
				}
			}
		} else {
			unit->lfoPhase = wrap01(unit->lfoPhase + lfoInc);
			// sample & hold: resample on phase wrap
			if (lfoWave == 3 && unit->lfoPhase < lfoInc) {
				unit->rng = unit->rng * 1664525u + 1013904223u;
				unit->lfoSh = (unit->rng / 2147483648.f) - 1.f;
			}
		}
		if (gateOn && unit->lfoDelayPos < 1.f) {
			unit->lfoDelayPos += recSr * recDelayTime;
			if (unit->lfoDelayPos > 1.f) unit->lfoDelayPos = 1.f;
		}
		float lfoD = gateOn ? unit->lfoDelayPos : 0.f;
		float raw = lfoRaw(lfoWave, unit->lfoPhase, unit->lfoSh);
		float lfo = (lfoUni ? raw * 0.5f + 0.5f : raw) * lfoD;
		float lfo01 = lfoUni ? lfo : lfo * 0.5f + 0.5f;
		if (lfo01 < 0.f) lfo01 = 0.f;
		if (lfo01 > 1.f) lfo01 = 1.f;
		if (pms != 0.f) {
			freq *= exp2f_fast(lfo * pmsScale);
		}
		float amod = 1.f - (lfo01 * ams);

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

		float envS[4], envSum = 0.f;
		for (int i = 0; i < 4; ++i) {
			envS[i] = unit->env[i].level * velScale[i] * kScale[i];
			envSum += envS[i];
		}

		// op4 (feedback source)
		unit->opPhase[3] = wrap01(unit->opPhase[3] + oscF[3] * recSr);
		float fb = unit->fbLast * fbGain;
		float pm4 = fb;
		if (dxOn) {
			float dxSeed = sinRad(kTwoPi * unit->opPhase[3] + fb);
			pm4 += dxPiAmt * sinRad(kTwoPi * unit->opPhase[3] + fb
				+ dxPiAmt * dxSeed);
		}
		float out4 = 0.f;
		if (opLive[3] && envS[3] > 1e-6f) {
			out4 = opWave(wave[3], unit->opPhase[3], pm4)
				* envS[3] * level[3] * amod;
		}
		unit->fbLast = out4;
		if (unit->fbLast > 1.f) unit->fbLast = 1.f;
		if (unit->fbLast < -1.f) unit->fbLast = -1.f;

		// Ops resolve deepest-first so modulation is same-sample, as on
		// the original hardware.
		unit->opPhase[2] = wrap01(unit->opPhase[2] + oscF[2] * recSr);
		float out3 = 0.f;
		if (opLive[2] && envS[2] > 1e-6f) {
			out3 = opWave(wave[2], unit->opPhase[2], rt.mod3_4 * out4)
				* envS[2] * level[2] * amod;
		}

		unit->opPhase[1] = wrap01(unit->opPhase[1] + oscF[1] * recSr);
		float out2 = 0.f;
		if (opLive[1] && envS[1] > 1e-6f) {
			out2 = opWave(wave[1], unit->opPhase[1],
					rt.mod2_3 * out3 + rt.mod2_4 * out4)
				* envS[1] * level[1] * amod;
		}

		unit->opPhase[0] = wrap01(unit->opPhase[0] + oscF[0] * recSr);
		float out1 = 0.f;
		if (opLive[0] && envS[0] > 1e-6f) {
			out1 = opWave(wave[0], unit->opPhase[0],
					rt.mod1_2 * out2 + rt.mod1_3 * out3 + rt.mod1_4 * out4)
				* envS[0] * level[0] * amod;
		}

		float mix = rt.car1 * out1 + rt.car2 * out2
			+ rt.car3 * out3 + rt.car4 * out4;
		out[n] = mix * rt.norm;
		envOut[n] = envSum;
	}
}

PluginLoad(DX100Voice) {
	ft = inTable;
	DefineSimpleUnit(DX100Voice);
}
