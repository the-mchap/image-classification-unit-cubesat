/**
 * @file uart.c
 *
 * @brief UART driver: Tx helpers, Rx ISRs, frame watchdog, and timeout
 *        recovery for all three communication channels.
 *
 * @details Three hardware UARTs are driven here:
 *
 * | Channel | UART | ISR           | Stream macro   | Dataframe |
 * |---------|------|---------------|----------------|-----------|
 * | OBC     | 2    | `INT_RDA2`    | @c OBC_STREAM  | @ref obc  |
 * | RPi     | 3    | `INT_RDA3`    | @c RPI_STREAM  | @ref rpi  |
 * | DBG     | 1    | `INT_RDA`     | @c DBG_STREAM  | @ref dbg  |
 *
 * **Rx path:** each `INT_RDAx` ISR accumulates bytes into its
 * @ref Dataframe_s buffer and raises `flag` when
 * @ref IS_FRAME_COMPLETE evaluates true.
 *
 * **Timeout path:** @c INT_TIMER0 (the "framedog") fires periodically.
 * If a channel has bytes but no complete frame, it sets a
 * @c timeout_pending flag. The main loop calls
 * @ref process_rx_timeout_events to drain those flags outside ISR
 * context — double-checked with the channel's RDA interrupt disabled
 * to avoid racing.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-04
 *
 * @ingroup driver_uart
 */
#include "globals.h"

/* pin_map.h and frame.h already pulled in via globals.h */
#include "../../include/drivers/uart.h"
#include "../../include/app/task_helper.h"

/* ================= Internal configuration (do not export) ================= */

/**
 * @def FRAME_STRUCT_OFFSET
 * @brief Byte offset from the start of the buffer to the first
 *        payload-dependent field (header size minus one).
 *
 * @see IS_FRAME_COMPLETE
 */
#define FRAME_STRUCT_OFFSET 5

/**
 * @def INVALID_FIELD
 * @brief Sentinel stuffed into NACK frames to mark "no valid data."
 */
#define INVALID_FIELD 0xFF

/** @brief OBC channel timeout flag. Set by ISR, consumed by main loop. */
static volatile bool rpi_timeout_pending = false;

/** @brief RPi channel timeout flag. Set by ISR, consumed by main loop. */
static volatile bool obc_timeout_pending = false;

/** @brief OBC channel — ISR-written, main-loop-consumed. */
Dataframe rpi = {{0}, 0, false, {0}};

/** @brief RPi channel — ISR-written, main-loop-consumed. */
Dataframe obc = {{0}, 0, false, {0}};

#ifdef DEBUG_MODE
/** @brief DBG channel timeout flag. @ref DEBUG_MODE only. */
static volatile bool dbg_timeout_pending = false;

/** @brief Debug channel — @ref DEBUG_MODE only. */
Dataframe dbg = {{0}, 0, false, {0}};
#endif /* DEBUG_MODE */

/**
 * @def IS_FRAME_COMPLETE(buffer, index, header_id)
 * @brief Evaluate whether a received UART frame is structurally complete.
 *
 * @details Checks three conditions simultaneously:
 *          1. Enough bytes have arrived (@c index >= @ref FRAME_MIN_SIZE).
 *          2. The sender ID matches the expected @a header_id.
 *          3. The stop byte sits at the expected offset derived from
 *             @ref DATA_LEN.
 *
 * @param buffer     Raw receive buffer.
 * @param index      Current write index into @a buffer.
 * @param header_id  Expected @ref UNIT_ID value for this channel.
 *
 * @see framedog_isr for the timeout fallback when this never fires.
 */
#define IS_FRAME_COMPLETE(buffer, index, header_id)                                      \
    (index >= FRAME_MIN_SIZE && buffer[UNIT_ID] == header_id &&                          \
     buffer[FRAME_STRUCT_OFFSET + *(buffer + DATA_LEN)] == STOP_BYTE)

/* ======================= Internal configuration end ======================= */

/* --------------------- Tx Public --------------------- */

/**
 * @brief Transmit raw bytes to the selected UART channel.
 *
 * @details Routes to @c fputc on the hardware stream, or to @c LOG
 *          hex-dump when the target is the debug terminal.
 *
 * @param[in] dataframe  Source buffer.
 * @param[in] size       Number of bytes to send.
 * @param[in] stream     Destination channel.
 *
 * @see send_ack, send_nack, construct_and_send
 */
void send_bytes(uint8_t *dataframe, uint16_t size, StreamSelect stream)
{
    if (size == 0 || dataframe == NULL)
    {
        LOG("\n\r <?> Size or frame was NULL. Could not send bytes.");
        return;
    }

#ifdef DEBUG_MODE
    if (stream == SELECT_DBG)
    {
        do
        {
            LOG("%02X", *dataframe++);
        } while (--size);

        return;
    }
#endif /* DEBUG_MODE */

    if (stream == SELECT_OBC)
    {
        do
        {
            fputc(*dataframe++, OBC_STREAM);
        } while (--size);

        return;
    }

    if (stream == SELECT_RPI)
    {
        do
        {
            fputc(*dataframe++, RPI_STREAM);
        } while (--size);

        return;
    }
}

/**
 * @brief Build and send an ACK frame echoing the received command + CRC.
 *
 * @details Constructs a minimal frame (@ref FRAME_MIN_SIZE) that mirrors
 *          the command ID and CRC of the received frame — the sender
 *          matches these to confirm delivery.
 *
 * @param[in] buffer  Raw received frame to acknowledge.
 * @param[in] stream  Channel to send the ACK on.
 *
 * @see is_ack, track_command
 */
void send_ack(uint8_t *buffer, StreamSelect stream)
{
    uint8_t ack_buffer[FRAME_MIN_SIZE] = {0};
    UartPacket *rx_pack = (UartPacket *)buffer;
    CrcField *rx_crc = (CrcField *)&rx_pack->payload[rx_pack->payload_length];
    UartPacket *tx_pack = (UartPacket *)ack_buffer;
    CrcField *tx_crc = (CrcField *)&tx_pack->payload[tx_pack->payload_length];

    tx_pack->sender_id = (stream == SELECT_RPI) ? RPI_ID : OBC_ID;
    tx_pack->command_id = rx_pack->command_id;  // ID
    tx_crc->msb = rx_crc->msb;                  // CRC_MSB
    tx_crc->lsb = rx_crc->lsb;                  // CRC_LSB
    tx_crc->stopbyte = STOP_BYTE;

    send_bytes(ack_buffer, FRAME_MIN_SIZE, stream);
}

/**
 * @brief Build and send a NACK frame for the given error condition.
 *
 * @details The NACK pattern varies by error type:
 *          - @ref UART_ERROR_TIMEOUT / @ref UART_ERROR_BAD_FRAME:
 *            all-zeros (header already zeroed by init).
 *          - @ref UART_ERROR_INVALID_CHECKSUM: command and CRC fields
 *            stuffed with @ref INVALID_FIELD (0xFF).
 *          - @ref UART_ERROR_NONE or out-of-range: silently ignored.
 *
 * @param[in] error   Error code that triggered the NACK.
 * @param[in] stream  Channel to send the NACK on.
 *
 * @see is_nack, process_rx_timeout_events
 */
void send_nack(UartError error, StreamSelect stream)
{
    if (error >= UART_ERROR_COUNT)
    {
        return;
    }

    uint8_t nack_buffer[FRAME_MIN_SIZE] = {0};
    UartPacket *tx_pack = (UartPacket *)nack_buffer;
    CrcField *tx_crc = (CrcField *)&tx_pack->payload[tx_pack->payload_length];

    tx_pack->sender_id = (stream == SELECT_RPI) ? RPI_ID : OBC_ID;
    tx_crc->stopbyte = STOP_BYTE;

    switch (error)
    {
    case UART_ERROR_TIMEOUT:
    case UART_ERROR_BAD_FRAME:  // NACK frame already initialized for these cases.
        break;

    case UART_ERROR_INVALID_CHECKSUM:
        tx_pack->command_id = INVALID_FIELD;
        tx_crc->msb = INVALID_FIELD;
        tx_crc->lsb = INVALID_FIELD;
        break;

    default:
        return;  // UART_ERROR_NONE or undefined — do not send.
    }

    send_bytes(nack_buffer, FRAME_MIN_SIZE, stream);
}

/* --------------------- Rx ISRs & Timeout --------------------- */

/**
 * @brief Timer0 "framedog" ISR — detect stalled frame receptions.
 *
 * @details Fires periodically (~1 s). For each channel, if bytes have
 *          arrived (@c index > 0) but no complete frame was detected
 *          (@c flag still false), the corresponding @c timeout_pending
 *          flag is raised for main-loop consumption.
 *
 * @warning If @c index somehow reaches @ref BUFFER_SIZE without a
 *          valid frame, the circular wrap in the RDA ISRs resets it
 *          to 0 — so the framedog won't see stale bytes in that edge
 *          case. Analyze this path if the frame protocol ever grows.
 *
 * @see process_rx_timeout_events
 */
#int_timer0
void framedog_isr(void)
{
    if (obc.index && !obc.flag)
    {
        obc_timeout_pending = true;
    }

    if (rpi.index && !rpi.flag)
    {
        rpi_timeout_pending = true;
    }

#ifdef DEBUG_MODE
    if (dbg.index && !dbg.flag)
    {
        dbg_timeout_pending = true;
    }
    output_toggle(LED_2);
#endif /* DEBUG_MODE */
}

/**
 * @brief Drain timeout flags set by @ref framedog_isr and send NACKs.
 *
 * @details For each channel with a pending timeout, this function:
 *          1. Disables the channel's RDA interrupt (avoid racing the ISR).
 *          2. Double-checks the flag (may have been cleared between check
 *             and disable).
 *          3. Resets the frame and marks a local "send NACK" flag.
 *          4. Re-enables the RDA interrupt.
 *          5. Sends the NACK outside the critical section.
 *
 * @pre Must be called from the main loop — not ISR-safe.
 *
 * @see framedog_isr, reset_frame, send_nack
 */
void process_rx_timeout_events(void)
{
    bool send_obc_nack = false;
    bool send_rpi_nack = false;

    if (obc_timeout_pending)
    {
        disable_interrupts(INT_RDA2);  // Avoid racing.

        if (obc_timeout_pending)
        {
            obc_timeout_pending = false;
            reset_frame(&obc);
            send_obc_nack = true;
        }

        enable_interrupts(INT_RDA2);
    }

    if (rpi_timeout_pending)
    {
        disable_interrupts(INT_RDA3);

        if (rpi_timeout_pending)
        {
            rpi_timeout_pending = false;
            reset_frame(&rpi);
            send_rpi_nack = true;
        }

        enable_interrupts(INT_RDA3);
    }

    if (send_obc_nack)
    {
        send_nack(UART_ERROR_TIMEOUT, SELECT_OBC);
    }

    if (send_rpi_nack)
    {
        send_nack(UART_ERROR_TIMEOUT, SELECT_RPI);
    }

#ifdef DEBUG_MODE
    bool send_dbg_nack = false;

    if (dbg_timeout_pending)
    {
        disable_interrupts(INT_RDA);

        if (dbg_timeout_pending)
        {
            dbg_timeout_pending = false;
            reset_frame(&dbg);
            send_dbg_nack = true;
        }

        enable_interrupts(INT_RDA);
    }

    if (send_dbg_nack)
    {
        send_nack(UART_ERROR_TIMEOUT, SELECT_DBG);
    }
#endif /* DEBUG_MODE */
}

#ifdef DEBUG_MODE
/**
 * @brief UART1 Rx ISR — Debug/OBC-simulator channel.
 *
 * @details Accumulates bytes from @ref DBG_STREAM into @ref dbg.
 *          Uses @ref OBC_ID as the expected header because the debug
 *          terminal emulates OBC framing.
 *
 * @see framedog_isr for timeout recovery when a frame never completes.
 */
#int_rda
void dbg_rx_isr(void)
{
    dbg.buffer[dbg.index++] = fgetc(DBG_STREAM);

    if (dbg.index >= BUFFER_SIZE)
    {
        dbg.index = 0;  // Behaves as a circular FIFO.
    }

    if (IS_FRAME_COMPLETE(dbg.buffer, dbg.index, OBC_ID))
    {
        set_timer0(0);  // Reset time (1s) to receive a frame.
        dbg.flag = true;
    }
}
#endif /* DEBUG_MODE */

/**
 * @brief UART2 Rx ISR — OBC channel.
 *
 * @details Accumulates bytes from @ref OBC_STREAM into @ref obc and
 *          raises @c obc.flag on a complete frame.
 *
 * @see framedog_isr, process_rx_timeout_events
 */
#int_rda2
void obc_rx_isr(void)
{
    obc.buffer[obc.index++] = fgetc(OBC_STREAM);

    if (obc.index >= BUFFER_SIZE)
    {
        obc.index = 0;  // Behaves as a circular FIFO.
    }

    if (IS_FRAME_COMPLETE(obc.buffer, obc.index, OBC_ID))
    {
        set_timer0(0);  // Reset time (1s) to receive a frame.
        obc.flag = true;
    }
}

/**
 * @brief UART3 Rx ISR — RPi channel.
 *
 * @details Accumulates bytes from @ref RPI_STREAM into @ref rpi and
 *          raises @c rpi.flag on a complete frame.
 *
 * @see framedog_isr, process_rx_timeout_events
 */
#int_rda3
void rpi_rx_isr(void)
{
    rpi.buffer[rpi.index++] = fgetc(RPI_STREAM);

    if (rpi.index >= BUFFER_SIZE)
    {
        rpi.index = 0;  // Behaves as a circular FIFO.
    }

    if (IS_FRAME_COMPLETE(rpi.buffer, rpi.index, RPI_ID))
    {
        set_timer0(0);  // Reset time (1s) to receive a frame.
        rpi.flag = true;
    }
}

/**
 * @brief Flush a channel's receive state (buffer, index, flag).
 *
 * @param[in,out] module  The @ref Dataframe_s instance to reset.
 *
 * @warning Does not flush @c tx_tracker — that persists for ACK
 *          matching across frames.
 *
 * @see process_rx_timeout_events, evaluate_dataframe
 */
void reset_frame(Dataframe *module)
{
#ifdef DEBUG_MODE
    char dbg_label[] = "DBG";
    char rpi_label[] = "RPI";

    LOG("\n\r Print %s frame before flushing: ", module == &dbg ? dbg_label : rpi_label);
    send_bytes((uint8_t *)module->buffer, module->index, SELECT_DBG);
#endif /* DEBUG_MODE */

    module->flag = false;
    memset((uint8_t *)module->buffer, 0, module->index);
    module->index = 0;
}
/* Then we played bones, and I'm yelling "Domino!"
 * Plus nobody I know got killed in South Central L.A.
 * Today was a good day
 *                                --Ice Cube, 1992 **/
