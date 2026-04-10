/**
 * @file obc_tasks.c
 *
 * @brief Task handlers for commands received from the OBC.
 *
 * @details Each public function maps 1:1 to an @ref ObcRxCommand_e
 *          member and is dispatched by the parser after CRC validation.
 *          Three categories of operations live here:
 *
 *          - **RPi relay** — power control and request forwarding
 *            (@ref rpi_on through @ref rpi_report_request).
 *          - **Write pipeline** — three flavours of LFM-to-OBC flash
 *            transfer, differing only in how the source address is
 *            resolved (last pointer, metadata match, or explicit).
 *          - **Deletion** — three flavours of flash erase, differing
 *            in how head/tail are resolved (explicit range, single
 *            address lookup, or metadata match).
 *
 *          The private helpers @ref complete_write and
 *          @ref complete_erase factor out the shared tail of each
 *          category so the public handlers stay slim.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-07
 *
 * @todo Rename @c reset_frame to @c flush_module or similar.
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/drivers/pin_map.h"
#include "../../include/flash/flash_types.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/flash.h"
#include "../../include/app/obc_tasks.h"
#include "../../include/app/obc_command.h"
#include "../../include/app/rpi_command.h"
#include "../../include/flash/metadata.h"
#include "../../include/flash/index_manager.h"
#include "../../include/app/task_helper.h"
#include "../../include/protocol/conversions.h"
#include "../../include/flash/eraser.h"
#include "../../include/flash/image_transfer.h"

/* ───────────────────────────────────────────── Internal configuration (do not export) */

/**
 * @def SRC_IS_REKT
 * @brief Sentinel value for "no valid source address found for occurrence".
 */
#define SRC_IS_REKT 0xDEADBABE

/**
 * @def PTR_END_CUE
 * @brief Stands for "Pointer end cue". All-ones sentinel marking the first unused
 * address-pointer slot.
 */
#define PTR_END_CUE 0xFFFFFFFFUL

/**
 * @enum PayloadSize_e
 * @brief Expected payload sizes for different OBC command types.
 */
typedef enum PayloadSize_e
{
    IMG_CODE_SIZE = 3,       ///< Image request code (3 bytes).
    METADATA_CODE_SIZE = 5,  ///< Compact metadata code (5 bytes).
    PEER_IMG_ADDR_SIZE = 8   ///< Address pair for peer image ops (8 bytes).
} PayloadSize;

/**
 * @struct CaptureRequest_s
 * @brief Payload fields for a photo capture command.
 */
typedef struct CaptureRequest_s
{
    uint8_t quantity;  ///< Number of photos to capture.
    uint8_t method;    ///< Capture method (burst, etc) — not yet implemented.
} CaptureRequest;

/**
 * @struct ImageRequest_s
 * @brief Payload fields for an image download request.
 */
typedef struct ImageRequest_s
{
    uint8_t code[IMG_CODE_SIZE];  ///< 3-byte image selection code.
    RpiTxCommand quality;         ///< @ref FULL_HD or @ref THUMB.
} ImageRequest;

/** @brief Scan the pointer section for the last valid address. */
#inline
static void get_last_pointer(uint32_t &address_pointer);

/** @brief Find a source address by EXIF metadata match. */
#inline
static void search_by_metadata(uint8_t *compact_src, uint32_t &src_out);

/** @brief Look up an index entry that contains a given address. */
#inline
static bool find_entry_by_address(uint32_t address, uint32_t &head, uint32_t &tail);

/** @brief Validate that a source address falls within the data region. */
#inline
static void check_source(uint32_t &source);

/** @brief Shared write tail: validate, log, and run the DMA pipeline. */
#inline
static void complete_write(uint32_t &source, uint32_t &target, uint32_t &size,
                           Dataframe *module);

/** @brief Shared erase tail: adjust bounds, erase, and update index. */
#inline
static void complete_erase(EraseContext *ctx, Dataframe *module);

/* ───────────────────────────────────────────────────────── Internal configuration end */

/* ────────── Public: RPi Power & Request Commands ────────── */

/**
 * @brief Switch RPi supply on via @ref RPI_EN.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 */
void rpi_on(Dataframe *module)
{
    reset_frame(module);
    output_high(RPI_EN);
}

/**
 * @brief Cut RPi power supply via @ref RPI_EN.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 */
void rpi_off(Dataframe *module)
{
    reset_frame(module);
    output_low(RPI_EN);
}

/**
 * @brief Request RPi to power off itself gracefully.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see poweroff_countdown (RPi-side handler)
 */
void rpi_request_poweroff(Dataframe *module)
{
    reset_frame(module);
    construct_and_send(SUDO_POWEROFF, NULL, 0, SELECT_RPI);
}

/**
 * @brief Request RPi to reboot.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @warning Rebooting RPi is power-hungry.
 */
void rpi_request_reboot(Dataframe *module)
{
    reset_frame(module);
    construct_and_send(SUDO_REBOOT, NULL, 0, SELECT_RPI);
}

/**
 * @brief Request RPi to capture photos.
 *
 * @details Extracts the capture quantity from the payload and forwards
 *          it to RPi via @ref CAM_ON.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 */
void rpi_request_capture(Dataframe *module)
{
    CaptureRequest photo;  // Capture method (burst, etc) yet to be implemented.

    get_payload(&photo.quantity, sizeof(photo.quantity), module);
    reset_frame(module);

    construct_and_send(CAPTURE_PHOTO, &photo.quantity, sizeof(photo.quantity),
                       SELECT_RPI);
}

/**
 * @brief Request a photo download from RPi based on selection code.
 *
 * @details Three request types are distinguished by the 3-byte code:
 *          - Metadata code: @c {0x50, count, category_byte}.
 *          - Latest: @c {0x52, category_byte, NULL}.
 *          - Best confidence: @c {0x54, category_byte, NULL}.
 *
 * @param[in,out] module   Source @ref Dataframe_s (flushed after use).
 * @param[in]     is_full  If true, request full-resolution; else low quality.
 *
 * @note MUX is not switched here. Flash ownership is claimed
 *       proactively in @ref check_flash_space, right before the
 *       RPi receives the write address.
 *
 * @see claim_flash_for_rpi, check_flash_space
 */
void rpi_request_image(Dataframe *module, bool is_full)
{
    ImageRequest image;

    get_payload(image.code, IMG_CODE_SIZE, module);
    reset_frame(module);

    image.quality = is_full == true ? TRANSFER_HQ : TRANSFER_LQ;

    construct_and_send(image.quality, image.code, IMG_CODE_SIZE, SELECT_RPI);
}

/**
 * @brief Request RPi's database report for downlink.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 */
void rpi_report_request(Dataframe *module)
{
    reset_frame(module);
    construct_and_send(REQUEST_REPORT, NULL, 0, SELECT_RPI);
}

/* ───────────────────────── Public: OBC Flash Write Commands ───────────────────────── */

/**
 * @brief Write from last address pointer to OBC flash.
 *
 * @details **PTR method:** payload = DST_ADDR (4) + SIZE (4) = 8 bytes.
 *          Source is resolved by scanning the pointer section for the
 *          last non-sentinel entry.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see get_last_pointer, complete_write
 */
void obc_write_pointer(Dataframe *module)
{
    uint32_t target, size, source;
    get_uint32_from_frame(target, module->buffer);
    get_uint32_from_frame(size, module->buffer + ADDRESS_SIZE);
    get_last_pointer(source);

    complete_write(source, target, size, module);
}

/**
 * @brief Write from metadata-matched source to OBC flash.
 *
 * @details **META method:** payload = DST_ADDR (4) + SIZE (4) +
 *          METADATA (5) = 13 bytes. Source is resolved by scanning
 *          all index entries for a matching EXIF UserComment.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see search_by_metadata, complete_write
 */
void obc_write_meta(Dataframe *module)
{
    uint32_t target, size, source;
    get_uint32_from_frame(target, module->buffer);
    get_uint32_from_frame(size, module->buffer + ADDRESS_SIZE);
    search_by_metadata(module->buffer + TWO_ADDRESS_SIZE, source);

    complete_write(source, target, size, module);
}

/**
 * @brief Write from explicit source address to OBC flash.
 *
 * @details **SRC method:** payload = DST_ADDR (4) + SRC_ADDR (4) +
 *          SIZE (4) = 12 bytes. Source is provided directly by Ground
 *          Control and validated against the data region.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see check_source, complete_write
 */
void obc_write_from_to(Dataframe *module)
{
    uint32_t target, size, source;
    get_uint32_from_frame(target, module->buffer);
    get_uint32_from_frame(size, module->buffer + ADDRESS_SIZE);
    get_uint32_from_frame(source, module->buffer + TWO_ADDRESS_SIZE);
    check_source(source);

    complete_write(source, target, size, module);
}

/* ────────── Public: Deletion Commands ────────── */

/**
 * @brief Delete flash data by explicit address range.
 *
 * @details Head and tail provided directly by Ground Control.
 *          Payload = 8 bytes, total UL frame ~14 bytes.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see complete_erase
 */
void delete_range(Dataframe *module)
{
    EraseContext ctx;
    get_uint32_pair_payload(ctx.head_address, ctx.tail_address, module->buffer);

    complete_erase(&ctx, module);
}

/**
 * @brief Delete flash data by single address lookup.
 *
 * @details One address provided; index entry lookup determines the
 *          head and tail. Payload = 4 bytes, total UL frame ~10 bytes.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see find_entry_by_address, complete_erase
 */
void delete_address(Dataframe *module)
{
    uint8_t addr_bytes[ADDRESS_SIZE];
    get_payload(addr_bytes, ADDRESS_SIZE, module);
    uint32_t address = bytes_to_uint32(addr_bytes);

    EraseContext ctx;
    if (!find_entry_by_address(address, ctx.head_address, ctx.tail_address))
    {
        reset_frame(module);
        construct_and_send(NAK_REQ, NULL, 0, SELECT_OBC);
#ifdef DEBUG_MODE
        construct_and_send(NAK_REQ, NULL, 0, SELECT_DBG);
#endif /* DEBUG_MODE */
        return;
    }

    complete_erase(&ctx, module);
}

/**
 * @brief Delete flash data by metadata search.
 *
 * @details Compact metadata code locates the target image, then index
 *          entry lookup determines head and tail. Payload = 5 bytes,
 *          total UL frame ~11 bytes.
 *
 * @param[in,out] module  Source @ref Dataframe_s (flushed after use).
 *
 * @see search_by_metadata, find_entry_by_address, complete_erase
 */
void delete_metadata(Dataframe *module)
{
    uint8_t meta[METADATA_CODE_SIZE];
    uint32_t source;
    get_payload(meta, METADATA_CODE_SIZE, module);
    search_by_metadata(meta, source);

    EraseContext ctx;
    if (source == SRC_IS_REKT ||
        !find_entry_by_address(source, ctx.head_address, ctx.tail_address))
    {
        reset_frame(module);
        construct_and_send(NAK_REQ, NULL, 0, SELECT_OBC);
#ifdef DEBUG_MODE
        construct_and_send(NAK_REQ, NULL, 0, SELECT_DBG);
#endif /* DEBUG_MODE */
        return;
    }

    complete_erase(&ctx, module);
}

/* ────────── Private ────────── */

/**
 * @brief Validate source address is within the data region.
 *
 * @param[in,out] source  Address to check (CCS reference). Set to
 *                        @ref SRC_IS_REKT if out of bounds.
 */
#inline
static void check_source(uint32_t &source)
{
    if (source < DATA_1ST || source > DATA_END)
    {
        source = SRC_IS_REKT;
    }
}

/**
 * @brief Shared tail for all write operations: validate, log, transfer.
 *
 * @details If @a source equals @ref SRC_IS_REKT, sends a @ref NAK_REQ
 *          to OBC and bails. Otherwise logs addresses/size and kicks
 *          off @ref pipeline_transfer.
 *
 * @param[in]     source  LFM source address (CCS reference).
 * @param[in]     target  OBC flash destination address (CCS reference).
 * @param[in]     size    Transfer size in bytes (CCS reference).
 * @param[in,out] module  Source @ref Dataframe_s (flushed first).
 *
 * @see pipeline_transfer
 */
#inline
static void complete_write(uint32_t &source, uint32_t &target, uint32_t &size,
                           Dataframe *module)
{
    reset_frame(module);

    if (source == SRC_IS_REKT)
    {
        construct_and_send(NAK_REQ, NULL, 0, SELECT_OBC);
#ifdef DEBUG_MODE
        construct_and_send(NAK_REQ, NULL, 0, SELECT_DBG);
#endif /* DEBUG_MODE */
        return;
    }

    LOG("\n\r ->Source (LFM) address:  0x%8LX (%Lu)\n\r ->target (OBCFM) address:  "
        "0x%8LX (%Lu)\n\r ->Size: 0x%8LX (%Lu Bytes)",
        source, source, target, target, size, size);

    pipeline_transfer(source, target, size);
}

/**
 * @brief Shared tail for all erase operations: adjust, erase, update.
 *
 * @details Logs the requested range, calls
 *          @ref deletion_bounds_adjustment to align and protect
 *          neighbors, runs @ref erase_data, and (on success) updates
 *          the index via @ref update_index_after_erase.
 *
 * @param[in,out] ctx     @ref EraseContext_s with head/tail to adjust.
 * @param[in,out] module  Source @ref Dataframe_s (flushed first).
 *
 * @todo Count tombstones at boot — if the section is full, trigger
 *       lazy GC before attempting new operations.
 */
#inline
static void complete_erase(EraseContext *ctx, Dataframe *module)
{
    reset_frame(module);

    LOG("\n\r ->Addresses: [ 0x%8LX - 0x%8LX ]\n\r ->Size: 0x%8LX (%Lu) Bytes",
        ctx->head_address, ctx->tail_address, ctx->tail_address - ctx->head_address + 1,
        ctx->tail_address - ctx->head_address + 1);

    deletion_bounds_adjustment(ctx);

    erase_data(ctx);

    if (!ctx->success)
    {
        LOG("\n\r <?> Error. No erasing has been executed.");
        return;
    }

    update_index_after_erase(ctx);
}

/**
 * @brief Scan the address-pointer section for the last valid entry.
 *
 * @details Address pointers are stored sequentially in flash in
 *          @ref ADDRESS_SIZE-byte slots. A slot equal to
 *          @ref PTR_END_CUE (0xFFFFFFFF) marks the first unused entry.
 *
 *          The function scans forward:
 *          - The first @ref PTR_END_CUE found marks "one-past-last."
 *          - The previous value is the last valid downloaded address.
 *          - If the first slot is @ref PTR_END_CUE, no valid address
 *            exists and @ref SRC_IS_REKT is returned.
 *
 * @param[out] address_pointer  Receives the last valid address, or
 *                              @ref SRC_IS_REKT if none exists.
 *
 * @warning If no @ref PTR_END_CUE is found the section is considered
 *          full. The returned value is the last slot, which may be
 *          ambiguous without additional metadata. Additional tracking
 *          (write count, tombstones, or GC) is required.
 */
#inline
static void get_last_pointer(uint32_t &address_pointer)
{
    uint8_t current_bytes[ADDRESS_SIZE] = {0};
    uint32_t current, last;
    uint16_t slot = ADDR_PTR_1ST;

    for (; slot <= ADDR_PTR_END; slot += ADDRESS_SIZE)
    {
        flash_read((uint32_t)slot, current_bytes, ADDRESS_SIZE);
        current = bytes_to_uint32(current_bytes);

        // PTR_END_CUE marks the first unused slot (one-past-last).
        if (current == PTR_END_CUE)
        {
            LOG("\n\r -> Last address pointer 0x%8LX found in slot %lu.", last,
                1 + ((slot - ADDR_PTR_1ST) >>
                     2));  // frustrated-human-friendly slot read.
            break;
        }

        last = current;  // Keep the last non-sentinel value seen.
    }

    if (slot == ADDR_PTR_1ST)
    {
        last = SRC_IS_REKT;
    }

    address_pointer = last;
    // If ADDRESS POINTER SECTION is full of addresses and the last one is not
    // PTR_END_CUE, is `last` actually the last or some past value?
    // This is a vulnerability, I need to detect with a technique like the count
    // tombstones at the start (below setup) to see if the whole section is full
    // or not. Basically if reaches a threeshold, a Lazy Garbage Collection is
    // executed, cleaning the whole first flash subsector and rewriting
    // DATA_STAT, and index entry sector. I got the full picture, and some of
    // the code to qsort data and bla bla is for another ocasion. For now I'm
    // keeping it as is with that in mind.
}

/**
 * @brief Scan the index for an image whose EXIF UserComment matches
 *        a compact metadata code.
 *
 * @details Walks every @ref DATA_STATE_FULL_DATA slot, extracts the
 *          UserComment from each JPEG via
 *          @ref metadata_extract_user_comment, and compares against
 *          the uplinked code via @ref match_metadata_code.
 *
 * @param[in]  compact_src  5-byte compact metadata code.
 * @param[out] src_out      Receives the flash base address of the
 *                          matched image, or @ref SRC_IS_REKT if no
 *                          match.
 */
#inline
static void search_by_metadata(uint8_t *compact_src, uint32_t &src_out)
{
    uint8_t compact[METADATA_CODE_SIZE];
    char comment[8] = {0};

    memcpy(compact, compact_src, METADATA_CODE_SIZE);

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState state = flash_get_state(slot);

        if (state == DATA_STATE_UNTOUCHED)
        {
            break;
        }

        if (state != DATA_STATE_FULL_DATA)
        {
            continue;
        }

        Entry entry;
        flash_get_entries(data_state_to_index_entry(slot), &entry);

        uint32_t jpeg_size = entry.end - entry.start + 1;

        int result = metadata_extract_user_comment(entry.start, jpeg_size, comment);

        if (result > 0 && match_metadata_code(compact, comment))
        {
            src_out = entry.start;
            return;
        }
    }

    src_out = SRC_IS_REKT;

    LOG("\n\r Error: No matching metadata found.");
}

/**
 * @brief Find the index entry that contains a given address.
 *
 * @details Scans all @ref DATA_STATE_FULL_DATA entries. Returns the
 *          first entry where @a address falls within [start, end].
 *
 * @param[in]  address  Address to look up.
 * @param[out] head     Receives entry start address on match
 *                      (CCS reference).
 * @param[out] tail     Receives entry end address on match
 *                      (CCS reference).
 *
 * @return @c true if an entry was found, @c false otherwise.
 */
#inline
static bool find_entry_by_address(uint32_t address, uint32_t &head, uint32_t &tail)
{
    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState state = flash_get_state(slot);

        if (state == DATA_STATE_UNTOUCHED)
        {
            break;
        }
        if (state != DATA_STATE_FULL_DATA)
        {
            continue;
        }

        Entry entry;
        flash_get_entries(data_state_to_index_entry(slot), &entry);

        if (address >= entry.start && address <= entry.end)
        {
            head = entry.start;
            tail = entry.end;
            return true;
        }
    }

    LOG("\n\r Error: No entry found for 0x%8LX.", address);
    return false;
}
/* You can't start a fire
 * You can't start a fire without a spark
 *          --Bruce Springsteen, 1984 **/
