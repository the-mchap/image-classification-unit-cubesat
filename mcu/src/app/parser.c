/**
 * @file parser.c
 *
 * @brief Central command router: validates incoming frames and
 *        dispatches them to OBC, RPi, or DBG task handlers.
 *
 * @details Every channel's @ref Dataframe_s passes through
 *          @ref frame_parsing once its @c flag is set. The function
 *          checks for NACK, ACK, and CRC validity before identifying
 *          the source channel and delegating to one of three static
 *          switch-dispatchers:
 *
 *          - @ref obc_task_select — OBC command set.
 *          - @ref rpi_task_select — RPi command set.
 *          - @ref dbg_task_select — superset of OBC + DBG-only
 *            commands (@ref DEBUG_MODE only).
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-08
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/protocol/frame.h"
#include "../../include/drivers/uart.h"
#include "../../include/app/parser.h"
#include "../../include/app/obc_command.h"
#include "../../include/app/rpi_command.h"
#include "../../include/drivers/setup.h"
#include "../../include/app/dbg_tasks.h"
#include "../../include/app/obc_tasks.h"
#include "../../include/app/rpi_tasks.h"
#include "../../include/app/task_helper.h"

/* ────────────────────── Internal configuration (do not export) ────────────────────── */

/** @brief Dispatch an OBC frame to its task handler by @ref CMD_ID. */
#inline
static void obc_task_select(Dataframe *module);

/** @brief Dispatch an RPi frame to its task handler by @ref CMD_ID. */
#inline
static void rpi_task_select(void);

#ifdef DEBUG_MODE
/** @brief Dispatch a debug frame — OBC superset + debug-only commands. */
#inline
static void dbg_task_select(Dataframe *module);
#endif /* DEBUG_MODE */

/* ────────────────────────────────── Internal configuration end ────────────────────── */

/* ──────────── Public ──────────── */

/**
 * @brief Parse an incoming frame and route it to the correct handler.
 *
 * @details Evaluation order:
 *          1. NACK check — if the peer rejected our last command.
 *          2. ACK check — if the peer acknowledged our last command.
 *          3. CRC + structural validation via @ref evaluate_dataframe.
 *          4. Channel routing by pointer identity (@c &obc, @c &rpi,
 *             @c &dbg).
 *
 * @param[in] module  @ref Dataframe_s with a complete, flagged frame.
 *
 * @pre @ref IS_FRAME_COMPLETE must have evaluated true for this module.
 *
 * @see evaluate_dataframe, is_nack, is_ack
 */
void frame_parsing(Dataframe *module)
{
    if (is_nack(module->buffer))
    {
        /* IMPORTANT HERE: Do we re-send previous command to OBC or RPI? */
        reset_frame(module);
        return;
    }

    if (is_ack(module->buffer, module->tx_tracker))
    {
        /* IMPORTANT HERE: Watchout for poweroff ACKs from OBC and RPI? */
        reset_frame(module);
        return;
    }

    if (!evaluate_dataframe(module))
    {
        reset_frame(module);
        return;
    }

    if (module == &obc)
    {
        obc_task_select(module);
        return;
    }

    if (module == &rpi)
    {
        rpi_task_select();
        return;
    }

#ifdef DEBUG_MODE
    if (module == &dbg)
    {
        dbg_task_select(module);
    }

    ready_cue();
#endif /* DEBUG_MODE */
}

/* ──────────── Private ──────────── */

/**
 * @brief Dispatch an OBC frame to its task handler.
 *
 * @details Routes by @ref CMD_ID across the full @ref ObcRxCommand_e
 *          set. Sends a NACK on unrecognised commands.
 *
 * @param[in] module  OBC @ref Dataframe_s to distinguish production from debug.
 */
#inline
static void obc_task_select(Dataframe *module)
{
    switch (module->buffer[CMD_ID])
    {
    case PWR_RPI_ON:
        LOG("\n\r-- OBC command identified as: PWR_RPI_ON --");
        rpi_on(module);
        break;

    case PWR_RPI_OFF:
        LOG("\n\r-- OBC command identified as: PWR_RPI_OFF --");
        rpi_off(module);
        break;

    case REQ_RPI_OFF:
        LOG("\n\r-- OBC command identified as: REQ_RPI_OFF --");
        rpi_request_poweroff(module);
        break;

    case REQ_RPI_RESET:
        LOG("\n\r-- OBC command identified as: REQ_RPI_RESET --");
        rpi_request_reboot(module);
        break;

    case REQ_RPI_PHCAP:
        LOG("\n\r-- OBC command identified as: REQ_RPI_PHCAP --");
        rpi_request_capture(module);
        break;

    case REQ_RPI_HQIMG:
        LOG("\n\r-- OBC command identified as: REQ_RPI_HQIMG --");
        rpi_request_image(module, true);
        break;

    case REQ_RPI_LQIMG:
        LOG("\n\r-- OBC command identified as: REQ_RPI_LQIMG --");
        rpi_request_image(module, false);
        break;

    case REQ_RPI_REPO:
        LOG("\n\r -- OBC command identified as: REQ_RPI_REPO --");
        rpi_report_request(module);
        break;

    case WRI_OBC_PTR:
        LOG("\n\r -- OBC command identified as: WRI_OBC_PTR --");
        obc_write_pointer(module);
        break;

    case WRI_OBC_META:
        LOG("\n\r -- OBC command identified as: WRI_OBC_META --");
        obc_write_meta(module);
        break;

    case WRI_OBC_SRC:
        LOG("\n\r -- OBC command identified as: WRI_OBC_SRC --");
        obc_write_from_to(module);
        break;

    case DEL_OBC_RANGE:
        LOG("\n\r -- OBC command identified as: DEL_OBC_RANGE --");
        delete_range(module);
        break;

    case DEL_OBC_ADDR:
        LOG("\n\r -- OBC command identified as: DEL_OBC_ADDR --");
        delete_address(module);
        break;

    case DEL_OBC_META:
        LOG("\n\r -- OBC command identified as: DEL_OBC_META --");
        delete_metadata(module);
        break;

    default:  // The only way to hit this condition is to send a well-crc-framed with an
              // unknown CMD_ID. Pretty rare case in production if you ask me. Non-zero
              // probability, tho. That's why it sends a FRAME-NACK if shit happens for
              // real.
        send_nack(UART_ERROR_BAD_FRAME, SELECT_OBC);
        LOG("\n\r -- NACK sent to OBC (bad frame) --");
        break;
    }
}

/**
 * @brief Dispatch an RPi frame to its task handler.
 *
 * @details Routes by @ref CMD_ID across the @ref RpiRxCommand_e set.
 *          Sends a NACK on unrecognised commands.
 */
#inline
static void rpi_task_select(void)
{
    switch (rpi.buffer[CMD_ID])
    {
    case TRANSFER_REQ:
        LOG("\n\r-- TRANSFER_REQ received from Raspberry --");
        check_flash_space();
        break;

    case TRANSFER_BEG:
        LOG("\n\r-- TRANSFER_BEG received from Raspberry --");
        flash_in_use();
        break;

    case TRANSFER_END:
        LOG("\n\r-- TRANSFER_END received from Raspberry --");
        update_index();
        break;

    case BOOT_ASSERTION:
        LOG("\n\r-- BOOT_ASSERTION received from Raspberry --");
        task_for_rpi();
        break;

    case PWROFF_ASSERTION:
        LOG("\n\r -- PWROFF_ASSERTION received from Raspberry --");
        kickoff_rpi_killer();
        break;

    case DATABASE_REPORT:
        LOG("\n\r -- DATABASE_REPORT received from Raspberry --");
        report_to_downlink();
        break;

    default:  // The only way to hit this condition is to send a well-crc-framed with an
              // unknown CMD_ID. Pretty rare case in production, if you ask me. Non-zero
              // probability, tho. That's why it sends a FRAME-NACK if shit happens for
              // real.
        send_nack(UART_ERROR_BAD_FRAME, SELECT_RPI);
        LOG("\n\r -- NACK sent to RPI (bad frame) --");
        break;
    }
}

#ifdef DEBUG_MODE
/**
 * @brief Dispatch a debug frame: OBC subset + debug-only commands.
 *
 * @details The DBG command set is a superset of the OBC set: every
 *          OBC command can be issued from the terminal, plus
 *          debug-only operations (flash dump, manual erase, DMA test,
 *          metadata viewer, etc.).
 *
 * @param[in] module  DBG @ref Dataframe_s to distinguish production from debug.
 */
#inline
static void dbg_task_select(Dataframe *module)
{
    switch (dbg.buffer[CMD_ID])
    {
    case TEST_RPI_FRAME:
        LOG("\n\r -- DBG command identified as: TEST_RPI_FRAME --");
        rpi_communication_test();  // rpi_uartframe_handling_test();
        break;

    case DUMP_LOG_FROM:
        LOG("\n\r -- DBG command identified as: DUMP_LOG_FROM --");
        debug_read_from();  // manual_read_dump();
        break;

    case SHOW_YA_BONES:
        LOG("\n\r -- DBG command identified as: SHOW_YA_BONES --");
        debug_log_entry();  // log_indexes();
        break;

    case DUMP_LOG_JPEGS:
        LOG("\n\r -- DBG command identified as: DUMP_LOG_JPEGS --");
        debug_log_image_data();  // dump_all_images();
        break;

    case DUMMY_DUMP:
        LOG("\n\r -- DBG command identified as: DUMMY_DUMP --");
        debug_write();  // write_in_local();
        break;

    case IAMSPEED:
        LOG("\n\r -- DBG command identified as: IAMSPEED --");
        debug_write_dma();  // dma_transfer();
        break;

    case META_DUMP_ME:
        LOG("\n\r -- DBG command identified as: META_DUMP_ME --");
        debug_metadata();  // dump_metadata();
        break;

    case RUBBER_ON_DELETE:
        LOG("\n\r -- DBG command identified as: RUBBER_ON_DELETE --");
        debug_delete();  // safe_delete();
        break;

    case RAW_DELETE:
        LOG("\n\r -- DBG command identified as: RAW_DELETE --");
        erase_manual();  // nuke();
        break;

        /* -------------------- OBC subset -------------------- */

    case PWR_RPI_ON:
        LOG("\n\r-- DBG command identified as: RPI_PWR_ON --");
        rpi_on(module);
        break;

    case PWR_RPI_OFF:
        LOG("\n\r-- DBG command identified as: RPI_PWR_OFF --");
        rpi_off(module);
        break;

    case REQ_RPI_OFF:
        LOG("\n\r-- DBG command identified as: RPI_REQ_OFF --");
        rpi_request_poweroff(module);
        break;

    case REQ_RPI_RESET:
        LOG("\n\r-- DBG command identified as: RPI_REQ_SET --");
        rpi_request_reboot(module);
        break;

    case REQ_RPI_PHCAP:
        LOG("\n\r-- DBG command identified as: RPI_REQ_CAP --");
        rpi_request_capture(module);
        break;

    case REQ_RPI_HQIMG:
        LOG("\n\r-- DBG command identified as: RPI_REQ_IMG --");
        rpi_request_image(module, false);
        break;

    case REQ_RPI_LQIMG:
        LOG("\n\r-- DBG command identified as: RPI_REQ_IMG --");
        rpi_request_image(module, true);
        break;

    case REQ_RPI_REPO:
        LOG("\n\r -- DBG command identified as: RPI_REQ_REP --");
        rpi_report_request(module);
        break;

    case WRI_OBC_PTR:
        LOG("\n\r -- DBG command identified as: OBC_WR_PTR --");
        obc_write_pointer(module);
        break;

    case WRI_OBC_META:
        LOG("\n\r -- DBG command identified as: OBC_WR_META --");
        obc_write_meta(module);
        break;

    case WRI_OBC_SRC:
        LOG("\n\r -- DBG command identified as: OBC_WR_SRC --");
        obc_write_from_to(module);
        break;

    case DEL_OBC_RANGE:
        LOG("\n\r -- DBG command identified as: OBC_DEL_RANGE --");
        delete_range(module);
        break;

    case DEL_OBC_ADDR:
        LOG("\n\r -- DBG command identified as: OBC_DEL_ADDR --");
        delete_address(module);
        break;

    case DEL_OBC_META:
        LOG("\n\r -- DBG command identified as: OBC_DEL_META --");
        delete_metadata(module);
        break;

    default:  // The only way to hit this condition is to send a well-crc-framed with an
              // unknown CMD_ID. Pretty rare case in production if you ask me. Non-zero
              // probability, tho. That's why it sends a FRAME-NACK if shit happened.
        send_nack(UART_ERROR_BAD_FRAME, SELECT_DBG);
        LOG("\n\r -- What on earth did bro send tho. NACK sent to DBG (bad frame) --");
        break;
    }
}
#endif /* DEBUG_MODE */
