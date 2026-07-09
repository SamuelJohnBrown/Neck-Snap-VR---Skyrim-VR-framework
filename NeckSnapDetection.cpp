#include "NeckSnapDetection.h"

#include "Engine.h"
#include "NeckSnapSound.h"
#include "CrimeBounty.h"
#include "config.h"

#include "skse64/GameAPI.h"
#include "skse64/GameData.h"
#include "skse64/GameForms.h"
#include "skse64/GameObjects.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameTypes.h"
#include "skse64/GameFormComponents.h"
#include "skse64/GameExtraData.h"
#include "skse64/NiObjects.h"
#include "skse64/NiNodes.h"
#include "skse64/NiTypes.h"
#include "skse64/GameVR.h"

#include <cmath>
#include <cstring>

namespace NeckSnapVR
{
	namespace
	{
		constexpr UInt32 kHealthActorValue = 24;

		UInt32 g_lastSnapReadyNpcFormId = 0;
		UInt32 g_lastDualHeadGrabNpcFormId = 0;
		UInt32 g_lastNeckSnappedNpcFormId = 0;
		UInt32 g_loggedDualHeadGrabNpcFormId = 0;
		UInt32 g_loggedLeftGrabDebugNpcFormId = 0;
		UInt32 g_loggedRightGrabDebugNpcFormId = 0;
		UInt32 g_loggedBothHandsGrabDebugNpcFormId = 0;
		UInt32 g_dualGrabLossFrames = 0;
		UInt32 g_oppositeMotionFrames = 0;
		bool g_hasPrevHandPos = false;
		bool g_hasBaselineHeadRot = false;
		NiMatrix33 g_baselineHeadRot{};
		NiPoint3 g_prevLeftHandPos{};
		NiPoint3 g_prevRightHandPos{};
		UInt32 g_motionDebugTick = 0;
		float g_peakHeadTwistDegrees = 0.0f;

		void UpdateNeckSnapMotion(PlayerCharacter* player, Actor* npc);
		void KillActorFromNeckSnap(Actor* victim);
		bool ExecuteNeckSnap(Actor* npc);
		const char* GetActorLogName(Actor* actor);
		const char* GetFormLogName(TESForm* form);
		void LogGrabbedNpcDebug(Actor* npc, PlayerCharacter* player, const char* context);
		void UpdateGrabbedNpcDebugLogging(PlayerCharacter* player);
		void EndDualHeadGrabSession(const char* reason = nullptr);
		void TryLatchFromDualHeadGrab(PlayerCharacter* player);
		NiAVObject* FindHeadBone(Actor* actor);
		NiAVObject* FindGrabbedBone(Actor* npc, NiObject* rigidBody);
		NiObject* GetRigidBodyFromBone(NiAVObject* bone);
		bool IsGrabbedActor(Actor* npc, TESObjectREFR* grabbedRefr);
		bool IsNonHeadBodyRigidBody(Actor* npc, NiObject* rigidBody);
		bool IsGrabbedRigidBodyOnHeadBone(Actor* npc, NiObject* rigidBody);
		bool IsRigidBodyNearHeadBone(Actor* npc, NiObject* rigidBody);
		bool IsHandGrabbingNpcHead(Actor* npc, bool isLeftHand);
		bool HasAnyHandGrabbingNpcHead(Actor* npc);
		bool IsPlayerBehindActor(Actor* npc, Actor* player, float maxDistance);
		Actor* ResolveGrabbedActor(TESObjectREFR* refr);
		NiPoint3 MatrixAxis(const NiMatrix33& rot, int axisIndex);

		float VecDot(const NiPoint3& a, const NiPoint3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		float VecLength(const NiPoint3& v)
		{
			return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
		}

		NiPoint3 Normalize(const NiPoint3& p)
		{
			const float len = VecLength(p);
			if (len < 0.0001f) {
				return NiPoint3(0.0f, 0.0f, 0.0f);
			}

			return NiPoint3(p.x / len, p.y / len, p.z / len);
		}

		NiPoint3 NormalizeHorizontal(const NiPoint3& p)
		{
			const float len = sqrtf(p.x * p.x + p.y * p.y);
			if (len < 0.0001f) {
				return NiPoint3(0.0f, 0.0f, 0.0f);
			}

			return NiPoint3(p.x / len, p.y / len, 0.0f);
		}

		NiPoint3 ActorForwardHorizontal(const Actor* actor)
		{
			if (!actor) {
				return NiPoint3(0.0f, 1.0f, 0.0f);
			}

			const float yawRad = actor->rot.z * (MATH_PI / 180.0f);
			return NormalizeHorizontal(NiPoint3(-sinf(yawRad), cosf(yawRad), 0.0f));
		}

		NiPoint3 BoneForwardHorizontal(NiAVObject* bone)
		{
			if (!bone) {
				return NiPoint3(0.0f, 1.0f, 0.0f);
			}

			return NormalizeHorizontal(MatrixAxis(bone->m_worldTransform.rot, 1));
		}

		float Distance3D(const NiPoint3& a, const NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return sqrtf(dx * dx + dy * dy + dz * dz);
		}

		NiPoint3 GetActorReferencePos(Actor* actor)
		{
			if (!actor) {
				return NiPoint3();
			}

			NiAVObject* head = FindHeadBone(actor);
			if (head) {
				return head->m_worldTransform.pos;
			}

			return static_cast<TESObjectREFR*>(actor)->pos;
		}

		bool IsActorUsingFurniture(Actor* actor)
		{
			if (!actor || !actor->processManager || !actor->processManager->middleProcess) {
				return false;
			}

			const UInt32 furnitureHandle = actor->processManager->middleProcess->furnitureHandle;
			return furnitureHandle != 0 && furnitureHandle != *g_invalidRefHandle;
		}

		bool EvaluateBehindCheck(
			const NiPoint3& referencePos,
			const NiPoint3& forwardHorizontal,
			const NiPoint3& playerPos,
			float maxDistance,
			float maxHeightDiff)
		{
			const NiPoint3 delta = NiPoint3(
				playerPos.x - referencePos.x,
				playerPos.y - referencePos.y,
				playerPos.z - referencePos.z);

			const float flatDistSq = delta.x * delta.x + delta.y * delta.y;
			if (flatDistSq > maxDistance * maxDistance) {
				return false;
			}

			if (fabsf(delta.z) > maxHeightDiff) {
				return false;
			}

			if (flatDistSq < 1.0f) {
				return false;
			}

			const NiPoint3 toPlayer = NormalizeHorizontal(delta);
			const float behindDot = VecDot(forwardHorizontal, toPlayer);
			return behindDot <= fMinBehindDot;
		}

		NiPoint3 DirectionHorizontal(const NiPoint3& from, const NiPoint3& to)
		{
			return NormalizeHorizontal(NiPoint3(to.x - from.x, to.y - from.y, 0.0f));
		}

		struct ProcessLists
		{
			std::uint8_t pad001[0x2F];
			tArray<UInt32> highActorHandles;
			tArray<UInt32> lowActorHandles;
			tArray<UInt32> middleHighActorHandles;
			tArray<UInt32> middleLowActorHandles;
		};

		ProcessLists* GetProcessLists()
		{
			static RelocPtr<ProcessLists*> singleton(0x01F831B0);
			return *singleton;
		}

		Actor* ResolveActorFromHandle(UInt32 handle)
		{
			if (handle == *g_invalidRefHandle) {
				return nullptr;
			}

			NiPointer<TESObjectREFR> refr;
			UInt32 mutableHandle = handle;
			if (!LookupREFRByHandle(mutableHandle, refr) || !refr) {
				return nullptr;
			}

			return DYNAMIC_CAST(refr, TESObjectREFR, Actor);
		}

		Actor* FindActorByFormId(UInt32 formId)
		{
			if (formId == 0) {
				return nullptr;
			}

			ProcessLists* processLists = GetProcessLists();
			if (!processLists) {
				return nullptr;
			}

			const tArray<UInt32>* actorLists[] = {
				&processLists->highActorHandles,
				&processLists->middleHighActorHandles,
				&processLists->middleLowActorHandles,
			};

			for (const tArray<UInt32>* actorList : actorLists) {
				if (!actorList || !actorList->entries || actorList->count == 0) {
					continue;
				}

				for (UInt32 i = 0; i < actorList->count; ++i) {
					Actor* actor = ResolveActorFromHandle(actorList->entries[i]);
					if (actor && actor->formID == formId) {
						return actor;
					}
				}
			}

			return nullptr;
		}

		bool IsUnarmedWeapon(TESForm* item)
		{
			if (!item || !item->IsWeapon()) {
				return false;
			}

			TESObjectWEAP* weapon = DYNAMIC_CAST(item, TESForm, TESObjectWEAP);
			if (!weapon) {
				return false;
			}

			const UInt8 weaponType = weapon->type();
			return weaponType == TESObjectWEAP::GameData::kType_HandToHandMelee
				|| weaponType == TESObjectWEAP::GameData::kType_H2H;
		}

		bool HasRealWeaponEquipped(Actor* actor)
		{
			TESForm* left = actor->GetEquippedObject(true);
			TESForm* right = actor->GetEquippedObject(false);

			if (left && left->IsWeapon() && !IsUnarmedWeapon(left)) {
				return true;
			}

			if (right && right->IsWeapon() && !IsUnarmedWeapon(right)) {
				return true;
			}

			return false;
		}

		bool HasSpellEquipped(Actor* actor)
		{
			TESForm* left = actor->GetEquippedObject(true);
			TESForm* right = actor->GetEquippedObject(false);

			if (left && (left->formType == kFormType_Spell || left->formType == kFormType_Shout)) {
				return true;
			}

			if (right && (right->formType == kFormType_Spell || right->formType == kFormType_Shout)) {
				return true;
			}

			return false;
		}

		bool IsPlayerUnarmed(Actor* player)
		{
			return !HasRealWeaponEquipped(player) && !HasSpellEquipped(player);
		}

		bool IsHumanoidNpc(Actor* actor)
		{
			if (!actor || actor->IsPlayerRef()) {
				return false;
			}

			TESNPC* npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
			if (!npc) {
				return false;
			}

			TESRace* race = actor->race;
			if (!race) {
				return false;
			}

			const UInt32 raceFlags = race->data.raceFlags;
			if ((raceFlags & TESRace::kRace_FaceGenHead) == 0) {
				return false;
			}

			if (raceFlags & (TESRace::kRace_Child | TESRace::kRace_Flies | TESRace::kRace_Immobile)) {
				return false;
			}

			return true;
		}

		BGSKeyword* GetActorTypeUndeadKeyword()
		{
			static BGSKeyword* cached = nullptr;
			static bool resolved = false;
			if (resolved) {
				return cached;
			}

			resolved = true;
			DataHandler* dataHandler = DataHandler::GetSingleton();
			if (!dataHandler) {
				return nullptr;
			}

			for (UInt32 i = 0; i < dataHandler->keywords.count; ++i) {
				BGSKeyword* keyword = nullptr;
				if (!dataHandler->keywords.GetNthItem(i, keyword) || !keyword) {
					continue;
				}

				const char* editorId = keyword->keyword.c_str();
				if (editorId && _stricmp(editorId, "ActorTypeUndead") == 0) {
					cached = keyword;
					break;
				}
			}

			return cached;
		}

		bool FormHasKeyword(TESForm* form, BGSKeyword* keyword)
		{
			if (!form || !keyword) {
				return false;
			}

			BGSKeywordForm* keywordForm = DYNAMIC_CAST(form, TESForm, BGSKeywordForm);
			return keywordForm && keywordForm->HasKeyword(keyword);
		}

		bool IsUndeadActor(Actor* actor)
		{
			if (!actor) {
				return false;
			}

			BGSKeyword* undeadKeyword = GetActorTypeUndeadKeyword();
			if (!undeadKeyword) {
				return false;
			}

			if (FormHasKeyword(actor->baseForm, undeadKeyword)) {
				return true;
			}

			return FormHasKeyword(actor->race, undeadKeyword);
		}

		bool IsEssentialActor(Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (actor->flags2 & Actor::kFlag_kEssential) {
				return true;
			}

			TESNPC* npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
			return npc && (npc->actorData.flags & (1 << 1));
		}

		bool IsValidNeckSnapVictim(Actor* actor)
		{
			return IsHumanoidNpc(actor) && !IsActiveFollower(actor);
		}

		void ClearRuntimeEssential(Actor* actor)
		{
			if (!actor) {
				return;
			}

			// Only clear the per-actor runtime flag. Do not touch actor values here;
			// SetBase/SetCurrent on kIsEssential (354) crashes on some NPCs.
			actor->flags2 &= ~Actor::kFlag_kEssential;
		}

		void KillActorFromNeckSnap(Actor* victim)
		{
			if (!victim || victim->IsDead(1)) {
				return;
			}

			const float maxHealth = victim->actorValueOwner.GetMaximum(kHealthActorValue);
			const float killDamage = maxHealth > 1.0f ? maxHealth * 100.0f : 99999.0f;
			victim->actorValueOwner.RestoreActorValue(Actor::kDamage, kHealthActorValue, -killDamage);
		}

		bool ExecuteNeckSnap(Actor* npc)
		{
			if (!npc || npc->IsDead(1)) {
				return false;
			}

			if (IsUndeadActor(npc)) {
				_MESSAGE(
					"Neck Snap VR [snap]: blocked on '%s' (formId %08X) - undead.",
					GetActorLogName(npc),
					npc->formID);
				return false;
			}

			if (IsActiveFollower(npc)) {
				_MESSAGE(
					"Neck Snap VR [snap]: blocked on '%s' (formId %08X) - follower.",
					GetActorLogName(npc),
					npc->formID);
				return false;
			}

			const bool essential = IsEssentialActor(npc);
			if (essential && !iAllowEssentialVictims) {
				_MESSAGE(
					"Neck Snap VR [snap]: blocked on '%s' (formId %08X) - essential (set AllowEssentialVictims=1 to allow).",
					GetActorLogName(npc),
					npc->formID);
				LOG_INFO(
					"[snap] blocked essential victim '%s' (formId %08X).",
					GetActorLogName(npc),
					npc->formID);
				return false;
			}

			if (essential) {
				ClearRuntimeEssential(npc);
				_MESSAGE(
					"Neck Snap VR [snap]: cleared essential flag on '%s' (formId %08X) before kill.",
					GetActorLogName(npc),
					npc->formID);
			}

			const bool soundPlayed = PlayNeckSnapSound(npc);
			_MESSAGE(
				"Neck Snap VR [sound]: ExecuteNeckSnap on '%s' (formId %08X) soundPlayed=%s.",
				GetActorLogName(npc),
				npc->formID,
				soundPlayed ? "YES" : "NO");

			PlayerCharacter* player = *g_thePlayer;
			if (player) {
				ApplyMurderBountyIfWitnessed(player, npc);
			}

			KillActorFromNeckSnap(npc);
			return true;
		}

		float GetNeckSnapRange(Actor* npc)
		{
			if (npc->race) {
				return npc->race->data.handReach + fReachMargin;
			}

			return fDefaultReach + fReachMargin;
		}

		const char* GetActorLogName(Actor* actor)
		{
			const char* name = CALL_MEMBER_FN(actor, GetReferenceName)();
			if (name && name[0] != '\0') {
				return name;
			}

			if (actor->baseForm) {
				const char* baseName = actor->baseForm->GetFullName();
				if (baseName && baseName[0] != '\0') {
					return baseName;
				}
			}

			return "Unknown NPC";
		}

		const char* GetFormLogName(TESForm* form)
		{
			if (!form) {
				return "none";
			}

			// Head parts and some equipped entries are not safe to query via GetFullName.
			switch (form->formType) {
			case kFormType_Armor:
			case kFormType_Race:
			case kFormType_NPC:
			case kFormType_Weapon:
			case kFormType_Faction:
			case kFormType_Spell:
			case kFormType_Book:
				break;
			default:
				return "unnamed";
			}

			const char* fullName = form->GetFullName();
			if (fullName && fullName[0] != '\0') {
				return fullName;
			}

			const char* name = form->GetName();
			if (name && name[0] != '\0') {
				return name;
			}

			return "unnamed";
		}

		void SafeFormLabel(TESForm* form, char* outBuffer, size_t bufferSize)
		{
			if (!outBuffer || bufferSize == 0) {
				return;
			}

			outBuffer[0] = '\0';
			if (!form) {
				_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "none");
				return;
			}

			switch (form->formType) {
			case kFormType_Armor:
			case kFormType_Race:
			case kFormType_NPC:
			case kFormType_Weapon:
			case kFormType_Faction:
			case kFormType_Spell:
			case kFormType_Book:
			{
				const char* fullName = form->GetFullName();
				if (fullName && fullName[0] != '\0') {
					_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "%s", fullName);
					return;
				}

				const char* name = form->GetName();
				if (name && name[0] != '\0') {
					_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "%s", name);
					return;
				}
				break;
			}
			default:
				break;
			}

			_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "form:%08X type:%u", form->formID, form->formType);
		}

		class MatchBySlot : public FormMatcher
		{
			UInt32 m_mask;

		public:
			explicit MatchBySlot(UInt32 mask) : m_mask(mask) {}

			bool Matches(TESForm* form) const override
			{
				if (!form) {
					return false;
				}

				BGSBipedObjectForm* biped = DYNAMIC_CAST(form, TESForm, BGSBipedObjectForm);
				return biped && (biped->data.parts & m_mask) != 0;
			}
		};

		TESForm* GetActorWornForm(Actor* actor, UInt32 slotMask)
		{
			if (!actor) {
				return nullptr;
			}

			ExtraContainerChanges* containerChanges =
				static_cast<ExtraContainerChanges*>(actor->extraData.GetByType(kExtraData_ContainerChanges));
			if (!containerChanges) {
				return nullptr;
			}

			MatchBySlot matcher(slotMask);
			return containerChanges->FindEquipped(matcher).pForm;
		}

		const char* GetHumanoidNpcRejectReason(Actor* actor)
		{
			if (!actor) {
				return "null actor";
			}

			if (actor->IsPlayerRef()) {
				return "is player";
			}

			TESNPC* npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
			if (!npc) {
				return "base form is not NPC";
			}

			TESRace* race = actor->race;
			if (!race) {
				return "missing race";
			}

			const UInt32 raceFlags = race->data.raceFlags;
			if ((raceFlags & TESRace::kRace_FaceGenHead) == 0) {
				return "race missing FaceGenHead";
			}

			if (raceFlags & TESRace::kRace_Child) {
				return "race is child";
			}

			if (raceFlags & TESRace::kRace_Flies) {
				return "race flies";
			}

			if (raceFlags & TESRace::kRace_Immobile) {
				return "race immobile";
			}

			return nullptr;
		}

		const char* GetGrabBoneName(Actor* npc, NiObject* rigidBody)
		{
			if (!npc || !rigidBody) {
				return "none";
			}

			NiAVObject* grabbedBone = FindGrabbedBone(npc, rigidBody);
			if (!grabbedBone || !grabbedBone->m_name || grabbedBone->m_name[0] == '\0') {
				return "unknown";
			}

			return grabbedBone->m_name;
		}

		void AppendHandGrabDebugLine(
			Actor* npc,
			bool isLeftHand,
			char* outBuffer,
			size_t bufferSize)
		{
			if (!outBuffer || bufferSize == 0) {
				return;
			}

			outBuffer[0] = '\0';
			if (!higgsInterface) {
				_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "%s=no_higgs", isLeftHand ? "L" : "R");
				return;
			}

			if (!higgsInterface->IsHoldingObject(isLeftHand)) {
				_snprintf_s(outBuffer, bufferSize, _TRUNCATE, "%s=idle", isLeftHand ? "L" : "R");
				return;
			}

			TESObjectREFR* grabbedRefr = higgsInterface->GetGrabbedObject(isLeftHand);
			NiObject* grabbedRigidBody = higgsInterface->GetGrabbedRigidBody(isLeftHand);
			const bool sameActor = IsGrabbedActor(npc, grabbedRefr);
			const char* grabBoneName = GetGrabBoneName(npc, grabbedRigidBody);
			const bool nonHeadBody = grabbedRigidBody && IsNonHeadBodyRigidBody(npc, grabbedRigidBody);
			const bool onHeadBone = grabbedRigidBody && IsGrabbedRigidBodyOnHeadBone(npc, grabbedRigidBody);
			const bool nearHead = grabbedRigidBody && IsRigidBodyNearHeadBone(npc, grabbedRigidBody);
			const bool countsAsHeadGrab = IsHandGrabbingNpcHead(npc, isLeftHand);

			NiAVObject* head = FindHeadBone(npc);
			NiObject* headRigidBody = GetRigidBodyFromBone(head);
			float headDistance = -1.0f;
			if (head && grabbedRigidBody) {
				NiAVObject* grabbedBone = FindGrabbedBone(npc, grabbedRigidBody);
				if (grabbedBone) {
					headDistance = Distance3D(head->m_worldTransform.pos, grabbedBone->m_worldTransform.pos);
				}
			}

			_snprintf_s(
				outBuffer,
				bufferSize,
				_TRUNCATE,
				"%s=grab sameActor=%s bone='%s' nonHeadBody=%s onHeadBone=%s nearHead=%s headDist=%.1f headRbMatch=%s countsAsHead=%s",
				isLeftHand ? "L" : "R",
				sameActor ? "Y" : "N",
				grabBoneName,
				nonHeadBody ? "Y" : "N",
				onHeadBone ? "Y" : "N",
				nearHead ? "Y" : "N",
				headDistance,
				(headRigidBody && grabbedRigidBody == headRigidBody) ? "Y" : "N",
				countsAsHeadGrab ? "Y" : "N");
		}

		void LogGrabbedNpcDebug(Actor* npc, PlayerCharacter* player, const char* context)
		{
			if (!npc || !player) {
				return;
			}

			TESRace* race = npc->race;
			const char* humanoidReject = GetHumanoidNpcRejectReason(npc);
			const bool validVictim = IsValidNeckSnapVictim(npc);
			const float snapRange = GetNeckSnapRange(npc);
			const bool behind = IsPlayerBehindActor(npc, player, snapRange);
			NiAVObject* headBone = FindHeadBone(npc);

			TESForm* headArmor = GetActorWornForm(npc, BGSBipedObjectForm::kPart_Head);
			TESForm* hair = GetActorWornForm(npc, BGSBipedObjectForm::kPart_Hair);
			TESForm* circlet = GetActorWornForm(npc, BGSBipedObjectForm::kPart_Circlet);
			TESForm* bodyArmor = GetActorWornForm(npc, BGSBipedObjectForm::kPart_Body);

			char leftGrabLine[512]{};
			char rightGrabLine[512]{};
			AppendHandGrabDebugLine(npc, true, leftGrabLine, sizeof(leftGrabLine));
			AppendHandGrabDebugLine(npc, false, rightGrabLine, sizeof(rightGrabLine));

			char raceLabel[128]{};
			char headArmorLabel[128]{};
			char hairLabel[128]{};
			char circletLabel[128]{};
			char bodyArmorLabel[128]{};
			SafeFormLabel(race, raceLabel, sizeof(raceLabel));
			SafeFormLabel(headArmor, headArmorLabel, sizeof(headArmorLabel));
			SafeFormLabel(hair, hairLabel, sizeof(hairLabel));
			SafeFormLabel(circlet, circletLabel, sizeof(circletLabel));
			SafeFormLabel(bodyArmor, bodyArmorLabel, sizeof(bodyArmorLabel));

			const char* headBoneName = "missing";
			if (headBone && headBone->m_name && headBone->m_name[0] != '\0') {
				headBoneName = headBone->m_name;
			}

			const char* actorName = GetActorLogName(npc);
			const char* rejectLabel = humanoidReject ? humanoidReject : "ok";

			_MESSAGE(
				"Neck Snap VR [grab-debug] %s: '%s' ref=%08X base=%08X race='%s' raceId=%08X raceFlags=%08X handReach=%.1f "
				"humanoidReject=%s validVictim=%s essential=%s undead=%s follower=%s dead=%s furniture=%s behind=%s snapRange=%.1f "
				"headBone='%s' headArmor='%s'(%08X) hair='%s'(%08X) circlet='%s'(%08X) bodyArmor='%s'(%08X) | %s | %s",
				context ? context : "grab",
				actorName,
				npc->formID,
				npc->baseForm ? npc->baseForm->formID : 0,
				raceLabel,
				race ? race->formID : 0,
				race ? race->data.raceFlags : 0,
				race ? race->data.handReach : 0.0f,
				rejectLabel,
				validVictim ? "Y" : "N",
				IsEssentialActor(npc) ? "Y" : "N",
				IsUndeadActor(npc) ? "Y" : "N",
				IsActiveFollower(npc) ? "Y" : "N",
				npc->IsDead(1) ? "Y" : "N",
				IsActorUsingFurniture(npc) ? "Y" : "N",
				behind ? "Y" : "N",
				snapRange,
				headBoneName,
				headArmorLabel,
				headArmor ? headArmor->formID : 0,
				hairLabel,
				hair ? hair->formID : 0,
				circletLabel,
				circlet ? circlet->formID : 0,
				bodyArmorLabel,
				bodyArmor ? bodyArmor->formID : 0,
				leftGrabLine,
				rightGrabLine);
			LOG_INFO(
				"[grab-debug] %s: '%s' ref=%08X race='%s' raceFlags=%08X headArmor='%s' bodyArmor='%s' validVictim=%s behind=%s %s | %s",
				context ? context : "grab",
				actorName,
				npc->formID,
				raceLabel,
				race ? race->data.raceFlags : 0,
				headArmorLabel,
				bodyArmorLabel,
				validVictim ? "Y" : "N",
				behind ? "Y" : "N",
				leftGrabLine,
				rightGrabLine);
		}

		Actor* ResolveGrabbedActorFromHand(bool isLeftHand)
		{
			if (!higgsInterface || !higgsInterface->IsHoldingObject(isLeftHand)) {
				return nullptr;
			}

			TESObjectREFR* grabbedRefr = higgsInterface->GetGrabbedObject(isLeftHand);
			return ResolveGrabbedActor(grabbedRefr);
		}

		void UpdateGrabbedNpcDebugLogging(PlayerCharacter* player)
		{
			if (!higgsInterface || !player) {
				g_loggedLeftGrabDebugNpcFormId = 0;
				g_loggedRightGrabDebugNpcFormId = 0;
				return;
			}

			const bool leftHolding = higgsInterface->IsHoldingObject(true);
			const bool rightHolding = higgsInterface->IsHoldingObject(false);
			Actor* leftActor = leftHolding ? ResolveGrabbedActorFromHand(true) : nullptr;
			Actor* rightActor = rightHolding ? ResolveGrabbedActorFromHand(false) : nullptr;
			const UInt32 leftFormId = leftActor ? leftActor->formID : 0;
			const UInt32 rightFormId = rightActor ? rightActor->formID : 0;

			if (!leftHolding) {
				g_loggedLeftGrabDebugNpcFormId = 0;
			} else if (leftActor && leftFormId != g_loggedLeftGrabDebugNpcFormId) {
				g_loggedLeftGrabDebugNpcFormId = leftFormId;
				LogGrabbedNpcDebug(leftActor, player, "left-hand grab started");
			}

			if (!rightHolding) {
				g_loggedRightGrabDebugNpcFormId = 0;
			} else if (rightActor && rightFormId != g_loggedRightGrabDebugNpcFormId) {
				g_loggedRightGrabDebugNpcFormId = rightFormId;
				LogGrabbedNpcDebug(rightActor, player, "right-hand grab started");
			}

			if (!leftHolding || !rightHolding || !leftActor || !rightActor || leftFormId != rightFormId) {
				g_loggedBothHandsGrabDebugNpcFormId = 0;
			} else if (leftFormId != g_loggedBothHandsGrabDebugNpcFormId && HasAnyHandGrabbingNpcHead(leftActor)) {
				g_loggedBothHandsGrabDebugNpcFormId = leftFormId;
				LogGrabbedNpcDebug(leftActor, player, "both hands on same NPC");
			}
		}

		NiAVObject* FindBone(Actor* actor, const char* boneName)
		{
			if (!actor || !boneName || !boneName[0]) {
				return nullptr;
			}

			NiAVObject* root = actor->GetNiNode();
			if (!root) {
				return nullptr;
			}

			BSFixedString nodeName(boneName);
			return root->GetObjectByName(&nodeName.data);
		}

		NiAVObject* FindHeadBone(Actor* actor)
		{
			static const char* kHeadBoneNames[] = {
				"NPC Head [Head]",
				"Head",
			};

			for (const char* boneName : kHeadBoneNames) {
				NiAVObject* head = FindBone(actor, boneName);
				if (head) {
					return head;
				}
			}

			return nullptr;
		}

		float AngleBetweenDegrees(const NiPoint3& a, const NiPoint3& b)
		{
			const float lenA = VecLength(a);
			const float lenB = VecLength(b);
			if (lenA < 0.0001f || lenB < 0.0001f) {
				return 0.0f;
			}

			float cosAngle = VecDot(a, b) / (lenA * lenB);
			if (cosAngle > 1.0f) {
				cosAngle = 1.0f;
			} else if (cosAngle < -1.0f) {
				cosAngle = -1.0f;
			}

			return acosf(cosAngle) * (180.0f / static_cast<float>(MATH_PI));
		}

		NiPoint3 MatrixAxis(const NiMatrix33& rot, int axisIndex)
		{
			return NiPoint3(
				rot.data[0][axisIndex],
				rot.data[1][axisIndex],
				rot.data[2][axisIndex]);
		}

		float GetHeadTwistDegrees(const NiMatrix33& baselineRot, const NiMatrix33& currentRot)
		{
			const float forwardAngle = AngleBetweenDegrees(
				MatrixAxis(baselineRot, 1),
				MatrixAxis(currentRot, 1));
			const float upAngle = AngleBetweenDegrees(
				MatrixAxis(baselineRot, 2),
				MatrixAxis(currentRot, 2));
			const float rightAngle = AngleBetweenDegrees(
				MatrixAxis(baselineRot, 0),
				MatrixAxis(currentRot, 0));

			return fmaxf(forwardAngle, fmaxf(upAngle, rightAngle));
		}

		void CaptureHeadTwistBaseline(Actor* npc)
		{
			g_hasBaselineHeadRot = false;

			NiAVObject* head = FindHeadBone(npc);
			if (!head) {
				return;
			}

			g_baselineHeadRot = head->m_worldTransform.rot;
			g_hasBaselineHeadRot = true;
		}

		float GetCurrentHeadTwistDegrees(Actor* npc)
		{
			if (!g_hasBaselineHeadRot) {
				return 0.0f;
			}

			NiAVObject* head = FindHeadBone(npc);
			if (!head) {
				return 0.0f;
			}

			return GetHeadTwistDegrees(g_baselineHeadRot, head->m_worldTransform.rot);
		}

		bool TryGetPlayerHandWorldPos(PlayerCharacter* player, bool isLeftHand, NiPoint3& outPos)
		{
			if (!player) {
				return false;
			}

			NiAVObject* root = player->GetNiNode();
			if (!root) {
				return false;
			}

			const char* boneName = isLeftHand ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]";
			BSFixedString nodeName(boneName);
			NiAVObject* hand = root->GetObjectByName(&nodeName.data);
			if (!hand) {
				return false;
			}

			outPos = hand->m_worldTransform.pos;
			return true;
		}

		bool TryGetPlayerControllerNodePos(PlayerCharacter* player, bool isLeftHand, NiPoint3& outPos)
		{
			if (!player) {
				return false;
			}

			const UInt32 controllerNodeIndex = isLeftHand
				? PlayerCharacter::kNode_LeftControllerNode
				: PlayerCharacter::kNode_RightControllerNode;
			NiAVObject* controllerNode = player->nodeList[controllerNodeIndex];
			if (!controllerNode) {
				return false;
			}

			outPos = controllerNode->m_worldTransform.pos;
			return true;
		}

		bool TryGetHiggsHandWorldPos(bool isLeftHand, NiPoint3& outPos)
		{
			if (!higgsInterface) {
				return false;
			}

			NiObject* handBody = higgsInterface->GetHandRigidBody(isLeftHand);
			if (!handBody) {
				return false;
			}

			void* hkBody = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(handBody) + 0x10);
			if (!hkBody) {
				return false;
			}

			const float* pos = reinterpret_cast<const float*>(reinterpret_cast<const char*>(hkBody) + 0x180);
			outPos.x = pos[0];
			outPos.y = pos[1];
			outPos.z = pos[2];
			return true;
		}

		bool TryGetVRControllerWorldPos(bool isLeftHand, NiPoint3& outPos)
		{
			BSOpenVR* openVR = *g_openVR;
			if (!openVR) {
				return false;
			}

			const UInt32 controllerIndex = isLeftHand ? BSVRInterface::kControllerHand_Left : BSVRInterface::kControllerHand_Right;
			NiNode* controllerNode = openVR->controller[controllerIndex];
			if (!controllerNode) {
				return false;
			}

			outPos = controllerNode->m_worldTransform.pos;
			return true;
		}

		bool TryGetControllerWorldPos(PlayerCharacter* player, bool isLeftHand, NiPoint3& outPos)
		{
			if (TryGetHiggsHandWorldPos(isLeftHand, outPos)) {
				return true;
			}

			if (TryGetPlayerControllerNodePos(player, isLeftHand, outPos)) {
				return true;
			}

			if (TryGetVRControllerWorldPos(isLeftHand, outPos)) {
				return true;
			}

			return TryGetPlayerHandWorldPos(player, isLeftHand, outPos);
		}

		const char* DescribeControllerPosSource(PlayerCharacter* player, bool isLeftHand, NiPoint3& outPos)
		{
			if (TryGetHiggsHandWorldPos(isLeftHand, outPos)) {
				return "higgsHand";
			}

			if (TryGetPlayerControllerNodePos(player, isLeftHand, outPos)) {
				return "controllerNode";
			}

			if (TryGetVRControllerWorldPos(isLeftHand, outPos)) {
				return "openVR";
			}

			if (TryGetPlayerHandWorldPos(player, isLeftHand, outPos)) {
				return "skeletonHand";
			}

			return "none";
		}

		void LogMotionDebug(
			Actor* npc,
			const NiPoint3& leftDelta,
			const NiPoint3& rightDelta,
			const NiPoint3& npcForward,
			float headTwist,
			bool validDelta,
			bool snapAlongNpcForward,
			bool snapAlongHandSeparation,
			bool snapOpposite3D,
			bool headTwistSnap,
			bool meaningfulHands,
			UInt32 oppositeMotionFrames,
			const char* leftSource,
			const char* rightSource)
		{
			if (!npc) {
				return;
			}

			++g_motionDebugTick;
			if ((g_motionDebugTick % static_cast<UInt32>(iMotionDebugLogIntervalFrames)) != 0) {
				return;
			}

			const float leftSpeed = VecLength(leftDelta);
			const float rightSpeed = VecLength(rightDelta);
			const float leftAlongForward = VecDot(leftDelta, Normalize(npcForward));
			const float rightAlongForward = VecDot(rightDelta, Normalize(npcForward));
			const float oppositeCos = (leftSpeed > 0.0001f && rightSpeed > 0.0001f)
				? VecDot(leftDelta, rightDelta) / (leftSpeed * rightSpeed)
				: 0.0f;

			_MESSAGE(
				"Neck Snap VR [motion]: '%s' valid=%s L=%.2f R=%.2f twist=%.1f peak=%.1f fwd=%s sep=%s opp3d=%s twistOk=%s cnt=%u base=%s src=%s/%s",
				GetActorLogName(npc),
				validDelta ? "Y" : "N",
				leftSpeed,
				rightSpeed,
				headTwist,
				g_peakHeadTwistDegrees,
				snapAlongNpcForward ? "Y" : "N",
				snapAlongHandSeparation ? "Y" : "N",
				snapOpposite3D ? "Y" : "N",
				headTwistSnap ? "Y" : "N",
				oppositeMotionFrames,
				g_hasBaselineHeadRot ? "Y" : "N",
				leftSource ? leftSource : "?",
				rightSource ? rightSource : "?");

			LOG_INFO(
				"[motion] '%s' deltaL=(%.2f, %.2f, %.2f) deltaR=(%.2f, %.2f, %.2f) alongFwd L=%.2f R=%.2f cos=%.2f meaningful=%s",
				GetActorLogName(npc),
				leftDelta.x,
				leftDelta.y,
				leftDelta.z,
				rightDelta.x,
				rightDelta.y,
				rightDelta.z,
				leftAlongForward,
				rightAlongForward,
				oppositeCos,
				meaningfulHands ? "Y" : "N");
		}

		bool HasOppositeSnapMotion(const NiPoint3& leftDelta, const NiPoint3& rightDelta, const NiPoint3& axis)
		{
			const NiPoint3 axisNorm = Normalize(axis);
			if (axisNorm.x == 0.0f && axisNorm.y == 0.0f && axisNorm.z == 0.0f) {
				return false;
			}

			const float leftAlong = VecDot(leftDelta, axisNorm);
			const float rightAlong = VecDot(rightDelta, axisNorm);

			const bool leftPushesForward = leftAlong >= fMinHandSpeed && rightAlong <= -fMinHandSpeed;
			const bool rightPushesForward = rightAlong >= fMinHandSpeed && leftAlong <= -fMinHandSpeed;
			if (!leftPushesForward && !rightPushesForward) {
				return false;
			}

			const float peakAxisSpeed = fmaxf(fabsf(leftAlong), fabsf(rightAlong));
			if (peakAxisSpeed < fMinPeakAxisStep) {
				return false;
			}

			const float weakerAxisSpeed = fminf(fabsf(leftAlong), fabsf(rightAlong));
			if (weakerAxisSpeed < fMinHandSpeed * fMinHandSpeedRatio) {
				return false;
			}

			return true;
		}

		bool HasOppositeSnapMotion3D(const NiPoint3& leftDelta, const NiPoint3& rightDelta)
		{
			const float leftSpeed = VecLength(leftDelta);
			const float rightSpeed = VecLength(rightDelta);
			if (leftSpeed < fMinHandSpeed || rightSpeed < fMinHandSpeed) {
				return false;
			}

			const float deltaDot = VecDot(leftDelta, rightDelta);
			const float cosAngle = deltaDot / (leftSpeed * rightSpeed);
			return cosAngle <= fOppositeCosThreshold;
		}

		bool HasMeaningfulHandMovement(const NiPoint3& leftDelta, const NiPoint3& rightDelta)
		{
			return VecLength(leftDelta) >= fSoftHandSpeed
				|| VecLength(rightDelta) >= fSoftHandSpeed;
		}

		bool HasHeadTwistSnap(Actor* npc)
		{
			const float headTwist = GetCurrentHeadTwistDegrees(npc);
			if (headTwist > g_peakHeadTwistDegrees) {
				g_peakHeadTwistDegrees = headTwist;
			}

			return g_peakHeadTwistDegrees >= fMinHeadTwistDegrees
				&& g_peakHeadTwistDegrees <= fMaxHeadTwistDegrees;
		}

		bool IsValidSnapFrameDelta(const NiPoint3& leftDelta, const NiPoint3& rightDelta)
		{
			return VecLength(leftDelta) <= fMaxFrameDelta
				&& VecLength(rightDelta) <= fMaxFrameDelta;
		}

		NiObject* GetRigidBodyFromBone(NiAVObject* bone)
		{
			if (!bone || !bone->unk040) {
				return nullptr;
			}

			const NiPointer<NiObject>& body =
				*reinterpret_cast<const NiPointer<NiObject>*>(reinterpret_cast<const char*>(bone->unk040) + 0x20);

			return body.m_pObject;
		}

		bool IsSameRigidBody(NiObject* a, NiObject* b)
		{
			return a && b && a == b;
		}

		bool IsHeadBoneName(const char* boneName)
		{
			if (!boneName || !boneName[0]) {
				return false;
			}

			if (strstr(boneName, "[Head]") != nullptr) {
				return true;
			}

			return _stricmp(boneName, "Head") == 0;
		}

		bool IsHeadAttachmentBoneName(const char* boneName)
		{
			if (!boneName || !boneName[0] || IsHeadBoneName(boneName)) {
				return false;
			}

			static const char* kHeadAttachmentMarkers[] = {
				"Helmet",
				"Helm",
				"Hair",
				"Hood",
				"Circlet",
				"Mask",
				"Horn",
				"Ear",
				"Beard",
				"Brow",
				"Blindfold",
				"Tiara",
				"Crown",
				"Hat",
				"Cap",
			};

			for (const char* marker : kHeadAttachmentMarkers) {
				if (strstr(boneName, marker) != nullptr) {
					return true;
				}
			}

			return false;
		}

		bool IsDescendantOfBone(NiAVObject* node, NiAVObject* ancestor)
		{
			while (node) {
				if (node == ancestor) {
					return true;
				}

				node = node->m_parent;
			}

			return false;
		}

		bool IsExcludedGrabBoneName(const char* boneName)
		{
			if (!boneName || !boneName[0] || IsHeadBoneName(boneName)) {
				return false;
			}

			static const char* kExcludeMarkers[] = {
				"[Neck]",
				"[Spn",
				"[Pelv]",
				"[Clv",
				"[UArm]",
				"[LArm]",
				"[LUArm]",
				"[RUArm]",
				"[LLArm]",
				"[RLArm]",
				"[Thigh]",
				"[Calf]",
				"[Foot]",
				"[Hand]",
			};

			for (const char* marker : kExcludeMarkers) {
				if (strstr(boneName, marker) != nullptr) {
					return true;
				}
			}

			return false;
		}

		NiAVObject* FindBoneForRigidBody(NiAVObject* node, NiObject* rigidBody)
		{
			if (!node || !rigidBody) {
				return nullptr;
			}

			if (IsSameRigidBody(GetRigidBodyFromBone(node), rigidBody)) {
				return node;
			}

			NiNode* niNode = node->GetAsNiNode();
			if (!niNode || !niNode->m_children.m_data) {
				return nullptr;
			}

			for (UInt16 i = 0; i < niNode->m_children.m_emptyRunStart; ++i) {
				NiAVObject* child = niNode->m_children.m_data[i];
				if (!child) {
					continue;
				}

				if (NiAVObject* found = FindBoneForRigidBody(child, rigidBody)) {
					return found;
				}
			}

			return nullptr;
		}

		NiAVObject* FindGrabbedBone(Actor* npc, NiObject* rigidBody)
		{
			if (!npc || !rigidBody) {
				return nullptr;
			}

			NiAVObject* root = npc->GetNiNode();
			if (!root) {
				return nullptr;
			}

			return FindBoneForRigidBody(root, rigidBody);
		}

		bool IsGrabbedRigidBodyOnHeadBone(Actor* npc, NiObject* rigidBody)
		{
			NiAVObject* grabbedBone = FindGrabbedBone(npc, rigidBody);
			if (!grabbedBone) {
				return false;
			}

			if (IsHeadBoneName(grabbedBone->m_name)) {
				return true;
			}

			if (IsHeadAttachmentBoneName(grabbedBone->m_name)) {
				return true;
			}

			NiAVObject* head = FindHeadBone(npc);
			return head && IsDescendantOfBone(grabbedBone, head);
		}

		bool IsRigidBodyNearHeadBone(Actor* npc, NiObject* rigidBody)
		{
			NiAVObject* head = FindHeadBone(npc);
			NiAVObject* grabbedBone = FindGrabbedBone(npc, rigidBody);
			if (!head || !grabbedBone) {
				return false;
			}

			return Distance3D(head->m_worldTransform.pos, grabbedBone->m_worldTransform.pos) <= fHeadGrabProximity;
		}

		bool IsGrabbedRigidBodyOnExcludedBone(Actor* npc, NiObject* rigidBody)
		{
			NiAVObject* grabbedBone = FindGrabbedBone(npc, rigidBody);
			if (!grabbedBone) {
				return false;
			}

			return IsExcludedGrabBoneName(grabbedBone->m_name);
		}

		bool IsNonHeadBodyRigidBody(Actor* npc, NiObject* rigidBody)
		{
			if (!npc || !rigidBody) {
				return false;
			}

			static const char* kNonHeadBoneNames[] = {
				"NPC Neck [Neck]",
				"Neck",
				"NPC Spine2 [Spn2]",
				"NPC Spine1 [Spn1]",
				"NPC Spine [Spine]",
				"NPC Pelvis [Pelv]",
				"Pelvis",
				"NPC L Clavicle [ClvL]",
				"NPC R Clavicle [ClvR]",
				"NPC L UpperArm [LUArm]",
				"NPC R UpperArm [RUArm]",
				"NPC L Forearm [LLArm]",
				"NPC R Forearm [RLArm]",
			};

			for (const char* boneName : kNonHeadBoneNames) {
				NiAVObject* bone = FindBone(npc, boneName);
				if (IsSameRigidBody(GetRigidBodyFromBone(bone), rigidBody)) {
					return true;
				}
			}

			return IsGrabbedRigidBodyOnExcludedBone(npc, rigidBody);
		}

		Actor* ResolveGrabbedActor(TESObjectREFR* refr)
		{
			if (!refr) {
				return nullptr;
			}

			return DYNAMIC_CAST(refr, TESObjectREFR, Actor);
		}

		bool IsGrabbedActor(Actor* npc, TESObjectREFR* grabbedRefr)
		{
			if (!npc || !grabbedRefr) {
				return false;
			}

			Actor* grabbedActor = ResolveGrabbedActor(grabbedRefr);
			if (grabbedActor) {
				return grabbedActor->formID == npc->formID;
			}

			return grabbedRefr->formID == npc->formID;
		}

		bool IsHandGrabbingNpcHead(Actor* npc, bool isLeftHand)
		{
			if (!higgsInterface || !npc) {
				return false;
			}

			if (!higgsInterface->IsHoldingObject(isLeftHand)) {
				return false;
			}

			TESObjectREFR* grabbedRefr = higgsInterface->GetGrabbedObject(isLeftHand);
			if (!IsGrabbedActor(npc, grabbedRefr)) {
				return false;
			}

			NiObject* grabbedRigidBody = higgsInterface->GetGrabbedRigidBody(isLeftHand);
			if (!grabbedRigidBody) {
				return false;
			}

			if (IsNonHeadBodyRigidBody(npc, grabbedRigidBody)) {
				return false;
			}

			if (IsGrabbedRigidBodyOnHeadBone(npc, grabbedRigidBody)) {
				return true;
			}

			NiAVObject* head = FindHeadBone(npc);
			NiObject* headRigidBody = GetRigidBodyFromBone(head);
			if (headRigidBody && grabbedRigidBody == headRigidBody) {
				return true;
			}

			return IsRigidBodyNearHeadBone(npc, grabbedRigidBody);
		}

		bool BothHandsGrabbingHead(Actor* npc)
		{
			if (!higgsInterface) {
				return false;
			}

			if (!higgsInterface->IsHoldingObject(true) || !higgsInterface->IsHoldingObject(false)) {
				return false;
			}

			if (!IsGrabbedActor(npc, higgsInterface->GetGrabbedObject(true))) {
				return false;
			}

			if (!IsGrabbedActor(npc, higgsInterface->GetGrabbedObject(false))) {
				return false;
			}

			return IsHandGrabbingNpcHead(npc, true) && IsHandGrabbingNpcHead(npc, false);
		}

		bool HasAnyHandGrabbingNpcHead(Actor* npc)
		{
			return IsHandGrabbingNpcHead(npc, true) || IsHandGrabbingNpcHead(npc, false);
		}

		bool IsPlayerBehindActor(Actor* npc, Actor* player, float maxDistance)
		{
			TESObjectREFR* npcRefr = static_cast<TESObjectREFR*>(npc);
			TESObjectREFR* playerRefr = static_cast<TESObjectREFR*>(player);
			const NiPoint3 playerPos = playerRefr->pos;

			if (EvaluateBehindCheck(
				npcRefr->pos,
				ActorForwardHorizontal(npc),
				playerPos,
				maxDistance,
				fMaxHeightDiff)) {
				return true;
			}

			NiAVObject* head = FindHeadBone(npc);
			if (!head) {
				return false;
			}

			const bool usingFurniture = IsActorUsingFurniture(npc);
			const float headHeightLimit = usingFurniture ? fMaxHeightDiff * 2.5f : fMaxHeightDiff * 1.5f;
			return EvaluateBehindCheck(
				head->m_worldTransform.pos,
				BoneForwardHorizontal(head),
				playerPos,
				maxDistance,
				headHeightLimit);
		}

		Actor* FindClosestSnapTarget(Actor* player)
		{
			ProcessLists* processLists = GetProcessLists();
			if (!processLists) {
				return nullptr;
			}

			TESObjectREFR* playerRefr = static_cast<TESObjectREFR*>(player);
			const tArray<UInt32>* actorLists[] = {
				&processLists->highActorHandles,
				&processLists->middleHighActorHandles,
				&processLists->middleLowActorHandles,
			};

			Actor* closestNpc = nullptr;
			float closestDistanceSq = FLT_MAX;

			for (const tArray<UInt32>* actorList : actorLists) {
				if (!actorList || !actorList->entries || actorList->count == 0) {
					continue;
				}

				for (UInt32 i = 0; i < actorList->count; ++i) {
					Actor* actor = ResolveActorFromHandle(actorList->entries[i]);
					if (!actor || !actor->GetNiNode() || actor->IsDead(1)) {
						continue;
					}

					if (!IsValidNeckSnapVictim(actor)) {
						continue;
					}

					const float snapRange = GetNeckSnapRange(actor);
					if (!IsPlayerBehindActor(actor, player, snapRange)) {
						continue;
					}

					const NiPoint3 targetPos = GetActorReferencePos(actor);
					const NiPoint3 delta = NiPoint3(
						playerRefr->pos.x - targetPos.x,
						playerRefr->pos.y - targetPos.y,
						playerRefr->pos.z - targetPos.z);
					const float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
					if (distSq < closestDistanceSq) {
						closestDistanceSq = distSq;
						closestNpc = actor;
					}
				}
			}

			return closestNpc;
		}

		bool IsLatchedNpcTooFar(Actor* npc, Actor* player)
		{
			TESObjectREFR* playerRefr = static_cast<TESObjectREFR*>(player);
			const NiPoint3 targetPos = GetActorReferencePos(npc);
			const NiPoint3 delta = NiPoint3(
				playerRefr->pos.x - targetPos.x,
				playerRefr->pos.y - targetPos.y,
				playerRefr->pos.z - targetPos.z);
			const float distSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
			return distSq > fLatchResetDistance * fLatchResetDistance;
		}

		void ResetHandMotionTracking()
		{
			g_hasPrevHandPos = false;
			g_hasBaselineHeadRot = false;
			g_oppositeMotionFrames = 0;
			g_motionDebugTick = 0;
			g_peakHeadTwistDegrees = 0.0f;
		}

		void EndDualHeadGrabSession(const char* reason)
		{
			if (reason && g_lastDualHeadGrabNpcFormId != 0) {
				_MESSAGE("Neck Snap VR [motion]: dual grab session ended (%s).", reason);
			}

			g_lastDualHeadGrabNpcFormId = 0;
			g_lastNeckSnappedNpcFormId = 0;
			g_dualGrabLossFrames = 0;
			ResetHandMotionTracking();
		}

		void ClearLatchedNpc()
		{
			g_lastSnapReadyNpcFormId = 0;
			EndDualHeadGrabSession();
		}

		void LatchSnapReadyNpc(PlayerCharacter* player, Actor* npc)
		{
			if (!npc || g_lastSnapReadyNpcFormId == npc->formID) {
				return;
			}

			g_lastSnapReadyNpcFormId = npc->formID;
			EndDualHeadGrabSession("behind latch changed npc");

			TESObjectREFR* playerRefr = static_cast<TESObjectREFR*>(player);
			const NiPoint3 targetPos = GetActorReferencePos(npc);
			const NiPoint3 delta = NiPoint3(
				playerRefr->pos.x - targetPos.x,
				playerRefr->pos.y - targetPos.y,
				playerRefr->pos.z - targetPos.z);
			const float distance = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

			_MESSAGE(
				"Neck Snap VR: unarmed player behind humanoid NPC '%s' (formId %08X, distance %.1f).",
				GetActorLogName(npc),
				npc->formID,
				distance);
			LOG_INFO(
				"Neck snap ready: unarmed player is directly behind humanoid NPC '%s' (formId %08X, distance %.1f).",
				GetActorLogName(npc),
				npc->formID,
				distance);
		}

		void UpdateBehindReadyState(PlayerCharacter* player)
		{
			Actor* closestNpc = FindClosestSnapTarget(player);
			if (!closestNpc) {
				return;
			}

			LatchSnapReadyNpc(player, closestNpc);
		}

		void TryLatchFromDualHeadGrab(PlayerCharacter* player)
		{
			if (g_lastSnapReadyNpcFormId != 0) {
				return;
			}

			ProcessLists* processLists = GetProcessLists();
			if (!processLists) {
				return;
			}

			const tArray<UInt32>* actorLists[] = {
				&processLists->highActorHandles,
				&processLists->middleHighActorHandles,
				&processLists->middleLowActorHandles,
			};

			for (const tArray<UInt32>* actorList : actorLists) {
				if (!actorList || !actorList->entries || actorList->count == 0) {
					continue;
				}

				for (UInt32 i = 0; i < actorList->count; ++i) {
					Actor* actor = ResolveActorFromHandle(actorList->entries[i]);
					if (!actor || !actor->GetNiNode() || actor->IsDead(1) || !IsValidNeckSnapVictim(actor)) {
						continue;
					}

					// One hand or both on head counts — second hand can join shortly after.
					if (!HasAnyHandGrabbingNpcHead(actor)) {
						continue;
					}

					const float snapRange = GetNeckSnapRange(actor);
					if (!IsPlayerBehindActor(actor, player, snapRange)) {
						continue;
					}

					LatchSnapReadyNpc(player, actor);
					return;
				}
			}
		}

		void UpdateDualHeadGrabState(PlayerCharacter* player)
		{
			if (g_lastSnapReadyNpcFormId == 0) {
				g_lastDualHeadGrabNpcFormId = 0;
				return;
			}

			Actor* readyNpc = FindActorByFormId(g_lastSnapReadyNpcFormId);
			if (!readyNpc || readyNpc->IsDead(1) || !IsValidNeckSnapVictim(readyNpc)) {
				ClearLatchedNpc();
				return;
			}

			if (IsLatchedNpcTooFar(readyNpc, player)) {
				ClearLatchedNpc();
				return;
			}

			if (!BothHandsGrabbingHead(readyNpc)) {
				// Grace only applies after dual grab was already established (e.g. brief HIGGS flicker).
				// Waiting with one hand for the second does not count as a loss.
				if (g_lastDualHeadGrabNpcFormId == readyNpc->formID) {
					++g_dualGrabLossFrames;
					if (g_dualGrabLossFrames < static_cast<UInt32>(iDualGrabReleaseGraceFrames)) {
						UpdateNeckSnapMotion(player, readyNpc);
					} else {
						EndDualHeadGrabSession("dual head grab lost");
					}
				}
				return;
			}

			g_dualGrabLossFrames = 0;

			if (g_lastDualHeadGrabNpcFormId == readyNpc->formID) {
				UpdateNeckSnapMotion(player, readyNpc);
				return;
			}

			g_lastDualHeadGrabNpcFormId = readyNpc->formID;
			g_lastNeckSnappedNpcFormId = 0;
			ResetHandMotionTracking();
			CaptureHeadTwistBaseline(readyNpc);
			_MESSAGE(
				"Neck Snap VR [motion]: tracking started for '%s' (head baseline %s).",
				GetActorLogName(readyNpc),
				g_hasBaselineHeadRot ? "captured" : "missing");

			if (g_loggedDualHeadGrabNpcFormId != readyNpc->formID) {
				g_loggedDualHeadGrabNpcFormId = readyNpc->formID;

				const char* grabbedBoneName = "unknown";
				if (higgsInterface) {
					NiAVObject* leftGrabBone = FindGrabbedBone(readyNpc, higgsInterface->GetGrabbedRigidBody(true));
					if (leftGrabBone && leftGrabBone->m_name && leftGrabBone->m_name[0] != '\0') {
						grabbedBoneName = leftGrabBone->m_name;
					}
				}

				TESObjectREFR* npcRefr = static_cast<TESObjectREFR*>(readyNpc);
				TESObjectREFR* playerRefr = static_cast<TESObjectREFR*>(player);
				const NiPoint3 delta = playerRefr->pos - npcRefr->pos;
				const float distance = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

				_MESSAGE(
					"Neck Snap VR: both hands grabbing head of NPC '%s' (formId %08X, bone '%s', distance %.1f).",
					GetActorLogName(readyNpc),
					readyNpc->formID,
					grabbedBoneName,
					distance);
				LOG_INFO(
					"Neck snap armed: both hands on head of '%s' (formId %08X, bone '%s', distance %.1f).",
					GetActorLogName(readyNpc),
					readyNpc->formID,
					grabbedBoneName,
					distance);
			}

			UpdateNeckSnapMotion(player, readyNpc);
		}

		void UpdateNeckSnapMotion(PlayerCharacter* player, Actor* npc)
		{
			if (!player || !npc || g_lastDualHeadGrabNpcFormId != npc->formID) {
				return;
			}

			if (g_lastNeckSnappedNpcFormId == npc->formID) {
				return;
			}

			NiPoint3 leftPos{};
			NiPoint3 rightPos{};
			const char* leftSource = DescribeControllerPosSource(player, true, leftPos);
			const char* rightSource = DescribeControllerPosSource(player, false, rightPos);
			if (!leftSource || _stricmp(leftSource, "none") == 0 || !rightSource || _stricmp(rightSource, "none") == 0) {
				++g_motionDebugTick;
				if ((g_motionDebugTick % static_cast<UInt32>(iMotionDebugLogIntervalFrames)) == 0) {
					_MESSAGE(
						"Neck Snap VR [motion]: controller position unavailable for '%s' (src=%s/%s).",
						GetActorLogName(npc),
						leftSource ? leftSource : "?",
						rightSource ? rightSource : "?");
				}
				return;
			}

			if (!g_hasPrevHandPos) {
				g_prevLeftHandPos = leftPos;
				g_prevRightHandPos = rightPos;
				g_hasPrevHandPos = true;
				_MESSAGE(
					"Neck Snap VR [motion]: seeded hand positions for '%s' (src=%s/%s).",
					GetActorLogName(npc),
					leftSource,
					rightSource);
				return;
			}

			const NiPoint3 leftDelta = NiPoint3(
				leftPos.x - g_prevLeftHandPos.x,
				leftPos.y - g_prevLeftHandPos.y,
				leftPos.z - g_prevLeftHandPos.z);
			const NiPoint3 rightDelta = NiPoint3(
				rightPos.x - g_prevRightHandPos.x,
				rightPos.y - g_prevRightHandPos.y,
				rightPos.z - g_prevRightHandPos.z);

			const float headTwist = GetCurrentHeadTwistDegrees(npc);
			const bool headTwistSnap = HasHeadTwistSnap(npc);
			const bool validDelta = IsValidSnapFrameDelta(leftDelta, rightDelta);
			const NiPoint3 npcForward = ActorForwardHorizontal(npc);
			const bool meaningfulHands = HasMeaningfulHandMovement(leftDelta, rightDelta);

			g_prevLeftHandPos = leftPos;
			g_prevRightHandPos = rightPos;

			bool snapAlongNpcForward = false;
			bool snapAlongHandSeparation = false;
			bool snapOpposite3D = false;
			bool controllerSnap = false;

			if (validDelta) {
				const NiPoint3 handSeparation = Normalize(NiPoint3(
					rightPos.x - leftPos.x,
					rightPos.y - leftPos.y,
					rightPos.z - leftPos.z));

				snapAlongNpcForward = HasOppositeSnapMotion(leftDelta, rightDelta, npcForward);
				snapAlongHandSeparation = HasOppositeSnapMotion(leftDelta, rightDelta, handSeparation);
				snapOpposite3D = HasOppositeSnapMotion3D(leftDelta, rightDelta);
				controllerSnap = snapAlongNpcForward || snapAlongHandSeparation || snapOpposite3D;
			}

			LogMotionDebug(
				npc,
				leftDelta,
				rightDelta,
				npcForward,
				headTwist,
				validDelta,
				snapAlongNpcForward,
				snapAlongHandSeparation,
				snapOpposite3D,
				headTwistSnap,
				meaningfulHands,
				g_oppositeMotionFrames,
				leftSource,
				rightSource);

			if (controllerSnap) {
				++g_oppositeMotionFrames;
			} else if (!headTwistSnap) {
				g_oppositeMotionFrames = 0;
			}

			const bool controllerSnapReady = controllerSnap
				&& g_oppositeMotionFrames >= static_cast<UInt32>(iOppositeMotionFrames);
			if (!controllerSnapReady && !headTwistSnap) {
				return;
			}

			g_lastNeckSnappedNpcFormId = npc->formID;
			g_oppositeMotionFrames = 0;

			const float leftSpeed = VecLength(leftDelta);
			const float rightSpeed = VecLength(rightDelta);

			if (!ExecuteNeckSnap(npc)) {
				_MESSAGE(
					"Neck Snap VR [motion]: snap motion triggered on '%s' (formId %08X) but execution was blocked.",
					GetActorLogName(npc),
					npc->formID);
				return;
			}

			_MESSAGE(
				"Neck Snap VR: %s's neck was snapped (formId %08X).",
				GetActorLogName(npc),
				npc->formID);
			LOG_INFO(
				"Neck snap executed: '%s' neck was snapped (formId %08X, left %.1f / right %.1f, headTwist %.1f / peak %.1f).",
				GetActorLogName(npc),
				npc->formID,
				leftSpeed,
				rightSpeed,
				headTwist,
				g_peakHeadTwistDegrees);
		}

	}  // namespace

	void ClearNeckSnapState()
	{
		g_lastSnapReadyNpcFormId = 0;
		g_lastDualHeadGrabNpcFormId = 0;
		g_lastNeckSnappedNpcFormId = 0;
		g_loggedDualHeadGrabNpcFormId = 0;
		g_loggedLeftGrabDebugNpcFormId = 0;
		g_loggedRightGrabDebugNpcFormId = 0;
		g_loggedBothHandsGrabDebugNpcFormId = 0;
		g_dualGrabLossFrames = 0;
		g_oppositeMotionFrames = 0;
		g_hasPrevHandPos = false;
		g_hasBaselineHeadRot = false;
		g_motionDebugTick = 0;
		g_peakHeadTwistDegrees = 0.0f;
	}

	void UpdateNeckSnapDetection()
	{
		PlayerCharacter* player = *g_thePlayer;
		if (!player || !IsPlayerUnarmed(player)) {
			ClearNeckSnapState();
			return;
		}

		UpdateBehindReadyState(player);
		UpdateGrabbedNpcDebugLogging(player);
		TryLatchFromDualHeadGrab(player);
		UpdateDualHeadGrabState(player);
	}

}
