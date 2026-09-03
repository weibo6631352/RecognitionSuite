# 仓库与交付结构

## 结论

采用一个 Git monorepo，目录边界为 `apps/RecognitionStudio`、`engines/ScanEngine`、`engines/VoiceEngine`。旧仓库历史不再保留；跨组件 ABI 变更、验收资料和发布版本由一个提交原子化管理。

Git 合并不等于源码耦合。两个引擎继续生成独立 SDK，宿主继续只链接公开 C/C++ ABI，不直接包含引擎内部源码。验证通过的 SDK 基线提交在根 `sdk/`；顶层 tag 表示源码、SDK 基线和应用的唯一、可复现版本。

## 日常构建与 SDK 刷新

日常应用开发不重建引擎。`apps/RecognitionStudio` 直接消费：

- `sdk/ScanEngine/windows-x64/`
- `sdk/VoiceEngine/windows-x64/`

执行 `scripts/构建.ps1` 默认只构建 Studio。

只有 SDK 需要升级时才执行 `scripts/构建.ps1 -Target SDKs`：如果引擎源码有变化，先把它暂存；脚本会拒绝未暂存或未跟踪的引擎源码，避免 SDK 与构建输入不一致。该模式只请求两个引擎各自的 SDK 目标，不重复组装独立 GUI/CLI 运行包。随后脚本在两个引擎的 `build/<preset>/sdk/` 生成 staging，再调用内部发布器规范化模型、完成全量 SHA-256 校验并以可回滚方式更新根 SDK 基线。每份清单同时记录基准 `source_commit` 和实际暂存引擎子树的 `source_tree`；SDK 与对应源码必须在同一提交中审查。

`-Target Engines`、`ScanEngine` 和 `VoiceEngine` 只构建引擎本地 staging，不会发布根 SDK；只有 `SDKs` 和 `All` 会更新基线。

## 三类产物

### 引擎独立运行包

- `engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/bin/`
- `engines/VoiceEngine/VoiceEngineWindowsX64/`

用于引擎单独演示和诊断，是本地 staging，不是 RecognitionSuite 最终交付目录，也不进入 Git。

### 集成 SDK

- `sdk/ScanEngine/windows-x64/`
- `sdk/VoiceEngine/windows-x64/`

SDK 包含 `bin/`、`include/`、`lib/`、`cmake/`、`doc/`、`examples/`、`licenses/`、`SDK_MANIFEST.json` 和 `SHA256SUMS.txt`。清单记录 CUDA 12.9、Windows 驱动下限以及 RTX 40/50 的原生代码和 PTX 目标。它们是经验证、由 Git/LFS 跟踪的应用输入，不应整体复制给最终用户。

VoiceEngine 的完整主 GGUF 超过远端单文件限制，因此 SDK 基线保存两个已校验分片。完整 GGUF 不允许写入根 SDK；Studio 只在自己的忽略构建输出中原子拼接并校验它，最终运行包只保留完整 GGUF。

### 最终用户运行包

`apps/RecognitionStudio/build/windows-msvc-qt5/bin/` 是最终运行包的 staging。发布时应复制或压缩到 `artifacts/<version>/RecognitionStudio-windows-x64/`，生成校验和后上传发布制品库。运行包应包含：

- `RecognitionStudio.exe` 和根目录 Qt/MSVC 运行库；
- `components/voiceengine/` 与 `components/scanengine/` 两个相互隔离的 SDK 运行时；
- `licenses/voiceengine/` 与 `licenses/scanengine/`；
- 可写的 `output/`。

最终运行包不应包含 `.git/`、源码、SDK 的公共头文件、`lib/cmake/examples`、构建中间文件或测试数据。`components/scanengine/include/cccl` 与 `include/cuda` 是 MLX 在目标机执行 NVRTC JIT 所需的私有运行时数据，不是供应用开发者使用的 SDK 头文件，因此必须保留。两个组件都携带 Qt/CUDA 的部分重复文件，体积会较大；当前优先保持 DLL 搜索路径和 SDK 运行时隔离，不在首版做跨组件去重。

## 发布检查

在构建完成后运行：

```powershell
.\scripts\验证成果.ps1
```

该检查验证已提交 SDK 基线和最终运行包的必要文件、清单、私有 CUDA/FFmpeg 目录、模型、许可证及旧产品名残留。发布前执行：

```powershell
.\scripts\验证成果.ps1 -VerifySdkHashes -VerifyCudaArchitectures
```

其中架构检查使用 CUDA 12.9 的 `cuobjdump`，强制两颗 Core DLL 同时包含
RTX 40 (`sm_89`) 与 RTX 50 (`sm_120a`) 原生代码及对应 PTX。准确率 98%
和 40/50 真机推理验收不属于目录检查，需要冻结测试集、真值和统一计分规则
另行出具报告。
