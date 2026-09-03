# 纸质扫描与语音识别系统

RecognitionStudio 是一个 Windows 桌面识别工具，当前范围只包含两项核心能力：

1. 纸质扫描识别：导入 PNG、JPEG、BMP、TIFF 或 WebP 扫描图片，识别文字与表格并生成 Excel；
2. 语音识别：导入常见音频文件，或使用系统默认麦克风进行流式转写。
3. UI 验收验证：加载冻结测试集清单，批量执行扫描或语音识别，在界面内计算并判定 98% 指标，同时内嵌展示真值、识别文本、产物哈希、样本 JSON、汇总报告和模型置信度。

任务计划、电子文档解析、人工业务校对、地面站文件、DTC、图形回放、项目管理和业务验收中心均不属于当前版本范围。

## 验收目标

- 软件整体运行稳定，两项识别功能运行完好，系统符合当前设计范围；
- 纸质扫描识别率达到 98%；
- 语音识别率达到 98%。

98% 是正式验收目标，必须在双方确认并冻结的代表性测试集上统计。纸质扫描建议按字符准确率统计，语音建议按“1 - 字错误率（CER）”统计；测试集、预处理规则、标点计分、数字与单位归一化方式需在验收前书面确认。少量演示样本不能替代完整统计测试。

验收清单格式、完整性规则和证据输出见 [`docs/验收验证.md`](../../docs/验收验证.md)，模型置信度的来源和使用边界见 [`docs/置信度校准.md`](../../docs/置信度校准.md)。正式验证会在识别前后逐项核对实际部署的 SDK/模型与编译时发布基线；任何基线不匹配、样本失败、证据缺失或报告写入失败都会使 UI 结论变为“验证无效”。模型置信度作为单独的辅助指标展示，不能替代与真值比较得到的实际准确率。

## 运行环境

- Windows 10/11 x64
- Visual Studio 2022 / MSVC v143
- Qt 5.12.9 `msvc2017_64`
- CMake 3.25 或更高版本
- NVIDIA GeForce RTX 40（`sm_89`）或 RTX 50（`sm_120`）系列显卡
- 根仓库中已提交的 VoiceEngine 与 ScanEngine 独立 SDK 基线

默认 SDK 目录：

```text
../../sdk/VoiceEngine/windows-x64
../../sdk/ScanEngine/windows-x64
```

## 构建与检查

```powershell
cmake --preset windows-msvc-qt5
cmake --build --preset windows-msvc-qt5
build\windows-msvc-qt5\bin\RecognitionStudio.exe --sdk-check
```

`--sdk-check` 返回 0 表示检测到 RTX 40 (`sm_89`) 或 RTX 50
(`sm_120`) 设备、两套 SDK 的 GPU 运行时均可初始化，并且实际部署的 SDK/模型
逐项 SHA-256 与构建时发布基线一致。它不加载完整模型；全量哈希在当前运行包上
约需一分钟，也不代表 98% 准确率或高显存压力输入已经验收通过。

主程序：

```text
build\windows-msvc-qt5\bin\RecognitionStudio.exe
```

## 集成边界

宿主只通过公开 C++ SDK 调用 VoiceEngine 和 ScanEngine，不复制两套产品的内部源码。OCR 在独立的 `RecognitionStudioScanWorker.exe` 中执行，任务结束后通过进程退出完整释放扫描侧 GPU 资源；切换到 ASR 时主界面按需自动加载语音模型，避免两套大模型同时占用显存。应用源码目录不保存 SDK DLL 或模型；运行包只从根 `sdk/` 的已验证 Git LFS 基线组装。生成结果和构建目录不进入 Git。
