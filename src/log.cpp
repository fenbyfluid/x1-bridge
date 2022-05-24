#include "log.h"

#include <string>
#include <stdexcept>

static bool output_callback_suspended = false;
static std::function<void(const char *message)> output_callback = nullptr;

LogSuspender::~LogSuspender() {
    output_callback_suspended = false;
}

void Log::print(const char *message) {
    ::fputs(message, stdout);
    ::fflush(stdout);

    if (output_callback && !output_callback_suspended) {
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

LogSuspender Log::suspendOutputCallback() {
    output_callback_suspended = true;
    return {};
}

void Log::setOutputCallback(std::function<void(const char *message)> function) {
    output_callback = function;
}
