# ScanEngine

C++ / Qt 桌面程序：把文档图片解析成 Excel。不要 Python。

流水线：版面（PP-DocLayoutV2）→ 表格方向 → Qwen2-VL 抽表 → OCR → 拼页 → 写出 xlsx。

| 平台 | 推理 | 说明 |
|---|---|---|
| Windows x64 | 预编译 MLX 0.31.1 + CUDA | 正式目标为 RTX 40（sm_89）与 RTX 50（sm_120）系列 |
| macOS arm64 | 官方 libmlx Metal | 同一套业务，Qt 5.15.2 |

没有认证 GPU 后端时程序直接失败，没有 CPU 产品路径。

---

## 目标平台

### Windows（正式）

- 系统：Windows 10/11 **64 位**
- CPU：与开发机同级即可（当前锁定 AMD Ryzen 9 9950）
- GPU：NVIDIA GeForce RTX 40 系列（`sm_89`）或 RTX 50 系列（`sm_120`）
- 驱动：安装与对应 GPU/CUDA 12.9 匹配的 NVIDIA 驱动（提供 `nvcuda.dll`）
- 不支持：32 位、MinGW、ARM Windows、无 NVIDIA GPU

### macOS

- 本机 **Apple Silicon arm64**
- 正式构建最低 **macOS 26**（锁定 MLX Metal）
- 开发用无 MLX preset 可在 macOS 14 上编，但不能当 GPU 产品跑

---

## 运行一份已经打好的包

打包目录默认是桌面 `ScanEngineWindowsGpu`，或 `build\win-qt5.12.9-msvc-mlx-cuda\bin\`。

**运行机需要**

- Windows 10/11 x64
- RTX 40/50 系列 GPU及对应 NVIDIA 驱动

**不用再装**

- Visual Studio、Qt SDK、CMake、CUDA Toolkit、cuDNN、Python

**包里已经带上**

- `ScanEngine.exe`（界面）、`ScanEngineTool.exe`（命令行）
- Qt / VC 运行库、`pdfium.dll`、`onnxruntime.dll`
- `runtimes\cuda\`：cublas / cuDNN / NVRTC 等（程序只从这里加载，不搜系统 PATH）
- `include\cccl`、`include\cuda`：运行时 JIT 头文件
- `models\`：VLM / 版面 / OCR / fastText / Magika
- `启动.bat` 或 `run.bat`（会设置 `MLX_CUDA_CONV_CACHE_SIZE=2048`）

用法：双击 `启动.bat` / `run.bat`，或直接开 `ScanEngine.exe`。把 PNG/JPEG/BMP/TIFF/WebP 拖进去或 `Ctrl+V` 粘贴。命令行：

```bat
ScanEngineTool.exe parse --input 某图.png
```

结果默认在包内 `output\YYYYMMDD-HHMMSS_文件名\`：源图副本、同级 xlsx、`work\` 过程文件。

---

## 开发环境（要改代码、重新编译）

构建机和运行机可以是同一台。构建机 **额外** 需要下面工具链；MLX 源码不用，仓库里已有编好的 `mlx.lib`。

| 组件 | 锁定版本 / 路径 |
|---|---|
| Git + Git LFS | 克隆后执行 `git lfs pull` |
| Visual Studio 2022 | v143 x64（Build Tools 即可，不要 MinGW） |
| Qt | `C:\Qt\Qt5.12.9\5.12.9\msvc2017_64` |
| CMake | ≥ 3.25（常用 3.30.5，可用 `C:\Qt\Tools\CMake_64`） |
| CUDA Toolkit | 12.9，默认装在 `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9` |
| cuDNN | 9.9.0.52，头文件和 `cudnn64_9.dll` 放进上述 CUDA 根目录 |
| 预编译 MLX | `third_party\windows-x64\mlx\lib\mlx.lib`（仓库自带） |

装工具链（推荐）：

1. 双击 `script\windows\start.cmd`
2. 选 **1 下载离线包**（VS / CMake / CUDA / cuDNN zip / Qt 官方安装包，包里没有的都下）
3. 把 `script\windows\offline\` 拷到离线机（若需要）
4. 选 **2 离线安装**（不访问网络；Qt 安装器里勾 **msvc2017 64-bit**，根目录 `C:\Qt`）

Mac 开发：锁定原生 arm64 Qt 5.15.2，见 `third_party/SCANENGINE_MACOS_QT_LOCK.json`。

---

## 构建与打包

### Windows

脚本：`script\windows\start.cmd` → 选 **3 构建并打包**。  
默认拷到桌面 `ScanEngineWindowsGpu`。

命令行：

```bat
cmake --preset win-qt5.12.9-msvc-mlx-cuda
cmake --build --preset win-qt5.12.9-msvc-mlx-cuda
```

产物：`build\win-qt5.12.9-msvc-mlx-cuda\bin\`（exe、Qt、模型、`runtimes\cuda`）。

仓库不再提供 Windows 无 GPU preset；Windows 配置阶段会拒绝关闭 MLX CUDA。

### macOS

```bash
cmake --preset macos-arm64-mlx
cmake --build build/macos-arm64-mlx
```

产物：`build/macos-arm64-mlx/bin/`。

---

## 仓库里有什么

| 路径 | 作用 |
|---|---|
| `src/` `include/` | 业务与界面 |
| `models/` | 官方权重（大文件 Git LFS） |
| `third_party/windows-x64/` | pdfium、ORT、预编译 MLX CUDA |
| `third_party/macos-arm64/` | pdfium、ORT、libmlx |
| `third_party/common/` | QXlsx、libjpeg、fastText 等源码 |
| `script/windows/` | 下载 / 安装 / 构建打包 |
| `docs/CONTRACT.md` | 作业目录、Excel、界面约定 |
| `docs/HYBRID.md` | 解析步骤 |
| `docs/WINDOWS.md` | Windows 工具链摘要 |

构建不从网上下依赖。VLM 权重因 LFS 单文件限制拆成 `model.safetensors.part1/part2`，程序按一个文件读。

Magika（代码语言）用锁定的 ONNX Runtime 1.28.0 CPU、batch=1，只在碰到代码块时加载，不进 GPU。
