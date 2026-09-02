# VoiceEngine

Windows 本地中文语音识别桌面程序（C++ / Qt）。

- 音频文件或麦克风转中文文字
- 保留合理中文标点，支持普通话、常见方言及中英混合
- 正式推理路径为 NVIDIA CUDA，支持 GeForce RTX 40 系列（`sm_89`）与
  RTX 50 系列（`sm_120`），构建包同时携带两种架构
- `VoiceEngine.exe`（Qt GUI）和 `VoiceEngineCli.exe` 共用 `VoiceEngineCore.dll`
- 绿色免安装运行包，目标机无需 Python、CUDA Toolkit 或 FFmpeg

技术路线见 `doc/技术路线.md`。若有不得不做的实现差异，见 `doc/技术偏离表.md`。

构建（与 `ScanEngine` 同一套 VS 2022 v143 + Qt 5.12.9 + CUDA 12.9）：

```text
cmake --preset win-qt5.12.9-msvc-cuda
cmake --build --preset win-qt5.12.9-msvc-cuda
```

克隆需要 Git LFS（`git lfs install`）。Qwen3-ASR-1.7B BF16 权重在 `models/qwen3-asr-1.7b/`（主权重为 part1/part2，加载时自动拼接）。

```text
VoiceEngineCli verify-runtime
```
