#include "editor_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace mimic {

namespace {
const ParamID kUiParams[] = {kParamMorph, kParamCarve, kParamGroove, kParamWet, kParamOutput, kParamFreeze, kParamSource, kParamInput, kParamWave, kParamBlend};

const char* findKey (const std::string& j, const char* key)
{
	const std::string pat = std::string ("\"") + key + "\"";
	const auto p = j.find (pat);
	if (p == std::string::npos)
		return nullptr;
	auto q = j.find (':', p + pat.size ());
	if (q == std::string::npos)
		return nullptr;
	++q;
	while (q < j.size () && (j[q] == ' ' || j[q] == '\t'))
		++q;
	return j.c_str () + q;
}
} // namespace

bool jsonString (const std::string& j, const char* key, std::string& out)
{
	const char* p = findKey (j, key);
	if (!p || *p != '"')
		return false;
	out.clear ();
	for (++p; *p && *p != '"'; ++p)
	{
		if (*p == '\\' && p[1])
			++p;
		out += *p;
	}
	return true;
}

bool jsonNumber (const std::string& j, const char* key, double& out)
{
	const char* p = findKey (j, key);
	if (!p)
		return false;
	char* end = nullptr;
	out = std::strtod (p, &end);
	return end != p;
}

EditorBridge::EditorBridge (Controller* c, Sender send) : c_ (c), send_ (std::move (send))
{
	c_->setObserver (this);
}

EditorBridge::~EditorBridge ()
{
	c_->setObserver (nullptr);
}

std::string EditorBridge::paramJson (ParamID id, double v)
{
	char b[96];
	std::snprintf (b, sizeof b, "{\"t\":\"param\",\"id\":%u,\"v\":%.6f}", static_cast<unsigned> (id), v);
	return b;
}

std::string EditorBridge::visJson (const dsp::VisFrame& f)
{
	std::string s;
	s.reserve (1200);
	char b[48];
	s += "{\"t\":\"vis\",\"s\":[";
	for (int i = 0; i < dsp::VisFrame::kBands; ++i)
	{
		std::snprintf (b, sizeof b, i ? ",%.3f" : "%.3f", static_cast<double> (f.src[i]));
		s += b;
	}
	s += "],\"y\":[";
	for (int i = 0; i < dsp::VisFrame::kBands; ++i)
	{
		std::snprintf (b, sizeof b, i ? ",%.3f" : "%.3f", static_cast<double> (f.synth[i]));
		s += b;
	}
	char tail[256];
	std::snprintf (tail, sizeof tail, "],\"e\":%.3f,\"l\":%.3f,\"j\":%.3f,\"f\":%.0f,\"c\":%.3f,\"i\":[%.3f,%.3f],\"o\":[%.3f,%.3f]}",
	               static_cast<double> (f.energy), static_cast<double> (f.lowE), static_cast<double> (f.jaw), static_cast<double> (f.freeze),
	               static_cast<double> (f.clip), static_cast<double> (f.inPk[0]), static_cast<double> (f.inPk[1]), static_cast<double> (f.outPk[0]),
	               static_cast<double> (f.outPk[1]));
	s += tail;
	return s;
}

void EditorBridge::sendAllParams ()
{
	for (ParamID id : kUiParams)
		send_ (paramJson (id, c_->getParamNormalized (id)));
}

void EditorBridge::onParam (ParamID id, double v)
{
	if (!ready_ || userEdit_)
		return; // pas d'écho vers la page pendant que l'utilisateur tourne le potard
	for (ParamID p : kUiParams)
		if (p == id)
		{
			send_ (paramJson (id, v));
			return;
		}
}

void EditorBridge::onWebMessage (const std::string& json)
{
	std::string t;
	if (!jsonString (json, "t", t))
		return;
	if (t == "ready")
	{
		ready_ = true;
		sendAllParams ();
		return;
	}
	double idD = 0, v = 0;
	if (!jsonNumber (json, "id", idD))
		return;
	const auto id = static_cast<ParamID> (idD);
	bool known = false;
	for (ParamID p : kUiParams)
		known = known || p == id;
	if (!known)
		return;
	if (t == "begin")
		c_->beginEdit (id);
	else if (t == "set" && jsonNumber (json, "v", v))
	{
		v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
		userEdit_ = true;
		c_->setParamNormalized (id, v);
		c_->performEdit (id, v);
		userEdit_ = false;
	}
	else if (t == "end")
		c_->endEdit (id);
}

void EditorBridge::onDroppedFile (const std::string& utf8)
{
	if (!utf8.empty ())
		c_->loadFile (utf8);
}

void EditorBridge::tick ()
{
	if (!ready_)
		return;
	dsp::VisFrame f;
	if (c_->visual ().read (f) && f.seq != lastSeq_)
	{
		lastSeq_ = f.seq;
		send_ (visJson (f));
	}
}

} // namespace mimic
