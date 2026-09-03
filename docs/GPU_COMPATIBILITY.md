# RTX 40/50 GPU 兼容性基线

## 产品约束

RecognitionStudio 的正式目标是 NVIDIA GeForce RTX 40 与 RTX 50 系列 GPU。
运行时按计算能力接受 `8.9` 或 `12.0`，因此同计算能力的其他 NVIDIA 专业卡也会
通过架构门禁，但不属于本项目声明的 GeForce 验收矩阵：

- RTX 40：设备计算能力 `8.9`，发布物必须含 `sm_89` 原生代码与 PTX；
- RTX 50：设备计算能力 `12.0`，发布物必须含 `sm_120a` 原生代码和
  `sm_120`/`sm_120a` PTX；
- CUDA Toolkit 构建版本为 12.9.41；Windows 驱动基线为 576.02；
- 不提供 CPU 识别降级路径。

`scripts/验证成果.ps1` 的 `-VerifyCudaArchitectures` 模式会读取两颗已发布
Core DLL 的 fatbin。SDK 发布会自动执行同一检查；正式发布还应执行：

```powershell
.\scripts\验证成果.ps1 -VerifySdkHashes -VerifyCudaArchitectures
```

这项静态检查可以阻止未来误发布单架构 SDK，但不能代替目标显卡真机推理。

## 2026-09-03 本机验证

验证环境：RTX 4060、计算能力 8.9、8188 MiB、Windows 驱动 576.02。

| 检查 | ScanEngine | VoiceEngine |
|---|---|---|
| 已提交 Core SHA-256 | `7c7ef14b898c35b03fb9cc48c8ef7b5e7d6c4fc03d216273fa73436f113b7e1` | `ca5fb14b7216457e9787345815348dfb2ea798917403f9bf839368ed5a73dbe1` |
| 原生 CUDA 代码 | `sm_89` × 89；`sm_120a` × 89 | `sm_89` × 141；`sm_120a` × 141 |
| PTX | `sm_89` × 89；`sm_120` × 89 | `sm_89` × 141；`sm_120a` × 141 |
| RTX 4060 实际推理 | `微信图片_20260604104758_14_53.jpg` 完成解析并生成 Excel | 1.7B BF16 全层加载到 CUDA0，流式 tone 完成 FINAL/END |

以上证明当前 SDK 在 RTX 40/Ada 上能实际发射 CUDA 工作并完成两条业务路径，
而不只是通过 DLL 加载检查。

## 发布前仍需完成的硬件矩阵

RTX 50/Blackwell 的原生代码和 PTX 已存在于当前 DLL，但本仓库所在机器没有
RTX 50，因此尚无 RTX 50 真机推理记录。对外标记一个发布版本为“40/50 真机
已验收”前，必须在至少一张 RTX 50 上执行：

1. `RecognitionStudio.exe --sdk-check`；
2. VoiceEngine 模型加载和一段真实音频/流式音频推理；
3. ScanEngine 至少一张普通扫描件及显存压力样本的完整解析；
4. 记录 GPU 型号、计算能力、显存、驱动、SDK manifest/Core SHA-256 和结果。

## 显存边界

GPU 系列兼容与所有输入在所有显存规格上通过是两件事。RTX 4060 8GB 已能完成
普通扫描件和 VoiceEngine 流式推理；但
`testdata/scan/qingxi/reference/dataset-reference.json` 记录了 4 张约
7884–8060 个视觉 token 的密集扫描件发生 CUDA OOM；本次已在 RTX 4060 8GB
上复现其中的 `微信图片_20260604104800_16_53.jpg`，其余 3 张仍需纳入完整回归。
它们必须保留为发布阻断用例。若目标包含 6GB/8GB 型号，需要先降低视觉 token
峰值或实现分块；否则发布说明必须明确最低显存和输入规模，不能笼统声称所有
RTX 40/50 SKU、所有输入均已验收。

官方依据：

- [CUDA 12.9 release notes](https://docs.nvidia.com/cuda/archive/12.9.0/cuda-toolkit-release-notes/index.html)
- [cuDNN 9.9 support matrix](https://docs.nvidia.com/deeplearning/cudnn/backend/v9.9.0/reference/support-matrix.html)
- [NVIDIA CUDA GPU compute capability](https://developer.nvidia.com/cuda-gpus)
