#include "log.h"

#include <string>
#include <stdexcept>

static bool output_callback_suspended = false;
static std::string output_callback_buffer = "";
static std::function<void(const std::string &message)> output_callback = nullptr;

LogSuspender::~LogSuspender() {
    output_callback_suspended = false;
}

void Log::print(const char *message) {
    ::fputs(message, stdout);
    ::fflush(stdout);

    if (output_callback && !output_callback_suspended) {
        output_callback_buffer.append(message);

        for (;;) {
            size_t newline = output_callback_buffer.find('\n');
            if (newline == std::string::npos) {
                break;
            }

            std::string line(output_callback_buffer.c_str(), newline);
            output_callback_buffer.erase(0, newline + 1);

            output_callback(line);
        }
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

    std::string buffer(length + 1, '\0');
    vsnprintf(&buffer[0], buffer.size(), format, args);
    buffer.resize(length);

    print(buffer.c_str());
}

LogSuspender Log::suspendOutputCallback() {
    output_callback_suspended = true;
    return {};
}

void Log::setOutputCallback(std::function<void(const std::string &message)> function) {
    output_callback = function;
}
