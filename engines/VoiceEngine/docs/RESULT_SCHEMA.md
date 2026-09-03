# Result JSON schema

```json
{
  "schema_version": 2,
  "text": "最终识别文本",
  "language": "zh",
  "duration_sec": 12.3,
  "model": "Qwen3-ASR-1.7B-BF16",
  "device": "CUDA:0 NVIDIA GeForce RTX 5090",
  "segments": [
    {
      "start": 0.0,
      "end": 12.3,
      "text": "片段文本",
      "confidence": {
        "value": 0.9175,
        "source": "decoder_token_geometric_mean",
        "calibrated": false
      },
      "tokens": [
        { "text": "片段", "confidence": 0.94 },
        { "text": "文本", "confidence": 0.90 }
      ]
    }
  ]
}
```

CLI `--format json` writes this object. `--format txt` writes `text` only. `transcribe-dir` writes one `.txt` per input. `benchmark` writes a metrics object with `cer` and the written 验收门槛.

`confidence.value` is the geometric mean of the selected decoder token
probabilities. It is an uncalibrated diagnostic signal, not an accuracy score
and not a substitute for comparison with frozen references.
