# 仓库与交付结构

## 结论

采用一个 Git monorepo，目录边界为 `apps/RecognitionStudio`、`engines/ScanEngine`、`engines/VoiceEngine`。旧仓库历史不再保留；跨组件 ABI 变更、验收资料和发布版本由一个提交原子化管理。

Git 合并不等于源码耦合。两个引擎继续生成独立 SDK，宿主继续只链接公开 C/C++ ABI，不直接包含引擎内部源码。顶层 tag 表示整套系统唯一、可复现的版本。

## 构建顺序

1. `engines/ScanEngine` 生成 `build/win-qt5.12.9-msvc-mlx-cuda/sdk/`。
2. `engines/VoiceEngine` 生成 `build/win-qt5.12.9-msvc-cuda/sdk/`。
3. `apps/RecognitionStudio` 从上述两个 SDK 构建最终应用。

顶层 `scripts/build.ps1` 固化了该顺序。子仓库仍可单独构建和测试。

## 三类产物

### 引擎独立运行包

- `engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/bin/`
- `engines/VoiceEngine/VoiceEngineWindowsX64/`

用于引擎单独演示和诊断，不是 RecognitionSuite 最终交付目录。

### 集成 SDK

- `engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk/`
- `engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk/`

SDK 应包含 `bin/`、`include/`、`lib/`、`cmake/`、`doc/`、`examples/` 和 `licenses/`。SDK 用于开发集成，不应整体复制给最终用户。

### 最终用户运行包

最终交付内容是 `apps/RecognitionStudio/build/windows-msvc-qt5/bin/`。它应包含：

- `RecognitionStudio.exe` 和根目录 Qt/MSVC 运行库；
- `components/voiceengine/` 与 `components/scanengine/` 两个相互隔离的 SDK 运行时；
- `licenses/voiceengine/` 与 `licenses/scanengine/`；
- 可写的 `output/`。

最终运行包不应包含 `.git/`、源码、SDK 的 `include/lib/cmake/examples`、构建中间文件或测试数据。两个组件都携带 Qt/CUDA 的部分重复文件，体积会较大；当前优先保持 DLL 搜索路径和 SDK 运行时隔离，不在首版做跨组件去重。

## 发布检查

在构建完成后运行：

```powershell
.\scripts\verify-artifacts.ps1
```

该检查验证两个 SDK 和最终运行包的必要文件、私有 CUDA/FFmpeg 目录、模型、许可证及旧产品名残留。准确率 98% 不属于目录检查，需要冻结测试集、真值和统一计分规则另行出具报告。
