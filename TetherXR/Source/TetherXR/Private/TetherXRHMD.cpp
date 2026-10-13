// Copyright (c) 2026 Tether contributors. MIT License.

#include "TetherXRHMD.h"

#include "DefaultSpectatorScreenController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/App.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "SceneView.h"
#include "TetherRuntime.h"
#include "TetherXRRenderBridge.h"

#if TETHERXR_UE_5_8_OR_NEWER
#include "Framework/Application/SlateApplication.h"
#include "RenderGraphBuilder.h"
#include "Rendering/SlateRenderer.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SWindow.h"
#else
#include "CommonRenderResources.h"
#include "EngineModule.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RendererInterface.h"
#include "RHIStaticStates.h"
#include "ScreenRendering.h"
#include "ShaderParameterUtils.h"
#endif

const FName FTetherXRHMD::SystemName(TEXT("TetherXR"));

namespace
{
	constexpr int32 NumBufferedTargets = 3;

#if TETHERXR_UE_5_8_OR_NEWER
	constexpr EHMDTrackingOrigin::Type DefaultTrackingOrigin = EHMDTrackingOrigin::LocalFloor;
#else
	constexpr EHMDTrackingOrigin::Type DefaultTrackingOrigin = EHMDTrackingOrigin::Floor;
#endif

	int32 EyeIndex(int32 ViewIndex)
	{
		return ViewIndex == EStereoscopicEye::eSSE_RIGHT_EYE ? 1 : 0;
	}
}

void FTetherTrackingFrame::ToUnreal(const FTetherPose& Pose, FQuat& OutOrientation, FVector& OutPosition) const
{
	const FTetherPose Relative = Origin.Inverse() * Pose;
	OutOrientation = TetherXR::ToUnrealRotation(Relative.Orientation);
	OutPosition = TetherXR::ToUnrealPosition(Relative.Position, WorldToMeters);
}

FTetherXRHMD::FTetherXRHMD()
	: FHeadMountedDisplayBase(nullptr)
	, TrackingOrigin(DefaultTrackingOrigin)
{
	SpectatorScreenController = MakeUnique<FDefaultSpectatorScreenController>(this);

	if (RHIGetInterfaceType() == ERHIInterfaceType::Metal)
	{
		RenderBridge = new FTetherXRRenderBridge();
	}
	else
	{
		UE_LOG(LogTetherXR, Log, TEXT("The RHI isn't Metal; TetherXR tracks but can't send frames"));
	}

#if TETHERXR_UE_5_8_OR_NEWER
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(this, &FTetherXRHMD::OnBackBufferReadyToPresent);
	}
#endif

	UpdateGameFrame();
	RenderFrame = GameFrame;
}

FTetherXRHMD::~FTetherXRHMD()
{
#if TETHERXR_UE_5_8_OR_NEWER
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().RemoveAll(this);
	}
#endif
}

FString FTetherXRHMD::GetVersionString() const
{
	return FString::Printf(TEXT("TetherXR - %s"), *FTetherRuntime::Get().GetLibraryPath());
}

// ---------------------------------------------------------------------------------------------------------------
// Tracking

const FTetherTrackingFrame& FTetherXRHMD::CurrentFrame() const
{
	return IsInRenderingThread() ? RenderFrame : GameFrame;
}

bool FTetherXRHMD::IsFloorOrigin() const
{
#if TETHERXR_UE_5_8_OR_NEWER
	return TrackingOrigin == EHMDTrackingOrigin::LocalFloor || TrackingOrigin == EHMDTrackingOrigin::Stage;
#else
	return TrackingOrigin == EHMDTrackingOrigin::Floor || TrackingOrigin == EHMDTrackingOrigin::Stage;
#endif
}

void FTetherXRHMD::FillFrame(FTetherTrackingFrame& Frame, const tether_xr_state& State, bool bLive) const
{
	// The display description outlives the session (Tether keeps it while idle).
	if (State.ipd > 0.f)
	{
		Frame.Ipd = State.ipd;
	}
	if (State.eye_width > 0 && State.eye_height > 0)
	{
		Frame.EyeSize = FIntPoint(State.eye_width, State.eye_height);
	}
	if (State.fov_tangents[0][0] > 0.f)
	{
		FMemory::Memcpy(Frame.FovTangents, State.fov_tangents, sizeof(Frame.FovTangents));
	}

	Frame.bStreaming = bLive;
	Frame.bHeadValid = bLive;
	if (bLive)
	{
		Frame.RawHead = State.head;
		Frame.Head = FTetherPose(State.head);
	}
	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		const qlh_controller& Controller = State.controllers[Hand];
		Frame.bControllerValid[Hand] = bLive && Controller.valid && Controller.pose_valid;
		if (Frame.bControllerValid[Hand])
		{
			Frame.Controllers[Hand] = FTetherPose(Controller.pose);
		}
	}
}

void FTetherXRHMD::UpdateGameFrame()
{
	check(IsInGameThread());

	tether_xr_state State;
	const bool bRead = FTetherRuntime::Get().ReadState(State);
	const bool bLive = bRead && State.streaming == 1;

	if (bLive != bWasStreaming)
	{
		bWasStreaming = bLive;
		if (bLive)
		{
			UE_LOG(LogTetherXR, Log, TEXT("Headset streaming through Tether: %ux%u per eye, %.0f Hz, IPD %.1f mm"),
				State.eye_width, State.eye_height, State.refresh_hz, State.ipd * 1000.f);
			PendingRecenter = ERecenter::Full; // A new Link session: anchor to where the user is now.
			PendingYaw = 0.f;
		}
		else
		{
			UE_LOG(LogTetherXR, Log, TEXT("Headset stopped streaming (%s)"), bRead ? TEXT("Link session ended") : TEXT("Tether isn't running"));
		}
	}

	if (bLive)
	{
		// The user held the Meta button: Tether bumps recenter_count.
		if (bLocalOriginSet && State.recenter_count != LastRecenterCount)
		{
			PendingRecenter = ERecenter::Full;
			PendingYaw = 0.f;
		}
		LastRecenterCount = State.recenter_count;
		if (!bLocalOriginSet)
		{
			PendingRecenter = ERecenter::Full;
		}

		if (PendingRecenter != ERecenter::None)
		{
			// LOCAL is anchored at the head's heading (yaw only) and position, like Tether's OpenXR runtime.
			const FTetherPose Head(State.head);
			const FQuat Heading = TetherXR::YawOnly(Head.Orientation, FMath::DegreesToRadians(PendingYaw));
			if (PendingRecenter != ERecenter::Position)
			{
				LocalOrigin.Orientation = Heading;
			}
			if (PendingRecenter != ERecenter::Orientation)
			{
				LocalOrigin.Position = Head.Position;
			}
			bLocalOriginSet = true;
			PendingRecenter = ERecenter::None;
			OnTrackingOriginChanged();
		}

		if (State.floor_height > 0.f)
		{
			FloorHeight = State.floor_height;
		}
	}

	FTetherTrackingFrame Frame = GameFrame;
	if (bRead)
	{
		FillFrame(Frame, State, bLive);
	}
	else
	{
		Frame.bStreaming = Frame.bHeadValid = false;
		Frame.bControllerValid[0] = Frame.bControllerValid[1] = false;
	}

	// Floor-level origins sit floor_height below LOCAL, as Tether's STAGE / LOCAL_FLOOR spaces do.
	Frame.Origin = LocalOrigin;
	if (IsFloorOrigin())
	{
		Frame.Origin.Position.Y -= FloorHeight;
	}
	Frame.WorldToMeters = WorldToMeters;
	GameFrame = Frame;
}

bool FTetherXRHMD::OnStartGameFrame(FWorldContext& WorldContext)
{
	if (const UWorld* World = WorldContext.World())
	{
		if (const AWorldSettings* Settings = World->GetWorldSettings())
		{
			WorldToMeters = Settings->WorldToMeters;
		}
	}
	RefreshTrackingToWorldTransform(WorldContext);
	UpdateGameFrame();
	return true;
}

bool FTetherXRHMD::HasValidTrackingPosition()
{
	return CurrentFrame().bHeadValid;
}

bool FTetherXRHMD::EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type)
{
	if (Type == EXRTrackedDeviceType::Any || Type == EXRTrackedDeviceType::HeadMountedDisplay)
	{
		OutDevices.Add(IXRTrackingSystem::HMDDeviceId);
		return true;
	}
	return false;
}

bool FTetherXRHMD::GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition)
{
	OutOrientation = FQuat::Identity;
	OutPosition = FVector::ZeroVector;
	const FTetherTrackingFrame& Frame = CurrentFrame();
	if (DeviceId != IXRTrackingSystem::HMDDeviceId || !Frame.bHeadValid)
	{
		return false;
	}
	Frame.ToUnreal(Frame.Head, OutOrientation, OutPosition);
	return true;
}

bool FTetherXRHMD::GetControllerPose(int32 Hand, bool bAim, float InWorldToMeters, FQuat& OutOrientation, FVector& OutPosition) const
{
	const FTetherTrackingFrame& Frame = CurrentFrame();
	if (Hand < 0 || Hand > 1 || !Frame.bControllerValid[Hand])
	{
		return false;
	}
	const FTetherPose Grip = Frame.Controllers[Hand];
	const FTetherPose Relative = Frame.Origin.Inverse() * (bAim ? TetherXR::AimFromGrip(Grip) : Grip);
	OutOrientation = TetherXR::ToUnrealRotation(Relative.Orientation);
	OutPosition = TetherXR::ToUnrealPosition(Relative.Position, InWorldToMeters);
	return true;
}

bool FTetherXRHMD::IsControllerTracked(int32 Hand) const
{
	return Hand >= 0 && Hand < 2 && CurrentFrame().bControllerValid[Hand];
}

bool FTetherXRHMD::GetRelativeEyePose(int32 DeviceId, int32 ViewIndex, FQuat& OutOrientation, FVector& OutPosition)
{
	OutOrientation = FQuat::Identity;
	OutPosition = FVector::ZeroVector;
	if (DeviceId != IXRTrackingSystem::HMDDeviceId ||
		(ViewIndex != EStereoscopicEye::eSSE_LEFT_EYE && ViewIndex != EStereoscopicEye::eSSE_RIGHT_EYE))
	{
		return false;
	}
	// Eyes sit on the head's X axis (Unreal Y), half the IPD each side.
	const FTetherTrackingFrame& Frame = CurrentFrame();
	const float Side = ViewIndex == EStereoscopicEye::eSSE_LEFT_EYE ? -0.5f : 0.5f;
	OutPosition = FVector(0, Side * Frame.Ipd * Frame.WorldToMeters, 0);
	return true;
}

float FTetherXRHMD::GetWorldToMetersScale() const
{
	return CurrentFrame().WorldToMeters;
}

void FTetherXRHMD::SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin)
{
	if (TrackingOrigin != NewOrigin)
	{
		TrackingOrigin = NewOrigin;
		UpdateGameFrame();
		OnTrackingOriginChanged();
	}
}

bool FTetherXRHMD::GetFloorToEyeTrackingTransform(FTransform& OutFloorToEye) const
{
	OutFloorToEye = FTransform(FVector(0, 0, FloorHeight * GameFrame.WorldToMeters));
	return true;
}

void FTetherXRHMD::ResetOrientationAndPosition(float Yaw)
{
	PendingRecenter = ERecenter::Full;
	PendingYaw = Yaw;
	UpdateGameFrame();
}

void FTetherXRHMD::ResetOrientation(float Yaw)
{
	PendingRecenter = ERecenter::Orientation;
	PendingYaw = Yaw;
	UpdateGameFrame();
}

void FTetherXRHMD::ResetPosition()
{
	PendingRecenter = ERecenter::Position;
	PendingYaw = 0.f;
	UpdateGameFrame();
}

// ---------------------------------------------------------------------------------------------------------------
// Frame pipeline: game thread -> render thread (late update) -> RHI thread (present)

#if TETHERXR_UE_5_8_OR_NEWER
void FTetherXRHMD::OnBeginRendering_GameThread(FSceneViewFamily& InViewFamily)
{
	SpectatorScreenController->BeginRenderViewFamily(InViewFamily);
#else
void FTetherXRHMD::OnBeginRendering_GameThread()
{
	SpectatorScreenController->BeginRenderViewFamily();
#endif
	ENQUEUE_RENDER_COMMAND(TetherXRBeginFrame)(
		[this, Frame = GameFrame, Texture = AcquiredTexture_GameThread](FRHICommandListImmediate&)
		{
			RenderFrame = Frame;
			AcquiredTexture_RenderThread = Texture;
		});
}

#if TETHERXR_UE_5_8_OR_NEWER
void FTetherXRHMD::OnBeginRendering_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
#else
void FTetherXRHMD::OnBeginRendering_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& ViewFamily)
{
	SpectatorScreenController->UpdateSpectatorScreenMode_RenderThread();
#endif
	// Late update: the newest head pose, relative to the origin the game thread used for this frame.
	tether_xr_state State;
	if (FTetherRuntime::Get().ReadState(State) && State.streaming == 1)
	{
		FillFrame(RenderFrame, State, true);
	}
	QueueSubmit_RenderThread(RHICmdList);
}

void FTetherXRHMD::QueueSubmit_RenderThread(FRHICommandListImmediate& RHICmdList)
{
	check(IsInRenderingThread());
	if (!RenderBridge.IsValid() || !RenderFrame.bHeadValid || !RenderTextures.IsValidIndex(AcquiredTexture_RenderThread))
	{
		return;
	}

	FTetherSubmitFrame Frame;
	Frame.Texture = RenderTextures[AcquiredTexture_RenderThread];
	Frame.Pose = RenderFrame.RawHead; // The exact pose every view of this frame is rendered from.
	RHICmdList.EnqueueLambda(TEXT("TetherXRQueueFrame"),
		[Bridge = RenderBridge, Frame = MoveTemp(Frame)](FRHICommandListImmediate&) mutable
		{
			Bridge->SetFrame_RHIThread(MoveTemp(Frame));
		});
}

#if TETHERXR_UE_5_8_OR_NEWER
void FTetherXRHMD::OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider)
{
	// Hook the present of whichever window shows the stereo viewport (the VR Preview / -vr game window).
	const TSharedPtr<ISlateViewport> Viewport = Window.GetViewport();
	if (RenderBridge.IsValid() && Viewport.IsValid() && Viewport->IsStereoscopic3D())
	{
		ViewportProvider.SetCustomPresent(RenderBridge);
	}
}
#else
FXRRenderBridge* FTetherXRHMD::GetActiveRenderBridge_GameThread(bool bUseSeparateRenderTarget)
{
	return bUseSeparateRenderTarget ? RenderBridge.GetReference() : nullptr;
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Display

bool FTetherXRHMD::IsHMDConnected()
{
	// Tether is running (it publishes state even while the headset is idle).
	tether_xr_state State;
	return FTetherRuntime::Get().ReadState(State);
}

void FTetherXRHMD::EnableHMD(bool bEnable)
{
	bHMDEnabled = bEnable;
	if (!bEnable)
	{
		EnableStereo(false);
	}
}

bool FTetherXRHMD::GetHMDMonitorInfo(MonitorInfo& OutInfo)
{
	const FIntPoint Eye = GameFrame.EyeSize;
	OutInfo.MonitorName = TEXT("Tether");
	OutInfo.MonitorId = 0;
	OutInfo.DesktopX = OutInfo.DesktopY = 0;
	OutInfo.ResolutionX = OutInfo.WindowSizeX = Eye.X * 2;
	OutInfo.ResolutionY = OutInfo.WindowSizeY = Eye.Y;
	return true;
}

void FTetherXRHMD::GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const
{
	const float* T = CurrentFrame().FovTangents[0];
	OutHFOVInDegrees = FMath::RadiansToDegrees(FMath::Atan(T[0]) + FMath::Atan(T[1]));
	OutVFOVInDegrees = FMath::RadiansToDegrees(FMath::Atan(T[2]) + FMath::Atan(T[3]));
}

float FTetherXRHMD::GetInterpupillaryDistance() const
{
	return CurrentFrame().Ipd;
}

void FTetherXRHMD::SetPixelDensity(const float NewDensity)
{
	PixelDensity = FMath::Clamp(NewDensity, PixelDensityMin, PixelDensityMax);
}

FIntPoint FTetherXRHMD::GetIdealRenderTargetSize() const
{
	const FIntPoint Eye = CurrentFrame().EyeSize;
	return FIntPoint(Eye.X * 2, Eye.Y);
}

bool FTetherXRHMD::EnableStereo(bool bStereo)
{
	bStereo = bStereo && bHMDEnabled;
	if (bStereo == bStereoEnabled)
	{
		return bStereoEnabled;
	}
	bStereoEnabled = bStereo;

	if (bStereo)
	{
		// Like a new OpenXR session in Tether's runtime: anchor LOCAL to where the user is now.
		PendingRecenter = ERecenter::Full;
		PendingYaw = 0.f;
		UpdateGameFrame();
		UE_CLOG(!GameFrame.bStreaming, LogTetherXR, Warning, TEXT("Stereo enabled but the headset isn't streaming; start Link from Tether"));
	}
	UE_LOG(LogTetherXR, Log, TEXT("Stereo %s"), bStereo ? TEXT("enabled") : TEXT("disabled"));

	FApp::SetUseVRFocus(bStereo);
	FApp::SetHasVRFocus(bStereo);
	return bStereoEnabled;
}

void FTetherXRHMD::AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	// Eyes side by side in the render target: left half, right half.
	const FIntPoint Ideal = GetIdealRenderTargetSize();
	const uint32 EyeWidth = FMath::CeilToInt(Ideal.X * PixelDensity) / 2;
	const uint32 EyeHeight = FMath::CeilToInt(Ideal.Y * PixelDensity);
	X = EyeIndex(ViewIndex) * EyeWidth;
	Y = 0;
	SizeX = EyeWidth;
	SizeY = EyeHeight;
}

FMatrix FTetherXRHMD::GetStereoProjectionMatrix(const int32 ViewIndex) const
{
	// Off-axis, reversed-Z, infinite far plane from the eye's FOV tangents (left, right, up, down).
	const float* T = CurrentFrame().FovTangents[EyeIndex(ViewIndex)];
	const float Left = -T[0];
	const float Right = T[1];
	const float Up = T[2];
	const float Down = -T[3];
	const float ZNear = IsInRenderingThread() ? GNearClippingPlane_RenderThread : GNearClippingPlane;

	return FMatrix(
		FPlane(2.f / (Right - Left), 0.f, 0.f, 0.f),
		FPlane(0.f, 2.f / (Up - Down), 0.f, 0.f),
		FPlane(-(Right + Left) / (Right - Left), -(Up + Down) / (Up - Down), 0.f, 1.f),
		FPlane(0.f, 0.f, ZNear, 0.f));
}

// ---------------------------------------------------------------------------------------------------------------
// Spectator (the Mac window)

#if TETHERXR_UE_5_8_OR_NEWER

void FTetherXRHMD::RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer, FRDGTextureRef SrcTexture, FVector2f WindowSize) const
{
	if (SpectatorScreenController)
	{
		SpectatorScreenController->RenderSpectatorScreen_RenderThread(GraphBuilder, BackBuffer, SrcTexture, nullptr, WindowSize);
	}
}

FIntRect FTetherXRHMD::GetFullFlatEyeRect_RenderThread(const FRHITextureDesc& EyeTexture) const
{
	return FIntRect(0, 0, EyeTexture.Extent.X / 2, EyeTexture.Extent.Y);
}

#else

void FTetherXRHMD::RenderTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer, FRHITexture* SrcTexture, FVector2D WindowSize) const
{
	if (SpectatorScreenController)
	{
		SpectatorScreenController->RenderSpectatorScreen_RenderThread(RHICmdList, BackBuffer, SrcTexture, WindowSize);
	}
}

FIntRect FTetherXRHMD::GetFullFlatEyeRect_RenderThread(FTexture2DRHIRef EyeTexture) const
{
	return FIntRect(0, 0, EyeTexture->GetSizeX() / 2, EyeTexture->GetSizeY());
}

void FTetherXRHMD::CopyTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* SrcTexture, FIntRect SrcRect, FRHITexture2D* DstTexture, FIntRect DstRect, bool bClearBlack, bool bNoAlpha) const
{
	check(IsInRenderingThread());
	const FIntPoint SrcSize = SrcTexture->GetSizeXY();
	const FIntPoint DstSize = DstTexture->GetSizeXY();
	if (SrcRect.IsEmpty())
	{
		SrcRect = FIntRect(FIntPoint::ZeroValue, SrcSize);
	}
	if (DstRect.IsEmpty())
	{
		DstRect = FIntRect(FIntPoint::ZeroValue, DstSize);
	}

	RHICmdList.Transition(FRHITransitionInfo(DstTexture, ERHIAccess::Unknown, ERHIAccess::RTV));
	FRHIRenderPassInfo PassInfo(DstTexture, bClearBlack ? ERenderTargetActions::Clear_Store : ERenderTargetActions::Load_Store);
	RHICmdList.BeginRenderPass(PassInfo, TEXT("TetherXRSpectator"));
	{
		RHICmdList.SetViewport(DstRect.Min.X, DstRect.Min.Y, 0.f, DstRect.Max.X, DstRect.Max.Y, 1.f);

		FGraphicsPipelineStateInitializer PipelineState;
		RHICmdList.ApplyCachedRenderTargets(PipelineState);
		PipelineState.BlendState = bNoAlpha ? TStaticBlendState<CW_RGB>::GetRHI() : TStaticBlendState<>::GetRHI();
		PipelineState.RasterizerState = TStaticRasterizerState<>::GetRHI();
		PipelineState.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		PipelineState.PrimitiveType = PT_TriangleList;

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
		TShaderMapRef<FScreenPS> PixelShader(ShaderMap);
		PipelineState.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		PipelineState.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		PipelineState.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		SetGraphicsPipelineState(RHICmdList, PipelineState, 0);
		SetShaderParametersLegacyPS(RHICmdList, PixelShader, TStaticSamplerState<SF_Bilinear>::GetRHI(), SrcTexture);

		GetRendererModule().DrawRectangle(RHICmdList,
			0, 0, DstRect.Width(), DstRect.Height(),
			SrcRect.Min.X / float(SrcSize.X), SrcRect.Min.Y / float(SrcSize.Y),
			SrcRect.Width() / float(SrcSize.X), SrcRect.Height() / float(SrcSize.Y),
			DstRect.Size(), FIntPoint(1, 1), VertexShader, EDRF_Default);
	}
	RHICmdList.EndRenderPass();
}

#endif

// ---------------------------------------------------------------------------------------------------------------
// Render targets: allocated here so each frame's texture is known when it's presented.

int32 FTetherXRHMD::AcquireColorTexture()
{
	check(IsInGameThread());
	const int32 Count = NumRenderTextures.load();
	if (Count <= 0)
	{
		AcquiredTexture_GameThread = INDEX_NONE;
		return INDEX_NONE;
	}
	AcquiredTexture_GameThread = (AcquiredTexture_GameThread + 1) % Count;
	return AcquiredTexture_GameThread;
}

#if TETHERXR_UE_5_8_OR_NEWER
bool FTetherXRHMD::AllocateRenderTargetTextures(FRHICommandListBase& RHICmdList, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTextureRHIRef>& OutTargetableTextures, TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples)
#else
bool FTetherXRHMD::AllocateRenderTargetTextures(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTexture2DRHIRef>& OutTargetableTextures, TArray<FTexture2DRHIRef>& OutShaderResourceTextures, uint32 NumSamples)
#endif
{
	check(IsInRenderingThread());
	RenderTextures.Reset();
	NumRenderTextures = 0;
	if (NumLayers != 1)
	{
		return false; // Side by side only; let the engine allocate (nothing is sent to the headset).
	}

	// 8-bit BGRA holds the tonemapped (sRGB-encoded) output Tether encodes as-is.
	const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("TetherXREyes"), SizeX, SizeY, PF_B8G8R8A8)
		.SetFlags(Flags | TargetableTextureFlags | ETextureCreateFlags::ShaderResource)
		.SetClearValue(FClearValueBinding::Black)
		.SetInitialState(ERHIAccess::SRVMask);

	for (int32 Index = 0; Index < NumBufferedTargets; ++Index)
	{
#if TETHERXR_UE_5_8_OR_NEWER
		FTextureRHIRef Texture = RHICmdList.CreateTexture(Desc);
#else
		FTextureRHIRef Texture = RHICreateTexture(Desc);
#endif
		RenderTextures.Add(Texture);
		OutTargetableTextures.Add(Texture);
		OutShaderResourceTextures.Add(Texture);
	}
	NumRenderTextures = RenderTextures.Num();
	UE_LOG(LogTetherXR, Log, TEXT("Allocated %d eye targets of %ux%u"), NumBufferedTargets, SizeX, SizeY);
	return true;
}
