# 冻结验收数据

RecognitionStudio 的“验收验证”页可直接加载以下清单：

- 扫描：`codex-independent-2026-09/scan/manifest.json`
- 语音：`codex-independent-2026-09/voice/manifest.json`

两份清单均由 Codex 独立提供，真值先于媒体生成，待测程序未参与出题或标注。
清单内的输入和真值 SHA-256 会在 UI 加载及每个样本执行前校验。

详细的来源、覆盖范围和计分规则见
`codex-independent-2026-09/README.md`。
