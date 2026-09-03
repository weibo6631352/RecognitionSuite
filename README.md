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
├─ sdk/                                 已验证、已提交的 Windows x64 SDK 基线
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

## 构建

Windows 构建机需要 VS 2022/v143 和 Qt 5.12.9 MSVC x64。日常开发直接使用仓库中已验证的两套 SDK，只构建 `RecognitionStudio`：

```powershell
.\scripts\构建.ps1
```

只有在明确更新引擎基线时才重建两套引擎，并把验证后的 staging SDK 发布到根 `sdk/`：

```powershell
# 如果引擎源码有变化，先暂存它，使 SDK 清单能记录确定的 source_tree。
git add engines/ScanEngine engines/VoiceEngine
.\scripts\构建.ps1 -Target SDKs
git status --short -- sdk
```

该流程需要 CUDA 12.9、cuDNN 9.9 以及引擎各自锁定的构建依赖。`-Target All` 会刷新 SDK 后再构建 Studio；普通应用开发不要使用它。
`-Target Engines`、`ScanEngine` 和 `VoiceEngine` 只生成引擎本地 staging，不更新已提交的根 SDK 基线。

只检查仓库状态或现有产物：

```powershell
git status --short --branch
.\scripts\验证成果.ps1
# 发布前执行较慢的全量 SDK 哈希与 RTX 40/50 二进制架构校验：
.\scripts\验证成果.ps1 -VerifySdkHashes -VerifyCudaArchitectures
```

根 `scripts/` 下的自有 PowerShell 脚本统一使用中文名称。`sdk/` 内已发布工具的英文路径属于 SDK 稳定接口，`third_party/` 内脚本属于上游源码，两者不随仓库辅助脚本改名。

两套 SDK 的 Core DLL 必须同时携带 RTX 40 (`sm_89`) 与 RTX 50
(`sm_120a`) 原生 CUDA 代码；最终程序启动时也会拒绝其他计算能力。
硬件验证矩阵及尚未关闭的显存边界见
[`docs/GPU_COMPATIBILITY.md`](docs/GPU_COMPATIBILITY.md)。详细的仓库边界、
交付物选择和发布规则见
[`docs/REPOSITORY_AND_RELEASE.md`](docs/REPOSITORY_AND_RELEASE.md)。

## 目录约定

- `docs/requirements-source/`：原始需求图片，仅作需求追溯。
- `testdata/scan/`：扫描识别输入及人工参考。
- `testdata/audio/`：语音识别输入；正式准确率测试仍需配套冻结真值。
- `sdk/<Engine>/windows-x64/`：已验证 SDK 基线，包含 LFS 管理的模型和二进制，进入 Git。
- `artifacts/`：最终应用归档、校验和与检查报告，不进入 Git，并应上传发布制品库。
- 各子项目的 `build/`：本地中间构建、SDK staging 和运行包，不进入 Git。
