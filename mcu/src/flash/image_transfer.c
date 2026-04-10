/**
 * @file image_transfer.c
 *
 * @brief Double-buffered DMA pipeline: LFM → OBC flash image transfer.
 *
 * @details While one page is being DMA'd to OBC flash over SPI1, the
 *          next page is read from LFM over SPI2 into the standby buffer.
 *          Two ISRs drive the handshake:
 *
 *          - **SSP1 ISR** (in @ref flash.c) — fires when DMA completes
 *            a page write; deasserts !CE1 and sets a completion flag.
 *          - **Timer3 ISR** — periodically raises a WIP poll flag so
 *            the foreground loop can check OBC flash status without
 *            SPI re-entrancy.
 *
 *          The main loop (@ref pipeline_transfer) spins through four
 *          helpers each iteration: @ref handle_dma_completion,
 *          @ref handle_wip_poll_tick, @ref try_fill_buffer, and
 *          @ref try_fire_dma — advancing the state machine until
 *          @c DONE or @c ERROR.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-05
 *
 * @ingroup storage
 */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/flash/image_transfer.h"
#include "../../include/drivers/flash.h"

/* ─────────────────── Internal configuration (do not export) ─────────────────── */

/**
 * @def WIP_TIMER_VALUE
 * @brief Timer3 preload for the WIP polling interval.
 *
 * @details 65248 at 16 MHz baud rate, 65460 at 4 MHz.
 */
#define WIP_TIMER_VALUE 65248U

/**
 * @def TIMEOUT_LOOP
 * @brief Software watchdog iteration limit for the pipeline loop.
 *
 * @note Needs adjustment based on real-world transfer sizes.
 */
#define TIMEOUT_LOOP 0x00FFF000UL

/**
 * @enum PipeState_e
 * @brief Pipeline state machine states.
 *
 * @see image_transfer.h for the @c \@dot digraph and @c \@msc chart.
 */
typedef enum PipeState_e
{
    IDLE,           ///< Initial state before first DMA fire.
    DMA_ACTIVE,     ///< DMAEN=1 — SPI2 is BLOCKED.
    FLASH_WRITING,  ///< DMAEN=0, WIP=1 — SPI2 is FREE.
    READY,          ///< WIP=0, buffer filled — safe to fire next DMA.
    DONE,           ///< All chunks transferred.
    ERROR           ///< Watchdog timeout or invalid state.
} PipeState;

/**
 * @struct Pipeline_s
 * @brief Double-buffered pipeline context.
 *
 * @invariant Exactly one of @c buf[0] / @c buf[1] is being DMA'd
 *            at any time; the other is available for LFM reads.
 */
typedef struct Pipeline_s
{
    uint8_t buf[2][PAGE_SIZE];  ///< Double buffer: [0] and [1].
    uint16_t buf_len[2];        ///< Valid bytes currently loaded per buffer.
    uint8_t active;             ///< Which buffer is being DMA'd (0 or 1).

    uint32_t src_address;    ///< Current LFM read address.
    uint32_t dst_address;    ///< Current OBC flash write address.
    uint32_t total_bytes;    ///< Total bytes left to transfer.
    uint32_t loop_watchdog;  ///< Software loop watchdog counter.

    volatile PipeState state;     ///< Current pipeline state.
    volatile bool wip_cleared;    ///< Set by foreground poller when WIP=0.
    volatile bool wip_poll_tick;  ///< Set by Timer3 ISR each tick.
    volatile bool buf_ready;      ///< Set by main loop after LFM read.
} Pipeline;

/** @brief Singleton pipeline context. */
static Pipeline pipe;

/** @brief DMA-write the active buffer to OBC flash. */
static void fire_dma(void);

/** @brief Read the next chunk from LFM into the inactive buffer. */
static void fill_next_buffer(void);

/** @brief Handle DMA TX completion from flash.c ISR. */
static void handle_dma_completion(void);

/** @brief Poll OBC WIP status when Timer3 sets the tick flag. */
static void handle_wip_poll_tick(void);

/** @brief Try to prefill the inactive buffer during the write window. */
static void try_fill_buffer(void);

/** @brief Fire DMA when both prerequisites (WIP + buffer) are met. */
static void try_fire_dma(void);

/** @brief Align the first write to a flash page boundary. */
static void pagealign_pass(Pipeline *pip);

/* ──────────── Internal configuration end ──────────── */

/* ──────────── Public ──────────── */

/**
 * @brief Execute a full pipelined DMA transfer from LFM to OBC flash.
 *
 * @details Blocks until the entire transfer completes (@c DONE) or the
 *          software watchdog fires (@c ERROR). The pipeline loop calls
 *          three helpers per iteration:
 *          1. @ref handle_dma_completion — poll DMA completion flag.
 *          2. @ref handle_wip_poll_tick — check WIP in foreground.
 *          3. @ref try_fill_buffer — prefill inactive buffer.
 *          4. @ref try_fire_dma — kick off the next DMA write.
 *
 * @param[in] src_addr  Starting LFM read address.
 * @param[in] dst_addr  Starting OBC flash write address.
 * @param[in] total     Total bytes to transfer.
 *
 * @pre @a src_addr must lie within @ref DATA_1ST .. @ref DATA_END.
 * @pre OBC flash target region must be erased.
 *
 * @warning Monopolises SPI1 (DMA) and Timer3 (WIP polling) for the
 *          duration of the transfer.
 *
 * @see obc_flash_write_dma, fire_dma
 */
void pipeline_transfer(uint32_t src_addr, uint32_t dst_addr, uint32_t total)
{
    if (total == 0 || src_addr < DATA_1ST || src_addr > DATA_END)
    {  // dst_addr is assigned by OBC. Remember OBC Flash must be in 4-byte address too.
        LOG("\n\r <?> Total size or source address are invalid.");
        return;
    }

    /* Init state machine. */
    pipe.src_address = src_addr;
    pipe.dst_address = dst_addr;
    pipe.total_bytes = total;
    pipe.active = 0;
    pipe.buf_len[0] = 0;
    pipe.buf_len[1] = 0;
    pipe.state = IDLE;
    pipe.wip_cleared = false;
    pipe.wip_poll_tick = false;
    pipe.buf_ready = false;
    pipe.loop_watchdog = 0;

    /** Pre-fill first buffer — SPI2 trivially free at this point. */
    pagealign_pass(&pipe);

    pipe.buf_ready = true;
    pipe.active = 0;

    LOG("\n\r SPI DMA mode starts...");
    fire_dma(); /* Kick off first DMA write. */

    /* Main pipeline loop. */
    while (pipe.state != DONE && pipe.state != ERROR)
    {
        handle_dma_completion();
        handle_wip_poll_tick();
        try_fill_buffer();
        try_fire_dma();

        if (++pipe.loop_watchdog > TIMEOUT_LOOP)
        {
            pipe.state = ERROR;
            LOG("\n\r <!> Pipeline watchdog timeout!");
            break;
        }
    }
}

/* ──────────── ISR ──────────── */

/**
 * @brief Timer3 ISR: raise a periodic WIP poll flag.
 *
 * @details Only active during @c FLASH_WRITING state. The actual SPI
 *          poll happens in foreground via @ref handle_wip_poll_tick to
 *          avoid SPI re-entrancy from ISR context.
 *
 * @note Assign to whichever free timer you pick. Configure for @ref WIP_TIMER_VALUE
 *       period before starting.
 */
#int_timer3
void obc_flash_wip(void)
{
    if (pipe.state != FLASH_WRITING)
    {
        return;
    }

    pipe.wip_poll_tick = true;
    set_timer3(WIP_TIMER_VALUE);
}

/* ──────────── Private ──────────── */

/**
 * @brief DMA-write the active buffer to OBC flash.
 *
 * @details Transitions to @c DMA_ACTIVE, clears all handshake flags,
 *          and fires @ref obc_flash_write_dma. On return, the ISR will
 *          eventually advance the state machine.
 *
 * @pre WIP must be cleared and @c buf_ready must be true.
 */
static void fire_dma(void)
{
    uint8_t *buf = pipe.buf[pipe.active];
    uint16_t size = pipe.buf_len[pipe.active];

    if (size == 0 || size > PAGE_SIZE)
    {
        LOG("\n\r <!> Invalid active buffer length.");
        pipe.state = ERROR;
        return;
    }

    pipe.state = DMA_ACTIVE;
    pipe.wip_cleared = false;
    pipe.wip_poll_tick = false;
    pipe.buf_ready = false;
    pipe.buf_len[pipe.active] = 0;

#ifdef DEBUG_MODE
    debug_mux_to_rpi();  // To debug with just one flash (LFM).
#endif                   /* DEBUG_MODE */
    obc_flash_write_dma(pipe.dst_address, buf, size);

    pipe.dst_address += size;
    pipe.total_bytes -= size;

    if (pipe.total_bytes == 0)
    {
        return;
    }
}

/**
 * @brief Read the next chunk from LFM into the inactive buffer.
 *
 * @details Swaps to the standby buffer, reads up to @ref PAGE_SIZE
 *          bytes, and sets @c buf_ready. If WIP was already cleared
 *          (Timer3 beat the read), advances state to @c READY.
 *
 * @pre @ref is_spi2_free must be true (DMAEN=0).
 */
static void fill_next_buffer(void)
{
    if (pipe.total_bytes == 0)
    {
        return;
    }

    uint8_t next = pipe.active ^ 1;  // Inactive buffer.
    uint16_t size =
        ((pipe.total_bytes >= PAGE_SIZE) ? PAGE_SIZE : ((uint16_t)pipe.total_bytes));

    LOG("\n\r Reading next buffer.");

#ifdef DEBUG_MODE
    disable_interrupts(INT_TIMER3);
#endif /* DEBUG_MODE */

    flash_read(pipe.src_address, pipe.buf[next], size);

#ifdef DEBUG_MODE
    enable_interrupts(INT_TIMER3);
#endif /* DEBUG_MODE */

    pipe.src_address += size;
    pipe.buf_len[next] = size;
    pipe.buf_ready = true;

    if (pipe.wip_cleared)
    { /* Advance to READY only if WIP already cleared (timer beat reading). */
        pipe.state = READY;
    }
}

/**
 * @brief Handle DMA TX completion signalled by the SSP1 ISR in flash.c.
 *
 * @details Polls @ref is_dma_tx_complete. On completion, clears the flag,
 *          decides @c DONE vs @c FLASH_WRITING based on remaining bytes,
 *          and arms Timer3 for WIP polling.
 */
static void handle_dma_completion(void)
{
    if (pipe.state != DMA_ACTIVE)
    {
        return;
    }

    if (!is_dma_tx_complete())
    {
        return;
    }

    clear_dma_tx_complete();

    pipe.state = pipe.total_bytes == 0 ? DONE : FLASH_WRITING;

    set_timer3(WIP_TIMER_VALUE);
    enable_interrupts(INT_TIMER3);
}

/**
 * @brief Poll OBC WIP status when Timer3 sets the tick flag.
 *
 * @details Only runs in @c FLASH_WRITING state. If WIP is cleared,
 *          sets @c wip_cleared and advances to @c READY (if buffer is
 *          also filled). Disables Timer3 until the next DMA cycle.
 */
static void handle_wip_poll_tick(void)
{
    if (pipe.state != FLASH_WRITING)
    {
        return;
    }

    if (!pipe.wip_poll_tick)
    {
        return;
    }

    pipe.wip_poll_tick = false;

    if (is_obc_wip())
    {
        LOG("\n\r WIP bit is still HIGH!");
        return;
    }

    pipe.wip_cleared = true;

    if (pipe.buf_ready)  // Advance to READY only if buffer is also filled.
    {
        LOG("\n\r Buffer ready");
        pipe.state = READY;
    }

    disable_interrupts(INT_TIMER3);  // Stop polling until next DMA cycle.
}

/**
 * @brief Try to prefill the inactive buffer during the OBC write window.
 *
 * @details Guards against premature reads: only proceeds when the state
 *          is @c FLASH_WRITING, SPI2 is free, WIP is cleared, and the
 *          buffer hasn't already been filled.
 */
static void try_fill_buffer(void)
{
    if (pipe.state != FLASH_WRITING)
    {
        LOG("\n\r Not in write window");
        return;
    }

    if (!is_spi2_free())
    {
        LOG("\n\r SPI2 not ready yet to use.");
        return;
    }

    if (!pipe.wip_cleared)
    {
        LOG("\n\r Waiting WIP clear before reading shared flash.");
        return;
    }

    if (pipe.buf_ready)
    {
        LOG("\n\r Tried to fill buffer more than once.");
        return;
    }

    fill_next_buffer();  // Next buffer can get next chunk of data.
}

/**
 * @brief Fire DMA when both prerequisites have been met.
 *
 * @details Only runs in @c READY state (WIP cleared + buffer filled).
 *          Swaps the active buffer index and calls @ref fire_dma.
 */
static void try_fire_dma(void)
{
    if (pipe.state != READY)
    {
        LOG("\n\r Pipe not ready yet!");
        return;
    }

    LOG("\n\r Pipe ready to fire DMA.\n\r Switching buffer...");
    // Golden window: OBC flash writing internally, SPI2 is free.
    pipe.active ^= 1;  // Switch to inactive buffer, 0 or 1.

    fire_dma();  // Both conditions met? Hit it.
}

/**
 * @brief Align the first write to a flash page boundary.
 *
 * @details If @c dst_address isn't page-aligned, the first chunk is
 *          shortened to fit within the current page. This avoids a
 *          page-wrap on the first DMA write.
 *
 * @param[in,out] pip  Pipeline context — @c src_address is advanced
 *                     and @c buf_len[0] is set.
 *
 * @note @c total_bytes is decremented later in @ref fire_dma.
 */
static void pagealign_pass(Pipeline *pip)
{
    uint16_t offset = (uint16_t)(pip->dst_address % PAGE_SIZE);
    uint16_t space_in_page = (offset == 0) ? PAGE_SIZE : (PAGE_SIZE - offset);

    uint16_t chunk_size =
        (pip->total_bytes > space_in_page) ? space_in_page : ((uint16_t)pip->total_bytes);

    flash_read(pip->src_address, pip->buf[0], chunk_size);

    /* total_bytes is decremented in fire_dma(). */
    pip->src_address += chunk_size;
    pip->buf_len[0] = chunk_size;
}
/* Hot bloodеd, you've been running around, you know you
 * Talk crazy when you try to get out, you know you
 * You're my baby, yeah you're coming around
 *                          --Harlee Case, 2021 **/
