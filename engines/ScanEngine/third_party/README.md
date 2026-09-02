# 仓库内依赖

构建不下载项目依赖。Qt SDK 与编译器由平台工具链提供，其余依赖固定在本目录：

- `common/QXlsx/`：QXlsx v1.4.4 源码与上游许可证。
- `common/libjpeg/`：IJG jpeg-9f 源码。
- `common/pdfium/include/`：本项目使用的 pdfium C API 头文件。
- `macos-arm64/`：arm64 MLX/Metal 与 pdfium 运行库。
- `windows-x64/`：x64 pdfium、ORT，以及预编译的 MLX CUDA 静态库。

`.dylib`、`.metallib`、`.dll`、Windows `mlx.lib` 与模型权重由 Git LFS 跟踪。
