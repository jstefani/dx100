// Measure the quantization ERROR (noise) in each approach.
// error = quantized - clean. More error energy = more audible grit.
s = Server.default;
s.waitForBoot({
	Routine({
		var e1 = Bus.control(s,1), e2 = Bus.control(s,1);
		var e1b = Bus.control(s,1), e2b = Bus.control(s,1);
		var q = 64;
		var mk = { arg nvoices, bPer, bSum;
			var freqs = [220,277.18,329.63,440,554.37,659.26,880,1108.7].keep(nvoices);
			{ var v = freqs.collect({ arg f; SinOsc.ar(f) * (1/nvoices) });
			  var clean = Mix(v);
			  var perVoice = Mix(v.collect({ arg x; (x*q).round(1.0)/q }));
			  var summed  = (clean*q).round(1.0)/q;
			  Out.kr(bPer.index, RunningSum.rms(perVoice - clean, 2048));
			  Out.kr(bSum.index, RunningSum.rms(summed  - clean, 2048));
			  Silent.ar }.play;
		};
		mk.(1, e1, e2);
		0.5.wait;
		("1 voice : per-voice err = " ++ e1.getSynchronous.round(0.000001)
			++ "   sum-then err = " ++ e2.getSynchronous.round(0.000001)).postln;
		mk.(8, e1b, e2b);
		0.5.wait;
		("8 voices: per-voice err = " ++ e1b.getSynchronous.round(0.000001)
			++ "   sum-then err = " ++ e2b.getSynchronous.round(0.000001)).postln;
		"".postln;
		("8-voice error RATIO (per-voice / sum-then) = " ++
			(e1b.getSynchronous / e2b.getSynchronous).round(0.01)).postln;
		0.exit;
	}).play;
});
