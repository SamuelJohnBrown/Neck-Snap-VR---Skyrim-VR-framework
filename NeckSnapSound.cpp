#include "NeckSnapSound.h"

#include "SkyrimVRESLAPI.h"
#include "config.h"

#include "skse64/GameForms.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/NiNodes.h"

namespace NeckSnapVR
{
	namespace
	{
		static constexpr const char* kNeckSnapEsp = "NeckSnap.esp";
		static constexpr UInt32 kNeckSnapSndrBaseFormId = 0x00000800;
		static constexpr UInt32 kNeckSnapSndrFallbackFormId = 0xFE000800;

		struct VRSoundHandle
		{
			UInt32 soundID = 0xFFFFFFFF;
			bool assumeSuccess = false;
			UInt8 pad05 = 0;
			UInt16 pad06 = 0;
			UInt32 state = 0;
		};

		typedef void* (*GetAudioManagerSingletonFn)();
		typedef bool (*BuildSoundDataFromDescriptorFn)(void* manager, VRSoundHandle* handle, void* soundDescriptor, UInt32 flags);
		typedef bool (*SoundHandlePlayFn)(VRSoundHandle* handle);
		typedef void (*SoundHandleSetObjectToFollowFn)(VRSoundHandle* handle, NiAVObject* node);

		static RelocAddr<GetAudioManagerSingletonFn> GetAudioManagerSingleton(0x00C29430);
		static RelocAddr<BuildSoundDataFromDescriptorFn> BuildSoundDataFromDescriptor(0x00C29F60);
		static RelocAddr<SoundHandlePlayFn> SoundHandlePlay(0x00C283E0);
		static RelocAddr<SoundHandleSetObjectToFollowFn> SoundHandleSetObjectToFollow(0x00C289C0);

		BGSSoundDescriptorForm* g_neckSnapSound = nullptr;
		UInt32 g_neckSnapSoundFormId = 0;

		BGSSoundDescriptorForm* ResolveSoundDescriptor(UInt32 formId)
		{
			if (formId == 0) {
				return nullptr;
			}

			TESForm* form = LookupFormByID(formId);
			if (!form) {
				return nullptr;
			}

			return DYNAMIC_CAST(form, TESForm, BGSSoundDescriptorForm);
		}

		const char* GetVictimLogName(Actor* victim)
		{
			if (!victim) {
				return "Unknown NPC";
			}

			const char* name = CALL_MEMBER_FN(victim, GetReferenceName)();
			if (name && name[0] != '\0') {
				return name;
			}

			if (victim->baseForm) {
				const char* baseName = victim->baseForm->GetFullName();
				if (baseName && baseName[0] != '\0') {
					return baseName;
				}
			}

			return "Unknown NPC";
		}

	}  // namespace

	void InitNeckSnapSound()
	{
		g_neckSnapSound = nullptr;
		g_neckSnapSoundFormId = 0;

		const UInt32 resolvedFormId =
			GetFullFormIdFromEspAndFormId(kNeckSnapEsp, kNeckSnapSndrBaseFormId);
		_MESSAGE(
			"Neck Snap VR [sound]: resolving SNDR from %s base=%08X resolved=%08X",
			kNeckSnapEsp,
			kNeckSnapSndrBaseFormId,
			resolvedFormId);

		if (resolvedFormId != 0) {
			g_neckSnapSound = ResolveSoundDescriptor(resolvedFormId);
			if (g_neckSnapSound) {
				g_neckSnapSoundFormId = resolvedFormId;
			}
		}

		if (!g_neckSnapSound) {
			_MESSAGE(
				"Neck Snap VR [sound]: ESP lookup failed, trying fallback SNDR %08X",
				kNeckSnapSndrFallbackFormId);
			g_neckSnapSound = ResolveSoundDescriptor(kNeckSnapSndrFallbackFormId);
			if (g_neckSnapSound) {
				g_neckSnapSoundFormId = kNeckSnapSndrFallbackFormId;
			}
		}

		if (g_neckSnapSound) {
			_MESSAGE("Neck Snap VR [sound]: loaded neck snap SNDR formId=%08X", g_neckSnapSoundFormId);
			LOG_INFO("[Sound] Loaded neck snap SNDR formId=%08X", g_neckSnapSoundFormId);
			return;
		}

		_MESSAGE(
			"Neck Snap VR [sound]: FAILED to load neck snap SNDR from %s (base=%08X fallback=%08X)",
			kNeckSnapEsp,
			kNeckSnapSndrBaseFormId,
			kNeckSnapSndrFallbackFormId);
		LOG_ERR(
			"[Sound] Failed to load neck snap SNDR from %s (base=%08X fallback=%08X)",
			kNeckSnapEsp,
			kNeckSnapSndrBaseFormId,
			kNeckSnapSndrFallbackFormId);
	}

	bool PlayNeckSnapSound(Actor* victim)
	{
		if (!victim) {
			_MESSAGE("Neck Snap VR [sound]: play skipped (null victim).");
			return false;
		}

		if (!g_neckSnapSound) {
			_MESSAGE(
				"Neck Snap VR [sound]: play skipped for '%s' (formId %08X) - SNDR not loaded.",
				GetVictimLogName(victim),
				victim->formID);
			return false;
		}

		void* audioManager = GetAudioManagerSingleton();
		if (!audioManager) {
			_MESSAGE(
				"Neck Snap VR [sound]: play failed for '%s' - BSAudioManager unavailable.",
				GetVictimLogName(victim));
			LOG_ERR("[Sound] BSAudioManager unavailable for neck snap SNDR");
			return false;
		}

		void* soundDescriptor = &g_neckSnapSound->soundDescriptor;
		VRSoundHandle handle{};
		if (!BuildSoundDataFromDescriptor(audioManager, &handle, soundDescriptor, 0x10)) {
			_MESSAGE(
				"Neck Snap VR [sound]: play failed for '%s' - BuildSoundDataFromDescriptor returned false (sndr %08X).",
				GetVictimLogName(victim),
				g_neckSnapSoundFormId);
			LOG_ERR("[Sound] BuildSoundDataFromDescriptor failed for neck snap SNDR");
			return false;
		}

		const bool hasFollowNode = victim->GetNiNode() != nullptr;
		if (NiNode* node = victim->GetNiNode()) {
			SoundHandleSetObjectToFollow(&handle, node);
		}

		if (SoundHandlePlay(&handle)) {
			_MESSAGE(
				"Neck Snap VR [sound]: PLAYED neck snap SNDR %08X on '%s' (victim formId %08X, followNode=%s).",
				g_neckSnapSoundFormId,
				GetVictimLogName(victim),
				victim->formID,
				hasFollowNode ? "Y" : "N");
			LOG_INFO("[Sound] Playing neck snap SNDR on victim formId=%08X", g_neckSnapSoundFormId);
			return true;
		}

		_MESSAGE(
			"Neck Snap VR [sound]: play failed for '%s' - SoundHandlePlay returned false (sndr %08X).",
			GetVictimLogName(victim),
			g_neckSnapSoundFormId);
		LOG_ERR("[Sound] Failed to play neck snap SNDR");
		return false;
	}

}
