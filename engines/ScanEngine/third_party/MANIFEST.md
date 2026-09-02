# 依赖清单

| 依赖 | 版本 / 平台 | SHA-256（运行时） |
|---|---|---|
| QXlsx | v1.4.4 / common source | — |
| IJG libjpeg | jpeg-9f / common source | 源码包校验值 `04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b` |
| MLX | 0.31.1 / macOS arm64 | `libmlx.dylib`: `e0dacaec414323e43010ade1b464214c837ebe82ca72465e897c1912fdfdc601` |
| MLX Metal | 0.31.1 / macOS arm64 | `mlx.metallib`: `198488eb61359e953580a9c4530400feee1a06dd2f28a930a6ffa58aec66a597` |
| MLX | 0.31.1 / Windows x64 CUDA 12.9 sm_120 | `mlx.lib`: `E996629C095855BBAB0B159E4C093861EA3D52665F6E14B1471A8F981DD2C59A` |
| dlfcn-win32 | 1.4.2 / Windows x64 | `dl.lib`: `DD9629A3AA16C168CA7BBC3AEF3BE0B8F3F8EE38D91253A169118E22C7EA9AB1` |
| pdfium | pdfium-binaries 151.0.7891 / macOS arm64 | `92c7fb47c1ecdeb943070b0cb688e3fa467beb192b2bd9faa2fe35f70324a4f7` |
| pdfium | pdfium-binaries 151.0.7891 / Windows x64 | `e66bd5aefc2829f12ef41227f83a6ec6d60673ec77ef64153a05e7ffbae9072e` |

模型版本、用途和路径见 `models/README.md`。`.safetensors`、`.dylib`、
`.metallib`、`.dll` 与 Windows `mlx.lib` 由 Git LFS 跟踪。
