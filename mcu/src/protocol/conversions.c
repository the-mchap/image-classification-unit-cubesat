/**
 * @file conversions.c
 *
 * @brief Byte-order conversions, 4 KiB address rounding, and
 *        index/data-state mapping implementation.
 *
 * @details All byte-order helpers rely on CCS @c make32 / @c make8
 *          intrinsics — they are not portable beyond PIC18. The
 *          index ↔ data-state conversions use bit shifts and masks
 *          derived from the flash section geometry.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 *
 * @ingroup protocol
 */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/protocol/endian.h"
#include "../../include/protocol/conversions.h"

/* ================== Internal configuration (do not export) ================== */

/**
 * @def ROUND_RATE
 * @brief Bitmask for flooring an address to its 4 KiB subsector start.
 *
 * @details Equivalent to @c ~(SECTOR_4K_SIZE - 1).
 */
#define ROUND_RATE 0xFFFFF000UL  // No pun intended here, as a matter of fact.

/**
 * @enum IndexConvertConstant_e
 * @brief Bit-manipulation constants for index ↔ data-state conversion.
 *
 * @see index_entry_to_data_state, data_state_to_index_entry
 */
typedef enum IndexConvertConstant_e
{
    ENTRY_TO_MAP_RATE = 0x0FFFU,  ///< Mask to isolate the offset within a section.
    MAP_TO_ENTRY_RATE = 0x1000U,  ///< Base offset OR'd back after shifting.
    SHIFT_RATE = 3                ///< Bit-shift between entry and map address spaces.
} IndexConvertConstant;

/* ======================== Internal configuration end ======================== */

/**
 * @brief Reconstruct a @c uint32_t from 4 big-endian bytes.
 *
 * @param[in] data  Pointer to a 4-byte big-endian sequence.
 *
 * @return Reconstructed 32-bit value.
 *
 * @warning PIC18-specific (@c make32 intrinsic).
 *
 * @see uint32_to_bytes, @ref globals_endian
 */
uint32_t bytes_to_uint32(uint8_t *data)
{
    return make32(data[LIL_END_MSB], data[LIL_END_B_1], data[LIL_END_B_2],
                  data[LIL_END_LSB]);
}

#ifdef DEBUG_MODE
/**
 * @brief Reconstruct a @c uint32_t from 4 little-endian bytes.
 *
 * @param[in] data  Pointer to a 4-byte little-endian sequence.
 *
 * @return Reconstructed 32-bit value.
 *
 * @warning PIC18-specific. @ref DEBUG_MODE only.
 */
uint32_t bytes_to_uint32_little(uint8_t *data)
{
    return make32(data[3], data[2], data[1], data[0]);
}
#endif /* DEBUG_MODE */

/**
 * @brief Decompose a @c uint32_t into 4 big-endian bytes.
 *
 * @param[out] out_bytes  Destination array (>= 4 bytes).
 * @param[in]  address    Value to decompose.
 *
 * @warning PIC18-specific (@c make8 intrinsic).
 *
 * @see bytes_to_uint32
 */
void uint32_to_bytes(uint8_t *out_bytes, uint32_t address)
{
    *(out_bytes + LIL_END_MSB) = make8(address, BIG_END_MSB);
    *(out_bytes + LIL_END_B_1) = make8(address, BIG_END_B_1);
    *(out_bytes + LIL_END_B_2) = make8(address, BIG_END_B_2);
    *(out_bytes + LIL_END_LSB) = make8(address, BIG_END_LSB);
}

/**
 * @brief Floor an address to its 4 KiB subsector boundary.
 *
 * @param[in] address  Address to round down.
 *
 * @return Aligned subsector start.
 *
 * @see round_up_4k
 */
uint32_t round_down_4k(uint32_t address)
{
    return address & ROUND_RATE;
}

/**
 * @brief Ceil an address to the next 4 KiB subsector boundary.
 *
 * @param[in] address  Address to round up.
 *
 * @return Aligned address, or @ref INVALID_ADDR if rounding would
 *         wrap past @ref DATA_END.
 *
 * @see round_down_4k
 */
uint32_t round_up_4k(uint32_t address)
{
    return (address >= DATA_END - SECTOR_4K_SIZE + 1)
               ? INVALID_ADDR
               : round_down_4k(address + SECTOR_4K_SIZE);
}

/**
 * @brief Convert an index entry address to its data-state map address.
 *
 * @param[in] index_entry_addr  Index entry address.
 *
 * @return Corresponding data-state map address.
 *
 * @see data_state_to_index_entry, @ref FlashSection_e
 */
uint16_t index_entry_to_data_state(uint32_t index_entry_addr)
{
    return (index_entry_addr & ENTRY_TO_MAP_RATE) >> SHIFT_RATE;
}

/**
 * @brief Convert a data-state map address to its index entry address.
 *
 * @param[in] index_map_addr  Data-state map address.
 *
 * @return Corresponding index entry address.
 *
 * @see index_entry_to_data_state, @ref FlashSection_e
 */
uint16_t data_state_to_index_entry(uint16_t index_map_addr)
{
    return (index_map_addr << SHIFT_RATE) | MAP_TO_ENTRY_RATE;
}

/**
 * @brief Convert a nibble (0..15) to its uppercase hex ASCII character.
 *
 * @param[in] n  Value 0..15.
 *
 * @return @c '0'–@c '9' or @c 'A'–@c 'F'.
 */
char nibble_to_hex(uint8_t n)
{
    return (n < 10) ? ('0' + n) : ('A' + n - 10);
}
