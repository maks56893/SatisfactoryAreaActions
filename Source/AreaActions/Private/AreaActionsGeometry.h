#pragma once

#include "CoreMinimal.h"
#include "AreaActionsLightweightLibrary.h"

/** Geometry used to match buildings against the selected area. Uses only engine math types so it can be unit tested
 * (Private/Tests) and mirrored in Tools/GeometryMirror. */
namespace AreaActionsGeometry
{
	/** Bounds are shrunk by this much (cm) before matching, so a building snapped exactly to the area border
	 * counts as inside and its neighbour on the other side as outside. */
	constexpr double BoundsTolerance = 1.0;

	inline FBox ShrinkForMatching(const FBox& LocalBox)
	{
		const FVector Extent = LocalBox.GetExtent();
		const FVector Shrink(
			FMath::Min(BoundsTolerance, Extent.X * 0.5),
			FMath::Min(BoundsTolerance, Extent.Y * 0.5),
			FMath::Min(BoundsTolerance, Extent.Z * 0.5));
		return FBox(LocalBox.Min + Shrink, LocalBox.Max - Shrink);
	}

	/** Corner i has X from bit 0, Y from bit 1, Z from bit 2 (0 = Min, 1 = Max). */
	inline void ComputeWorldCorners(const FBox& LocalBox, const FTransform& Transform, FVector (&OutCorners)[8])
	{
		for (int32 i = 0; i < 8; ++i)
		{
			const FVector Local(
				(i & 1) ? LocalBox.Max.X : LocalBox.Min.X,
				(i & 2) ? LocalBox.Max.Y : LocalBox.Min.Y,
				(i & 4) ? LocalBox.Max.Z : LocalBox.Min.Z);
			OutCorners[i] = Transform.TransformPosition(Local);
		}
	}

	inline double Cross(const FVector2D& O, const FVector2D& A, const FVector2D& B)
	{
		return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
	}

	/** Convex hull (Andrew's monotone chain) of the XY projection of the box corners. */
	inline TArray<FVector2D, TInlineAllocator<16>> ProjectedHull(const FVector (&Corners)[8])
	{
		TArray<FVector2D, TInlineAllocator<16>> Points;
		for (const FVector& C : Corners)
		{
			Points.Add(FVector2D(C.X, C.Y));
		}
		Points.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });

		TArray<FVector2D, TInlineAllocator<16>> Hull;
		Hull.SetNum(Points.Num() * 2);
		int32 K = 0;
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			while (K >= 2 && Cross(Hull[K - 2], Hull[K - 1], Points[i]) <= 0) { --K; }
			Hull[K++] = Points[i];
		}
		for (int32 i = Points.Num() - 2, T = K + 1; i >= 0; --i)
		{
			while (K >= T && Cross(Hull[K - 2], Hull[K - 1], Points[i]) <= 0) { --K; }
			Hull[K++] = Points[i];
		}
		Hull.SetNum(FMath::Max(K - 1, 1));
		return Hull;
	}

	/** Even-odd rule, works for concave polygons. */
	template <typename TPolygon>
	inline bool PointInPolygon(const FVector2D& P, const TPolygon& Polygon)
	{
		bool bInside = false;
		for (int32 i = 0, j = Polygon.Num() - 1; i < Polygon.Num(); j = i++)
		{
			const FVector2D& A = Polygon[i];
			const FVector2D& B = Polygon[j];
			if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}

	inline bool SegmentsIntersect(const FVector2D& P1, const FVector2D& P2, const FVector2D& Q1, const FVector2D& Q2)
	{
		const double D1 = Cross(Q1, Q2, P1);
		const double D2 = Cross(Q1, Q2, P2);
		const double D3 = Cross(P1, P2, Q1);
		const double D4 = Cross(P1, P2, Q2);
		return ((D1 > 0 && D2 < 0) || (D1 < 0 && D2 > 0)) && ((D3 > 0 && D4 < 0) || (D3 < 0 && D4 > 0));
	}

	template <typename TPolygonA, typename TPolygonB>
	inline bool AnyEdgesIntersect(const TPolygonA& A, const TPolygonB& B)
	{
		if (A.Num() < 2 || B.Num() < 2)
		{
			return false;
		}
		for (int32 i = 0, j = A.Num() - 1; i < A.Num(); j = i++)
		{
			for (int32 k = 0, l = B.Num() - 1; k < B.Num(); l = k++)
			{
				if (SegmentsIntersect(A[j], A[i], B[l], B[k]))
				{
					return true;
				}
			}
		}
		return false;
	}

	inline bool MatchesArea(const FVector (&Corners)[8], const TArray<FVector2D>& Polygon, const FBox2D& PolygonBox, double MinZ, double MaxZ, EAreaActionsAreaTest Test)
	{
		double ZMin = TNumericLimits<double>::Max();
		double ZMax = TNumericLimits<double>::Lowest();
		FVector Center = FVector::ZeroVector;
		FBox2D Box2D(ForceInit);
		for (const FVector& C : Corners)
		{
			ZMin = FMath::Min(ZMin, C.Z);
			ZMax = FMath::Max(ZMax, C.Z);
			Center += C / 8.0;
			Box2D += FVector2D(C.X, C.Y);
		}

		switch (Test)
		{
		case EAreaActionsAreaTest::Center:
			return Center.Z >= MinZ && Center.Z <= MaxZ && PointInPolygon(FVector2D(Center.X, Center.Y), Polygon);

		case EAreaActionsAreaTest::FullyInside:
		{
			if (ZMin < MinZ || ZMax > MaxZ || !PolygonBox.IsInside(Box2D))
			{
				return false;
			}
			const auto Hull = ProjectedHull(Corners);
			for (const FVector2D& P : Hull)
			{
				if (!PointInPolygon(P, Polygon))
				{
					return false;
				}
			}
			// All hull points inside a concave polygon can still have an edge leaving it.
			return !AnyEdgesIntersect(Hull, Polygon);
		}

		case EAreaActionsAreaTest::Intersects:
		default:
		{
			if (ZMax < MinZ || ZMin > MaxZ || !PolygonBox.Intersect(Box2D))
			{
				return false;
			}
			const auto Hull = ProjectedHull(Corners);
			for (const FVector2D& P : Hull)
			{
				if (PointInPolygon(P, Polygon))
				{
					return true;
				}
			}
			// Building larger than (or surrounding) part of the area.
			if (Hull.Num() >= 3)
			{
				for (const FVector2D& P : Polygon)
				{
					if (PointInPolygon(P, Hull))
					{
						return true;
					}
				}
			}
			return AnyEdgesIntersect(Hull, Polygon);
		}
		}
	}
}
