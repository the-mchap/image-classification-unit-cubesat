/**
 * @file rpi_tasks.c
 *
 * @brief RPi-side task handlers: write lifecycle, boot forwarding,
 *        power-off sequencing, and report relay to downlink.
 *
 * @details Three functional areas live here:
 *
 *          - **Write lifecycle** — @ref check_flash_space ->
 *            @ref flash_in_use -> @ref update_index. The RPi negotiates
 *            free space, locks flash for exclusive access, writes its
 *            image, and hands back the final address + size so the MCU
 *            can update the index.
 *          - **Command forwarding** — @ref task_for_rpi relays an
 *            OBC-originated task to the RPi after boot; @ref
 *            report_to_downlink pushes the RPi's database report back
 *            toward ground control.
 *          - **Power management** — @ref poweroff_countdown starts a
 *            Timer1-based 25-second grace period before hard-cutting
 *            @ref RPI_EN, giving the SBC time to flush and halt.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/drivers/pin_map.h"
#include "../../include/flash/flash_types.h"
#include "../../include/app/rpi_tasks.h"
#include "../../include/app/rpi_command.h"
#include "../../include/app/obc_command.h"
#include "../../include/flash/space_finder.h"
#include "../../include/protocol/conversions.h"
#include "../../include/drivers/uart.h"
#include "../../include/app/task_helper.h"
#include "../../include/drivers/flash.h"
#include "../../include/flash/index_manager.h"

/* ────────────────────── Internal configuration (do not export) ────────────────────── */

/**
 * @def KILL_TARGET_TICK
 * @brief Number of Timer1 overflows before power is cut.
 * @details 150 × 100 ms = 15 seconds — enough for the RPi to flush
 *          its filesystem and reach `halt`.
 */
#define KILL_TARGET_TICK 110

/**
 * @brief Running tick count for the Timer1 power-off countdown.
 *
 * @details Incremented by @ref rpi_killer_check on every overflow. When it
 *          reaches @ref KILL_TARGET_TICK the RPi power rail is cut.
 * @invariant Only written by @ref rpi_killer_check and set by
 *            @ref kickoff_rpi_killer.
 *
 * @warning Up to 255 ticks. Change its type for a longer countdown.
 */
static volatile uint8_t rpi_killer_tick = 0;

/* ──────────────────────────── Internal configuration end ──────────────────────────── */

/* ──────────── RPi Write Lifecycle ──────────── */

/**
 * @brief Check for free space in LFM and reply ACC/DEC to the RPi.
 *
 * @details Triggered by @c RPI_WR_REQ. Extracts the 4-byte image size
 *          from the RPi frame payload, calls @ref get_available_address
 *          to locate a gap large enough, and replies:
 *          - **ACC_OP** + 4-byte start address on success (MUX held to
 *            RPi so it can begin writing).
 *          - **DEC_OP** if no contiguous space is available (MUX
 *            released).
 *
 * @see get_available_address, construct_and_send
 */
void check_flash_space(void)
{
    uint8_t image_size_bytes[] = {0x00, 0x00, 0x00, 0x00};

    get_payload(image_size_bytes, sizeof(image_size_bytes), &rpi);

    reset_frame(&rpi);

    LOG("\n\r->Photo size (bytes in HEX) Raspberry took:");
#ifdef DEBUG_MODE
    send_bytes(image_size_bytes, ADDRESS_SIZE, SELECT_DBG);
#endif /* DEBUG_MODE */

    uint32_t image_size = bytes_to_uint32(image_size_bytes);
    LOG("\n\r Photo size: %Lu Bytes \n", image_size);

    uint32_t address = get_available_address(image_size);

    if (address < DATA_1ST || address > DATA_END)
    {
        decline_flash();
        construct_and_send(DECLINE_ACTION, NULL, 0, SELECT_RPI);
        LOG("\n\r <?> Not enough space available in Flash for that photo.");
        return;
    }

    LOG("\n\r <!> Address available found: 0x%8LX\n", address);

    uint8_t address_bytes[ADDRESS_SIZE] = {0};
    uint32_to_bytes(address_bytes, address);
    claim_flash_for_rpi();
    construct_and_send(ACCEPT_ACTION, address_bytes, ADDRESS_SIZE, SELECT_RPI);
}

/**
 * @brief Acknowledge that the RPi is actively writing to flash.
 *
 * @details Triggered by @c TRANSFER_BEG. At this point
 *          @ref claim_flash_for_rpi has already been called in
 *          @ref check_flash_space, so the MUX and ownership flag
 *          are set. This handler just consumes the frame.
 *
 * @see check_flash_space, update_index
 */
void flash_in_use(void)
{
    reset_frame(&rpi);
}

/**
 * @brief Finalise the RPi write session: release flash and update the
 *        index.
 *
 * @details Triggered by @c RPI_WR_END. Extracts an 8-byte payload
 *          consisting of the final write address (4 bytes) and total
 *          size (4 bytes). The start address is derived as
 *          `final_address − size`, and @ref update_index_after_write
 *          records the new entry.
 *
 * @see update_index_after_write
 */
void update_index(void)
{
    uint8_t addr_and_size[TWO_ADDRESS_SIZE] = {0};

    get_payload(addr_and_size, TWO_ADDRESS_SIZE, &rpi);

    reset_frame(&rpi);

    uint32_t final_address = bytes_to_uint32(addr_and_size);
    uint32_t size = bytes_to_uint32(addr_and_size + ADDRESS_SIZE);

    LOG("\n\r->Final address written to: 0x%8LX\n\r-> Size: %Lu bytes.\n", final_address,
        size);

    release_flash();
    LOG("\n\r -> Flash released.");

    if (!update_index_after_write(final_address - size, size))
    {
        LOG("\n\r <?> Couldn't update entry and status after write.");
        return;
    }
}

/* ──────────── RPi Command Forwarding ──────────── */

/**
 * @brief Forward an OBC-originated task to the RPi after boot.
 *
 * @details Triggered by @c RPI_BOOT. When the RPi signals readiness,
 *          the MCU should relay the pending task. Possible tasks:
 *          1. Capture + classify + store photos.
 *          2. Write a specific image (identified by metadata/ID).
 *          3. Generate and send a database report.
 *
 * @pre The task must have been previously received from OBC — the RPi
 *      never initiates these.
 *
 * @warning Stub: task relay logic is pending design (variable-length
 *          payload depends on which task type the OBC requested).
 */
void task_for_rpi(void)
{
    /* This will need some design first before coding. */
    /* How long is going to be the array to send? Depends on task.*/
    /*      1. To take photos, 6 (minimal).
     *      2. To request a particular photo (metadata ID, or something), UP TO 9 bytes!
     *      3. To request a report, 6 (minimal). */

    // uint8_t rpi_task[] = {RPI_ID, 0x00, 0x00, 0x00, 0x00, STOP_BYTE};
    // rpi_task[COMMAND] = dbg.buffer[COMMAND];

    reset_frame(&rpi);
}

/**
 * @brief Forward the RPi database report to ground control via OBC.
 *
 * @details Triggered by @c RPI_REPORT. Extracts a 6-byte report
 *          payload from the RPi frame, wraps it in a @ref REPORT_DL
 *          frame, and routes it toward the OBC for downlink.
 *
 * @see construct_and_send
 */
void report_to_downlink(void)
{
    uint8_t report_bytes[] = {0, 0, 0, 0, 0, 0};
    uint8_t report_size = sizeof(report_bytes);

    get_payload(report_bytes, report_size, &rpi);

    reset_frame(&rpi);

    construct_and_send(DL_REPO, report_bytes, report_size, SELECT_OBC);

#ifdef DEBUG_MODE
    LOG("\n\r -> Report:");
    send_bytes(report_bytes, report_size, SELECT_DBG);
    LOG("\n\r -> With frame protocol format:");
    construct_and_send(DL_REPO, report_bytes, report_size, SELECT_DBG);
#endif /* DEBUG_MODE */
}

/* ───────────────────────────────── Power Management ───────────────────────────────── */

/**
 * @brief Start the ~14.4-second power-off countdown for the RPi.
 *
 * @details Triggered by @c RPI_PWROFF — the RPi acknowledging that it
 *          has begun its shutdown sequence. Resets the tick counter,
 *          preloads Timer1, and enables the Timer1 interrupt. After
 *          110 overflows (~14.4 s) @ref rpi_killer_check hard-cuts @ref RPI_EN.
 *
 * @see rpi_killer_check
 */
void kickoff_rpi_killer(void)
{
    reset_frame(&rpi);

    rpi_killer_tick = 0;

    set_timer1(0);  // Whole 16-bit TMR1 register for every tick.
    enable_interrupts(INT_TIMER1);
}

/* ─────────────────────────────────────── ISR ─────────────────────────────────────── */

/**
 * @brief Timer1 Interruption Service Routine: tick the power-off countdown 🔥.
 *
 * @details Fires every ~131.1 ms. Increments @ref rpi_killer_tick
 *          until it reaches @ref KILL_TARGET_TICK, (110 ticks = ~14.4 seconds), then:
 *          1. ISR disables itself.
 *          2. Pulls @ref RPI_EN low — hard power-off.
 *
 * @todo A post-shutdown flag could be set here to later inform OBC that the whole ICU
 *       power supply can be cut. Do not perform frame sendings inside ISRs
 */
#int_timer1
void rpi_killer_check(void)
{
    if (++rpi_killer_tick >= KILL_TARGET_TICK)
    { /* 14.4 seconds have elapsed. */
        // setup_timer_1(T1_DISABLED); // Better if this is gone.
        disable_interrupts(INT_TIMER1);
        output_low(RPI_EN);

        /* Here a flag can be set to later tell OBC to cut whole ICU power supply. */
        /* Do not perform frame sendings inside ISRs*/
    }

    set_timer1(0);
}
/* I don't know but it's been said
 * Your heart is stronger than your head
 *            --Anthony Kiedis, 2016 **/
