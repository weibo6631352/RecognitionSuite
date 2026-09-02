# 官方权重（本仓库本地目录）

C++ 只读这里，不再访问旧 Python 仓库。

| 目录 | 内容 |
|---|---|
| `vlm/` | MinerU2.5-Pro-2605-1.2B 官方 snapshot（`model.safetensors.part1/part2` + tokenizer） |
| `layout/PP-DocLayoutV2/` | 官方 `model.safetensors` + config |
| `ocr/` | `ch_PP-OCRv6_small_{det,rec}_infer.safetensors` |
| `dict/ppocrv6_dict.txt` | 官方 PP-OCRv6 词表（入库） |
| `fasttext/lid.176.ftz` | fast-langdetect 0.2.5 的 176 语言压缩模型（含离线许可） |
| `magika/standard_v3_3/` | Magika 1.0.3 CODE 语言 model/config（ORT CPU） |

路径由 `scanengine.json` 的 `models-dir` 给出，相对可执行文件目录（`build/<preset>/bin/`）。源码树这份构建时拷进 `bin/models/`。大型官方权重通过 Git LFS 纳入仓库；不足 1 MiB 的 `lid.176.ftz` 直接由 Git 管理，并在首次加载时强制核对内容哈希。VLM 权重仅因 GitHub LFS 单文件大小限制拆成两个连续分片，C++ 运行时按原始 safetensors 字节流读取。

主要权重 SHA-256：

- VLM：`abf8681ca63b8dec7b67de257af47b821f179442f72998d0696ae2ed9232a5f0`
- Layout：`e60f3725aeedc88fd319416ef166bda79171a41516a301c27cab9132dc2739d2`
- OCR det：`89a96a8adc4e9cd0c994098edc76022e496d35844392562b4694c8fbc583f2da`
- OCR rec：`f65a332afe5aa663f0b9d5706f4ae8457b5b4058a842d5c1eb22df505c27d642`
- fastText lid.176.ftz：`8f3472cfe8738a7b6099e8e999c3cbfae0dcd15696aac7d7738a8039db603e83`

`fasttext/NOTICE.MD` 保存模型上游声明，`fasttext/LICENSE.CC-BY-SA-3.0.txt` 保存完整 CC BY-SA 3.0 Unported 法律文本；`LICENSE.fastText-MIT.txt`、`LICENSE.fast-langdetect-MIT.txt`、`LICENSE.fasttext-langdetect-MIT.txt` 与 `NOTICE.fast-langdetect.md` 保存静态推理源码及两层兼容组件的许可/来源声明。这些文件随模型一起进入 Windows/macOS 独立部署目录。
