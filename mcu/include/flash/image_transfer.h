/**
 * @file image_transfer.h
 *
 * @brief Double-buffered DMA pipeline for LFM-to-OBC flash image transfer.
 *
 * @details Orchestrates a fully pipelined bulk copy: while one page is being
 *          DMA'd to OBC flash over SPI1, the next page is read from LFM over
 *          SPI2 into the standby buffer. Two ISRs drive the handshake:
 *
 *          - **SSP1 ISR** -- fires when DMA completes a page write; transitions
 *            the state machine out of @c DMA_ACTIVE.
 *          - **Timer3 ISR** -- periodically raises a WIP poll flag so the
 *            foreground loop can check OBC flash status without SPI re-entrancy.
 *
 *          The pipeline state machine (internal @c PipeState_e):
 *
 * @dot
 * digraph PipeState {
 *     rankdir=LR; node [shape=box, style=rounded];
 *     IDLE          -> DMA_ACTIVE    [label="fire_dma()"];
 *     DMA_ACTIVE    -> FLASH_WRITING [label="SSP1 ISR\n(bytes left)"];
 *     DMA_ACTIVE    -> DONE          [label="SSP1 ISR\n(total==0)"];
 *     FLASH_WRITING -> READY         [label="WIP=0 &&\nbuf_ready"];
 *     FLASH_WRITING -> ERROR         [label="watchdog\ntimeout"];
 *     READY         -> DMA_ACTIVE    [label="fire_dma()"];
 * }
 * @enddot
 *
 *          ISR-to-foreground handshake:
 *
 * @msc
 * SSP1_ISR, Timer3_ISR, MainLoop;
 * MainLoop -> MainLoop   [label="fire_dma() -> DMA_ACTIVE"];
 * SSP1_ISR -> SSP1_ISR   [label="DMAEN cleared -> FLASH_WRITING"];
 * SSP1_ISR -> Timer3_ISR [label="enable Timer3"];
 * Timer3_ISR -> MainLoop [label="wip_poll_tick = true"];
 * MainLoop -> MainLoop   [label="poll WIP -> wip_cleared"];
 * MainLoop -> MainLoop   [label="fill_next_buffer -> buf_ready"];
 * MainLoop -> MainLoop   [label="READY -> fire_dma()"];
 * @endmsc
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-05
 *
 * @ingroup storage
 */
#ifndef IMAGE_TRANSFER_H
#define IMAGE_TRANSFER_H

/**
 * @defgroup storage Storage Layer
 * @brief Index management, metadata, space allocation, erasure, and
 *        image transfer pipeline.
 *
 * @see @ref globals_flash for flash geometry and section boundaries.
 * @see @ref driver_flash for the low-level SPI operations these
 *      modules build on.
 * @{
 */

/**
 * @brief Execute a full pipelined DMA transfer from LFM to OBC flash.
 *
 * @details Blocks until the entire transfer completes (state reaches
 *          @c DONE) or the software watchdog fires (@c ERROR). SPI2 is
 *          intermittently blocked while DMA is active.
 *
 * @param[in] src_addr  Starting LFM read address.
 * @param[in] dst_addr  Starting OBC flash write address.
 * @param[in] total     Total bytes to transfer.
 *
 * @pre @p src_addr must lie within @ref DATA_1ST .. @ref DATA_END.
 * @pre OBC flash target region must be erased.
 *
 * @warning Monopolizes SPI1 (DMA) and Timer3 (WIP polling) for the
 *          duration of the transfer.
 *
 * @see @ref driver_flash for the underlying SPI operations.
 */
void pipeline_transfer(uint32_t src_addr, uint32_t dst_addr, uint32_t total);

/** @} */ /* storage */

#endif /* IMAGE_TRANSFER_H */
