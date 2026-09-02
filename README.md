# RecognitionSuite

纸质扫描与语音识别系统 monorepo。桌面应用、两套引擎、共同需求、验收样本和发布检查在一次提交中原子化版本管理。

## 仓库关系

```text
RecognitionSuite
├─ apps/
│  └─ RecognitionStudio                 最终桌面程序，只调用公开 SDK
├─ engines/
│  ├─ ScanEngine                        文档/表格识别产品与 SDK
│  └─ VoiceEngine                       音频/麦克风识别产品与 SDK
├─ docs/                                需求、技术总结、发布约定
├─ testdata/                            冻结验收输入与人工参考
└─ scripts/                             联合构建和产物检查
```

两套引擎仍是独立构建和发布边界，`RecognitionStudio` 仍只消费公开 SDK；但 Git 只保留一个根仓库，从而让跨组件 ABI 调整、验收数据和最终发布始终落在同一个可复现提交中。

## 获取源码

```powershell
git clone <RecognitionSuite 仓库地址>
cd RecognitionSuite
git lfs install
git lfs pull
```

当前仓库尚未配置远端。配置新的 RecognitionSuite 远端后，只需维护这一处 Git/LFS 仓库。

## 构建

Windows 构建机需要 VS 2022/v143、Qt 5.12.9 MSVC x64 和 CUDA 12.9。按依赖顺序构建全部组件：

```powershell
.\scripts\build.ps1
```

只检查仓库状态或现有产物：

```powershell
.\scripts\status.ps1
.\scripts\verify-artifacts.ps1
```

详细的仓库边界、交付物选择和发布规则见 [`docs/REPOSITORY_AND_RELEASE.md`](docs/REPOSITORY_AND_RELEASE.md)。

## 目录约定

- `docs/requirements-source/`：原始需求图片，仅作需求追溯。
- `testdata/scan/`：扫描识别输入及人工参考。
- `testdata/audio/`：语音识别输入；正式准确率测试仍需配套冻结真值。
- `artifacts/`：可选的最终归档和检查报告，不进入 Git。
- 各子仓库的 `build/`：本机构建、SDK 和运行包，不进入 Git。
