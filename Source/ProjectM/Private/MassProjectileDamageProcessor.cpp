#include "MassProjectileDamageProcessor.h"

#include "MassEntityView.h"
#include "MassLODTypes.h"
#include "MassCommonFragments.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MassPlayerSubsystem.h"
#include "Character/CommanderCharacter.h"
#include "MilitaryStructureSubsystem.h"
#include <MassVisualEffectsSubsystem.h>

//----------------------------------------------------------------------//
//  UMassProjectileWithDamageTrait
//----------------------------------------------------------------------//
void UMassProjectileWithDamageTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	BuildContext.AddFragment<FMassPreviousLocationFragment>();

	FProjectileDamageFragment& ProjectileDamageFragment = BuildContext.AddFragment_GetRef<FProjectileDamageFragment>();
	ProjectileDamageFragment.DamagePerHit = DamagePerHit;
	ProjectileDamageFragment.Caliber = Caliber;

	if (ExplosionEntityConfig)
	{
		UMassVisualEffectsSubsystem* MassVisualEffectsSubsystem = UWorld::GetSubsystem<UMassVisualEffectsSubsystem>(&World);
		ProjectileDamageFragment.ExplosionEntityConfigIndex = MassVisualEffectsSubsystem->FindOrAddEntityConfig(ExplosionEntityConfig);
	}

	BuildContext.AddTag<FMassProjectileWithDamageTag>();

	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddFragment<FMassVelocityFragment>();
	FMassForceFragment& ForceTemplate = BuildContext.AddFragment_GetRef<FMassForceFragment>();
	ForceTemplate.Value = FVector(0.f, 0.f, GravityMagnitude);

	// Needed because of UMassApplyMovementProcessor::ConfigureQueries requirements even though it's not used
	UMassEntitySubsystem* EntitySubsystem = UWorld::GetSubsystem<UMassEntitySubsystem>(&World);
	check(EntitySubsystem);
	const FConstSharedStruct MovementFragment = EntitySubsystem->GetOrCreateConstSharedFragment(UE::StructUtils::GetStructCrc32(FConstStructView::Make(Movement)), Movement);
	BuildContext.AddConstSharedFragment(MovementFragment);

	const FConstSharedStruct MinZFragment = EntitySubsystem->GetOrCreateConstSharedFragment(UE::StructUtils::GetStructCrc32(FConstStructView::Make(MinZ)), MinZ);
	BuildContext.AddConstSharedFragment(MinZFragment);

	const FConstSharedStruct DebugParametersFragment = EntitySubsystem->GetOrCreateConstSharedFragment(UE::StructUtils::GetStructCrc32(FConstStructView::Make(DebugParameters)), DebugParameters);
	BuildContext.AddConstSharedFragment(DebugParametersFragment);
}

//----------------------------------------------------------------------//
//  UMassProjectileDamagableTrait
//----------------------------------------------------------------------//
void UMassProjectileDamagableTrait::BuildTemplate(FMassEntityTemplateBuildContext& BuildContext, UWorld& World) const
{
	if (bIsSoldier)
	{
		BuildContext.AddTag<FMassProjectileDamagableSoldierTag>();
	}

	FProjectileDamagableFragment& ProjectileDamagableTemplate = BuildContext.AddFragment_GetRef<FProjectileDamagableFragment>();
	ProjectileDamagableTemplate.MinCaliberForDamage = MinCaliberForDamage;
}

//----------------------------------------------------------------------//
//  UMassProjectileDamageProcessor
//----------------------------------------------------------------------//
UMassProjectileDamageProcessor::UMassProjectileDamageProcessor()
{
	bAutoRegisterWithProcessingPhases = true;
	ExecutionFlags = (int32)EProcessorExecutionFlags::All;
	ProcessingPhase = EMassProcessingPhase::PostPhysics;
}

void UMassProjectileDamageProcessor::ConfigureQueries()
{
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FAgentRadiusFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FProjectileDamageFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassPreviousLocationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddTagRequirement<FMassProjectileWithDamageTag>(EMassFragmentPresence::All);
	EntityQuery.AddConstSharedRequirement<FDebugParameters>(EMassFragmentPresence::All);
}

static void FindCloseObstacles(const FVector& Center, const float SearchRadius, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid,
	TArray<FMassNavigationObstacleItem, TFixedAllocator<2>>& OutCloseEntities, const int32 MaxResults)
{
	OutCloseEntities.Reset();
	const FVector Extent(SearchRadius, SearchRadius, 0.f);
	const FBox QueryBox = FBox(Center - Extent, Center + Extent);

	struct FSortingCell
	{
		int32 X;
		int32 Y;
		int32 Level;
		float SqDist;
	};
	TArray<FSortingCell, TInlineAllocator<64>> Cells;
	const FVector QueryCenter = QueryBox.GetCenter();

	for (int32 Level = 0; Level < AvoidanceObstacleGrid.NumLevels; Level++)
	{
		const float CellSize = AvoidanceObstacleGrid.GetCellSize(Level);
		const FNavigationObstacleHashGrid2D::FCellRect Rect = AvoidanceObstacleGrid.CalcQueryBounds(QueryBox, Level);
		for (int32 Y = Rect.MinY; Y <= Rect.MaxY; Y++)
		{
			for (int32 X = Rect.MinX; X <= Rect.MaxX; X++)
			{
				const float CenterX = (X + 0.5f) * CellSize;
				const float CenterY = (Y + 0.5f) * CellSize;
				const float DX = CenterX - QueryCenter.X;
				const float DY = CenterY - QueryCenter.Y;
				const float SqDist = DX * DX + DY * DY;
				FSortingCell SortCell;
				SortCell.X = X;
				SortCell.Y = Y;
				SortCell.Level = Level;
				SortCell.SqDist = SqDist;
				Cells.Add(SortCell);
			}
		}
	}

	Cells.Sort([](const FSortingCell& A, const FSortingCell& B) { return A.SqDist < B.SqDist; });

	for (const FSortingCell& SortedCell : Cells)
	{
		if (const FNavigationObstacleHashGrid2D::FCell* Cell = AvoidanceObstacleGrid.FindCell(SortedCell.X, SortedCell.Y, SortedCell.Level))
		{
			const TSparseArray<FNavigationObstacleHashGrid2D::FItem>& Items = AvoidanceObstacleGrid.GetItems();
			for (int32 Idx = Cell->First; Idx != INDEX_NONE; Idx = Items[Idx].Next)
			{
				OutCloseEntities.Add(Items[Idx].ID);
				if (OutCloseEntities.Num() >= MaxResults)
				{
					return;
				}
			}
		}
	}
}

void UMassProjectileDamageProcessor::Initialize(UObject& Owner)
{
	Super::Initialize(Owner);

	NavigationSubsystem = UWorld::GetSubsystem<UMassNavigationSubsystem>(Owner.GetWorld());
}

// Returns true if found another entity.
bool GetClosestEntity(const FMassEntityHandle& Entity, UMassEntitySubsystem& EntitySubsystem, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid, const FVector& Location, const float Radius, TArray<FMassNavigationObstacleItem, TFixedAllocator<2>>& CloseEntities, FMassEntityHandle& OutOtherEntity)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassProjectileDamageProcessor_GetClosestEntity);

	FindCloseObstacles(Location, Radius, AvoidanceObstacleGrid, CloseEntities, 2);

	for (const FNavigationObstacleHashGrid2D::ItemIDType OtherEntity : CloseEntities)
	{
		// Skip self
		if (OtherEntity.Entity == Entity)
		{
			continue;
		}

		// Skip invalid entities.
		if (!EntitySubsystem.IsEntityValid(OtherEntity.Entity))
		{
			continue;
		}
		OutOtherEntity = OtherEntity.Entity;
		return true;
	}

	OutOtherEntity = UMassEntitySubsystem::InvalidEntity;
	return false;
}

bool DidCollideViaLineTrace(const UWorld &World, const FVector& StartLocation, const FVector &EndLocation, const bool& DrawLineTraces, TQueue<FHitResult>& DebugLinesToDrawQueue)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassProjectileDamageProcessor_DidCollideViaLineTrace);

	FHitResult Result;
	bool const bSuccess = World.LineTraceSingleByChannel(Result, StartLocation, EndLocation, ECollisionChannel::ECC_Visibility);
	if (DrawLineTraces)
	{
		DebugLinesToDrawQueue.Enqueue(Result);
	}

	return bSuccess;
}

struct FCapsule
{
	FVector a;
	FVector b;
	float r;
};

float ClosestPtSegmentSegment(FVector p1, FVector q1, FVector p2, FVector q2,
	float& s, float& t, FVector& c1, FVector& c2)
{
	FVector d1 = q1 - p1; // Direction vector of segment S1
	FVector d2 = q2 - p2; // Direction vector of segment S2
	FVector r = p1 - p2;
	float a = FVector::DotProduct(d1, d1); // Squared length of segment S1, always nonnegative
	float e = FVector::DotProduct(d2, d2); // Squared length of segment S2, always nonnegative
	float f = FVector::DotProduct(d2, r);
	// Check if either or both segments degenerate into points
	if (a <= SMALL_NUMBER && e <= SMALL_NUMBER) {
		// Both segments degenerate into points
		s = t = 0.0f;
		c1 = p1;
		c2 = p2;
		return FVector::DotProduct(c1 - c2, c1 - c2);
	}
	if (a <= SMALL_NUMBER) {
		// First segment degenerates into a point
		s = 0.0f;
		t = f / e; // s = 0 => t = (b*s + f) / e = f / e
		t = FMath::Clamp(t, 0.0f, 1.0f);
	}
	else {
		float c = FVector::DotProduct(d1, r);
		if (e <= SMALL_NUMBER) {
			// Second segment degenerates into a point
			t = 0.0f;
			s = FMath::Clamp(-c / a, 0.0f, 1.0f); // t = 0 => s = (b*t - c) / a = -c / a
		}
		else {
			// The general nondegenerate case starts here
			float b = FVector::DotProduct(d1, d2);
			float denom = a * e - b * b; // Always nonnegative
			// If segments not parallel, compute closest point on L1 to L2 and
			// clamp to segment S1. Else pick arbitrary s (here 0)
			if (denom != 0.0f) {
				s = FMath::Clamp((b * f - c * e) / denom, 0.0f, 1.0f);
			}
			else s = 0.0f;
			// Compute point on L2 closest to S1(s) using
			// t = Dot((P1 + D1*s) - P2,D2) / Dot(D2,D2) = (b*s + f) / e
			t = (b * s + f) / e;
			// If t in [0,1] done. Else clamp t, recompute s for the new value
			// of t using s = Dot((P2 + D2*t) - P1,D1) / Dot(D1,D1)= (t*b - c) / a
			// and clamp s to [0, 1]
			if (t < 0.0f) {
				t = 0.0f;
				s = FMath::Clamp(-c / a, 0.0f, 1.0f);
			}
			else if (t > 1.0f) {
				t = 1.0f;
				s = FMath::Clamp((b - c) / a, 0.0f, 1.0f);
			}
		}
	}
	c1 = p1 + d1 * s;
	c2 = p2 + d2 * t;
	return FVector::DotProduct(c1 - c2, c1 - c2);
}

bool TestCapsuleCapsule(FCapsule capsule1, FCapsule capsule2)
{
	// Compute (squared) distance between the inner structures of the capsules
	float s, t;
	FVector c1, c2;
	float dist2 = ClosestPtSegmentSegment(capsule1.a, capsule1.b,
		capsule2.a, capsule2.b, s, t, c1, c2);
	// If (squared) distance smaller than (squared) sum of radii, they collide
	float radius = capsule1.r + capsule2.r;
	return dist2 <= radius * radius;
}

FVector GetCapsuleCenter(const FCapsule& Capsule)
{
	return (Capsule.b - Capsule.a) / 2.f + Capsule.a;
}

float GetCapsuleHalfHeight(const FCapsule& Capsule)
{
	return (Capsule.b - Capsule.a).Size() / 2.f;
}

void DrawCapsule(const FCapsule& Capsule, const UWorld& World, const FLinearColor &Color = FLinearColor::Red)
{
	FQuat const CapsuleRot = FRotationMatrix::MakeFromZ(Capsule.b - Capsule.a).ToQuat();
	DrawDebugCapsule(&World, GetCapsuleCenter(Capsule), GetCapsuleHalfHeight(Capsule), Capsule.r, CapsuleRot, Color.ToFColor(true), true);
}

FCapsule MakeCapsule(const FTransform& Transform, const FVector& CenterOffset, const float& Radius, const float& Length)
{
	FCapsule Capsule;

	FVector Center = Transform.GetLocation() + CenterOffset;
	FVector Forward = Transform.GetRotation().GetForwardVector().GetSafeNormal();

	Capsule.a = Center + Forward * (Length / 2.f);
	Capsule.b = Center + Forward * (-Length / 2.f);
	Capsule.r = Radius;

	return Capsule;
}

bool UMassProjectileDamageProcessor_DrawCapsules = false;
FAutoConsoleVariableRef CVarUMassProjectileDamageProcessor_DrawCapsules(TEXT("pm.UMassProjectileDamageProcessor_DrawCapsules"), UMassProjectileDamageProcessor_DrawCapsules, TEXT("UMassProjectileDamageProcessor: Debug draw capsules used for collisions detection"));

bool DidCollideWithEntity(const FVector& StartLocation, const FVector& EndLocation, const float Radius, FTransformFragment* OtherTransformFragment, const bool& DrawCapsules, const UWorld& World, const bool& bIsOtherEntitySoldier, TQueue<FCapsule>& DebugCapsulesToDrawQueue)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassProjectileDamageProcessor_DidCollideWithEntity);
	if (!OtherTransformFragment)
	{
		return false;
	}

	FTransform OtherEntityTransform = OtherTransformFragment->GetTransform();
	FVector OtherEntityLocation = OtherEntityTransform.GetLocation();

	FCapsule ProjectileCapsule;
	ProjectileCapsule.a = StartLocation;
	ProjectileCapsule.b = EndLocation;
	ProjectileCapsule.r = Radius;

	FCapsule OtherEntityCapsule;
	if (bIsOtherEntitySoldier)
	{
		OtherEntityCapsule.a = OtherEntityLocation;
		static const float EntityHeight = 200.0f; // TODO: don't hard-code; add new fragment for this?
		static const float EntityRadius = 40.0f; // TODO: don't hard-code; read from other entity's AgentRadius?
		OtherEntityCapsule.b = OtherEntityLocation + FVector(0.f, 0.f, EntityHeight);
		OtherEntityCapsule.r = EntityRadius;
	}
	else // Tank
	{
		OtherEntityCapsule = MakeCapsule(OtherEntityTransform, FVector(0.f, 0.f, 150.f), 170.f, 800.f);
	}

	if (DrawCapsules || UMassProjectileDamageProcessor_DrawCapsules)
	{
		DebugCapsulesToDrawQueue.Enqueue(ProjectileCapsule);
		DebugCapsulesToDrawQueue.Enqueue(OtherEntityCapsule);
	}

	return TestCapsuleCapsule(ProjectileCapsule, OtherEntityCapsule);
}

bool CanProjectileDamageEntity(const FProjectileDamagableFragment* ProjectileDamagableFragment, const float& ProjectileCaliber)
{
	return ProjectileDamagableFragment && ProjectileCaliber >= ProjectileDamagableFragment->MinCaliberForDamage;
}

bool UMassProjectileDamageProcessor_SkipDealingDamage = false;
FAutoConsoleVariableRef CVarUMassProjectileDamageProcessor_SkipDealingDamage(TEXT("pm.UMassProjectileDamageProcessor_SkipDealingDamage"), UMassProjectileDamageProcessor_SkipDealingDamage, TEXT("UMassProjectileDamageProcessor: Skip dealing damage"));

void HandleProjectileImpact(TQueue<FMassEntityHandle>& ProjectilesToDestroy, const FMassEntityHandle Entity, UWorld* World, const FProjectileDamageFragment& ProjectileDamageFragment, const FVector& Location)
{
	ProjectilesToDestroy.Enqueue(Entity);

	if (ProjectileDamageFragment.ExplosionEntityConfigIndex >= 0)
	{
		UMassVisualEffectsSubsystem* MassVisualEffectsSubsystem = UWorld::GetSubsystem<UMassVisualEffectsSubsystem>(World);
		check(MassVisualEffectsSubsystem);

		// Must be done async because we can't spawn Mass entities in the middle of a Mass processor's Execute method.
		AsyncTask(ENamedThreads::GameThread, [MassVisualEffectsSubsystem, ExplosionEntityConfigIndex = ProjectileDamageFragment.ExplosionEntityConfigIndex, Location]()
		{
			MassVisualEffectsSubsystem->SpawnEntity(ExplosionEntityConfigIndex, Location);
		});
	}
}

void ProcessProjectileDamageEntity(FMassExecutionContext& Context, FMassEntityHandle Entity, UMassEntitySubsystem& EntitySubsystem, const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid, const FTransformFragment& Location, const FAgentRadiusFragment& Radius, const FProjectileDamageFragment& ProjectileDamageFragment, TArray<FMassNavigationObstacleItem, TFixedAllocator<2>>& CloseEntities, const FMassPreviousLocationFragment& PreviousLocationFragment, const bool& DrawLineTraces, TQueue<FMassEntityHandle>& ProjectilesToDestroy, TQueue<FMassEntityHandle>& SoldiersToDestroy, TQueue<FMassEntityHandle>& PlayersToDestroy, TQueue<FHitResult>& DebugLinesToDrawQueue, TQueue<FCapsule>& DebugCapsulesToDrawQueue)
{
	UWorld* World = EntitySubsystem.GetWorld();

	// If collide via line trace, we hit the environment, so destroy projectile.
	const FVector& CurrentLocation = Location.GetTransform().GetLocation();
	if (DidCollideViaLineTrace(*World, PreviousLocationFragment.Location, CurrentLocation, DrawLineTraces, DebugLinesToDrawQueue))
	{
		HandleProjectileImpact(ProjectilesToDestroy, Entity, World, ProjectileDamageFragment, CurrentLocation);
		return;
	}

	FMassEntityHandle OtherEntity;
	bool bHasCloseEntity = GetClosestEntity(Entity, EntitySubsystem, AvoidanceObstacleGrid, Location.GetTransform().GetTranslation(), Radius.Radius, CloseEntities, OtherEntity);
	if (!bHasCloseEntity) {
		return;
	}

	FMassEntityView OtherEntityView(EntitySubsystem, OtherEntity);
	FTransformFragment* OtherTransformFragment = OtherEntityView.GetFragmentDataPtr<FTransformFragment>();
	const bool& bIsOtherEntitySoldier = OtherEntityView.HasTag<FMassProjectileDamagableSoldierTag>();

	if (!DidCollideWithEntity(PreviousLocationFragment.Location, CurrentLocation, Radius.Radius, OtherTransformFragment, DrawLineTraces, *World, bIsOtherEntitySoldier, DebugCapsulesToDrawQueue))
	{
		return;
	}

	HandleProjectileImpact(ProjectilesToDestroy, Entity, World, ProjectileDamageFragment, CurrentLocation);

	FMassHealthFragment* OtherHealthFragment = OtherEntityView.GetFragmentDataPtr<FMassHealthFragment>();
	if (!OtherHealthFragment)
	{
		return;
	}

	const bool& bCanProjectileDamageOtherEntity = CanProjectileDamageEntity(OtherEntityView.GetFragmentDataPtr<FProjectileDamagableFragment>(), ProjectileDamageFragment.Caliber);
	if (!bCanProjectileDamageOtherEntity)
	{
		return;
	}

	if (UMassProjectileDamageProcessor_SkipDealingDamage)
	{
		return;
	}

	OtherHealthFragment->Value -= ProjectileDamageFragment.DamagePerHit;

	// Handle health reaching 0.
	if (OtherHealthFragment->Value <= 0)
	{
		bool bHasPlayerTag = OtherEntityView.HasTag<FMassPlayerControllableCharacterTag>();
		if (!bHasPlayerTag)
		{
			SoldiersToDestroy.Enqueue(OtherEntity);
		} else {
			PlayersToDestroy.Enqueue(OtherEntity);
		}
	}
}

bool UMassProjectileDamageProcessor_UseParallelForEachEntityChunk = true;
FAutoConsoleVariableRef CVarUMassProjectileDamageProcessor_UseParallelForEachEntityChunk(TEXT("pm.UMassProjectileDamageProcessor_UseParallelForEachEntityChunk"), UMassProjectileDamageProcessor_UseParallelForEachEntityChunk, TEXT("Use ParallelForEachEntityChunk in UMassProjectileDamageProcessor::Execute to improve performance"));

void ProcessQueues(TQueue<FMassEntityHandle>& ProjectilesToDestroy, TQueue<FMassEntityHandle>& SoldiersToDestroy, TQueue<FMassEntityHandle>& PlayersToDestroy, TQueue<FHitResult>& DebugLinesToDrawQueue, TQueue<FCapsule>& DebugCapsulesToDrawQueue, UWorld* World, FMassExecutionContext& Context)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassProjectileDamageProcessor_ProcessQueues);

	// Destroy projectiles.
	while (!ProjectilesToDestroy.IsEmpty())
	{
		FMassEntityHandle EntityToDestroy;
		bool bSuccess = ProjectilesToDestroy.Dequeue(EntityToDestroy);
		check(bSuccess);
		Context.Defer().DestroyEntity(EntityToDestroy);
	}

	// Destroy AI soldiers.
	UMilitaryStructureSubsystem* MilitaryStructureSubsystem = UWorld::GetSubsystem<UMilitaryStructureSubsystem>(World);
	check(MilitaryStructureSubsystem);
	while (!SoldiersToDestroy.IsEmpty())
	{
		FMassEntityHandle EntityToDestroy;
		bool bSuccess = SoldiersToDestroy.Dequeue(EntityToDestroy);
		check(bSuccess);
		Context.Defer().DestroyEntity(EntityToDestroy);
		MilitaryStructureSubsystem->DestroyEntity(EntityToDestroy);
	}

	// Destroy player soldiers.
	UMassPlayerSubsystem* PlayerSubsystem = UWorld::GetSubsystem<UMassPlayerSubsystem>(World);
	check(PlayerSubsystem);
	while (!PlayersToDestroy.IsEmpty())
	{
		FMassEntityHandle EntityToDestroy;
		bool bSuccess = PlayersToDestroy.Dequeue(EntityToDestroy);
		check(bSuccess);
		AActor* OtherActor = PlayerSubsystem->GetActorForEntity(EntityToDestroy);
		check(OtherActor);
		ACommanderCharacter* Character = CastChecked<ACommanderCharacter>(OtherActor);
		AsyncTask(ENamedThreads::GameThread, [Character]()
		{
			Character->DidDie();
		});
	}

	// Draw debug lines.
	while (!DebugLinesToDrawQueue.IsEmpty())
	{
		FHitResult HitResult;
		bool bSuccess = DebugLinesToDrawQueue.Dequeue(HitResult);
		check(bSuccess);

		static const FLinearColor TraceColor = FLinearColor::Red;
		static const FLinearColor TraceHitColor = FLinearColor::Green;

		if (HitResult.bBlockingHit)
		{
			// Red up to the blocking hit, green thereafter
			::DrawDebugLine(World, HitResult.TraceStart, HitResult.ImpactPoint, TraceColor.ToFColor(true), true);
			::DrawDebugLine(World, HitResult.ImpactPoint, HitResult.TraceEnd, TraceHitColor.ToFColor(true), true);
			::DrawDebugPoint(World, HitResult.ImpactPoint, 16.f, TraceColor.ToFColor(true), true);
		}
		else
		{
			// no hit means all red
			::DrawDebugLine(World, HitResult.TraceStart, HitResult.TraceEnd, TraceColor.ToFColor(true), true);
		}
	}

	// Draw debug capsules.
	while (!DebugCapsulesToDrawQueue.IsEmpty())
	{
		FCapsule Capsule;
		bool bSuccess = DebugCapsulesToDrawQueue.Dequeue(Capsule);
		check(bSuccess);
		DrawCapsule(Capsule, *World);
	}
		
}

void UMassProjectileDamageProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	QUICK_SCOPE_CYCLE_COUNTER(UMassProjectileDamageProcessor);

	if (!NavigationSubsystem)
	{
		return;
	}

	TQueue<FMassEntityHandle> ProjectilesToDestroy;
	TQueue<FMassEntityHandle> SoldiersToDestroy;
	TQueue<FMassEntityHandle> PlayersToDestroy;
	TQueue<FHitResult> DebugLinesToDrawQueue;
	TQueue<FCapsule> DebugCapsulesToDrawQueue;

	auto ExecuteFunction = [&EntitySubsystem, &NavigationSubsystem = NavigationSubsystem, &ProjectilesToDestroy, &SoldiersToDestroy, &PlayersToDestroy, &DebugLinesToDrawQueue, &DebugCapsulesToDrawQueue](FMassExecutionContext& Context)
	{
		const int32 NumEntities = Context.GetNumEntities();

		const TConstArrayView<FTransformFragment> LocationList = Context.GetFragmentView<FTransformFragment>();
		const TConstArrayView<FAgentRadiusFragment> RadiusList = Context.GetFragmentView<FAgentRadiusFragment>();
		const TConstArrayView<FProjectileDamageFragment> ProjectileDamageList = Context.GetFragmentView<FProjectileDamageFragment>();
		const TArrayView<FMassPreviousLocationFragment> PreviousLocationList = Context.GetMutableFragmentView<FMassPreviousLocationFragment>();
		const FDebugParameters& DebugParameters = Context.GetConstSharedFragment<FDebugParameters>();

		// Arrays used to store close obstacles
		TArray<FMassNavigationObstacleItem, TFixedAllocator<2>> CloseEntities;

		// Used for storing sorted list or nearest obstacles.
		struct FSortedObstacle
		{
			FVector LocationCached;
			FVector Forward;
			FMassNavigationObstacleItem ObstacleItem;
			float SqDist;
		};

		// TODO: We're incorrectly assuming all obstacles can get damaged by projectile.
		const FNavigationObstacleHashGrid2D& AvoidanceObstacleGrid = NavigationSubsystem->GetObstacleGridMutable();

		for (int32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
		{
			ProcessProjectileDamageEntity(Context, Context.GetEntity(EntityIndex), EntitySubsystem, AvoidanceObstacleGrid, LocationList[EntityIndex], RadiusList[EntityIndex], ProjectileDamageList[EntityIndex], CloseEntities, PreviousLocationList[EntityIndex], DebugParameters.DrawLineTraces, ProjectilesToDestroy, SoldiersToDestroy, PlayersToDestroy, DebugLinesToDrawQueue, DebugCapsulesToDrawQueue);
			PreviousLocationList[EntityIndex].Location = LocationList[EntityIndex].GetTransform().GetLocation();
		}
	};

	if (UMassProjectileDamageProcessor_UseParallelForEachEntityChunk)
	{
		EntityQuery.ParallelForEachEntityChunk(EntitySubsystem, Context, ExecuteFunction);
	}
	else
	{
		EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, ExecuteFunction);
	}

	ProcessQueues(ProjectilesToDestroy,  SoldiersToDestroy, PlayersToDestroy, DebugLinesToDrawQueue, DebugCapsulesToDrawQueue, EntitySubsystem.GetWorld(), Context);
}
