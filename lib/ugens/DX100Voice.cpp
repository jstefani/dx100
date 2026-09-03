// DX100Voice — one 4-op FM voice as a single UGen.
// Waves, algorithms, feedback, envelopes, LFO in C++ so unused
// branches are not computed (SC Select cannot skip).

#include "SC_PlugIn.h"
#include <math.h>
#include <stdint.h>

static InterfaceTable* ft;

static const float kPi = 3.14159265358979323846f;
static const float kTwoPi = 6.28318530717958647692f;
static const float kModIndex = 7.0f;

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

struct OpEnv {
	int stage;
	float level;
	float start;
	float target;
	float pos;
	float dur;
	float curve;
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

// SC Env curve: pos 0..1
static float curvePos(float pos, float curve) {
	if (fabsf(curve) < 0.001f) return pos;
	float denom = 1.f - expf(curve);
	if (fabsf(denom) < 1e-8f) return pos;
	return (1.f - expf(pos * curve)) / denom;
}

static float rateTime(float rate, float lo, float hi) {
	rate = clip01(rate);
	return linexp(rate, 0.f, 1.f, hi, lo);
}

static void envStartSeg(OpEnv* e, float start, float target, float dur, float curve) {
	e->start = start;
	e->target = target;
	e->level = start;
	e->pos = 0.f;
	e->dur = dur > 1e-6f ? dur : 1e-6f;
	e->curve = curve;
}

static float envTick(OpEnv* e, float recSr) {
	if (e->stage == kIdle) {
		e->level = 0.f;
		return 0.f;
	}
	if (e->stage == kSus) {
		return e->level;
	}
	e->pos += recSr / e->dur;
	if (e->pos >= 1.f) {
		e->pos = 1.f;
		e->level = e->target;
		return e->level;
	}
	e->level = e->start + (e->target - e->start) * curvePos(e->pos, e->curve);
	return e->level;
}

static int envSegDone(const OpEnv* e) { return e->pos >= 1.f; }

static float opWave(int wave, float phase, float pm) {
	float s = sinf(kTwoPi * phase + pm);
	switch (wave) {
	case 0: return s;
	case 1: return s > 0.f ? s : 0.f;
	case 2: return fabsf(s);
	case 3: {
		float pulse = phase < 0.5f ? 1.f : 0.f;
		return (s > 0.f ? s : 0.f) * pulse;
	}
	case 4: {
		float s2 = sinf(kTwoPi * 2.f * phase + pm);
		return s2 * (phase < 0.5f ? 1.f : 0.f);
	}
	case 5: {
		float s2 = sinf(kTwoPi * 2.f * phase + pm);
		float alt = s2 * (phase < 0.5f ? 1.f : 0.f);
		return alt > 0.f ? alt : 0.f;
	}
	case 6: return fabsf(s) * s;
	case 7: {
		float s2 = sinf(kTwoPi * 2.f * phase + pm);
		float s3 = sinf(kTwoPi * 3.f * phase + pm);
		return (s + s2 * 0.5f + s3 * 0.333f) * 0.6f;
	}
	default: return s;
	}
}

static float lfoRaw(int wave, float p, float sh) {
	switch (wave) {
	case 0: // tri
		if (p < 0.25f) return p * 4.f;
		if (p < 0.75f) return 2.f - p * 4.f;
		return p * 4.f - 4.f;
	case 1: return sinf(p * kTwoPi);
	case 2: return p < 0.5f ? 1.f : -1.f;
	case 3: return sh;
	case 4: return p * 2.f - 1.f;
	case 5: return 1.f - p * 2.f;
	default: return 0.f;
	}
}

// modulator into op3, op2, op1 and carrier mix, algo 0..15
static void algoTick(int algo, float o1, float o2, float o3, float o4,
		float* pm3, float* pm2, float* pm1, float* mix, float* nCar) {
	const float m = kModIndex;
	float z = 0.f;
	float p3 = z, p2 = z, p1 = z, c = o1, n = 1.f;
	switch (algo) {
	case 0: p3 = o4 * m; p2 = o3 * m; p1 = o2 * m; c = o1; n = 1.f; break;
	case 1: p3 = z; p2 = (o3 + o4) * m; p1 = o2 * m; c = o1; n = 1.f; break;
	case 2: p3 = z; p2 = o3 * m; p1 = (o2 + o4) * m; c = o1; n = 1.f; break;
	case 3: p3 = o4 * m; p2 = z; p1 = (o3 + o2) * m; c = o1; n = 1.f; break;
	case 4: p3 = o4 * m; p2 = o4 * m; p1 = o3 * m; c = o1 + o2; n = 2.f; break;
	case 5: p3 = o4 * m; p2 = o4 * m; p1 = o4 * m; c = o1 + o2 + o3; n = 3.f; break;
	case 6: p3 = o4 * m; p2 = z; p1 = z; c = o1 + o2 + o3; n = 3.f; break;
	case 7: p3 = z; p2 = z; p1 = z; c = o1 + o2 + o3 + o4; n = 4.f; break;
	case 8: p3 = o4 * m; p2 = z; p1 = o2 * m; c = o1 + o3; n = 2.f; break;
	case 9: p3 = o4 * m; p2 = o3 * m; p1 = z; c = o1 + o2; n = 2.f; break;
	case 10: p3 = z; p2 = z; p1 = (o4 + o3 + o2) * m; c = o1; n = 1.f; break;
	case 11: p3 = o4 * m; p2 = o4 * m; p1 = z; c = o1 + o2 + o3; n = 3.f; break;
	case 12: p3 = o4 * m; p2 = o4 * m; p1 = (o3 + o2) * m; c = o1; n = 1.f; break;
	case 13: p3 = o4 * m; p2 = o3 * m; p1 = (o2 + o4) * m; c = o1; n = 1.f; break;
	case 14: p3 = o4 * m; p2 = o3 * m; p1 = (o2 + o3) * m; c = o1; n = 1.f; break;
	case 15: p3 = o4 * m; p2 = o3 * m; p1 = o4 * m; c = o1 + o2; n = 2.f; break;
	default: break;
	}
	*pm3 = p3;
	*pm2 = p2;
	*pm1 = p1;
	*mix = c;
	*nCar = n;
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
	DX100Voice_next(unit, 1);
}

static void DX100Voice_next(DX100Voice* unit, int inNumSamples) {
	float* out = OUT(o_snd);
	float* envOut = OUT(o_env);

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
	int gateEdge = gateOn && unit->prevGate < 0.5f;
	int trigEdge = ttrig > 0.f && unit->prevTrig <= 0.f;
	unit->prevGate = gate;
	unit->prevTrig = ttrig;

	float slide = (portMode >= 0.5f) ? port : port * legato;
	if (slide < 0.f) slide = 0.f;
	if (slide > 5.f) slide = 5.f;

	float delayTime = linlin(lfoDelay, 0.f, 1.f, 0.001f, 4.f);
	float pegDur = linexp(pegRate, 0.f, 1.f, 4.f, 0.004f);

	if (gateEdge) {
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

	for (int n = 0; n < inNumSamples; ++n) {
		float targetHz = hzIn * powf(2.f, transpose / 12.f);
		if (targetHz < 8.f) targetHz = 8.f;
		if (targetHz > 12000.f) targetHz = 12000.f;
		if (slide <= 0.0005f) {
			unit->freqLag = targetHz;
		} else {
			float a = 1.f - expf(-unit->recSr / slide);
			unit->freqLag += (targetHz - unit->freqLag) * a;
		}
		float freq = unit->freqLag;

		if (gateOn && unit->pegPos < 1.f) {
			unit->pegPos += unit->recSr / pegDur;
			if (unit->pegPos > 1.f) unit->pegPos = 1.f;
		}
		float peg = (1.f - unit->pegPos) * pegLevP;
		freq *= powf(2.f, peg * pegAmt * 2.f);

		float keyTrack = log2f(fmaxf(freq / 261.63f, 0.05f));
		float scl = powf(2.f, keyTrack * (-rateScale) * 0.5f);

		// envelopes
		for (int i = 0; i < 4; ++i) {
			OpEnv* e = &unit->env[i];
			if (gateEdge) {
				e->stage = kAtk;
				envStartSeg(e, 0.f, 1.f, rateTime(atkR[i], 0.0008f, 6.f) * scl, 0.f);
			} else if (!gateOn && e->stage != kIdle && e->stage != kRel) {
				e->stage = kRel;
				envStartSeg(e, e->level, 0.f, rateTime(relR[i], 0.004f, 10.f) * scl, -4.f);
			}
			envTick(e, unit->recSr);
			if (envSegDone(e)) {
				if (e->stage == kAtk) {
					e->stage = kD1;
					envStartSeg(e, 1.f, d1L[i], rateTime(d1R[i], 0.004f, 12.f) * scl, -4.f);
				} else if (e->stage == kD1) {
					e->stage = kD2;
					envStartSeg(e, d1L[i], 0.f, rateTime(d2R[i], 0.01f, 40.f) * scl, -4.f);
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
				unit->lfoPhase += lfoRate * unit->recSr;
				if (unit->lfoPhase >= 1.f) {
					unit->lfoPhase = 1.f;
					unit->oneshotDone = 1;
				}
			}
		} else {
			unit->lfoPhase = wrap01(unit->lfoPhase + lfoRate * unit->recSr);
			if (lfoWave == 3) {
				float prev = unit->lfoPhase - lfoRate * unit->recSr;
				if (prev < 0.f || floorf(prev * 1.f) != floorf(unit->lfoPhase)) {
					// impulse at wrap
				}
				// sample on phase wrap
				if (unit->lfoPhase < lfoRate * unit->recSr) {
					unit->rng = unit->rng * 1664525u + 1013904223u;
					unit->lfoSh = (unit->rng / 2147483648.f) - 1.f;
				}
			}
		}
		if (gateOn && unit->lfoDelayPos < 1.f) {
			unit->lfoDelayPos += unit->recSr / delayTime;
			if (unit->lfoDelayPos > 1.f) unit->lfoDelayPos = 1.f;
		}
		float lfoD = gateOn ? unit->lfoDelayPos : 0.f;
		float raw = lfoRaw(lfoWave, unit->lfoPhase, unit->lfoSh);
		float lfo = (lfoUni ? raw * 0.5f + 0.5f : raw) * lfoD;
		float lfo01 = lfoUni ? lfo : lfo * 0.5f + 0.5f;
		if (lfo01 < 0.f) lfo01 = 0.f;
		if (lfo01 > 1.f) lfo01 = 1.f;
		float pmod = powf(2.f, lfo * pms * 0.0833f);
		float amod = 1.f - (lfo01 * ams);
		freq *= pmod;

		int algSel = algo;
		if (alms > 1e-6f) {
			float a = (float)algo + lfo * alms * 15.f;
			algSel = (int)floorf(a + 0.5f);
			algSel %= 16;
			if (algSel < 0) algSel += 16;
		}

		float oscF[4];
		for (int i = 0; i < 4; ++i) {
			float f = fixed[i] ? fixHz[i] : freq * ratio[i];
			f *= powf(2.f, det[i] / 1200.f);
			if (f < 0.f) f = 0.f;
			oscF[i] = f;
		}

		float envS[4], envSum = 0.f;
		for (int i = 0; i < 4; ++i) {
			float vScale = 1.f - vs[i] * (1.f - vel);
			float kScale = powf(2.f, keyTrack * (-ks[i]));
			envS[i] = unit->env[i].level * vScale * kScale;
			envSum += envS[i];
		}

		// op4 (feedback)
		unit->opPhase[3] = wrap01(unit->opPhase[3] + oscF[3] * unit->recSr);
		float fb = unit->fbLast * feedback * 6.f;
		float dxPh = 0.f;
		if (dxFeedback > 1e-6f) {
			float dxSeed = sinf(kTwoPi * unit->opPhase[3] + fb);
			dxPh = dxFeedback * kPi * sinf(kTwoPi * unit->opPhase[3] + fb + dxFeedback * kPi * dxSeed);
		}
		float out4 = 0.f;
		if (level[3] > 1e-5f && envS[3] > 1e-6f) {
			out4 = opWave(wave[3], unit->opPhase[3], fb + dxPh) * envS[3] * level[3] * amod;
		}
		unit->fbLast = out4;
		if (unit->fbLast > 1.f) unit->fbLast = 1.f;
		if (unit->fbLast < -1.f) unit->fbLast = -1.f;

		float dummy, nCar, mix;
		float pm3, pm2, pm1;
		// need out3/out2/out1 for routing — compute in order using current outs
		// first pass: get pm from previous sample's outs? Yamaha is same-sample
		// if we compute 4 then 3 then 2 then 1, pm uses this-sample deeper ops.
		algoTick(algSel, 0, 0, 0, out4, &pm3, &pm2, &pm1, &dummy, &nCar);

		unit->opPhase[2] = wrap01(unit->opPhase[2] + oscF[2] * unit->recSr);
		float out3 = 0.f;
		if (level[2] > 1e-5f && envS[2] > 1e-6f) {
			out3 = opWave(wave[2], unit->opPhase[2], pm3) * envS[2] * level[2] * amod;
		}
		algoTick(algSel, 0, 0, out3, out4, &pm3, &pm2, &pm1, &dummy, &nCar);

		unit->opPhase[1] = wrap01(unit->opPhase[1] + oscF[1] * unit->recSr);
		float out2 = 0.f;
		if (level[1] > 1e-5f && envS[1] > 1e-6f) {
			out2 = opWave(wave[1], unit->opPhase[1], pm2) * envS[1] * level[1] * amod;
		}
		algoTick(algSel, 0, out2, out3, out4, &pm3, &pm2, &pm1, &dummy, &nCar);

		unit->opPhase[0] = wrap01(unit->opPhase[0] + oscF[0] * unit->recSr);
		float out1 = 0.f;
		if (level[0] > 1e-5f && envS[0] > 1e-6f) {
			out1 = opWave(wave[0], unit->opPhase[0], pm1) * envS[0] * level[0] * amod;
		}
		algoTick(algSel, out1, out2, out3, out4, &pm3, &pm2, &pm1, &mix, &nCar);

		float snd = mix / sqrtf(nCar > 0.f ? nCar : 1.f);
		out[n] = snd;
		envOut[n] = envSum;
	}
}

PluginLoad(DX100Voice) {
	ft = inTable;
	DefineSimpleUnit(DX100Voice);
}
