/**
 * @file crc.h
 *
 * @brief CRC-16 CCITT table-driven checksum computation and extraction.
 *
 * @details Adapted from [pycrc](https://pycrc.org) with the following
 *          configuration:
 *
 *          | Parameter    | Value           |
 *          |--------------|-----------------|
 *          | Width        | 16              |
 *          | Poly         | `0x1021`        |
 *          | XorIn        | `0x1D0F`        |
 *          | ReflectIn    | False           |
 *          | XorOut       | `0x0000`        |
 *          | ReflectOut   | False           |
 *          | Algorithm    | Table-driven    |
 *
 *          **Test vectors:**
 *
 *          | Input                          | Checksum |
 *          |--------------------------------|----------|
 *          | `{}`  (empty)                  | `0x1D0F` |
 *          | `{0x65}`                       | `0x9479` |
 *          | `{0x31, 0x32, ..., 0x39}`      | `0xE5CC` |
 *          | `{0x65}` x 256                 | `0xE938` |
 *
 * @author MSc Ariel Manabe
 *
 * @version 1.0
 *
 * @date 2024-12-28
 */
#ifndef CRC_H
#define CRC_H

/**
 * @defgroup protocol Protocol Layer
 * @brief CRC, byte conversions, command enums, and task helpers.
 *
 * @see @ref globals_protocol for frame layout and @ref Dataframe_s.
 * @{
 */

/**
 * @brief Compute CRC-16 over a frame's command, length, and payload.
 *
 * @details Seeded with `0x1D0F`. Computed from @ref CMD_ID through the
 *          last payload byte. The header (@ref UNIT_ID) and footer
 *          (CRC + @ref STOP_BYTE) are excluded.
 *
 * @param[in] data  Raw frame buffer. Size is derived from the
 *                  @ref DATA_LEN field internally.
 *
 * @return Computed 16-bit CRC.
 *
 * @pre Buffer must contain at least @ref FRAME_MIN_SIZE bytes.
 *
 * @see extract_checksum_received
 */
uint16_t calculate_crc(uint8_t *data);

/**
 * @brief Extract the 16-bit CRC embedded in a received frame.
 *
 * @details Reconstructs the CRC from the two big-endian bytes
 *          immediately following the payload (position derived
 *          from @ref DATA_LEN).
 *
 * @param[in] data  Raw frame buffer.
 *
 * @return Extracted 16-bit CRC.
 *
 * @pre Buffer must be validated for minimum length
 *      (@ref FRAME_MIN_SIZE) before calling.
 *
 * @warning PIC18 8-bit specific byte reconstruction.
 *
 * @see calculate_crc
 */
uint16_t extract_checksum_received(uint8_t *data);

/** @} */ /* protocol */

#endif /* CRC_H */
