#include "AreaActionsLightweightLibrary.h"

#include "AreaActions.h"
#include "AreaActionsGeometry.h"
#include "Buildables/FGBuildable.h"
#include "Engine/StaticMesh.h"
#include "FGRecipe.h"
#include "InstanceData.h"

using namespace AreaActionsGeometry;

namespace
{
	/** Local-space bounds of a lightweight. The runtime bounds are only populated where instances were spawned
	 * (not necessarily on a dedicated server), so fall back to the meshes the class would create. */
	FBox GetLocalBounds(UClass* BuildableClass, const FRuntimeBuildableInstanceData& Data, TMap<UClass*, FBox>& ClassBoundsCache)
	{
		if (Data.BoundingBox.IsValid)
		{
			return Data.BoundingBox;
		}
		// Type-specific data (e.g. beam length) changes the meshes, so only cache bounds of instances without it.
		const bool bCacheable = !Data.TypeSpecificData.IsValid();
		if (bCacheable)
		{
			if (const FBox* Cached = ClassBoundsCache.Find(BuildableClass))
			{
				return *Cached;
			}
		}
		FBox Box(ForceInit);
		if (const AFGBuildable* CDO = BuildableClass ? BuildableClass->GetDefaultObject<AFGBuildable>() : nullptr)
		{
			TArray<FInstanceData> Instances;
			CDO->CreateLightweightBuildableInstanceData(Data.TypeSpecificData, Instances);
			for (const FInstanceData& Instance : Instances)
			{
				if (Instance.StaticMesh)
				{
					Box += Instance.StaticMesh->GetBoundingBox().TransformBy(Instance.RelativeTransform);
				}
			}
		}
		if (!Box.IsValid)
		{
			Box = FBox(FVector::ZeroVector, FVector::ZeroVector);
		}
		if (bCacheable)
		{
			ClassBoundsCache.Add(BuildableClass, Box);
		}
		return Box;
	}

	AFGBuildable* GetBuildableCDO(TSubclassOf<AFGBuildable> BuildableClass)
	{
		return BuildableClass ? BuildableClass->GetDefaultObject<AFGBuildable>() : nullptr;
	}
}

bool UAreaActionsLightweightLibrary::IsLightweightBuildableClass(TSubclassOf<AFGBuildable> BuildableClass)
{
	const AFGBuildable* CDO = GetBuildableCDO(BuildableClass);
	return CDO && CDO->GetLightweightInstanceData() && CDO->ManagedByLightweightBuildableSubsystem();
}

TArray<FLightweightBuildableInstanceRef> UAreaActionsLightweightLibrary::GetLightweightsInArea(UObject* WorldContext, const TArray<FVector2D>& AreaPolygon, double MinZ, double MaxZ, EAreaActionsAreaTest Test, const TArray<TSubclassOf<AFGBuildable>>& ClassFilter)
{
	TArray<FLightweightBuildableInstanceRef> Result;
	if (AreaPolygon.Num() < 3)
	{
		return Result;
	}
	AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(WorldContext);
	if (!Subsystem)
	{
		return Result;
	}

	const FBox2D PolygonBox(AreaPolygon);
	TMap<UClass*, FBox> ClassBoundsCache;
	for (const auto& Pair : Subsystem->GetAllLightweightBuildableInstances())
	{
		UClass* BuildableClass = Pair.Key.Get();
		if (!BuildableClass)
		{
			continue;
		}
		if (ClassFilter.Num() > 0 && !ClassFilter.ContainsByPredicate([BuildableClass](const TSubclassOf<AFGBuildable>& Filter) { return Filter && BuildableClass->IsChildOf(Filter); }))
		{
			continue;
		}
		const TArray<FRuntimeBuildableInstanceData>& Instances = Pair.Value;
		for (int32 Index = 0; Index < Instances.Num(); ++Index)
		{
			const FRuntimeBuildableInstanceData& Data = Instances[Index];
			// Removed instances are cleared (recipe reset) and their slot is reused later.
			if (!Data.BuiltWithRecipe)
			{
				continue;
			}
			FVector Corners[8];
			ComputeWorldCorners(ShrinkForMatching(GetLocalBounds(BuildableClass, Data, ClassBoundsCache)), Data.Transform, Corners);
			if (MatchesArea(Corners, AreaPolygon, PolygonBox, MinZ, MaxZ, Test))
			{
				Result.AddDefaulted_GetRef().Initialize(Subsystem, BuildableClass, Index);
			}
		}
	}
	return Result;
}

bool UAreaActionsLightweightLibrary::GetLightweightWorldCorners(const FLightweightBuildableInstanceRef& Instance, TArray<FVector>& OutCorners)
{
	OutCorners.Reset();
	const FRuntimeBuildableInstanceData* Data = Instance.ResolveBuildableInstanceData();
	if (!Data)
	{
		return false;
	}
	TMap<UClass*, FBox> ClassBoundsCache;
	FVector Corners[8];
	ComputeWorldCorners(GetLocalBounds(Instance.GetBuildableClass().Get(), *Data, ClassBoundsCache), Data->Transform, Corners);
	OutCorners.Append(Corners, 8);
	return true;
}

bool UAreaActionsLightweightLibrary::GetLightweightDismantleRefund(UObject* WorldContext, const FLightweightBuildableInstanceRef& Instance, TArray<FInventoryStack>& OutRefund)
{
	OutRefund.Reset();
	const FRuntimeBuildableInstanceData* Data = Instance.ResolveBuildableInstanceData();
	const AFGBuildable* CDO = GetBuildableCDO(Instance.GetBuildableClass());
	if (!Data || !CDO)
	{
		return false;
	}
	CDO->GetLightweightBuildableDismantleRefundReturns(WorldContext, Data->BuiltWithRecipe, Data->TypeSpecificData, OutRefund);
	return true;
}

int32 UAreaActionsLightweightLibrary::RemoveLightweights(UObject* WorldContext, const TArray<FLightweightBuildableInstanceRef>& Instances)
{
	const AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(WorldContext);
	if (!Subsystem || !Subsystem->HasAuthority())
	{
		UE_LOG(LogAreaActions, Warning, TEXT("RemoveLightweights called without authority"));
		return 0;
	}
	int32 Removed = 0;
	for (FLightweightBuildableInstanceRef Instance : Instances)
	{
		if (Instance.Remove())
		{
			++Removed;
		}
	}
	return Removed;
}

bool UAreaActionsLightweightLibrary::MakeLightweightSnapshot(const FLightweightBuildableInstanceRef& Instance, FAreaActionsLightweightSnapshot& OutSnapshot)
{
	const FRuntimeBuildableInstanceData* Data = Instance.ResolveBuildableInstanceData();
	if (!Data)
	{
		return false;
	}
	OutSnapshot.BuildableClass = Instance.GetBuildableClass();
	OutSnapshot.Transform = Data->Transform;
	OutSnapshot.BuiltWithRecipe = Data->BuiltWithRecipe;
	OutSnapshot.CustomizationData = Data->CustomizationData;
	OutSnapshot.TypeSpecificData = Data->TypeSpecificData;
	return true;
}

bool UAreaActionsLightweightLibrary::SpawnLightweight(UObject* WorldContext, const FAreaActionsLightweightSnapshot& Snapshot, FLightweightBuildableInstanceRef& OutInstance)
{
	AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(WorldContext);
	if (!Subsystem || !Subsystem->HasAuthority())
	{
		UE_LOG(LogAreaActions, Warning, TEXT("SpawnLightweight called without authority"));
		return false;
	}
	if (!Snapshot.BuiltWithRecipe || !IsLightweightBuildableClass(Snapshot.BuildableClass))
	{
		UE_LOG(LogAreaActions, Warning, TEXT("SpawnLightweight: %s is not a lightweight buildable or has no recipe"), *GetNameSafe(Snapshot.BuildableClass));
		return false;
	}

	FRuntimeBuildableInstanceData Data;
	Data.Transform = Snapshot.Transform;
	Data.CustomizationData = Snapshot.CustomizationData;
	Data.BuiltWithRecipe = Snapshot.BuiltWithRecipe;
	Data.TypeSpecificData = Snapshot.TypeSpecificData;

	const int32 Index = Subsystem->AddFromBuildableInstanceData(Snapshot.BuildableClass, Data);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	OutInstance.Initialize(Subsystem, Snapshot.BuildableClass, Index);
	return true;
}
