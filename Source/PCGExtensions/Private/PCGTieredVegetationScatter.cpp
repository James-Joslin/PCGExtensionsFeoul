// PCGTieredVegetationScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGTieredVegetationScatter.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "CollisionQueryParams.h"
#include "PCGExtScatterCommon.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGTieredVegetationScatter)

// ─────────────────────────────────────────────
//  Settings — pins
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGTieredVegetationScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "InTooltip",
			"Candidate points to place this tier from (Surface Sampler / jittered grid / "
			"Poisson). Do valley/volume subtraction upstream. Biome weight attributes "
			"(e.g. Biome_Meadow) read from these points if present."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGTieredVegetationScatterSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "OutTooltip",
			"Placed instances with a MeshPath attribute. Biome weight attributes from the "
			"input points are preserved. Feed a By-Attribute Static Mesh Spawner and/or "
			"further processing nodes."));

	Pins.Emplace(PCGExtScatterCommon::CompanionsPinLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "CompTooltip",
			"Optional understory ring points (transform-ready, mesh-less). Assign shrub "
			"meshes downstream or pipe into Scatter Around Points."));

	Pins.Emplace(PCGExtScatterCommon::RejectedPinLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "RejTooltip",
			"Culled candidates (slope / exclusion / noise / density). Debug only."));

	return Pins;
}

FPCGElementPtr UPCGTieredVegetationScatterSettings::CreateElement() const
{
	return MakeShared<FPCGTieredVegetationScatterElement>();
}

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────

bool FPCGTieredVegetationScatterElement::BuildMeshCache(
	const TArray<FPCGVegMeshEntry>& Entries,
	TArray<FVegMeshCache>& OutCache,
	float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGVegMeshEntry& Entry : Entries)
	{
		if (Entry.Mesh.IsNull())
		{
			continue;
		}

		FVegMeshCache CacheEntry;
		CacheEntry.MeshPath = Entry.Mesh.ToSoftObjectPath();
		CacheEntry.ScaleRange = Entry.ScaleRange;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		CacheEntry.CumulativeWeight = OutTotalWeight;
		OutCache.Add(CacheEntry);
	}

	return OutCache.Num() > 0;
}

int32 FPCGTieredVegetationScatterElement::SelectMeshIndex(
	const TArray<FVegMeshCache>& Cache, float TotalWeight, FRandomStream& Rng)
{
	const float Roll = Rng.FRandRange(0.0f, TotalWeight);
	for (int32 i = 0; i < Cache.Num(); ++i)
	{
		if (Roll <= Cache[i].CumulativeWeight)
		{
			return i;
		}
	}
	return Cache.Num() - 1;
}

float FPCGTieredVegetationScatterElement::ReadFloatAttr(
	const UPCGMetadata* Meta, FName Name, int64 EntryKey, float Default)
{
	if (!Meta || Name.IsNone() || !Meta->HasAttribute(Name))
	{
		return Default;
	}

	const FPCGMetadataAttributeBase* Base = Meta->GetConstAttribute(Name);
	if (!Base)
	{
		return Default;
	}

	// BUGFIX: this used to require the attribute to be EXACTLY float, silently
	// returning Default (0) for anything else. PCG's Create Constant and
	// Attribute Maths nodes produce DOUBLE by default, so a graph-authored
	// weight like Biome_Default read as 0 at every point — and the
	// WeightedAverage residual then quietly substituted a neutral density of
	// 1.0, producing uniform scatter across the whole map. Accept any common
	// numeric type instead.
	const int16 TypeId = Base->GetTypeId();
	if (TypeId == PCG::Private::MetadataTypes<float>::Id)
	{
		return static_cast<const FPCGMetadataAttribute<float>*>(Base)->GetValueFromItemKey(EntryKey);
	}
	if (TypeId == PCG::Private::MetadataTypes<double>::Id)
	{
		return static_cast<float>(
			static_cast<const FPCGMetadataAttribute<double>*>(Base)->GetValueFromItemKey(EntryKey));
	}
	if (TypeId == PCG::Private::MetadataTypes<int32>::Id)
	{
		return static_cast<float>(
			static_cast<const FPCGMetadataAttribute<int32>*>(Base)->GetValueFromItemKey(EntryKey));
	}
	if (TypeId == PCG::Private::MetadataTypes<int64>::Id)
	{
		return static_cast<float>(
			static_cast<const FPCGMetadataAttribute<int64>*>(Base)->GetValueFromItemKey(EntryKey));
	}

	return Default;
}

void FPCGTieredVegetationScatterElement::ComputeBiomeFactors(
	const TArray<FPCGVegBiomeResponse>& Responses,
	EPCGVegBiomeCombine Mode,
	const UPCGMetadata* Meta,
	int64 EntryKey,
	float& OutDensity,
	float& OutScale)
{
	OutDensity = 1.0f;
	OutScale = 1.0f;

	if (Responses.Num() == 0 || !Meta)
	{
		return;
	}

	switch (Mode)
	{
	case EPCGVegBiomeCombine::WeightedAverage:
	{
		float SumW = 0.0f, SumWD = 0.0f, SumWS = 0.0f;
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			SumW += W;
			SumWD += W * R.DensityMultiplier;
			SumWS += W * R.ScaleMultiplier;
		}
		// Residual weight (where no biome dominates) stays neutral at 1.0.
		const float Residual = FMath::Max(0.0f, 1.0f - SumW);
		const float Denom = SumW + Residual; // == max(1, SumW)
		if (Denom > KINDA_SMALL_NUMBER)
		{
			OutDensity = (SumWD + Residual * 1.0f) / Denom;
			OutScale = (SumWS + Residual * 1.0f) / Denom;
		}
		break;
	}
	case EPCGVegBiomeCombine::Max:
	{
		float BestW = 0.0f;
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			if (W > BestW)
			{
				BestW = W;
				OutDensity = R.DensityMultiplier;
				OutScale = R.ScaleMultiplier;
			}
		}
		// Fade toward neutral when even the dominant biome is weak.
		OutDensity = FMath::Lerp(1.0f, OutDensity, BestW);
		OutScale = FMath::Lerp(1.0f, OutScale, BestW);
		break;
	}
	case EPCGVegBiomeCombine::Multiply:
	{
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			OutDensity *= FMath::Lerp(1.0f, R.DensityMultiplier, W);
			OutScale *= FMath::Lerp(1.0f, R.ScaleMultiplier, W);
		}
		break;
	}
	}

	OutDensity = FMath::Max(0.0f, OutDensity);
	OutScale = FMath::Max(0.01f, OutScale);
}

FQuat FPCGTieredVegetationScatterElement::MakeFoliageRotation(
	const FVector& SurfaceNormal, float YawDeg, float AlignAmount,
	float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg)
{
	const FVector WorldUp(0.0, 0.0, 1.0);
	const FVector N = SurfaceNormal.GetSafeNormal();

	FVector DesiredUp = WorldUp;
	if (AlignAmount > 0.0f)
	{
		const float NDot = FMath::Clamp(FVector::DotProduct(WorldUp, N), -1.0f, 1.0f);
		const float FullAngle = FMath::Acos(NDot);
		if (FullAngle > KINDA_SMALL_NUMBER)
		{
			const float MaxRad = FMath::DegreesToRadians(MaxAlignDeg);
			float Alpha = AlignAmount;
			if (FullAngle * Alpha > MaxRad)
			{
				Alpha = MaxRad / FullAngle;
			}
			DesiredUp = FMath::Lerp(WorldUp, N, Alpha).GetSafeNormal();
		}
	}

	const FQuat AlignQuat = FQuat::FindBetweenNormals(WorldUp, DesiredUp);
	const FQuat YawQuat(DesiredUp, FMath::DegreesToRadians(YawDeg));
	const FQuat JitterQuat = FRotator(JitterPitchDeg, 0.0f, JitterRollDeg).Quaternion();

	return YawQuat * AlignQuat * JitterQuat;
}

float FPCGTieredVegetationScatterElement::Perlin01(const FVector& Position, float Frequency, int32 Seed)
{
	// Offset the sample domain by the seed so different tiers/layers decorrelate.
	const FVector SeedOffset(Seed * 13.13f, Seed * 7.77f, Seed * 3.33f);
	const float N = FMath::PerlinNoise3D(Position * Frequency + SeedOffset);
	return FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
}

// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

bool FPCGTieredVegetationScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGTieredVegetationScatterElement::Execute);

	const UPCGTieredVegetationScatterSettings* Settings =
		Context->GetInputSettings<UPCGTieredVegetationScatterSettings>();
	check(Settings);

	// ── World for line tracing ──
	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}
	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGTieredVegetationScatter", "NoWorld",
			"Tiered Vegetation Scatter: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Mesh cache ──
	TArray<FVegMeshCache> MeshCache;
	float MeshTotalWeight = 0.0f;
	if (!BuildMeshCache(Settings->MeshSet, MeshCache, MeshTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGTieredVegetationScatter", "NoMeshes",
			"Tiered Vegetation Scatter: No valid meshes in Mesh Set."));
		return true;
	}

	// ── Validate config ──
	const float MinSlope = FMath::Min(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MaxSlope = FMath::Max(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	const float CompMinDist = FMath::Max(Settings->CompanionMinDistance, 0.0f);
	const float CompMinDistSq = CompMinDist * CompMinDist;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());
	const FVector& WorldUp = PCGExtScatterCommon::WorldUp;

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	// ── Outputs (accumulated across all input data sets) ──
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	const TArray<FPCGTaggedData> CandidateInputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	// XY hash grids replace the old O(n²) AcceptedPositions / CompanionPositions linear
	// scans. CellSize == query radius so the 3×3 neighbour scan is exhaustive. Grids span
	// all input data sets (global pruning), matching the previous accumulate-across-inputs
	// behaviour. A zero radius leaves the grid unused.
	PCGExtScatterCommon::FMinDistGrid AcceptedGrid;
	AcceptedGrid.CellSize = FMath::Max(MinDist, 1.0f);
	PCGExtScatterCommon::FMinDistGrid CompanionGrid;
	CompanionGrid.CellSize = FMath::Max(CompMinDist, 1.0f);

	// Local projection lambda.
	auto Project = [&](const FVector& XY, double RefZ, FVector& OutPos, FVector& OutNormal) -> bool
		{
			const FVector Start(XY.X, XY.Y, RefZ + Settings->TraceStartHeight);
			const FVector End(XY.X, XY.Y, RefZ - Settings->TraceDistance);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Start, End, Settings->TraceChannel, QueryParams))
			{
				OutPos = Hit.ImpactPoint;
				OutNormal = Hit.ImpactNormal;
				return true;
			}
			return false;
		};

	// CPU-side staging for one input data set's worth of generated points. Filled during
	// the per-point pass, then flushed into freshly-allocated UPCGBasePointData below.
	struct FStagedPrimary
	{
		FTransform Transform;
		int32 Seed = 0;
		float Density = 1.0f;
		FSoftObjectPath MeshPath;
		// Source point's metadata entry — the output entry is parented to this so
		// inherited attributes (Biome_*, etc.) survive through the node.
		int64 SourceMetaEntry = PCGInvalidEntryKey;
	};
	struct FStagedSimple
	{
		FTransform Transform;
		int32 Seed = 0;
		// Companions inherit the spawning primary's source entry so biome weights
		// remain readable by downstream scatter nodes.
		int64 SourceMetaEntry = PCGInvalidEntryKey;
	};

	// Fold a per-input-data-set counter into per-point seeds so points sourced from
	// different input data sets don't collide (the source point index resets per input).
	int32 InputDataSetIndex = 0;

	// Validate the configured biome attributes once against the first real input,
	// so misconfiguration (typo'd name, wrong attribute type) shows up as a graph
	// warning instead of silently collapsing the biome response to neutral 1.0.
	bool bValidatedBiomeAttrs = false;

	for (const FPCGTaggedData& Input : CandidateInputs)
	{
		const UPCGBasePointData* InData = Cast<UPCGBasePointData>(Input.Data);
		if (!InData)
		{
			continue;
		}

		const int32 NumInPoints = InData->GetNumPoints();
		if (NumInPoints == 0)
		{
			continue;
		}

		// Per-input-data-set salt for the seed (see staging note above).
		const int32 InputSalt = InputDataSetIndex++;

		const UPCGMetadata* InMeta = InData->ConstMetadata();

		if (!bValidatedBiomeAttrs && InMeta && Settings->BiomeResponses.Num() > 0)
		{
			bValidatedBiomeAttrs = true;
			for (const FPCGVegBiomeResponse& R : Settings->BiomeResponses)
			{
				if (R.BiomeAttribute.IsNone())
				{
					continue;
				}
				if (!InMeta->HasAttribute(R.BiomeAttribute))
				{
					PCGE_LOG(Warning, GraphAndLog, FText::Format(
						NSLOCTEXT("PCGTieredVegetationScatter", "MissingBiomeAttr",
							"Tiered Vegetation Scatter: BiomeResponse attribute \"{0}\" does not "
							"exist on the input points. Its weight reads as 0 everywhere, and via "
							"the WeightedAverage residual the biome response degrades toward "
							"neutral density 1.0 — check the attribute name against the Spline "
							"Biome Mask output."),
						FText::FromName(R.BiomeAttribute)));
					continue;
				}
				const FPCGMetadataAttributeBase* Base = InMeta->GetConstAttribute(R.BiomeAttribute);
				const int16 TypeId = Base ? Base->GetTypeId() : -1;
				const bool bNumeric =
					TypeId == PCG::Private::MetadataTypes<float>::Id ||
					TypeId == PCG::Private::MetadataTypes<double>::Id ||
					TypeId == PCG::Private::MetadataTypes<int32>::Id ||
					TypeId == PCG::Private::MetadataTypes<int64>::Id;
				if (!bNumeric)
				{
					PCGE_LOG(Warning, GraphAndLog, FText::Format(
						NSLOCTEXT("PCGTieredVegetationScatter", "BadBiomeAttrType",
							"Tiered Vegetation Scatter: BiomeResponse attribute \"{0}\" exists but "
							"is not a numeric type (float/double/int32/int64). Its weight reads as "
							"0 everywhere."),
						FText::FromName(R.BiomeAttribute)));
				}
			}
		}

		const TConstPCGValueRange<FTransform> InTransforms = InData->GetConstTransformValueRange();
		const TConstPCGValueRange<int32> InSeeds = InData->GetConstSeedValueRange();
		const TConstPCGValueRange<int64> InMetaEntries = InData->GetConstMetadataEntryValueRange();

		TArray<FStagedPrimary> StagedPrimaries;
		TArray<FStagedSimple> StagedCompanions;
		TArray<int32> RejectedReadIndices;   // source indices kept on the Rejected pin
		TArray<FVector> RejectedPositions;    // projected position for each rejected point

		// Reject helper: stage the source index + its projected position (the original
		// node relocated the rejected copy to the projected surface point).
		auto Reject = [&](int32 ReadIndex, const FVector& ProjPos)
			{
				if (Settings->bOutputRejected)
				{
					RejectedReadIndices.Add(ReadIndex);
					RejectedPositions.Add(ProjPos);
				}
			};

		for (int32 Idx = 0; Idx < NumInPoints; ++Idx)
		{
			const FVector CandPos = InTransforms[Idx].GetLocation();

			// Hash rather than add: InSeeds[Idx] + Idx could overflow int32 (UB) and
			// collide for symmetric seed/index pairs.
			const int32 PointSeed = PCGHelpers::ComputeSeed(
				PCGHelpers::ComputeSeed(BaseSeed, InSeeds[Idx], Idx), InputSalt);
			FRandomStream Rng(PointSeed);

			// 1. Project onto the landscape.
			FVector ProjPos, Normal;
			if (!Project(CandPos, CandPos.Z, ProjPos, Normal))
			{
				continue; // no surface — silently drop (not even a "reject")
			}

			// 2. Slope band.
			const float SlopeDot = FVector::DotProduct(Normal, WorldUp);
			if (SlopeDot < MinSlope || SlopeDot > MaxSlope)
			{
				Reject(Idx, ProjPos);
				continue;
			}

			// 3. Noise cluster gate.
			float Noise = 1.0f;
			float NoiseDensityFactor = 1.0f;
			if (Settings->bUseNoiseMask)
			{
				Noise = Perlin01(ProjPos, Settings->NoiseFrequency, Settings->NoiseSeed);
				if (Settings->bInvertNoise)
				{
					Noise = 1.0f - Noise;
				}
				if (Noise < Settings->NoiseThreshold)
				{
					Reject(Idx, ProjPos);
					continue;
				}

				// Remap the surviving range [Threshold, 1] back to [0, 1] before it
				// feeds the keep probability. Previously the RAW noise value was
				// multiplied into Keep a second time after already gating, which
				// silently scaled effective density inside clusters by ~Threshold–1.0
				// (roughly halving it at the default 0.3 threshold). The remapped
				// factor expresses "position within the cluster" instead: soft edges
				// near the threshold, full density at cluster cores.
				NoiseDensityFactor = (Settings->NoiseThreshold < 1.0f - KINDA_SMALL_NUMBER)
					? (Noise - Settings->NoiseThreshold) / (1.0f - Settings->NoiseThreshold)
					: 1.0f;
			}

			// 4. Biome response.
			float BiomeDensity = 1.0f, BiomeScale = 1.0f;
			ComputeBiomeFactors(Settings->BiomeResponses, Settings->BiomeCombineMode,
				InMeta, InMetaEntries[Idx], BiomeDensity, BiomeScale);

			// 6. Density roll.
			const float Keep = FMath::Clamp(
				Settings->KeepProbability * NoiseDensityFactor * BiomeDensity, 0.0f, 1.0f);
			if (Rng.FRand() > Keep)
			{
				Reject(Idx, ProjPos);
				continue;
			}

			// 7. Min-distance prune.
			if (MinDist > 0.0f && AcceptedGrid.HasWithin(ProjPos, MinDistSq))
			{
				Reject(Idx, ProjPos);
				continue;
			}

			// 8. Build the instance.
			const int32 MeshIdx = SelectMeshIndex(MeshCache, MeshTotalWeight, Rng);
			const FVegMeshCache& Chosen = MeshCache[MeshIdx];

			const float TierScale = Rng.FRandRange(Settings->TierScaleRange.X, Settings->TierScaleRange.Y);
			const float MeshScale = Rng.FRandRange(Chosen.ScaleRange.X, Chosen.ScaleRange.Y);
			float FinalScale = TierScale * MeshScale;
			if (Settings->bBiomeModulatesScale)
			{
				FinalScale *= BiomeScale;
			}

			const float YawDeg = Settings->bRandomYaw ? Rng.FRandRange(0.0f, 360.0f) : 0.0f;
			const float JitterP = Rng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const float JitterR = Rng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const FQuat Rot = MakeFoliageRotation(Normal, YawDeg,
				Settings->SlopeAlignAmount, Settings->MaxAlignAngleDeg, JitterP, JitterR);

			const FVector FinalPos = ProjPos + FVector(0.0, 0.0, Settings->ZOffset);

			FStagedPrimary Primary;
			Primary.Transform = FTransform(Rot, FinalPos, FVector(FinalScale));
			Primary.Seed = PointSeed;
			// Writing the keep probability as Density can cause a SECOND round of
			// stochastic thinning in downstream nodes that sample density; only do
			// so when explicitly requested.
			Primary.Density = Settings->bWriteKeepProbabilityAsDensity ? Keep : 1.0f;
			Primary.MeshPath = Chosen.MeshPath;
			Primary.SourceMetaEntry = InMetaEntries[Idx];
			StagedPrimaries.Add(Primary);

			AcceptedGrid.Add(ProjPos);

			// 9. Companions.
			// TODO(audit): companion rings use a uniform radius distribution (Radius drawn
			// linearly in [RMin,RMax]) which clumps toward the centre, and they only avoid
			// other companions -- they don't avoid primary instances. Owner may want area-
			// uniform radial sampling and primary-avoidance. Left unchanged for now.
			if (Settings->bGenerateCompanions)
			{
				const int32 NComp = Rng.RandRange(
					FMath::Min(Settings->CompanionsPerPrimaryMin, Settings->CompanionsPerPrimaryMax),
					FMath::Max(Settings->CompanionsPerPrimaryMin, Settings->CompanionsPerPrimaryMax));

				const float RMin = FMath::Min(Settings->CompanionRadiusMin, Settings->CompanionRadiusMax);
				const float RMax = FMath::Max(Settings->CompanionRadiusMin, Settings->CompanionRadiusMax);

				for (int32 c = 0; c < NComp; ++c)
				{
					const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);
					const float Radius = Rng.FRandRange(RMin, RMax);
					const FVector CompXY(
						ProjPos.X + FMath::Cos(Angle) * Radius,
						ProjPos.Y + FMath::Sin(Angle) * Radius,
						ProjPos.Z);

					FVector CompPos, CompNormal;
					if (!Project(CompXY, ProjPos.Z, CompPos, CompNormal))
					{
						continue;
					}

					const float CompSlope = FVector::DotProduct(CompNormal, WorldUp);
					if (CompSlope < MinSlope || CompSlope > MaxSlope)
					{
						continue;
					}

					if (CompMinDist > 0.0f && CompanionGrid.HasWithin(CompPos, CompMinDistSq))
					{
						continue;
					}

					const int32 CompSeed = PCGHelpers::ComputeSeed(PointSeed, c + 1);
					FRandomStream CompRng(CompSeed);
					const float CompScale = CompRng.FRandRange(
						Settings->CompanionScaleRange.X, Settings->CompanionScaleRange.Y);
					const float CYaw = Settings->bRandomYaw ? CompRng.FRandRange(0.0f, 360.0f) : 0.0f;
					const FQuat CRot = MakeFoliageRotation(CompNormal, CYaw,
						Settings->SlopeAlignAmount, Settings->MaxAlignAngleDeg, 0.0f, 0.0f);

					FStagedSimple Comp;
					Comp.Transform = FTransform(CRot,
						CompPos + FVector(0.0, 0.0, Settings->CompanionZOffset),
						FVector(CompScale));
					Comp.Seed = CompSeed;
					Comp.SourceMetaEntry = InMetaEntries[Idx];
					StagedCompanions.Add(Comp);

					CompanionGrid.Add(CompPos);
				}
			}
		}

		// ── Flush this input data set's staged points into output point data ──

		// Default output: generated primaries (transform/seed/density + MeshPath attr).
		if (StagedPrimaries.Num() > 0)
		{
			UPCGBasePointData* OutData = FPCGContext::NewPointData_AnyThread(Context);
			FPCGInitializeFromDataParams InitParams(InData);
			OutData->InitializeFromDataWithParams(InitParams);
			OutData->SetNumPoints(StagedPrimaries.Num(), /*bInitializeValues=*/false);
			OutData->AllocateProperties(
				EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::Seed |
				EPCGPointNativeProperties::Density | EPCGPointNativeProperties::Steepness |
				EPCGPointNativeProperties::BoundsMin | EPCGPointNativeProperties::BoundsMax |
				EPCGPointNativeProperties::MetadataEntry);
			OutData->SetSteepness(1.0f);
			OutData->SetExtents(FVector(50.0f));

			UPCGMetadata* OutMeta = OutData->MutableMetadata();
			OutMeta->CreateSoftObjectPathAttribute(
				Settings->MeshPathAttributeName, FSoftObjectPath(), /*bAllowsInterpolation=*/false);
			FPCGMetadataAttribute<FSoftObjectPath>* MeshPathAttr =
				OutMeta->GetMutableTypedAttribute<FSoftObjectPath>(Settings->MeshPathAttributeName);

			TPCGValueRange<FTransform> Transforms = OutData->GetTransformValueRange();
			TPCGValueRange<int32> Seeds = OutData->GetSeedValueRange();
			TPCGValueRange<float> Densities = OutData->GetDensityValueRange();
			TPCGValueRange<int64> MetaEntries = OutData->GetMetadataEntryValueRange();

			for (int32 i = 0; i < StagedPrimaries.Num(); ++i)
			{
				const FStagedPrimary& P = StagedPrimaries[i];
				Transforms[i] = P.Transform;
				Seeds[i] = P.Seed;
				Densities[i] = P.Density;

				// Parent the new entry to the source point's entry so inherited
				// attributes (Biome_*, etc.) survive through the node.
				//
				// CRASH FIX: do NOT seed InitializeOnSet with the source key. Its
				// internal heuristic only re-parents keys that fall in the PARENT
				// key range; if the engine didn't parent OutMeta to InMeta (this
				// varies with FPCGInitializeFromDataParams behaviour across
				// versions), the seeded key lands in the local range, is left
				// untouched, and becomes a DANGLING entry key — the attribute
				// inspector crashes dereferencing it. Verify the parent
				// relationship explicitly and only then create a parented entry.
				if (P.SourceMetaEntry != PCGInvalidEntryKey && OutMeta->GetParent() == InMeta)
				{
					MetaEntries[i] = OutMeta->AddEntry(P.SourceMetaEntry);
				}
				else
				{
					MetaEntries[i] = OutMeta->AddEntry();
				}
				if (MeshPathAttr)
				{
					MeshPathAttr->SetValue(MetaEntries[i], P.MeshPath);
				}
			}

			FPCGTaggedData& T = Outputs.Emplace_GetRef();
			T.Data = OutData;
			T.Pin = PCGPinConstants::DefaultOutputLabel;
		}

		// Companions output: transform-ready, mesh-less understory points.
		if (Settings->bGenerateCompanions && StagedCompanions.Num() > 0)
		{
			UPCGBasePointData* CompData = FPCGContext::NewPointData_AnyThread(Context);
			FPCGInitializeFromDataParams InitParams(InData);
			CompData->InitializeFromDataWithParams(InitParams);
			CompData->SetNumPoints(StagedCompanions.Num(), /*bInitializeValues=*/false);
			CompData->AllocateProperties(
				EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::Seed |
				EPCGPointNativeProperties::Density | EPCGPointNativeProperties::Steepness |
				EPCGPointNativeProperties::BoundsMin | EPCGPointNativeProperties::BoundsMax |
				EPCGPointNativeProperties::MetadataEntry);
			CompData->SetDensity(1.0f);
			CompData->SetSteepness(1.0f);
			CompData->SetExtents(FVector(30.0f));

			UPCGMetadata* CompMeta = CompData->MutableMetadata();

			TPCGValueRange<FTransform> Transforms = CompData->GetTransformValueRange();
			TPCGValueRange<int32> Seeds = CompData->GetSeedValueRange();
			TPCGValueRange<int64> CompMetaEntries = CompData->GetMetadataEntryValueRange();

			for (int32 i = 0; i < StagedCompanions.Num(); ++i)
			{
				Transforms[i] = StagedCompanions[i].Transform;
				Seeds[i] = StagedCompanions[i].Seed;

				// Companions inherit the spawning primary's source entry so biome
				// weights remain readable downstream. Same explicit parent check as
				// the primary flush — never seed InitializeOnSet with a foreign key
				// (dangling-key crash risk if the metadata isn't parented).
				if (StagedCompanions[i].SourceMetaEntry != PCGInvalidEntryKey &&
					CompMeta->GetParent() == InMeta)
				{
					CompMetaEntries[i] = CompMeta->AddEntry(StagedCompanions[i].SourceMetaEntry);
				}
				else
				{
					CompMetaEntries[i] = CompMeta->AddEntry();
				}
			}

			FPCGTaggedData& T = Outputs.Emplace_GetRef();
			T.Data = CompData;
			T.Pin = PCGExtScatterCommon::CompanionsPinLabel;
		}

		// Rejected output: culled candidates copied 1:1 from the source then relocated to
		// their projected surface position (debug only).
		if (Settings->bOutputRejected && RejectedReadIndices.Num() > 0)
		{
			// These are parallel arrays filled together in Reject(); a desync here
			// (e.g. a future reject path pushing only one of them) would silently
			// relocate the wrong points below.
			ensureMsgf(RejectedReadIndices.Num() == RejectedPositions.Num(),
				TEXT("TieredVegetationScatter: rejected index/position arrays out of sync (%d vs %d)."),
				RejectedReadIndices.Num(), RejectedPositions.Num());

			UPCGBasePointData* RejData = FPCGContext::NewPointData_AnyThread(Context);
			FPCGInitializeFromDataParams InitParams(InData);
			RejData->InitializeFromDataWithParams(InitParams);
			RejData->SetNumPoints(RejectedReadIndices.Num(), /*bInitializeValues=*/false);
			RejData->AllocateProperties(InData->GetAllocatedProperties());

			TArray<int32> WriteIndices;
			WriteIndices.SetNumUninitialized(RejectedReadIndices.Num());
			for (int32 i = 0; i < WriteIndices.Num(); ++i)
			{
				WriteIndices[i] = i;
			}
			InData->CopyPropertiesTo(RejData, RejectedReadIndices, WriteIndices,
				InData->GetAllocatedProperties());

			// Relocate to the projected surface point (matching the original behaviour).
			TPCGValueRange<FTransform> Transforms = RejData->GetTransformValueRange();
			for (int32 i = 0; i < RejectedPositions.Num(); ++i)
			{
				Transforms[i].SetLocation(RejectedPositions[i]);
			}

			FPCGTaggedData& T = Outputs.Emplace_GetRef();
			T.Data = RejData;
			T.Pin = PCGExtScatterCommon::RejectedPinLabel;
		}
	}

	return true;
}