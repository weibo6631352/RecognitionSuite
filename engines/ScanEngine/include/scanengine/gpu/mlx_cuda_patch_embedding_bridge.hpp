#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scanengine {
namespace gpu {

// Narrow host ABI for the first Qwen2-VL vision operator.  This header is
// deliberately free of MLX/CUDA/Qt types: scanengine_core remains C++17 and
// never includes an MLX header, while the compiled implementation is C++20.
struct MlxCudaPatchEmbeddingRequest {
    const float* patches = nullptr;           // [patchCount, patchInner]
    std::size_t patchElementCount = 0;
    int patchCount = 0;
    int patchInner = 0;

    const std::uint16_t* weightBf16 = nullptr; // [outputDim, patchInner]
    std::size_t weightElementCount = 0;
    int outputDim = 0;

    int deviceIndex = 0;
};

struct MlxCudaPatchEmbeddingResult {
    std::vector<float> output;                 // BF16 result expanded to F32
    int outputRows = 0;
    int outputColumns = 0;

    int gpuDeviceCount = 0;
    int deviceIndex = -1;
    int computeCapabilityMajor = 0;
    int computeCapabilityMinor = 0;
    std::string deviceName;
    std::string architecture;
    std::string cublasLtModulePath;

    bool graphScheduledOnGpu = false;
    bool patchBufferDeviceResident = false;
    bool weightBufferDeviceResident = false;
    bool outputBufferDeviceResident = false;
    bool outputDtypeBf16 = false;
    bool outputFinite = false;

    // These counters describe bridge-owned host-to-device edges, not graph
    // operations.  A successful call has exactly one edge for each input.
    int patchUploadCount = 0;
    int weightUploadCount = 0;
    std::size_t patchUploadBytes = 0;
    std::size_t weightUploadBytes = 0;
    std::size_t peakMlxBytes = 0;
};

bool runMlxCudaPatchEmbedding(
    const MlxCudaPatchEmbeddingRequest& request,
    MlxCudaPatchEmbeddingResult* result,
    std::string* error);

}  // namespace gpu
}  // namespace scanengine
