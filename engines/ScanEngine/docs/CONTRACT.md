# 作业目录、CLI 与 Excel

独立 C++ 产品。权重在 `models/`，由 `scanengine.json` 指向。无 Python。

## 边界

- Windows：Qt 5.12.9 `msvc2017_64`、VS 2022 v143/x64、C++17。不用 MinGW，不用 Qt 6-only API。
- 推理：Windows MLX CUDA，Mac MLX Metal。无 CPU 产品路径。
- ONNX Runtime 只给 Magika 做 CODE 语言判定（ORT 1.28.0 CPU、batch=1），不进 VLM/GPU。

## 作业目录

```
{output_root}/
  YYYYMMDD-HHMMSS_源文件全名(含后缀)/
    源文件全名(含后缀)
    源文件 stem.xlsx
    work/
```

| 项 | 规则 |
|---|---|
| 时间戳 | 本地时间 `yyyyMMdd-HHmmss` |
| 非法字符 | `<>:"/\\|?*` 与换行/制表 → `_`，再去掉首尾 ` ._`，空则 `doc`，最长 120 |
| 目录名 | `{stamp}_{safe(source.name)}` |
| 冲突 | 追加 `_2` `_3` … |
| Excel | `{safe(source.stem)}.xlsx`，与源文件同级，不进 `work/` |
| 界面列表 | 只展示 Excel 和作业根目录 |

## 输入与解析

- 图片：PNG、JPEG、BMP、TIFF、WebP，以及只含这些图的文件夹。
- 文件选择、拖放、剪贴板图片 / 路径；`Ctrl+V` 贴图。
- 固定 C++ hybrid medium，`gpu-required`。无设备下拉。high / twoStep 直接失败。
- Parser 与 `ScanEngineTool env`：VLM、layout、OCR、dict、fastText、Magika、ORT 缺一即失败。

流程：分配作业目录 → PP-DocLayoutV2 → 表方向（只处理 table crop）→ VLM extract → OCR sidecar → middle → Excel。

状态：`idle → prepare → process → outputs → done`，失败 `failed`，取消 `cancelled`。

## Excel

1. 优先 `*_content_list.json` 里 `type==table` 的 `table_body`/`html`。
2. 否则 Markdown 里的 `<table>`。
3. 再否则纯文本写成单列「文本」表。
4. rowspan/colspan 做成网格和合并区；细边框。
5. 表名去掉 `:\ / ? * [ ]`，≤31，重名加 `_2`。
6. 无数据时一张「空」表，A1=`未识别到表格`。
7. `table_count` 不计「文本」表。

## 预览与配置

- 磁盘上的 `.md` 不改写。预览时本地图改 `file://`，`<table>`/`<img>` 原样。
- `config.local.json`（exe 同目录）：`output_dir`、`window_geometry`。默认输出 `bin/output/`。
- 窗口 1440×900，三栏 320 / 520 / 520。结果区：解析结果 / 运行日志。

## 构建

Windows：`script\windows\开始.bat`，或

```bat
cmake --preset win-qt5.12.9-msvc-mlx-cuda
cmake --build --preset win-qt5.12.9-msvc-mlx-cuda
```

Mac：

```bash
cmake --preset macos-arm64-mlx
cmake --build build/macos-arm64-mlx
```

验收：`parse` / GUI 成功、`fallback_count=0`、Excel 能打开且表可用。
