#if !defined(NGX_DLL) && !defined(NGX_NO_FUNCS)
#error "NGX_DLL not defined."
#endif

#ifndef NGX_H
#define NGX_H

#include <stdint.h>

struct ID3D12Resource;
struct ID3D11Resource;

enum NVSDK_NGX_Version : int {
	NVSDK_NGX_Version_API = 0x12,
};

enum NVSDK_NGX_Result : uint32_t {
	NVSDK_NGX_Result_Success = 0x1,
	NVSDK_NGX_Result_Fail = 0xBAD00000,
	NVSDK_NGX_Result_FAIL_FeatureNotSupported = 0xBAD00001,
	NVSDK_NGX_Result_FAIL_PlatformError = 0xBAD00002,
	NVSDK_NGX_Result_FAIL_FeatureAlreadyExists = 0xBAD00003,
	NVSDK_NGX_Result_FAIL_FeatureNotFound = 0xBAD00004,
	NVSDK_NGX_Result_FAIL_InvalidParameter = 0xBAD00005,
	NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall = 0xBAD00006,
	NVSDK_NGX_Result_FAIL_NotInitialized = 0xBAD00007,
	NVSDK_NGX_Result_FAIL_UnsupportedInputFormat = 0xBAD00008,
	NVSDK_NGX_Result_FAIL_RWFlagMissing = 0xBAD00009,
	NVSDK_NGX_Result_FAIL_MissingInput = 0xBAD0000A,
	NVSDK_NGX_Result_FAIL_UnableToInitializeFeature = 0xBAD0000B,
	NVSDK_NGX_Result_FAIL_OutOfDate = 0xBAD0000C,
	NVSDK_NGX_Result_FAIL_OutOfGPUMemory = 0xBAD0000D,
	NVSDK_NGX_Result_FAIL_UnsupportedFormat = 0xBAD0000E,
	NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath = 0xBAD0000F,
	NVSDK_NGX_Result_FAIL_UnsupportedParameter = 0xBAD00010,
};

enum NVSDK_NGX_Buffer_Format : int {
	NVSDK_NGX_Buffer_Format_Unknown = 0x0,
	NVSDK_NGX_Buffer_Format_RGB8UI = 0x1,
	NVSDK_NGX_Buffer_Format_RGB16F = 0x2,
	NVSDK_NGX_Buffer_Format_RGB32F = 0x3,
	NVSDK_NGX_Buffer_Format_RGBA8UI = 0x4,
	NVSDK_NGX_Buffer_Format_RGBA16F = 0x5,
	NVSDK_NGX_Buffer_Format_RGBA32F = 0x6,
};

enum NVSDK_NGX_Feature : int {
	NVSDK_NGX_Feature_Reserved0 = 0x0,
	NVSDK_NGX_Feature_SuperSampling = 0x1,
	NVSDK_NGX_Feature_InPainting = 0x2,
	NVSDK_NGX_Feature_ImageSuperResolution = 0x3,
	NVSDK_NGX_Feature_SlowMotion = 0x4,
	NVSDK_NGX_Feature_VideoSuperResolution = 0x5,
	NVSDK_NGX_Feature_Reserved6 = 0x6,
	NVSDK_NGX_Feature_Reserved7 = 0x7,
	NVSDK_NGX_Feature_Reserved8 = 0x8,
	NVSDK_NGX_Feature_ImageSignalProcessing = 0x9,
	NVSDK_NGX_Feature_Count = 0xA,
};

struct NVSDK_NGX_Parameter {
	virtual void Set(const char *, void *) = 0;
	virtual void Set(const char *, struct ID3D12Resource *) = 0;
	virtual void Set(const char *, struct ID3D11Resource *) = 0;
	virtual void Set(const char *, int) = 0;
	virtual void Set(const char *, unsigned int) = 0;
	virtual void Set(const char *, long double) = 0;
	virtual void Set(const char *, float) = 0;
	virtual void Set(const char *, uint64_t) = 0;
	virtual NVSDK_NGX_Result Get(const char *, void **) = 0;
	virtual NVSDK_NGX_Result Get(const char *, struct ID3D12Resource **) = 0;
	virtual NVSDK_NGX_Result Get(const char *, struct ID3D11Resource **) = 0;
	virtual NVSDK_NGX_Result Get(const char *, int *) = 0;
	virtual NVSDK_NGX_Result Get(const char *, unsigned int *) = 0;
	virtual NVSDK_NGX_Result Get(const char *, long double *) = 0;
	virtual NVSDK_NGX_Result Get(const char *, float *) = 0;
	virtual NVSDK_NGX_Result Get(const char *, uint64_t *) = 0;
	virtual void Reset() = 0;
};

void NV_new_Parameter(NVSDK_NGX_Parameter **p);

struct NVSDK_NGX_Handle {
	NVSDK_NGX_Feature Id;
};

// Parameters
#define NVSDK_NGX_Parameter_ImageSuperResolution_Available "ImageSuperResolution.Available"
#define NVSDK_NGX_Parameter_Width "Width"
#define NVSDK_NGX_Parameter_Height "Height"
#define NVSDK_NGX_Parameter_Scale "Scale"
#define NVSDK_NGX_Parameter_Scratch "Scratch"
#define NVSDK_NGX_Parameter_Scratch_SizeInBytes "Scratch.SizeInBytes"
#define NVSDK_NGX_Parameter_Color_SizeInBytes "Color.SizeInBytes"
#define NVSDK_NGX_Parameter_Color_Format "Color.Format"
#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Output_SizeInBytes "Output.SizeInBytes"
#define NVSDK_NGX_Parameter_Output_Format "Output.Format"
#define NVSDK_NGX_Parameter_Output "Output"

#ifndef NGX_NO_FUNCS
#include "autodll.h"

// NVSDK_NGX_Init
// -------------------------------------
// 
// InApplicationId:
//      Unique Id provided by NVIDIA
//
// InApplicationDataPath:
//      Folder to store logs and other temporary files (write access required)
//
// InDevice: [d3d11/12 only]
//      DirectX device to use
//
// DESCRIPTION:
//      Initializes new SDK instance.
//
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_Init, (unsigned long long InApplicationId, const wchar_t *InApplicationDataPath, NVSDK_NGX_Version InSDKVersion/* = NVSDK_NGX_Version_API*/));

// NVSDK_NGX_GetScratchBufferSize
// ----------------------------------------------------------
//
// InFeatureId:
//      AI feature in question
//
// InParameters:
//      Parameters used by the feature to help estimate scratch buffer size
//
// OutSizeInBytes:
//      Number of bytes needed for the scratch buffer for the specified feature.
//
// DESCRIPTION:
//      SDK needs a buffer of a certain size provided by the client in
//      order to initialize AI feature. Once feature is no longer
//      needed buffer can be released. It is safe to reuse the same
//      scratch buffer for different features as long as minimum size
//      requirement is met for all features. Please note that some
//      features might not need a scratch buffer so return size of 0
//      is completely valid.
//
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_GetScratchBufferSize, (NVSDK_NGX_Feature InFeatureId, const NVSDK_NGX_Parameter *InParameters, size_t *OutSizeInBytes));

// NVSDK_NGX_CreateFeature
// -------------------------------------
//
// InCmdList:[d3d12 only]
//      Command list to use to execute GPU commands. Must be:
//      - Open and recording 
//      - With node mask including the device provided in NVSDK_NGX_D3D12_Init
//      - Execute on non-copy command queue.
// InDevCtx: [d3d11 only]
//      Device context to use to execute GPU commands
//
// InFeatureID:
//      AI feature to initialize
//
// InParameters:
//      List of parameters 
// 
// OutHandle:
//      Handle which uniquely identifies the feature. If feature with
//      provided parameters already exists the "already exists" error code is returned.
//
// DESCRIPTION:
//      Each feature needs to be created before it can be used. 
//      Refer to the sample code to find out which input parameters
//      are needed to create specific feature.
//
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_CreateFeature, (NVSDK_NGX_Feature InFeatureID, const NVSDK_NGX_Parameter *InParameters, NVSDK_NGX_Handle **OutHandle));

// NVSDK_NGX_EvaluateFeature
// -------------------------------------
//
// InCmdList:[d3d12 only]
//      Command list to use to execute GPU commands. Must be:
//      - Open and recording 
//      - With node mask including the device provided in NVSDK_NGX_D3D12_Init
//      - Execute on non-copy command queue.
// InDevCtx: [d3d11 only]
//      Device context to use to execute GPU commands
//
// InFeatureHandle:
//      Handle representing feature to be evaluated
// 
// InParameters:
//      List of parameters required to evaluate feature
//
// InCallback:
//      Optional callback for features which might take longer
//      to execute. If specified SDK will call it with progress
//      values in range 0.0f - 1.0f. Client application can indicate
//      that evaluation should be cancelled by setting OutShouldCancel
//      to true.
//
// DESCRIPTION:
//      Evaluates given feature using the provided parameters and
//      pre-trained NN. Please note that for most features
//      it can be beneficial to pass as many input buffers and parameters
//      as possible (for example provide all render targets like color, albedo, normals, depth etc)
//
typedef void (*PFN_NVSDK_NGX_ProgressCallback)(float InCurrentProgress, bool &OutShouldCancel);
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_EvaluateFeature, (const NVSDK_NGX_Handle *InFeatureHandle, const NVSDK_NGX_Parameter *InParameters, PFN_NVSDK_NGX_ProgressCallback InCallback /* = nullptr */));

// NVSDK_NGX_Release
// -------------------------------------
// 
// InHandle:
//      Handle to feature to be released
//
// DESCRIPTION:
//      Releases feature with a given handle.
//      Handles are not reference counted so
//      after this call it is invalid to use provided handle.
//
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_ReleaseFeature, (NVSDK_NGX_Handle *InHandle));

// NVSDK_NGX_Shutdown
// -------------------------------------
// 
// DESCRIPTION:
//      Shuts down the current SDK instance and releases all resources.
//
EXT_FN(NGX_DLL, NVSDK_NGX_Result, NVSDK_NGX_CUDA_Shutdown, ());
#endif // !defined(NGX_NO_FUNCS)

#endif // defined(NGX_H)
