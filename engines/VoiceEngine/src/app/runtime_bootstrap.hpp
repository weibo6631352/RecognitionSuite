#pragma once

#include <string>

namespace voiceengine_app {

// Configure process-wide DLL search directories before VoiceEngineCore.dll is
// delay-loaded. The returned cookies are retained for the process lifetime.
bool initialize_private_runtimes(const wchar_t* runtime_root,
                                 std::wstring* error = nullptr);

}  // namespace voiceengine_app
