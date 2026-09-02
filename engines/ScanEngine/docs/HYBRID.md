# C++ hybrid

桌面端只跑官方 hybrid **medium**：

PP-DocLayoutV2 → 表方向 → VLM `extract_with_layout` → OCR sidecar → middle / Excel。

不调用外部解析进程。high / `two_step_extract` 不在产品范围。

## 步骤

1. 版面 → `steps/01_layout.json`
2. 表方向 → `steps/01b_layout_oriented.json`
3. medium 标签 → `steps/02_vlm_blocks.json`
4. Qwen2-VL greedy extract → `steps/03_extract.json`
5. OCR det sidecar → `04` / `04b` / `04c`
6. middle.json（fastText 普通文本语言 + Magika CODE 语言）+ content_list + Excel

## 权重

源码树 `models/`，构建后 `bin/models/`（`scanengine.json`）：

- `vlm/` — MinerU2.5-Pro-1.2B
- `layout/PP-DocLayoutV2/`
- `ocr/` + `dict/ppocrv6_dict.txt`
- `fasttext/lid.176.ftz`
- `magika/standard_v3_3/`

## 推理后端

| 平台 | 后端 | 位置 |
|---|---|---|
| Windows | MLX 0.31.1 CUDA 静态库，sm_89 + sm_120 | `third_party/windows-x64/mlx/` |
| Mac | 官方 libmlx Metal | `third_party/macos-arm64/mlx/` |

没有 MLX 时 layout/OCR/VLM 直接失败，不再走自制 CPU 图。

Magika 用锁定 ORT 1.28.0 CPU、batch=1，只判代码语言，不进 GPU。

## 构建

Windows：`win-qt5.12.9-msvc-mlx-cuda`（Qt 5.12.9 + VS 2022 v143 + CUDA 12.9）。  
部署：`windeployqt`、`pdfium.dll`、`onnxruntime.dll`、`runtimes/cuda/`、JIT 头文件。

Mac：`macos-arm64-mlx`（Qt 5.15.2 arm64，最低 macOS 26）。  
`macos-qt5` 仅开发用（无 MLX）。

依赖在 `third_party/`，构建不下载。链接先到 `raw/`，再部署到 `bin/`。
