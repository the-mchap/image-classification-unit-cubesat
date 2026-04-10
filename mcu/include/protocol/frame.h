/**
 * @file frame.h
 *
 * @brief UART frame wire format: byte offsets, magic constants, and
 *        buffer size constraints.
 *
 * @details Every UART frame on this system (OBC, RPi, DBG) follows the
 *          same wire format:
 *
 * @code
 *  Byte:  [0]       [1]      [2]       [3..N+2]  [N+3]    [N+4]    [N+5]
 *         UNIT_ID   CMD_ID   DATA_LEN  payload   CRC_MSB  CRC_LSB  STOP
 * @endcode
 *
 * Where @c N = @ref DATA_LEN value (0 .. @ref PAYLOAD_MAX_SIZE).
 *
 * @see uart.c for the ISRs and Tx/Rx functions that build
 *      and consume these frames.
 * @see parser.c for the command routing logic that dispatches on
 *      @ref CMD_ID.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup globals_protocol
 */
#ifndef FRAME_H
#define FRAME_H

/**
 * @defgroup globals_protocol UART Protocol Types
 * @brief Frame layout, constants, and the per-channel state structure.
 * @{
 */

/**
 * @enum FrameProtocolIndex_e
 * @brief Byte offsets into a UART protocol frame header.
 *
 * @see @ref FrameConst_e for the expected values at some of these offsets.
 */
typedef enum FrameProtocolIndex_e
{
    UNIT_ID,         ///< Sender/unit identifier byte.
    CMD_ID,          ///< Command identifier byte.
    DATA_LEN,        ///< Payload length byte.
    CRC_MSB_OFFSET,  ///< CRC most significant byte offset.
    CRC_LSB_OFFSET   ///< CRC least significant byte offset.
} FrameProtocolIndex;

/**
 * @enum FrameConst_e
 * @brief Magic constants used in the UART protocol framing.
 *
 * @see @ref obc_command.h and @ref rpi_command.h for the command IDs
 *      that ride alongside these identifiers.
 */
typedef enum FrameConst_e
{
    OBC_ID = 0x1C,    ///< OBC sender identifier.
    RPI_ID = 0x3E,    ///< RPi sender identifier.
    STOP_BYTE = 0x0A  ///< End-of-frame delimiter.
} FrameConst;

/**
 * @enum FrameSize_e
 * @brief Frame and buffer size constraints.
 */
typedef enum FrameSize_e
{
    BUFFER_SIZE = 45,      ///< Total Rx buffer length.
    FRAME_MIN_SIZE = 6,    /**< Minimum valid frame size
                                (header + CRC + stop). */
    PAYLOAD_MAX_SIZE = 39  ///< Maximum payload bytes per frame.
} FrameSize;

/** @} */ /* globals_protocol */

#endif /* FRAME_H */
