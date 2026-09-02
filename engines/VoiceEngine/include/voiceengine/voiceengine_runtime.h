#ifndef VOICEENGINE_RUNTIME_H
#define VOICEENGINE_RUNTIME_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must be called before the first VoiceEngineCore API call. Consumers that use
// VoiceEngineCore.lib must also delay-load VoiceEngineCore.dll. A null root uses
// the executable directory. Returns 1 on success and 0 on failure.
int32_t voiceengine_runtime_initialize(const wchar_t* runtime_root_utf16,
                                      wchar_t* error_buffer,
                                      uint32_t error_buffer_chars);

#ifdef __cplusplus
}
#endif

#endif
