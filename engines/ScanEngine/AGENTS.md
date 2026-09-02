# Agent 公约

产品是本仓库 C++ / Qt hybrid。不要 Python，不要 RapidOCR，不要 mineru CLI。

ONNX Runtime 只给 Magika 做 CODE 语言判定（ORT 1.28.0 CPU、batch=1），不得进入 VLM / GPU。

1. 先读 `README.md`、`docs/CONTRACT.md`、`docs/HYBRID.md`。
2. 权重只读 `models/`，构建后在 `build/<preset>/bin/models/`。大文件走 Git LFS。
3. 作业输出走 `bin/output/`，不提交。
4. Windows 构建：`script\windows\开始.bat`，或 preset `win-qt5.12.9-msvc-mlx-cuda`。
5. Mac 构建：`cmake --preset macos-arm64-mlx`。
6. 本地 commit 用英文祈使句。用户明确要求时再 `git push`。
