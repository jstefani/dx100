// Render SinOsc with a large DC phase offset, raw vs wrapped, and compare.
// If SinOsc's phase input breaks past 8pi, these differ.
s = Server.default;
s.options.numOutputBusChannels = 2;
s.waitForBoot({
	var dur = 0.2, phases = [1, 7, 20, 30, 60, 120];
	Routine({
		phases.do({ arg p;
			var b1, b2, d1, d2, maxdiff;
			b1 = Buffer.alloc(s, 1024, 1); b2 = Buffer.alloc(s, 1024, 1);
			s.sync;
			// record raw
			{ RecordBuf.ar(SinOsc.ar(440, DC.ar(p)), b1, loop: 0, doneAction: 2); Silent.ar }.play;
			0.2.wait;
			{ RecordBuf.ar(SinOsc.ar(440, DC.ar(p).wrap(-pi,pi)), b2, loop: 0, doneAction: 2); Silent.ar }.play;
			0.2.wait;
			s.sync;
			d1 = nil; d2 = nil;
			b1.loadToFloatArray(action: { arg a; d1 = a });
			b2.loadToFloatArray(action: { arg a; d2 = a });
			while({ d1.isNil or: { d2.isNil } }, { 0.05.wait });
			maxdiff = (d1 - d2).abs.maxItem;
			("phase % : maxdiff = %".format(
				p.asString.padRight(5), maxdiff)).postln;
			b1.free; b2.free;
		});
		"=== UGEN TEST DONE ===".postln;
		0.exit;
	}).play;
});
