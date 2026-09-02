#include <scanengine/scanengine.hpp>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 4) {
        std::wcerr << L"usage: scanengine_sdk_cpp_smoke RUNTIME_ROOT "
                      L"[INPUT_FILE OUTPUT_ROOT]\n";
        return 2;
    }
    try {
        auto context = scanengine::Context::open(argv[1]);
        const auto runtime = context.runtime_info();
        std::cout << "backend " << runtime.backend
                  << ", gpu=" << runtime.gpu_available << std::endl;
        if (argc == 4) {
            const auto result = context.submit_file(
                argv[2], argv[3], SE_EFFORT_MEDIUM).wait();
            std::wcout << result.output_dir << std::endl;
        }
        return 0;
    } catch (const scanengine::Error& error) {
        std::cerr << error.what() << " (status=" << error.status() << ")\n";
        return 3;
    }
}
