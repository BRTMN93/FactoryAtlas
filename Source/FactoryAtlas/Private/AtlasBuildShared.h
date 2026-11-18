#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class AActor;
class UFGConnectionComponent;
class UFGConstructDisqualifier;
namespace Atlas { struct FPort; }

// Helpers for building from the map, defined in AtlasBelts.cpp and shared with the power wires in AtlasWires.cpp.
namespace AtlasBeltDetail
{
	/** Clearance overlaps are what "allow clipping" lets through. */
	bool IsClippingDisqualifier(const TSubclassOf<UFGConstructDisqualifier>& Disqualifier);
	/** The build gun's own trace channel. False when the game has none by that name. */
	bool FindBuildGunChannel(ECollisionChannel& Out);
	/** The building a hit belongs to, past the abstract instance manager most buildings collide through. */
	AActor* HitOwner(const FHitResult& Hit);
	/** Names the building a hit belongs to, so a hologram can snap to it. */
	void ResolveForPlacement(FHitResult& Hit);
	/** Whether a connection already has something on it. */
	bool IsPortConnected(const UFGConnectionComponent* Connection);
	/** Fills Port's kind, direction and place from a free connection. False for one that isn't a port to draw. */
	bool DescribePort(UFGConnectionComponent* Connection, Atlas::FPort& Port);
}
