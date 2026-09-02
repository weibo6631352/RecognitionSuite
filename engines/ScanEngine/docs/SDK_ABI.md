# ScanEngine external SDK

ScanEngine exposes a stable Windows x64 C ABI. Consumers link the imported CMake
target `ScanEngine::SDK`, call `scanengine_runtime_initialize()` once on the process
main thread, and only then call functions in `scanengine_api.h`.
The release build creates a standalone `sdk/` directory beside `bin/`; the SDK
does not depend on files from the desktop application's directory.

## ABI rules

- Public symbols use `extern "C"`; Qt, STL, exceptions, and C++ classes never
  cross the DLL boundary.
- Every extensible structure begins with `struct_size` and `api_version`.
- Set both fields before every call. Version 1 output functions validate the
  complete v1 structure size and never write beyond the caller's declared
  capacity; future fields may only be appended.
- Paths use UTF-16 on Windows; status and result text use UTF-8.
- Memory returned by the core is released by `scanengine_free()` or
  `scanengine_result_free()`.
- Contexts are strongly typed opaque handles. Submit, poll, cancel, result, and
  release form the same
  asynchronous task lifecycle used by the VoiceEngine SDK.
- One context runs one parse at a time. Multiple contexts may be created, but
  GPU memory and the underlying model cache are process resources. All
  ScanEngine contexts in one process use the same package root.
- A context retains only its most recently submitted task. A new submit replaces
  the previous completed task. After consuming a result, call
  `scanengine_result_free()` and `scanengine_task_release()`; release returns
  `SE_ERR_BUSY` while parsing is active and invalidates that task ID.
- Parse effort is the typed `se_effort` enum. `SE_EFFORT_DEFAULT` currently
  selects the same balanced policy as `SE_EFFORT_MEDIUM`.

## CMake consumer

```cmake
find_package(ScanEngine CONFIG REQUIRED
    PATHS "PATH_TO_SDK/cmake" NO_DEFAULT_PATH)
target_link_libraries(my_app PRIVATE ScanEngine::SDK)
```

Before the first core call:

```cpp
#include <scanengine/scanengine_runtime.h>

wchar_t error[1024]{};
if (!scanengine_runtime_initialize(L"PATH_TO_SDK/bin", error, 1024)) {
    // report error
}
```

The imported target supplies the core import library, runtime bootstrap static
library, `delayimp`, and `/DELAYLOAD:ScanEngineCore.dll`. `sdk/bin` contains the
core DLL, Qt runtime, models, configuration, and private `runtimes/cuda` tree.
`SCANENGINE_RUNTIME_ROOT`, exported by the CMake package, is the absolute path to
that directory.

```text
build/<preset>/
|-- bin/                  desktop software build
`-- sdk/                  standalone third-party SDK
    |-- bin/              Core DLL, models, Qt, and private runtime
    |-- include/
    |-- lib/
    |-- cmake/
    |-- examples/
    |-- doc/
    `-- licenses/
```

Create the context on the process main thread. A non-Qt host gets a
private `QCoreApplication`; a host that already owns a `QGuiApplication` keeps
the optional platform-font rendering path as well. A Qt host must use a Qt 5
binary compatible with the packaged Qt 5.12.9 runtime.

`sdk/examples/sdk_smoke` is a standalone consumer. With only the package root
argument it checks DLL loading and GPU readiness; adding an input file and an
output root runs the full asynchronous parse lifecycle.

## C++ convenience API

Applications using C++17 may include `scanengine/scanengine.hpp`. This is a
header-only RAII layer over the same C ABI; it adds no DLL and no third-party
dependency. Context, task, and result memory are managed automatically. Errors
are reported as `scanengine::Error`, whose `status()` is the original `se_status`.

```cmake
target_link_libraries(my_cpp_app PRIVATE ScanEngine::CXX)
```

`ScanEngine::CXX` selects C++17 and links the same `ScanEngine::SDK` target.

```cpp
#include <scanengine/scanengine.hpp>

auto context = scanengine::Context::open(package_root);
scanengine::Result result = context.submit_file(
    input_path, output_root, SE_EFFORT_MEDIUM).wait();
use(result.output_dir, result.excel_path, result.markdown_path);
```

`Task` is movable and not copyable. It retains its context, so the underlying C
context stays alive until the task is consumed or destroyed. `Task::result()`
copies all DLL-owned strings and paths, then releases the task. Destroying an
unfinished task requests cancellation; final context destruction may wait for
its worker to stop. The one-task-per-context rule is unchanged.
`sdk/examples/sdk_cpp_smoke` is an independently buildable C++ consumer.

## Parse lifecycle

```cpp
se_parse_params parse{};
parse.struct_size = sizeof(parse);
parse.api_version = SCANENGINE_API_VERSION;
parse.input_path_utf16 = input;
parse.output_root_utf16 = output_root;
parse.effort = SE_EFFORT_MEDIUM;

uint64_t task = 0;
scanengine_submit_file(context, &parse, &task);

se_task_status status{};
status.struct_size = sizeof(status);
status.api_version = SCANENGINE_API_VERSION;
do {
    scanengine_task_poll(context, task, &status);
} while (status.state == SE_TASK_QUEUED
         || status.state == SE_TASK_RUNNING);

se_result result{};
result.struct_size = sizeof(result);
result.api_version = SCANENGINE_API_VERSION;
scanengine_task_result(context, task, &result);
consume_outputs(result.excel_path_utf16,
                result.markdown_path_utf16,
                result.json_path_utf16);
scanengine_result_free(&result);
scanengine_task_release(context, task);
```

## Threading and ownership

- One context is single-flight. Poll and cancel may run from a host thread while
  its parse worker is active; a second submit returns `SE_ERR_BUSY`.
- Context destruction is exclusive. Stop new calls first; destruction requests
  cancellation and joins the active worker.
- Result strings and paths remain owned by `se_result` until
  `scanengine_result_free()`. Releasing the task does not invalidate an already
  copied result.
- `scanengine_last_error()` returns a thread-local snapshot that remains valid
  until the next last-error call on that thread.
- Parser worker exceptions are contained and reported as `SE_ERR_INTERNAL`;
  C++ exceptions do not form part of the DLL ABI.
