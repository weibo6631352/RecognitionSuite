# Windows 工具链

正式链只有一套：Qt 5.12.9 `msvc2017_64` + VS 2022 v143/x64 + `/MD` + CUDA 12.9。不用 MinGW，不用 Ninja。

| 项 | 锁定 |
|---|---|
| 生成器 | Visual Studio 17 2022，`v143,host=x64` |
| 业务 C++ | C++17；MLX bridge C++20 |
| Qt | `C:\Qt\5.12.9\msvc2017_64` |
| CUDA | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9` |
| cuDNN | 9.9.0.52，装在 CUDA 根目录 |
| CMake | ≥ 3.25（本机常用 3.30.5） |
| MLX | `third_party/windows-x64/mlx/lib/mlx.lib`，不再编译源码 |
| 目标卡 | RTX 5090，`120a-real;120-virtual` |

换机仍是同一套：Windows x64 + 9950 + 5090 + CUDA 12.9。

开发机装工具链：双击 `script\windows\开始.bat`，先「下载离线包」再「离线安装」。离线包在 `script\windows\offline\`，可整目录拷走。

构建：同一脚本选「构建并打包」，或

```bat
cmake --preset win-qt5.12.9-msvc-mlx-cuda
cmake --build --preset win-qt5.12.9-msvc-mlx-cuda
```

可运行目录：`build\win-qt5.12.9-msvc-mlx-cuda\bin\`。打包默认到桌面 `ScanEngineWindowsGpu`。运行包自带 `runtimes\cuda`，运行机不必装 CUDA Toolkit。
