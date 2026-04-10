/**
 * @file crc.c
 *
 * @brief CRC-16 CCITT table-driven computation and extraction.
 *
 * @details Implements the two functions declared in @ref crc.h: one to
 *          compute a CRC over a frame's command + payload region, and
 *          one to reconstruct the CRC embedded in a received frame.
 *          The 256-entry lookup table lives in SRAM for speed — the ROM
 *          variant (commented below) saves RAM at a small cost.
 *
 * @author MSc Ariel Manabe. Tweaked by the_mchap.
 *
 * @version 1.0
 *
 * @date 2024-12-28
 *
 * @ingroup protocol
 */
#include "globals.h"

#include "../../include/protocol/frame.h"
#include "../../include/protocol/crc.h"
#include "../../include/drivers/uart.h"
#include "../../include/app/task_helper.h"

/* ============ Internal configuration (do not export, bro) ============ */

/** @brief CRC-16 CCITT initial seed value. Do not change. */
#define CRC_SEED 0x1D0FU

/** @brief Mask for isolating the low byte of a 16-bit value. */
#define LOW_BYTE_MASK 0xFF

/** @brief Lookup table size — 2^BITS_PER_BYTE (256 entries). */
#define TABLE_SIZE (1 << BITS_PER_BYTE)

/**
 * @brief Pre-computed CRC-16 CCITT lookup table (256 entries).
 *
 * @details Generated for poly `0x1021` with no reflection. Stored in
 *          SRAM for fastest access. To save SRAM at the cost of speed,
 *          prefix with `rom`:
 *          @code
 *          static const unsigned int16 rom crc_table[TABLE_SIZE] = { ... };
 *          @endcode
 */
static const uint16_t crc_table[TABLE_SIZE] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129,
    0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF, 0x1231, 0x0210, 0x3273, 0x2252,
    0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C,
    0xF3FF, 0xE3DE, 0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D, 0x3653, 0x2672,
    0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4, 0xB75B, 0xA77A, 0x9719, 0x8738,
    0xF7DF, 0xE7FE, 0xD79D, 0xC7BC, 0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861,
    0x2802, 0x3823, 0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC,
    0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A, 0x6CA6, 0x7C87, 0x4CE4, 0x5CC5,
    0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B,
    0x8D68, 0x9D49, 0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78, 0x9188, 0x81A9,
    0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3,
    0x5004, 0x4025, 0x7046, 0x6067, 0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C,
    0xE37F, 0xF35E, 0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D, 0x34E2, 0x24C3,
    0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405, 0xA7DB, 0xB7FA, 0x8799, 0x97B8,
    0xE75F, 0xF77E, 0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676,
    0x4615, 0x5634, 0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3, 0xCB7D, 0xDB5C,
    0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A, 0x4A75, 0x5A54, 0x6A37, 0x7A16,
    0x0AF1, 0x1AD0, 0x2AB3, 0x3A92, 0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B,
    0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36,
    0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0};

/* ============ Internal configuration end, bro ============ */

/**
 * @brief Compute CRC-16 over a frame's command, length, and payload.
 *
 * @details Walks from @ref CMD_ID through the last payload byte
 *          (length derived from @ref DATA_LEN + 2-byte header
 *          overhead). Per iteration:
 *          1. XOR the CRC high byte with the current data byte →
 *             table index.
 *          2. Look up @c crc_table[index], XOR with CRC shifted left
 *             by @ref BITS_PER_BYTE.
 *
 *          Minimum @c appended is 2 (command_id + payload_length),
 *          so the do-while always executes at least once.
 *
 * @param[in] buffer  Raw frame buffer. The @ref DATA_LEN field
 *                    determines how many bytes are processed.
 *
 * @return Computed 16-bit CRC, or 0 if @a buffer is NULL.
 *
 * @pre Buffer must contain at least @ref FRAME_MIN_SIZE bytes.
 *
 * @see extract_checksum_received, assign_crc
 */
uint16_t calculate_crc(uint8_t *buffer)
{
    if (buffer == NULL)
    {
        LOG("\n\r <?> Pointer to calculate CRC is NULL!\n");
        return 0;
    }

    UartPacket *packet = (UartPacket *)buffer;
    /** Get the amount of appended data according to payload length. */
    uint8_t appended = packet->payload_length +  // plus overhead size.
                       (sizeof(packet->command_id) + sizeof(packet->payload_length));

    uint16_t table_index;
    uint16_t crc = CRC_SEED;  // Initial CRC-16 value. DO NOT CHANGE IT.

    /** Operates exclusively over [command_id .. payload[n]] for CRC-16.
     *  appended >= 2 always, no risk of executing with appended == 0. */
    do
    {
        table_index = (crc >> BITS_PER_BYTE ^ *++buffer) & LOW_BYTE_MASK;
        crc = crc_table[table_index] ^ (crc << BITS_PER_BYTE);
    } while (--appended);

    return crc;
}

/**
 * @brief Extract the 16-bit CRC embedded in a received frame.
 *
 * @details Reconstructs the checksum from two big-endian bytes sitting
 *          right after the payload. Position is derived from
 *          @ref UartPacket_s::payload_length via the @ref CrcField_s
 *          overlay.
 *
 * @param[in] buffer  Raw frame buffer.
 *
 * @return Extracted 16-bit CRC, or 0 if @a buffer is NULL.
 *
 * @pre Buffer must be validated for minimum length
 *      (@ref FRAME_MIN_SIZE) before calling.
 *
 * @warning PIC18 8-bit specific byte reconstruction.
 *
 * @see calculate_crc, evaluate_dataframe
 */
uint16_t extract_checksum_received(uint8_t *buffer)
{
    if (buffer == NULL)
    {
        LOG("\n\r <?> Pointer to extract checksum is NULL!");
        return NULL;  // This can be dangerous too, though.
    }
    // Map the struct to the start of the buffer.
    // Calculate where the CRC is based on the 'payload_length' field.
    // The CRC starts right after the payload ends.
    UartPacket *packet = (UartPacket *)buffer;
    CrcField *crc_pointer = (CrcField *)&packet->payload[packet->payload_length];

    return ((uint16_t)crc_pointer->msb << BITS_PER_BYTE) | crc_pointer->lsb;
}
