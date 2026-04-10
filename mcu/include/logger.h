/**
 * @file logger.h
 *
 * @brief Logging macro for debug/production builds.
 *
 * @details Provides a single @ref LOG macro that routes to @ref logger
 *          in @c logger.c under @ref DEBUG_MODE, and vanishes entirely
 *          in production builds — zero overhead, zero code.
 *
 *          All serial output is confined to the single @c logger.c
 *          compilation unit, preventing the CCS linker label overflow
 *          that occurs when multiple units each generate their own
 *          printf runtime.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-30
 */

#ifndef LOGGER_H
#define LOGGER_H

/**
 * @defgroup logger Logger
 * @brief Compile-time switchable logging facility.
 * @{
 */

#ifdef DEBUG_MODE
/**
 * @def LOG(fmt, ...)
 * @brief Printf-style logging to the debug UART stream.
 *
 * Expands to a call to @ref logger when @ref DEBUG_MODE is defined.
 * In production builds, stripped entirely by the preprocessor —
 * the call site compiles to nothing.
 *
 * @param[in] fmt  Format string (printf-style).
 * @param[in] ...  Optional format arguments.
 *
 * @note Uses @c ##__VA_ARGS__ to allow calls with no variadic
 *       arguments, e.g., `LOG("hello")`.
 *
 * @see logger, globals.h for @c DBG_STREAM and @ref DEBUG_MODE.
 */
void logger(char *fmt, ...);
#define LOG(fmt, ...) logger(fmt, ##__VA_ARGS__)
#else
/* Production: strip LOG calls entirely, zero code. */
#define LOG(fmt, ...)
#endif /* DEBUG_MODE */

/** @} */ /* logger */

#endif /* LOGGER_H */
