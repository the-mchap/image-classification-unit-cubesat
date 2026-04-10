/**
 * @file task_helper.h
 *
 * @brief Frame construction, CRC assignment, ACK/NACK detection, and
 *        payload extraction utilities for the task handlers.
 *
 * @details Provides the shared building blocks that @c obc_tasks.c,
 *          @c rpi_tasks.c, and @c dbg_tasks.c use to parse incoming
 *          frames, build responses, and validate integrity. Defines the
 *          overlay structs (@ref UartPacket_s, @ref CrcField_s) that
 *          map directly onto the raw UART byte buffer.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-04
 *
 * @ingroup app
 */
#ifndef TASK_HELPER_H
#define TASK_HELPER_H

#include "../drivers/uart.h"

/**
 * @struct UartPacket_s
 * @brief Overlay struct mapping the header + payload region of a raw
 *        UART frame buffer.
 *
 * @details Cast a @c uint8_t* buffer to @c UartPacket* to access
 *          fields by name instead of magic offsets:
 *
 * @code
 *  UartPacket *pkt = (UartPacket *)buffer;
 *  uint8_t cmd = pkt->command_id;
 *  uint8_t len = pkt->payload_length;
 * @endcode
 *
 * @see @ref FrameProtocolIndex_e for the matching byte-offset enum.
 * @see CrcField_s for the footer that follows the payload.
 */
typedef struct UartPacket_s
{
    uint8_t sender_id;                  ///< @ref UNIT_ID byte.
    uint8_t command_id;                 ///< @ref CMD_ID byte.
    uint8_t payload_length;             ///< @ref DATA_LEN byte.
    uint8_t payload[PAYLOAD_MAX_SIZE];  ///< Variable-length payload.
} UartPacket;

/**
 * @struct CrcField_s
 * @brief Overlay struct for the frame footer (CRC + stop byte).
 *
 * @details Sits immediately after @c UartPacket_s::payload at offset
 *          @c payload_length. Cast to access CRC and stop byte:
 *
 * @code
 *  CrcField *crc = (CrcField *)&pkt->payload[pkt->payload_length];
 * @endcode
 *
 * @see assign_crc, extract_checksum_received
 */
typedef struct CrcField_s
{
    uint8_t msb;       ///< CRC most significant byte.
    uint8_t lsb;       ///< CRC least significant byte.
    uint8_t stopbyte;  ///< @ref STOP_BYTE delimiter.
} CrcField;

/**
 * @struct Payload_s
 * @brief Lightweight pointer + length pair for passing payload slices.
 */
typedef struct Payload_s
{
    uint8_t *data;  ///< Pointer to payload bytes.
    uint8_t len;    ///< Number of valid bytes.
} Payload;

/** @name Frame Construction & Transmission
 * @{
 */

/**
 * @brief Build a complete UART frame and transmit it.
 *
 * @details Populates header, copies payload, appends CRC via
 *          @ref assign_crc, and calls @ref send_bytes.
 *
 * @param[in] command_id  Command byte for @ref CMD_ID.
 * @param[in] data        Payload buffer.
 * @param[in] size        Payload length.
 * @param[in] stream_id   Destination channel.
 *
 * @see track_command, assign_crc
 */
void construct_and_send(uint8_t command_id, uint8_t *data, uint8_t size,
                        StreamSelect stream_id);

/**
 * @brief Compute CRC-16 and append it (+ stop byte) to a frame buffer.
 *
 * @param[in,out] array  Frame buffer with command, length, and payload
 *                       already populated.
 *
 * @pre Command, payload, and payload length must be set before calling.
 *
 * @see calculate_crc
 */
void assign_crc(uint8_t *array);

/**
 * @brief Snapshot a sent frame's header + CRC for ACK matching.
 *
 * @param[in]     array    Sent frame buffer.
 * @param[in,out] tracker  @ref Dataframe_s::tx_tracker to update.
 *
 * @see is_ack
 */
void track_command(uint8_t *array, uint8_t *tracker);
/** @} */

/** @name Frame Validation
 * @{
 */

/**
 * @brief Validate a received frame's CRC and respond ACK or NACK.
 *
 * @param[in] module  @ref Dataframe_s holding the received frame.
 *
 * @return @c true if CRC matched (ACK sent), @c false (NACK sent).
 *
 * @see calculate_crc, extract_checksum_received
 */
bool evaluate_dataframe(Dataframe *module);

/**
 * @brief Check if a received frame matches a known NACK pattern.
 *
 * @param[in] data_buffer  Raw received frame buffer.
 *
 * @return @c true if the frame is a recognized NACK type.
 *
 * @see is_ack
 */
bool is_nack(uint8_t *data_buffer);

/**
 * @brief Check if a received frame is an ACK for a previously sent command.
 *
 * @param[in] buffer         Raw received frame buffer.
 * @param[in] command_track  Tracked Tx header from @ref track_command.
 *
 * @return @c true if command ID and CRC both match the tracked frame.
 *
 * @see track_command, is_nack
 */
bool is_ack(uint8_t *buffer, uint8_t *command_track);
/** @} */

/** @name Payload Extraction
 * @{
 */

/** @brief Copy payload bytes out of a received frame. */
void get_payload(uint8_t *payload, uint8_t size, Dataframe *module);

/** @brief Extract a single @c uint32_t from a raw byte buffer. */
void get_uint32_from_frame(uint32_t &value, uint8_t *buffer);

/**
 * @brief Extract two @c uint32_t values from a frame payload (address pair).
 *
 * @pre @ref DATA_LEN must equal @ref TWO_ADDRESS_SIZE (8).
 */
void get_uint32_pair_payload(uint32_t &first, uint32_t &second, uint8_t *buffer);
/** @} */

#endif /* TASK_HELPER_H */
