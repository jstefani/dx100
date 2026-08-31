// Do two simultaneous synths each get their OWN LocalIn/LocalOut bus?
// Each synth integrates a constant into a feedback loop at a different
// rate. If the loops are private, each converges to its own value.
// If shared, they interfere.
s = Server.default;
s.waitForBoot({
	SynthDef(\fbtest, { arg out=0, step=0.1;
		var v = LocalIn.ar(1) + step;
		v = v.clip2(1000);
		LocalOut.ar(v);
		Out.kr(out, A2K.kr(v));
	}).add;
	s.sync;
	Routine({
		var b1 = Bus.control(s,1), b2 = Bus.control(s,1), g;
		g = ParGroup.tail(s.defaultGroup);
		s.sync;
		Synth(\fbtest, [\out, b1.index, \step, 1.0], g);
		Synth(\fbtest, [\out, b2.index, \step, 2.0], g);
		0.4.wait;
		("voice A (step 1.0) = " ++ b1.getSynchronous).postln;
		("voice B (step 2.0) = " ++ b2.getSynchronous).postln;
		"(ratio should be ~2.0 if loops are private)".postln;
		("ratio = " ++ (b2.getSynchronous / b1.getSynchronous).round(0.001)).postln;
		0.exit;
	}).play;
});
