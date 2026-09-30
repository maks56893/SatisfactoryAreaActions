#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGFactoryColoringTypes.h"
#include "FGDynamicStruct.h"
#include "FGInventoryComponent.h"
#include "AreaActionsLightweightLibrary.generated.h"

class AFGBuildable;
class UFGRecipe;

/** How a building is matched against the selected area (polygon + Z range). */
UENUM(BlueprintType)
enum class EAreaActionsAreaTest : uint8
{
	/** Bounds overlap the area. */
	Intersects,
	/** Bounds are completely inside the area. */
	FullyInside,
	/** Center of the bounds is inside the area. */
	Center
};

/**
 * Everything needed to re-create a lightweight buildable (foundation, wall, ...).
 * Used by Copy/Fill (spawn at a new transform) and by the backup/undo system.
 */
USTRUCT(BlueprintType)
struct AREAACTIONS_API FAreaActionsLightweightSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, Category = "Area Actions")
	TSubclassOf<AFGBuildable> BuildableClass;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, Category = "Area Actions")
	FTransform Transform;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, Category = "Area Actions")
	TSubclassOf<UFGRecipe> BuiltWithRecipe;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, Category = "Area Actions")
	FFactoryCustomizationData CustomizationData;

	/** Type-specific data, e.g. beam length. Must be kept when copying. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, SaveGame, Category = "Area Actions")
	FFGDynamicStruct TypeSpecificData;
};

/**
 * Since 1.0 walls, foundations, beams etc. are "lightweight buildables": they have no actor, so
 * GetAllActorsOfClass does not see them, and the lightweight subsystem only exposes per-instance
 * functions to Blueprint. This library adds the area-wide operations Area Actions needs.
 *
 * Functions that modify the world require authority (host / dedicated server) and return
 * 0 / false on clients.
 */
UCLASS()
class AREAACTIONS_API UAreaActionsLightweightLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** True if buildings of this class are managed by the lightweight buildable subsystem (never exist as persistent actors). */
	UFUNCTION(BlueprintPure, Category = "Area Actions|Lightweight")
	static bool IsLightweightBuildableClass(TSubclassOf<AFGBuildable> BuildableClass);

	/**
	 * Returns all lightweight buildables matching the area.
	 * @param AreaPolygon  Area corners in world XY, in order (the same Corners2D the tool uses). At least 3 points.
	 * @param MinZ/MaxZ    Vertical range of the area.
	 * @param ClassFilter  Only return buildables that are children of one of these classes. Empty = all.
	 */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Lightweight", meta = (WorldContext = "WorldContext", AutoCreateRefTerm = "ClassFilter"))
	static TArray<FLightweightBuildableInstanceRef> GetLightweightsInArea(UObject* WorldContext, const TArray<FVector2D>& AreaPolygon, double MinZ, double MaxZ, EAreaActionsAreaTest Test, const TArray<TSubclassOf<AFGBuildable>>& ClassFilter);

	/** 8 world-space corners of the lightweight's bounding box; corner i takes X/Y/Z max from bits 0/1/2. False if the instance is gone. */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Lightweight")
	static bool GetLightweightWorldCorners(const FLightweightBuildableInstanceRef& Instance, TArray<FVector>& OutCorners);

	/** Items the player gets back when dismantling this lightweight (respects recipe cost multipliers). False if the instance is gone. */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Lightweight", meta = (WorldContext = "WorldContext"))
	static bool GetLightweightDismantleRefund(UObject* WorldContext, const FLightweightBuildableInstanceRef& Instance, TArray<FInventoryStack>& OutRefund);

	/** Removes the given lightweights from the world (no refund - use GetLightweightDismantleRefund). Returns the number removed. Authority only. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Area Actions|Lightweight", meta = (WorldContext = "WorldContext"))
	static int32 RemoveLightweights(UObject* WorldContext, const TArray<FLightweightBuildableInstanceRef>& Instances);

	/** Captures everything needed to re-create the lightweight. False if the instance is gone. */
	UFUNCTION(BlueprintCallable, Category = "Area Actions|Lightweight")
	static bool MakeLightweightSnapshot(const FLightweightBuildableInstanceRef& Instance, FAreaActionsLightweightSnapshot& OutSnapshot);

	/** Creates a lightweight buildable from a snapshot at Snapshot.Transform. Authority only. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Area Actions|Lightweight", meta = (WorldContext = "WorldContext"))
	static bool SpawnLightweight(UObject* WorldContext, const FAreaActionsLightweightSnapshot& Snapshot, FLightweightBuildableInstanceRef& OutInstance);

};
