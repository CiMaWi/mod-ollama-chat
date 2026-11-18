// Selftest stand-in for the core's Log.h: the log macros take their arguments and write nothing.
#ifndef OLLAMA_SELFTEST_LOG_H
#define OLLAMA_SELFTEST_LOG_H

template <typename... Args>
inline void SelftestLog(Args&&...)
{
}

#define LOG_ERROR(...) SelftestLog(__VA_ARGS__)
#define LOG_WARN(...) SelftestLog(__VA_ARGS__)
#define LOG_INFO(...) SelftestLog(__VA_ARGS__)
#define LOG_DEBUG(...) SelftestLog(__VA_ARGS__)

#endif  // OLLAMA_SELFTEST_LOG_H
