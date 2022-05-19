#pragma once

#include <cstdarg>
#include <functional>

class Log {
public:
    static void print(const char *message);
    static void printf(const char *format, ...);
    static void vprintf(const char *format, va_list args);

    static void setOutputCallback(std::function<void(const char *message)> function);
};
