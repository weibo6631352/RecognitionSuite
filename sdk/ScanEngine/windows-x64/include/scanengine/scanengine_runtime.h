#ifndef SCANENGINE_RUNTIME_H
#define SCANENGINE_RUNTIME_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once, on the process main thread, before the first ScanEngineCore API
// call. Consumers linking ScanEngineCore.lib must delay-load ScanEngineCore.dll.
int32_t scanengine_runtime_initialize(const wchar_t* runtime_root_utf16,
                                    wchar_t* error_buffer,
                                    uint32_t error_buffer_chars);

#ifdef __cplusplus
}
#endif

#endif
