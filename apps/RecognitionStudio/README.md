# 纸质扫描与语音识别系统

RecognitionStudio 是一个 Windows 桌面识别工具，当前范围只包含两项核心能力：

1. 纸质扫描识别：导入 PNG、JPEG、BMP、TIFF 或 WebP 扫描图片，识别文字与表格并生成 Excel；
2. 语音识别：导入常见音频文件，或使用系统默认麦克风进行流式转写。

任务计划、电子文档解析、人工业务校对、地面站文件、DTC、图形回放、项目管理和业务验收中心均不属于当前版本范围。

## 验收目标

- 软件整体运行稳定，两项识别功能运行完好，系统符合当前设计范围；
- 纸质扫描识别率达到 98%；
- 语音识别率达到 98%。

98% 是正式验收目标，必须在双方确认并冻结的代表性测试集上统计。纸质扫描建议按字符准确率统计，语音建议按“1 - 字错误率（CER）”统计；测试集、预处理规则、标点计分、数字与单位归一化方式需在验收前书面确认。少量演示样本不能替代完整统计测试。

## 运行环境

- Windows 10/11 x64
- Visual Studio 2022 / MSVC v143
- Qt 5.12.9 `msvc2017_64`
- CMake 3.25 或更高版本
- NVIDIA GeForce RTX 40（`sm_89`）或 RTX 50（`sm_120`）系列显卡
- 已构建的 VoiceEngine 与 ScanEngine 独立 SDK

默认 SDK 目录：

```text
../../engines/VoiceEngine/build/win-qt5.12.9-msvc-cuda/sdk
../../engines/ScanEngine/build/win-qt5.12.9-msvc-mlx-cuda/sdk
```

## 构建与检查

```powershell
cmake --preset windows-msvc-qt5
cmake --build --preset windows-msvc-qt5
build\windows-msvc-qt5\bin\RecognitionStudio.exe --sdk-check
```

`--sdk-check` 返回 0 表示两套 SDK 的 GPU 运行时均可初始化；它不代表 98% 准确率验收已经通过。

主程序：

```text
build\windows-msvc-qt5\bin\RecognitionStudio.exe
```

## 集成边界

宿主只通过公开 C++ SDK 调用 VoiceEngine 和 ScanEngine，不复制两套产品的内部源码。OCR 与 ASR 共用 GPU 时串行执行，避免显存争用。源码仓库不包含模型、SDK DLL、生成结果或构建目录。
