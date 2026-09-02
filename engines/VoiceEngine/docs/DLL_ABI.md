# VoiceEngineCore.dll C ABI

Public headers: `include/voiceengine/voiceengine_api.h`, `include/voiceengine/voiceengine_version.h`.

- API version: `VOICEENGINE_API_VERSION` (currently 1)
- Every public struct has `struct_size` and `api_version`
- Paths are UTF-16 (`wchar_t*`)
- Text and JSON are UTF-8
- Memory from the DLL is released with `voiceengine_free` / `voiceengine_result_free`
- No `QString`, `std::string`, STL containers, C++ exceptions, or virtual classes are exported

## Functions

| Function | Role |
|---|---|
| `voiceengine_api_version` | Query version |
| `voiceengine_context_create` / `destroy` | Context lifecycle |
| `voiceengine_cuda_check` | NVIDIA driver / device probe |
| `voiceengine_model_load` / `unload` | Qwen3-ASR-1.7B BF16 on CUDA |
| `voiceengine_submit_file` | Async audio file |
| `voiceengine_submit_pcm` | Async PCM |
| `voiceengine_task_poll` | Status / progress / interim text |
| `voiceengine_task_result` | Final text + JSON |
| `voiceengine_task_cancel` | Cancel queued or running task |
| `voiceengine_last_error` | Last UTF-8 error |

One active inference per context; additional submits are queued.

CUDA or model init failure returns `VE_ERR_CUDA` / `VE_ERR_MODEL` and never starts CPU ASR.
