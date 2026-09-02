#include "internal.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace voiceengine {
namespace {

typedef int CUresult;
typedef int CUdevice;

typedef CUresult (*cuInit_t)(unsigned int);
typedef CUresult (*cuDeviceGetCount_t)(int*);
typedef CUresult (*cuDeviceGet_t)(CUdevice*, int);
typedef CUresult (*cuDeviceGetName_t)(char*, int, CUdevice);
typedef CUresult (*cuDeviceComputeCapability_t)(int*, int*, CUdevice);
typedef CUresult (*cuDeviceTotalMem_t)(size_t*, CUdevice);

}  // namespace

bool cuda_query(ve_cuda_info* out, bool simulate_fail) {
    if (!out) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->api_version = VOICEENGINE_API_VERSION;
    out->available = 0;
    out->device_index = -1;

    if (simulate_fail) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "CUDA initialization failed (simulated). CPU ASR is not a product path.");
        return false;
    }

    HMODULE nvcuda = LoadLibraryW(L"nvcuda.dll");
    if (!nvcuda) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "nvcuda.dll not found. Install an NVIDIA driver. CPU ASR is disabled.");
        return false;
    }

    auto pInit = reinterpret_cast<cuInit_t>(GetProcAddress(nvcuda, "cuInit"));
    auto pCount = reinterpret_cast<cuDeviceGetCount_t>(GetProcAddress(nvcuda, "cuDeviceGetCount"));
    auto pGet = reinterpret_cast<cuDeviceGet_t>(GetProcAddress(nvcuda, "cuDeviceGet"));
    auto pName = reinterpret_cast<cuDeviceGetName_t>(GetProcAddress(nvcuda, "cuDeviceGetName"));
    auto pCc = reinterpret_cast<cuDeviceComputeCapability_t>(
        GetProcAddress(nvcuda, "cuDeviceComputeCapability"));
    auto pMem = reinterpret_cast<cuDeviceTotalMem_t>(GetProcAddress(nvcuda, "cuDeviceTotalMem_v2"));
    if (!pMem) {
        pMem = reinterpret_cast<cuDeviceTotalMem_t>(GetProcAddress(nvcuda, "cuDeviceTotalMem"));
    }
    if (!pInit || !pCount || !pGet || !pName || !pCc || !pMem) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "nvcuda.dll is missing required driver symbols. CPU ASR is disabled.");
        return false;
    }

    if (pInit(0) != 0) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "cuInit failed. CPU ASR is disabled.");
        return false;
    }
    int count = 0;
    if (pCount(&count) != 0 || count <= 0) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "No CUDA device. CPU ASR is disabled.");
        return false;
    }

    CUdevice dev = 0;
    if (pGet(&dev, 0) != 0) {
        std::snprintf(out->error_utf8, sizeof(out->error_utf8),
                      "cuDeviceGet failed. CPU ASR is disabled.");
        return false;
    }

    char name[256];
    name[0] = 0;
    pName(name, 255, dev);
    int major = 0;
    int minor = 0;
    pCc(&major, &minor, dev);
    size_t bytes = 0;
    pMem(&bytes, dev);

    out->available = 1;
    out->device_index = 0;
    out->compute_major = major;
    out->compute_minor = minor;
    out->vram_mib = static_cast<int32_t>(bytes / (1024 * 1024));
    std::snprintf(out->name, sizeof(out->name), "%s", name);
    return true;
}

}  // namespace voiceengine
