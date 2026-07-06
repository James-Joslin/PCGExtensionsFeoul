// PCGTieredVegetationScatter.h
// Custom PCG node that places one vegetation tier (large trees, medium trees,
// shrubs, ...) from an input candidate point cloud. Does the heavy per-point
// lifting: landscape projection, slope-band filtering, per-biome density/scale
// response read from weight attributes, single-layer noise thinning,
// min-distance pruning, weighted mesh assignment, and optional companion
// (understory) output. Per-point metadata (e.g. biome weight attributes) is
// preserved on the Out and Companions pins for downstream nodes.
//
// Designed to be instanced once per tier and configured entirely in the Details
// panel. Candidate generation and valley/volume subtraction are done upstream
// with built-in nodes (Surface Sampler / jittered grid / Poisson + your volume
// subtraction), then fed into the In pin.
//
// Replaces the chain: Surface Sampler → Normal To Density → Density Filter →
// Density Noise → Self Pruning → Transform Points.
//
// Place in your project's Source/<Module>/Public/ directory.
// Requires "PCG" in Build.cs PublicDependencyModuleNames.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "PCGContext.h"
#include "Engine/StaticMesh.h"

#include "PCGTieredVegetationScatter.generated.h"

// ─────────────────────────────────────────────
//  Biome combine mode
// ─────────────────────────────────────────────

UENUM(BlueprintType)
enum class EPCGVegBiomeCombine : uint8
{
	/** Σ(weight·mult) / Σweight. Smooth blends. Pairs with normalised Spline Biome Mask weights. */
	WeightedAverage  UMETA(DisplayName = "Weighted Average"),
	/** Highest weighted multiplier wins. Sharper dominant-biome behaviour. */
	Max              UMETA(DisplayName = "Max"),
	/** Product of lerp(1, mult, weight) across biomes. Stacks reductions. */
	Multiply         UMETA(DisplayName = "Multiply")
};

// ─────────────────────────────────────────────
//  Mesh entry (weighted, with per-mesh scale range)
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGVegMeshEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Relative probability of this mesh being selected within the set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh", meta = (ClampMin = "0.01"))
	float Weight = 1.0f;

	/** Per-mesh uniform scale range (multiplied by the tier scale and biome scale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	FVector2D ScaleRange = FVector2D(0.9f, 1.1f);
};

// ─────────────────────────────────────────────
//  Per-biome response
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGVegBiomeResponse
{
	GENERATED_BODY()

	/**
	 * Float weight attribute (0–1) written by the Spline Biome Mask
	 * (e.g. "Biome_Meadow", "Biome_Forest"). If absent on the input, this
	 * entry contributes nothing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	FName BiomeAttribute = FName(TEXT("Biome_Meadow"));

	/** Density scaling where this biome dominates. Large trees: meadow 0.15, forest 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float DensityMultiplier = 1.0f;

	/** Optional mesh-scale bias where this biome dominates (e.g. stunted meadow trees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float ScaleMultiplier = 1.0f;
};

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

// NOTE: mesh-bounds obstacle exclusion was previously advertised but never
// implemented; all references to it have been removed from the documentation.
// If the feature is added later, reinstate an "Exclusion Sources" input pin.
UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGTieredVegetationScatterSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("TieredVegetationScatter")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGTieredVegetationScatter", "NodeTitle", "Tiered Vegetation Scatter");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGTieredVegetationScatter", "Tooltip",
			"Places one vegetation tier from input candidate points. Projects onto the "
			"landscape, filters by slope band, modulates density/scale per biome (read from "
			"weight attributes), applies a noise cluster mask, prunes by minimum distance, "
			"assigns a weighted mesh (stored as a MeshPath attribute), and optionally emits "
			"companion/understory points. Per-point metadata (biome weights etc.) is "
			"preserved on the outputs. Instance once per tier and configure in the Details "
			"panel.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Distribution ──────────────────────────────────────────────

	/**
	 * Baseline probability a candidate survives, before noise/biome/exclusion thinning.
	 * 1.0 = keep all that pass the filters. Lower to globally thin a dense candidate set.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float KeepProbability = 1.0f;

	/**
	 * Minimum spacing between accepted instances (UU). Poisson-like decimation of the
	 * candidate set. Trees 400–600, shrubs 100–200. 0 = no pruning (rely on upstream spacing).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution",
		meta = (ClampMin = "0.0"))
	float MinDistance = 500.0f;

	// ── Noise Cluster Mask (single Perlin layer) ──────────────────

	/** Enable the built-in Perlin cluster mask. Disable to rely on biome/upstream density only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	bool bUseNoiseMask = true;

	/** Cluster pattern scale. Lower = larger clumps / clearings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseMask", ClampMin = "0.0001", ClampMax = "1.0"))
	float NoiseFrequency = 0.004f;

	/** Below this noise value (0–1) no instance spawns — carves hard clearings / cluster edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseMask", ClampMin = "0.0", ClampMax = "1.0"))
	float NoiseThreshold = 0.3f;

	/** Spawn in the low-noise regions (clearings) instead of the clumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseMask"))
	bool bInvertNoise = false;

	/** Noise seed. Share across tiers so undergrowth aligns with canopy clusters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseMask"))
	int32 NoiseSeed = 0;

	// ── Slope & Surface ───────────────────────────────────────────

	/** Min slope dot (surface normal · world-up). Large trees ~0.7, shrubs ~0.4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlopeDot = 0.7f;

	/** Max slope dot. 1.0 keeps flat ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlopeDot = 1.0f;

	/** 0 = always upright (recommended for trees). 1 = fully aligned to the surface normal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlopeAlignAmount = 0.15f;

	/** Clamp on the slope-align lean so meshes never tip past this off vertical. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float MaxAlignAngleDeg = 30.0f;

	/** Z offset to sink the base into the surface (negative = sink). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface")
	float ZOffset = -10.0f;

	// ── Projection ────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "100.0", ClampMax = "20000.0"))
	float TraceStartHeight = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	// ── Biome Response ────────────────────────────────────────────

	/** One entry per biome this tier responds to. Empty = ignore biomes (uniform). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome",
		meta = (TitleProperty = "BiomeAttribute"))
	TArray<FPCGVegBiomeResponse> BiomeResponses;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	EPCGVegBiomeCombine BiomeCombineMode = EPCGVegBiomeCombine::WeightedAverage;

	/** Apply the per-biome ScaleMultiplier as well as DensityMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bBiomeModulatesScale = true;

	// ── Mesh Set ──────────────────────────────────────────────────

	/** Weighted mesh variants for this tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set",
		meta = (TitleProperty = "Mesh"))
	TArray<FPCGVegMeshEntry> MeshSet;

	/**
	 * Attribute the chosen mesh path is written to. Use a By-Attribute Static Mesh
	 * Spawner downstream.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set")
	FName MeshPathAttributeName = FName(TEXT("MeshPath"));

	/**
	 * Write the final keep probability into each output point's Density. Useful for
	 * visualisation/debugging, but beware: downstream nodes that sample density
	 * (e.g. spawner density-as-probability modes, Density Filter) will apply a
	 * SECOND round of thinning. Disable to output Density = 1.0.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set")
	bool bWriteKeepProbabilityAsDensity = true;

	// ── Transform Randomisation ───────────────────────────────────

	/** Tier-level uniform scale range, applied on top of each mesh's ScaleRange. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector2D TierScaleRange = FVector2D(1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bRandomYaw = true;

	/** Small random pitch/roll tilt for organic variation (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform",
		meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float PitchRollJitterDeg = 2.0f;

	// ── Companions / Understory (optional Companions pin) ─────────

	/**
	 * Emit a ring of understory points around each accepted instance (e.g. shrubs around
	 * big trees). Output is transform-ready but mesh-less — assign shrub meshes downstream
	 * with a Spawner, or pipe into Scatter Around Points for richer control. Companions
	 * inherit slope filtering, projection, and the primary's per-point metadata
	 * (biome weights).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions")
	bool bGenerateCompanions = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions", ClampMin = "0", ClampMax = "16"))
	int32 CompanionsPerPrimaryMin = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions", ClampMin = "0", ClampMax = "16"))
	int32 CompanionsPerPrimaryMax = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions", ClampMin = "0.0"))
	float CompanionRadiusMin = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions", ClampMin = "10.0"))
	float CompanionRadiusMax = 450.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions", ClampMin = "0.0"))
	float CompanionMinDistance = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions"))
	FVector2D CompanionScaleRange = FVector2D(0.6f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Companions",
		meta = (EditCondition = "bGenerateCompanions"))
	float CompanionZOffset = -5.0f;

	// ── Debug ─────────────────────────────────────────────────────

	/** Emit culled candidates on the Rejected pin for debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bOutputRejected = false;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGTieredVegetationScatterElement
	: public IPCGElement
{
protected:
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	/** Cached, weight-accumulated mesh entry. */
	struct FVegMeshCache
	{
		FSoftObjectPath MeshPath;
		FVector2D ScaleRange = FVector2D(1.0f, 1.0f);
		float CumulativeWeight = 0.0f;
	};

	/** Load meshes, accumulate weights. Returns false if none valid. */
	static bool BuildMeshCache(const TArray<FPCGVegMeshEntry>& Entries,
		TArray<FVegMeshCache>& OutCache, float& OutTotalWeight);

	/** Weighted random mesh index. */
	static int32 SelectMeshIndex(const TArray<FVegMeshCache>& Cache, float TotalWeight, FRandomStream& Rng);

	/** Read a const float attribute value, or Default if absent/wrong type. */
	static float ReadFloatAttr(const UPCGMetadata* Meta, FName Name, int64 EntryKey, float Default);

	/** Combine the per-biome responses at a point into a density and scale multiplier. */
	static void ComputeBiomeFactors(const TArray<FPCGVegBiomeResponse>& Responses,
		EPCGVegBiomeCombine Mode, const UPCGMetadata* Meta, int64 EntryKey,
		float& OutDensity, float& OutScale);

	/** Build an upright-ish foliage rotation with optional slope lean, yaw and jitter. */
	static FQuat MakeFoliageRotation(const FVector& SurfaceNormal, float YawDeg,
		float AlignAmount, float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg);

	/** Perlin sample remapped to 0–1. */
	static float Perlin01(const FVector& Position, float Frequency, int32 Seed);
};