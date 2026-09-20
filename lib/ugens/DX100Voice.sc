// DX100Voice — sclang class for the C++ 4-op FM UGen.
// Plugin binary must live in Platform.userExtensionDir + "/dx100/"
// (see lib/ugens/README.md). Class is compiled from dust.

DX100Voice : MultiOutUGen {
	*ar { arg hz = 220, gate = 0, vel = 1, legato = 0,
		algo = 0, feedback = 0, dxFeedback = 0,
		transpose = 0, port = 0, portMode = 0,
		lfoIn = 0, lfoDelay = 0, lfoUni = 0,
		pms = 0, ams = 0, alms = 0,
		pr1 = 0.5, pr2 = 0.5, pr3 = 0.5, pl1 = 0, pl2 = 0, pl3 = 0,
		breath = 0, egBias = 0,
		r1 = 1, r2 = 1, r3 = 1, r4 = 1,
		d1 = 0, d2 = 0, d3 = 0, d4 = 0,
		f1 = 100, f2 = 100, f3 = 100, f4 = 100,
		x1 = 0, x2 = 0, x3 = 0, x4 = 0,
		w1 = 0, w2 = 0, w3 = 0, w4 = 0,
		l1 = 1, l2 = 0, l3 = 0, l4 = 0,
		a1 = 1, a2 = 1, a3 = 1, a4 = 1,
		b1 = 0.4, b2 = 0.4, b3 = 0.4, b4 = 0.4,
		c1 = 0.8, c2 = 0.8, c3 = 0.8, c4 = 0.8,
		e1 = 0.15, e2 = 0.15, e3 = 0.15, e4 = 0.15,
		g1 = 0.5, g2 = 0.5, g3 = 0.5, g4 = 0.5,
		k1 = 0, k2 = 0, k3 = 0, k4 = 0,
		v1 = 0, v2 = 0, v3 = 0, v4 = 0,
		m1 = 0, m2 = 0, m3 = 0, m4 = 0,
		s1 = 0, s2 = 0, s3 = 0, s4 = 0,
		z1 = 0, z2 = 0, z3 = 0, z4 = 0,
		grit = 1, oversample = 1;
		^this.multiNew('audio',
			hz, gate, vel, legato,
			algo, feedback, dxFeedback,
			transpose, port, portMode,
			lfoIn, lfoDelay, lfoUni,
			pms, ams, alms,
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
	}

	init { arg ... theInputs;
		inputs = theInputs;
		^this.initOutputs(2, rate);
	}
}
