#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ffnvcodec/nvEncodeAPI.h"

typedef NVENCSTATUS (NVENCAPI *PFN_GET_MAX_SUPPORTED_VERSION)(uint32_t *version);
typedef NVENCSTATUS (NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *function_list);

static int fail(const char *message)
{
    fprintf(stderr, "self-test: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("expected mode: cuda, software, abi-mismatch, init-failure, or init-version-mismatch");

    void *library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return fail(dlerror());

    PFN_GET_MAX_SUPPORTED_VERSION get_max_version =
        (PFN_GET_MAX_SUPPORTED_VERSION)dlsym(library, "NvEncodeAPIGetMaxSupportedVersion");
    PFN_CREATE_INSTANCE create_instance =
        (PFN_CREATE_INSTANCE)dlsym(library, "NvEncodeAPICreateInstance");
    if (!get_max_version || !create_instance)
        return fail("proxy entry points were not exported");

    uint32_t max_version = 0;
    if (get_max_version(&max_version) != NV_ENC_SUCCESS ||
        max_version != ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
        return fail("GetMaxSupportedVersion did not forward to the test driver");

    NV_ENCODE_API_FUNCTION_LIST functions = { 0 };
    functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const int mismatch = strcmp(argv[1], "abi-mismatch") == 0;
    if (mismatch)
        functions.version ^= 1U;

    const NVENCSTATUS create_status = create_instance(&functions);
    if (mismatch) {
        if (create_status != NV_ENC_ERR_INVALID_VERSION)
            return fail("ABI mismatch was not rejected by the version guard");
        (void)dlclose(library);
        return 0;
    }
    if (create_status != NV_ENC_SUCCESS)
        return fail("CreateInstance was not forwarded");

    NV_ENC_INITIALIZE_PARAMS initialize = { 0 };
    NV_ENC_CONFIG config = { 0 };
    initialize.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initialize.encodeGUID = NV_ENC_CODEC_H264_GUID;
    initialize.presetGUID = NV_ENC_PRESET_P4_GUID;
    initialize.encodeWidth = 2560;
    initialize.encodeHeight = 1440;
    initialize.frameRateNum = 360;
    initialize.frameRateDen = 1;
    initialize.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    initialize.encodeConfig = &config;
    config.version = NV_ENC_CONFIG_VER;
    if (strcmp(argv[1], "cuda") != 0)
        config.rcParams.version = NV_ENC_RC_PARAMS_VER;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.constQP.qpInterP = 37;
    config.rcParams.constQP.qpInterB = 39;
    config.rcParams.constQP.qpIntra = 35;
    config.rcParams.averageBitRate = 50000000;
    config.rcParams.maxBitRate = 60000000;
    config.rcParams.vbvBufferSize = 40000000;
    config.rcParams.enableMinQP = 1;
    config.rcParams.minQP.qpInterP = 18;
    config.rcParams.minQP.qpIntra = 20;
    config.rcParams.enableMaxQP = 1;
    config.rcParams.maxQP.qpInterP = 44;
    config.rcParams.maxQP.qpIntra = 42;
    config.rcParams.enableInitialRCQP = 1;
    config.rcParams.initialRCQP.qpInterP = 28;
    config.rcParams.initialRCQP.qpIntra = 24;
    config.rcParams.targetQuality = 33;
    config.rcParams.targetQualityLSB = 128;
    config.rcParams.enableAQ = 1;
    config.rcParams.enableTemporalAQ = 1;
    config.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
    config.rcParams.lowDelayKeyFrameScale = 2;
    if (strcmp(argv[1], "init-version-mismatch") == 0)
        config.rcParams.version ^= 1U;
    const int init_should_fail = strcmp(argv[1], "init-failure") == 0;
    const NVENCSTATUS initialize_status =
        functions.nvEncInitializeEncoder((void *)0x4567, &initialize);
    if (init_should_fail) {
        if (initialize_status != NV_ENC_ERR_GENERIC)
            return fail("InitializeEncoder failure did not reach the caller");
    } else if (initialize_status != NV_ENC_SUCCESS) {
        return fail("InitializeEncoder wrapper did not forward");
    }

    NV_ENC_REGISTER_RESOURCE resource = { 0 };
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = strcmp(argv[1], "software") == 0
        ? NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX
        : NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    if (functions.nvEncRegisterResource((void *)0x4567, &resource) != NV_ENC_SUCCESS ||
        resource.registeredResource != (void *)0x1234)
        return fail("RegisterResource did not reach the fake driver");

    NV_ENC_CREATE_INPUT_BUFFER software_input = { 0 };
    if (strcmp(argv[1], "software") == 0) {
        if (functions.nvEncCreateInputBuffer((void *)0x4567, &software_input) != NV_ENC_SUCCESS ||
            software_input.inputBuffer != (void *)0x2345)
            return fail("CreateInputBuffer did not reach the fake driver");

        NV_ENC_LOCK_INPUT_BUFFER lock_input = { 0 };
        if (functions.nvEncLockInputBuffer((void *)0x4567, &lock_input) != NV_ENC_SUCCESS ||
            lock_input.bufferDataPtr != (void *)0x3456 || lock_input.pitch != 4096)
            return fail("LockInputBuffer did not reach the fake driver");
    } else if (strcmp(argv[1], "cuda") != 0 && !init_should_fail &&
               strcmp(argv[1], "init-version-mismatch") != 0) {
        return fail("unknown mode");
    }

    NV_ENC_MAP_INPUT_RESOURCE map = { 0 };
    map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    map.registeredResource = resource.registeredResource;
    if (functions.nvEncMapInputResource((void *)0x4567, &map) != NV_ENC_SUCCESS ||
        map.mappedResource != (void *)0x5678)
        return fail("MapInputResource did not reach the fake driver");

    const int software = strcmp(argv[1], "software") == 0;
    const uint32_t picture_count = software ? 1 : 3;
    for (uint32_t frame = 0; frame < picture_count; ++frame) {
        NV_ENC_PIC_PARAMS picture = { 0 };
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.frameIdx = frame;
        picture.inputBuffer = map.mappedResource;
        const NVENCSTATUS expected = frame == 1 ? NV_ENC_ERR_NEED_MORE_INPUT
            : (frame == 2 ? NV_ENC_ERR_GENERIC : NV_ENC_SUCCESS);
        if (functions.nvEncEncodePicture((void *)0x4567, &picture) != expected)
            return fail("EncodePicture did not forward the mapped input and driver status");
    }

    if (software) {
        NV_ENC_PIC_PARAMS picture = { 0 };
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.frameIdx = 3;
        picture.inputBuffer = software_input.inputBuffer;
        if (functions.nvEncEncodePicture((void *)0x4567, &picture) != NV_ENC_SUCCESS)
            return fail("EncodePicture did not forward the software input buffer");
    }

    NV_ENC_PIC_PARAMS flush = { 0 };
    flush.version = NV_ENC_PIC_PARAMS_VER;
    flush.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    if (functions.nvEncEncodePicture((void *)0x4567, &flush) != NV_ENC_SUCCESS)
        return fail("EncodePicture did not forward the EOS flush");
    if (functions.nvEncEncodePicture((void *)0x4567, NULL) != NV_ENC_SUCCESS)
        return fail("EncodePicture did not forward the null-picture flush");

    if (functions.nvEncUnmapInputResource((void *)0x4567, map.mappedResource) != NV_ENC_SUCCESS)
        return fail("UnmapInputResource did not reach the fake driver");
    if (functions.nvEncUnregisterResource((void *)0x4567, resource.registeredResource) != NV_ENC_SUCCESS)
        return fail("UnregisterResource did not reach the fake driver");

    (void)dlclose(library);
    return 0;
}
