#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AreaActionsLightweightLibrary.h"
#include "AreaActionsDismantleSelector.generated.h"

class AFGBuildGun;
class AFGCharacterPlayer;
class UFGBuildGunStateDismantle;

/**
 * Hands the buildings of an area over to the vanilla dismantle mode: equips the build gun, switches it to
 * dismantle and adds every building to the pending (multi-select) list, exactly as if the player had
 * selected them one by one. The player then reviews the refund preview and confirms with the normal
 * dismantle input.
 *
 * Runs locally for the local player only. Lives while the build gun stays in dismantle mode with pending
 * buildings, because lightweight buildings (foundations, walls, ...) are only selectable through temporary
 * actors, and those are kept alive by the instance converter this actor registers over the area.
 *
 * AddPendingDismantleActor is private in 1.2; Config/AccessTransformers.ini makes this class a friend of
 * UFGBuildGunStateDismantle.
 */
UCLASS(NotBlueprintable, Transient)
class AREAACTIONS_API AAreaActionsDismantleSelector : public AActor
{
	GENERATED_BODY()
public:
	AAreaActionsDismantleSelector();

	/**
	 * Switches the player's build gun to dismantle and selects the given buildings plus every lightweight
	 * building inside the area. Selection happens over the next frames; returns false if it could not start.
	 * @param Actors       Buildings (actors) to select, e.g. the actors the area action runs on.
	 * @param AreaCorners  The area's corner locations (only X/Y are used); lightweights are searched here.
	 * @param MinZ/MaxZ    Vertical range of the area for lightweights.
	 */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Dismantle")
	static bool SelectForDismantle(AFGCharacterPlayer* Player, const TArray<AActor*>& Actors, const TArray<FVector>& AreaCorners, double MinZ, double MaxZ, bool bIncludeLightweights = true);

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	enum class EStage : uint8
	{
		WaitForDismantleState,
		AddToSelection,
		KeepAlive,
		Grace
	};

	AFGBuildGun* GetBuildGun() const;
	UFGBuildGunStateDismantle* GetDismantleState() const;
	void RegisterInstanceConverter();
	void AddBatch(UFGBuildGunStateDismantle* DismantleState);
	void LogOutcome() const;

	/** Buildings added to the selection per frame, so huge areas do not stall a single frame. */
	static constexpr int32 BatchSize = 250;
	/** Give up if the build gun did not reach dismantle mode in this time. */
	static constexpr float StateTimeoutSeconds = 5.f;
	/** How long the instance converter stays after the selection is gone. */
	static constexpr float GraceSeconds = 3.f;

	TWeakObjectPtr<AFGCharacterPlayer> Player;
	TArray<TWeakObjectPtr<AActor>> ActorQueue;
	TArray<FAreaActionsLightweightIndex> LightweightQueue;
	EStage Stage = EStage::WaitForDismantleState;
	float StageTime = 0.f;
	float RetryTimer = 0.f;
	bool bConverterRegistered = false;
	bool bEnabledMultiSelect = false;
	/** What was added, for the outcome log. */
	TArray<TWeakObjectPtr<AActor>> TrackedActors;
	TArray<FAreaActionsLightweightIndex> TrackedLightweights;
	int32 NumRequested = 0;
	int32 NumSkipped = 0;
};
