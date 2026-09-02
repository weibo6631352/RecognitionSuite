# Benchmark and 验收门槛

`VoiceEngineCli benchmark --dataset DIR` expects `*.wav` plus sibling `*.txt` references.

The 技术路线 requires CER / 标点 F1 / 专有名词 to meet a project threshold but does not give numbers. This file is the written first-version threshold:

| Metric | Threshold |
|---|---|
| CER | ≤ 0.10 |
| 标点 F1 | ≥ 0.75 |
| 专有名词 F1 | ≥ 0.70 |

These numbers can be revised after the official Chinese test set is frozen. The CLI always emits the metrics it can compute; missing optional scores are `null`.
