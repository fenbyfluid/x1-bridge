#pragma once

#include <cstdarg>
#include <functional>

class LogSuspender {
    LogSuspender(const LogSuspender &) = delete;

public:
    ~LogSuspender();
};

class Log {
public:
    static void print(const char *message);
    static void printf(const char *format, ...);
    static void vprintf(const char *format, va_list args);

    static LogSuspender suspendOutputCallback();
    static void setOutputCallback(std::function<void(const std::string &message)> function);
};
