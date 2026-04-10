/**
 * @file conversions.h
 *
 * @brief Byte-order conversions, address rounding, and index/state
 *        mapping utilities.
 *
 * @details Helper routines shared across the protocol and storage
 *          layers. Byte conversions use CCS @c make32 / @c make8
 *          intrinsics and are PIC18-specific.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 *
 * @ingroup protocol
 */
#ifndef CONVERSIONS_H
#define CONVERSIONS_H

/** @name Byte-Order Conversions
 * @{
 */

/**
 * @brief Reconstruct a @c uint32_t from 4 big-endian bytes.
 *
 * @param[in] data  Pointer to a 4-byte big-endian sequence.
 *
 * @return Reconstructed 32-bit value.
 *
 * @warning PIC18-specific (@c make32 intrinsic).
 *
 * @see @ref globals_endian for byte-position indices.
 */
uint32_t bytes_to_uint32(uint8_t *data);

/**
 * @brief Decompose a @c uint32_t into 4 big-endian bytes.
 *
 * @param[out] out_bytes  Destination array (>= 4 bytes).
 * @param[in]  address    Value to convert.
 *
 * @warning PIC18-specific (@c make8 intrinsic).
 */
void uint32_to_bytes(uint8_t *out_bytes, uint32_t address);

/** @brief Convert a nibble (0..15) to uppercase hex ASCII. */
char nibble_to_hex(uint8_t n);
/** @} */

/** @name 4 KiB Address Rounding
 * @{
 */

/** @brief Round down (floor) to the 4 KiB subsector boundary. */
uint32_t round_down_4k(uint32_t address);

/**
 * @brief Round up (ceil) to the next 4 KiB subsector boundary.
 *
 * @return Aligned address, or @ref INVALID_ADDR on wrap.
 */
uint32_t round_up_4k(uint32_t address);
/** @} */

/** @name Index / Data-State Mapping
 *
 * Bidirectional conversion between index entry addresses and
 * their corresponding data-state map addresses.
 *
 * @see @ref FlashSection_e for the memory regions these map between.
 * @{
 */
uint16_t data_state_to_index_entry(uint16_t idx_state);
uint16_t index_entry_to_data_state(uint32_t index);
/** @} */

#endif /* CONVERSIONS_H */
