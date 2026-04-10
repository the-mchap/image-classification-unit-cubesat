/**
 * @file uart.h
 *
 * @brief UART driver for OBC, RPi, and Debug communication channels.
 *
 * @details Handles Tx/Rx, frame validation, ACK/NACK responses, and
 *          per-channel timeout recovery. Three hardware UARTs are
 *          managed (OBC on UART2, RPi on UART3, DBG on UART1 under
 *          @ref DEBUG_MODE), each backed by a @ref Dataframe_s instance.
 *
 *          ISR-to-foreground handshake:
 *
 * @msc
 * RDA_ISR, Timer0_ISR, MainLoop;
 * RDA_ISR    -> RDA_ISR    [label="byte -> buffer[index++]"];
 * RDA_ISR    -> RDA_ISR    [label="frame complete -> flag=true"];
 * Timer0_ISR -> Timer0_ISR [label="index && !flag -> timeout_pending"];
 * MainLoop   -> MainLoop   [label="timeout? -> reset_frame + NACK"];
 * MainLoop   -> MainLoop   [label="flag? -> parser dispatch"];
 * @endmsc
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-06
 */
#ifndef UART_INTERFACE_H
#define UART_INTERFACE_H

/**
 * @struct Dataframe_s
 * @brief Per-channel UART state: receive buffer, write index, and flags.
 *
 * One instance exists per active UART channel (@c obc, @c rpi, and
 * optionally @c dbg under @ref DEBUG_MODE).
 *
 * @invariant @c index is always < @ref BUFFER_SIZE; ISRs wrap it to 0
 *            when it reaches the limit.
 * @invariant @c flag is set to @c true **only** by an ISR after
 *            @ref IS_FRAME_COMPLETE evaluates true, and cleared
 *            exclusively by @ref reset_frame from main-loop context.
 *
 * @note Members marked @c volatile are touched from ISR context.
 *
 * @see uart.c for the ISR and @ref reset_frame definitions.
 */
typedef struct Dataframe_s
{
    volatile uint8_t buffer[BUFFER_SIZE];  ///< Raw Rx ring buffer.
    volatile uint8_t index;                ///< Current write position.
    volatile bool flag;                    ///< Frame-complete flag.
    uint8_t tx_tracker[FRAME_MIN_SIZE];    ///< Last Tx header for ACK matching.
} Dataframe;

/**
 * @defgroup driver_uart UART Driver
 * @brief UART ISRs, Tx/Rx, frame validation, and timeout recovery.
 *
 * @see @ref globals_protocol for @ref Dataframe_s and frame constants.
 * @see parser.c for command dispatch after a valid frame is received.
 * @{
 */

/**
 * @enum UartError_e
 * @brief Error codes returned via NACK frames.
 */
typedef enum UartError_e
{
    UART_ERROR_NONE,              ///< No error.
    UART_ERROR_TIMEOUT,           ///< Frame reception timed out.
    UART_ERROR_BAD_FRAME,         ///< Structural validation failed.
    UART_ERROR_INVALID_CHECKSUM,  ///< CRC mismatch.
    UART_ERROR_COUNT              ///< Sentinel -- total error codes.
} UartError;

/**
 * @enum StreamSelect_e
 * @brief Selects which UART channel to transmit on.
 *
 * @see send_bytes, send_ack, send_nack
 */
typedef enum StreamSelect_e
{
    SELECT_OBC,     ///< UART2 -- OBC link.
    SELECT_RPI,     ///< UART3 -- RPi link.
    SELECT_DBG,     ///< UART1 -- Debug terminal (active under @ref DEBUG_MODE).
    SELECT_INVALID  ///< Sentinel -- out-of-range guard.
} StreamSelect;

/** @name Transmit
 * @{
 */
void send_bytes(uint8_t *data, uint16_t size, StreamSelect stream);
void send_ack(uint8_t *buffer, StreamSelect stream);
void send_nack(UartError error, StreamSelect stream);
/** @} */

/** @name Receive & State
 * @{
 */

/**
 * @brief Process timeout flags set by the framedog ISR.
 *
 * @details Checks each channel's pending-timeout flag, resets the
 *          frame, and sends a @ref UART_ERROR_TIMEOUT NACK. Must be
 *          called from the main loop -- not ISR-safe.
 *
 * @see reset_frame
 */
void process_rx_timeout_events(void);

/**
 * @brief Flush a channel's receive state (buffer, index, flag).
 *
 * @param[in,out] module  The @ref Dataframe_s instance to reset.
 *
 * @warning Does not flush @c tx_tracker.
 */
void reset_frame(Dataframe *module);
/** @} */

/** @name Per-Channel State
 *
 * One @ref Dataframe_s per active UART channel. ISR-written,
 * main-loop-consumed.
 * @{
 */
extern Dataframe obc;  ///< OBC channel (UART2).
extern Dataframe rpi;  ///< RPi channel (UART3).

#ifdef DEBUG_MODE
extern Dataframe dbg;  ///< Debug channel (UART1). @ref DEBUG_MODE only.
#endif                 /* DEBUG_MODE */
/** @} */

/** @} */ /* driver_uart */

#endif /* UART_INTERFACE_H */
