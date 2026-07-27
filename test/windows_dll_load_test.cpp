#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string format_windows_error(const DWORD code) {
    char* message = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&message), 0, nullptr);
    const std::string result =
        length != 0 && message != nullptr ? std::string{message, length} : "unknown error";
    if (message != nullptr)
        LocalFree(message);
    return result;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "usage: gdpp_windows_dll_load_test <compiler-extension.dll>\n";
        return 2;
    }

    const std::filesystem::path library_path = std::filesystem::absolute(argv[1]);
    SetLastError(ERROR_SUCCESS);
    const HMODULE library =
        LoadLibraryExW(library_path.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library == nullptr) {
        const DWORD error = GetLastError();
        std::cerr << "LoadLibraryExW failed for " << library_path.string() << ": " << error << " ("
                  << format_windows_error(error) << ")\n";
        return 1;
    }

    if (GetProcAddress(library, "gdpp_library_init") == nullptr) {
        const DWORD error = GetLastError();
        std::cerr << "gdpp_library_init is not exported by " << library_path.string() << ": "
                  << error << " (" << format_windows_error(error) << ")\n";
        FreeLibrary(library);
        return 1;
    }

    if (FreeLibrary(library) == 0) {
        const DWORD error = GetLastError();
        std::cerr << "FreeLibrary failed for " << library_path.string() << ": " << error << " ("
                  << format_windows_error(error) << ")\n";
        return 1;
    }

    return 0;
}
