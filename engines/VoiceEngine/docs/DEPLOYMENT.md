# Deployment

Copy `VoiceEngineWindowsX64/` to the target machine. No installer.

Required on the target:

- Windows 11 x64
- NVIDIA driver that can run CUDA 12.9 / RTX 5090 (`sm_120`)
- Enough VRAM for Qwen3-ASR-1.7B BF16

Not required: WSL, Python, Visual Studio, CMake, CUDA Toolkit, system FFmpeg.

Layout matches the 技术路线: `VoiceEngine.exe`, `VoiceEngineCore.dll`, `VoiceEngineCli.exe`, Qt/MSVC, `runtimes/cuda`, `runtimes/ffmpeg`, `models/qwen3-asr-1.7b`, `licenses`, `voiceengine.json`, `启动.bat`.

FFmpeg and CUDA directories are added with `AddDllDirectory`; they are not written to the system `PATH`.
