// Engine_DX100
// Yamaha 4-operator FM (DX100 / DX21 / DX27 / TX81Z) style.
// 8 algorithms, per-op rate/level envelopes in the dB domain, feedback
// on op4, TX81Z-style operator waveforms, one shared LFO with key sync,
// AMS/PMS/ALMS, per-op AME, 3-stage pitch EG, wheel/breath ranges.

Engine_DX100 : CroneEngine {
	// Raised from 8 after the UGen optimisation cut per-voice cost ~4x
	// (8 voices went from 95% to 26% of the audio thread on a norns).
	classvar <maxVoices = 16;

	var <gr;
	var <fxBus;
	var <fxGroup;
	var <lfoBus;
	var <lfoGroup;
	var <fxLfo;
	var <lfoSync;
	var <fxChar, <fxChorus, <fxPhaser, <fxOut;
	var <voices;
	var <voiceOrder;
	var <active;
	var <held;
	var <ctlBus;
	var <poly;
	var <voiceCap;
	var <scaleExp;
	var <headroom;
	var fxAlive;
	var charMix, chorusMix, phaserMix;
	var sleepGen;

	*new { arg context, doneCallback;
		^super.new(context, doneCallback);
	}

	alloc {
		SynthDef(\dx100, {
			arg out, lfoBus, hz = 220, gate = 0, vel = 1, legato = 0,
			persist = 0, killGate = 1, voiceScale = 1, headroom = 1,
			algo = 0, feedback = 0, dxFeedback = 0, amp = 0.4, pan = 0, transpose = 0,
			port = 0, portMode = 0,
			lfoDelay = 0, lfoUni = 0,
			pms = 0, ams = 0, alms = 0, wheelPm = 0, wheelAm = 0,
			pr1 = 0.5, pr2 = 0.5, pr3 = 0.5, pl1 = 0, pl2 = 0, pl3 = 0,
			breath = 0, egBias = 0,
			// per-operator: ratio, detune (steps), fixed hz, fixed mode, wave, level
			r1 = 1, r2 = 1, r3 = 1, r4 = 1,
			d1 = 0, d2 = 0, d3 = 0, d4 = 0,
			f1 = 100, f2 = 100, f3 = 100, f4 = 100,
			x1 = 0, x2 = 0, x3 = 0, x4 = 0,
			w1 = 0, w2 = 0, w3 = 0, w4 = 0,
			l1 = 1, l2 = 0, l3 = 0, l4 = 0,
			// per-operator envelope: AR, D1R, D1L, D2R, RR (0..1, higher = faster)
			a1 = 1, a2 = 1, a3 = 1, a4 = 1,
			b1 = 0.4, b2 = 0.4, b3 = 0.4, b4 = 0.4,
			c1 = 0.8, c2 = 0.8, c3 = 0.8, c4 = 0.8,
			e1 = 0.15, e2 = 0.15, e3 = 0.15, e4 = 0.15,
			g1 = 0.5, g2 = 0.5, g3 = 0.5, g4 = 0.5,
			// level scaling, velocity, AME, rate scaling, EG bias per operator
			k1 = 0, k2 = 0, k3 = 0, k4 = 0,
			v1 = 0, v2 = 0, v3 = 0, v4 = 0,
			m1 = 0, m2 = 0, m3 = 0, m4 = 0,
			s1 = 0, s2 = 0, s3 = 0, s4 = 0,
			z1 = 0, z2 = 0, z3 = 0, z4 = 0,
			grit = 1, oversample = 1;

			var envGate, kill, v, snd, envSum, lfoIn;

			envGate = gate.clip(0, 1);
			kill = EnvGen.kr(Env.asr(0.001, 1, 0.02), killGate, doneAction: 2);
			lfoIn = In.kr(lfoBus);

			// 4-op FM + envelopes: one C++ UGen (lib/ugens/DX100Voice).
			// Wheel/breath LFO ranges add to the voice's own PMD/AMD.
			v = DX100Voice.ar(
				hz, envGate, vel, legato,
				algo, feedback, dxFeedback,
				transpose, port, portMode,
				lfoIn, lfoDelay, lfoUni,
				(pms + wheelPm).clip(0, 1), (ams + wheelAm).clip(0, 1), alms,
				pr1, pr2, pr3, pl1, pl2, pl3,
				breath, egBias,
				r1, r2, r3, r4, d1, d2, d3, d4,
				f1, f2, f3, f4, x1, x2, x3, x4,
				w1, w2, w3, w4, l1, l2, l3, l4,
				a1, a2, a3, a4, b1, b2, b3, b4,
				c1, c2, c3, c4, e1, e2, e3, e4,
				g1, g2, g3, g4, k1, k2, k3, k4,
				v1, v2, v3, v4, m1, m2, m3, m4,
				s1, s2, s3, s4, z1, z2, z3, z4,
				grit, oversample
			);
			snd = v[0];
			envSum = v[1];
			FreeSelf.kr(
				(1 - persist)
				* (envGate < 0.5)
				* (Lag.kr(A2K.kr(envSum), 0.05) < 0.0005)
				* (Sweep.kr(1 - envGate) > 0.05)
			);
			snd = LeakDC.ar(snd);
			// Velocity reaches the sound only through per-op KVS, as on
			// the hardware; no global velocity->amp here.
			snd = snd * kill * Lag.kr(amp, 0.05);
			snd = snd * Lag.kr(voiceScale, 0.03) * Lag.kr(headroom, 0.05);
			Out.ar(out, Pan2.ar(snd, Lag.kr(pan, 0.08)));
		}).add;

		// One LFO for the whole engine, as on the hardware. Voices read
		// it from a control bus and apply their own delay/PMS/AMS. Key
		// sync restarts it at the positive peak (90 degrees) on key-on.
		SynthDef(\dx100lfo, {
			arg bus, lfoRate = 4, lfoWave = 0, lfoOneshot = 0, t_sync = 0;
			var rate, ph, shot, tri, sn, sq, sh, up, down, raw, wrapTrig;
			rate = lfoRate.clip(0.0008, 60);
			ph = Phasor.kr(t_sync, rate * ControlDur.ir, 0, 1, 0.25);
			// one-shot: a single cycle from the sync point, then park
			shot = Sweep.kr(t_sync, rate).min(1);
			ph = Select.kr(lfoOneshot, [ph, shot]);
			tri = 1 - (((ph + 0.25).wrap(0, 1) * 4) - 2).abs;
			sn = (ph * 2pi).sin;
			sq = ((ph < 0.5) * 2) - 1;
			wrapTrig = (HPZ1.kr(ph) < -0.5) + t_sync + Impulse.kr(0);
			sh = Latch.kr(WhiteNoise.kr, wrapTrig);
			up = (ph * 2) - 1;
			down = 1 - (ph * 2);
			raw = Select.kr(lfoWave, [tri, sn, sq, sh, up, down]);
			Out.kr(bus, raw);
		}).add;

		// ---- post-mix fx, one insert per section ----
		// Nonlinear / sample-destroying processes must see the mix, not
		// individual voices (see \dx100). SelectX still runs both branches,
		// so unused sections are separate synths paused with /n_run.
		// Order: character -> chorus -> phaser -> limiter/out.
		SynthDef(\dx100char, {
			arg bus, hiss = 0, bits = 0, srate = 0, drive = 0, glitch = 0;
			var snd, b, sr;
			snd = In.ar(bus, 2);
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
			snd = SelectX.ar(Lag.kr(glitch, 0.1).clip(0, 1),
				[snd, Latch.ar(snd,
					Impulse.ar(LFNoise0.kr(6).range(200, 9000)))]);
			ReplaceOut.ar(bus, snd);
		}).add;

		SynthDef(\dx100chorus, {
			arg bus, chorus = 0, chorusRate = 0.4, chorusWidth = 0.5;
			var snd, chMix, chRate, chDep, chW, chWet, haas, chPhase, mid, side;
			snd = In.ar(bus, 2);
			// stereo chorus: two delayed taps. width 0 = mono,
			// 1 = Haas ~26ms + opposite LFO + 3.2x M/S.
			chMix = Lag.kr(chorus, 0.08).clip(0, 1);
			chRate = Lag.kr(chorusRate, 0.08).clip(0.03, 8);
			chW = Lag.kr(chorusWidth, 0.08).clip(0, 1);
			chDep = 0.0016 + (chMix * 0.0022);
			chPhase = chW * pi;
			haas = chW.pow(1.4) * 0.026;
			chWet = (
				DelayC.ar(snd, 0.08, (
					[0.009, 0.009 + haas]
					+ (chDep * SinOsc.kr(chRate, [0, chPhase * 2 / 3]))
				).clip(0.001, 0.07))
				+ DelayC.ar(snd, 0.08, (
					[0.015, 0.015 + (haas * 0.7)]
					+ (chDep * SinOsc.kr(chRate * 0.87, [pi / 2, pi / 2 + chPhase]))
				).clip(0.001, 0.07))
			) * 0.5;
			mid = (chWet[0] + chWet[1]) * 0.5;
			side = (chWet[0] - chWet[1]) * 0.5 * (chW * 3.2);
			chWet = [mid + side, mid - side];
			ReplaceOut.ar(bus, snd + (chWet * chMix * 0.7));
		}).add;

		SynthDef(\dx100phaser, {
			arg bus, phaser = 0, phaserRate = 0.2, phaserWidth = 0.5;
			var snd, phMix, phRate, phW, phFreq, phWet, mid, side;
			snd = In.ar(bus, 2);
			// 4-stage phaser. width 0 = same sweep both channels,
			// 1 = opposite LFO, split ranges, 3.2x M/S.
			phMix = Lag.kr(phaser, 0.08).clip(0, 1);
			phRate = Lag.kr(phaserRate, 0.08).clip(0.02, 4);
			phW = Lag.kr(phaserWidth, 0.08).clip(0, 1);
			phFreq = [
				LFTri.kr(phRate, 0).exprange(
					160 / (1 + (phW * 0.5)), 1700 * (1 + phW)),
				LFTri.kr(phRate, phW * 0.5).exprange(
					160 * (1 + (phW * 2.2)), 1700 * (1 + (phW * 2)))
			];
			phWet = snd;
			4.do({
				phWet = BAllPass.ar(phWet, phFreq, 0.6);
			});
			mid = (phWet[0] + phWet[1]) * 0.5;
			side = (phWet[0] - phWet[1]) * 0.5 * (phW * 3.2);
			phWet = [mid + side, mid - side];
			ReplaceOut.ar(bus, snd + (phWet * phMix * 0.55));
		}).add;

		SynthDef(\dx100out, {
			arg bus, out;
			var snd;
			snd = In.ar(bus, 2);
			snd = Limiter.ar(LeakDC.ar(snd), 0.95, 0.01);
			Out.ar(out, snd);
		}).add;

		context.server.sync;

		ctlBus = Dictionary.new;
		[
			\algo, \feedback, \dxFeedback, \amp, \pan, \transpose,
			\port, \portMode,
			\lfoDelay, \lfoUni,
			\pms, \ams, \alms, \wheelPm, \wheelAm,
			\pr1, \pr2, \pr3, \pl1, \pl2, \pl3,
			\breath, \egBias,
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
			\m1, \m2, \m3, \m4,
			\s1, \s2, \s3, \s4,
			\z1, \z2, \z3, \z4,
			\grit, \oversample
			// NOTE: drive/hiss/bits/srate/glitch/chorus/phaser are NOT
			// here -- they are post-mix insert controls, not voice buses.
			// lfoRate/lfoWave/lfoOneshot/lfoSync go to the shared LFO synth.
		].do({ arg name;
			ctlBus.put(name, Bus.control(context.server));
		});

		// defaults (a serviceable electric bass, DX100 preset 1 territory)
		ctlBus[\algo].setSynchronous(0);
		ctlBus[\feedback].setSynchronous(5);
		ctlBus[\dxFeedback].setSynchronous(0);
		ctlBus[\amp].setSynchronous(0.4);
		ctlBus[\pan].setSynchronous(0);
		ctlBus[\transpose].setSynchronous(0);
		ctlBus[\port].setSynchronous(0);
		ctlBus[\portMode].setSynchronous(0);
		ctlBus[\lfoDelay].setSynchronous(0);
		ctlBus[\lfoUni].setSynchronous(0);
		ctlBus[\pms].setSynchronous(0);
		ctlBus[\ams].setSynchronous(0);
		ctlBus[\alms].setSynchronous(0);
		ctlBus[\wheelPm].setSynchronous(0);
		ctlBus[\wheelAm].setSynchronous(0);
		[\pr1, \pr2, \pr3].do({ arg k; ctlBus[k].setSynchronous(0.5) });
		[\pl1, \pl2, \pl3].do({ arg k; ctlBus[k].setSynchronous(0) });
		ctlBus[\breath].setSynchronous(0);
		ctlBus[\egBias].setSynchronous(0);
		[\r1, \r2, \r3, \r4].do({ arg k; ctlBus[k].setSynchronous(1) });
		[\d1, \d2, \d3, \d4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\f1, \f2, \f3, \f4].do({ arg k; ctlBus[k].setSynchronous(100) });
		[\x1, \x2, \x3, \x4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\w1, \w2, \w3, \w4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\l1, \l2, \l3, \l4].do({ arg k, i;
			ctlBus[k].setSynchronous([1.0, 0.82, 0.72, 0.65][i]);
		});
		[\a1, \a2, \a3, \a4].do({ arg k; ctlBus[k].setSynchronous(1.0) });
		[\b1, \b2, \b3, \b4].do({ arg k; ctlBus[k].setSynchronous(0.4) });
		[\c1, \c2, \c3, \c4].do({ arg k, i;
			ctlBus[k].setSynchronous([0.8, 0.6, 0.5, 0.4][i]);
		});
		[\e1, \e2, \e3, \e4].do({ arg k; ctlBus[k].setSynchronous(0.25) });
		[\g1, \g2, \g3, \g4].do({ arg k; ctlBus[k].setSynchronous(0.55) });
		[\k1, \k2, \k3, \k4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\v1, \v2, \v3, \v4].do({ arg k, i;
			ctlBus[k].setSynchronous([0.3, 0.5, 0.5, 0.5][i]);
		});
		[\m1, \m2, \m3, \m4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\s1, \s2, \s3, \s4].do({ arg k; ctlBus[k].setSynchronous(0) });
		[\z1, \z2, \z3, \z4].do({ arg k; ctlBus[k].setSynchronous(0) });
		ctlBus[\grit].setSynchronous(1);
		ctlBus[\oversample].setSynchronous(1);

		// voices -> private stereo bus -> gated inserts -> limiter/out.
		fxBus = Bus.audio(context.server, 2);
		lfoBus = Bus.control(context.server);
		// LFO runs ahead of the voices so they read this block's value.
		lfoGroup = Group.tail(context.xg);
		gr = ParGroup.tail(context.xg);
		fxGroup = Group.tail(context.xg);
		fxLfo = Synth.tail(lfoGroup, \dx100lfo, [\bus, lfoBus.index]);
		lfoSync = true;
		fxChar = Synth.tail(fxGroup, \dx100char, [\bus, fxBus.index]);
		fxChorus = Synth.tail(fxGroup, \dx100chorus, [\bus, fxBus.index]);
		fxPhaser = Synth.tail(fxGroup, \dx100phaser, [\bus, fxBus.index]);
		fxOut = Synth.tail(fxGroup, \dx100out,
			[\bus, fxBus.index, \out, context.out_b.index]);
		fxAlive = true;
		sleepGen = IdentityDictionary.new;
		charMix = IdentityDictionary[
			\hiss -> 0, \bits -> 0, \srate -> 0, \drive -> 0, \glitch -> 0
		];
		chorusMix = 0;
		phaserMix = 0;
		context.server.sync;
		fxChar.run(false);
		fxChorus.run(false);
		fxPhaser.run(false);
		voices = Dictionary.new;
		voiceOrder = List.new;
		active = List.new;
		held = IdentitySet.new;
		poly = 1;
		voiceCap = Engine_DX100.maxVoices;
		scaleExp = 0.5;
		headroom = 1.0;

		[\hiss, \bits, \srate, \drive, \glitch].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				charMix[name] = msg[1];
				fxChar.set(name, msg[1]);
				if(charMix.values.any({ arg v; v > 0 }), {
					this.wakeFx(fxChar);
				}, {
					this.sleepFx(fxChar, {
						charMix.values.any({ arg v; v > 0 }).not
					});
				});
			});
		});
		this.addCommand("chorus", "f", { arg msg;
			chorusMix = msg[1];
			fxChorus.set(\chorus, chorusMix);
			if(chorusMix > 0, {
				this.wakeFx(fxChorus);
			}, {
				this.sleepFx(fxChorus, { chorusMix <= 0 });
			});
		});
		[\chorusRate, \chorusWidth].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				fxChorus.set(name, msg[1]);
			});
		});
		this.addCommand("phaser", "f", { arg msg;
			phaserMix = msg[1];
			fxPhaser.set(\phaser, phaserMix);
			if(phaserMix > 0, {
				this.wakeFx(fxPhaser);
			}, {
				this.sleepFx(fxPhaser, { phaserMix <= 0 });
			});
		});
		[\phaserRate, \phaserWidth].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				fxPhaser.set(name, msg[1]);
			});
		});

		[\lfoRate, \lfoWave, \lfoOneshot].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				fxLfo.set(name, msg[1]);
			});
		});
		this.addCommand("lfoSync", "f", { arg msg;
			lfoSync = msg[1] > 0.5;
		});

		this.addCommand("scale_exp", "f", { arg msg;
			scaleExp = msg[1].clip(0, 1.5);
			this.rebalance;
		});

		this.addCommand("headroom", "f", { arg msg;
			headroom = msg[1].clip(0, 2);
			active.do({ arg syn;
				if(syn.notNil, { syn.set(\headroom, headroom) });
			});
		});

		this.addCommand("voice_mode", "f", { arg msg;
			poly = (msg[1] > 0.5).if({ 1 }, { 0 });
			this.noteOffAll;
		});

		this.addCommand("max_voices", "f", { arg msg;
			voiceCap = msg[1].asInteger.clip(1, Engine_DX100.maxVoices);
			while({ active.size > voiceCap }, { this.steal });
			this.rebalance;
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
		n = active.size.max(1);
		// scaleExp 0 = no scaling, 0.5 = 1/sqrt(N), 1.0 = 1/N
		scale = n.pow(scaleExp.neg);
		active.do({ arg syn;
			if(syn.notNil, { syn.set(\voiceScale, scale) });
		});
		^scale;
	}

	noteOn { arg id, hz, vel, legato, trig;
		var syn, args, persist;
		if(poly == 0, { id = 0 });
		// Hardware LFO key sync: every key-on restarts the (shared) LFO.
		// Mono legato passes trig=0 so a slurred note does not.
		if(lfoSync and: { trig > 0 }, { fxLfo.set(\t_sync, 1) });
		syn = voices[id];
		// Reuse whenever this id has a language-side node. Do not test
		// isPlaying: NodeWatcher is false until /n_go, so a retrigger in
		// that window used to spawn a second synth. The old one kept
		// gate=1, dropped out of `voices`, and never freed — CPU compounded.
		if(syn.notNil, {
			syn.set(
				\hz, hz, \vel, vel, \legato, legato,
				\gate, 1, \killGate, 1
			);
			voiceOrder.remove(id);
			voiceOrder.add(id);
			active.remove(syn);
			active.add(syn);
			held.add(syn);
			^this;
		});
		while({ active.size >= voiceCap }, { this.steal });
		persist = (poly == 0).if({ 1 }, { 0 });
		args = [
			\out, fxBus.index, \lfoBus, lfoBus.index,
			\hz, hz, \vel, vel, \legato, legato,
			\gate, 1, \persist, persist, \killGate, 1
		];
		ctlBus.keysValuesDo({ arg name, bus;
			args = args.add(name).add(bus.getSynchronous);
		});
		args = args.add(\voiceScale).add(
			(active.size + 1).max(1).pow(scaleExp.neg));
		args = args.add(\headroom).add(headroom);
		syn = Synth(\dx100, args, gr);
		ctlBus.keys.do({ arg name;
			syn.map(name, ctlBus[name]);
		});
		NodeWatcher.register(syn);
		syn.onFree({
			active.remove(syn);
			held.remove(syn);
			if(voices[id] === syn, {
				voices.removeAt(id);
				voiceOrder.remove(id);
			});
			this.rebalance;
		});
		voices.put(id, syn);
		voiceOrder.add(id);
		active.add(syn);
		held.add(syn);
		this.rebalance;
	}

	noteOff { arg id;
		var syn;
		if(poly == 0, { id = 0 });
		syn = voices[id];
		if(syn.notNil, {
			held.remove(syn);
			syn.set(\gate, 0);
		});
	}

	noteOffAll {
		// Group set reaches orphans that fell out of `voices`.
		gr.set(\gate, 0, \killGate, 0);
		held.clear;
	}

	// Prefer oldest releasing tail. Only steal a held note if every
	// live synth is still gated.
	steal {
		var syn, id;
		if(active.size == 0, { ^this });
		syn = active.detect({ arg s; held.includes(s).not });
		if(syn.isNil, { syn = active.first });
		active.remove(syn);
		held.remove(syn);
		id = voices.findKeyForValue(syn);
		if(id.notNil, {
			voices.removeAt(id);
			voiceOrder.remove(id);
		});
		if(syn.notNil, { syn.set(\killGate, 0) });
	}

	// Pause after mix lag (80–100ms) so the wet fade finishes first.
	// Every call used to queue its own timer, so automating a character
	// param stacked one pending sleep per message, all firing later. Keep
	// a generation counter per synth and let only the newest one act.
	sleepFx { arg syn, stillOff;
		var gen;
		gen = (sleepGen[syn] ? 0) + 1;
		sleepGen[syn] = gen;
		SystemClock.sched(0.12, {
			if(fxAlive
				and: { sleepGen[syn] == gen }
				and: { stillOff.value },
				{ syn.run(false) });
			nil;
		});
	}

	// Cancel any pending sleep for a synth we are about to wake.
	wakeFx { arg syn;
		sleepGen[syn] = (sleepGen[syn] ? 0) + 1;
		syn.run(true);
	}

	free {
		fxAlive = false;
		gr.free;
		lfoGroup.free;
		fxGroup.free;
		fxBus.free;
		lfoBus.free;
		ctlBus.do({ arg b; b.free });
	}
}
