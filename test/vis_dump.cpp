// Rend la démo avec le moteur et écrit les VisFrame (60 Hz) en JSON pour tester l'interface hors plugin.
#include "audio_file.h"
#include "dsp/mimic_dsp.h"
#include <cstdio>
using namespace mimic::dsp;
int main (int argc, char** argv)
{
	std::string err;
	auto clip = mimic::loadAudioClip (argv[1], err);
	if (!clip) return 1;
	Engine e; e.prepare (48000.0, 1024); e.setParams (ParamValues {});
	const int blk = 800, total = (int) clip->frames () / blk * blk;
	std::vector<float> l (blk), r (blk);
	std::FILE* f = std::fopen (argv[2], "w");
	std::fprintf (f, "[");
	bool first = true;
	for (int pos = 0; pos < total; pos += blk)
	{
		NoteEvent ev[3]; int n = 0;
		if (pos == 0) { ev[n++] = {0, true, 45, 1.0f}; ev[n++] = {0, true, 57, 1.0f}; ev[n++] = {0, true, 64, 0.8f}; }
		e.process (clip->l.data () + pos, clip->r.data () + pos, l.data (), r.data (), blk, ev, n);
		const VisFrame& v = e.visual ();
		std::fprintf (f, "%s{\"t\":\"vis\",\"s\":[", first ? "" : ","); first = false;
		for (int i = 0; i < 64; ++i) std::fprintf (f, "%s%.3f", i ? "," : "", v.src[i]);
		std::fprintf (f, "],\"y\":[");
		for (int i = 0; i < 64; ++i) std::fprintf (f, "%s%.3f", i ? "," : "", v.synth[i]);
		std::fprintf (f, "],\"e\":%.3f,\"l\":%.3f,\"j\":%.3f,\"f\":%.0f,\"c\":%.3f,\"i\":[%.3f,%.3f],\"o\":[%.3f,%.3f]}", v.energy, v.lowE, v.jaw, v.freeze, v.clip, v.inPk[0], v.inPk[1], v.outPk[0], v.outPk[1]);
	}
	std::fprintf (f, "]");
	std::fclose (f);
	return 0;
}
