/**
 * @file endian.h
 *
 * @brief Byte-position index enums for 4-byte big/little-endian fields.
 *
 * @see conversions.c for the conversion routines that rely on these.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup globals_endian
 */
#ifndef ENDIAN_H
#define ENDIAN_H

/**
 * @defgroup globals_endian Endianness Helpers
 * @brief Byte-position index enums for 4-byte big/little-endian fields.
 * @{
 */

/**
 * @enum BigEndianBytes_e
 * @brief Byte-position indices for big-endian 4-byte fields.
 */
typedef enum BigEndianBytes_e
{
    BIG_END_LSB,  ///< Least significant byte (index 0).
    BIG_END_B_2,  ///< Byte 2.
    BIG_END_B_1,  ///< Byte 1.
    BIG_END_MSB   ///< Most significant byte (index 3).
} BigEndianBytes;

/**
 * @enum LittleEndianBytes_e
 * @brief Byte-position indices for little-endian 4-byte fields.
 */
typedef enum LittleEndianBytes_e
{
    LIL_END_MSB,  ///< Most significant byte (index 0).
    LIL_END_B_1,  ///< Byte 1.
    LIL_END_B_2,  ///< Byte 2.
    LIL_END_LSB   ///< Least significant byte (index 3).
} LittleEndianBytes;

/** @} */ /* globals_endian */

#endif /* ENDIAN_H */
