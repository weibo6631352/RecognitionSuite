# Deployment

Copy `VoiceEngineWindowsX64/` to the target machine. No installer.

Required on the target:

- Windows 11 x64
- NVIDIA GeForce RTX 40 (`sm_89`) or RTX 50 (`sm_120`) series GPU
- NVIDIA Windows driver 576.02 or newer for the shipped CUDA 12.9 runtime
- Enough VRAM for Qwen3-ASR-1.7B BF16

Not required: WSL, Python, Visual Studio, CMake, CUDA Toolkit, system FFmpeg.

Layout matches the 技术路线: `VoiceEngine.exe`, `VoiceEngineCore.dll`, `VoiceEngineCli.exe`, Qt/MSVC (including `vcomp140.dll`), `runtimes/cuda`, `runtimes/ffmpeg`, `models/qwen3-asr-1.7b`, `licenses`, `voiceengine.json`, `启动.bat`.

FFmpeg and CUDA directories are added with `AddDllDirectory`; they are not written to the system `PATH`.
