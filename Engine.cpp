#include "Engine.h"

#include "OnFrame.h"

namespace NeckSnapVR
{
	SKSETrampolineInterface* g_trampolineInterface = nullptr;

	HiggsPluginAPI::IHiggsInterface001* higgsInterface;
	vrikPluginApi::IVrikInterface001* vrikInterface;

	SkyrimVRESLPluginAPI::ISkyrimVRESLInterface001* skyrimVRESLInterface;

	void StartMod()
	{
		InstallFrameHook();
		LOG_INFO("[Init] Neck Snap VR started");
	}
}
