# third_party pin manifest

| Component | Version | Source | License | Checksum |
|---|---|---|---|---|
| llama.cpp + libmtmd | commit `9731ad3f29da96f588711a0d1eb08cf210721e16` | https://github.com/ggml-org/llama.cpp | MIT | commit id in `pins/llama.cpp/SHA256.txt` |
| FFmpeg win64 lgpl-shared | n8.1.2-44-g7c533d0f86 | BtbN autobuild-2026-08-18-15-03 | LGPL-3.0 | `windows-x64/ffmpeg/SHA256SUMS.txt` |
| CUDA toolkit (build) | 12.9.0 | NVIDIA | NVIDIA EULA | toolkit already on the ScanEngine machine |
| Qt | 5.12.9 msvc2017_64 | same install as ScanEngine | LGPL | path `C:/Qt/5.12.9/msvc2017_64` |
| Qwen3-ASR-1.7B BF16 GGUF | ggml-org conversion | https://huggingface.co/ggml-org/Qwen3-ASR-1.7B-GGUF | Apache-2.0 | recorded after local download in `models/qwen3-asr-1.7b/SHA256SUMS.txt` |

CMake does not download these. Missing trees fail configure.
