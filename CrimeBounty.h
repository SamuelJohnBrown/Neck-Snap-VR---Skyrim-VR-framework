#pragma once

class Actor;
class PlayerCharacter;

namespace NeckSnapVR
{
	bool IsActiveFollower(Actor* actor);
	void ApplyMurderBountyIfWitnessed(PlayerCharacter* player, Actor* victim);
}
