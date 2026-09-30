// Run without the game: UnrealEditor-Cmd.exe FactoryGame.uproject -ExecCmds="Automation RunTests AreaActions; Quit" -unattended -nullrhi -nosplash
#include "Misc/AutomationTest.h"
#include "AreaActionsGeometry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Box of the given half extent (cm), rotated by Yaw around its center, matched the same way as GetLightweightsInArea. */
	bool Match(const FVector& Center, const FVector& HalfExtent, double Yaw, const TArray<FVector2D>& Polygon, double MinZ, double MaxZ, EAreaActionsAreaTest Test)
	{
		FVector Corners[8];
		AreaActionsGeometry::ComputeWorldCorners(AreaActionsGeometry::ShrinkForMatching(FBox(-HalfExtent, HalfExtent)), FTransform(FRotator(0, Yaw, 0), Center), Corners);
		return AreaActionsGeometry::MatchesArea(Corners, Polygon, FBox2D(Polygon), MinZ, MaxZ, Test);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAreaActionsGeometryTest, "AreaActions.Geometry.MatchesArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::ProductFilter)

bool FAreaActionsGeometryTest::RunTest(const FString& Parameters)
{
	using T = EAreaActionsAreaTest;
	const TArray<FVector2D> Square = {{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
	// L shape: the square minus its top-right quarter (notch x,y in 500..1000)
	const TArray<FVector2D> LShape = {{0, 0}, {1000, 0}, {1000, 500}, {500, 500}, {500, 1000}, {0, 1000}};
	// U shape: gap x in 400..600, y above 400
	const TArray<FVector2D> UShape = {{0, 0}, {1000, 0}, {1000, 1000}, {600, 1000}, {600, 400}, {400, 400}, {400, 1000}, {0, 1000}};
	const FVector Foundation(400, 400, 50); // 8m x 8m x 1m foundation, half extents

	// Well inside
	TestTrue(TEXT("inside: intersects"), Match({500, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::Intersects));
	TestTrue(TEXT("inside: fully inside"), Match({500, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::FullyInside));
	TestTrue(TEXT("inside: center"), Match({500, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::Center));

	// Straddling the right edge, center outside
	TestTrue(TEXT("edge: intersects"), Match({1050, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::Intersects));
	TestFalse(TEXT("edge: fully inside"), Match({1050, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::FullyInside));
	TestFalse(TEXT("edge: center"), Match({1050, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::Center));

	// Outside
	TestFalse(TEXT("outside: intersects"), Match({2000, 500, 0}, {100, 100, 50}, 0, Square, -100, 100, T::Intersects));

	// Z range
	TestFalse(TEXT("above: intersects"), Match({500, 500, 1000}, {100, 100, 50}, 0, Square, -100, 100, T::Intersects));
	TestTrue(TEXT("overlapping Z: intersects"), Match({500, 500, 140}, {100, 100, 50}, 0, Square, -100, 100, T::Intersects));
	TestFalse(TEXT("overlapping Z: fully inside"), Match({500, 500, 140}, {100, 100, 50}, 0, Square, -100, 100, T::FullyInside));

	// Area completely inside one huge building: no building corner is in the area
	TestTrue(TEXT("surrounding: intersects"), Match({500, 500, 0}, {5000, 5000, 50}, 0, Square, -100, 100, T::Intersects));
	TestFalse(TEXT("surrounding: fully inside"), Match({500, 500, 0}, {5000, 5000, 50}, 0, Square, -100, 100, T::FullyInside));

	// Thin wall crossing the whole area: no corners inside either shape, only edges cross
	TestTrue(TEXT("crossing: intersects"), Match({500, 500, 0}, {2000, 10, 50}, 0, Square, -100, 100, T::Intersects));

	// Rotated 45 deg near the corner: axis-aligned bounds would overlap, the rotated box does not
	TestFalse(TEXT("rotated: intersects"), Match({-80, -80, 0}, {100, 10, 50}, -45, Square, -100, 100, T::Intersects));
	TestTrue(TEXT("rotated inside: fully inside"), Match({500, 500, 0}, {300, 10, 50}, 45, Square, -100, 100, T::FullyInside));

	// Concave area
	TestFalse(TEXT("L notch: intersects"), Match({750, 750, 0}, {100, 100, 50}, 0, LShape, -100, 100, T::Intersects));
	TestTrue(TEXT("L arm: fully inside"), Match({250, 750, 0}, {100, 100, 50}, 0, LShape, -100, 100, T::FullyInside));
	// All four corners in the L, but the box spans the notch
	TestFalse(TEXT("L across notch: fully inside"), Match({500, 500, 0}, {450, 450, 50}, 0, LShape, -100, 100, T::FullyInside));
	TestTrue(TEXT("L across notch: intersects"), Match({500, 500, 0}, {450, 450, 50}, 0, LShape, -100, 100, T::Intersects));
	// All four corners in the arms of the U, the middle spans the gap: only the edge test catches it
	TestFalse(TEXT("U across gap: fully inside"), Match({500, 700, 0}, {400, 100, 50}, 0, UShape, -100, 100, T::FullyInside));
	TestTrue(TEXT("U across gap: intersects"), Match({500, 700, 0}, {400, 100, 50}, 0, UShape, -100, 100, T::Intersects));

	// Real-size foundation on a 3x3 foundation grid area
	const TArray<FVector2D> Grid = {{0, 0}, {2400, 0}, {2400, 2400}, {0, 2400}};
	TestTrue(TEXT("foundation: fully inside"), Match({1200, 1200, 0}, Foundation, 0, Grid, -100, 100, T::FullyInside));
	TestTrue(TEXT("foundation on border: fully inside"), Match({400, 400, 0}, Foundation, 0, Grid, -100, 100, T::FullyInside));
	TestFalse(TEXT("neighbour foundation: intersects"), Match({2800, 1200, 0}, Foundation, 0, Grid, -100, 100, T::Intersects));

	// Hull of an axis-aligned box is its 4 XY corners
	FVector Corners[8];
	AreaActionsGeometry::ComputeWorldCorners(FBox(-Foundation, Foundation), FTransform::Identity, Corners);
	TestEqual(TEXT("hull size"), AreaActionsGeometry::ProjectedHull(Corners).Num(), 4);

	return true;
}

#endif
