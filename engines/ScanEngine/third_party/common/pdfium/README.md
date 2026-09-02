# 官方 pdfium

使用固定版本的官方 pdfium-binaries。
`page_to_image` 走 `FPDF_RenderPageBitmap`，不是对原图 bicubic。

公共 C API 头文件在 `third_party/common/pdfium/include/`，运行库按平台拆分并通过
Git LFS 纳入仓库。

## Mac arm64

固定的 `libpdfium.dylib` 文件位置：
`third_party/macos-arm64/pdfium/libpdfium.dylib`。

## Windows x64

固定 pdfium-binaries `151.0.7891`（build 7891），文件位置为
`third_party/windows-x64/pdfium/pdfium.dll`。程序通过
`LoadLibraryW` / `GetProcAddress` 加载该 DLL，无需 MSVC import library。
