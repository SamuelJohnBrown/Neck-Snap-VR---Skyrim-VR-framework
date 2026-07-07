#include "CrimeBounty.h"

#include "config.h"

#include "skse64/GameAPI.h"
#include "skse64/GameData.h"
#include "skse64/GameForms.h"
#include "skse64/GameObjects.h"
#include "skse64/GameReferences.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameTypes.h"

namespace NeckSnapVR
{
	namespace
	{
		constexpr std::size_t kModCrimeGoldVtableIndex = 0xB8;
		constexpr float kWitnessMaxDistance = 4096.0f;
		constexpr float kWitnessMaxDistanceSq = kWitnessMaxDistance * kWitnessMaxDistance;

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

		void ModCrimeGoldValue(Actor* player, TESFaction* faction, bool violent, std::int32_t amount)
		{
			if (!player || !faction || amount <= 0) {
				return;
			}

			auto** vtable = reinterpret_cast<std::uintptr_t**>(player);
			if (!vtable || !*vtable) {
				return;
			}

			using Fn = void (*)(Actor*, TESFaction*, bool, std::int32_t);
			auto fn = reinterpret_cast<Fn>((*vtable)[kModCrimeGoldVtableIndex]);
			if (!fn) {
				return;
			}

			fn(player, faction, violent, amount);
		}

		float DistanceSquared(const NiPoint3& a, const NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return dx * dx + dy * dy + dz * dz;
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

		const char* ActorDisplayName(Actor* actor)
		{
			if (!actor) {
				return "<null>";
			}

			TESObjectREFR* refr = static_cast<TESObjectREFR*>(actor);
			const char* name = CALL_MEMBER_FN(refr, GetReferenceName)();
			if (name && name[0]) {
				return name;
			}

			if (actor->baseForm) {
				const char* baseName = actor->baseForm->GetFullName();
				if (baseName && baseName[0]) {
					return baseName;
				}
			}

			return "<unnamed>";
		}

		TESFaction* GetVictimCrimeFaction(Actor* victim)
		{
			if (!victim || !victim->baseForm) {
				return nullptr;
			}

			TESNPC* npc = DYNAMIC_CAST(victim->baseForm, TESForm, TESNPC);
			if (!npc || !npc->faction) {
				return nullptr;
			}

			return npc->faction;
		}

		bool FactionReportsMurder(TESFaction* faction)
		{
			if (!faction) {
				return false;
			}

			if (faction->factionFlags & TESFaction::kFactionFlag_IgnoreMurder) {
				return false;
			}

			if (faction->factionFlags & TESFaction::kFactionFlag_NoReportCrime) {
				return false;
			}

			return true;
		}

		std::int32_t GetMurderBountyAmount(TESFaction* faction)
		{
			if (!faction) {
				return 0;
			}

			const std::int32_t murderGold = faction->crimeValues.murder;
			return murderGold > 0 ? murderGold : 1000;
		}

		bool CanActorWitnessMurder(Actor* witness, Actor* player, Actor* victim, const NiPoint3& victimPos)
		{
			if (!witness || !player || !victim) {
				return false;
			}

			if (witness == player || witness == victim) {
				return false;
			}

			if (IsActiveFollower(witness)) {
				return false;
			}

			if (witness->IsDead(1) || !witness->GetNiNode()) {
				return false;
			}

			TESObjectREFR* witnessRefr = static_cast<TESObjectREFR*>(witness);
			if (DistanceSquared(witnessRefr->pos, victimPos) > kWitnessMaxDistanceSq) {
				return false;
			}

			UInt8 losArg = 0;
			const bool seesPlayer = HasLOS(witness, player, &losArg) != 0;
			losArg = 0;
			const bool seesVictim = HasLOS(witness, victim, &losArg) != 0;
			return seesPlayer || seesVictim;
		}

		Actor* FindMurderWitness(Actor* player, Actor* victim)
		{
			ProcessLists* processLists = GetProcessLists();
			if (!processLists) {
				return nullptr;
			}

			TESObjectREFR* victimRefr = static_cast<TESObjectREFR*>(victim);
			const NiPoint3 victimPos = victimRefr->pos;

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
					Actor* candidate = ResolveActorFromHandle(actorList->entries[i]);
					if (!candidate) {
						continue;
					}

					if (CanActorWitnessMurder(candidate, player, victim, victimPos)) {
						return candidate;
					}
				}
			}

			return nullptr;
		}

	}  // namespace

	bool IsActiveFollower(Actor* actor)
	{
		if (!actor) {
			return false;
		}

		return (actor->flags1 & Actor::kFlags_IsPlayerTeammate) != 0;
	}

	void ApplyMurderBountyIfWitnessed(PlayerCharacter* player, Actor* victim)
	{
		if (!player || !victim) {
			return;
		}

		TESFaction* crimeFaction = GetVictimCrimeFaction(victim);
		if (!FactionReportsMurder(crimeFaction)) {
			_MESSAGE(
				"Neck Snap VR [crime]: no murder bounty faction for '%s'.",
				ActorDisplayName(victim));
			LOG_INFO("[Crime] No murder bounty faction for %s", ActorDisplayName(victim));
			return;
		}

		Actor* witness = FindMurderWitness(static_cast<Actor*>(player), victim);
		if (!witness) {
			_MESSAGE(
				"Neck Snap VR [crime]: unwitnessed neck snap; no bounty for '%s'.",
				ActorDisplayName(victim));
			LOG_INFO("[Crime] Neck snap unwitnessed; no bounty for %s", ActorDisplayName(victim));
			return;
		}

		const std::int32_t bounty = GetMurderBountyAmount(crimeFaction);
		ModCrimeGoldValue(static_cast<Actor*>(player), crimeFaction, true, bounty);

		_MESSAGE(
			"Neck Snap VR [crime]: witnessed neck snap murder | victim=%s witness=%s faction=%s bounty=%d",
			ActorDisplayName(victim),
			ActorDisplayName(witness),
			crimeFaction->GetFullName() ? crimeFaction->GetFullName() : "<faction>",
			bounty);
		LOG_INFO(
			"[Crime] Witnessed neck snap murder | victim=%s witness=%s faction=%s bounty=%d",
			ActorDisplayName(victim),
			ActorDisplayName(witness),
			crimeFaction->GetFullName() ? crimeFaction->GetFullName() : "<faction>",
			bounty);
	}

}
