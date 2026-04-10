/**
 * @file task_helper.c
 *
 * @brief Frame construction, CRC assignment, ACK/NACK detection, and
 *        payload extraction — the shared toolbox for all task handlers.
 *
 * @details Every incoming and outgoing UART frame passes through at
 *          least one function in this file. The overlay structs
 *          @ref UartPacket_s and @ref CrcField_s avoid magic offsets
 *          by mapping directly onto the raw byte buffers.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-12
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/protocol/frame.h"
#include "../../include/flash/flash_types.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/flash.h"
#include "../../include/app/task_helper.h"
#include "../../include/protocol/conversions.h"
#include "../../include/protocol/crc.h"
#include "../../include/app/rpi_command.h"

/* ================= Internal configuration (do not export) ================= */

/**
 * @def FRAME_OVERHEAD
 * @brief Total non-payload bytes in a frame (header + CRC + stop byte).
 */
#define FRAME_OVERHEAD 6

#ifdef DEBUG_MODE
char obc_label[] = "OBC";  ///< Debug log label for OBC channel.
char rpi_label[] = "RPi";  ///< Debug log label for RPi channel.
#endif                     /* DEBUG_MODE */

/* ======================= Internal configuration end ======================= */

/* ---------------------- Payload Extraction ---------------------- */

/**
 * @brief Copy payload bytes out of a received frame.
 *
 * @param[out]    payload  Destination buffer.
 * @param[in]     size     Number of bytes to copy.
 * @param[in,out] module   Source @ref Dataframe_s.
 *
 * @see construct_and_send
 */
void get_payload(uint8_t *payload, uint8_t size, Dataframe *module)
{
    volatile UartPacket *rx_packet = (volatile UartPacket *)module->buffer;
    memcpy(payload, (uint8_t *)rx_packet->payload, size);
}

/**
 * @brief Extract a single @c uint32_t from a raw byte buffer.
 *
 * @param[out] value   Destination variable (CCS reference).
 * @param[in]  buffer  Source buffer (>= 4 bytes).
 *
 * @see bytes_to_uint32
 */
void get_uint32_from_frame(uint32_t &value, uint8_t *buffer)
{
    if (buffer == NULL)
    {
        LOG("\n\r <?> Buffer is NULL.");
        return;
    }

    value = bytes_to_uint32(buffer);
}

/**
 * @brief Extract two @c uint32_t values from a frame payload
 *        (typically an address + size pair).
 *
 * @param[out] value_1  First 4 bytes of payload.
 * @param[out] value_2  Second 4 bytes of payload.
 * @param[in]  buffer   Raw frame buffer.
 *
 * @pre @ref DATA_LEN must equal @ref TWO_ADDRESS_SIZE (8).
 *
 * @see bytes_to_uint32
 */
void get_uint32_pair_payload(uint32_t &value_1, uint32_t &value_2, uint8_t *buffer)
{
    volatile UartPacket *rx_packet = (volatile UartPacket *)buffer;

    if (rx_packet->payload_length != TWO_ADDRESS_SIZE)
    {
        LOG("\n\r ->Addresses/size from frame are incorrect.");
        return;
    }

    value_1 = bytes_to_uint32((uint8_t *)rx_packet->payload);
    value_2 = bytes_to_uint32((uint8_t *)rx_packet->payload + ADDRESS_SIZE);
}

/* ---------------------- Frame Construction ---------------------- */

/**
 * @brief Build a complete UART frame and transmit it.
 *
 * @details Populates the @ref UartPacket_s header, copies the payload,
 *          appends CRC via @ref assign_crc, transmits via
 *          @ref send_bytes, and snapshots the frame for ACK matching
 *          via @ref track_command.
 *
 * @param[in] command_id  Command byte for @ref CMD_ID.
 * @param[in] data        Payload buffer (may be NULL if @a size is 0).
 * @param[in] size        Payload length.
 * @param[in] stream_id   Destination channel.
 *
 * @see assign_crc, track_command, send_bytes
 */
void construct_and_send(uint8_t command_id, uint8_t *data, uint8_t size,
                        StreamSelect stream_id)
{
    uint8_t tx_buffer[BUFFER_SIZE] = {0};
    UartPacket *tx_ptr = (UartPacket *)tx_buffer;

    tx_ptr->sender_id = (stream_id == SELECT_DBG ? OBC_ID : RPI_ID);
    tx_ptr->command_id = command_id;
    tx_ptr->payload_length = size;
    memcpy(tx_ptr->payload, data, size);
    assign_crc(tx_buffer);

    send_bytes(tx_buffer, FRAME_OVERHEAD + tx_ptr->payload_length, stream_id);

#ifdef DEBUG_MODE
    LOG("\n\r->Sent to %s: ", stream_id == SELECT_RPI ? rpi_label : obc_label);
    send_bytes(tx_buffer, FRAME_OVERHEAD + tx_ptr->payload_length, SELECT_DBG);
    track_command(tx_buffer, stream_id == SELECT_DBG ? dbg.tx_tracker : rpi.tx_tracker);
#endif /* DEBUG_MODE */

    track_command(tx_buffer, stream_id == SELECT_OBC ? obc.tx_tracker : rpi.tx_tracker);
}

/**
 * @brief Snapshot a sent frame's header + CRC for ACK matching.
 *
 * @details Copies sender ID, command ID, and CRC into the tracker
 *          buffer (a minimal frame with zero-length payload). The
 *          receiver's @ref is_ack compares against this snapshot.
 *
 * @param[in]     array    Sent frame buffer.
 * @param[in,out] tracker  @ref Dataframe_s::tx_tracker to update.
 *
 * @see is_ack, construct_and_send
 */
void track_command(uint8_t *array, uint8_t *tracker)
{
    UartPacket *tx_packet = (UartPacket *)array;
    CrcField *crc_tx = (CrcField *)&tx_packet->payload[tx_packet->payload_length];
    UartPacket *track = (UartPacket *)tracker;
    CrcField *crc_trk = (CrcField *)&track->payload[track->payload_length];

    track->sender_id = tx_packet->sender_id;
    track->command_id = tx_packet->command_id;
    track->payload_length = 0;
    crc_trk->msb = crc_tx->msb;
    crc_trk->lsb = crc_tx->lsb;
    crc_trk->stopbyte = STOP_BYTE;
}

/**
 * @brief Compute CRC-16 and append it (+ stop byte) to a frame buffer.
 *
 * @details Uses a local union to split the 16-bit CRC into MSB/LSB
 *          and writes them into the @ref CrcField_s overlay.
 *
 * @param[in,out] array  Frame buffer with command, length, and payload
 *                       already populated.
 *
 * @pre Command, payload, and payload length must be set before calling.
 *
 * @see calculate_crc
 */
void assign_crc(uint8_t *array)
{
    union Crc_u
    {
        uint16_t value;
        uint8_t bytes[2];
    } crc;

    UartPacket *packet = (UartPacket *)array;
    CrcField *crc_ptr = (CrcField *)&packet->payload[packet->payload_length];

    crc.value = calculate_crc(array);
    crc_ptr->msb = *(crc.bytes + 1);  // crc[1];
    crc_ptr->lsb = *(crc.bytes);      // crc[0];
    crc_ptr->stopbyte = STOP_BYTE;
}

/* ---------------------- Frame Validation ---------------------- */

/**
 * @brief Check if a received frame matches a known NACK pattern.
 *
 * @details Two NACK signatures are recognized:
 *          - **CRC NACK:** command = 0xFF, length = 0, CRC = 0xFFFF.
 *          - **Frame NACK:** command = 0x00, length = 0, CRC = 0x0000.
 *
 * @param[in] buffer  Raw received frame buffer.
 *
 * @return @c true if the frame matches either NACK pattern.
 *
 * @see is_ack, send_nack
 */
bool is_nack(uint8_t *buffer)
{
    volatile UartPacket *rx_pack = (volatile UartPacket *)buffer;
    volatile CrcField *rx_crc =
        (volatile CrcField *)&rx_pack->payload[rx_pack->payload_length];

    if (rx_pack->command_id == 0xFF && !rx_pack->payload_length && rx_crc->msb == 0xFF &&
        rx_crc->lsb == 0xFF)
    { /* NACK crc pattern = 0xFF, 0x00, 0xFF, 0xFF. */
        LOG("\n\r <!> NACK (crc) received from %s <!>\n",
            rx_pack->sender_id == OBC_ID ? obc_label : rpi_label);
        return true;
    }

    if (!rx_pack->command_id && !rx_pack->payload_length && !rx_crc->msb && !rx_crc->lsb)
    { /* NACK frame pattern = 0x00, 0x00, 0x00, 0x00. */
        LOG("\n\r <!> NACK (frame) received from %s <!>\n\n",
            rx_pack->sender_id == OBC_ID ? obc_label : rpi_label);
        return true;
    }

    return false;
}

/**
 * @brief Check if a received frame is an ACK for a previously sent
 *        command.
 *
 * @details Compares the received command ID against the tracked one,
 *          then does a 2-byte @c memcmp on the CRC fields. Both must
 *          match for a positive ACK.
 *
 * @param[in] buffer    Raw received frame buffer.
 * @param[in] tx_track  Tracked Tx snapshot from @ref track_command.
 *
 * @return @c true if command ID and CRC both match the tracked frame.
 *
 * @see track_command, is_nack
 */
bool is_ack(uint8_t *buffer, uint8_t *tx_track)
{
    volatile UartPacket *rx = (volatile UartPacket *)buffer;
    UartPacket *track = (UartPacket *)tx_track;

    if (rx->command_id != track->command_id)
    {
        return false;
    }

    volatile CrcField *crc_rx = (volatile CrcField *)&rx->payload[rx->payload_length];
    CrcField *crc_track = (CrcField *)&track->payload[track->payload_length];

    if (!memcmp((uint8_t *)crc_rx, (uint8_t *)crc_track, 2))
    { /* crc_rx == crc_track. */
        LOG("\n\r <!> ACK received from %s <!>\n\n",
            rx->sender_id == OBC_ID ? obc_label : rpi_label);
        return true;
    }

    return false;
}

/**
 * @brief Validate a received frame's CRC and respond ACK or NACK.
 *
 * @details Computes the CRC over the frame and compares it against
 *          the embedded checksum. Sends an ACK on match, a
 *          @ref UART_ERROR_INVALID_CHECKSUM NACK on mismatch.
 *
 * @param[in] module  @ref Dataframe_s holding the received frame.
 *
 * @return @c true if CRC matched (ACK sent), @c false otherwise
 *         (NACK sent).
 *
 * @see calculate_crc, extract_checksum_received, send_ack, send_nack
 */
bool evaluate_dataframe(Dataframe *module)
{
    if (calculate_crc(module->buffer) == extract_checksum_received(module->buffer))
    {
#ifndef DEBUG_MODE
        send_ack(module->buffer, (module == &obc ? SELECT_OBC : SELECT_RPI));
#else
        send_ack(module->buffer, (module == &dbg ? SELECT_DBG : SELECT_RPI));
        LOG("\n\r <!> ACK sent to %s <!>\n\n",
            module->buffer[UNIT_ID] == OBC_ID ? obc_label : rpi_label);
#endif /* DEBUG_MODE */
        return true;
    }

#ifndef DEBUG_MODE
    send_nack(UART_ERROR_INVALID_CHECKSUM, (module == &obc ? SELECT_OBC : SELECT_RPI));
#else
    send_nack(UART_ERROR_INVALID_CHECKSUM, (module == &dbg ? SELECT_DBG : SELECT_RPI));
    LOG("\n\r -> NACK (crc) sent to %s <!>\n\n",
        module->buffer[UNIT_ID] == OBC_ID ? obc_label : rpi_label);
#endif /* DEBUG_MODE */

    return false;
}
