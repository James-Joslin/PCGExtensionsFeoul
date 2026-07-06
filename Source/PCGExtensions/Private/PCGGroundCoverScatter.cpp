// PCGGroundCoverScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGGroundCoverScatter.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "CollisionQueryParams.h"

#include "PCGExtScatterCommon.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGGroundCoverScatter)

// File-local noise helpers. A named namespace (not anonymous / not `static`) so the
// Unity build can't collide these with same-named helpers in sibling scatter .cpp files.
namespace PCGGroundCoverScatter
{
	// ── Deterministic per-cell hash → two floats in [0,1). Used by Worley + Random. ──
	FORCEINLINE FVector2D CellRandom2(int32 CX, int32 CY, int32 Seed)
	{
		uint32 H = uint32(CX) * 374761393u + uint32(CY) * 668265263u + uint32(Seed) * 2246822519u;
		H = (H ^ (H >> 13)) * 1274126177u;
		const uint32 HX = H ^ (H >> 16);
		H = (H * 2654435761u) ^ (H >> 15);
		const uint32 HY = H ^ (H >> 16);
		return FVector2D(float(HX & 0xFFFFFFu) / 16777216.0f, float(HY & 0xFFFFFFu) / 16777216.0f);
	}

	// ── 2D Worley: F1 = nearest feature distance, F2 = second nearest (in cell units). ──
	void Worley2D(const FVector2D& P, int32 Seed, float& OutF1, float& OutF2)
	{
		const int32 CX = FMath::FloorToInt(P.X);
		const int32 CY = FMath::FloorToInt(P.Y);
		float F1 = BIG_NUMBER;
		float F2 = BIG_NUMBER;

		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				const int32 NX = CX + dx;
				const int32 NY = CY + dy;
				const FVector2D Feature = FVector2D(NX, NY) + CellRandom2(NX, NY, Seed);
				const float D = FVector2D::Distance(P, Feature);
				if (D < F1) { F2 = F1; F1 = D; }
				else if (D < F2) { F2 = D; }
			}
		}
		OutF1 = F1;
		OutF2 = F2;
	}

	// ── Per-position hash random in [0,1] (quantised to ~1 UU). ──
	FORCEINLINE float PosRandom01(const FVector& Pos, int32 Seed)
	{
		const FVector2D R = CellRandom2(FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y), Seed);
		return R.X;
	}
}

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UPCGGroundCoverScatterSettings::UPCGGroundCoverScatterSettings()
{
	// Default noise stack: large Perlin patches × medium Worley clumps + fine Perlin + micro jitter.
	NoiseLayers.Reset();

	FPCGGroundNoiseLayer L0;
	L0.NoiseType = EPCGGroundNoiseType::Perlin;
	L0.Frequency = 0.004f; L0.Amplitude = 1.0f; L0.BlendMode = EPCGGroundNoiseBlend::Multiply; L0.Seed = 0;
	NoiseLayers.Add(L0);

	FPCGGroundNoiseLayer L1;
	L1.NoiseType = EPCGGroundNoiseType::Worley_F1;
	L1.Frequency = 0.03f; L1.Amplitude = 1.0f; L1.bInvert = true; // invert → dense at cell cores
	L1.BlendMode = EPCGGroundNoiseBlend::Multiply; L1.Seed = 1;
	NoiseLayers.Add(L1);

	FPCGGroundNoiseLayer L2;
	L2.NoiseType = EPCGGroundNoiseType::Perlin;
	L2.Frequency = 0.1f; L2.Amplitude = 0.3f; L2.BlendMode = EPCGGroundNoiseBlend::Add; L2.Seed = 2;
	NoiseLayers.Add(L2);

	FPCGGroundNoiseLayer L3;
	L3.NoiseType = EPCGGroundNoiseType::Random;
	L3.Frequency = 0.0f; L3.Amplitude = 0.1f; L3.BlendMode = EPCGGroundNoiseBlend::Add; L3.Seed = 3;
	NoiseLayers.Add(L3);
}

TArray<FPCGPinProperties> UPCGGroundCoverScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "InTooltip",
			"Dense candidate points (Surface Sampler / jittered grid). Biome weight attributes "
			"(e.g. Biome_Meadow) are read from these points if present."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGGroundCoverScatterSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "OutTooltip",
			"Placed ground-cover instances with a MeshPath attribute. Feed a By-Attribute "
			"Static Mesh Spawner."));

	Pins.Emplace(PCGExtScatterCommon::RejectedPinLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "RejTooltip",
			"Culled candidates (slope / noise / spacing). Debug only."));

	return Pins;
}

FPCGElementPtr UPCGGroundCoverScatterSettings::CreateElement() const
{
	return MakeShared<FPCGGroundCoverScatterElement>();
}

// ─────────────────────────────────────────────
//  Helpers — mesh
// ─────────────────────────────────────────────

bool FPCGGroundCoverScatterElement::BuildMeshCache(
	const TArray<FPCGGroundMeshEntry>& Entries,
	TArray<FGroundMeshCache>& OutCache, float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGGroundMeshEntry& Entry : Entries)
	{
		if (Entry.Mesh.IsNull())
		{
			continue;
		}

		FGroundMeshCache CacheEntry;
		CacheEntry.MeshPath = Entry.Mesh.ToSoftObjectPath();
		CacheEntry.ScaleRange = Entry.ScaleRange;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		CacheEntry.CumulativeWeight = OutTotalWeight;
		OutCache.Add(CacheEntry);
	}

	return OutCache.Num() > 0;
}

int32 FPCGGroundCoverScatterElement::SelectMeshIndex(
	const TArray<FGroundMeshCache>& Cache, float TotalWeight, FRandomStream& Rng)
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

// ─────────────────────────────────────────────
//  Helpers — attributes
// ─────────────────────────────────────────────

float FPCGGroundCoverScatterElement::ReadFloatAttr(
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

	// BUGFIX: previously required EXACTLY float, silently returning Default (0)
	// for double/int attributes — PCG's Create Constant / Attribute Maths nodes
	// produce DOUBLE by default, so graph-authored weights read as 0 everywhere
	// and the biome response collapsed to neutral. Accept common numeric types.
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

// ─────────────────────────────────────────────
//  Helpers — biome
// ─────────────────────────────────────────────

void FPCGGroundCoverScatterElement::ComputeBiomeFactors(
	const TArray<FPCGGroundBiomeResponse>& Responses, EPCGGroundBiomeCombine Mode,
	const UPCGMetadata* Meta, int64 EntryKey, float& OutDensity, float& OutScale)
{
	OutDensity = 1.0f;
	OutScale = 1.0f;

	if (Responses.Num() == 0 || !Meta)
	{
		return;
	}

	switch (Mode)
	{
	case EPCGGroundBiomeCombine::WeightedAverage:
	{
		float SumW = 0.0f, SumWD = 0.0f, SumWS = 0.0f;
		for (const FPCGGroundBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			SumW += W;
			SumWD += W * R.DensityMultiplier;
			SumWS += W * R.ScaleMultiplier;
		}
		const float Residual = FMath::Max(0.0f, 1.0f - SumW);
		const float Denom = SumW + Residual;
		if (Denom > KINDA_SMALL_NUMBER)
		{
			OutDensity = (SumWD + Residual) / Denom;
			OutScale = (SumWS + Residual) / Denom;
		}
		break;
	}
	case EPCGGroundBiomeCombine::Max:
	{
		float BestW = 0.0f;
		for (const FPCGGroundBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			if (W > BestW)
			{
				BestW = W;
				OutDensity = R.DensityMultiplier;
				OutScale = R.ScaleMultiplier;
			}
		}
		OutDensity = FMath::Lerp(1.0f, OutDensity, BestW);
		OutScale = FMath::Lerp(1.0f, OutScale, BestW);
		break;
	}
	case EPCGGroundBiomeCombine::Multiply:
	{
		for (const FPCGGroundBiomeResponse& R : Responses)
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

// ─────────────────────────────────────────────
//  Helpers — transform
// ─────────────────────────────────────────────

FQuat FPCGGroundCoverScatterElement::MakeFoliageRotation(
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

// ─────────────────────────────────────────────
//  Helpers — noise stack
// ─────────────────────────────────────────────

float FPCGGroundCoverScatterElement::SampleNoiseStack(
	const TArray<FPCGGroundNoiseLayer>& Layers, const FVector& Position, bool bClamp)
{
	float Result = 1.0f;

	for (const FPCGGroundNoiseLayer& L : Layers)
	{
		float V = 0.0f;
		switch (L.NoiseType)
		{
		case EPCGGroundNoiseType::Perlin:
		{
			const FVector SeedOffset(L.Seed * 13.13f, L.Seed * 7.77f, L.Seed * 3.33f);
			const float N = FMath::PerlinNoise3D(Position * L.Frequency + SeedOffset);
			V = FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
			break;
		}
		case EPCGGroundNoiseType::Worley_F1:
		{
			// TODO(audit): Worley returns raw cell-distance (in cell units), not normalised
			// to [0,1]. F1 routinely exceeds 1 except near a feature centre, so the Clamp
			// floors most of the field at 1.0; with the default inverted-Multiply layer this
			// culls heavily/unevenly. Math left unchanged.
			float F1, F2;
			PCGGroundCoverScatter::Worley2D(FVector2D(Position.X, Position.Y) * L.Frequency, L.Seed, F1, F2);
			V = FMath::Clamp(F1, 0.0f, 1.0f); // 0 at feature centre → 1 at cell edge
			break;
		}
		case EPCGGroundNoiseType::Worley_F2MinusF1:
		{
			float F1, F2;
			PCGGroundCoverScatter::Worley2D(FVector2D(Position.X, Position.Y) * L.Frequency, L.Seed, F1, F2);
			V = FMath::Clamp(F2 - F1, 0.0f, 1.0f); // ridge lines between cells
			break;
		}
		case EPCGGroundNoiseType::Random:
		{
			V = PCGGroundCoverScatter::PosRandom01(Position, L.Seed);
			break;
		}
		}

		if (L.bInvert)
		{
			V = 1.0f - V;
		}
		V = V * L.Amplitude + L.Bias;

		switch (L.BlendMode)
		{
		case EPCGGroundNoiseBlend::Multiply: Result *= V; break;
		case EPCGGroundNoiseBlend::Add:      Result += V; break;
		case EPCGGroundNoiseBlend::Min:      Result = FMath::Min(Result, V); break;
		case EPCGGroundNoiseBlend::Max:      Result = FMath::Max(Result, V); break;
		case EPCGGroundNoiseBlend::Replace:  Result = V; break;
		}
	}

	return bClamp ? FMath::Clamp(Result, 0.0f, 1.0f) : Result;
}

// ─────────────────────────────────────────────
//  Element — execution
// ─────────────────────────────────────────────

bool FPCGGroundCoverScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGGroundCoverScatterElement::Execute);

	const UPCGGroundCoverScatterSettings* Settings =
		Context->GetInputSettings<UPCGGroundCoverScatterSettings>();
	check(Settings);

	// ── World for line tracing ──
	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}
	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGGroundCoverScatter", "NoWorld",
			"Ground Cover Scatter: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Mesh cache ──
	TArray<FGroundMeshCache> MeshCache;
	float MeshTotalWeight = 0.0f;
	if (!BuildMeshCache(Settings->MeshSet, MeshCache, MeshTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGGroundCoverScatter", "NoMeshes",
			"Ground Cover Scatter: No valid meshes in Mesh Set."));
		return true;
	}
	// ── Validate config ──
	const float MinSlope = FMath::Min(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MaxSlope = FMath::Max(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	// Min-distance acceleration grid (cell size must be >= MinDistance for the 3x3 lookup).
	PCGExtScatterCommon::FMinDistGrid SpacingGrid;
	SpacingGrid.CellSize = FMath::Max(MinDist, 1.0f);

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

	// ── Outputs ──
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;
	const TArray<FPCGTaggedData> CandidateInputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	// Validate the configured biome attributes once against the first real input,
	// so misconfiguration (typo'd name, wrong attribute type) shows up as a graph
	// warning instead of silently collapsing the biome response to neutral 1.0.
	bool bValidatedBiomeAttrs = false;

	// One output (and optional Rejected) per input data set -- initialised from THAT input's
	// schema/metadata. Keying a single shared output to the first input drops the other inputs'
	// attributes; per-input outputs preserve each input's biome/metadata parentage.
	for (const FPCGTaggedData& Input : CandidateInputs)
	{
		const UPCGBasePointData* InData = Cast<UPCGBasePointData>(Input.Data);
		if (!InData)
		{
			continue;
		}
		const int32 NumCandidates = InData->GetNumPoints();
		if (NumCandidates == 0)
		{
			continue;
		}
		const UPCGMetadata* InMeta = InData->ConstMetadata();

		if (!bValidatedBiomeAttrs && InMeta && Settings->BiomeResponses.Num() > 0)
		{
			bValidatedBiomeAttrs = true;
			for (const FPCGGroundBiomeResponse& R : Settings->BiomeResponses)
			{
				if (R.BiomeAttribute.IsNone())
				{
					continue;
				}
				if (!InMeta->HasAttribute(R.BiomeAttribute))
				{
					PCGE_LOG(Warning, GraphAndLog, FText::Format(
						NSLOCTEXT("PCGGroundCoverScatter", "MissingBiomeAttr",
							"Ground Cover Scatter: BiomeResponse attribute \"{0}\" does not exist "
							"on the input points. Its weight reads as 0 everywhere and the biome "
							"response degrades toward neutral — check the attribute name against "
							"the Spline Biome Mask output."),
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
						NSLOCTEXT("PCGGroundCoverScatter", "BadBiomeAttrType",
							"Ground Cover Scatter: BiomeResponse attribute \"{0}\" exists but is "
							"not a numeric type (float/double/int32/int64). Its weight reads as 0 "
							"everywhere."),
						FText::FromName(R.BiomeAttribute)));
				}
			}
		}

		const TConstPCGValueRange<FTransform> InTransforms = InData->GetConstTransformValueRange();
		const TConstPCGValueRange<int32> InSeeds = InData->GetConstSeedValueRange();
		const TConstPCGValueRange<float> InDensities = InData->GetConstDensityValueRange();
		const TConstPCGValueRange<int64> InMetadataEntries = InData->GetConstMetadataEntryValueRange();

		// One generated instance per accepted candidate. Buffered because the kept count is
		// only known after the filter pass (line trace / noise / spacing reject variably).
		struct FInstance
		{
			FTransform Transform;
			float Density;
			FSoftObjectPath MeshPath;
			// Source point's metadata entry — the output entry is parented to this so
			// inherited attributes (Biome_*, etc.) survive through the node.
			int64 SourceMetaEntry = PCGInvalidEntryKey;
		};
		TArray<FInstance> Instances;
		Instances.Reserve(NumCandidates);

		// Scattered candidate indices that were culled (for the optional Rejected output).
		TArray<int32> RejectedIndices;
		if (Settings->bOutputRejected)
		{
			RejectedIndices.Reserve(NumCandidates);
		}

		for (int32 Idx = 0; Idx < NumCandidates; ++Idx)
		{
			const FTransform& CandTransform = InTransforms[Idx];
			const FVector CandPos = CandTransform.GetLocation();

			// Hash rather than add: InSeeds[Idx] + Idx could overflow int32 (UB) and
			// collide for symmetric seed/index pairs.
			const int32 PointSeed = PCGHelpers::ComputeSeed(BaseSeed, InSeeds[Idx], Idx);
			FRandomStream Rng(PointSeed);

			auto Reject = [&]()
				{
					if (Settings->bOutputRejected)
					{
						RejectedIndices.Add(Idx);
					}
				};

			// 1. Optional XY jitter (break grid regularity), then project.
			FVector SampleXY = CandPos;
			if (Settings->PositionJitter > 0.0f)
			{
				const float Ang = Rng.FRandRange(0.0f, 2.0f * PI);
				const float Rad = Rng.FRandRange(0.0f, Settings->PositionJitter);
				SampleXY.X += FMath::Cos(Ang) * Rad;
				SampleXY.Y += FMath::Sin(Ang) * Rad;
			}

			FVector ProjPos, Normal;
			if (!Project(SampleXY, CandPos.Z, ProjPos, Normal))
			{
				continue; // no surface — drop silently
			}

			// 2. Slope band.
			const float SlopeDot = FVector::DotProduct(Normal, PCGExtScatterCommon::WorldUp);
			if (SlopeDot < MinSlope || SlopeDot > MaxSlope)
			{
				Reject();
				continue;
			}

			// 3. Noise stack → density.
			float NoiseDensity = 1.0f;
			if (Settings->bUseNoiseStack && Settings->NoiseLayers.Num() > 0)
			{
				NoiseDensity = SampleNoiseStack(Settings->NoiseLayers, ProjPos, Settings->bClampNoise);
				if (NoiseDensity < Settings->NoiseCullThreshold)
				{
					Reject();
					continue;
				}
			}

			// 4. Biome response.
			float BiomeDensity = 1.0f, BiomeScale = 1.0f;
			ComputeBiomeFactors(Settings->BiomeResponses, Settings->BiomeCombineMode,
				InMeta, InMetadataEntries[Idx], BiomeDensity, BiomeScale);


			const float Keep = FMath::Clamp(
				Settings->KeepProbability * NoiseDensity * BiomeDensity, 0.0f, 1.0f);

			if (Rng.FRand() > Keep)
			{
				Reject();
				continue;
			}

			// 7. Min-distance prune (spatial-hash accelerated).
			if (MinDist > 0.0f)
			{
				if (SpacingGrid.HasWithin(ProjPos, MinDistSq))
				{
					Reject();
					continue;
				}
			}

			// 8. Build the instance.
			const int32 MeshIdx = SelectMeshIndex(MeshCache, MeshTotalWeight, Rng);
			const FGroundMeshCache& Chosen = MeshCache[MeshIdx];

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

			FInstance& Inst = Instances.Emplace_GetRef();
			Inst.Transform = FTransform(Rot, FinalPos, FVector(FinalScale));
			Inst.Density = Settings->bWriteDensity ? Keep : InDensities[Idx];
			Inst.MeshPath = Chosen.MeshPath;
			Inst.SourceMetaEntry = InMetadataEntries[Idx];

			if (MinDist > 0.0f)
			{
				SpacingGrid.Add(ProjPos);
			}
		}

		// ── Default output: generate the accepted instances ──
		if (Instances.Num() > 0)
		{
			UPCGBasePointData* OutData = FPCGContext::NewPointData_AnyThread(Context);
			FPCGInitializeFromDataParams InitParams(InData);
			OutData->InitializeFromDataWithParams(InitParams);
			OutData->SetNumPoints(Instances.Num(), /*bInitializeValues=*/false);
			OutData->AllocateProperties(
				EPCGPointNativeProperties::Transform | EPCGPointNativeProperties::Density |
				EPCGPointNativeProperties::BoundsMin | EPCGPointNativeProperties::BoundsMax |
				EPCGPointNativeProperties::Steepness | EPCGPointNativeProperties::Seed |
				EPCGPointNativeProperties::MetadataEntry);

			// SoftObjectPath attribute so a By-Attribute Static Mesh Spawner consumes it natively.
			UPCGMetadata* OutMeta = OutData->MutableMetadata();
			OutMeta->CreateSoftObjectPathAttribute(
				Settings->MeshPathAttributeName, FSoftObjectPath(), /*bAllowsInterpolation=*/false);
			FPCGMetadataAttribute<FSoftObjectPath>* MeshPathAttr =
				OutMeta->GetMutableTypedAttribute<FSoftObjectPath>(Settings->MeshPathAttributeName);

			TPCGValueRange<FTransform> OutTransforms = OutData->GetTransformValueRange();
			TPCGValueRange<float> OutDensities = OutData->GetDensityValueRange();
			TPCGValueRange<FVector> OutBoundsMin = OutData->GetBoundsMinValueRange();
			TPCGValueRange<FVector> OutBoundsMax = OutData->GetBoundsMaxValueRange();
			TPCGValueRange<float> OutSteepness = OutData->GetSteepnessValueRange();
			TPCGValueRange<int32> OutSeeds = OutData->GetSeedValueRange();
			TPCGValueRange<int64> OutMetadataEntries = OutData->GetMetadataEntryValueRange();

			for (int32 i = 0; i < Instances.Num(); ++i)
			{
				const FInstance& Inst = Instances[i];
				OutTransforms[i] = Inst.Transform;
				OutDensities[i] = Inst.Density;
				OutBoundsMin[i] = FVector(-8.0);
				OutBoundsMax[i] = FVector(8.0);
				OutSteepness[i] = 1.0f;
				OutSeeds[i] = PCGHelpers::ComputeSeedFromPosition(Inst.Transform.GetLocation());

				// Parent the new entry to the source point's entry so inherited
				// attributes (Biome_*, etc.) survive through the node.
				//
				// CRASH FIX: never seed InitializeOnSet with a foreign key — its
				// key-range heuristic leaves the key untouched (dangling) if the
				// output metadata isn't actually parented to the input's. Verify
				// the parent relationship and create the entry explicitly.
				if (Inst.SourceMetaEntry != PCGInvalidEntryKey && OutMeta->GetParent() == InMeta)
				{
					OutMetadataEntries[i] = OutMeta->AddEntry(Inst.SourceMetaEntry);
				}
				else
				{
					OutMetadataEntries[i] = OutMeta->AddEntry();
				}
				if (MeshPathAttr)
				{
					MeshPathAttr->SetValue(OutMetadataEntries[i], Inst.MeshPath);
				}
			}

			FPCGTaggedData& T = Outputs.Emplace_GetRef(Input);
			T.Data = OutData;
			T.Pin = PCGPinConstants::DefaultOutputLabel;
		}

		// ── Rejected output: copy culled candidates verbatim (preserves their metadata) ──
		if (Settings->bOutputRejected && RejectedIndices.Num() > 0)
		{
			UPCGBasePointData* RejData = FPCGContext::NewPointData_AnyThread(Context);
			FPCGInitializeFromDataParams RejInitParams(InData);
			RejData->InitializeFromDataWithParams(RejInitParams);
			RejData->SetNumPoints(RejectedIndices.Num(), /*bInitializeValues=*/false);

			TArray<int32> WriteIndices;
			WriteIndices.SetNumUninitialized(RejectedIndices.Num());
			for (int32 i = 0; i < RejectedIndices.Num(); ++i)
			{
				WriteIndices[i] = i;
			}
			InData->CopyPointsTo(RejData, RejectedIndices, WriteIndices);

			FPCGTaggedData& T = Outputs.Emplace_GetRef(Input);
			T.Data = RejData;
			T.Pin = PCGExtScatterCommon::RejectedPinLabel;
		}
	}

	return true;
}