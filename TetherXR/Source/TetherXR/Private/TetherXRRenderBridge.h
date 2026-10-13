// Copyright (c) 2026 Tether contributors. MIT License.

#pragma once

#include "TetherXRCommon.h"
#include "XRRenderBridge.h"

/** One rendered stereo frame waiting to be handed to Tether. */
struct FTetherSubmitFrame
{
	/** Side-by-side eyes: left half, right half. */
	FTextureRHIRef Texture;
	/** Head pose the views were rendered for, in Tether's tracking space. */
	qlh_pose Pose{};
};

/**
 * Custom present on the stereo viewport: after the engine has rendered both eyes into the viewport's render
 * target, it hands that texture to Tether on the engine's own Metal command queue, then lets the normal
 * present run so the Mac window keeps showing the spectator view.
 */
class FTetherXRRenderBridge : public FXRRenderBridge
{
public:
	virtual ~FTetherXRRenderBridge() override;

	/** Queues the frame to submit at the next present. RHI thread (enqueue from the render thread). */
	void SetFrame_RHIThread(FTetherSubmitFrame&& Frame);

	virtual bool NeedsNativePresent() override { return true; }

#if TETHERXR_UE_5_8_OR_NEWER
	virtual bool Present(FRHIViewport* Viewport, IRHICommandContext& RHICmdContext, int32& InOutSyncInterval) override;
	virtual bool PresentOnSubmissionThread(int32& InOutSyncInterval) override { return true; }
#else
	virtual bool Present(int32& InOutSyncInterval) override;
	virtual void PostPresent() override;
#endif

private:
	/** Encodes the frame on `MetalCommandQueue`, which must already hold the commands that rendered it. */
	void Submit(void* MetalCommandQueue, const FTetherSubmitFrame& Frame);

	TOptional<FTetherSubmitFrame> Pending;

	FCriticalSection SubmitLock;
	void* Submitter = nullptr;
	bool bSubmitterFailed = false;

#if !TETHERXR_UE_5_8_OR_NEWER
	TOptional<FTetherSubmitFrame> Presented;
	void* PresentedQueue = nullptr;
#endif
};
