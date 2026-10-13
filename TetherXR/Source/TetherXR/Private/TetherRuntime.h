// Copyright (c) 2026 Tether contributors. MIT License.

#pragma once

#include "TetherXRCommon.h"

/**
 * The Tether runtime library (libTetherOpenXR.dylib), loaded at runtime. Everything TetherXR needs from
 * Tether goes through the small C API in tether_direct.h; when the library is missing every call fails
 * gracefully and the plugin stays dormant.
 */
class FTetherRuntime
{
public:
	static FTetherRuntime& Get();

	/** Finds and loads the library once. Safe to call repeatedly. */
	bool Load();
	void Unload();

	bool IsLoaded() const { return ReadStateFn != nullptr; }
	const FString& GetLibraryPath() const { return LibraryPath; }

	/** Latest tracking published by the Tether app; false if Tether isn't running (or the library is missing). */
	bool ReadState(tether_xr_state& OutState) const;

	void* CreateSubmitter(void* MetalCommandQueue) const;
	bool Submit(void* Submitter, void* MetalTexture, const int32 LeftRect[4], const int32 RightRect[4], const qlh_pose& Pose) const;
	void DestroySubmitter(void* Submitter) const;

private:
	static FString FindLibrary();

	bool bAttempted = false;
	void* Handle = nullptr;
	FString LibraryPath;

	decltype(&tether_direct_read_state) ReadStateFn = nullptr;
	decltype(&tether_direct_create) CreateFn = nullptr;
	decltype(&tether_direct_submit) SubmitFn = nullptr;
	decltype(&tether_direct_destroy) DestroyFn = nullptr;
};
