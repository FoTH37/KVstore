#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#ifdef __cplusplus
// C++ 环境：日志级别常量
constexpr int LOG_LEVEL_DEBUG = 0;
constexpr int LOG_LEVEL_INFO  = 1;
constexpr int LOG_LEVEL_WARN  = 2;
constexpr int LOG_LEVEL_ERROR = 3;

// 全局日志级别变量
inline int g_logLevel = LOG_LEVEL_INFO;
#else
// C 环境：日志级别常量
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

// 在 C 文件中声明外部变量（在 main.cc 中定义）
extern int g_logLevel;
#endif

// 日志宏（兼容 C 和 C++）
#define LOG_DEBUG(fmt, ...) \
    do { if (g_logLevel <= LOG_LEVEL_DEBUG) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)  \
    do { if (g_logLevel <= LOG_LEVEL_INFO)  fprintf(stderr, "[INFO] "  fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)  \
    do { if (g_logLevel <= LOG_LEVEL_WARN)  fprintf(stderr, "[WARN] "  fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...) \
    do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)

#endif