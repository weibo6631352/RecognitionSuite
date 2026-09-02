# Result JSON schema

```json
{
  "schema_version": 1,
  "text": "最终识别文本",
  "language": "zh",
  "duration_sec": 12.3,
  "model": "Qwen3-ASR-1.7B-BF16",
  "device": "CUDA:0 NVIDIA GeForce RTX 5090",
  "segments": [
    { "start": 0.0, "end": 12.3, "text": "片段文本" }
  ]
}
```

CLI `--format json` writes this object. `--format txt` writes `text` only. `transcribe-dir` writes one `.txt` per input. `benchmark` writes a metrics object with `cer` and the written 验收门槛.
