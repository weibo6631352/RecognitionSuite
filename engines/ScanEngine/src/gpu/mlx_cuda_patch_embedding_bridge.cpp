#include "scanengine/gpu/mlx_cuda_patch_embedding_bridge.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mlx/backend/cuda/allocator.h>
#include <mlx/array.h>
#include <mlx/memory.h>
#include <mlx/mlx.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace scanengine {
namespace gpu {

namespace mx = mlx::core;

namespace {

class DefaultDeviceGuard {
public:
    DefaultDeviceGuard() : previous_(mx::default_device()) {}
    ~DefaultDeviceGuard() { mx::set_default_device(previous_); }

    DefaultDeviceGuard(const DefaultDeviceGuard&) = delete;
    DefaultDeviceGuard& operator=(const DefaultDeviceGuard&) = delete;

private:
    mx::Device previous_;
};

bool isDeviceResident(const mx::array& value, int expectedDevice) {
    const auto* buffer = static_cast<const mx::cu::CudaBuffer*>(
        value.buffer().ptr());
    if (buffer == nullptr || buffer->data == nullptr)
        return false;
    // MLX's CUDA allocator records device=-1 for unified/managed buffers.
    // Those are still CUDA-resident and GPU-addressable.
    return buffer->device == expectedDevice || buffer->device == -1;
}

template <typename T>
T deviceInfoValue(
    const std::unordered_map<std::string, std::variant<std::string, size_t>>& info,
    std::string_view key,
    T fallback) {
    const auto found = info.find(std::string(key));
    if (found == info.end())
        return fallback;
    if (const auto* value = std::get_if<T>(&found->second))
        return *value;
    return fallback;
}

std::string loadedModulePath(const wchar_t* moduleName) {
    const HMODULE module = ::GetModuleHandleW(moduleName);
    if (module == nullptr)
        return {};
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    const auto utf8 = std::filesystem::path(path).generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

}  // namespace

bool runMlxCudaPatchEmbedding(
    const MlxCudaPatchEmbeddingRequest& request,
    MlxCudaPatchEmbeddingResult* result,
    std::string* error) {
    auto fail = [&](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };
    if (!result)
        return fail("MLX CUDA patch embedding result is null");
    *result = {};

    try {
        if (!request.patches || !request.weightBf16)
            return fail("MLX CUDA patch embedding input pointer is null");
        if (request.patchCount <= 0 || request.patchInner <= 0
            || request.outputDim <= 0) {
            return fail("MLX CUDA patch embedding dimensions must be positive");
        }

        const std::size_t patchElements =
            std::size_t(request.patchCount) * std::size_t(request.patchInner);
        const std::size_t weightElements =
            std::size_t(request.outputDim) * std::size_t(request.patchInner);
        if (patchElements != request.patchElementCount
            || weightElements != request.weightElementCount) {
            return fail("MLX CUDA patch embedding element count mismatch");
        }
        if (request.patchCount > std::numeric_limits<std::int32_t>::max()
            || request.patchInner > std::numeric_limits<std::int32_t>::max()
            || request.outputDim > std::numeric_limits<std::int32_t>::max()) {
            return fail("MLX CUDA patch embedding shape exceeds MLX limits");
        }

        const std::span<const float> patches(
            request.patches, request.patchElementCount);
        const std::span<const std::uint16_t> rawWeight(
            request.weightBf16, request.weightElementCount);

        result->gpuDeviceCount = mx::device_count(mx::Device::gpu);
        if (request.deviceIndex < 0
            || request.deviceIndex >= result->gpuDeviceCount) {
            return fail("requested MLX CUDA device is not available");
        }
        const mx::Device device(mx::Device::gpu, request.deviceIndex);
        if (!mx::is_available(device))
            return fail("MLX reports the requested CUDA device unavailable");

        DefaultDeviceGuard defaultDevice;
        mx::set_default_device(device);
        result->deviceIndex = request.deviceIndex;

        const auto& deviceInfo = mx::device_info(device);
        result->deviceName = deviceInfoValue<std::string>(
            deviceInfo, "device_name", {});
        result->architecture = deviceInfoValue<std::string>(
            deviceInfo, "architecture", {});
        result->computeCapabilityMajor = static_cast<int>(
            deviceInfoValue<size_t>(deviceInfo, "compute_capability_major", 0));
        result->computeCapabilityMinor = static_cast<int>(
            deviceInfoValue<size_t>(deviceInfo, "compute_capability_minor", 0));

        // Preserve the raw safetensors BF16 payload bit-for-bit.  Converting a
        // uint16_t numerically would corrupt it, so populate MLX's BF16 storage
        // explicitly before the single device copy.
        std::vector<mx::bfloat16_t> weightValues(rawWeight.size());
        for (std::size_t index = 0; index < rawWeight.size(); ++index)
            weightValues[index].bits_ = rawWeight[index];

        mx::reset_peak_memory();
        mx::array hostPatches(
            patches.begin(), {request.patchCount, request.patchInner}, mx::float32);
        mx::array hostWeight(
            weightValues.begin(), {request.outputDim, request.patchInner},
            mx::bfloat16);

        // Exactly two bridge-owned host-to-device edges.  The formal image
        // tensor remains F32 until the official dtype cast on the GPU; the
        // formal patch projection weight remains raw BF16 throughout.
        mx::array devicePatches = mx::copy(hostPatches, device);
        ++result->patchUploadCount;
        result->patchUploadBytes = patches.size_bytes();
        mx::array deviceWeight = mx::copy(hostWeight, device);
        ++result->weightUploadCount;
        result->weightUploadBytes = rawWeight.size_bytes();
        mx::eval({devicePatches, deviceWeight});

        result->patchBufferDeviceResident =
            isDeviceResident(devicePatches, request.deviceIndex);
        result->weightBufferDeviceResident =
            isDeviceResident(deviceWeight, request.deviceIndex);

        const mx::array patchesBf16 =
            mx::astype(devicePatches, mx::bfloat16, device);
        const mx::array weightTranspose =
            mx::transpose(deviceWeight, {1, 0}, device);
        mx::array output = mx::matmul(patchesBf16, weightTranspose, device);
        result->outputDtypeBf16 = output.dtype() == mx::bfloat16;
        result->outputRows = output.shape(0);
        result->outputColumns = output.shape(1);

        const bool hadGpuPrimitive = output.has_primitive();
        mx::eval(output);
        result->graphScheduledOnGpu = hadGpuPrimitive &&
            isDeviceResident(output, request.deviceIndex);
        result->outputBufferDeviceResident =
            isDeviceResident(output, request.deviceIndex);
        result->peakMlxBytes = mx::get_peak_memory();
        result->cublasLtModulePath = loadedModulePath(L"cublasLt64_12.dll");

        // Residency is captured above, before raw_ptr() performs the explicit
        // diagnostic readback to host-visible memory.
        const mx::bfloat16_t* outputData = output.data<mx::bfloat16_t>();
        result->output.resize(output.size());
        result->outputFinite = true;
        for (std::size_t index = 0; index < output.size(); ++index) {
            const float value = static_cast<float>(outputData[index]);
            result->output[index] = value;
            result->outputFinite = result->outputFinite && std::isfinite(value);
        }

        if (error)
            error->clear();
        return true;
    } catch (const std::exception& exception) {
        return fail(std::string("MLX CUDA patch embedding failed: ")
                    + exception.what());
    } catch (...) {
        return fail("MLX CUDA patch embedding failed with an unknown exception");
    }
}

}  // namespace gpu
}  // namespace scanengine
