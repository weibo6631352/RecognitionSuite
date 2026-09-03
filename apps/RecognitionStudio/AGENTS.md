# RecognitionStudio development contract

- Windows x64 only: Visual Studio 2022, MSVC v143, Qt 5.12.9, C++17.
- Integrate VoiceEngine and ScanEngine through the committed SDK baselines
  under the repository root `sdk/` directory only. Do not copy product source
  code into this application.
- Keep the application GPU-only, target NVIDIA GeForce RTX 40 (sm_89) and RTX 50
  (sm_120) series GPUs, and fail clearly when either SDK runtime is unavailable.
- Keep the GUI limited to SDK status, paper-scan recognition, and audio file/microphone recognition. Do not add business workflows until requirements are explicitly provided.
- Do not place SDK DLLs or models inside this application directory. The
  reviewed SDK baselines, including their LFS-managed binaries and models,
  are committed only under the repository root `sdk/`. Do not commit generated
  output or build directories.
- Local commits use concise English imperative messages. Push only when the user explicitly requests it.
