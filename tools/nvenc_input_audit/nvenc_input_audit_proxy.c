#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ffnvcodec/nvEncodeAPI.h"

#if !defined(__x86_64__) || UINTPTR_MAX != UINT64_MAX
#error "The audited FFmpeg/NVENC ABI is pinned to Linux x86_64."
#endif

/* Layout signature for FFmpeg/nv-codec-headers n13.1.15.0 (NVENC API 13.1). */
_Static_assert(NVENCAPI_MAJOR_VERSION == 13, "Unexpected NVENC API major version");
_Static_assert(NVENCAPI_MINOR_VERSION == 1, "Unexpected NVENC API minor version");
_Static_assert(NV_ENCODE_API_FUNCTION_LIST_VER == 0x7102000dU,
               "Unexpected NVENC function-list version");
_Static_assert(sizeof(NV_ENCODE_API_FUNCTION_LIST) == 2552,
               "Unexpected NVENC function-list size");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncCreateInputBuffer) == 104,
               "Unexpected nvEncCreateInputBuffer offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncInitializeEncoder) == 96,
               "Unexpected nvEncInitializeEncoder offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncEncodePicture) == 136,
               "Unexpected nvEncEncodePicture offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncLockInputBuffer) == 160,
               "Unexpected nvEncLockInputBuffer offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncMapInputResource) == 208,
               "Unexpected nvEncMapInputResource offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnmapInputResource) == 216,
               "Unexpected nvEncUnmapInputResource offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRegisterResource) == 248,
               "Unexpected nvEncRegisterResource offset");
_Static_assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnregisterResource) == 256,
               "Unexpected nvEncUnregisterResource offset");
_Static_assert(sizeof(NV_ENC_PIC_PARAMS) == 3360,
               "Unexpected NV_ENC_PIC_PARAMS size");
_Static_assert(offsetof(NV_ENC_PIC_PARAMS, version) == 0,
               "Unexpected NV_ENC_PIC_PARAMS version offset");
_Static_assert(offsetof(NV_ENC_PIC_PARAMS, encodePicFlags) == 16,
               "Unexpected NV_ENC_PIC_PARAMS encodePicFlags offset");
_Static_assert(offsetof(NV_ENC_PIC_PARAMS, inputBuffer) == 40,
               "Unexpected NV_ENC_PIC_PARAMS inputBuffer offset");
_Static_assert(sizeof(NV_ENC_MAP_INPUT_RESOURCE) == 1544,
               "Unexpected NV_ENC_MAP_INPUT_RESOURCE size");
_Static_assert(offsetof(NV_ENC_MAP_INPUT_RESOURCE, version) == 0,
               "Unexpected NV_ENC_MAP_INPUT_RESOURCE version offset");
_Static_assert(offsetof(NV_ENC_MAP_INPUT_RESOURCE, registeredResource) == 16,
               "Unexpected NV_ENC_MAP_INPUT_RESOURCE registeredResource offset");
_Static_assert(offsetof(NV_ENC_MAP_INPUT_RESOURCE, mappedResource) == 24,
               "Unexpected NV_ENC_MAP_INPUT_RESOURCE mappedResource offset");
_Static_assert(sizeof(NV_ENC_REGISTER_RESOURCE) == 1536,
               "Unexpected NV_ENC_REGISTER_RESOURCE size");
_Static_assert(offsetof(NV_ENC_REGISTER_RESOURCE, version) == 0,
               "Unexpected NV_ENC_REGISTER_RESOURCE version offset");
_Static_assert(offsetof(NV_ENC_REGISTER_RESOURCE, resourceType) == 4,
               "Unexpected NV_ENC_REGISTER_RESOURCE resourceType offset");
_Static_assert(offsetof(NV_ENC_REGISTER_RESOURCE, registeredResource) == 32,
               "Unexpected NV_ENC_REGISTER_RESOURCE registeredResource offset");

typedef NVENCSTATUS (NVENCAPI *PFN_GET_MAX_SUPPORTED_VERSION)(uint32_t *version);
typedef NVENCSTATUS (NVENCAPI *PFN_CREATE_INSTANCE)(NV_ENCODE_API_FUNCTION_LIST *function_list);

#define MAX_INIT_RECORDS 8
#define MAX_TRACKED_RESOURCES 1024
#define MAX_TRACKED_MAPPED_INPUTS 2048

typedef struct ResourceRecord {
    void *encoder;
    NV_ENC_REGISTERED_PTR handle;
    int is_cuda_device_ptr;
    int active;
} ResourceRecord;

typedef struct MappedInputRecord {
    void *encoder;
    NV_ENC_INPUT_PTR handle;
    uint32_t active_maps;
    int all_maps_are_cuda_device_ptr;
    int active;
} MappedInputRecord;

typedef enum InputOrigin {
    INPUT_ORIGIN_UNKNOWN = 0,
    INPUT_ORIGIN_CUDA_DEVICE_PTR = 1,
    INPUT_ORIGIN_NON_CUDA = 2,
} InputOrigin;

typedef struct InitRecord {
    uint32_t params_version;
    uint32_t config_version;
    uint32_t rc_params_version;
    uint32_t encode_width;
    uint32_t encode_height;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint32_t average_bit_rate;
    uint32_t max_bit_rate;
    uint32_t vbv_buffer_size;
    uint32_t rate_control_mode;
    uint32_t const_qp_inter_p;
    uint32_t const_qp_inter_b;
    uint32_t const_qp_intra;
    uint32_t enable_min_qp;
    uint32_t min_qp_inter_p;
    uint32_t min_qp_intra;
    uint32_t enable_max_qp;
    uint32_t max_qp_inter_p;
    uint32_t max_qp_intra;
    uint32_t enable_initial_rc_qp;
    uint32_t initial_rc_qp_inter_p;
    uint32_t initial_rc_qp_intra;
    uint32_t target_quality;
    uint32_t target_quality_lsb;
    uint32_t enable_aq;
    uint32_t enable_temporal_aq;
    uint32_t multi_pass;
    uint32_t low_delay_key_frame_scale;
    uint32_t tuning_info;
    uint32_t codec_guid[4];
    uint32_t preset_guid[4];
    int config_present;
    int params_version_valid;
    int config_version_valid;
    int rc_params_version_valid;
    int32_t return_status;
} InitRecord;

static pthread_once_t g_real_library_once = PTHREAD_ONCE_INIT;
static void *g_real_library;
static PFN_GET_MAX_SUPPORTED_VERSION g_real_get_max_version;
static PFN_CREATE_INSTANCE g_real_create_instance;
static _Atomic(PNVENCINITIALIZEENCODER) g_real_initialize_encoder;
static _Atomic(PNVENCCREATEINPUTBUFFER) g_real_create_input_buffer;
static _Atomic(PNVENCLOCKINPUTBUFFER) g_real_lock_input_buffer;
static _Atomic(PNVENCREGISTERRESOURCE) g_real_register_resource;
static _Atomic(PNVENCUNREGISTERRESOURCE) g_real_unregister_resource;
static _Atomic(PNVENCMAPINPUTRESOURCE) g_real_map_input_resource;
static _Atomic(PNVENCUNMAPINPUTRESOURCE) g_real_unmap_input_resource;
static _Atomic(PNVENCENCODEPICTURE) g_real_encode_picture;

static _Atomic uint64_t g_api_create_calls;
static _Atomic uint64_t g_api_create_successes;
static _Atomic uint64_t g_version_mismatches;
static _Atomic uint64_t g_initialize_parameter_version_mismatches;
static _Atomic uint64_t g_library_load_failures;
static _Atomic uint64_t g_null_callback_tables;
static _Atomic uint64_t g_register_resource_calls;
static _Atomic uint64_t g_register_resource_successes;
static _Atomic uint64_t g_cuda_device_ptr_registrations;
static _Atomic uint64_t g_non_cuda_resource_registrations;
static _Atomic uint64_t g_invalid_register_parameters;
static _Atomic uint64_t g_register_parameter_version_mismatches;
static _Atomic uint64_t g_invalid_register_outputs;
static _Atomic uint64_t g_resource_tracking_overflows;
static _Atomic uint64_t g_unregister_resource_calls;
static _Atomic uint64_t g_unregister_resource_successes;
static _Atomic uint64_t g_untracked_unregister_calls;
static _Atomic uint64_t g_map_input_resource_calls;
static _Atomic uint64_t g_map_input_resource_successes;
static _Atomic uint64_t g_map_input_resource_failures;
static _Atomic uint64_t g_map_parameter_version_mismatches;
static _Atomic uint64_t g_cuda_device_ptr_maps;
static _Atomic uint64_t g_non_cuda_maps;
static _Atomic uint64_t g_unqualified_maps;
static _Atomic uint64_t g_mapping_tracking_overflows;
static _Atomic uint64_t g_unmap_input_resource_calls;
static _Atomic uint64_t g_unmap_input_resource_successes;
static _Atomic uint64_t g_untracked_unmap_calls;
static _Atomic uint64_t g_encode_picture_calls;
static _Atomic uint64_t g_picture_input_attempts;
static _Atomic uint64_t g_successful_picture_submissions;
static _Atomic uint64_t g_successful_cuda_picture_submissions;
static _Atomic uint64_t g_successful_non_cuda_or_unmapped_picture_submissions;
static _Atomic uint64_t g_cuda_picture_input_attempts;
static _Atomic uint64_t g_non_cuda_or_unmapped_picture_inputs;
static _Atomic uint64_t g_encode_picture_parameter_version_mismatches;
static _Atomic uint64_t g_encode_picture_failures;
static _Atomic uint64_t g_tracking_lock_failures;
static _Atomic uint64_t g_create_input_buffer_calls;
static _Atomic uint64_t g_lock_input_buffer_calls;
static _Atomic uint64_t g_initialize_encoder_calls;
static _Atomic uint64_t g_initialize_encoder_successes;
static _Atomic uint64_t g_initialize_encoder_failures;
static _Atomic uint64_t g_unqualified_initialize_successes;
static _Atomic uint64_t g_valid_initialize_successes;
static _Atomic uint32_t g_observed_function_list_version;
static _Atomic uint32_t g_real_max_api_version;
static pthread_mutex_t g_init_records_mutex = PTHREAD_MUTEX_INITIALIZER;
static InitRecord g_init_records[MAX_INIT_RECORDS];
static uint32_t g_init_record_count;
static pthread_mutex_t g_resource_tracking_mutex = PTHREAD_MUTEX_INITIALIZER;
static ResourceRecord g_resources[MAX_TRACKED_RESOURCES];
static MappedInputRecord g_mapped_inputs[MAX_TRACKED_MAPPED_INPUTS];

static const char *audit_real_library_path(void)
{
    const char *path = getenv("FLUXREC_NVENC_AUDIT_REAL_LIBRARY");
    return path && path[0] ? path : "/usr/lib/libnvidia-encode.so.1";
}

static void load_real_library(void)
{
    const char *path = audit_real_library_path();
    char canonical_real[PATH_MAX];
    char canonical_self[PATH_MAX];
    Dl_info self_info;

    if (!path || !path[0])
        goto failed;

    if (realpath(path, canonical_real) && dladdr((void *)NvEncodeAPICreateInstance, &self_info) &&
        self_info.dli_fname && realpath(self_info.dli_fname, canonical_self) &&
        strcmp(canonical_real, canonical_self) == 0) {
        /* The caller pointed the proxy back at itself. */
        goto failed;
    }

    g_real_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_real_library)
        goto failed;

    g_real_get_max_version = (PFN_GET_MAX_SUPPORTED_VERSION)
        dlsym(g_real_library, "NvEncodeAPIGetMaxSupportedVersion");
    g_real_create_instance = (PFN_CREATE_INSTANCE)
        dlsym(g_real_library, "NvEncodeAPICreateInstance");
    if (!g_real_get_max_version || !g_real_create_instance)
        goto failed;

    return;

failed:
    atomic_fetch_add_explicit(&g_library_load_failures, 1, memory_order_relaxed);
}

static void json_escape(char *destination, size_t capacity, const char *source)
{
    size_t used = 0;
    if (!capacity)
        return;

    for (const unsigned char *p = (const unsigned char *)(source ? source : ""); *p; ++p) {
        if ((*p == '"' || *p == '\\') && used + 2 < capacity) {
            destination[used++] = '\\';
            destination[used++] = (char)*p;
        } else if (*p < 0x20 && used + 6 < capacity) {
            int count = snprintf(destination + used, capacity - used, "\\u%04x", *p);
            if (count < 0)
                break;
            used += (size_t)count;
        } else if (used + 1 < capacity) {
            destination[used++] = (char)*p;
        } else {
            break;
        }
    }
    destination[used] = '\0';
}

static int guid_words(const GUID *guid, uint32_t words[4])
{
    if (!guid)
        return 0;
    words[0] = guid->Data1;
    words[1] = ((uint32_t)guid->Data2 << 16) | guid->Data3;
    words[2] = ((uint32_t)guid->Data4[0] << 24) | ((uint32_t)guid->Data4[1] << 16) |
               ((uint32_t)guid->Data4[2] << 8) | guid->Data4[3];
    words[3] = ((uint32_t)guid->Data4[4] << 24) | ((uint32_t)guid->Data4[5] << 16) |
               ((uint32_t)guid->Data4[6] << 8) | guid->Data4[7];
    return 1;
}

static int guid_equal(const GUID *left, const GUID *right)
{
    return left && right && left->Data1 == right->Data1 && left->Data2 == right->Data2 &&
           left->Data3 == right->Data3 && memcmp(left->Data4, right->Data4, sizeof(left->Data4)) == 0;
}

static const char *codec_name(const GUID *guid)
{
    if (guid_equal(guid, &NV_ENC_CODEC_H264_GUID))
        return "H264";
    if (guid_equal(guid, &NV_ENC_CODEC_HEVC_GUID))
        return "HEVC";
    if (guid_equal(guid, &NV_ENC_CODEC_AV1_GUID))
        return "AV1";
    return "unknown";
}

static const char *preset_name(const GUID *guid)
{
    static const GUID *const presets[] = {
        &NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID, &NV_ENC_PRESET_P3_GUID,
        &NV_ENC_PRESET_P4_GUID, &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID,
        &NV_ENC_PRESET_P7_GUID,
    };
    static const char *const names[] = { "P1", "P2", "P3", "P4", "P5", "P6", "P7" };
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); ++i) {
        if (guid_equal(guid, presets[i]))
            return names[i];
    }
    return "custom_or_other";
}

static const char *rate_control_name(uint32_t mode)
{
    switch (mode) {
    case NV_ENC_PARAMS_RC_CONSTQP: return "CONSTQP";
    case NV_ENC_PARAMS_RC_VBR: return "VBR";
    case NV_ENC_PARAMS_RC_CBR: return "CBR";
    default: return "unknown";
    }
}

static const char *tuning_name(uint32_t tuning)
{
    switch (tuning) {
    case NV_ENC_TUNING_INFO_HIGH_QUALITY: return "high_quality";
    case NV_ENC_TUNING_INFO_LOW_LATENCY: return "low_latency";
    case NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY: return "ultra_low_latency";
    case NV_ENC_TUNING_INFO_LOSSLESS: return "lossless";
    default: return "undefined_or_unknown";
    }
}

static const char *multi_pass_name(uint32_t multi_pass)
{
    switch (multi_pass) {
    case NV_ENC_MULTI_PASS_DISABLED: return "disabled";
    case NV_ENC_TWO_PASS_QUARTER_RESOLUTION: return "two_pass_quarter_resolution";
    case NV_ENC_TWO_PASS_FULL_RESOLUTION: return "two_pass_full_resolution";
    default: return "unknown";
    }
}

static void write_summary(void)
{
    const uint64_t create_calls = atomic_load_explicit(&g_api_create_calls, memory_order_relaxed);
    const uint64_t create_successes = atomic_load_explicit(&g_api_create_successes, memory_order_relaxed);
    const uint64_t version_mismatches = atomic_load_explicit(&g_version_mismatches, memory_order_relaxed);
    const uint64_t init_version_mismatches = atomic_load_explicit(
        &g_initialize_parameter_version_mismatches, memory_order_relaxed);
    const uint64_t load_failures = atomic_load_explicit(&g_library_load_failures, memory_order_relaxed);
    const uint64_t null_tables = atomic_load_explicit(&g_null_callback_tables, memory_order_relaxed);
    const uint64_t register_calls = atomic_load_explicit(&g_register_resource_calls, memory_order_relaxed);
    const uint64_t register_successes = atomic_load_explicit(&g_register_resource_successes, memory_order_relaxed);
    const uint64_t cuda_registrations = atomic_load_explicit(&g_cuda_device_ptr_registrations, memory_order_relaxed);
    const uint64_t other_registrations = atomic_load_explicit(&g_non_cuda_resource_registrations, memory_order_relaxed);
    const uint64_t invalid_registers = atomic_load_explicit(&g_invalid_register_parameters, memory_order_relaxed);
    const uint64_t register_version_mismatches = atomic_load_explicit(
        &g_register_parameter_version_mismatches, memory_order_relaxed);
    const uint64_t invalid_register_outputs = atomic_load_explicit(&g_invalid_register_outputs, memory_order_relaxed);
    const uint64_t resource_tracking_overflows = atomic_load_explicit(&g_resource_tracking_overflows, memory_order_relaxed);
    const uint64_t unregister_calls = atomic_load_explicit(&g_unregister_resource_calls, memory_order_relaxed);
    const uint64_t unregister_successes = atomic_load_explicit(&g_unregister_resource_successes, memory_order_relaxed);
    const uint64_t untracked_unregisters = atomic_load_explicit(&g_untracked_unregister_calls, memory_order_relaxed);
    const uint64_t map_calls = atomic_load_explicit(&g_map_input_resource_calls, memory_order_relaxed);
    const uint64_t map_successes = atomic_load_explicit(&g_map_input_resource_successes, memory_order_relaxed);
    const uint64_t map_failures = atomic_load_explicit(&g_map_input_resource_failures, memory_order_relaxed);
    const uint64_t map_version_mismatches = atomic_load_explicit(&g_map_parameter_version_mismatches, memory_order_relaxed);
    const uint64_t cuda_maps = atomic_load_explicit(&g_cuda_device_ptr_maps, memory_order_relaxed);
    const uint64_t other_maps = atomic_load_explicit(&g_non_cuda_maps, memory_order_relaxed);
    const uint64_t unqualified_maps = atomic_load_explicit(&g_unqualified_maps, memory_order_relaxed);
    const uint64_t mapping_tracking_overflows = atomic_load_explicit(&g_mapping_tracking_overflows, memory_order_relaxed);
    const uint64_t unmap_calls = atomic_load_explicit(&g_unmap_input_resource_calls, memory_order_relaxed);
    const uint64_t unmap_successes = atomic_load_explicit(&g_unmap_input_resource_successes, memory_order_relaxed);
    const uint64_t untracked_unmaps = atomic_load_explicit(&g_untracked_unmap_calls, memory_order_relaxed);
    const uint64_t encode_calls = atomic_load_explicit(&g_encode_picture_calls, memory_order_relaxed);
    const uint64_t picture_input_attempts = atomic_load_explicit(&g_picture_input_attempts, memory_order_relaxed);
    const uint64_t successful_picture_submissions = atomic_load_explicit(&g_successful_picture_submissions, memory_order_relaxed);
    const uint64_t successful_cuda_picture_submissions = atomic_load_explicit(&g_successful_cuda_picture_submissions, memory_order_relaxed);
    const uint64_t successful_other_picture_submissions = atomic_load_explicit(
        &g_successful_non_cuda_or_unmapped_picture_submissions, memory_order_relaxed);
    const uint64_t cuda_picture_attempts = atomic_load_explicit(&g_cuda_picture_input_attempts, memory_order_relaxed);
    const uint64_t other_picture_inputs = atomic_load_explicit(&g_non_cuda_or_unmapped_picture_inputs, memory_order_relaxed);
    const uint64_t picture_version_mismatches = atomic_load_explicit(
        &g_encode_picture_parameter_version_mismatches, memory_order_relaxed);
    const uint64_t encode_failures = atomic_load_explicit(&g_encode_picture_failures, memory_order_relaxed);
    const uint64_t tracking_lock_failures = atomic_load_explicit(&g_tracking_lock_failures, memory_order_relaxed);
    const uint64_t create_input_calls = atomic_load_explicit(&g_create_input_buffer_calls, memory_order_relaxed);
    const uint64_t lock_input_calls = atomic_load_explicit(&g_lock_input_buffer_calls, memory_order_relaxed);
    const uint64_t initialize_calls = atomic_load_explicit(&g_initialize_encoder_calls, memory_order_relaxed);
    const uint64_t initialize_successes = atomic_load_explicit(&g_initialize_encoder_successes, memory_order_relaxed);
    const uint64_t initialize_failures = atomic_load_explicit(&g_initialize_encoder_failures, memory_order_relaxed);
    const uint64_t unqualified_initialize_successes = atomic_load_explicit(
        &g_unqualified_initialize_successes, memory_order_relaxed);
    const uint64_t valid_initialize_successes = atomic_load_explicit(
        &g_valid_initialize_successes, memory_order_relaxed);
    const uint32_t observed_version = atomic_load_explicit(&g_observed_function_list_version, memory_order_relaxed);
    const uint32_t real_max_version = atomic_load_explicit(&g_real_max_api_version, memory_order_relaxed);
    const int initialized = create_successes > 0;
    const int abi_ok = version_mismatches == 0 && init_version_mismatches == 0 &&
                       register_version_mismatches == 0 && map_version_mismatches == 0 &&
                       picture_version_mismatches == 0 &&
                       null_tables == 0 && load_failures == 0;
    const int software_branch = create_input_calls > 0 || lock_input_calls > 0;
    char escaped_library[PATH_MAX * 2];
    char escaped_audit_run_id[512];
    const char *audit_run_id = getenv("FLUXREC_ZERO_CPU_AUDIT_RUN_ID");
    if (!audit_run_id || !audit_run_id[0])
        audit_run_id = "unset";
    json_escape(escaped_library, sizeof(escaped_library), audit_real_library_path());
    json_escape(escaped_audit_run_id, sizeof(escaped_audit_run_id), audit_run_id);

    InitRecord init_records[MAX_INIT_RECORDS];
    uint32_t init_record_count;
    pthread_mutex_lock(&g_init_records_mutex);
    init_record_count = g_init_record_count;
    memcpy(init_records, g_init_records, init_record_count * sizeof(init_records[0]));
    pthread_mutex_unlock(&g_init_records_mutex);

    const int initialization_ok = initialize_calls > 0 && initialize_successes > 0 &&
                                  initialize_failures == 0 && unqualified_initialize_successes == 0 &&
                                  valid_initialize_successes > 0;
    const int passed = initialized && init_record_count > 0 && initialization_ok && abi_ok &&
                       cuda_registrations > 0 && register_successes > 0 && cuda_maps > 0 &&
                       other_registrations == 0 && invalid_registers == 0 &&
                       invalid_register_outputs == 0 && resource_tracking_overflows == 0 &&
                       mapping_tracking_overflows == 0 && tracking_lock_failures == 0 &&
                       successful_picture_submissions > 0 &&
                       successful_cuda_picture_submissions == successful_picture_submissions &&
                       successful_other_picture_submissions == 0 && other_picture_inputs == 0 &&
                       create_input_calls == 0 && lock_input_calls == 0;
    const char *status = create_calls == 0 ? "incomplete" : (passed ? "pass" : "fail");

    char json[16384];
    int length = snprintf(
        json, sizeof(json),
        "{\n"
        "  \"schema\": \"fluxrec.nvenc_input_audit.v1\",\n"
        "  \"status\": \"%s\",\n"
        "  \"pass\": %s,\n"
        "  \"process_pid\": %ld,\n"
        "  \"audit_run_id\": \"%s\",\n"
        "  \"header\": {\"nvenc_api\": \"%u.%u\", \"function_list_version\": \"0x%08x\", \"function_list_size\": %zu, \"initialize_encoder_offset\": %zu, \"encode_picture_offset\": %zu, \"create_input_buffer_offset\": %zu, \"lock_input_buffer_offset\": %zu, \"map_input_resource_offset\": %zu, \"unmap_input_resource_offset\": %zu, \"register_resource_offset\": %zu, \"unregister_resource_offset\": %zu, \"pic_params_size\": %zu, \"map_input_resource_size\": %zu},\n"
        "  \"observed_function_list_version\": \"0x%08x\",\n"
        "  \"real_driver_max_api_version\": \"0x%08x\",\n"
        "  \"real_library\": \"%s\",\n"
        "  \"counts\": {\"api_create_calls\": %llu, \"api_create_successes\": %llu, \"function_list_version_mismatches\": %llu, \"initialize_parameter_version_mismatches\": %llu, \"real_library_load_failures\": %llu, \"missing_callback_tables\": %llu, \"register_resource_calls\": %llu, \"register_resource_successes\": %llu, \"cuda_device_ptr_registrations\": %llu, \"non_cuda_resource_registrations\": %llu, \"invalid_register_parameters\": %llu, \"register_parameter_version_mismatches\": %llu, \"invalid_register_outputs\": %llu, \"resource_tracking_overflows\": %llu, \"unregister_resource_calls\": %llu, \"unregister_resource_successes\": %llu, \"untracked_unregister_calls\": %llu, \"map_input_resource_calls\": %llu, \"map_input_resource_successes\": %llu, \"map_input_resource_failures\": %llu, \"map_parameter_version_mismatches\": %llu, \"cuda_device_ptr_maps\": %llu, \"non_cuda_maps\": %llu, \"unqualified_maps\": %llu, \"mapping_tracking_overflows\": %llu, \"unmap_input_resource_calls\": %llu, \"unmap_input_resource_successes\": %llu, \"untracked_unmap_calls\": %llu, \"encode_picture_calls\": %llu, \"picture_input_attempts\": %llu, \"successful_picture_submissions\": %llu, \"successful_cuda_device_ptr_picture_submissions\": %llu, \"successful_non_cuda_or_unmapped_picture_submissions\": %llu, \"cuda_device_ptr_picture_input_attempts\": %llu, \"non_cuda_or_unmapped_picture_inputs\": %llu, \"encode_picture_parameter_version_mismatches\": %llu, \"encode_picture_failures\": %llu, \"tracking_lock_failures\": %llu, \"initialize_encoder_calls\": %llu, \"initialize_encoder_successes\": %llu, \"initialize_encoder_failures\": %llu, \"valid_initialize_successes\": %llu, \"unqualified_initialize_successes\": %llu, \"initialize_records_stored\": %u, \"create_input_buffer_calls\": %llu, \"lock_input_buffer_calls\": %llu},\n"
        "  \"cuda_device_pointer_input_observed\": %s,\n"
        "  \"cuda_device_pointer_picture_input_observed\": %s,\n"
        "  \"non_cuda_or_unmapped_picture_input_observed\": %s,\n"
        "  \"software_input_branch_observed\": %s,\n"
        "  \"layout_guard\": \"%s\",\n"
        "  \"encoder_initializations\": [",
        status, passed ? "true" : "false",
        (long)getpid(), escaped_audit_run_id,
        NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, NV_ENCODE_API_FUNCTION_LIST_VER,
        sizeof(NV_ENCODE_API_FUNCTION_LIST),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncInitializeEncoder),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncEncodePicture),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncCreateInputBuffer),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncLockInputBuffer),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncMapInputResource),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnmapInputResource),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRegisterResource),
        offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnregisterResource),
        sizeof(NV_ENC_PIC_PARAMS), sizeof(NV_ENC_MAP_INPUT_RESOURCE),
        observed_version, real_max_version, escaped_library,
        (unsigned long long)create_calls, (unsigned long long)create_successes,
        (unsigned long long)version_mismatches, (unsigned long long)init_version_mismatches,
        (unsigned long long)load_failures,
        (unsigned long long)null_tables, (unsigned long long)register_calls,
        (unsigned long long)register_successes,
        (unsigned long long)cuda_registrations, (unsigned long long)other_registrations,
        (unsigned long long)invalid_registers,
        (unsigned long long)register_version_mismatches,
        (unsigned long long)invalid_register_outputs,
        (unsigned long long)resource_tracking_overflows,
        (unsigned long long)unregister_calls, (unsigned long long)unregister_successes,
        (unsigned long long)untracked_unregisters,
        (unsigned long long)map_calls, (unsigned long long)map_successes,
        (unsigned long long)map_failures, (unsigned long long)map_version_mismatches,
        (unsigned long long)cuda_maps, (unsigned long long)other_maps,
        (unsigned long long)unqualified_maps, (unsigned long long)mapping_tracking_overflows,
        (unsigned long long)unmap_calls, (unsigned long long)unmap_successes,
        (unsigned long long)untracked_unmaps,
        (unsigned long long)encode_calls, (unsigned long long)picture_input_attempts,
        (unsigned long long)successful_picture_submissions,
        (unsigned long long)successful_cuda_picture_submissions,
        (unsigned long long)successful_other_picture_submissions,
        (unsigned long long)cuda_picture_attempts, (unsigned long long)other_picture_inputs,
        (unsigned long long)picture_version_mismatches, (unsigned long long)encode_failures,
        (unsigned long long)tracking_lock_failures,
        (unsigned long long)initialize_calls, (unsigned long long)initialize_successes,
        (unsigned long long)initialize_failures, (unsigned long long)valid_initialize_successes,
        (unsigned long long)unqualified_initialize_successes, init_record_count,
        (unsigned long long)create_input_calls,
        (unsigned long long)lock_input_calls,
        cuda_registrations > 0 ? "true" : "false",
        cuda_picture_attempts > 0 ? "true" : "false",
        other_picture_inputs > 0 ? "true" : "false",
        software_branch ? "true" : "false",
        abi_ok ? "passed" : "failed");
    if (length < 0 || (size_t)length >= sizeof(json))
        return;

    size_t used = (size_t)length;
    for (uint32_t i = 0; i < init_record_count; ++i) {
        const InitRecord *record = &init_records[i];
        const char *codec = "unknown";
        const char *preset = "custom_or_other";
        const char *rate_control = "unknown";
        const char *tuning = "undefined_or_unknown";
        GUID codec_guid = { record->codec_guid[0], (uint16_t)(record->codec_guid[1] >> 16),
                            (uint16_t)record->codec_guid[1],
                            { (uint8_t)(record->codec_guid[2] >> 24),
                              (uint8_t)(record->codec_guid[2] >> 16),
                              (uint8_t)(record->codec_guid[2] >> 8),
                              (uint8_t)record->codec_guid[2],
                              (uint8_t)(record->codec_guid[3] >> 24),
                              (uint8_t)(record->codec_guid[3] >> 16),
                              (uint8_t)(record->codec_guid[3] >> 8),
                              (uint8_t)record->codec_guid[3] } };
        GUID preset_guid = { record->preset_guid[0], (uint16_t)(record->preset_guid[1] >> 16),
                             (uint16_t)record->preset_guid[1],
                             { (uint8_t)(record->preset_guid[2] >> 24),
                               (uint8_t)(record->preset_guid[2] >> 16),
                               (uint8_t)(record->preset_guid[2] >> 8),
                               (uint8_t)record->preset_guid[2],
                               (uint8_t)(record->preset_guid[3] >> 24),
                               (uint8_t)(record->preset_guid[3] >> 16),
                               (uint8_t)(record->preset_guid[3] >> 8),
                               (uint8_t)record->preset_guid[3] } };
        codec = codec_name(&codec_guid);
        preset = preset_name(&preset_guid);
        rate_control = rate_control_name(record->rate_control_mode);
        tuning = tuning_name(record->tuning_info);
        int count = snprintf(
            json + used, sizeof(json) - used,
            "%s\n    {\"return_status\": %d, \"params_version\": \"0x%08x\", \"params_version_valid\": %s, \"config_present\": %s, \"config_version\": \"0x%08x\", \"config_version_valid\": %s, \"rc_params_version\": \"0x%08x\", \"rc_params_version_valid\": %s, \"codec\": \"%s\", \"preset\": \"%s\", \"tuning\": \"%s\", \"rate_control_mode\": \"%s\", \"average_bit_rate\": %u, \"max_bit_rate\": %u, \"vbv_buffer_size\": %u, \"rate_control_details\": {\"valid\": %s, \"const_qp_p\": %u, \"const_qp_b\": %u, \"const_qp_i\": %u, \"enable_min_qp\": %u, \"min_qp_p\": %u, \"min_qp_i\": %u, \"enable_max_qp\": %u, \"max_qp_p\": %u, \"max_qp_i\": %u, \"enable_initial_rc_qp\": %u, \"initial_rc_qp_p\": %u, \"initial_rc_qp_i\": %u, \"target_quality\": %u, \"target_quality_lsb\": %u, \"enable_aq\": %u, \"enable_temporal_aq\": %u, \"multi_pass\": %u, \"multi_pass_mode\": \"%s\", \"low_delay_key_frame_scale\": %u}, \"frame_rate\": {\"numerator\": %u, \"denominator\": %u}, \"size\": {\"width\": %u, \"height\": %u}, \"codec_guid_words\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"], \"preset_guid_words\": [\"0x%08x\", \"0x%08x\", \"0x%08x\", \"0x%08x\"]}",
            i ? "," : "", record->return_status, record->params_version,
            record->params_version_valid ? "true" : "false", record->config_present ? "true" : "false",
            record->config_version,
            record->config_version_valid ? "true" : "false", record->rc_params_version,
            record->rc_params_version_valid ? "true" : "false", codec, preset, tuning,
            rate_control, record->average_bit_rate, record->max_bit_rate,
            record->vbv_buffer_size, record->rc_params_version_valid ? "true" : "false",
            record->const_qp_inter_p, record->const_qp_inter_b, record->const_qp_intra,
            record->enable_min_qp, record->min_qp_inter_p, record->min_qp_intra,
            record->enable_max_qp, record->max_qp_inter_p, record->max_qp_intra,
            record->enable_initial_rc_qp, record->initial_rc_qp_inter_p,
            record->initial_rc_qp_intra, record->target_quality, record->target_quality_lsb,
            record->enable_aq, record->enable_temporal_aq, record->multi_pass,
            multi_pass_name(record->multi_pass), record->low_delay_key_frame_scale,
            record->frame_rate_num, record->frame_rate_den,
            record->encode_width, record->encode_height,
            record->codec_guid[0], record->codec_guid[1], record->codec_guid[2], record->codec_guid[3],
            record->preset_guid[0], record->preset_guid[1], record->preset_guid[2], record->preset_guid[3]);
        if (count < 0 || (size_t)count >= sizeof(json) - used)
            return;
        used += (size_t)count;
    }
    static const char json_end[] = "\n  ]\n}\n";
    if (used + sizeof(json_end) - 1 >= sizeof(json))
        return;
    memcpy(json + used, json_end, sizeof(json_end) - 1);
    used += sizeof(json_end) - 1;

    const char *output_path = getenv("FLUXREC_NVENC_AUDIT_JSON");
    if (output_path && output_path[0]) {
        char temporary_path[PATH_MAX];
        int path_length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.%ld",
                                   output_path, (long)getpid());
        if (path_length > 0 && (size_t)path_length < sizeof(temporary_path)) {
            int fd = open(temporary_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
            if (fd >= 0) {
                size_t written = 0;
                while (written < used) {
                    ssize_t count = write(fd, json + written, used - written);
                    if (count < 0 && errno == EINTR)
                        continue;
                    if (count <= 0)
                        break;
                    written += (size_t)count;
                }
                (void)fsync(fd);
                (void)close(fd);
                if (written == used)
                    (void)rename(temporary_path, output_path);
                else
                    (void)unlink(temporary_path);
                return;
            }
        }
    }

    static const char stderr_prefix[] = "FLUXREC_NVENC_INPUT_AUDIT ";
    (void)write(STDERR_FILENO, stderr_prefix, sizeof(stderr_prefix) - 1);
    (void)write(STDERR_FILENO, json, used);
}

static NVENCSTATUS NVENCAPI audit_initialize_encoder(
    void *encoder, NV_ENC_INITIALIZE_PARAMS *params)
{
    atomic_fetch_add_explicit(&g_initialize_encoder_calls, 1, memory_order_relaxed);
    PNVENCINITIALIZEENCODER original = atomic_load_explicit(&g_real_initialize_encoder,
                                                             memory_order_acquire);
    if (!original) {
        atomic_fetch_add_explicit(&g_initialize_encoder_failures, 1, memory_order_relaxed);
        return NV_ENC_ERR_INVALID_VERSION;
    }

    InitRecord record = { 0 };
    /* A preset-only initialize call is valid; absent config/RC blocks need no layout read. */
    record.config_version_valid = 1;
    record.rc_params_version_valid = 1;
    if (params) {
        record.params_version = params->version;
        record.params_version_valid = params->version == NV_ENC_INITIALIZE_PARAMS_VER;
        if (record.params_version_valid) {
            record.encode_width = params->encodeWidth;
            record.encode_height = params->encodeHeight;
            record.frame_rate_num = params->frameRateNum;
            record.frame_rate_den = params->frameRateDen;
            record.tuning_info = (uint32_t)params->tuningInfo;
            (void)guid_words(&params->encodeGUID, record.codec_guid);
            (void)guid_words(&params->presetGUID, record.preset_guid);
            if (params->encodeConfig) {
                record.config_present = 1;
                record.config_version = params->encodeConfig->version;
                record.config_version_valid = record.config_version == NV_ENC_CONFIG_VER;
                if (record.config_version_valid) {
                    const NV_ENC_RC_PARAMS *rc = &params->encodeConfig->rcParams;
                    record.rc_params_version = rc->version;
                    /* FFmpeg copies this embedded RC block from the driver's preset config.
                     * API 13.1 drivers may leave its nested version word zero; the enclosing
                     * NV_ENC_CONFIG version guards the block layout. */
                    record.rc_params_version_valid = record.rc_params_version == 0 ||
                                                     record.rc_params_version == NV_ENC_RC_PARAMS_VER;
                    if (record.rc_params_version_valid) {
                        record.rate_control_mode = (uint32_t)rc->rateControlMode;
                        record.average_bit_rate = rc->averageBitRate;
                        record.max_bit_rate = rc->maxBitRate;
                        record.vbv_buffer_size = rc->vbvBufferSize;
                        record.const_qp_inter_p = rc->constQP.qpInterP;
                        record.const_qp_inter_b = rc->constQP.qpInterB;
                        record.const_qp_intra = rc->constQP.qpIntra;
                        record.enable_min_qp = rc->enableMinQP;
                        record.min_qp_inter_p = rc->minQP.qpInterP;
                        record.min_qp_intra = rc->minQP.qpIntra;
                        record.enable_max_qp = rc->enableMaxQP;
                        record.max_qp_inter_p = rc->maxQP.qpInterP;
                        record.max_qp_intra = rc->maxQP.qpIntra;
                        record.enable_initial_rc_qp = rc->enableInitialRCQP;
                        record.initial_rc_qp_inter_p = rc->initialRCQP.qpInterP;
                        record.initial_rc_qp_intra = rc->initialRCQP.qpIntra;
                        record.target_quality = rc->targetQuality;
                        record.target_quality_lsb = rc->targetQualityLSB;
                        record.enable_aq = rc->enableAQ;
                        record.enable_temporal_aq = rc->enableTemporalAQ;
                        record.multi_pass = (uint32_t)rc->multiPass;
                        record.low_delay_key_frame_scale = rc->lowDelayKeyFrameScale;
                    }
                }
            }
        }
    }

    if (!record.params_version_valid || !record.config_version_valid ||
        !record.rc_params_version_valid) {
        atomic_fetch_add_explicit(&g_initialize_parameter_version_mismatches, 1,
                                  memory_order_relaxed);
    }

    const NVENCSTATUS status = original(encoder, params);
    record.return_status = (int32_t)status;
    if (status == NV_ENC_SUCCESS) {
        atomic_fetch_add_explicit(&g_initialize_encoder_successes, 1, memory_order_relaxed);
        if (record.params_version_valid && record.config_present && record.config_version_valid &&
            record.rc_params_version_valid) {
            atomic_fetch_add_explicit(&g_valid_initialize_successes, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_unqualified_initialize_successes, 1, memory_order_relaxed);
        }
    } else {
        atomic_fetch_add_explicit(&g_initialize_encoder_failures, 1, memory_order_relaxed);
    }
    pthread_mutex_lock(&g_init_records_mutex);
    if (g_init_record_count < MAX_INIT_RECORDS)
        g_init_records[g_init_record_count++] = record;
    pthread_mutex_unlock(&g_init_records_mutex);
    return status;
}

static InputOrigin lookup_resource_origin(void *encoder, NV_ENC_REGISTERED_PTR handle)
{
    if (!handle || pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return INPUT_ORIGIN_UNKNOWN;
    }

    InputOrigin origin = INPUT_ORIGIN_UNKNOWN;
    for (size_t i = 0; i < MAX_TRACKED_RESOURCES; ++i) {
        if (g_resources[i].active && g_resources[i].encoder == encoder &&
            g_resources[i].handle == handle) {
            origin = g_resources[i].is_cuda_device_ptr
                ? INPUT_ORIGIN_CUDA_DEVICE_PTR : INPUT_ORIGIN_NON_CUDA;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
    return origin;
}

static void remember_resource(void *encoder, NV_ENC_REGISTERED_PTR handle, int is_cuda_device_ptr)
{
    if (!handle) {
        atomic_fetch_add_explicit(&g_invalid_register_outputs, 1, memory_order_relaxed);
        return;
    }
    if (pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return;
    }

    ResourceRecord *available = NULL;
    for (size_t i = 0; i < MAX_TRACKED_RESOURCES; ++i) {
        if (g_resources[i].active && g_resources[i].encoder == encoder &&
            g_resources[i].handle == handle) {
            available = &g_resources[i];
            break;
        }
        if (!g_resources[i].active && !available)
            available = &g_resources[i];
    }
    if (available) {
        available->encoder = encoder;
        available->handle = handle;
        available->is_cuda_device_ptr = is_cuda_device_ptr;
        available->active = 1;
    } else {
        atomic_fetch_add_explicit(&g_resource_tracking_overflows, 1, memory_order_relaxed);
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
}

static int forget_resource(void *encoder, NV_ENC_REGISTERED_PTR handle)
{
    int found = 0;
    if (!handle || pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return 0;
    }
    for (size_t i = 0; i < MAX_TRACKED_RESOURCES; ++i) {
        if (g_resources[i].active && g_resources[i].encoder == encoder &&
            g_resources[i].handle == handle) {
            memset(&g_resources[i], 0, sizeof(g_resources[i]));
            found = 1;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
    return found;
}

static void remember_mapping(void *encoder, NV_ENC_INPUT_PTR handle, InputOrigin origin)
{
    if (!handle) {
        atomic_fetch_add_explicit(&g_unqualified_maps, 1, memory_order_relaxed);
        return;
    }
    if (pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return;
    }

    MappedInputRecord *available = NULL;
    for (size_t i = 0; i < MAX_TRACKED_MAPPED_INPUTS; ++i) {
        if (g_mapped_inputs[i].active && g_mapped_inputs[i].encoder == encoder &&
            g_mapped_inputs[i].handle == handle) {
            available = &g_mapped_inputs[i];
            break;
        }
        if (!g_mapped_inputs[i].active && !available)
            available = &g_mapped_inputs[i];
    }
    if (!available) {
        atomic_fetch_add_explicit(&g_mapping_tracking_overflows, 1, memory_order_relaxed);
    } else if (!available->active) {
        available->encoder = encoder;
        available->handle = handle;
        available->active_maps = 1;
        available->all_maps_are_cuda_device_ptr = origin == INPUT_ORIGIN_CUDA_DEVICE_PTR;
        available->active = 1;
    } else if (available->active_maps == UINT32_MAX) {
        atomic_fetch_add_explicit(&g_mapping_tracking_overflows, 1, memory_order_relaxed);
        available->all_maps_are_cuda_device_ptr = 0;
    } else {
        ++available->active_maps;
        if (origin != INPUT_ORIGIN_CUDA_DEVICE_PTR)
            available->all_maps_are_cuda_device_ptr = 0;
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
}

static InputOrigin lookup_mapping_origin(void *encoder, NV_ENC_INPUT_PTR handle)
{
    if (!handle || pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return INPUT_ORIGIN_UNKNOWN;
    }

    InputOrigin origin = INPUT_ORIGIN_UNKNOWN;
    for (size_t i = 0; i < MAX_TRACKED_MAPPED_INPUTS; ++i) {
        if (g_mapped_inputs[i].active && g_mapped_inputs[i].encoder == encoder &&
            g_mapped_inputs[i].handle == handle) {
            origin = g_mapped_inputs[i].all_maps_are_cuda_device_ptr
                ? INPUT_ORIGIN_CUDA_DEVICE_PTR : INPUT_ORIGIN_NON_CUDA;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
    return origin;
}

static int forget_mapping(void *encoder, NV_ENC_INPUT_PTR handle)
{
    int found = 0;
    if (!handle || pthread_mutex_lock(&g_resource_tracking_mutex) != 0) {
        atomic_fetch_add_explicit(&g_tracking_lock_failures, 1, memory_order_relaxed);
        return 0;
    }
    for (size_t i = 0; i < MAX_TRACKED_MAPPED_INPUTS; ++i) {
        if (g_mapped_inputs[i].active && g_mapped_inputs[i].encoder == encoder &&
            g_mapped_inputs[i].handle == handle) {
            if (g_mapped_inputs[i].active_maps > 1) {
                --g_mapped_inputs[i].active_maps;
            } else {
                memset(&g_mapped_inputs[i], 0, sizeof(g_mapped_inputs[i]));
            }
            found = 1;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_resource_tracking_mutex);
    return found;
}

static int encode_status_accepted(NVENCSTATUS status)
{
    return status == NV_ENC_SUCCESS || status == NV_ENC_ERR_NEED_MORE_INPUT;
}

static NVENCSTATUS NVENCAPI audit_map_input_resource(
    void *encoder, NV_ENC_MAP_INPUT_RESOURCE *params)
{
    PNVENCMAPINPUTRESOURCE original = atomic_load_explicit(&g_real_map_input_resource,
                                                            memory_order_acquire);
    atomic_fetch_add_explicit(&g_map_input_resource_calls, 1, memory_order_relaxed);
    InputOrigin origin = INPUT_ORIGIN_UNKNOWN;
    int version_valid = params && params->version == NV_ENC_MAP_INPUT_RESOURCE_VER;
    if (version_valid)
        origin = lookup_resource_origin(encoder, params->registeredResource);
    else
        atomic_fetch_add_explicit(&g_map_parameter_version_mismatches, 1, memory_order_relaxed);

    const NVENCSTATUS status = original
        ? original(encoder, params) : NV_ENC_ERR_INVALID_VERSION;
    if (status != NV_ENC_SUCCESS) {
        atomic_fetch_add_explicit(&g_map_input_resource_failures, 1, memory_order_relaxed);
        return status;
    }

    atomic_fetch_add_explicit(&g_map_input_resource_successes, 1, memory_order_relaxed);
    if (!version_valid || !params->mappedResource)
        origin = INPUT_ORIGIN_UNKNOWN;
    if (origin == INPUT_ORIGIN_CUDA_DEVICE_PTR)
        atomic_fetch_add_explicit(&g_cuda_device_ptr_maps, 1, memory_order_relaxed);
    else if (origin == INPUT_ORIGIN_NON_CUDA)
        atomic_fetch_add_explicit(&g_non_cuda_maps, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&g_unqualified_maps, 1, memory_order_relaxed);
    if (version_valid && params->mappedResource)
        remember_mapping(encoder, params->mappedResource, origin);
    return status;
}

static NVENCSTATUS NVENCAPI audit_unmap_input_resource(
    void *encoder, NV_ENC_INPUT_PTR mapped_input_buffer)
{
    PNVENCUNMAPINPUTRESOURCE original = atomic_load_explicit(&g_real_unmap_input_resource,
                                                              memory_order_acquire);
    atomic_fetch_add_explicit(&g_unmap_input_resource_calls, 1, memory_order_relaxed);
    const NVENCSTATUS status = original
        ? original(encoder, mapped_input_buffer) : NV_ENC_ERR_INVALID_VERSION;
    if (status == NV_ENC_SUCCESS) {
        atomic_fetch_add_explicit(&g_unmap_input_resource_successes, 1, memory_order_relaxed);
        if (!forget_mapping(encoder, mapped_input_buffer))
            atomic_fetch_add_explicit(&g_untracked_unmap_calls, 1, memory_order_relaxed);
    }
    return status;
}

static NVENCSTATUS NVENCAPI audit_unregister_resource(
    void *encoder, NV_ENC_REGISTERED_PTR registered_resource)
{
    PNVENCUNREGISTERRESOURCE original = atomic_load_explicit(&g_real_unregister_resource,
                                                              memory_order_acquire);
    atomic_fetch_add_explicit(&g_unregister_resource_calls, 1, memory_order_relaxed);
    const NVENCSTATUS status = original
        ? original(encoder, registered_resource) : NV_ENC_ERR_INVALID_VERSION;
    if (status == NV_ENC_SUCCESS) {
        atomic_fetch_add_explicit(&g_unregister_resource_successes, 1, memory_order_relaxed);
        if (!forget_resource(encoder, registered_resource))
            atomic_fetch_add_explicit(&g_untracked_unregister_calls, 1, memory_order_relaxed);
    }
    return status;
}

static NVENCSTATUS NVENCAPI audit_encode_picture(
    void *encoder, NV_ENC_PIC_PARAMS *params)
{
    PNVENCENCODEPICTURE original = atomic_load_explicit(&g_real_encode_picture,
                                                         memory_order_acquire);
    atomic_fetch_add_explicit(&g_encode_picture_calls, 1, memory_order_relaxed);

    int is_picture = 0;
    InputOrigin origin = INPUT_ORIGIN_UNKNOWN;
    if (params) {
        if (params->version == NV_ENC_PIC_PARAMS_VER) {
            const int flush = !params->inputBuffer &&
                              (params->encodePicFlags & NV_ENC_PIC_FLAG_EOS) != 0;
            is_picture = !flush;
            if (is_picture) {
                atomic_fetch_add_explicit(&g_picture_input_attempts, 1, memory_order_relaxed);
                origin = params->inputBuffer
                    ? lookup_mapping_origin(encoder, params->inputBuffer) : INPUT_ORIGIN_UNKNOWN;
            }
        } else {
            atomic_fetch_add_explicit(&g_encode_picture_parameter_version_mismatches, 1,
                                      memory_order_relaxed);
            is_picture = 1;
            atomic_fetch_add_explicit(&g_picture_input_attempts, 1, memory_order_relaxed);
        }
    }

    if (is_picture) {
        if (origin == INPUT_ORIGIN_CUDA_DEVICE_PTR)
            atomic_fetch_add_explicit(&g_cuda_picture_input_attempts, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&g_non_cuda_or_unmapped_picture_inputs, 1, memory_order_relaxed);
    }

    const NVENCSTATUS status = original
        ? original(encoder, params) : NV_ENC_ERR_INVALID_VERSION;
    if (is_picture && encode_status_accepted(status)) {
        atomic_fetch_add_explicit(&g_successful_picture_submissions, 1, memory_order_relaxed);
        if (origin == INPUT_ORIGIN_CUDA_DEVICE_PTR)
            atomic_fetch_add_explicit(&g_successful_cuda_picture_submissions, 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&g_successful_non_cuda_or_unmapped_picture_submissions, 1,
                                      memory_order_relaxed);
    } else if (is_picture) {
        atomic_fetch_add_explicit(&g_encode_picture_failures, 1, memory_order_relaxed);
    }
    return status;
}

static NVENCSTATUS NVENCAPI audit_create_input_buffer(
    void *encoder, NV_ENC_CREATE_INPUT_BUFFER *params)
{
    PNVENCCREATEINPUTBUFFER original = atomic_load_explicit(&g_real_create_input_buffer,
                                                            memory_order_acquire);
    atomic_fetch_add_explicit(&g_create_input_buffer_calls, 1, memory_order_relaxed);
    return original ? original(encoder, params) : NV_ENC_ERR_INVALID_VERSION;
}

static NVENCSTATUS NVENCAPI audit_lock_input_buffer(
    void *encoder, NV_ENC_LOCK_INPUT_BUFFER *params)
{
    PNVENCLOCKINPUTBUFFER original = atomic_load_explicit(&g_real_lock_input_buffer,
                                                          memory_order_acquire);
    atomic_fetch_add_explicit(&g_lock_input_buffer_calls, 1, memory_order_relaxed);
    return original ? original(encoder, params) : NV_ENC_ERR_INVALID_VERSION;
}

static NVENCSTATUS NVENCAPI audit_register_resource(
    void *encoder, NV_ENC_REGISTER_RESOURCE *params)
{
    PNVENCREGISTERRESOURCE original = atomic_load_explicit(&g_real_register_resource,
                                                            memory_order_acquire);
    atomic_fetch_add_explicit(&g_register_resource_calls, 1, memory_order_relaxed);
    int params_version_valid = params && params->version == NV_ENC_REGISTER_RESOURCE_VER;
    int is_cuda_device_ptr = 0;
    if (!params) {
        atomic_fetch_add_explicit(&g_invalid_register_parameters, 1, memory_order_relaxed);
    } else if (!params_version_valid) {
        atomic_fetch_add_explicit(&g_register_parameter_version_mismatches, 1,
                                  memory_order_relaxed);
    } else if (params->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR) {
        is_cuda_device_ptr = 1;
        atomic_fetch_add_explicit(&g_cuda_device_ptr_registrations, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_non_cuda_resource_registrations, 1, memory_order_relaxed);
    }
    const NVENCSTATUS status = original
        ? original(encoder, params) : NV_ENC_ERR_INVALID_VERSION;
    if (status == NV_ENC_SUCCESS && params && params_version_valid) {
        atomic_fetch_add_explicit(&g_register_resource_successes, 1, memory_order_relaxed);
        remember_resource(encoder, params->registeredResource, is_cuda_device_ptr);
    }
    return status;
}

__attribute__((visibility("default")))
NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    pthread_once(&g_real_library_once, load_real_library);
    if (!g_real_get_max_version)
        return NV_ENC_ERR_NO_ENCODE_DEVICE;

    const NVENCSTATUS status = g_real_get_max_version(version);
    if (status == NV_ENC_SUCCESS && version)
        atomic_store_explicit(&g_real_max_api_version, *version, memory_order_relaxed);
    return status;
}

__attribute__((visibility("default")))
NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *function_list)
{
    atomic_fetch_add_explicit(&g_api_create_calls, 1, memory_order_relaxed);
    pthread_once(&g_real_library_once, load_real_library);
    if (!g_real_create_instance)
        return NV_ENC_ERR_NO_ENCODE_DEVICE;
    if (!function_list)
        return NV_ENC_ERR_INVALID_PTR;

    atomic_store_explicit(&g_observed_function_list_version, function_list->version,
                          memory_order_relaxed);
    if (function_list->version != NV_ENCODE_API_FUNCTION_LIST_VER) {
        atomic_fetch_add_explicit(&g_version_mismatches, 1, memory_order_relaxed);
        return NV_ENC_ERR_INVALID_VERSION;
    }

    const NVENCSTATUS status = g_real_create_instance(function_list);
    if (status != NV_ENC_SUCCESS)
        return status;

    if (!function_list->nvEncCreateInputBuffer || !function_list->nvEncLockInputBuffer ||
        !function_list->nvEncRegisterResource || !function_list->nvEncUnregisterResource ||
        !function_list->nvEncMapInputResource || !function_list->nvEncUnmapInputResource ||
        !function_list->nvEncEncodePicture || !function_list->nvEncInitializeEncoder) {
        atomic_fetch_add_explicit(&g_null_callback_tables, 1, memory_order_relaxed);
        return NV_ENC_ERR_INVALID_VERSION;
    }

    atomic_store_explicit(&g_real_create_input_buffer, function_list->nvEncCreateInputBuffer,
                          memory_order_release);
    atomic_store_explicit(&g_real_initialize_encoder, function_list->nvEncInitializeEncoder,
                          memory_order_release);
    atomic_store_explicit(&g_real_lock_input_buffer, function_list->nvEncLockInputBuffer,
                          memory_order_release);
    atomic_store_explicit(&g_real_register_resource, function_list->nvEncRegisterResource,
                          memory_order_release);
    atomic_store_explicit(&g_real_unregister_resource, function_list->nvEncUnregisterResource,
                          memory_order_release);
    atomic_store_explicit(&g_real_map_input_resource, function_list->nvEncMapInputResource,
                          memory_order_release);
    atomic_store_explicit(&g_real_unmap_input_resource, function_list->nvEncUnmapInputResource,
                          memory_order_release);
    atomic_store_explicit(&g_real_encode_picture, function_list->nvEncEncodePicture,
                          memory_order_release);
    function_list->nvEncCreateInputBuffer = audit_create_input_buffer;
    function_list->nvEncInitializeEncoder = audit_initialize_encoder;
    function_list->nvEncLockInputBuffer = audit_lock_input_buffer;
    function_list->nvEncRegisterResource = audit_register_resource;
    function_list->nvEncUnregisterResource = audit_unregister_resource;
    function_list->nvEncMapInputResource = audit_map_input_resource;
    function_list->nvEncUnmapInputResource = audit_unmap_input_resource;
    function_list->nvEncEncodePicture = audit_encode_picture;

    atomic_fetch_add_explicit(&g_api_create_successes, 1, memory_order_relaxed);
    return status;
}

__attribute__((destructor)) static void nvenc_input_audit_unload(void)
{
    write_summary();
    if (g_real_library) {
        (void)dlclose(g_real_library);
        g_real_library = NULL;
    }
}
