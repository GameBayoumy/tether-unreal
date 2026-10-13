// Copyright (c) 2026 Tether contributors. MIT License.

#pragma once

#include "TetherXRCommon.h"
#include "IInputDevice.h"
#include "InputCoreTypes.h"
#include "XRMotionControllerBase.h"

class FTetherXRHMD;

/**
 * Touch controllers through Tether: emits the engine's Oculus Touch keys (so Enhanced Input mappings such as
 * the VR template's work) and serves grip/aim poses to MotionControllerComponents.
 */
class FTetherXRInput : public IInputDevice, public FXRMotionControllerBase
{
public:
	explicit FTetherXRInput(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler);
	virtual ~FTetherXRInput() override;

	/** Registers the few Touch keys the engine doesn't define (thumb rests), under the engine's naming. */
	static void RegisterKeys();

	// IInputDevice
	virtual void Tick(float DeltaTime) override {}
	virtual void SendControllerEvents() override;
	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override { MessageHandler = InMessageHandler; }
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }
	virtual void SetChannelValue(int32 ControllerId, FForceFeedbackChannelType ChannelType, float Value) override {}
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override {}
	virtual bool SupportsForceFeedback(int32 ControllerId) override { return false; }

	// IMotionController
	using FXRMotionControllerBase::GetControllerOrientationAndPosition;
	virtual FName GetMotionControllerDeviceTypeName() const override;
	virtual bool GetControllerOrientationAndPosition(const int32 ControllerIndex, const FName MotionSource, FRotator& OutOrientation, FVector& OutPosition, float WorldToMetersScale) const override;
	virtual ETrackingStatus GetControllerTrackingStatus(const int32 ControllerIndex, const FName MotionSource) const override;
	virtual void EnumerateSources(TArray<FMotionControllerSource>& SourcesOut) const override;

private:
	struct FButton
	{
		FKey Key;
		bool bPressed = false;
	};

	struct FAxis
	{
		FKey Key;
		float Value = 0.f;
	};

	/** Keys of one hand, in a fixed order (see SendControllerEvents). */
	struct FHand
	{
		TArray<FButton> Buttons;
		TArray<FAxis> Axes;
	};

	static FTetherXRHMD* GetHMD();
	void SetButton(FButton& Button, bool bPressed);
	void SetAxis(FAxis& Axis, float Value);
	void ReleaseAll();

	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;
	FHand Hands[2];
	bool bActive = false;
};
