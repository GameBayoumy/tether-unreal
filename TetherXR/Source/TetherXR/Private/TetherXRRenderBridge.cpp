// Copyright (c) 2026 Tether contributors. MIT License.

#include "TetherXRRenderBridge.h"

#include "TetherRuntime.h"

#if TETHERXR_UE_5_8_OR_NEWER
#include "IMetalDynamicRHI.h"
#endif

FTetherXRRenderBridge::~FTetherXRRenderBridge()
{
	FScopeLock Lock(&SubmitLock);
	FTetherRuntime::Get().DestroySubmitter(Submitter);
	Submitter = nullptr;
}

void FTetherXRRenderBridge::SetFrame_RHIThread(FTetherSubmitFrame&& Frame)
{
	Pending = MoveTemp(Frame);
}

#if TETHERXR_UE_5_8_OR_NEWER

bool FTetherXRRenderBridge::Present(FRHIViewport* Viewport, IRHICommandContext& RHICmdContext, int32& InOutSyncInterval)
{
	if (Pending.IsSet() && IsRHIMetal())
	{
		// The rendering of this frame has already been handed to the submission thread, which commits work
		// to the GPU in order: running there puts Tether's command buffer right behind it on the same queue.
		TRefCountPtr<FTetherXRRenderBridge> Self(this);
		GetIMetalDynamicRHI()->RHIRunOnQueue(
			[Self, Frame = MoveTemp(Pending.GetValue())](MTL::CommandQueue* Queue)
			{
				Self->Submit(Queue, Frame);
			},
			false);
		Pending.Reset();
	}
	return true; // Still present the Mac window (spectator view).
}

#else

bool FTetherXRRenderBridge::Present(int32& InOutSyncInterval)
{
	if (Pending.IsSet())
	{
		// This frame's commands are still being encoded; PostPresent runs once they're committed.
		FScopeLock Lock(&SubmitLock);
		Presented = MoveTemp(Pending);
		PresentedQueue = GDynamicRHI->RHIGetNativeGraphicsQueue();
		Pending.Reset();
	}
	return true;
}

void FTetherXRRenderBridge::PostPresent()
{
	// Called from Metal's scheduled handler once the frame's command buffer is committed, so Tether's
	// command buffer lands behind it on the queue.
	TOptional<FTetherSubmitFrame> Frame;
	void* Queue = nullptr;
	{
		FScopeLock Lock(&SubmitLock);
		Frame = MoveTemp(Presented);
		Presented.Reset();
		Queue = PresentedQueue;
	}
	if (Frame.IsSet())
	{
		Submit(Queue, Frame.GetValue());
	}
}

#endif

void FTetherXRRenderBridge::Submit(void* MetalCommandQueue, const FTetherSubmitFrame& Frame)
{
	FScopeLock Lock(&SubmitLock);
	const FTetherRuntime& Runtime = FTetherRuntime::Get();
	if (!Submitter && !bSubmitterFailed)
	{
		Submitter = Runtime.CreateSubmitter(MetalCommandQueue);
		bSubmitterFailed = Submitter == nullptr;
		UE_CLOG(bSubmitterFailed, LogTetherXR, Warning, TEXT("Tether couldn't start a frame submitter; nothing will reach the headset"));
		UE_CLOG(!bSubmitterFailed, LogTetherXR, Log, TEXT("Streaming frames to Tether"));
	}
	if (!Submitter || !Frame.Texture.IsValid())
	{
		return;
	}

	const FIntPoint Size = Frame.Texture->GetSizeXY();
	const int32 EyeWidth = Size.X / 2;
	const int32 LeftRect[4] = { 0, 0, EyeWidth, Size.Y };
	const int32 RightRect[4] = { EyeWidth, 0, EyeWidth, Size.Y };
	Runtime.Submit(Submitter, Frame.Texture->GetNativeResource(), LeftRect, RightRect, Frame.Pose);
}
