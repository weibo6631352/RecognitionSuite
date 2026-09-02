# Windows x64 MLX CUDA（预编译）

与 Mac 的 `third_party/macos-arm64/mlx/` 同一策略：仓库只放编好的库和头文件，产品构建不再编译 MLX。

当前包锁定 Windows x64、MSVC v143 `/MD`、CUDA 12.9、cuDNN 9.9.0.52，
同时携带 RTX 40（`sm_89`）与 RTX 50（`sm_120a`/`compute_120`）代码。
40 系构建机能够生成 50 系代码；发布前仍应在对应硬件上做一次实际识别。

| 路径 | 内容 |
|---|---|
| `include/mlx/` | MLX 0.31.1 公开头文件 |
| `lib/mlx.lib` | 静态库（含应用私有 CUDA 运行时加载，不再另附补丁源） |
| `lib/dl.lib` | dlfcn-win32 静态库 |
| `jit-include/cccl/` | CCCL 3.1.3 头文件，运行时 JIT 部署到 `bin/include/cccl` |

`mlx.lib`：531625456 bytes，SHA-256
`49085D977406ABA9DDB3AAC62B37759332B83024957259ABB0F9A65C51635C0D`  
`dl.lib`：16704 bytes，SHA-256
`15FBDF17F582C8FCEE4C8C5A7F95AE32DE608F19FDDB3FF4F0508FA5360E46C5`

ScanEngine 专用的补充算子在产品桥接静态库中编译。

构建机仍需安装同一套 CUDA 12.9 Toolkit（含 cuDNN），用于链接 `cublasLt`/`nvrtc`/`cudnn` 以及部署 `runtimes/cuda` 和 `include/cuda`。不需要 MLX 源码、CUTLASS 或重新跑 nvcc。
