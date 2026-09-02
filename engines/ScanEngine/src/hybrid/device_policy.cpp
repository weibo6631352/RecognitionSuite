#include "scanengine/hybrid/device_policy.hpp"
#include "scanengine/config.hpp"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>

#if defined(SCANENGINE_MLX)
#include <mlx/mlx.h>
namespace mx = mlx::core;
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scanengine {
namespace hybrid {
namespace {

constexpr size_t kGiB = size_t(1) << 30;

#if defined(_WIN32) && defined(SCANENGINE_MLX)
constexpr const wchar_t* kPrivateCudaDlls[] = {
    L"cudart64_12.dll",
    L"cublasLt64_12.dll",
    L"nvrtc64_120_0.dll",
    L"nvrtc-builtins64_129.dll",
    L"cudnn64_9.dll",
    L"cudnn_graph64_9.dll",
    L"cudnn_ops64_9.dll",
    L"cudnn_cnn64_9.dll",
    L"cudnn_adv64_9.dll",
    L"cudnn_heuristic64_9.dll",
    L"cudnn_engines_precompiled64_9.dll",
    L"cudnn_engines_runtime_compiled64_9.dll",
};

struct PrivateCudaRuntimeState {
    bool ready = false;
    QString error;
    QString directory;
    DLL_DIRECTORY_COOKIE cookie = nullptr;
};

PrivateCudaRuntimeState& privateCudaRuntimeState() {
    static PrivateCudaRuntimeState state;
    return state;
}

std::once_flag gPrivateCudaRuntimeOnce;

bool probePrivateDll(const QString& directory,
                     const wchar_t* fileName,
                     const char* symbol,
                     QString* error) {
    const QString path = QDir(directory).filePath(
        QString::fromWCharArray(fileName));
    HMODULE module = LoadLibraryExW(
        reinterpret_cast<LPCWSTR>(path.utf16()), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!module) {
        if (error) {
            *error = QStringLiteral("无法加载应用私有 GPU 运行库 %1（Windows 错误 %2）")
                         .arg(path)
                         .arg(GetLastError());
        }
        return false;
    }
    const bool found = GetProcAddress(module, symbol) != nullptr;
    FreeLibrary(module);
    if (!found && error) {
        *error = QStringLiteral("应用私有 GPU 运行库缺少入口 %1：%2")
                     .arg(QString::fromLatin1(symbol), path);
    }
    return found;
}
#endif

size_t hostPhysicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<size_t>(status.ullTotalPhys);
#endif
    return 0;
}

#if defined(SCANENGINE_MLX)
size_t deviceInfoSize(
    const std::unordered_map<std::string, std::variant<std::string, size_t>>& info,
    const char* key) {
    const auto found = info.find(key);
    if (found == info.end())
        return 0;
    if (const auto* value = std::get_if<size_t>(&found->second))
        return *value;
    return 0;
}

size_t gpuTotalMemoryBytes() {
    try {
        const mx::Device gpu(mx::Device::gpu);
        if (!mx::is_available(gpu))
            return 0;
        const auto& info = mx::device_info(gpu);
        const size_t total = deviceInfoSize(info, "total_memory");
        if (total)
            return total;
        return deviceInfoSize(info, "memory_size");
    } catch (...) {
        return 0;
    }
}
#endif

std::mutex gCacheLimitMutex;
size_t gAppliedCacheLimit = 0;

}  // namespace

DeviceMode deviceModeFromName(const QString& name, bool* ok) {
    const QString normalized = name.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QLatin1String("auto")
        || normalized == QLatin1String("gpu")
        || normalized == QLatin1String("gpu-required")
        || normalized == QLatin1String("gpurequired")) {
        if (ok)
            *ok = true;
        return DeviceMode::GpuRequired;
    }
    if (ok)
        *ok = false;
    return DeviceMode::GpuRequired;
}

QString deviceModeName(DeviceMode) {
    return QStringLiteral("gpu-required");
}

bool certifiedGpuBackendCompiled() {
#if defined(SCANENGINE_MLX)
    return true;
#else
    return false;
#endif
}

bool initializeHybridRuntime(QString* reason) {
#if defined(_WIN32) && defined(SCANENGINE_MLX)
    std::call_once(gPrivateCudaRuntimeOnce, [] {
        PrivateCudaRuntimeState& state = privateCudaRuntimeState();
        // Applications use their executable directory by default. SDK hosts
        // override this through scanengine_context_create(), so model and private
        // runtime lookup never accidentally follows the third-party host EXE.
        const QString root = scanengine::repoRoot();
        if (root.isEmpty()) {
            state.error = QStringLiteral("无法确定程序目录，未注册应用私有 CUDA 运行库");
            return;
        }
        state.directory = QDir(root).filePath(QStringLiteral("runtimes/cuda"));
        if (!QDir(state.directory).exists()) {
            state.error = QStringLiteral("缺少应用私有 CUDA 目录：%1")
                              .arg(state.directory);
            return;
        }
        for (const wchar_t* fileName : kPrivateCudaDlls) {
            const QString path = QDir(state.directory).filePath(
                QString::fromWCharArray(fileName));
            if (!QFileInfo(path).isFile()) {
                state.error = QStringLiteral("缺少应用私有 GPU 运行库：%1")
                                  .arg(path);
                return;
            }
        }
        if (!SetDefaultDllDirectories(
                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
            state.error = QStringLiteral("设置 Windows DLL 搜索策略失败（错误 %1）")
                              .arg(GetLastError());
            return;
        }
        state.cookie = AddDllDirectory(
            reinterpret_cast<LPCWSTR>(state.directory.utf16()));
        if (!state.cookie) {
            state.error = QStringLiteral("注册应用私有 CUDA 目录失败：%1（错误 %2）")
                              .arg(state.directory)
                              .arg(GetLastError());
            return;
        }
        state.ready = true;
    });
    const PrivateCudaRuntimeState& state = privateCudaRuntimeState();
    if (reason)
        *reason = state.error;
    return state.ready;
#else
    if (reason)
        reason->clear();
    return true;
#endif
}

bool certifiedGpuBackendReady(QString* reason) {
    if (!certifiedGpuBackendCompiled()) {
        if (reason) {
            *reason = QStringLiteral(
                "需要 GPU：此程序未包含认证的 GPU 推理后端");
        }
        return false;
    }
#if defined(_WIN32) && defined(SCANENGINE_MLX)
    QString runtimeError;
    if (!initializeHybridRuntime(&runtimeError)) {
        if (reason)
            *reason = runtimeError;
        return false;
    }
    const QString runtimeDirectory = privateCudaRuntimeState().directory;
    if (!probePrivateDll(runtimeDirectory, L"cublasLt64_12.dll",
                         "cublasLtCreate", &runtimeError)
        || !probePrivateDll(runtimeDirectory, L"nvrtc64_120_0.dll",
                            "nvrtcVersion", &runtimeError)
        || !probePrivateDll(runtimeDirectory, L"cudnn64_9.dll",
                            "cudnnGetVersion", &runtimeError)) {
        if (reason)
            *reason = runtimeError;
        return false;
    }
#endif
#if defined(SCANENGINE_MLX)
    try {
        const mx::Device gpu(mx::Device::gpu);
        if (!mx::is_available(gpu)) {
            if (reason)
                *reason = QStringLiteral("需要 GPU：MLX 未检测到可用的 GPU 设备");
            return false;
        }
    } catch (const std::exception& e) {
        if (reason) {
            *reason = QStringLiteral("GPU 初始化失败：%1")
                          .arg(QString::fromUtf8(e.what()));
        }
        return false;
    } catch (...) {
        if (reason)
            *reason = QStringLiteral("GPU 初始化失败：未知错误");
        return false;
    }
#endif
    if (reason)
        reason->clear();
    return true;
}

bool applyHybridDeviceMode(DeviceMode mode, int* fallbackCount, QString* err) {
    auto fail = [&](const QString& message) {
        if (err)
            *err = message;
        return false;
    };
    if (fallbackCount)
        *fallbackCount = 0;
    Q_UNUSED(mode);
    QString reason;
    if (!certifiedGpuBackendReady(&reason))
        return fail(reason);
    // Multi-crop pages overflow MLX's default 128-entry CUDA conv cache.
    if (qgetenv("MLX_CUDA_CONV_CACHE_SIZE").isEmpty())
        qputenv("MLX_CUDA_CONV_CACHE_SIZE", QByteArrayLiteral("2048"));
#if defined(SCANENGINE_MLX)
    const size_t cacheLimit = recommendedHybridGpuCacheLimit();
    mx::set_cache_limit(cacheLimit);
    {
        std::lock_guard<std::mutex> lock(gCacheLimitMutex);
        if (gAppliedCacheLimit != cacheLimit) {
            std::fprintf(stderr,
                         "gpu: cache_limit=%.1fGiB (vram=%.1fGiB ram=%.1fGiB)\n",
                         double(cacheLimit) / double(kGiB),
                         double(gpuTotalMemoryBytes()) / double(kGiB),
                         double(hostPhysicalMemoryBytes()) / double(kGiB));
        }
        gAppliedCacheLimit = cacheLimit;
    }
#endif
    if (err)
        err->clear();
    return true;
}

void releaseHybridRuntimeCache() {
#if defined(SCANENGINE_MLX)
    mx::clear_cache();
#endif
}

size_t recommendedHybridGpuCacheLimit() {
    const QByteArray overrideBytes = qgetenv("SCANENGINE_GPU_CACHE_LIMIT");
    if (!overrideBytes.isEmpty()) {
        bool ok = false;
        const qulonglong parsed =
            QString::fromLatin1(overrideBytes).toULongLong(&ok);
        if (ok && parsed > 0)
            return static_cast<size_t>(parsed);
    }

#if defined(SCANENGINE_MLX)
    size_t vram = gpuTotalMemoryBytes();
#else
    size_t vram = 0;
#endif
    const size_t ram = hostPhysicalMemoryBytes();
    if (vram == 0)
        vram = 8 * kGiB;

    // Unused MLX buffers only. Active tensors, weights, and cuDNN sit outside
    // this cap. Stay well under both VRAM and host RAM (WDDM can pin RAM).
    size_t cache = vram * 45 / 100;
    if (ram)
        cache = std::min(cache, ram * 30 / 100);

    size_t headroom = std::max(vram * 40 / 100, 8 * kGiB);
    if (vram <= 16 * kGiB)
        headroom = std::max(vram * 40 / 100, 4 * kGiB);
    if (cache + headroom > vram)
        cache = vram > headroom ? vram - headroom : vram / 4;

    size_t floor = kGiB;
    if (vram >= 24 * kGiB)
        floor = 8 * kGiB;
    else if (vram >= 12 * kGiB)
        floor = 4 * kGiB;
    floor = std::min(floor, vram * 30 / 100);
    if (cache < floor)
        cache = floor;
    return cache;
}

size_t appliedHybridGpuCacheLimit() {
    std::lock_guard<std::mutex> lock(gCacheLimitMutex);
    return gAppliedCacheLimit;
}

QString certifiedGpuBackendName() {
#if defined(SCANENGINE_MLX)
#if defined(_WIN32)
    return QStringLiteral("mlx-cuda");
#else
    return QStringLiteral("mlx-metal");
#endif
#else
    return QStringLiteral("none");
#endif
}

}  // namespace hybrid
}  // namespace scanengine
