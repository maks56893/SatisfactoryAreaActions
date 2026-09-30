#include "AreaActionsDismantleSelector.h"

#include "AreaActions.h"
#include "Buildables/FGBuildable.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "FGCharacterPlayer.h"
#include "FGLightweightBuildableSubsystem.h"

AAreaActionsDismantleSelector::AAreaActionsDismantleSelector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	bReplicates = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

bool AAreaActionsDismantleSelector::SelectForDismantle(AFGCharacterPlayer* Player, const TArray<AActor*>& Actors, const TArray<FVector>& AreaCorners, double MinZ, double MaxZ, bool bIncludeLightweights)
{
	if (!Player || !Player->IsLocallyControlled() || !Player->GetBuildGun())
	{
		UE_LOG(LogAreaActions, Warning, TEXT("SelectForDismantle: needs the local player with a build gun"));
		return false;
	}
	UWorld* World = Player->GetWorld();

	// One selection at a time.
	for (TActorIterator<AAreaActionsDismantleSelector> It(World); It; ++It)
	{
		It->Destroy();
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	AAreaActionsDismantleSelector* Selector = World->SpawnActor<AAreaActionsDismantleSelector>(Player->GetActorLocation(), FRotator::ZeroRotator, Params);
	if (!Selector)
	{
		return false;
	}
	Selector->Player = Player;
	for (AActor* Actor : Actors)
	{
		// Temporaries stand in for lightweight buildings near the player or near a previous selection. The area
		// search below finds lightweights precisely; taking temporaries here would add duplicates and, through the
		// Blueprint-side bounds check, neighbours outside the area.
		const AFGBuildable* Buildable = Cast<AFGBuildable>(Actor);
		if (IsValid(Actor) && !(Buildable && Buildable->GetIsLightweightTemporary()))
		{
			Selector->ActorQueue.Add(Actor);
		}
	}

	if (bIncludeLightweights && AreaCorners.Num() >= 3)
	{
		TArray<FVector2D> Polygon;
		for (const FVector& Corner : AreaCorners)
		{
			Polygon.Add(FVector2D(Corner.X, Corner.Y));
		}
		UAreaActionsLightweightLibrary::FindLightweightsInArea(AFGLightweightBuildableSubsystem::Get(World), Polygon, MinZ, MaxZ,
			EAreaActionsAreaTest::Intersects, {}, Selector->LightweightQueue);
	}

	UE_LOG(LogAreaActions, Log, TEXT("SelectForDismantle: %d buildings, %d lightweight buildings"), Selector->ActorQueue.Num(), Selector->LightweightQueue.Num());
	if (Selector->ActorQueue.Num() == 0 && Selector->LightweightQueue.Num() == 0)
	{
		Selector->Destroy();
		return false;
	}

	// Equip the build gun (this unequips the Area Actions tool, which shares the arms slot).
	if (!Player->GetBuildGun()->IsEquipped())
	{
		Player->ToggleBuildGun();
	}
	return true;
}

AFGBuildGun* AAreaActionsDismantleSelector::GetBuildGun() const
{
	return Player.IsValid() ? Player->GetBuildGun() : nullptr;
}

UFGBuildGunStateDismantle* AAreaActionsDismantleSelector::GetDismantleState() const
{
	AFGBuildGun* BuildGun = GetBuildGun();
	return BuildGun ? Cast<UFGBuildGunStateDismantle>(BuildGun->GetBuildGunStateFor(EBuildGunState::BGS_DISMANTLE)) : nullptr;
}

void AAreaActionsDismantleSelector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	StageTime += DeltaSeconds;

	AFGBuildGun* BuildGun = GetBuildGun();
	UFGBuildGunStateDismantle* DismantleState = GetDismantleState();
	if (!BuildGun || !DismantleState)
	{
		Destroy();
		return;
	}
	const bool bInDismantle = BuildGun->IsEquipped() && BuildGun->IsInState(EBuildGunState::BGS_DISMANTLE);

	switch (Stage)
	{
	case EStage::WaitForDismantleState:
		if (bInDismantle)
		{
			// Add to the selection the way holding the multi-select key does; released again once done.
			if (!DismantleState->IsMultiDismantleActive())
			{
				DismantleState->SetMultiDismantleState(true);
				bEnabledMultiSelect = true;
			}
			RegisterInstanceConverter();
			Stage = EStage::AddToSelection;
			StageTime = 0.f;
		}
		else if (StageTime > StateTimeoutSeconds)
		{
			UE_LOG(LogAreaActions, Warning, TEXT("SelectForDismantle: build gun did not enter dismantle mode, giving up"));
			Destroy();
		}
		else if ((RetryTimer -= DeltaSeconds) <= 0.f)
		{
			// GotoDismantleState is ignored until the build gun has finished equipping, so keep asking.
			RetryTimer = 0.25f;
			if (BuildGun->IsEquipped())
			{
				BuildGun->GotoDismantleState();
			}
		}
		break;

	case EStage::AddToSelection:
		if (!bInDismantle)
		{
			Destroy();
			return;
		}
		AddBatch(DismantleState);
		if (ActorQueue.Num() == 0 && LightweightQueue.Num() == 0)
		{
			UE_LOG(LogAreaActions, Log, TEXT("SelectForDismantle: requested %d, skipped %d, now pending %d (limit %d)"),
				NumRequested, NumSkipped, DismantleState->GetNumPendingDismantleActors(false), DismantleState->GetMaxNumPendingDismantleActors());
			// Like releasing the multi-select key: the selection stays, aiming no longer adds to it.
			if (bEnabledMultiSelect)
			{
				DismantleState->SetMultiDismantleState(false);
				bEnabledMultiSelect = false;
			}
			Stage = EStage::KeepAlive;
			StageTime = 0.f;
		}
		break;

	case EStage::KeepAlive:
		// Keep the lightweight temporaries alive until the player dismantles, clears the selection or leaves the mode,
		// plus a grace period: the dismantle request may be processed after the pending list is already cleared.
		if (!bInDismantle || (StageTime > 1.f && DismantleState->GetNumPendingDismantleActors(false) == 0))
		{
			Stage = EStage::Grace;
			StageTime = 0.f;
		}
		break;

	case EStage::Grace:
		if (StageTime > GraceSeconds)
		{
			LogOutcome();
			Destroy();
		}
		break;
	}
}

void AAreaActionsDismantleSelector::RegisterInstanceConverter()
{
	if (LightweightQueue.Num() == 0)
	{
		return;
	}
	AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
	if (!Subsystem)
	{
		return;
	}
	FBox Bounds(ForceInit);
	for (const FAreaActionsLightweightIndex& Entry : LightweightQueue)
	{
		Bounds += Entry.Location;
	}
	// Margin covers the size of a single building around its pivot.
	const float Radius = Bounds.GetExtent().Size() + 2500.f;
	SetActorLocation(Bounds.GetCenter());
	Subsystem->AddInstanceConverterInstigator(Radius, this, FTransform(Bounds.GetCenter()));
	bConverterRegistered = true;
}

void AAreaActionsDismantleSelector::AddBatch(UFGBuildGunStateDismantle* DismantleState)
{
	AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(GetWorld());
	int32 Budget = BatchSize;

	while (Budget > 0 && ActorQueue.Num() > 0)
	{
		--Budget;
		AActor* Actor = ActorQueue.Pop(EAllowShrinking::No).Get();
		if (!IsValid(Actor))
		{
			++NumSkipped;
			continue;
		}
		DismantleState->AddPendingDismantleActor(Actor);
		TrackedActors.Add(Actor);
		++NumRequested;
	}

	while (Budget > 0 && LightweightQueue.Num() > 0 && Subsystem)
	{
		--Budget;
		const FAreaActionsLightweightIndex Entry = LightweightQueue.Pop(EAllowShrinking::No);
		FRuntimeBuildableInstanceData* Data = Subsystem->GetRuntimeDataForBuildableClassAndIndex(Entry.BuildableClass, Entry.Index);
		if (!Data || !Data->BuiltWithRecipe)
		{
			++NumSkipped;
			continue;
		}
		// The dismantle mode works on actors, so select the lightweight through its temporary buildable.
		bool bDidSpawn = false;
		FInstanceToTemporaryBuildable* Temporary = Subsystem->FindOrSpawnBuildableForRuntimeData(Entry.BuildableClass, Data, Entry.Index, bDidSpawn);
		if (!Temporary || !Temporary->Buildable)
		{
			++NumSkipped;
			continue;
		}
		DismantleState->AddPendingDismantleActor(Temporary->Buildable);
		TrackedLightweights.Add(Entry);
		++NumRequested;
	}
}

void AAreaActionsDismantleSelector::LogOutcome() const
{
	int32 ActorsGone = 0;
	for (const TWeakObjectPtr<AActor>& Actor : TrackedActors)
	{
		ActorsGone += Actor.IsValid() ? 0 : 1;
	}
	int32 LightweightsLeft = 0;
	if (AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(GetWorld()))
	{
		for (const FAreaActionsLightweightIndex& Entry : TrackedLightweights)
		{
			const FRuntimeBuildableInstanceData* Data = Subsystem->GetRuntimeDataForBuildableClassAndIndex(Entry.BuildableClass, Entry.Index);
			// The slot may have been reused by a new building; compare positions to be sure it is the same one.
			if (Data && Data->BuiltWithRecipe && Data->Transform.GetLocation().Equals(Entry.Location, 1.0))
			{
				++LightweightsLeft;
			}
		}
	}
	UE_LOG(LogAreaActions, Log, TEXT("SelectForDismantle: finished. Buildings removed %d/%d, lightweight buildings still present %d/%d"),
		ActorsGone, TrackedActors.Num(), LightweightsLeft, TrackedLightweights.Num());
}

void AAreaActionsDismantleSelector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bConverterRegistered)
	{
		if (AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(GetWorld()))
		{
			Subsystem->RemoveInstanceConverterInstigator(this);
		}
		bConverterRegistered = false;
	}
	Super::EndPlay(EndPlayReason);
}
