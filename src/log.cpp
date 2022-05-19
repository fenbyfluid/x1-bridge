#include "log.h"

#include <string>
#include <stdexcept>

static std::function<void(const char *message)> output_callback = nullptr;

void Log::print(const char *message) {
    ::fputs(message, stdout);
    ::fflush(stdout);

    if (output_callback) {
        output_callback(message);
    }
}

void Log::printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void Log::vprintf(const char *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);

    if (length < 0) {
        throw std::runtime_error("string formatting failed");
    }

    std::string buffer(length, '\0');
    vsnprintf(&buffer[0], buffer.size() + 1, format, args);

    print(buffer.c_str());
}

void Log::setOutputCallback(std::function<void(const char *message)> function) {
    output_callback = function;
}
