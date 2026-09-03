// Engine_DX100
// Yamaha 4-operator FM (DX100 / DX21 / DX27 / TX81Z) style.
// 8 algorithms, per-op rate/level envelopes, feedback on op4,
// TX81Z-style operator waveforms, LFO with AMS/PMS/ALMS,
// one-shot and unipolar.

Engine_DX100 : CroneEngine {
	classvar <maxVoices = 8;

	var <gr;
	var <fxBus;
	var <fxGroup;
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

	*new { arg context, doneCallback;
		^super.new(context, doneCallback);
	}

	alloc {
		SynthDef(\dx100, {
			arg out, hz = 220, gate = 0, vel = 1, legato = 0, t_trig = 0,
			persist = 0, killGate = 1, voiceScale = 1, headroom = 1,
			algo = 0, feedback = 0, dxFeedback = 0, amp = 0.4, pan = 0, transpose = 0,
			port = 0, portMode = 0,
			lfoRate = 4, lfoWave = 0, lfoDelay = 0, lfoOneshot = 0, lfoUni = 0,
			pms = 0, ams = 0, alms = 0,
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

			var envGate, kill, v, snd, envSum;

			envGate = gate.clip(0, 1);
			kill = EnvGen.kr(Env.asr(0.001, 1, 0.02), killGate, doneAction: 2);

			// 4-op FM, envelopes, LFO: one C++ UGen (lib/ugens/DX100Voice).
			v = DX100Voice.ar(
				hz, envGate, vel, legato, t_trig,
				algo, feedback, dxFeedback,
				transpose, port, portMode,
				lfoRate, lfoWave, lfoDelay, lfoOneshot, lfoUni,
				pms, ams, alms,
				pitchEgAmt, pitchEgRate, pitchEgLevel,
				r1, r2, r3, r4, d1, d2, d3, d4,
				f1, f2, f3, f4, x1, x2, x3, x4,
				w1, w2, w3, w4, l1, l2, l3, l4,
				a1, a2, a3, a4, b1, b2, b3, b4,
				c1, c2, c3, c4, e1, e2, e3, e4,
				g1, g2, g3, g4, k1, k2, k3, k4,
				v1, v2, v3, v4, rateScale
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
			snd = snd * kill * Lag.kr(amp, 0.05) * vel.linlin(0, 1, 0.5, 1.0);
			snd = snd * Lag.kr(voiceScale, 0.03) * Lag.kr(headroom, 0.05);
			Out.ar(out, Pan2.ar(snd, Lag.kr(pan, 0.08)));
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
			\lfoRate, \lfoWave, \lfoDelay, \lfoOneshot, \lfoUni,
			\pms, \ams, \alms,
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
			// NOTE: drive/hiss/bits/srate/glitch/chorus/phaser are NOT
			// here -- they are post-mix insert controls, not voice buses.
		].do({ arg name;
			ctlBus.put(name, Bus.control(context.server));
		});

		// defaults (a serviceable electric bass, DX100 preset 1 territory)
		ctlBus[\algo].setSynchronous(0);
		ctlBus[\feedback].setSynchronous(0.4);
		ctlBus[\dxFeedback].setSynchronous(0);
		ctlBus[\amp].setSynchronous(0.4);
		ctlBus[\pan].setSynchronous(0);
		ctlBus[\transpose].setSynchronous(0);
		ctlBus[\port].setSynchronous(0);
		ctlBus[\portMode].setSynchronous(0);
		ctlBus[\lfoRate].setSynchronous(4);
		ctlBus[\lfoWave].setSynchronous(0);
		ctlBus[\lfoDelay].setSynchronous(0);
		ctlBus[\lfoOneshot].setSynchronous(0);
		ctlBus[\lfoUni].setSynchronous(0);
		ctlBus[\pms].setSynchronous(0);
		ctlBus[\ams].setSynchronous(0);
		ctlBus[\alms].setSynchronous(0);
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

		// voices -> private stereo bus -> gated inserts -> limiter/out.
		fxBus = Bus.audio(context.server, 2);
		gr = ParGroup.tail(context.xg);
		fxGroup = Group.tail(context.xg);
		fxChar = Synth.tail(fxGroup, \dx100char, [\bus, fxBus.index]);
		fxChorus = Synth.tail(fxGroup, \dx100chorus, [\bus, fxBus.index]);
		fxPhaser = Synth.tail(fxGroup, \dx100phaser, [\bus, fxBus.index]);
		fxOut = Synth.tail(fxGroup, \dx100out,
			[\bus, fxBus.index, \out, context.out_b.index]);
		fxAlive = true;
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
					fxChar.run(true);
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
				fxChorus.run(true);
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
				fxPhaser.run(true);
			}, {
				this.sleepFx(fxPhaser, { phaserMix <= 0 });
			});
		});
		[\phaserRate, \phaserWidth].do({ arg name;
			this.addCommand(name, "f", { arg msg;
				fxPhaser.set(name, msg[1]);
			});
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
		syn = voices[id];
		// Reuse whenever this id has a language-side node. Do not test
		// isPlaying: NodeWatcher is false until /n_go, so a retrigger in
		// that window used to spawn a second synth. The old one kept
		// gate=1, dropped out of `voices`, and never freed — CPU compounded.
		if(syn.notNil, {
			syn.set(
				\hz, hz, \vel, vel, \legato, legato,
				\gate, 1, \t_trig, trig, \killGate, 1
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
			\out, fxBus.index,
			\hz, hz, \vel, vel, \legato, legato,
			\gate, 1, \t_trig, trig, \persist, persist, \killGate, 1
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
	sleepFx { arg syn, stillOff;
		SystemClock.sched(0.12, {
			if(fxAlive and: { stillOff.value }, { syn.run(false) });
			nil;
		});
	}

	free {
		fxAlive = false;
		gr.free;
		fxGroup.free;
		fxBus.free;
		ctlBus.do({ arg b; b.free });
	}
}
