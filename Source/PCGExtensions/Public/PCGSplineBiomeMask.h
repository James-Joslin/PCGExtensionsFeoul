// PCGSplineBiomeMask.h
//
// Custom PCG node that stamps per-biome float weight attributes (0–1) onto
// candidate points based on their proximity to closed-loop spline actors.
// Designed as the upstream producer for TieredVegetationScatter and
// GroundCoverScatter, which read these weights in ComputeBiomeFactors().
//
// Workflow:
//   1. Place closed-loop spline actors in the level, each with a Tag matching
//      a BiomeEntry (e.g. tag = "Forest"). Both Actor Tags (Details → Actor →
//      Tags) and Component Tags on the spline component are accepted.
//   2. In the PCG graph: Surface Sampler → candidate points → In pin.
//   3. Get Spline Data (or Get Actor Data) → Splines pin.
//   4. This node writes Biome_Forest, Biome_Meadow, ... as float metadata.
//   5. Output feeds TieredVegetationScatter / GroundCoverScatter per tier.
//
// Points inside a spline polygon get weight 1.0. Points outside get a
// configurable falloff (linear / smooth-step / exponential) from the spline
// edge out to OuterRadius, optionally perturbed by Perlin noise for organic
// boundary shapes. Weights can be normalised so Σ ≤ 1.0 (pairs with the
// WeightedAverage biome combine mode on the scatter nodes).
//
// Place in your project's Source/<Module>/Public/ directory.
// Requires "PCG" in Build.cs PublicDependencyModuleNames.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGSplineBiomeMask.generated.h"

// ─────────────────────────────────────────────
//  Falloff type
// ─────────────────────────────────────────────

UENUM(BlueprintType)
enum class EPCGBiomeFalloffType : uint8
{
	/** Straight 1 → 0 ramp. Cheapest, slightly artificial edges. */
	Linear      UMETA(DisplayName = "Linear"),
	/** Hermite smoothstep — smooth acceleration / deceleration at both ends. */
	SmoothStep  UMETA(DisplayName = "Smooth Step"),
	/** Steep initial falloff that eases gently to zero. Natural-feeling. */
	Exponential UMETA(DisplayName = "Exponential")
};

// ─────────────────────────────────────────────
//  Per-biome entry
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGBiomeMaskEntry
{
	GENERATED_BODY()

	/**
	 * Tag on the source spline actor in the level (e.g. "Forest").
	 * Matched against the actor's Actor Tags AND its components' Component
	 * Tags (so tagging the Spline component inside a Blueprint works too).
	 * Falls back to checking the FPCGTaggedData tags (set via PCG graph Tag
	 * nodes) if the actor reference is unavailable.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	FName SplineActorTag = FName(TEXT("Forest"));

	/**
	 * Float attribute name written onto each point (e.g. "Biome_Forest").
	 * Must match the BiomeAttribute name configured on downstream
	 * TieredVegetationScatter / GroundCoverScatter BiomeResponse entries.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	FName BiomeAttributeName = FName(TEXT("Biome_Forest"));

	/**
	 * Buffer distance outside the spline edge where weight stays 1.0.
	 * Set to 0 for falloff to begin immediately at the polygon boundary.
	 * Useful for preventing vegetation pop at the exact spline edge.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Falloff",
		meta = (ClampMin = "0.0"))
	float InnerRadius = 0.0f;

	/**
	 * Distance from the spline edge where weight reaches 0.0.
	 * The blend zone spans from InnerRadius to OuterRadius.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Falloff",
		meta = (ClampMin = "0.0"))
	float OuterRadius = 3000.0f;

	/** Shape of the weight falloff between InnerRadius and OuterRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Falloff")
	EPCGBiomeFalloffType FalloffType = EPCGBiomeFalloffType::SmoothStep;
};

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGSplineBiomeMaskSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("SplineBiomeMask")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGSplineBiomeMask", "NodeTitle", "Spline Biome Mask");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGSplineBiomeMask", "Tooltip",
			"Stamps per-biome float weight attributes (0\u20131) onto candidate points based "
			"on proximity to closed-loop spline actors. Points inside a spline polygon receive "
			"weight 1.0; points outside receive a configurable falloff from the spline edge "
			"out to OuterRadius. Weights can be normalised to sum \u2264 1.0. Feed the output "
			"into TieredVegetationScatter / GroundCoverScatter which read these attributes via "
			"their BiomeResponse entries.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Biome Configuration ───────────────────────────────────────

	/**
	 * One entry per biome. Each matches spline actors in the level by their
	 * Actor Tag and writes a named float attribute onto every candidate point.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Mask",
		meta = (TitleProperty = "SplineActorTag"))
	TArray<FPCGBiomeMaskEntry> BiomeEntries;

	/**
	 * Normalise weights so they sum to at most 1.0 at every point.
	 * When enabled, overlapping biome zones compete proportionally rather
	 * than stacking. Pairs cleanly with the WeightedAverage biome combine
	 * mode on the downstream scatter nodes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Mask")
	bool bNormaliseWeights = true;

	/**
	 * Also write a residual weight attribute = 1 - Σ(biome weights): how strongly
	 * "no biome" (default terrain) this point is. Use this instead of a Create
	 * Constant + Subtract chain in the graph — it stays correct automatically when
	 * more biomes are added, and its attribute default of 0.0 means missing/broken
	 * metadata downstream reads as "no biome data" rather than masquerading as
	 * pure default biome.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Mask")
	bool bEmitResidualWeight = false;

	/** Attribute name for the residual weight (e.g. "Biome_Default"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome Mask",
		meta = (EditCondition = "bEmitResidualWeight"))
	FName ResidualAttributeName = FName(TEXT("Biome_Default"));

	// ── Spline Sampling ───────────────────────────────────────────

	/**
	 * Sample points per spline segment for the polyline approximation used
	 * in point-in-polygon and distance tests. Higher = more accurate at
	 * tightly curved spline regions, but costs O(N × samples) per point.
	 * 8 is fine for most landscape-scale biome boundaries.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spline Sampling",
		meta = (ClampMin = "2", ClampMax = "32"))
	int32 SamplesPerSegment = 8;

	// ── Edge Noise ────────────────────────────────────────────────

	/**
	 * Perturb the effective outer radius per-point with Perlin noise so biome
	 * edges look organic rather than perfectly following the spline geometry.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Noise")
	bool bUseEdgeNoise = false;

	/** Spatial frequency of the edge perturbation. Lower = larger undulations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Noise",
		meta = (EditCondition = "bUseEdgeNoise", ClampMin = "0.0001", ClampMax = "0.1"))
	float EdgeNoiseFrequency = 0.003f;

	/** Maximum perturbation of the outer radius in UU (both directions). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Noise",
		meta = (EditCondition = "bUseEdgeNoise", ClampMin = "0.0"))
	float EdgeNoiseAmplitude = 500.0f;

	/** Seed for edge noise. Share across biome entries for correlated edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Edge Noise",
		meta = (EditCondition = "bUseEdgeNoise"))
	int32 EdgeNoiseSeed = 42;

	// ── Debug ─────────────────────────────────────────────────────

	/** Log spline matching, per-biome coverage stats, and timing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bLogDiagnostics = false;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGSplineBiomeMaskElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	/** Pre-sampled polyline for one spline actor. */
	struct FBiomePolyline
	{
		TArray<FVector2D> Points;
	};

	/** All polylines belonging to one biome. */
	struct FBiomeGroup
	{
		int32 EntryIndex = INDEX_NONE;
		TArray<FBiomePolyline> Polylines;
	};

	/** Sample a spline into an XY polyline for geometry tests. */
	static TArray<FVector2D> SampleSplineToPolyline(
		const class UPCGSplineData* SplineData, int32 SamplesPerSegment);

	/** 2D ray-casting point-in-polygon. Polygon is implicitly closed. */
	static bool PointInPolygon2D(const FVector2D& P, const TArray<FVector2D>& Polygon);

	/** Minimum 2D distance from P to the polyline boundary. */
	static float DistanceToPolyline2D(const FVector2D& P, const TArray<FVector2D>& Polyline);

	/** Map a distance through the inner/outer falloff. */
	static float ApplyFalloff(float Distance, float InnerRadius, float OuterRadius,
		EPCGBiomeFalloffType Type);

	/** Perlin sample remapped to 0–1. Same implementation as the scatter nodes. */
	static float Perlin01(const FVector& Position, float Frequency, int32 Seed);
};