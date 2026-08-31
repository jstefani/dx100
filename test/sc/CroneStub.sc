// Minimal stubs so norns engine classes compile outside the norns tree.
CroneEngine {
	var <>context, <>doneCallback;
	*new { arg context, doneCallback;
		^super.newCopyArgs(context, doneCallback).alloc;
	}
	alloc {}
	addCommand { arg name, format, func; }
	free {}
}
