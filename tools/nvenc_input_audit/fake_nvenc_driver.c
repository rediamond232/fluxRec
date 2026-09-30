#include <string.h>
#include <stdlib.h>
#include "ffnvcodec/nvEncodeAPI.h"

static NVENCSTATUS NVENCAPI fake_register_resource(void *encoder, NV_ENC_REGISTER_RESOURCE *params)
{
    (void)encoder;
    if (!params)
        return NV_ENC_ERR_INVALID_PTR;
    params->registeredResource = (void *)0x1234;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI fake_unregister_resource(void *encoder, NV_ENC_REGISTERED_PTR resource)
{
    (void)encoder;
    return resource == (void *)0x1234 ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PARAM;
}

static NVENCSTATUS NVENCAPI fake_map_input_resource(void *encoder, NV_ENC_MAP_INPUT_RESOURCE *params)
{
    (void)encoder;
    if (!params)
        return NV_ENC_ERR_INVALID_PTR;
    if (params->registeredResource != (void *)0x1234)
        return NV_ENC_ERR_RESOURCE_NOT_REGISTERED;
    params->mappedResource = (void *)0x5678;
    params->mappedBufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI fake_unmap_input_resource(void *encoder, NV_ENC_INPUT_PTR mapped_input)
{
    (void)encoder;
    return mapped_input == (void *)0x5678 ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PARAM;
}

static NVENCSTATUS NVENCAPI fake_encode_picture(void *encoder, NV_ENC_PIC_PARAMS *params)
{
    (void)encoder;
    if (!params)
        return NV_ENC_SUCCESS;
    if (!params->inputBuffer && (params->encodePicFlags & NV_ENC_PIC_FLAG_EOS))
        return NV_ENC_SUCCESS;
    if (params->frameIdx == 1)
        return NV_ENC_ERR_NEED_MORE_INPUT;
    if (params->frameIdx == 2)
        return NV_ENC_ERR_GENERIC;
    return params->inputBuffer ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PARAM;
}

static NVENCSTATUS NVENCAPI fake_create_input_buffer(void *encoder, NV_ENC_CREATE_INPUT_BUFFER *params)
{
    (void)encoder;
    if (!params)
        return NV_ENC_ERR_INVALID_PTR;
    params->inputBuffer = (void *)0x2345;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI fake_lock_input_buffer(void *encoder, NV_ENC_LOCK_INPUT_BUFFER *params)
{
    (void)encoder;
    if (!params)
        return NV_ENC_ERR_INVALID_PTR;
    params->bufferDataPtr = (void *)0x3456;
    params->pitch = 4096;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI fake_initialize_encoder(void *encoder, NV_ENC_INITIALIZE_PARAMS *params)
{
    (void)encoder;
    if (getenv("NVENC_AUDIT_SELFTEST_INIT_FAIL"))
        return NV_ENC_ERR_GENERIC;
    return params ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    if (!version)
        return NV_ENC_ERR_INVALID_PTR;
    *version = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *function_list)
{
    if (!function_list)
        return NV_ENC_ERR_INVALID_PTR;
    if (function_list->version != NV_ENCODE_API_FUNCTION_LIST_VER)
        return NV_ENC_ERR_INVALID_VERSION;

    function_list->nvEncInitializeEncoder = fake_initialize_encoder;
    function_list->nvEncCreateInputBuffer = fake_create_input_buffer;
    function_list->nvEncLockInputBuffer = fake_lock_input_buffer;
    function_list->nvEncEncodePicture = fake_encode_picture;
    function_list->nvEncMapInputResource = fake_map_input_resource;
    function_list->nvEncUnmapInputResource = fake_unmap_input_resource;
    function_list->nvEncRegisterResource = fake_register_resource;
    function_list->nvEncUnregisterResource = fake_unregister_resource;
    return NV_ENC_SUCCESS;
}
