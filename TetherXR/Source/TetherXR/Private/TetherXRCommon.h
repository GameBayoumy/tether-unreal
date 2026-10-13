// Copyright (c) 2026 Tether contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

THIRD_PARTY_INCLUDES_START
#include "tether_direct.h"
THIRD_PARTY_INCLUDES_END

#define TETHERXR_UE_5_8_OR_NEWER (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))

DECLARE_LOG_CATEGORY_EXTERN(LogTetherXR, Log, All);

/**
 * A rigid pose in Tether's tracking space: OpenXR conventions (+X right, +Y up, -Z forward, metres,
 * right-handed). FQuat/FVector are used purely as maths containers here; convert with ToUnreal*.
 */
struct FTetherPose
{
	FQuat Orientation = FQuat::Identity;
	FVector Position = FVector::ZeroVector;

	FTetherPose() = default;
	FTetherPose(const FQuat& InOrientation, const FVector& InPosition)
		: Orientation(InOrientation), Position(InPosition)
	{
	}

	explicit FTetherPose(const qlh_pose& Pose)
		: Orientation(Pose.orientation[0], Pose.orientation[1], Pose.orientation[2], Pose.orientation[3])
		, Position(Pose.position[0], Pose.position[1], Pose.position[2])
	{
		Orientation.Normalize();
	}

	FTetherPose Inverse() const
	{
		const FQuat Inv = Orientation.Inverse();
		return FTetherPose(Inv, -Inv.RotateVector(Position));
	}

	/** this * Other: Other expressed in this pose's frame, brought into the parent frame. */
	FTetherPose operator*(const FTetherPose& Other) const
	{
		return FTetherPose(Orientation * Other.Orientation, Position + Orientation.RotateVector(Other.Position));
	}
};

namespace TetherXR
{
	/**
	 * OpenXR -> Unreal basis change: X_ue = -Z, Y_ue = X, Z_ue = Y. It flips handedness (det = -1), so a
	 * rotation's axis maps as an axial vector: q_ue = (qz, -qx, -qy, qw) == (-qz, qx, qy, -qw).
	 */
	inline FVector ToUnrealPosition(const FVector& P, float WorldToMeters)
	{
		return FVector(-P.Z, P.X, P.Y) * WorldToMeters;
	}

	inline FQuat ToUnrealRotation(const FQuat& Q)
	{
		return FQuat(-Q.Z, Q.X, Q.Y, -Q.W);
	}

	/** Heading of a pose (rotation about +Y only), plus ExtraYaw radians. */
	inline FQuat YawOnly(const FQuat& Orientation, double ExtraYaw = 0.0)
	{
		const FVector Forward = Orientation.RotateVector(FVector(0, 0, -1));
		const double Yaw = FMath::Atan2(-Forward.X, -Forward.Z) + ExtraYaw;
		return FQuat(FVector(0, 1, 0), Yaw);
	}

	/** Quest's aim pose: the grip pose pitched up 15 degrees about its X axis (matches Tether's runtime). */
	inline FTetherPose AimFromGrip(const FTetherPose& Grip)
	{
		return Grip * FTetherPose(FQuat(FVector(1, 0, 0), FMath::DegreesToRadians(15.0)), FVector::ZeroVector);
	}
}
