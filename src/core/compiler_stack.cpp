#include "gdpp/core/compiler_stack.hpp"

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace gdpp {
namespace {

thread_local bool on_compiler_stack = false;

struct CompilerStackTask final {
    const std::function<void()>* function{nullptr};
    std::exception_ptr exception;
};

void execute_compiler_task(CompilerStackTask& task) noexcept {
    on_compiler_stack = true;
    try {
        (*task.function)();
    } catch (...) {
        task.exception = std::current_exception();
    }
    on_compiler_stack = false;
}

#if defined(_WIN32)

unsigned int __stdcall compiler_thread_entry(void* context) noexcept {
    execute_compiler_task(*static_cast<CompilerStackTask*>(context));
    return 0;
}

#else

void* compiler_thread_entry(void* context) noexcept {
    execute_compiler_task(*static_cast<CompilerStackTask*>(context));
    return nullptr;
}

#endif

} // namespace

void run_on_compiler_stack(const std::function<void()>& task) {
    if (!task)
        throw std::invalid_argument{"compiler stack task is empty"};
    if (on_compiler_stack) {
        task();
        return;
    }

    CompilerStackTask state{&task, nullptr};
#if defined(_WIN32)
    const auto handle_value =
        _beginthreadex(nullptr, static_cast<unsigned int>(compiler_worker_stack_bytes),
                       &compiler_thread_entry, &state, 0, nullptr);
    if (handle_value == 0)
        throw std::system_error{errno, std::generic_category(),
                                "could not create the compiler worker thread"};
    const auto handle = reinterpret_cast<HANDLE>(handle_value);
    const auto wait_result = WaitForSingleObject(handle, INFINITE);
    const auto wait_error = wait_result == WAIT_OBJECT_0 ? ERROR_SUCCESS : GetLastError();
    const auto close_result = CloseHandle(handle);
    const auto close_error = close_result != 0 ? ERROR_SUCCESS : GetLastError();
    if (wait_result != WAIT_OBJECT_0)
        throw std::system_error{static_cast<int>(wait_error), std::system_category(),
                                "could not join the compiler worker thread"};
    if (close_result == 0)
        throw std::system_error{static_cast<int>(close_error), std::system_category(),
                                "could not close the compiler worker handle"};
#else
    pthread_attr_t attributes;
    auto error = pthread_attr_init(&attributes);
    if (error != 0)
        throw std::system_error{error, std::generic_category(),
                                "could not initialize compiler worker attributes"};
    error = pthread_attr_setstacksize(&attributes, compiler_worker_stack_bytes);
    if (error != 0) {
        (void)pthread_attr_destroy(&attributes);
        throw std::system_error{error, std::generic_category(),
                                "could not reserve the compiler worker stack"};
    }
    pthread_t thread;
    error = pthread_create(&thread, &attributes, &compiler_thread_entry, &state);
    const auto destroy_error = pthread_attr_destroy(&attributes);
    if (error != 0)
        throw std::system_error{error, std::generic_category(),
                                "could not create the compiler worker thread"};
    error = pthread_join(thread, nullptr);
    if (error != 0)
        throw std::system_error{error, std::generic_category(),
                                "could not join the compiler worker thread"};
    if (destroy_error != 0)
        throw std::system_error{destroy_error, std::generic_category(),
                                "could not destroy compiler worker attributes"};
#endif
    if (state.exception)
        std::rethrow_exception(state.exception);
}

} // namespace gdpp
