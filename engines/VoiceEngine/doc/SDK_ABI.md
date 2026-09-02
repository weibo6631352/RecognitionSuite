# VoiceEngine external SDK

VoiceEngine exposes a stable Windows x64 C ABI. Consumers link the imported
CMake target `VoiceEngine::SDK`, call `voiceengine_runtime_initialize()` once on
the process main thread, and only then call functions in `voiceengine_api.h`.
The release build creates a standalone `sdk/` directory beside `bin/`; the SDK
does not depend on files from the desktop application's directory.

## ABI rules

- Public symbols use `extern "C"`; Qt, STL, exceptions, and C++ classes never
  cross the DLL boundary.
- Every extensible structure begins with `struct_size` and `api_version`.
- Set both fields before every call. Version 1 output functions validate the
  complete v1 structure size and never write beyond the caller's declared
  capacity; future fields may only be appended.
- Paths use UTF-16 on Windows; result text and JSON use UTF-8.
- Memory returned by the core is released by `voiceengine_free()` or
  `voiceengine_result_free()`.
- Contexts and streams are strongly typed opaque handles. Submit, poll, cancel,
  result, and release form the asynchronous task lifecycle shared with the
  ScanEngine SDK.
- `voiceengine_task_result()` copies the public result but does not release the
  task. Call `voiceengine_result_free()` and `voiceengine_task_release()` after
  consuming it. Release returns `VE_ERR_BUSY` while a task is still active.

## CMake consumer

```cmake
find_package(VoiceEngine CONFIG REQUIRED
    PATHS "PATH_TO_SDK/cmake" NO_DEFAULT_PATH)
target_link_libraries(my_app PRIVATE VoiceEngine::SDK)
```

Before the first core call:

```cpp
#include <voiceengine/voiceengine_runtime.h>

wchar_t error[1024]{};
if (!voiceengine_runtime_initialize(L"PATH_TO_SDK/bin", error, 1024)) {
    // report error
}
```

The imported target supplies the core import library, runtime bootstrap static
library, `delayimp`, and `/DELAYLOAD:VoiceEngineCore.dll`. `sdk/bin` contains the
backend DLL, models, MSVC runtime, and private `runtimes/cuda` and
`runtimes/ffmpeg` trees. `VOICEENGINE_RUNTIME_ROOT`, exported by the CMake
package, is the absolute path to that directory.

```text
build/<preset>/
|-- bin/                  desktop software build
`-- sdk/                  standalone third-party SDK
    |-- bin/              Core DLL, models, and private runtime
    |-- include/
    |-- lib/
    |-- cmake/
    |-- examples/
    |-- doc/
    `-- licenses/
```

`sdk/examples/sdk_smoke` is a standalone consumer. With only the package root
argument it checks DLL loading and CUDA readiness; adding an audio file also
loads the packaged model and runs the complete asynchronous transcription flow.
Passing `--stream-tone` instead exercises the streaming session ABI with
incremental PCM blocks and a real packaged GPU model.

## C++ convenience API

Applications using C++17 may include `voiceengine/voiceengine.hpp`. This is a
header-only RAII layer over the same C ABI; it adds no DLL and no third-party
dependency. Context, task, result memory, and stream lifetime are managed
automatically. Errors are reported as `voiceengine::Error`, whose `status()` is
the original `ve_status` value.

```cmake
target_link_libraries(my_cpp_app PRIVATE VoiceEngine::CXX)
```

`VoiceEngine::CXX` selects C++17 and links the same `VoiceEngine::SDK` target.

```cpp
#include <voiceengine/voiceengine.hpp>

auto context = voiceengine::Context::open(sdk_runtime_root);
context.load_model(); // Uses sdk/bin/models/qwen3-asr-1.7b.

auto task = context.submit_file(audio_path);
voiceengine::Result result = task.wait();
use(result.text, result.json);
```

`Task` and `Stream` are movable and not copyable. They retain their context, so
the underlying C context remains alive until its last operation object is
destroyed. `Task::result()` copies the DLL-owned strings and releases the task.
Destroying an unfinished task requests cancellation; final context destruction
may wait for its worker to stop, matching the C API ownership rules.
`sdk/examples/sdk_cpp_smoke` is an independently buildable C++ consumer.

## Streaming microphone recognition

VoiceEngine deliberately keeps device capture outside the core DLL. The host
application may use WASAPI, Qt Multimedia, PortAudio, a browser bridge, or a
network audio source. It pushes normalized interleaved `float32` PCM frames to
one persistent stream; VoiceEngine owns VAD, pre-roll, endpointing, cumulative
window decoding, long-speech rolling, audio-bridge de-duplication, and
partial/final event delivery.

`chunk_ms` is the recognition refresh interval, not a text or audio boundary.
At each refresh VoiceEngine re-evaluates the current VAD-delimited utterance and
may revise its last few tokens before returning the complete current hypothesis.
Consequently, PARTIAL text always replaces the previous PARTIAL for the same
`segment_id`; it must never be appended. `overlap_ms` remains in ABI v1 for
source and binary compatibility with earlier SDKs, but cumulative streaming no
longer uses it for periodic recognition chunks. A short internal audio bridge is
used only when a long segment must be rolled without a natural pause.

Long speech is rolled automatically without ending the stream. Once the active
window is about 30 seconds long, a short quiet interval can stabilize it as a
FINAL event. If speech remains genuinely uninterrupted, VoiceEngine selects the
lowest-energy boundary near the 45-second hard window and continues in a new
segment. The new segment receives a 1.6-second audio bridge across that boundary.
The unstable sentence tail is held back until the bridged window recognizes it
again, and matching text is removed with punctuation-insensitive Unicode
comparison before events are delivered. This keeps technical roll boundaries
from creating duplicated or prematurely punctuated phrases.
No transcript is prefilled into the model, which avoids turning recognition
into autoregressive text continuation. There is no fixed limit on the duration
of the microphone session.

The shipped RTX 5090 profile keeps all model layers on CUDA, uses an 8192-token
context with 2048/512 logical and physical batches, and allows up to 384 newly
generated tokens per rolled window. A 45-second audio window occupies only a
small part of that context, so increasing the context or batches would mainly
reserve more VRAM rather than improve continuity. The AMD 9950X handles capture,
resampling, VAD, and UI work; it is not used as an ASR fallback.

```cpp
ve_stream_params p{};
p.struct_size = sizeof(p);
p.api_version = VOICEENGINE_API_VERSION;
p.sample_rate = capture_sample_rate;
p.channels = capture_channels;
p.chunk_ms = 2000;
p.overlap_ms = 200;
p.endpoint_silence_ms = 800;
p.pre_roll_ms = 200;
p.vad_rms_threshold = 0.004f;

ve_stream* stream = nullptr;
if (voiceengine_stream_create(context, &p, &stream) != VE_OK) {
    // The model must already be loaded.
}

// Repeat from the capture callback. frame_count counts frames, not float values.
// For stereo, samples contains frame_count * 2 interleaved floats.
voiceengine_stream_push_f32(stream, samples, frame_count);

// Repeat from the host event loop or a polling thread.
ve_stream_event event{};
event.struct_size = sizeof(event);
event.api_version = VOICEENGINE_API_VERSION;
while (voiceengine_stream_poll(stream, &event) == VE_OK) {
    if (event.type == VE_STREAM_EVENT_PARTIAL) {
        replace_live_caption(event.text_utf8);
    } else if (event.type == VE_STREAM_EVENT_FINAL) {
        append_stable_text(event.text_utf8);
    } else if (event.type == VE_STREAM_EVENT_ERROR) {
        report_stream_error(event.error, event.text_utf8);
    } else if (event.type == VE_STREAM_EVENT_END) {
        stream_ended();
    }
}

// Normal stop drains buffered speech and eventually emits END.
voiceengine_stream_finish(stream);
// Immediate abort uses voiceengine_stream_cancel(stream) instead.
// Keep polling through END, then release the session.
voiceengine_stream_destroy(stream);
```

The equivalent C++ wrapper is intentionally small:

```cpp
voiceengine::Stream stream = context.create_stream();

// In the host's capture callback; frames is the number of audio frames.
stream.push(interleaved_samples, frames);

// In the host event loop or polling thread.
if (auto event = stream.poll()) {
    if (event->type == VE_STREAM_EVENT_PARTIAL)
        replace_live_caption(event->text);
    else if (event->type == VE_STREAM_EVENT_FINAL)
        append_stable_text(event->text);
}

stream.finish();
```

`voiceengine_stream_poll()` returns `VE_ERR_BUSY` when no event is currently
available; that is normal and is not an inference failure. A push can return
`VE_ERR_BUSY` only when the decoder has accumulated roughly 120 seconds of
genuinely unprocessed audio on the stream timeline. Cumulative snapshots are
not counted twice. This is backpressure for a decoder that is falling behind,
not an utterance or session-duration limit; hosts must retain and retry the
same capture block rather than silently discard it.
The 120-second threshold is intentionally not sized to GPU memory: enlarging it
does not improve recognition, and would only let a stalled decoder hide more
audio and increase recovery latency. On an RTX 5090 the normal queue should stay
near empty; the threshold remains a last-resort loss-prevention guard.
The host should keep capture blocks short (typically 20-100 ms), avoid unloading
the model while a stream exists, destroy every stream before destroying its
context, and never call `voiceengine_stream_destroy()` concurrently with another
operation on the same stream.

`ve_stream_event::text_bytes` reports the complete UTF-8 byte count and
`text_truncated` is set when the fixed 4096-byte event preview is incomplete.
Automatic rolling keeps normal microphone FINAL events comfortably below that
preview size. A host may still call `voiceengine_stream_flush()` when its own
workflow needs an immediate stable boundary.

`voiceengine_stream_flush()` closes the current utterance but keeps the session
open. `voiceengine_stream_finish()` closes the input and drains it. A FINAL event
is stable for one VAD-delimited or automatically rolled segment; PARTIAL replaces
the current live caption for that segment. Timestamps are milliseconds from the
beginning of the stream and `segment_id` groups all events belonging to the same
display segment.

## Offline task lifecycle

`ve_pcm_desc::frame_count` always counts audio frames. The sample array contains
`frame_count * channels` interleaved float values. File and PCM tasks use the
same ownership sequence:

```cpp
uint64_t task = 0;
voiceengine_submit_file(context, input_path, &task);

ve_task_status status{};
status.struct_size = sizeof(status);
status.api_version = VOICEENGINE_API_VERSION;
do {
    voiceengine_task_poll(context, task, &status);
} while (status.state == VE_TASK_QUEUED
         || status.state == VE_TASK_RUNNING);

ve_result result{};
result.struct_size = sizeof(result);
result.api_version = VOICEENGINE_API_VERSION;
voiceengine_task_result(context, task, &result);
consume(result.text_utf8, result.json_utf8);
voiceengine_result_free(&result);
voiceengine_task_release(context, task);
```

The SDK is GPU-only. `ve_model_params` contains only the optional model
directory; CUDA is a fixed product requirement rather than a caller-selectable
fallback or layer-count option.

## Threading and ownership

- Submit, poll, cancel, result, and release may be called from different host
  threads for different task IDs. Do not call model load or unload concurrently
  with task or stream inference.
- Stream push, poll, flush, finish, and cancel are internally synchronized.
  Destroy is exclusive: stop concurrent stream calls before destroying it.
- Destroy all streams before destroying their context. Destroying a context
  cancels and joins ordinary tasks owned by that context.
- Returned result strings remain owned by the result until
  `voiceengine_result_free()`. `voiceengine_last_error()` returns a thread-local
  snapshot that remains valid until the next last-error call on that thread.
- Public C entry points translate implementation exceptions into status codes;
  C++ exceptions do not form part of the DLL ABI.
