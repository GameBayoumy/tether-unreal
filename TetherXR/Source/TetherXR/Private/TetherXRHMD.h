// Copyright (c) 2026 Tether contributors. MIT License.

#pragma once

#include "TetherXRCommon.h"
#include "HeadMountedDisplayBase.h"
#include "XRRenderTargetManager.h"

class FTetherXRRenderBridge;
class ISlateViewportProvider;
class SWindow;

/** Everything one frame (game or render thread) needs about the headset. */
struct FTetherTrackingFrame
{
	bool bStreaming = false;
	bool bHeadValid = false;
	/** Head pose exactly as Tether published it (tracking space); submitted with the frame. */
	qlh_pose RawHead{};
	FTetherPose Head;
	FTetherPose Controllers[2];
	bool bControllerValid[2] = { false, false };
	/** Tracking-space origin the engine sees: the recentred heading/position, lowered to the floor if requested. */
	FTetherPose Origin;
	float WorldToMeters = 100.f;
	float Ipd = 0.063f;
	/** Per eye: left, right, up, down (positive tangents). */
	float FovTangents[2][4] = { { 1.376f, 0.839f, 0.966f, 1.428f }, { 0.839f, 1.376f, 0.966f, 1.428f } };
	FIntPoint EyeSize = FIntPoint(1808, 1920);

	/** A tracked pose relative to Origin, in Unreal tracking space. */
	void ToUnreal(const FTetherPose& Pose, FQuat& OutOrientation, FVector& OutPosition) const;
};

/**
 * Quest headset through Tether: an XRBase head-mounted display that renders both eyes side by side into its
 * own render targets and hands each finished frame to Tether (see FTetherXRRenderBridge).
 */
class FTetherXRHMD
	: public FHeadMountedDisplayBase
	, public FXRRenderTargetManager
	, public TSharedFromThis<FTetherXRHMD, ESPMode::ThreadSafe>
{
public:
	static const FName SystemName;

	FTetherXRHMD();
	virtual ~FTetherXRHMD() override;

	/** Pose of a controller's grip (or aim) in Unreal tracking space; picks the game or render thread frame. */
	bool GetControllerPose(int32 Hand, bool bAim, float WorldToMeters, FQuat& OutOrientation, FVector& OutPosition) const;
	bool IsControllerTracked(int32 Hand) const;

	// IXRSystemIdentifier / IXRTrackingSystem
	virtual FName GetSystemName() const override { return SystemName; }
	virtual int32 GetXRSystemFlags() const override { return EXRSystemFlags::IsHeadMounted; }
	virtual FString GetVersionString() const override;
	virtual bool DoesSupportPositionalTracking() const override { return true; }
	virtual bool HasValidTrackingPosition() override;
	virtual bool EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type = EXRTrackedDeviceType::Any) override;
	virtual bool GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual bool GetRelativeEyePose(int32 DeviceId, int32 ViewIndex, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual float GetWorldToMetersScale() const override;
	virtual void SetTrackingOrigin(EHMDTrackingOrigin::Type NewOrigin) override;
	virtual EHMDTrackingOrigin::Type GetTrackingOrigin() const override { return TrackingOrigin; }
	virtual bool GetFloorToEyeTrackingTransform(FTransform& OutFloorToEye) const override;
	virtual void ResetOrientationAndPosition(float Yaw = 0.f) override;
	virtual void ResetOrientation(float Yaw = 0.f) override;
	virtual void ResetPosition() override;
	virtual IHeadMountedDisplay* GetHMDDevice() override { return this; }
	virtual TSharedPtr<IStereoRendering, ESPMode::ThreadSafe> GetStereoRenderingDevice() override { return SharedThis(this); }
	virtual bool OnStartGameFrame(FWorldContext& WorldContext) override;
#if TETHERXR_UE_5_8_OR_NEWER
	virtual void OnBeginRendering_GameThread(FSceneViewFamily& InViewFamily) override;
	virtual void OnBeginRendering_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& ViewFamily) override;
#else
	virtual void OnBeginRendering_GameThread() override;
	virtual void OnBeginRendering_RenderThread(FRHICommandListImmediate& RHICmdList, FSceneViewFamily& ViewFamily) override;
#endif

	// IHeadMountedDisplay
	virtual bool IsHMDConnected() override;
	virtual bool IsHMDEnabled() const override { return bHMDEnabled; }
	virtual void EnableHMD(bool bEnable = true) override;
	virtual bool GetHMDMonitorInfo(MonitorInfo& OutInfo) override;
	virtual void GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const override;
	virtual void SetInterpupillaryDistance(float NewInterpupillaryDistance) override {}
	virtual float GetInterpupillaryDistance() const override;
	virtual bool IsChromaAbCorrectionEnabled() const override { return false; }
	virtual float GetPixelDenity() const override { return PixelDensity; }
	virtual void SetPixelDensity(const float NewDensity) override;
	virtual FIntPoint GetIdealRenderTargetSize() const override;

	// IStereoRendering
	virtual bool IsStereoEnabled() const override { return bStereoEnabled; }
	virtual bool EnableStereo(bool bStereo = true) override;
	virtual void AdjustViewRect(const int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const override;
	virtual FMatrix GetStereoProjectionMatrix(const int32 ViewIndex) const override;
	virtual IStereoRenderTargetManager* GetRenderTargetManager() override { return this; }
#if TETHERXR_UE_5_8_OR_NEWER
	virtual void RenderTexture_RenderThread(FRDGBuilder& GraphBuilder, FRDGTextureRef BackBuffer, FRDGTextureRef SrcTexture, FVector2f WindowSize) const override;
	virtual FIntRect GetFullFlatEyeRect_RenderThread(const FRHITextureDesc& EyeTexture) const override;
#else
	virtual void RenderTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* BackBuffer, FRHITexture* SrcTexture, FVector2D WindowSize) const override;
	virtual FIntRect GetFullFlatEyeRect_RenderThread(FTexture2DRHIRef EyeTexture) const override;
	virtual void CopyTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture2D* SrcTexture, FIntRect SrcRect, FRHITexture2D* DstTexture, FIntRect DstRect, bool bClearBlack, bool bNoAlpha) const override;
#endif

	// IStereoRenderTargetManager
	virtual bool ShouldUseSeparateRenderTarget() const override { return IsStereoEnabled(); }
	virtual EPixelFormat GetActualColorSwapchainFormat() const override { return PF_B8G8R8A8; }
	virtual int32 AcquireColorTexture() override;
#if TETHERXR_UE_5_8_OR_NEWER
	virtual bool AllocateRenderTargetTextures(FRHICommandListBase& RHICmdList, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTextureRHIRef>& OutTargetableTextures, TArray<FTextureRHIRef>& OutShaderResourceTextures, uint32 NumSamples = 1) override;
#else
	virtual bool AllocateRenderTargetTextures(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumLayers, ETextureCreateFlags Flags, ETextureCreateFlags TargetableTextureFlags, TArray<FTexture2DRHIRef>& OutTargetableTextures, TArray<FTexture2DRHIRef>& OutShaderResourceTextures, uint32 NumSamples = 1) override;
protected:
	virtual FXRRenderBridge* GetActiveRenderBridge_GameThread(bool bUseSeparateRenderTarget) override;
#endif

private:
	enum class ERecenter : uint8 { None, Full, Orientation, Position };

	const FTetherTrackingFrame& CurrentFrame() const;
	bool IsFloorOrigin() const;
	/** Builds a frame from Tether's state relative to Origin. */
	void FillFrame(FTetherTrackingFrame& Frame, const tether_xr_state& State, bool bLive) const;
	/** Game thread: reads Tether, applies recentring, refreshes GameFrame. */
	void UpdateGameFrame();
	void QueueSubmit_RenderThread(FRHICommandListImmediate& RHICmdList);
#if TETHERXR_UE_5_8_OR_NEWER
	void OnBackBufferReadyToPresent(SWindow& Window, ISlateViewportProvider& ViewportProvider);
#endif

	FTetherTrackingFrame GameFrame;
	FTetherTrackingFrame RenderFrame;

	// Recentring (game thread). LocalOrigin is the eye-level origin: head heading + position at the last recenter.
	FTetherPose LocalOrigin;
	bool bLocalOriginSet = false;
	uint32 LastRecenterCount = 0;
	ERecenter PendingRecenter = ERecenter::None;
	float PendingYaw = 0.f;
	float FloorHeight = 1.6f;
	EHMDTrackingOrigin::Type TrackingOrigin;
	bool bWasStreaming = false;
	float WorldToMeters = 100.f;

	std::atomic<bool> bStereoEnabled{ false };
	bool bHMDEnabled = true;
	float PixelDensity = 1.f;

	// Render targets the engine renders both eyes into; one is acquired per frame.
	TArray<FTextureRHIRef> RenderTextures; // render thread
	std::atomic<int32> NumRenderTextures{ 0 };
	int32 AcquiredTexture_GameThread = INDEX_NONE;
	int32 AcquiredTexture_RenderThread = INDEX_NONE;

	TRefCountPtr<FTetherXRRenderBridge> RenderBridge;
};
