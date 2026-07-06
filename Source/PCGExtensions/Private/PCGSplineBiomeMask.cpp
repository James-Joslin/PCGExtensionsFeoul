// PCGSplineBiomeMask.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGSplineBiomeMask.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSplineData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGSplineBiomeMask)

namespace
{
	const FName SplinesLabel(TEXT("Splines"));

	// ── Point-to-segment squared distance (2D). ──
	FORCEINLINE float PointToSegmentDistSq2D(
		const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D AB = B - A;
		const float LenSq = AB.SizeSquared();
		float T = 0.0f;
		if (LenSq > SMALL_NUMBER)
		{
			T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / LenSq, 0.0f, 1.0f);
		}
		const FVector2D Closest = A + T * AB;
		return FVector2D::DistSquared(P, Closest);
	}

	// ── Hermite smoothstep. ──
	FORCEINLINE float SmoothStep01(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
}

// ─────────────────────────────────────────────
//  Settings — pins
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGSplineBiomeMaskSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGSplineBiomeMask", "InTooltip",
			"Candidate points to stamp biome weights onto (Surface Sampler / "
			"jittered grid / Poisson). Points pass through unchanged except for "
			"the addition of float biome weight attributes."));

	Pins.Emplace(SplinesLabel,
		EPCGDataType::Spline,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGSplineBiomeMask", "SplineTooltip",
			"Closed-loop spline data from biome boundary actors. Each spline's "
			"source actor must carry an Actor Tag matching a BiomeEntry (e.g. "
			"\"Forest\"). Multiple splines can share the same biome tag."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGSplineBiomeMaskSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGSplineBiomeMask", "OutTooltip",
			"Input points enriched with per-biome float weight attributes "
			"(e.g. Biome_Forest = 0.7). Feed into TieredVegetationScatter / "
			"GroundCoverScatter."));

	return Pins;
}

FPCGElementPtr UPCGSplineBiomeMaskSettings::CreateElement() const
{
	return MakeShared<FPCGSplineBiomeMaskElement>();
}

// ─────────────────────────────────────────────
//  Helpers — spline sampling
// ─────────────────────────────────────────────

TArray<FVector2D> FPCGSplineBiomeMaskElement::SampleSplineToPolyline(
	const UPCGSplineData* SplineData, int32 SamplesPerSegment)
{
	TArray<FVector2D> Points;

	if (!SplineData)
	{
		return Points;
	}

	const int32 NumSegments = SplineData->GetNumSegments();
	if (NumSegments <= 0)
	{
		return Points;
	}

	Points.Reserve(NumSegments * SamplesPerSegment + 1);

	for (int32 Seg = 0; Seg < NumSegments; ++Seg)
	{
		const FVector::FReal SegLen = SplineData->GetSegmentLength(Seg);

		for (int32 S = 0; S < SamplesPerSegment; ++S)
		{
			const FVector::FReal Dist =
				static_cast<FVector::FReal>(S) / static_cast<FVector::FReal>(SamplesPerSegment) * SegLen;
			const FTransform T = SplineData->GetTransformAtDistance(Seg, Dist, /*bWorldSpace=*/true);
			const FVector Loc = T.GetLocation();
			Points.Add(FVector2D(Loc.X, Loc.Y));
		}
	}

	// BUGFIX: each segment samples S = 0 .. SamplesPerSegment-1, so a segment's
	// endpoint is only covered by the NEXT segment's start sample. That is correct
	// for closed loops (the last segment wraps to the first point), but for OPEN
	// splines the true final control point was dropped entirely, distorting the
	// implicit closing chord. Append it explicitly.
	if (!SplineData->SplineStruct.bClosedLoop && NumSegments > 0)
	{
		const int32 LastSeg = NumSegments - 1;
		const FTransform TEnd = SplineData->GetTransformAtDistance(
			LastSeg, SplineData->GetSegmentLength(LastSeg), /*bWorldSpace=*/true);
		const FVector EndLoc = TEnd.GetLocation();
		Points.Add(FVector2D(EndLoc.X, EndLoc.Y));
	}

	return Points;
}

// ─────────────────────────────────────────────
//  Helpers — geometry
// ─────────────────────────────────────────────

bool FPCGSplineBiomeMaskElement::PointInPolygon2D(
	const FVector2D& P, const TArray<FVector2D>& Polygon)
{
	// 2D ray-casting: count how many polygon edges a rightward ray from P crosses.
	// Odd crossings = inside.
	const int32 N = Polygon.Num();
	if (N < 3)
	{
		return false;
	}

	int32 Crossings = 0;
	for (int32 I = 0, J = N - 1; I < N; J = I++)
	{
		const FVector2D& A = Polygon[J];
		const FVector2D& B = Polygon[I];

		const bool bABelow = (A.Y <= P.Y);
		const bool bBBelow = (B.Y <= P.Y);
		if (bABelow == bBBelow)
		{
			continue; // edge doesn't straddle the ray's Y
		}

		// X intercept of edge at P.Y
		const float XIntercept = A.X + (P.Y - A.Y) / (B.Y - A.Y) * (B.X - A.X);
		if (P.X < XIntercept)
		{
			++Crossings;
		}
	}

	return (Crossings & 1) != 0;
}

float FPCGSplineBiomeMaskElement::DistanceToPolyline2D(
	const FVector2D& P, const TArray<FVector2D>& Polyline)
{
	const int32 N = Polyline.Num();
	if (N == 0)
	{
		return BIG_NUMBER;
	}
	if (N == 1)
	{
		return FVector2D::Distance(P, Polyline[0]);
	}

	float MinDistSq = BIG_NUMBER;

	// Walk consecutive segments.
	for (int32 I = 0; I < N - 1; ++I)
	{
		MinDistSq = FMath::Min(MinDistSq,
			PointToSegmentDistSq2D(P, Polyline[I], Polyline[I + 1]));
	}

	// Closing segment (first ↔ last) — polygon is always closed.
	MinDistSq = FMath::Min(MinDistSq,
		PointToSegmentDistSq2D(P, Polyline.Last(), Polyline[0]));

	return FMath::Sqrt(MinDistSq);
}

// ─────────────────────────────────────────────
//  Helpers — falloff
// ─────────────────────────────────────────────

float FPCGSplineBiomeMaskElement::ApplyFalloff(
	float Distance, float InnerRadius, float OuterRadius, EPCGBiomeFalloffType Type)
{
	if (Distance <= InnerRadius)
	{
		return 1.0f;
	}
	if (Distance >= OuterRadius || OuterRadius <= InnerRadius)
	{
		return 0.0f;
	}

	const float T = (Distance - InnerRadius) / (OuterRadius - InnerRadius); // 0 → 1

	switch (Type)
	{
	case EPCGBiomeFalloffType::Linear:
		return 1.0f - T;

	case EPCGBiomeFalloffType::SmoothStep:
		return 1.0f - SmoothStep01(T);

	case EPCGBiomeFalloffType::Exponential:
		// Steep initial drop, gentle tail. e^(-3T) normalised so f(0)=1, f(1)≈0.
		return FMath::Clamp((FMath::Exp(-3.0f * T) - FMath::Exp(-3.0f)) / (1.0f - FMath::Exp(-3.0f)),
			0.0f, 1.0f);

	default:
		return 1.0f - T;
	}
}

// ─────────────────────────────────────────────
//  Helpers — noise
// ─────────────────────────────────────────────

float FPCGSplineBiomeMaskElement::Perlin01(const FVector& Position, float Frequency, int32 Seed)
{
	// Same implementation as TieredVegetationScatter / GroundCoverScatter.
	const FVector SeedOffset(Seed * 13.13f, Seed * 7.77f, Seed * 3.33f);
	const float N = FMath::PerlinNoise3D(Position * Frequency + SeedOffset);
	return FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
}

// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

bool FPCGSplineBiomeMaskElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGSplineBiomeMaskElement::Execute);

	const UPCGSplineBiomeMaskSettings* Settings =
		Context->GetInputSettings<UPCGSplineBiomeMaskSettings>();
	check(Settings);

	// ── Early out: no biomes configured → pass through unchanged ──
	if (Settings->BiomeEntries.Num() == 0)
	{
		PCGE_LOG(Warning, GraphAndLog, NSLOCTEXT("PCGSplineBiomeMask", "NoBiomes",
			"Spline Biome Mask: No BiomeEntries configured. Points pass through unchanged."));
		Context->OutputData = Context->InputData;
		return true;
	}

	// ── 1. Collect spline data from the Splines pin ──
	const TArray<FPCGTaggedData> SplineInputs =
		Context->InputData.GetInputsByPin(SplinesLabel);

	// ── 2. Build biome groups: match splines to entries via actor tags ──
	const int32 NumBiomes = Settings->BiomeEntries.Num();
	TArray<FBiomeGroup> Groups;
	Groups.SetNum(NumBiomes);
	for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
	{
		Groups[Idx].EntryIndex = Idx;
	}

	int32 MatchedSplines = 0;
	int32 UnmatchedSplines = 0;

	for (const FPCGTaggedData& SpInput : SplineInputs)
	{
		const UPCGSplineData* SplineData = Cast<UPCGSplineData>(SpInput.Data);
		if (!SplineData)
		{
#if WITH_EDITOR
			if (Settings->bLogDiagnostics)
			{
				PCGE_LOG(Warning, GraphAndLog, NSLOCTEXT("PCGSplineBiomeMask", "NotSpline",
					"Spline Biome Mask: Non-spline data on Splines pin — skipping."));
			}
#endif
			continue;
		}

		// Try matching by source actor tags first, then FPCGTaggedData tags.
		int32 MatchIdx = INDEX_NONE;

		AActor* SourceActor = SplineData->TargetActor.Get();
		if (SourceActor)
		{
			for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
			{
				const FName& Tag = Settings->BiomeEntries[Idx].SplineActorTag;

				// BUGFIX: ActorHasTag only checks the ACTOR's Tags array. Tags placed
				// on the spline COMPONENT inside a Blueprint (a very easy mistake —
				// the Details "Tags" field on a selected component is a Component
				// Tag) were invisible, so those splines silently fell into the
				// unmatched bucket. Accept component tags as well.
				bool bMatch = SourceActor->ActorHasTag(Tag);
				if (!bMatch)
				{
					for (const UActorComponent* Comp : SourceActor->GetComponents())
					{
						if (Comp && Comp->ComponentHasTag(Tag))
						{
							bMatch = true;
							break;
						}
					}
				}

				if (bMatch)
				{
					MatchIdx = Idx;
					break;
				}
			}
		}

		// Fallback: check PCG-level tags on the tagged data wrapper.
		if (MatchIdx == INDEX_NONE)
		{
			for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
			{
				const FString TagStr = Settings->BiomeEntries[Idx].SplineActorTag.ToString();
				if (SpInput.Tags.Contains(TagStr))
				{
					MatchIdx = Idx;
					break;
				}
			}
		}

		if (MatchIdx == INDEX_NONE)
		{
			++UnmatchedSplines;
#if WITH_EDITOR
			if (Settings->bLogDiagnostics)
			{
				const FString ActorName = SourceActor
					? SourceActor->GetActorNameOrLabel()
					: TEXT("<no actor ref>");
				PCGE_LOG(Warning, GraphAndLog, FText::Format(
					NSLOCTEXT("PCGSplineBiomeMask", "Unmatched",
						"Spline Biome Mask: Spline from actor \"{0}\" did not match any "
						"BiomeEntry SplineActorTag. Skipping."),
					FText::FromString(ActorName)));
			}
#endif
			continue;
		}

		// Open (non-closed-loop) splines are implicitly closed with a straight
		// chord by the geometry tests — almost never the intent for a biome
		// boundary. Warn loudly so a forgotten "Closed Loop" checkbox on the
		// spline component doesn't silently produce a mangled polygon.
		if (!SplineData->SplineStruct.bClosedLoop)
		{
			AActor* WarnActor = SplineData->TargetActor.Get();
			PCGE_LOG(Warning, GraphAndLog, FText::Format(
				NSLOCTEXT("PCGSplineBiomeMask", "OpenSpline",
					"Spline Biome Mask: Spline from actor \"{0}\" is not a closed loop. "
					"The polygon will be closed with a straight chord between its "
					"endpoints — enable 'Closed Loop' on the spline component if this "
					"is a biome boundary."),
				FText::FromString(WarnActor ? WarnActor->GetActorNameOrLabel() : TEXT("<no actor ref>"))));
		}

		// Sample the spline into an XY polyline.
		FBiomePolyline Poly;
		Poly.Points = SampleSplineToPolyline(SplineData, Settings->SamplesPerSegment);

		if (Poly.Points.Num() < 3)
		{
#if WITH_EDITOR
			if (Settings->bLogDiagnostics)
			{
				PCGE_LOG(Warning, GraphAndLog, NSLOCTEXT("PCGSplineBiomeMask", "TooFewPts",
					"Spline Biome Mask: Spline sampled to fewer than 3 points — cannot "
					"form a polygon. Skipping."));
			}
#endif
			continue;
		}

		Groups[MatchIdx].Polylines.Add(MoveTemp(Poly));
		++MatchedSplines;
	}

#if WITH_EDITOR
	if (Settings->bLogDiagnostics)
	{
		PCGE_LOG(Log, GraphAndLog, FText::Format(
			NSLOCTEXT("PCGSplineBiomeMask", "MatchSummary",
				"Spline Biome Mask: {0} spline(s) matched, {1} unmatched, across {2} biome(s)."),
			FText::AsNumber(MatchedSplines),
			FText::AsNumber(UnmatchedSplines),
			FText::AsNumber(NumBiomes)));

		for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
		{
			PCGE_LOG(Log, GraphAndLog, FText::Format(
				NSLOCTEXT("PCGSplineBiomeMask", "BiomeDetail",
					"  [{0}] tag=\"{1}\" attr=\"{2}\" → {3} polyline(s)"),
				FText::AsNumber(Idx),
				FText::FromName(Settings->BiomeEntries[Idx].SplineActorTag),
				FText::FromName(Settings->BiomeEntries[Idx].BiomeAttributeName),
				FText::AsNumber(Groups[Idx].Polylines.Num())));
		}
	}
#endif

	if (MatchedSplines == 0)
	{
		PCGE_LOG(Warning, GraphAndLog, NSLOCTEXT("PCGSplineBiomeMask", "NoMatches",
			"Spline Biome Mask: No splines matched any BiomeEntry. "
			"Points pass through with all biome weights = 0."));
	}

	// ── 3. Process input points ──
	const TArray<FPCGTaggedData> PointInputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	// Pre-allocate per-point weight scratch buffer.
	TArray<float> Weights;
	Weights.SetNumZeroed(NumBiomes);

	for (const FPCGTaggedData& PtInput : PointInputs)
	{
		// ToPointData fallback for composite/union data.
		const UPCGPointData* InData = Cast<UPCGPointData>(PtInput.Data);
		if (!InData)
		{
			const UPCGSpatialData* Spatial = Cast<UPCGSpatialData>(PtInput.Data);
			if (Spatial)
			{
				InData = Spatial->ToPointData(Context);
			}
			if (!InData)
			{
				continue;
			}
		}

		const TArray<FPCGPoint>& InPoints = InData->GetPoints();
		if (InPoints.Num() == 0)
		{
			continue;
		}

		// Create output — inherits metadata schema from input.
		UPCGPointData* OutData = NewObject<UPCGPointData>();
		OutData->InitializeFromData(InData);
		TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
		OutPoints = InPoints; // copy points (preserves positions, seeds, etc.)

		UPCGMetadata* OutMeta = OutData->MutableMetadata();

		// Create (or find) a float attribute per biome on the output metadata.
		TArray<FPCGMetadataAttribute<float>*> BiomeAttrs;
		BiomeAttrs.SetNum(NumBiomes);
		for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
		{
			BiomeAttrs[Idx] = OutMeta->FindOrCreateAttribute<float>(
				Settings->BiomeEntries[Idx].BiomeAttributeName, 0.0f,
				/*bAllowInterpolation=*/true, /*bOverrideParent=*/true);
		}

		// Optional residual attribute: 1 - Σ(biome weights), i.e. how "default"
		// this point is. Replaces the fragile Create Constant + Subtract graph
		// pattern (whose attribute DEFAULT of 1.0 can camouflage metadata bugs
		// downstream by making broken points look like pure default biome).
		FPCGMetadataAttribute<float>* ResidualAttr = nullptr;
		if (Settings->bEmitResidualWeight && !Settings->ResidualAttributeName.IsNone())
		{
			ResidualAttr = OutMeta->FindOrCreateAttribute<float>(
				Settings->ResidualAttributeName, 0.0f,
				/*bAllowInterpolation=*/true, /*bOverrideParent=*/true);
		}

		// ── Per-point biome weight computation ──
#if WITH_EDITOR
		TArray<int32> InsideCounts;
		TArray<int32> FalloffCounts;
		if (Settings->bLogDiagnostics)
		{
			InsideCounts.SetNumZeroed(NumBiomes);
			FalloffCounts.SetNumZeroed(NumBiomes);
		}
#endif

		for (int32 PtIdx = 0; PtIdx < OutPoints.Num(); ++PtIdx)
		{
			FPCGPoint& Point = OutPoints[PtIdx];
			const FVector Pos = Point.Transform.GetLocation();
			const FVector2D PosXY(Pos.X, Pos.Y);

			// Localise the metadata entry before writing. InitializeOnSet handles
			// both cases correctly: an invalid key gets a fresh entry; a key that
			// belongs to the PARENT metadata gets a new local entry parented to it.
			// (Previously we wrote attribute values against parent-owned keys —
			// it happened to work, but it's the inverse of the sanctioned pattern
			// and fragile across engine versions.)
			OutMeta->InitializeOnSet(Point.MetadataEntry);

			// Compute weight for each biome.
			for (int32 BiomeIdx = 0; BiomeIdx < NumBiomes; ++BiomeIdx)
			{
				Weights[BiomeIdx] = 0.0f;
			}

			for (int32 BiomeIdx = 0; BiomeIdx < NumBiomes; ++BiomeIdx)
			{
				const FPCGBiomeMaskEntry& Entry = Settings->BiomeEntries[BiomeIdx];
				const FBiomeGroup& Group = Groups[BiomeIdx];

				float BestWeight = 0.0f;

				for (const FBiomePolyline& Poly : Group.Polylines)
				{
					// Inside the closed polygon → full weight.
					if (PointInPolygon2D(PosXY, Poly.Points))
					{
						BestWeight = 1.0f;
#if WITH_EDITOR
						if (Settings->bLogDiagnostics) { InsideCounts[BiomeIdx]++; }
#endif
						break; // can't beat 1.0
					}

					// Outside — compute distance to nearest polygon edge.
					float Dist = DistanceToPolyline2D(PosXY, Poly.Points);

					// Optional edge noise perturbation.
					if (Settings->bUseEdgeNoise)
					{
						// Noise output is 0–1; remap to [-Amplitude, +Amplitude].
						const float N = Perlin01(Pos, Settings->EdgeNoiseFrequency,
							Settings->EdgeNoiseSeed + BiomeIdx);
						const float Offset = (N - 0.5f) * 2.0f * Settings->EdgeNoiseAmplitude;
						// Positive offset pushes the effective boundary outward (increases
						// the outer radius), negative pulls it inward.
						Dist -= Offset;
						// Clamp so noise can't flip a point to negative distance.
						Dist = FMath::Max(0.0f, Dist);
					}

					const float W = ApplyFalloff(Dist, Entry.InnerRadius,
						Entry.OuterRadius, Entry.FalloffType);
					if (W > BestWeight)
					{
						BestWeight = W;
#if WITH_EDITOR
						if (Settings->bLogDiagnostics && W > 0.0f) { FalloffCounts[BiomeIdx]++; }
#endif
					}
				}

				Weights[BiomeIdx] = BestWeight;
			}

			// Optional normalisation: scale proportionally so Σ ≤ 1.0.
			if (Settings->bNormaliseWeights)
			{
				float Sum = 0.0f;
				for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
				{
					Sum += Weights[Idx];
				}
				if (Sum > 1.0f)
				{
					const float InvSum = 1.0f / Sum;
					for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
					{
						Weights[Idx] *= InvSum;
					}
				}
			}

			// Write attributes.
			for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
			{
				BiomeAttrs[Idx]->SetValue(Point.MetadataEntry, Weights[Idx]);
			}

			// Residual = whatever weight is left over once all biomes have claimed
			// theirs. With bNormaliseWeights on, Σ ≤ 1 is guaranteed post-scale.
			if (ResidualAttr)
			{
				float Sum = 0.0f;
				for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
				{
					Sum += Weights[Idx];
				}
				ResidualAttr->SetValue(Point.MetadataEntry,
					FMath::Clamp(1.0f - Sum, 0.0f, 1.0f));
			}
		}

#if WITH_EDITOR
		if (Settings->bLogDiagnostics)
		{
			PCGE_LOG(Log, GraphAndLog, FText::Format(
				NSLOCTEXT("PCGSplineBiomeMask", "PointSummary",
					"Spline Biome Mask: Processed {0} points."),
				FText::AsNumber(OutPoints.Num())));
			for (int32 Idx = 0; Idx < NumBiomes; ++Idx)
			{
				PCGE_LOG(Log, GraphAndLog, FText::Format(
					NSLOCTEXT("PCGSplineBiomeMask", "CoverageDetail",
						"  [{0}] {1}: {2} inside, {3} in falloff zone"),
					FText::AsNumber(Idx),
					FText::FromName(Settings->BiomeEntries[Idx].BiomeAttributeName),
					FText::AsNumber(InsideCounts[Idx]),
					FText::AsNumber(FalloffCounts[Idx])));
			}
		}
#endif

		// Emit.
		FPCGTaggedData& Out = Outputs.Emplace_GetRef();
		Out.Data = OutData;
		Out.Pin = PCGPinConstants::DefaultOutputLabel;
	}

	return true;
}