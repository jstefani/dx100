// Engine_DX100
// Yamaha 4-operator FM (DX100 / DX21 / DX27 / TX81Z) style.
// 8 algorithms, per-op rate/level envelopes, feedback on op4,
// TX81Z-style operator waveforms, LFO with AMS/PMS.

Engine_DX100 : CroneEngine {
	classvar <maxVoices = 8;

	var <gr;
	var <fxBus;
	var <fx;
	var <voices;
	var <voiceOrder;
	var <ctlBus;
	var <poly;
	var <scaleExp;
	var <headroom;

	*new { arg context, doneCallback;
		^super.new(context, doneCallback);
	}

	alloc {
		SynthDef(\dx100, {
			arg out, hz = 220, gate = 0, vel = 1, legato = 0, t_trig = 0,
			persist = 0, killGate = 1, voiceScale = 1, headroom = 1,
			algo = 0, feedback = 0, amp = 0.4, pan = 0, transpose = 0,
			port = 0, portMode = 0,
			lfoRate = 4, lfoWave = 0, lfoDelay = 0, pms = 0, ams = 0,
			pitchEgAmt = 0, pitchEgRate = 0.5, pitchEgLevel = 0,
			// per-operator: ratio, detune (cents), fixed hz, fixed mode, wave, level
			r1 = 1, r2 = 1, r3 = 1, r4 = 1,
			d1 = 0, d2 = 0, d3 = 0, d4 = 0,
			f1 = 100, f2 = 100, f3 = 100, f4 = 100,
			x1 = 0, x2 = 0, x3 = 0, x4 = 0,
			w1 = 0, w2 = 0, w3 = 0, w4 = 0,
			l1 = 1, l2 = 0, l3 = 0, l4 = 0,
			// per-operator envelope: attack rate, decay1 rate, decay1 level,
			// decay2 rate, release rate  (rates 0..1, higher = faster)
			a1 = 0.9, a2 = 0.9, a3 = 0.9, a4 = 0.9,
			b1 = 0.4, b2 = 0.4, b3 = 0.4, b4 = 0.4,
			c1 = 0.8, c2 = 0.8, c3 = 0.8, c4 = 0.8,
			e1 = 0.15, e2 = 0.15, e3 = 0.15, e4 = 0.15,
			g1 = 0.5, g2 = 0.5, g3 = 0.5, g4 = 0.5,
			// key scaling + velocity sensitivity per operator
			k1 = 0, k2 = 0, k3 = 0, k4 = 0,
			v1 = 0, v2 = 0, v3 = 0, v4 = 0,
			rateScale = 0;

			var freq, slide, envGate, kill, keyTrack, velAmt;
			var lfo, lfoEnv, pitchEg, pmod, amod;
			var ratios, dets, fixed, fixedHz, waves, levels;
			var oscFreq, envs, ops, snd, fb, fbBuf;
			var opWave, mkEnv, rateTime, out1, out2, out3, out4;
			var m, carrierSum, nCarriers;

			// ---- pitch ----
			hz = Lag.kr(hz, 0.001) * (2 ** (transpose / 12));
			slide = Select.kr(portMode.round.clip(0, 1), [
				port * legato.clip(0, 1),
				port
			]);
			freq = VarLag.kr(hz, slide.clip(0, 5), 0).clip(8, 12000);

			envGate = gate.clip(0, 1);
			kill = EnvGen.kr(Env.asr(0.001, 1, 0.02), killGate, doneAction: 2);

			// key scaling: 0 at C3, +/- across the keyboard
			keyTrack = (freq / 261.63).max(0.05).log2;

			// ---- pitch envelope (DX21/TX81Z style, bipolar) ----
			pitchEg = EnvGen.kr(
				Env([1, 0], [1], \lin),
				envGate,
				timeScale: Lag.kr(pitchEgRate, 0.05).linexp(0, 1, 4.0, 0.004)
			);
			pitchEg = pitchEg * Lag.kr(pitchEgLevel, 0.05);
			freq = freq * (2 ** (pitchEg * Lag.kr(pitchEgAmt, 0.05) * 2));

			// ---- LFO ----
			lfoRate = Lag.kr(lfoRate, 0.05).clip(0.02, 60);
			lfoEnv = EnvGen.kr(Env([0, 1], [1], \lin), envGate,
				timeScale: Lag.kr(lfoDelay, 0.05).linlin(0, 1, 0.001, 4));
			lfo = Select.kr(lfoWave.round.clip(0, 3), [
				LFTri.kr(lfoRate),
				SinOsc.kr(lfoRate),
				LFPulse.kr(lfoRate, 0, 0.5).linlin(0, 1, -1, 1),
				Latch.kr(WhiteNoise.kr, Impulse.kr(lfoRate))
			]) * lfoEnv;

			// pitch mod sensitivity (PMS) -> ~ +/- 1 semitone at full
			pmod = 2 ** (lfo * Lag.kr(pms, 0.05) * 0.0833);
			// amp mod sensitivity (AMS) -> applied to modulators & carriers
			amod = 1 - (lfo.range(0, 1) * Lag.kr(ams, 0.05));

			freq = freq * pmod;

			// ---- operator frequencies ----
			ratios = [r1, r2, r3, r4];
			dets = [d1, d2, d3, d4];
			fixed = [x1, x2, x3, x4];
			fixedHz = [f1, f2, f3, f4];
			waves = [w1, w2, w3, w4];
			levels = [l1, l2, l3, l4];

			oscFreq = 4.collect({ arg i;
				Select.kr(fixed[i].round.clip(0, 1), [
					freq * Lag.kr(ratios[i], 0.02),
					Lag.kr(fixedHz[i], 0.02)
				]) * (2 ** (Lag.kr(dets[i], 0.05) / 1200));
			});

			// ---- envelopes ----
			// DX rate params (0..1, higher = faster) mapped to times.
			rateTime = { arg rate, lo, hi;
				Lag.kr(rate, 0.02).clip(0, 1).linexp(0, 1, hi, lo);
			};

			// DX envelope: attack -> decay1 to D1L -> decay2 toward 0 while
			// held -> release. Env's releaseNode makes the gate-off branch
			// jump straight to the release segment from wherever it is.
			mkEnv = { arg atkR, d1R, d1L, d2R, relR, keyScale, velSens;
				var at, dt, st, d2t, rt, e, scl, vScale;
				// rate scaling: higher notes get faster envelopes
				scl = 2 ** (keyTrack * Lag.kr(rateScale, 0.05).neg * 0.5);
				at = rateTime.(atkR, 0.0008, 6.0) * scl;
				dt = rateTime.(d1R, 0.004, 12.0) * scl;
				st = Lag.kr(d1L, 0.02).clip(0, 1);
				d2t = rateTime.(d2R, 0.01, 40.0) * scl;
				rt = rateTime.(relR, 0.004, 10.0) * scl;
				// nodes: 0 start, 1 peak, 2 D1L, 3 decay2 floor (sustain node
				// -- held here once D2R has run its course), 4 silence.
				e = EnvGen.kr(
					Env(
						[0, 1, st, 0, 0],
						[at, dt, d2t, rt],
						[\lin, -4, -4, -4],
						releaseNode: 3
					),
					envGate,
					doneAction: 0
				);
				// velocity sensitivity
				vScale = 1 - (Lag.kr(velSens, 0.05) * (1 - vel.clip(0, 1)));
				// key level scaling: level falls off toward high notes
				e * vScale * (2 ** (keyTrack * Lag.kr(keyScale, 0.05).neg));
			};

			envs = [
				mkEnv.(a1, b1, c1, e1, g1, k1, v1),
				mkEnv.(a2, b2, c2, e2, g2, k2, v2),
				mkEnv.(a3, b3, c3, e3, g3, k3, v3),
				mkEnv.(a4, b4, c4, e4, g4, k4, v4)
			];

			// free once the gate is down and every operator envelope has
			// decayed away. a short hold keeps a re-trigger from cutting
			// the tail off mid-release.
			FreeSelf.kr(
				(1 - persist)
				* (envGate < 0.5)
				* (Lag.kr(envs.sum, 0.05) < 0.0005)
				* (Sweep.kr(1 - envGate) > 0.05)
			);

			// ---- operator waveforms (TX81Z set) ----
			// phase in, sample out. w: 0 sine, 1 half-sine, 2 abs-sine,
			// 3 quarter-sine, 4 alt-sine, 5 alt half, 6 sine^2ish, 7 saw-ish
			opWave = { arg ph, w, fq;
				var s, half, absS, quart, alt, altHalf, squared, sawish;
				// SinOsc's phase input degrades past about +/-8pi (~25 rad):
				// measured max error vs a wrapped phase is ~1e-7 below that
				// and jumps to ~2.0 above it. This engine's worst case is
				// m(7.0) * two modulators at full level = 14 rad, so the
				// bound is not currently reached -- this wrap is cheap
				// insurance if m or operator levels are ever raised, not a
				// fix for an audible defect.
				ph = ph.wrap(-pi, pi);
				s = SinOsc.ar(fq, ph);
				half = s.max(0);
				absS = s.abs;
				quart = SinOsc.ar(fq, ph).max(0) * LFPulse.ar(fq, 0, 0.5);
				alt = SinOsc.ar(fq * 2, ph) * LFPulse.ar(fq, 0, 0.5);
				altHalf = alt.max(0);
				squared = s * s * s.sign;
				sawish = (s + (SinOsc.ar(fq * 2, ph) * 0.5)
					+ (SinOsc.ar(fq * 3, ph) * 0.333)) * 0.6;
				Select.ar(w.round.clip(0, 7), [
					s, half, absS, quart, alt, altHalf, squared, sawish
				]);
			};

			// ---- operator 4 with feedback ----
			// op4 phase-modulates itself; clip the loop so a stray non-finite
			// sample can never latch the voice into silence.
			fbBuf = LocalIn.ar(1).clip2(1.0);
			fb = fbBuf * Lag.kr(feedback, 0.05).clip(0, 1) * 6;
			out4 = opWave.(fb, waves[3], oscFreq[3]) * envs[3]
				* Lag.kr(levels[3], 0.03) * amod;
			LocalOut.ar(out4.clip2(1.0));

			// modulation index: how far a modulator swings the carrier phase
			m = 7.0;

			// ---- algorithm routing ----
			// Each algorithm is a distinct patch of who modulates whom.
			// op4 is always the deepest modulator (and holds feedback).
			// Yamaha 4-op algorithm set, 1-indexed as ALG 1..8:
			//  1: 4>3>2>1                     (serial)
			//  2: (4>2), 3>2>1  ->  4 and 3 both into 2
			//  3: 4>1, 3>2>1
			//  4: 4>3, (3+2)>1  ->  4>3>1 and 2>1
			//  5: 4>3>1 , 4>2   -> split
			//  6: 4>1, 4>2, 4>3 (three carriers from one mod)
			//  7: 4>3, then 3+2+1 all carriers
			//  8: all four parallel carriers

			// op3: modulated by op4 in algs 1, 4, 5, 6, 7
			out3 = Select.ar(algo.round.clip(0, 7), [
				opWave.(out4 * m, waves[2], oscFreq[2]),                 // 1: 4>3
				opWave.(DC.ar(0), waves[2], oscFreq[2]),                 // 2: 3 free
				opWave.(DC.ar(0), waves[2], oscFreq[2]),                 // 3: 3 free
				opWave.(out4 * m, waves[2], oscFreq[2]),                 // 4: 4>3
				opWave.(out4 * m, waves[2], oscFreq[2]),                 // 5: 4>3
				opWave.(out4 * m, waves[2], oscFreq[2]),                 // 6: 4>3
				opWave.(out4 * m, waves[2], oscFreq[2]),                 // 7: 4>3
				opWave.(DC.ar(0), waves[2], oscFreq[2])                  // 8: parallel
			]) * envs[2] * Lag.kr(levels[2], 0.03) * amod;

			// op2
			out2 = Select.ar(algo.round.clip(0, 7), [
				opWave.(out3 * m, waves[1], oscFreq[1]),                 // 1: 3>2
				opWave.((out3 + out4) * m, waves[1], oscFreq[1]),        // 2: 3+4>2
				opWave.(out3 * m, waves[1], oscFreq[1]),                 // 3: 3>2
				opWave.(DC.ar(0), waves[1], oscFreq[1]),                 // 4: 2 free
				opWave.(out4 * m, waves[1], oscFreq[1]),                 // 5: 4>2
				opWave.(out4 * m, waves[1], oscFreq[1]),                 // 6: 4>2
				opWave.(DC.ar(0), waves[1], oscFreq[1]),                 // 7: carrier
				opWave.(DC.ar(0), waves[1], oscFreq[1])                  // 8: carrier
			]) * envs[1] * Lag.kr(levels[1], 0.03) * amod;

			// op1 (always a carrier)
			out1 = Select.ar(algo.round.clip(0, 7), [
				opWave.(out2 * m, waves[0], oscFreq[0]),                 // 1: 2>1
				opWave.(out2 * m, waves[0], oscFreq[0]),                 // 2: 2>1
				opWave.((out2 + out4) * m, waves[0], oscFreq[0]),        // 3: 2+4>1
				opWave.((out3 + out2) * m, waves[0], oscFreq[0]),        // 4: 3+2>1
				opWave.(out3 * m, waves[0], oscFreq[0]),                 // 5: 3>1
				opWave.(out4 * m, waves[0], oscFreq[0]),                 // 6: 4>1
				opWave.(DC.ar(0), waves[0], oscFreq[0]),                 // 7: carrier
				opWave.(DC.ar(0), waves[0], oscFreq[0])                  // 8: carrier
			]) * envs[0] * Lag.kr(levels[0], 0.03) * amod;

			// which ops reach the output, per algorithm
			carrierSum = Select.ar(algo.round.clip(0, 7), [
				out1,                        // 1
				out1,                        // 2
				out1,                        // 3
				out1,                        // 4
				out1 + out2,                 // 5
				out1 + out2 + out3,          // 6
				out1 + out2 + out3,          // 7
				out1 + out2 + out3 + out4    // 8
			]);
			nCarriers = Select.kr(algo.round.clip(0, 7),
				[1, 1, 1, 1, 2, 3, 3, 4]);
			snd = carrierSum / nCarriers.sqrt;

			// ---- output stage ----
			// The character section (hiss/bits/srate/drive/glitch) lives in
			// the \dx100fx synth downstream, NOT here. Applying it per voice
			// quantizes each voice separately and sums the results, which is
			// not the same as quantizing the mix: the error products are
			// uncorrelated between voices and beat against each other.
			// Measured at 8 voices that is 2.8x the quantization error of
			// processing the sum (0.0125 vs 0.0045 RMS), audible as
			// low-bit-rate grain and ring-mod artifacts that are completely
			// absent on a single note. Same reasoning as orgn's single
			// \ulaw fx synth on a shared bus.
			snd = LeakDC.ar(snd);
			snd = snd * kill * Lag.kr(amp, 0.05) * vel.linlin(0, 1, 0.5, 1.0);
			snd = snd * Lag.kr(voiceScale, 0.03) * Lag.kr(headroom, 0.05);
			Out.ar(out, Pan2.ar(snd, Lag.kr(pan, 0.08)));
		}).add;

		// ---- character / output fx, ONE instance for the whole synth ----
		// Runs once on the summed voice bus. Everything here is a nonlinear
		// or sample-destroying process, and all of them must see the mix
		// rather than individual voices (see the note in \dx100 above).
		SynthDef(\dx100fx, {
			arg in, out, hiss = 0, bits = 0, srate = 0, drive = 0, glitch = 0;
			var snd, b, sr;

			snd = In.ar(in, 2);

			snd = snd + (PinkNoise.ar * Lag.kr(hiss, 0.08) * 0.012);

			// 12-bit-ish DAC crunch of the originals.
			// core UGens only -- no SC3-plugins dependency.
			b = Lag.kr(bits, 0.08).clip(0, 1);
			snd = SelectX.ar(b, [
				snd,
				(snd * (2 ** (12 - (b * 6)))).round(1.0) / (2 ** (12 - (b * 6)))
			]);

			sr = Lag.kr(srate, 0.08).clip(0, 1);
			snd = SelectX.ar(sr, [
				snd,
				Latch.ar(snd, Impulse.ar(sr.linexp(0.001, 1, 24000, 1500)))
			]);

			drive = Lag.kr(drive, 0.05);
			snd = SelectX.ar(drive.clip(0, 1),
				[snd, (snd * (1 + (drive * 8))).tanh * 0.7]);

			// digital glitch: occasional sample-hold stutter
			snd = SelectX.ar(Lag.kr(glitch, 0.1).clip(0, 1),
				[snd, Latch.ar(snd,
					Impulse.ar(LFNoise0.kr(6).range(200, 9000)))]);

			// catch the summed peaks that no per-voice trim can reach.
			snd = Limiter.ar(LeakDC.ar(snd), 0.95, 0.01);
			Out.ar(out, snd);
		}).add;

		context.server.sync;

		ctlBus = Dictionary.new;
		[
			\algo, \feedback, \amp, \pan, \transpose,
			\port, \portMode,
			\lfoRate, \lfoWave, \lfoDelay, \pms, \ams,
			\pitchEgAmt, \pitchEgRate, \pitchEgLevel,
			\r1, \r2, \r3, \r4,
			\d1, \d2, \d3, \d4,
			\f1, \f2, \f3, \f4,
			\x1, \x2, \x3, \x4,
			\w1, \w2, \w3, \w4,
			\l1, \l2, \l3, \l4,
			\a1, \a2, \a3, \a4,
			\b1, \b2, \b3, \b4,
			\c1, \c2, \c3, \c4,
			\e1, \e2, \e3, \e4,
			\g1, \g2, \g3, \g4,
			\k1, \k2, \k3, \k4,
			\v1, \v2, \v3, \v4,
			\rateScale
			// NOTE: drive/hiss/bits/srate/glitch are NOT here -- they are
			// fx-synth controls now, set directly on `fx`.
		].do({ arg name;
			ctlBus.put(name, Bus.control(context.server));
		});

		// defaults (a serviceable electric bass, DX100 preset 1 territory)
		ctlBus[\algo].setSynchronous(0);
		ctlBus[\feedback].setSynchronous(0.4);
		ctlBus[\amp].setSynchronous(0.4);
		ctlBus[\pan].setSynchronous(0);
		ctlBus[\transpose].setSynchronous(0);
		ctlBus[\port].setSynchronous(0);
		ctlBus[\portMode].setSynchronous(0);
		ctlBus[\lfoRate].setSynchronous(4);
		ctlBus[\lfoWave].setSynchronous(0);
		ctlBus[\lfoDelay].setSynchronous(0);
		ctlBus[\pms].setSynchronous(0);
		ctlBus[\ams].setSynchronous(0);
		ctlBus[\pitchEgAmt].setSynchronous(0);
		ctlBus[\pitchEgRate].setSynchronous(0.5);
		ctlBus[\pitchEgLevel].setSynchronous(0);
		[\r1, \r2, \r3, \r4].do({ arg k, i;
			ctlBus[k].setSynchronous([1, 1, 1, 1][i]);
		});
		[\d1, \d2, \d3, \d4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\f1, \f2, \f3, \f4].do({ arg k; ctlBus[k].setSynchronous(100) });
		[\x1, \x2, \x3, \x4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\w1, \w2, \w3, \w4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\l1, \l2, \l3, \l4].do({ arg k, i;
			ctlBus[k].setSynchronous([1.0, 0.75, 0.6, 0.5][i]);
		});
		[\a1, \a2, \a3, \a4].do({ arg k; ctlBus[k].setSynchronous(0.95) });
		[\b1, \b2, \b3, \b4].do({ arg k; ctlBus[k].setSynchronous(0.45) });
		[\c1, \c2, \c3, \c4].do({ arg k, i;
			ctlBus[k].setSynchronous([0.8, 0.5, 0.4, 0.3][i]);
		});
		[\e1, \e2, \e3, \e4].do({ arg k; ctlBus[k].setSynchronous(0.2) });
		[\g1, \g2, \g3, \g4].do({ arg k; ctlBus[k].setSynchronous(0.55) });
		[\k1, \k2, \k3, \k4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\v1, \v2, \v3, \v4].do({ arg k, i;
			ctlBus[k].setSynchronous([0.3, 0.5, 0.5, 0.5][i]);
		});
		ctlBus[\rateScale].setSynchronous(0);
		ctlBus[\drive].setSynchronous(0);
		ctlBus[\hiss].setSynchronous(0);
		ctlBus[\bits].setSynchronous(0);
		ctlBus[\srate].setSynchronous(0);
		ctlBus[\glitch].setSynchronous(0);

		// voices -> private stereo bus -> single fx synth -> norns out.
		fxBus = Bus.audio(context.server, 2);
		gr = ParGroup.tail(context.xg);
		fx = Synth.tail(context.xg, \dx100fx,
			[\in, fxBus.index, \out, context.out_b.index]);
		context.server.sync;
		voices = Dictionary.new;
		voiceOrder = List.new;
		poly = 1;
		scaleExp = 0.5;
		headroom = 1.0;

		// character controls live on the single fx synth
		[\hiss, \bits, \srate, \drive, \glitch].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				fx.set(name, msg[1]);
			});
		});

		this.addCommand("scale_exp", "f", { arg msg;
			scaleExp = msg[1].clip(0, 1.5);
			this.rebalance;
		});

		this.addCommand("headroom", "f", { arg msg;
			headroom = msg[1].clip(0, 2);
			voices.do({ arg syn;
				if(syn.notNil and: { syn.isPlaying }, {
					syn.set(\headroom, headroom);
				});
			});
		});

		this.addCommand("voice_mode", "f", { arg msg;
			poly = (msg[1] > 0.5).if({ 1 }, { 0 });
			this.noteOffAll;
		});

		this.addCommand("note_on", "iffff", { arg msg;
			this.noteOn(msg[1].asInteger, msg[2], msg[3], msg[4], msg[5]);
		});

		this.addCommand("note_off", "i", { arg msg;
			this.noteOff(msg[1].asInteger);
		});

		this.addCommand("note_off_all", "", {
			this.noteOffAll;
		});

		ctlBus.keys.do({ arg name;
			this.addCommand(name, "f", { arg msg;
				ctlBus[name].setSynchronous(msg[1]);
			});
		});
	}

	// N independent voices sum to about sqrt(N) louder. Scale every live
	// voice by 1/sqrt(N) so a chord sits at roughly the level of one note
	// instead of driving the output bus into clipping.
	rebalance {
		var n, scale;
		n = voices.size.max(1);
		// scaleExp 0 = no scaling, 0.5 = 1/sqrt(N), 1.0 = 1/N
		scale = n.pow(scaleExp.neg);
		voices.do({ arg syn;
			if(syn.notNil and: { syn.isPlaying }, {
				syn.set(\voiceScale, scale);
			});
		});
		^scale;
	}

	noteOn { arg id, hz, vel, legato, trig;
		var syn, args, persist;
		if(poly == 0, { id = 0 });
		syn = voices[id];
		if(syn.notNil and: { syn.isPlaying }, {
			syn.set(
				\hz, hz, \vel, vel, \legato, legato,
				\gate, 1, \t_trig, trig, \killGate, 1
			);
			voiceOrder.remove(id);
			voiceOrder.add(id);
		}, {
			if(voices.size >= Engine_DX100.maxVoices, { this.steal });
			persist = (poly == 0).if({ 1 }, { 0 });
			args = [
				\out, fxBus.index,
				\hz, hz, \vel, vel, \legato, legato,
				\gate, 1, \t_trig, trig, \persist, persist, \killGate, 1
			];
			ctlBus.keysValuesDo({ arg name, bus;
				args = args.add(name).add(bus.getSynchronous);
			});
			args = args.add(\voiceScale).add(
				(voices.size + 1).max(1).pow(scaleExp.neg));
			args = args.add(\headroom).add(headroom);
			syn = Synth(\dx100, args, gr);
			ctlBus.keys.do({ arg name;
				syn.map(name, ctlBus[name]);
			});
			NodeWatcher.register(syn);
			syn.onFree({
				voices.removeAt(id);
				voiceOrder.remove(id);
				// a freed voice leaves more headroom for the rest
				this.rebalance;
			});
			voices.put(id, syn);
			voiceOrder.add(id);
			this.rebalance;
		});
	}

	noteOff { arg id;
		var syn;
		if(poly == 0, { id = 0 });
		syn = voices[id];
		if(syn.notNil and: { syn.isPlaying }, {
			syn.set(\gate, 0);
		});
	}

	noteOffAll {
		voices.keysValuesDo({ arg id, syn;
			if(syn.notNil and: { syn.isPlaying }, {
				syn.set(\gate, 0);
			});
		});
	}

	steal {
		var id, syn;
		if(voiceOrder.size == 0, { ^this });
		id = voiceOrder.removeAt(0);
		syn = voices[id];
		if(syn.notNil and: { syn.isPlaying }, {
			syn.set(\killGate, 0);
		}, {
			if(syn.notNil, { syn.free });
		});
		voices.removeAt(id);
	}

	free {
		gr.free;
		fx.free;
		fxBus.free;
		ctlBus.do({ arg b; b.free });
	}
}
