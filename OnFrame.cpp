#include "OnFrame.h"

#include "Helper.h"
#include "NeckSnapDetection.h"
#include "config.h"

#include "skse64/GameAPI.h"
#include "skse64/GameMenus.h"
#include "skse64_common/BranchTrampoline.h"
#include "skse64_common/Relocation.h"

namespace NeckSnapVR
{
	namespace
	{
		typedef void (*OriginalOnFrameFn)();
		OriginalOnFrameFn g_originalOnFrame = nullptr;

		bool ShouldRunDetection()
		{
			PlayerCharacter* player = *g_thePlayer;
			if (!player || !player->loadedState || !player->loadedState->node) {
				return false;
			}

			MenuManager* menuManager = MenuManager::GetSingleton();
			if (menuManager) {
				BSFixedString consoleMenu("Console");
				BSFixedString mainMenu("MainMenu");
				if (menuManager->IsMenuOpen(&consoleMenu) || menuManager->IsMenuOpen(&mainMenu)) {
					return false;
				}
			}

			return true;
		}

		void OnFrameUpdate()
		{
			if (ShouldRunDetection()) {
				UpdateNeckSnapDetection();
			}

			if (g_originalOnFrame) {
				g_originalOnFrame();
			}
		}

	}  // namespace

	void InstallFrameHook()
	{
		RelocAddr<std::uintptr_t> onFrameBase(0x005BAB10);
		const std::uintptr_t hookSite = onFrameBase + 0x7EE;

		g_originalOnFrame = reinterpret_cast<OriginalOnFrameFn>(
			Write5Call(hookSite, reinterpret_cast<std::uintptr_t>(&OnFrameUpdate)));

		LOG_INFO("[Init] Frame hook installed at 0x%llX", static_cast<unsigned long long>(hookSite));
	}

}
