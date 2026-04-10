/**
 * @file logger.c
 *
 * @brief Centralized debug logging engine — the only compilation unit
 *        that touches @c fputc on @c DBG_STREAM.
 *
 * @details In CCS C's Multiple Compilation Units mode, every unit that
 *          calls @c fprintf generates its own copy of the printf runtime
 *          (format parser, integer-to-ASCII conversion, etc.). With 13+
 *          units using @ref LOG, the combined label count overflows the
 *          linker's ~32 K limit.
 *
 *          This file solves the problem by providing a single @ref logger
 *          function that replaces @c fprintf entirely. All output goes
 *          through @c fputc — no printf runtime is generated anywhere
 *          in the project. The format string is parsed manually using
 *          CCS C's @c <stdarg.h> for variadic argument extraction.
 *
 *          Argument types are determined by peeking at the va_list entry
 *          size byte (CCS C tags each variadic argument with its byte
 *          count), so format modifier accuracy (e.g., @c L vs @c l) is
 *          not critical — the correct width is always read from the
 *          argument list itself.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-30
 *
 * @ingroup logger
 */
#include "globals.h"

#include "logger.h"

#ifdef DEBUG_MODE

#include <stdarg.h>

/* ──────────── Private (inline helpers) ──────────── */

/**
 * @def READ_INT_ARG(ap, dest)
 * @brief Extract the next integer from the CCS va_list into @p dest.
 *
 * @details Peeks at the va_list entry-size byte to auto-detect
 *          uint8_t / uint16_t / uint32_t, then promotes to uint32_t.
 *          Must be a macro — @c va_arg needs the caller's @c ap.
 */
#define READ_INT_ARG(ap, dest)                                                           \
    do                                                                                   \
    {                                                                                    \
        uint8_t _sz = (uint8_t)*(ap) - 1;                                                \
        if (_sz <= 1)                                                                    \
            (dest) = (uint32_t)va_arg((ap), uint8_t);                                    \
        else if (_sz <= 2)                                                               \
            (dest) = (uint32_t)va_arg((ap), uint16_t);                                   \
        else                                                                             \
            (dest) = va_arg((ap), uint32_t);                                             \
    } while (false)

typedef enum Format_e
{
    UNSIGNED_INT = 'u',
    CHARACTER = 's',
    SIGNED_INT = 'd',
    UPPER_HEX = 'X',
    LOWER_HEX = 'x'
} Format;

#inline
static void parse_spec(char **fmt, uint8_t *zero_pad, uint8_t *width);
#inline
static void put_str(char *str);
static void put_dec(uint32_t val, uint8_t width, uint8_t zero_pad);
static void put_hex(uint32_t val, uint8_t width, uint8_t zero_pad, uint8_t uppercase);

/* ========================== Public =========================== */

/**
 * @brief Printf-style debug logger — format engine for @ref LOG.
 *
 * @details Parses @p fmt and emits characters via @c fputc to
 *          @c DBG_STREAM. Variadic arguments are extracted using
 *          CCS C's @c <stdarg.h>; argument sizes are auto-detected
 *          by inspecting the va_list entry-size byte, so
 *          uint8_t / uint16_t / uint32_t values are all handled correctly
 *          regardless of whether @c L or @c l modifiers are present.
 *
 * Supported specifiers: @c %s, @c %u, @c %d, @c %X, @c %x, @c %%.
 * Width and zero-pad flag (@c 0) are supported. Length modifiers
 * (@c L, @c l) are accepted and skipped — type is inferred from
 * the actual argument size in the va_list.
 *
 * @param[in] fmt Format string.
 * @param[in] ... Optional arguments matching the format specifiers.
 */
void logger(char *fmt, ...)
{
    va_list ap;
    uint8_t ch;
    uint8_t zero_pad;
    uint8_t width;
    uint32_t val;

    va_start(ap, fmt);

    while (*fmt)
    {
        if (*fmt != '%')
        {
            fputc(*fmt++, DBG_STREAM);
            continue;
        }
        fmt++;

        if (*fmt == '\0')
            break;
        if (*fmt == '%')
        {
            fputc('%', DBG_STREAM);
            fmt++;
            continue;
        }

        parse_spec(&fmt, &zero_pad, &width);
        ch = *fmt++;

        switch (ch)
        {
        case UNSIGNED_INT:
        case SIGNED_INT:
            READ_INT_ARG(ap, val);
            put_dec(val, width, zero_pad);
            break;

        case UPPER_HEX:
        case LOWER_HEX:
            READ_INT_ARG(ap, val);
            put_hex(val, width, zero_pad, ch == 'X');
            break;

        case CHARACTER:
            put_str(va_arg(ap, char *));
            break;

        default:
            fputc('%', DBG_STREAM);
            fputc(ch, DBG_STREAM);
            break;
        }
    }
    // va_end(ap);
}

/* ========================== Private ========================== */

/**
 * @brief Convert a 4-bit nibble to its ASCII hex character.
 *
 * @param[in] nibble    Value 0–15.
 * @param[in] uppercase Non-zero for A–F, zero for a–f.
 *
 * @return The hex character.
 */
#inline
static char to_hex(uint8_t nibble, uint8_t uppercase)
{
    if (nibble < 10)
        return '0' + nibble;
    return (uppercase ? 'A' : 'a') + nibble - 10;
}

/**
 * @brief Emit a 32-bit value as a hex string to @c DBG_STREAM.
 *
 * @param[in] val       Value to format.
 * @param[in] width     Minimum field width (0 = no padding).
 * @param[in] zero_pad  Non-zero to pad with '0', otherwise spaces.
 * @param[in] uppercase Non-zero for A–F.
 */
static void put_hex(uint32_t val, uint8_t width, uint8_t zero_pad, uint8_t uppercase)
{
    char buf[8];
    uint8_t len = 0;
    uint8_t pad;

    if (val == 0)
    {
        buf[len++] = '0';
    }
    else
    {
        while (val != 0)
        {
            buf[len++] = to_hex((uint8_t)(val & 0x0F), uppercase);
            val >>= 4;
        }
    }

    pad = (width > len) ? width - len : 0;
    while (pad-- > 0)
        fputc(zero_pad ? '0' : ' ', DBG_STREAM);
    while (len-- > 0)
        fputc(buf[len], DBG_STREAM);
}

/**
 * @brief Emit a 32-bit value as unsigned decimal to @c DBG_STREAM.
 *
 * @param[in] val      Value to format.
 * @param[in] width    Minimum field width (0 = no padding).
 * @param[in] zero_pad Non-zero to pad with '0', otherwise spaces.
 */
static void put_dec(uint32_t val, uint8_t width, uint8_t zero_pad)
{
    char buf[10];
    uint8_t len = 0;
    uint8_t pad;

    if (val == 0)
    {
        buf[len++] = '0';
    }
    else
    {
        while (val != 0)
        {
            buf[len++] = '0' + (uint8_t)(val % 10);
            val /= 10;
        }
    }

    pad = (width > len) ? width - len : 0;
    while (pad-- > 0)
        fputc(zero_pad ? '0' : ' ', DBG_STREAM);
    while (len-- > 0)
        fputc(buf[len], DBG_STREAM);
}

/**
 * @brief Advance @p fmt past the zero-pad flag, width digits,
 *        and optional length modifier (L / l).
 */
#inline
static void parse_spec(char **fmt, uint8_t *zero_pad, uint8_t *width)
{
    *zero_pad = 0;
    if (**fmt == '0')
    {
        *zero_pad = 1;
        (*fmt)++;
    }

    *width = 0;
    while (**fmt >= '0' && **fmt <= '9')
    {
        *width = *width * 10 + (**fmt - '0');
        (*fmt)++;
    }

    if (**fmt == 'L' || **fmt == 'l')
        (*fmt)++;
}

/**
 * @brief Emit a null-terminated string to @c DBG_STREAM.
 */
#inline
static void put_str(char *str)
{
    while (*str)
    {
        fputc(*str++, DBG_STREAM);
    }
}

#endif /* DEBUG_MODE */
/* I'm burning through the sky, yeah
 * Two hundred degrees, that's why they call me Mister Fahrenheit
 * I'm travelling at the speed of light
 * I want to make a supersonic woman of you
 *                                    --Freddie Mercury, 1978 **/
