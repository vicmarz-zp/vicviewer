// NVENC API definitions - minimal subset for H.264 encoding
// Based on NVIDIA Video Codec SDK 12.x public API
#pragma once

#include <cstdint>
#include <Windows.h>

namespace vic::encoder::nvenc {

// API Version
#define NVENCAPI_MAJOR_VERSION 12
#define NVENCAPI_MINOR_VERSION 2
#define NVENCAPI_VERSION ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)

// Make version macro
#define NVENCAPI_STRUCT_VERSION(ver) ((uint32_t)(NVENCAPI_VERSION) | ((ver) << 16))

// Status codes
enum NVENCSTATUS {
    NV_ENC_SUCCESS = 0,
    NV_ENC_ERR_NO_ENCODE_DEVICE = 1,
    NV_ENC_ERR_UNSUPPORTED_DEVICE = 2,
    NV_ENC_ERR_INVALID_ENCODERDEVICE = 3,
    NV_ENC_ERR_INVALID_DEVICE = 4,
    NV_ENC_ERR_DEVICE_NOT_EXIST = 5,
    NV_ENC_ERR_INVALID_PTR = 6,
    NV_ENC_ERR_INVALID_EVENT = 7,
    NV_ENC_ERR_INVALID_PARAM = 8,
    NV_ENC_ERR_INVALID_CALL = 9,
    NV_ENC_ERR_OUT_OF_MEMORY = 10,
    NV_ENC_ERR_ENCODER_NOT_INITIALIZED = 11,
    NV_ENC_ERR_UNSUPPORTED_PARAM = 12,
    NV_ENC_ERR_LOCK_BUSY = 13,
    NV_ENC_ERR_NOT_ENOUGH_BUFFER = 14,
    NV_ENC_ERR_INVALID_VERSION = 15,
    NV_ENC_ERR_MAP_FAILED = 16,
    NV_ENC_ERR_NEED_MORE_INPUT = 17,
    NV_ENC_ERR_ENCODER_BUSY = 18,
    NV_ENC_ERR_EVENT_NOT_REGISTERD = 19,
    NV_ENC_ERR_GENERIC = 20,
    NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY = 21,
    NV_ENC_ERR_UNIMPLEMENTED = 22,
    NV_ENC_ERR_RESOURCE_REGISTER_FAILED = 23,
    NV_ENC_ERR_RESOURCE_NOT_REGISTERED = 24,
    NV_ENC_ERR_RESOURCE_NOT_MAPPED = 25,
};

// Input/Output resource types
enum NV_ENC_INPUT_RESOURCE_TYPE {
    NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX = 0,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR = 1,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY = 2,
    NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX = 3
};

// Buffer format
enum NV_ENC_BUFFER_FORMAT {
    NV_ENC_BUFFER_FORMAT_UNDEFINED = 0,
    NV_ENC_BUFFER_FORMAT_NV12 = 1,
    NV_ENC_BUFFER_FORMAT_YV12 = 0x10,
    NV_ENC_BUFFER_FORMAT_IYUV = 0x100,
    NV_ENC_BUFFER_FORMAT_YUV444 = 0x1000,
    NV_ENC_BUFFER_FORMAT_YUV420_10BIT = 0x10000,
    NV_ENC_BUFFER_FORMAT_YUV444_10BIT = 0x100000,
    NV_ENC_BUFFER_FORMAT_ARGB = 0x1000000,
    NV_ENC_BUFFER_FORMAT_ARGB10 = 0x2000000,
    NV_ENC_BUFFER_FORMAT_AYUV = 0x4000000,
    NV_ENC_BUFFER_FORMAT_ABGR = 0x10000000,
    NV_ENC_BUFFER_FORMAT_ABGR10 = 0x20000000,
};

// Picture type
enum NV_ENC_PIC_TYPE {
    NV_ENC_PIC_TYPE_P = 0,
    NV_ENC_PIC_TYPE_B = 1,
    NV_ENC_PIC_TYPE_I = 2,
    NV_ENC_PIC_TYPE_IDR = 3,
    NV_ENC_PIC_TYPE_BI = 4,
    NV_ENC_PIC_TYPE_SKIPPED = 5,
    NV_ENC_PIC_TYPE_INTRA_REFRESH = 6,
    NV_ENC_PIC_TYPE_NONREF_P = 7,
    NV_ENC_PIC_TYPE_UNKNOWN = 0xFF
};

// Rate control modes
enum NV_ENC_PARAMS_RC_MODE {
    NV_ENC_PARAMS_RC_CONSTQP = 0x0,
    NV_ENC_PARAMS_RC_VBR = 0x1,
    NV_ENC_PARAMS_RC_CBR = 0x2,
    NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ = 0x8,
    NV_ENC_PARAMS_RC_CBR_HQ = 0x10,
    NV_ENC_PARAMS_RC_VBR_HQ = 0x20
};

// Multi-pass encoding
enum NV_ENC_MULTI_PASS {
    NV_ENC_MULTI_PASS_DISABLED = 0,
    NV_ENC_MULTI_PASS_QUARTER_RESOLUTION = 1,
    NV_ENC_MULTI_PASS_FULL_RESOLUTION = 2
};

// Tuning info
enum NV_ENC_TUNING_INFO {
    NV_ENC_TUNING_INFO_UNDEFINED = 0,
    NV_ENC_TUNING_INFO_HIGH_QUALITY = 1,
    NV_ENC_TUNING_INFO_LOW_LATENCY = 2,
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY = 3,
    NV_ENC_TUNING_INFO_LOSSLESS = 4
};

// Device type
enum NV_ENC_DEVICE_TYPE {
    NV_ENC_DEVICE_TYPE_DIRECTX = 0,
    NV_ENC_DEVICE_TYPE_CUDA = 1,
    NV_ENC_DEVICE_TYPE_OPENGL = 2
};

// GUIDs
static const GUID NV_ENC_CODEC_H264_GUID = 
    { 0x6bc82762, 0x4e63, 0x4ca4, { 0xaa, 0x85, 0x1e, 0x50, 0xf3, 0x21, 0xf6, 0xbf } };
static const GUID NV_ENC_CODEC_HEVC_GUID = 
    { 0x790cdc88, 0x4522, 0x4d7b, { 0x94, 0x25, 0xbd, 0xa9, 0x97, 0x5f, 0x76, 0x03 } };

// H.264 profile GUIDs
static const GUID NV_ENC_H264_PROFILE_BASELINE_GUID =
    { 0x0727bcaa, 0x78c4, 0x4c83, { 0x8c, 0x2f, 0xef, 0x3d, 0xff, 0x26, 0x7c, 0x6a } };
static const GUID NV_ENC_H264_PROFILE_MAIN_GUID =
    { 0x60b5c1d4, 0x67fe, 0x4790, { 0x94, 0xd5, 0xc4, 0x72, 0x6d, 0x7b, 0x6e, 0x6d } };
static const GUID NV_ENC_H264_PROFILE_HIGH_GUID =
    { 0xe7cbc309, 0x4f7a, 0x4b89, { 0xaf, 0x2a, 0xd5, 0x37, 0xc9, 0x2b, 0xe3, 0x10 } };

// Preset GUIDs (P1=fastest, P7=highest quality)
static const GUID NV_ENC_PRESET_P1_GUID =
    { 0xfc0a8d3e, 0x45f8, 0x4cf8, { 0x80, 0xc7, 0x29, 0x87, 0x71, 0xeb, 0x2f, 0xc5 } };
static const GUID NV_ENC_PRESET_P2_GUID =
    { 0xf581cfb8, 0x88d6, 0x4381, { 0x93, 0xf0, 0xdf, 0x13, 0xf9, 0xc2, 0x78, 0x56 } };
static const GUID NV_ENC_PRESET_P3_GUID =
    { 0x36850110, 0x3a07, 0x441f, { 0x94, 0xd5, 0x3a, 0x7f, 0x51, 0x73, 0x0a, 0xb8 } };
static const GUID NV_ENC_PRESET_P4_GUID =
    { 0x90a7b826, 0xdf06, 0x4862, { 0xb9, 0xd2, 0xcd, 0x6d, 0x73, 0xa0, 0x80, 0x31 } };
static const GUID NV_ENC_PRESET_P5_GUID =
    { 0x21c6e6b4, 0x297a, 0x4cba, { 0x99, 0x8f, 0xb6, 0xcb, 0xde, 0x72, 0xad, 0xe3 } };
static const GUID NV_ENC_PRESET_P6_GUID =
    { 0x8e75c279, 0x6299, 0x4ab6, { 0x83, 0x62, 0x82, 0xc9, 0x3e, 0x44, 0x9a, 0x41 } };
static const GUID NV_ENC_PRESET_P7_GUID =
    { 0x84848c12, 0x6f71, 0x4c13, { 0x93, 0x1b, 0x53, 0xe2, 0x83, 0xf5, 0x79, 0x74 } };

#pragma pack(push, 8)

// Open encode session params
struct NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t version;
    NV_ENC_DEVICE_TYPE deviceType;
    void* device;
    uint32_t reserved;
    uint32_t apiVersion;
    uint32_t reserved1[253];
    void* reserved2[64];
};
#define NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER NVENCAPI_STRUCT_VERSION(1)

// Encode config
struct NV_ENC_CONFIG_H264 {
    uint32_t enableStereoMVC : 1;
    uint32_t hierarchicalPFrames : 1;
    uint32_t hierarchicalBFrames : 1;
    uint32_t outputBufferingPeriodSEI : 1;
    uint32_t outputPictureTimingSEI : 1;
    uint32_t outputAUD : 1;
    uint32_t disableSPSPPS : 1;
    uint32_t outputFramePackingSEI : 1;
    uint32_t outputRecoveryPointSEI : 1;
    uint32_t enableIntraRefresh : 1;
    uint32_t enableConstrainedEncoding : 1;
    uint32_t repeatSPSPPS : 1;
    uint32_t enableVFR : 1;
    uint32_t enableLTR : 1;
    uint32_t qpPrimeYZeroTransformBypassFlag : 1;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t enableFillerDataInsertion : 1;
    uint32_t disableSVCPrefixNalu : 1;
    uint32_t enableScalabilityInfoSEI : 1;
    uint32_t singleSliceIntraRefresh : 1;
    uint32_t reserved : 12;
    uint32_t level;
    uint32_t idrPeriod;
    uint32_t separateColourPlaneFlag;
    uint32_t disableDeblockingFilterIDC;
    uint32_t numTemporalLayers;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t adaptiveTransformMode;
    uint32_t fmoMode;
    uint32_t bdirectMode;
    uint32_t entropyCodingMode;
    uint32_t stereoMode;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFrames;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t h264VUIParameters;
    uint32_t ltrNumFrames;
    uint32_t ltrTrustMode;
    uint32_t chromaFormatIDC;
    uint32_t maxTemporalLayers;
    uint32_t useBFramesAsRef;
    uint32_t numRefL0;
    uint32_t numRefL1;
    uint32_t reserved1[267];
    void* reserved2[64];
};

// Rate control params
struct NV_ENC_RC_PARAMS {
    uint32_t version;
    NV_ENC_PARAMS_RC_MODE rateControlMode;
    int32_t constQP_I;
    int32_t constQP_P;
    int32_t constQP_B;
    uint32_t averageBitRate;
    uint32_t maxBitRate;
    uint32_t vbvBufferSize;
    uint32_t vbvInitialDelay;
    uint32_t enableMinQP : 1;
    uint32_t enableMaxQP : 1;
    uint32_t enableInitialRCQP : 1;
    uint32_t enableAQ : 1;
    uint32_t reservedBitField1 : 1;
    uint32_t enableLookahead : 1;
    uint32_t disableIadapt : 1;
    uint32_t disableBadapt : 1;
    uint32_t enableTemporalAQ : 1;
    uint32_t zeroReorderDelay : 1;
    uint32_t enableNonRefP : 1;
    uint32_t strictGOPTarget : 1;
    uint32_t aqStrength : 4;
    uint32_t reservedBitFields : 16;
    int32_t minQP_I;
    int32_t minQP_P;
    int32_t minQP_B;
    int32_t maxQP_I;
    int32_t maxQP_P;
    int32_t maxQP_B;
    int32_t initialRCQP_I;
    int32_t initialRCQP_P;
    int32_t initialRCQP_B;
    uint32_t temporallayerIdxMask;
    uint8_t temporalLayerQP[8];
    uint8_t targetQuality;
    uint8_t targetQualityLSB;
    uint16_t lookaheadDepth;
    uint8_t lowDelayKeyFrameScale;
    uint8_t reserved1[3];
    NV_ENC_MULTI_PASS multiPass;
    int8_t alphaLayerBitrateRatio;
    uint8_t reserved[3];
    uint32_t cbQPIndexOffset;
    uint32_t crQPIndexOffset;
    uint32_t reserved2[285];
};

// Encode config
struct NV_ENC_CONFIG {
    uint32_t version;
    GUID profileGUID;
    uint32_t gopLength;
    int32_t frameIntervalP;
    uint32_t monoChromeEncoding;
    uint32_t frameFieldMode;
    uint32_t mvPrecision;
    NV_ENC_RC_PARAMS rcParams;
    union {
        NV_ENC_CONFIG_H264 h264Config;
        uint32_t reserved[320];
    } encodeCodecConfig;
    uint32_t reserved[278];
    void* reserved2[64];
};
#define NV_ENC_CONFIG_VER NVENCAPI_STRUCT_VERSION(8)

// Initialize params
struct NV_ENC_INITIALIZE_PARAMS {
    uint32_t version;
    GUID encodeGUID;
    GUID presetGUID;
    uint32_t encodeWidth;
    uint32_t encodeHeight;
    uint32_t darWidth;
    uint32_t darHeight;
    uint32_t frameRateNum;
    uint32_t frameRateDen;
    uint32_t enableEncodeAsync;
    uint32_t enablePTD;
    uint32_t reportSliceOffsets : 1;
    uint32_t enableSubFrameWrite : 1;
    uint32_t enableExternalMEHints : 1;
    uint32_t enableMEOnlyMode : 1;
    uint32_t enableWeightedPrediction : 1;
    uint32_t enableOutputInVidmem : 1;
    uint32_t reservedBitFields : 26;
    uint32_t privDataSize;
    void* privData;
    NV_ENC_CONFIG* encodeConfig;
    uint32_t maxEncodeWidth;
    uint32_t maxEncodeHeight;
    void* maxMEHintCountsPerBlock[2];
    NV_ENC_TUNING_INFO tuningInfo;
    uint32_t bufferFormat;
    uint32_t numStateBuffers;
    uint32_t outputStatsLevel;
    uint32_t reserved[285];
    void* reserved2[64];
};
#define NV_ENC_INITIALIZE_PARAMS_VER NVENCAPI_STRUCT_VERSION(6)

// Preset config
struct NV_ENC_PRESET_CONFIG {
    uint32_t version;
    NV_ENC_CONFIG presetCfg;
    uint32_t reserved1[255];
    void* reserved2[64];
};
#define NV_ENC_PRESET_CONFIG_VER NVENCAPI_STRUCT_VERSION(4)

// Create input buffer params
struct NV_ENC_CREATE_INPUT_BUFFER {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t memoryHeap;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t reserved;
    void* inputBuffer;
    void* pSysMemBuffer;
    uint32_t reserved1[57];
    void* reserved2[63];
};
#define NV_ENC_CREATE_INPUT_BUFFER_VER NVENCAPI_STRUCT_VERSION(1)

// Create bitstream buffer params
struct NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t version;
    uint32_t size;
    uint32_t memoryHeap;
    uint32_t reserved;
    void* bitstreamBuffer;
    void* bitstreamBufferPtr;
    uint32_t reserved1[58];
    void* reserved2[64];
};
#define NV_ENC_CREATE_BITSTREAM_BUFFER_VER NVENCAPI_STRUCT_VERSION(1)

// Lock input buffer params
struct NV_ENC_LOCK_INPUT_BUFFER {
    uint32_t version;
    uint32_t doNotWait : 1;
    uint32_t reservedBitFields : 31;
    void* inputBuffer;
    void* bufferDataPtr;
    uint32_t pitch;
    uint32_t reserved1[62];
    void* reserved2[64];
};
#define NV_ENC_LOCK_INPUT_BUFFER_VER NVENCAPI_STRUCT_VERSION(1)

// Lock bitstream params
struct NV_ENC_LOCK_BITSTREAM {
    uint32_t version;
    uint32_t doNotWait : 1;
    uint32_t ltrFrame : 1;
    uint32_t getRCStats : 1;
    uint32_t reservedBitFields : 29;
    void* outputBitstream;
    uint32_t* sliceOffsets;
    uint32_t frameIdx;
    uint32_t hwEncodeStatus;
    uint32_t numSlices;
    uint32_t bitstreamSizeInBytes;
    uint64_t outputTimeStamp;
    uint64_t outputDuration;
    void* bitstreamBufferPtr;
    NV_ENC_PIC_TYPE pictureType;
    uint32_t pictureStruct;
    uint32_t frameAvgQP;
    uint32_t frameSatd;
    uint32_t ltrFrameIdx;
    uint32_t ltrFrameBitmap;
    uint32_t reserved[13];
    uint32_t intraMBCount;
    uint32_t interMBCount;
    int32_t averageMVX;
    int32_t averageMVY;
    uint32_t reserved1[219];
    void* reserved2[64];
};
#define NV_ENC_LOCK_BITSTREAM_VER NVENCAPI_STRUCT_VERSION(2)

// Pic params
struct NV_ENC_PIC_PARAMS {
    uint32_t version;
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t inputPitch;
    uint32_t encodePicFlags;
    uint32_t frameIdx;
    uint64_t inputTimeStamp;
    uint64_t inputDuration;
    void* inputBuffer;
    void* outputBitstream;
    void* completionEvent;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t pictureStruct;
    uint32_t pictureType;
    GUID codecPicParams;
    void* meHintCountsPerBlock[2];
    void* meExternalHints;
    uint32_t reserved1[6];
    void* reserved2[2];
    int8_t* qpDeltaMap;
    uint32_t qpDeltaMapSize;
    uint32_t reservedBitFields;
    uint32_t meHintRefPicDist[2];
    uint32_t alphaBuffer;
    uint32_t reserved3[286];
    void* reserved4[60];
};
#define NV_ENC_PIC_PARAMS_VER NVENCAPI_STRUCT_VERSION(6)

// Register resource params
struct NV_ENC_REGISTER_RESOURCE {
    uint32_t version;
    NV_ENC_INPUT_RESOURCE_TYPE resourceType;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t subResourceIndex;
    void* resourceToRegister;
    void* registeredResource;
    NV_ENC_BUFFER_FORMAT bufferFormat;
    uint32_t bufferUsage;
    uint32_t reserved[62];
    void* reserved2[63];
};
#define NV_ENC_REGISTER_RESOURCE_VER NVENCAPI_STRUCT_VERSION(4)

// Map input resource params
struct NV_ENC_MAP_INPUT_RESOURCE {
    uint32_t version;
    uint32_t subResourceIndex;
    void* inputResource;
    void* registeredResource;
    void* mappedResource;
    NV_ENC_BUFFER_FORMAT mappedBufferFmt;
    uint32_t reserved1[62];
    void* reserved2[63];
};
#define NV_ENC_MAP_INPUT_RESOURCE_VER NVENCAPI_STRUCT_VERSION(4)

// Function pointers structure
struct NV_ENCODE_API_FUNCTION_LIST {
    uint32_t version;
    uint32_t reserved;
    NVENCSTATUS (*nvEncOpenEncodeSession)(void* device, uint32_t deviceType, void** encoder);
    NVENCSTATUS (*nvEncGetEncodeGUIDCount)(void* encoder, uint32_t* encodeGUIDCount);
    NVENCSTATUS (*nvEncGetEncodeGUIDs)(void* encoder, GUID* GUIDs, uint32_t guidArraySize, uint32_t* GUIDCount);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDCount)(void* encoder, GUID encodeGUID, uint32_t* encodeProfileGUIDCount);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDs)(void* encoder, GUID encodeGUID, GUID* profileGUIDs, uint32_t guidArraySize, uint32_t* GUIDCount);
    NVENCSTATUS (*nvEncGetInputFormatCount)(void* encoder, GUID encodeGUID, uint32_t* inputFmtCount);
    NVENCSTATUS (*nvEncGetInputFormats)(void* encoder, GUID encodeGUID, NV_ENC_BUFFER_FORMAT* inputFmts, uint32_t inputFmtArraySize, uint32_t* inputFmtCount);
    NVENCSTATUS (*nvEncGetEncodeCaps)(void* encoder, GUID encodeGUID, void* capsParam, int* capsVal);
    NVENCSTATUS (*nvEncGetEncodePresetCount)(void* encoder, GUID encodeGUID, uint32_t* encodePresetGUIDCount);
    NVENCSTATUS (*nvEncGetEncodePresetGUIDs)(void* encoder, GUID encodeGUID, GUID* presetGUIDs, uint32_t guidArraySize, uint32_t* encodePresetGUIDCount);
    NVENCSTATUS (*nvEncGetEncodePresetConfig)(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_PRESET_CONFIG* presetConfig);
    NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void* encoder, GUID encodeGUID, GUID presetGUID, NV_ENC_TUNING_INFO tuningInfo, NV_ENC_PRESET_CONFIG* presetConfig);
    NVENCSTATUS (*nvEncInitializeEncoder)(void* encoder, NV_ENC_INITIALIZE_PARAMS* createEncodeParams);
    NVENCSTATUS (*nvEncCreateInputBuffer)(void* encoder, NV_ENC_CREATE_INPUT_BUFFER* createInputBufferParams);
    NVENCSTATUS (*nvEncDestroyInputBuffer)(void* encoder, void* inputBuffer);
    NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void* encoder, NV_ENC_CREATE_BITSTREAM_BUFFER* createBitstreamBufferParams);
    NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void* encoder, void* bitstreamBuffer);
    NVENCSTATUS (*nvEncEncodePicture)(void* encoder, NV_ENC_PIC_PARAMS* encodePicParams);
    NVENCSTATUS (*nvEncLockBitstream)(void* encoder, NV_ENC_LOCK_BITSTREAM* lockBitstreamBufferParams);
    NVENCSTATUS (*nvEncUnlockBitstream)(void* encoder, void* bitstreamBuffer);
    NVENCSTATUS (*nvEncLockInputBuffer)(void* encoder, NV_ENC_LOCK_INPUT_BUFFER* lockInputBufferParams);
    NVENCSTATUS (*nvEncUnlockInputBuffer)(void* encoder, void* inputBuffer);
    NVENCSTATUS (*nvEncGetEncodeStats)(void* encoder, void* encodeStats);
    NVENCSTATUS (*nvEncGetSequenceParams)(void* encoder, void* sequenceParamPayload);
    NVENCSTATUS (*nvEncGetSequenceParamEx)(void* encoder, void* encInitParams, void* sequenceParamPayload);
    NVENCSTATUS (*nvEncRegisterAsyncEvent)(void* encoder, void* eventParams);
    NVENCSTATUS (*nvEncUnregisterAsyncEvent)(void* encoder, void* eventParams);
    NVENCSTATUS (*nvEncMapInputResource)(void* encoder, NV_ENC_MAP_INPUT_RESOURCE* mapInputResParams);
    NVENCSTATUS (*nvEncUnmapInputResource)(void* encoder, void* mappedInputBuffer);
    NVENCSTATUS (*nvEncDestroyEncoder)(void* encoder);
    NVENCSTATUS (*nvEncInvalidateRefFrames)(void* encoder, uint64_t invalidRefFrameTimeStamp);
    NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* openSessionExParams, void** encoder);
    NVENCSTATUS (*nvEncRegisterResource)(void* encoder, NV_ENC_REGISTER_RESOURCE* registerResParams);
    NVENCSTATUS (*nvEncUnregisterResource)(void* encoder, void* registeredRes);
    NVENCSTATUS (*nvEncReconfigureEncoder)(void* encoder, void* reInitEncodeParams);
    void* reserved1;
    NVENCSTATUS (*nvEncCreateMVBuffer)(void* encoder, void* createMVBufferParams);
    NVENCSTATUS (*nvEncDestroyMVBuffer)(void* encoder, void* mvBuffer);
    NVENCSTATUS (*nvEncRunMotionEstimationOnly)(void* encoder, void* meOnlyParams);
    const char* (*nvEncGetLastErrorString)(void* encoder);
    NVENCSTATUS (*nvEncSetIOCudaStreams)(void* encoder, void* inputStream, void* outputStream);
    NVENCSTATUS (*nvEncGetEncodePresetConfigsV2)(void* encoder, GUID encodeGUID, GUID* presetGUIDs, uint32_t guidArraySize, uint32_t* encodePresetGUIDCount);
    NVENCSTATUS (*nvEncGetEncodeCapsEx)(void* encoder, GUID encodeGUID, void* capsParam, int* capsVal);
    void* reserved2[284];
};
#define NV_ENCODE_API_FUNCTION_LIST_VER NVENCAPI_STRUCT_VERSION(2)

#pragma pack(pop)

// Calling convention for NVENC
#ifndef NVENCAPI
#ifdef _WIN32
#define NVENCAPI __stdcall
#else
#define NVENCAPI
#endif
#endif

// Function type for NvEncodeAPICreateInstance
typedef NVENCSTATUS (NVENCAPI *PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST* functionList);
typedef NVENCSTATUS (NVENCAPI *PNVENCODEAPIGETMAXSUPPORTEDVERSION)(uint32_t* version);

} // namespace vic::encoder::nvenc
