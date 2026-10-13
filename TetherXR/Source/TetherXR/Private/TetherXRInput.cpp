// Copyright (c) 2026 Tether contributors. MIT License.

#include "TetherXRInput.h"

#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "IXRTrackingSystem.h"
#include "TetherRuntime.h"
#include "TetherXRHMD.h"

#define LOCTEXT_NAMESPACE "TetherXR"

namespace
{
	// The engine has no thumb rest keys; these follow its "OculusTouch_<Hand>_<Input>" naming.
	const FKey LeftThumbRest(TEXT("OculusTouch_Left_ThumbRest"));
	const FKey RightThumbRest(TEXT("OculusTouch_Right_ThumbRest"));

	const FName LeftGripSource(TEXT("LeftGrip"));
	const FName RightGripSource(TEXT("RightGrip"));
	const FName LeftAimSource(TEXT("LeftAim"));
	const FName RightAimSource(TEXT("RightAim"));

	// Capacitive sensors in qlh_controller::touches (as Tether's OpenXR runtime reads them).
	constexpr uint32 TouchFaceLower = 0x01; // X / A
	constexpr uint32 TouchFaceUpper = 0x02; // Y / B
	constexpr uint32 TouchThumbstick = 0x04;
	constexpr uint32 TouchTrigger = 0x08;
	constexpr uint32 TouchThumbRest = 0x10;

	// Button order within FHand::Buttons; SendControllerEvents fills the same order.
	enum EButton
	{
		FaceLowerClick, FaceUpperClick, FaceLowerTouch, FaceUpperTouch, Menu,
		TriggerClick, TriggerTouch, GripClick, ThumbstickClick, ThumbstickTouch, ThumbRest,
		ThumbstickUp, ThumbstickDown, ThumbstickLeft, ThumbstickRight,
		NumButtons
	};

	enum EAxis { TriggerAxis, GripAxis, ThumbstickX, ThumbstickY, NumAxes };

	/** Press above Press, release below Release, otherwise keep the previous state. */
	bool Hysteresis(float Value, bool bWasPressed, float Press, float Release)
	{
		return bWasPressed ? Value > Release : Value > Press;
	}

	/** Resolves a MotionControllerComponent source to a hand and whether it wants the aim pose. */
	bool ResolveSource(const FName Source, int32& OutHand, bool& bOutAim)
	{
		if (Source == IMotionController::LeftHandSourceId || Source == LeftGripSource || Source == LeftAimSource)
		{
			OutHand = 0;
			bOutAim = Source == LeftAimSource;
			return true;
		}
		if (Source == IMotionController::RightHandSourceId || Source == RightGripSource || Source == RightAimSource)
		{
			OutHand = 1;
			bOutAim = Source == RightAimSource;
			return true;
		}
		return false;
	}
}

FTetherXRInput::FTetherXRInput(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	const FKey Keys[2][NumButtons] = {
		{
			EKeys::OculusTouch_Left_X_Click, EKeys::OculusTouch_Left_Y_Click,
			EKeys::OculusTouch_Left_X_Touch, EKeys::OculusTouch_Left_Y_Touch, EKeys::OculusTouch_Left_Menu_Click,
			EKeys::OculusTouch_Left_Trigger_Click, EKeys::OculusTouch_Left_Trigger_Touch, EKeys::OculusTouch_Left_Grip_Click,
			EKeys::OculusTouch_Left_Thumbstick_Click, EKeys::OculusTouch_Left_Thumbstick_Touch, LeftThumbRest,
			EKeys::OculusTouch_Left_Thumbstick_Up, EKeys::OculusTouch_Left_Thumbstick_Down,
			EKeys::OculusTouch_Left_Thumbstick_Left, EKeys::OculusTouch_Left_Thumbstick_Right,
		},
		{
			EKeys::OculusTouch_Right_A_Click, EKeys::OculusTouch_Right_B_Click,
			EKeys::OculusTouch_Right_A_Touch, EKeys::OculusTouch_Right_B_Touch, FKey(), // The right menu button is the system button.
			EKeys::OculusTouch_Right_Trigger_Click, EKeys::OculusTouch_Right_Trigger_Touch, EKeys::OculusTouch_Right_Grip_Click,
			EKeys::OculusTouch_Right_Thumbstick_Click, EKeys::OculusTouch_Right_Thumbstick_Touch, RightThumbRest,
			EKeys::OculusTouch_Right_Thumbstick_Up, EKeys::OculusTouch_Right_Thumbstick_Down,
			EKeys::OculusTouch_Right_Thumbstick_Left, EKeys::OculusTouch_Right_Thumbstick_Right,
		},
	};
	const FKey AxisKeys[2][NumAxes] = {
		{ EKeys::OculusTouch_Left_Trigger_Axis, EKeys::OculusTouch_Left_Grip_Axis, EKeys::OculusTouch_Left_Thumbstick_X, EKeys::OculusTouch_Left_Thumbstick_Y },
		{ EKeys::OculusTouch_Right_Trigger_Axis, EKeys::OculusTouch_Right_Grip_Axis, EKeys::OculusTouch_Right_Thumbstick_X, EKeys::OculusTouch_Right_Thumbstick_Y },
	};
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		for (const FKey& Key : Keys[Hand])
		{
			Hands[Hand].Buttons.Add({ Key, false });
		}
		for (const FKey& Key : AxisKeys[Hand])
		{
			Hands[Hand].Axes.Add({ Key, 0.f });
		}
	}

	IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
}

FTetherXRInput::~FTetherXRInput()
{
	IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
}

void FTetherXRInput::RegisterKeys()
{
	auto AddIfMissing = [](const FKey& Key, const FText& DisplayName)
	{
		if (!EKeys::GetKeyDetails(Key).IsValid())
		{
			EKeys::AddKey(FKeyDetails(Key, DisplayName, FKeyDetails::GamepadKey | FKeyDetails::NotBlueprintBindableKey, TEXT("OculusTouch")));
		}
	};
	AddIfMissing(LeftThumbRest, LOCTEXT("OculusTouch_Left_ThumbRest", "Oculus Touch (L) Thumb Rest"));
	AddIfMissing(RightThumbRest, LOCTEXT("OculusTouch_Right_ThumbRest", "Oculus Touch (R) Thumb Rest"));
}

FTetherXRHMD* FTetherXRInput::GetHMD()
{
	if (GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == FTetherXRHMD::SystemName)
	{
		return static_cast<FTetherXRHMD*>(GEngine->XRSystem.Get());
	}
	return nullptr;
}

void FTetherXRInput::SetButton(FButton& Button, bool bPressed)
{
	if (Button.bPressed == bPressed || !Button.Key.IsValid())
	{
		return;
	}
	Button.bPressed = bPressed;
	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	if (bPressed)
	{
		MessageHandler->OnControllerButtonPressed(Button.Key.GetFName(), User, Device, false);
	}
	else
	{
		MessageHandler->OnControllerButtonReleased(Button.Key.GetFName(), User, Device, false);
	}
}

void FTetherXRInput::SetAxis(FAxis& Axis, float Value)
{
	if (FMath::IsNearlyEqual(Axis.Value, Value, 1e-4f))
	{
		return;
	}
	Axis.Value = Value;
	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	MessageHandler->OnControllerAnalog(Axis.Key.GetFName(), User, Device, Value);
}

void FTetherXRInput::ReleaseAll()
{
	for (FHand& Hand : Hands)
	{
		for (FButton& Button : Hand.Buttons)
		{
			SetButton(Button, false);
		}
		for (FAxis& Axis : Hand.Axes)
		{
			SetAxis(Axis, 0.f);
		}
	}
}

void FTetherXRInput::SendControllerEvents()
{
	// Only while something renders to the headset (VR Preview, -vr): the editor UI shouldn't see Touch input.
	const FTetherXRHMD* HMD = GetHMD();
	tether_xr_state State;
	const bool bLive = HMD && HMD->IsStereoEnabled() && FTetherRuntime::Get().ReadState(State) && State.streaming == 1;
	if (!bLive)
	{
		if (bActive)
		{
			ReleaseAll();
			bActive = false;
		}
		return;
	}
	bActive = true;

	for (int32 HandIndex = 0; HandIndex < 2; ++HandIndex)
	{
		FHand& Hand = Hands[HandIndex];
		const qlh_controller& C = State.controllers[HandIndex];
		if (!C.valid)
		{
			for (FButton& Button : Hand.Buttons) { SetButton(Button, false); }
			for (FAxis& Axis : Hand.Axes) { SetAxis(Axis, 0.f); }
			continue;
		}

		// Same physical mapping as Tether's OpenXR runtime (Input.swift).
		const bool bLeft = HandIndex == 0;
		const uint32 LowerBit = bLeft ? QLH_BTN_X : QLH_BTN_A;
		const uint32 UpperBit = bLeft ? QLH_BTN_Y : QLH_BTN_B;
		const uint32 StickBit = bLeft ? QLH_BTN_STICK_L : QLH_BTN_STICK_R;
		TArray<FButton>& B = Hand.Buttons;

		SetButton(B[FaceLowerClick], (C.buttons & LowerBit) != 0);
		SetButton(B[FaceUpperClick], (C.buttons & UpperBit) != 0);
		SetButton(B[FaceLowerTouch], (C.touches & TouchFaceLower) != 0);
		SetButton(B[FaceUpperTouch], (C.touches & TouchFaceUpper) != 0);
		SetButton(B[Menu], bLeft && (C.buttons & QLH_BTN_MENU) != 0);
		SetButton(B[TriggerClick], Hysteresis(C.trigger, B[TriggerClick].bPressed, 0.55f, 0.45f));
		SetButton(B[TriggerTouch], (C.touches & TouchTrigger) != 0 || C.trigger > 0.05f);
		SetButton(B[GripClick], Hysteresis(C.grip, B[GripClick].bPressed, 0.55f, 0.45f));
		SetButton(B[ThumbstickClick], (C.buttons & StickBit) != 0);
		SetButton(B[ThumbstickTouch], (C.touches & TouchThumbstick) != 0);
		SetButton(B[ThumbRest], (C.touches & TouchThumbRest) != 0);
		SetButton(B[ThumbstickUp], Hysteresis(C.stick_y, B[ThumbstickUp].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickDown], Hysteresis(-C.stick_y, B[ThumbstickDown].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickLeft], Hysteresis(-C.stick_x, B[ThumbstickLeft].bPressed, 0.7f, 0.6f));
		SetButton(B[ThumbstickRight], Hysteresis(C.stick_x, B[ThumbstickRight].bPressed, 0.7f, 0.6f));

		SetAxis(Hand.Axes[TriggerAxis], C.trigger);
		SetAxis(Hand.Axes[GripAxis], C.grip);
		SetAxis(Hand.Axes[ThumbstickX], C.stick_x);
		SetAxis(Hand.Axes[ThumbstickY], C.stick_y);
	}
}

FName FTetherXRInput::GetMotionControllerDeviceTypeName() const
{
	return FTetherXRHMD::SystemName;
}

bool FTetherXRInput::GetControllerOrientationAndPosition(const int32 ControllerIndex, const FName MotionSource, FRotator& OutOrientation, FVector& OutPosition, float WorldToMetersScale) const
{
	int32 Hand = 0;
	bool bAim = false;
	const FTetherXRHMD* HMD = GetHMD();
	FQuat Orientation;
	if (ControllerIndex != 0 || !HMD || !ResolveSource(MotionSource, Hand, bAim) ||
		!HMD->GetControllerPose(Hand, bAim, WorldToMetersScale, Orientation, OutPosition))
	{
		return false;
	}
	OutOrientation = Orientation.Rotator();
	return true;
}

ETrackingStatus FTetherXRInput::GetControllerTrackingStatus(const int32 ControllerIndex, const FName MotionSource) const
{
	int32 Hand = 0;
	bool bAim = false;
	const FTetherXRHMD* HMD = GetHMD();
	const bool bTracked = ControllerIndex == 0 && HMD && ResolveSource(MotionSource, Hand, bAim) && HMD->IsControllerTracked(Hand);
	return bTracked ? ETrackingStatus::Tracked : ETrackingStatus::NotTracked;
}

void FTetherXRInput::EnumerateSources(TArray<FMotionControllerSource>& SourcesOut) const
{
	FXRMotionControllerBase::EnumerateSources(SourcesOut);
	for (const FName& Source : { LeftGripSource, RightGripSource, LeftAimSource, RightAimSource })
	{
		if (!SourcesOut.ContainsByPredicate([&Source](const FMotionControllerSource& Existing) { return Existing.SourceName == Source; }))
		{
			SourcesOut.Add(FMotionControllerSource(Source));
		}
	}
}

#undef LOCTEXT_NAMESPACE
