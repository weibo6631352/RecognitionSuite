#pragma once

#include <QString>

namespace scanengine {
namespace hybrid {

enum class DeviceMode {
    GpuRequired,
};

DeviceMode deviceModeFromName(const QString& name, bool* ok = nullptr);
QString deviceModeName(DeviceMode mode);

// Compile-time certified tensor backend: Metal on macos-arm64-mlx, CUDA on
// win-qt5.12.9-msvc-mlx-cuda.
bool certifiedGpuBackendCompiled();

// Register app-private GPU runtime directories. Call at process startup before
// the first MLX operation. Repeated calls are harmless.
bool initializeHybridRuntime(QString* reason = nullptr);

// False when the binary, app-private runtime, or physical GPU is unavailable.
bool certifiedGpuBackendReady(QString* reason = nullptr);

// Product path is GPU only. auto/gpu/gpu-required are accepted aliases.
// Missing backend fails closed; there is no CPU or Python fallback.
bool applyHybridDeviceMode(DeviceMode mode, int* fallbackCount, QString* err);

QString certifiedGpuBackendName();

// Drop unused MLX GPU buffers after a page. GUI batch keeps one process;
// without this the CUDA allocator cache grows and later pages get slower.
void releaseHybridRuntimeCache();

// Unused-buffer cache budget from live GPU VRAM and host RAM.
// Leaves headroom for weights, activations, and the driver; not a 1 GiB floor
// and not the default 95% of VRAM.
size_t recommendedHybridGpuCacheLimit();
size_t appliedHybridGpuCacheLimit();

}  // namespace hybrid
}  // namespace scanengine
