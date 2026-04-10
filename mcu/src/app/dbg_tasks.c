/**
 * @file dbg_tasks.c
 *
 * @brief Debug terminal task handlers: flash I/O tests, RPi comms
 *        probe, DMA pipeline exercise, metadata viewer, and manual
 *        erase.
 *
 * @details Compiled only under @ref DEBUG_MODE. Every function here
 *          is invoked from the debug parser after a matching
 *          @ref DbgRxCommand_e arrives on the serial terminal. Two
 *          broad categories:
 *
 *          - **RPi debug controls** — @ref rpi_communication_test,
 *            @ref debug_report_request.
 *          - **Flash debug operations** — reads, writes (CPU and
 *            DMA), metadata extraction, erase (tiered and manual),
 *            and index/status logging.
 *
 * @note The author uses [tio](https://github.com/tio/tio), btw.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-07
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/protocol/frame.h"
#include "../../include/app/dbg_tasks.h"
#include "../../include/app/obc_command.h"
#include "../../include/flash/space_finder.h"
#include "../../include/app/rpi_command.h"
#include "../../include/flash/metadata.h"
#include "../../include/flash/index_manager.h"
#include "../../include/app/task_helper.h"
#include "../../include/drivers/uart.h"
#include "../../include/drivers/flash.h"
#include "../../include/protocol/conversions.h"
#include "../../include/flash/eraser.h"
#include "../../include/flash/image_transfer.h"

#ifdef DEBUG_MODE

/* ─────────────────── Internal configuration (do not export) ─────────────────── */

/** @brief Read a flash region and dump it to the debug terminal. */
static void read_and_print(uint32_t addr_1st, uint32_t size);

/** @brief CPU-blocking write test using synthetic data. */
static void write_data(uint32_t addr_1st, uint32_t total_size);

/** @brief Fill a buffer with synthetic source data for write tests. */
static void get_data(uint8_t *data, uint16_t size);

/** @brief Align the first non-blocking write to a page boundary. */
static void align_pass(uint32_t *current_dst, uint32_t *current_src, uint8_t *buffer,
                       uint32_t *remaining);

/**
 * @union ImageSize_u
 * @brief Overlay for accessing an image size as a uint32 or raw bytes.
 *
 * @note Can be fused with @ref AddressAndSize_s if endianness is right.
 */
typedef union ImageSize_u
{
    uint32_t value;               ///< Size as a 32-bit integer.
    uint8_t bytes[ADDRESS_SIZE];  ///< Same value as raw bytes.
} ImageSize;

/**
 * @struct AddressAndSize_s
 * @brief Address + size pair extracted from debug payloads.
 */
typedef struct AddressAndSize_s
{
    uint32_t address;  ///< Flash start address.
    uint32_t size;     ///< Byte count.
} AddressAndSize;

/* ──────────── Internal configuration end ──────────── */

/* ──────────── RPi Debug Controls ──────────── */

/**
 * @brief Test RPi UART by forwarding an arbitrary frame from the
 *        debug terminal.
 *
 * @details The tester crafts a full dataframe in the terminal:
 *          `{OBC_ID, FRAME_TEST_RPI, <payload...>, CRC_MSB,
 *          CRC_LSB, STOP_BYTE}`. The embedded payload is extracted
 *          and forwarded verbatim to the RPi channel.
 *
 * @note It's the tester's responsibility to construct a valid RPi
 *       frame inside the debug wrapper.
 *
 * @see track_command
 */
void rpi_communication_test(void)
{
    volatile UartPacket *rx_packet = (volatile UartPacket *)dbg.buffer;

    if (rx_packet->payload_length > PAYLOAD_MAX_SIZE)
    {
        LOG("\n\r ->Frame for Raspberry out of range! Range: 0-39 bytes.");
        reset_frame(&dbg);
        return;
    }

    uint8_t buffer[PAYLOAD_MAX_SIZE] = {0};
    memcpy(buffer, (uint8_t *)rx_packet->payload, rx_packet->payload_length);

    send_bytes(buffer, rx_packet->payload_length, SELECT_DBG);
    send_bytes(buffer, rx_packet->payload_length, SELECT_RPI);

    reset_frame(&dbg);

    track_command(buffer, rpi.tx_tracker);
}

/**
 * @brief Request the RPi to send its database report.
 *
 * @details Sends a @ref REPORT_DL command to the RPi channel,
 *          prompting it to reply with its stored report data.
 */
void debug_report_request(void)
{
    reset_frame(&dbg);

    LOG("\n\r -- DBG command identified as: DEBUG_REPORT --");

    construct_and_send(REQUEST_REPORT, NULL, 0, SELECT_RPI);
}

/* ──────────── Flash Debug Operations ──────────── */

/**
 * @brief Read and dump flash data from a user-specified address + size.
 *
 * @details Extracts a (address, size) pair from the debug payload and
 *          prints the raw flash contents to the terminal.
 *
 * @pre Payload has passed frame and CRC verification.
 *
 * @see read_and_print
 */
void debug_read_from(void)
{
    AddressAndSize read;
    get_uint32_pair_payload(read.address, read.size, dbg.buffer);

    reset_frame(&dbg);

    LOG("\n\r ->Address received: 0x%8LX"
        "\n\r ->Size received: 0x%8LX (%Lu Bytes)",
        read.address, read.size, read.size);

    read_and_print(read.address, read.size);
}

/**
 * @brief Dump the full index status section and index entry section.
 *
 * @details Reads and prints two flash regions:
 *          1. @ref DATA_STAT_1ST .. @ref DATA_STAT_END (entry states).
 *          2. @ref IDX_ENTRY_1ST .. @ref IDX_ENTRY_END (index entries).
 *
 * @see read_and_print
 */
void debug_log_entry(void)
{
    reset_frame(&dbg);

    LOG("\n\r ENTRY STATUS:\n");
    read_and_print(DATA_STAT_1ST, DATA_STAT_END - DATA_STAT_1ST + 1U);
    LOG("\n\r INDEX ENTRY:\n");
    read_and_print(IDX_ENTRY_1ST, IDX_ENTRY_END - IDX_ENTRY_1ST + 1U);
}

/**
 * @brief Dump every stored image's raw data to the debug terminal.
 *
 * @details Walks the data-state slots. For each @ref DATA_STATE_FULL_DATA
 *          entry, resolves its index entry and prints the entire image
 *          blob. Stops at the first @ref DATA_STATE_UNTOUCHED slot.
 *
 * @see flash_get_entries, data_state_to_index_entry, read_and_print
 */
void debug_log_image_data(void)
{
    reset_frame(&dbg);

    Entry entry = {0, 0};
    uint16_t count = 0;

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState slot_state = flash_get_state(slot);

        if (slot_state == DATA_STATE_FULL_DATA)
        {
            LOG("\n\r -> Image %lu\n", ++count);

            flash_get_entries(data_state_to_index_entry(slot), &entry);
            read_and_print(entry.start, entry.end - entry.start + 1U);

            continue;
        }

        if (slot_state == DATA_STATE_UNTOUCHED)
        {
            break;
        }
    }

    LOG("\n\r <!> Image reading finished.");
}

/**
 * @brief Non-blocking (CPU-free) flash write test: source → destination.
 *
 * @details Handles page alignment, full-page copies, and a tail
 *          partial page — all using @ref flash_write.
 *
 * @param[in] source      LFM source address to read from.
 * @param[in] destiny     Destination flash address to write to.
 * @param[in] total_size  Total bytes to copy.
 *
 * @pre @a destiny must fall within @ref DATA_1ST .. @ref DATA_END.
 *
 * @see align_pass, flash_write
 */
static void write_from_to(uint32_t source, uint32_t destiny, uint32_t total_size)
{
    if (destiny < DATA_1ST || (destiny + total_size - 1U) > DATA_END)
    {
        LOG("\n\r<?> Forbidden zone.\n");
        return;
    }

    static uint8_t buffer[PAGE_SIZE];
    uint32_t current_dst = destiny;
    uint32_t current_src = source;
    uint32_t remaining = total_size;

    /* Alignment (first partial page). */
    align_pass(&current_dst, &current_src, buffer, &remaining);

    /* Full pages. */
    while (remaining >= PAGE_SIZE)
    {
        flash_read(current_src, buffer, PAGE_SIZE);
        flash_write(current_dst, buffer, PAGE_SIZE);

        current_src += PAGE_SIZE;
        current_dst += PAGE_SIZE;
        remaining -= PAGE_SIZE;
    }

    /* Tail (final partial page). */
    if (remaining > 0)
    {
        flash_read(current_src, buffer, (uint16_t)remaining);
        flash_write(current_dst, buffer, (uint16_t)remaining);
    }

    LOG("\n\r <!> Writing beta test finished.\n");
}

/**
 * @brief Align the first write to a page boundary for non-blocking
 *        copy tests.
 *
 * @param[in,out] current_dst  Destination address — advanced past the
 *                             partial page.
 * @param[in,out] current_src  Source address — advanced in lockstep.
 * @param[in]     buffer       Scratch buffer (at least @ref PAGE_SIZE).
 * @param[in,out] remaining    Bytes left — decremented by the partial
 *                             page size.
 */
static void align_pass(uint32_t *current_dst, uint32_t *current_src, uint8_t *buffer,
                       uint32_t *remaining)
{
    uint16_t offset = (uint16_t)(*current_dst % PAGE_SIZE);

    if (offset == 0)
    {
        LOG("\n\r First destinatary address is already page-aligned.");
        return;
    }

    uint16_t space_in_page = PAGE_SIZE - offset;
    uint16_t chunk = (*remaining > space_in_page) ? space_in_page : (uint16_t)*remaining;

    flash_read(*current_src, buffer, chunk);
    flash_write(*current_dst, buffer, chunk);

    *current_src += chunk;
    *current_dst += chunk;
    *remaining -= chunk;
}

/**
 * @brief CPU-free write test: extract source, destination, and size
 *        from the debug payload and copy flash-to-flash.
 *
 * @details Parses a 12-byte payload: source address (4), destination
 *          address (4), size (4). Delegates to @ref write_from_to.
 *
 * @see write_from_to
 *
 * @todo Fix this shit that was tweaked for a test.
 */
void debug_write(void)  // copy_from_to
{
    uint8_t payload_bytes[TWO_ADDRESS_SIZE + ADDRESS_SIZE] = {0};

    get_payload(payload_bytes, sizeof(payload_bytes), &dbg);

    reset_frame(&dbg);

    uint32_t src_addr = bytes_to_uint32(payload_bytes);
    uint32_t dst_addr = bytes_to_uint32(payload_bytes + ADDRESS_SIZE);
    uint32_t total = bytes_to_uint32(payload_bytes + TWO_ADDRESS_SIZE);

    LOG("\n\r ->Source address: 0x%8LX\n\r ->Destiny address: 0x%8LX\n\r ->Size: %Lu "
        "Bytes\n",
        src_addr, dst_addr, total);

    write_from_to(src_addr, dst_addr, total);

    if (!update_index_after_write(dst_addr, total))
    {
        LOG("\n\r <?> Couldn't update entry and status.");
        return;
    }

    LOG("\n\r <!> Entry and status updated.");
}

/**
 * @brief DMA pipeline write test: source → destination via
 *        @ref pipeline_transfer.
 *
 * @details Parses a 12-byte payload (source, destination, size) and
 *          kicks off the double-buffered DMA pipeline.
 *
 * @see pipeline_transfer
 */
void debug_write_dma(void)
{
    uint8_t payload_bytes[ADDRESS_SIZE * 3] = {0};

    get_payload(payload_bytes, sizeof(payload_bytes), &dbg);

    reset_frame(&dbg);

    uint32_t src_addr = bytes_to_uint32(payload_bytes);
    uint32_t dst_addr = bytes_to_uint32(payload_bytes + ADDRESS_SIZE);
    uint32_t total = bytes_to_uint32(payload_bytes + TWO_ADDRESS_SIZE);

    LOG("\n\r ->Source address: 0x%8LX\n\r ->Destiny address: 0x%8LX\n\r ->Size: %Lu "
        "Bytes\n",
        src_addr, dst_addr, total);

    pipeline_transfer(src_addr, dst_addr, total);
}

/**
 * @brief Extract and display EXIF UserComment for every stored image.
 *
 * @details Walks the data-state slots looking for
 *          @ref DATA_STATE_FULL_DATA entries. For each, resolves the
 *          index entry, computes the JPEG size, and calls
 *          @ref metadata_extract_user_comment to pull the UserComment
 *          string.
 *
 * @see metadata_extract_user_comment, flash_get_entries
 */
void debug_metadata(void)
{
    reset_frame(&dbg);

    Entry entry = {0, 0};
    uint16_t count = 0;
    int16_t meta_status;
    uint32_t jpeg_size;
    char comment[8] = {0};  // Magic number.

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState slot_state = flash_get_state(slot);

        if (slot_state == DATA_STATE_FULL_DATA)
        {
            LOG("\n\r -> Image %lu metadata: ", ++count);

            flash_get_entries(data_state_to_index_entry(slot), &entry);

            if (entry.end < entry.start)
            {
                LOG("\n\r <?> Invalid entry bounds.");
                continue;
            }

            jpeg_size = entry.end - entry.start + 1U;
            meta_status = metadata_extract_user_comment(entry.start, jpeg_size, comment);

            if (meta_status > 0)
            {
                LOG("\n\r \"%s\"", comment);
            }
            else if (meta_status == 0)
            {
                LOG("\n\r <?> UserComment not found.");
            }
            else
            {
                LOG("\n\r <?> Metadata parse/read error.");
            }

            continue;
        }

        if (slot_state == DATA_STATE_UNTOUCHED)
        {
            break;
        }
    }

    LOG("\n\r <!> Image metadata reading finished.\n");
}

/* @todo Count tombstones, maybe at boot. */

/**
 * @brief Tiered erase test: extract address range and run the full
 *        five-phase erase + index update.
 *
 * @details Parses a (head, tail) address pair from the payload, adjusts
 *          boundaries via @ref deletion_bounds_adjustment, erases via
 *          @ref erase_data, and updates the index via
 *          @ref update_index_after_erase.
 *
 * @see erase_data, deletion_bounds_adjustment, update_index_after_erase
 */
void debug_delete(void)
{
    EraseContext ctx = {0, 0, false};

    get_uint32_pair_payload(ctx.head_address, ctx.tail_address, dbg.buffer);

    reset_frame(&dbg);

    LOG("\n\r ->Addresses received:"
        " [ 0x%8LX - 0x%8LX ]"
        "\n\r ->Size: 0x%8LX (%Lu) Bytes",
        ctx.head_address, ctx.tail_address, ctx.tail_address - ctx.head_address,
        ctx.tail_address - ctx.tail_address);

    deletion_bounds_adjustment(&ctx);

    erase_data(&ctx);

    if (!ctx.success)
    {
        LOG("\n\r <?> Error, it didn't erase anything.");
        return;
    }

    update_index_after_erase(&ctx);
}

/**
 * @brief Manual single-sector erase: pick any opcode and address.
 *
 * @details Parses a 5-byte payload: erase command byte (1) + address
 *          (4). Dispatches to @ref flash_erase with the matching
 *          timeout for the requested sector size.
 *
 * @note Magic numbers don't matter it's just debugging 👀.
 *
 * @warning @ref DELETE_64M nukes an entire die. Careful with that one,
 *          buddy.
 *
 * @todo crc_helper needs to explicit the values from @ref DeletionKind_e when crafting a
 *       frame.
 *
 * @see flash_erase
 */
void erase_manual(void)
{
    uint8_t payload[ADDRESS_SIZE + 1] = {0};
    get_payload(payload, sizeof(payload), &dbg);

    LOG("\n\r -> Erase command: 0x%02X\n\r -> Address: 0x%8LX", payload[0],
        bytes_to_uint32(payload + 1));

    reset_frame(&dbg);

    switch ((DeletionKind)payload[0])
    {
    case DELETE_4K:
        flash_erase(bytes_to_uint32(payload + 1), (DeletionKind)payload[0], 22223U);
        break;

    case DELETE_32K:
    case DELETE_64K:
        flash_erase(bytes_to_uint32(payload + 1), (DeletionKind)payload[0], 55556U);
        break;

    case DELETE_64M: /* Careful with this one, buddy. */
        flash_erase(bytes_to_uint32(payload + 1), (DeletionKind)payload[0], 0xFFFFU);
        break;

    default:
        LOG("\n\rYo, what'bout sending a valid deletion size, dawg.");
        break;
    }
}

/* ──────────── Private helpers ──────────── */

/**
 * @brief Print a visual separator line to the debug terminal.
 */
#inline
static void print_separator_line(void)
{
    LOG("\n\r----------------------------------------------------\n\r"
        "----------------------------------------------------\n\r");
}

/**
 * @brief Read a flash region page-by-page and print to the terminal.
 *
 * @details Reads in @ref PAGE_SIZE chunks, printing each to the debug
 *          channel. Handles a remainder partial page at the tail.
 *
 * @param[in] addr_1st  Starting flash address.
 * @param[in] size      Total bytes to read and print.
 */
static void read_and_print(uint32_t addr_1st, uint32_t size)
{
    static uint8_t buffer[PAGE_SIZE] = {0};

    print_separator_line();

    if (size < PAGE_SIZE) /* Just one read. */
    {
        flash_read(addr_1st, buffer, size);
        send_bytes(buffer, size, SELECT_DBG);
        print_separator_line();
        return;
    }

    uint32_t current = addr_1st;

    do
    {
        flash_read(current, buffer, PAGE_SIZE);
        send_bytes(buffer, PAGE_SIZE, SELECT_DBG);

        current += PAGE_SIZE;
        size -= PAGE_SIZE;
    } while (size >= PAGE_SIZE);

    if (size) /* Remainder. */
    {
        flash_read(current, buffer, size);
        send_bytes(buffer, size, SELECT_DBG);
    }

    print_separator_line();
}

/**
 * @brief Fill a buffer with synthetic incrementing data for write tests.
 *
 * @details In production, this would be replaced by a @ref flash_read
 *          call (LFM source → OBC target). For debug, it generates a
 *          simple 0x00..0xFF ramp.
 *
 * @param[out] data  Destination buffer.
 * @param[in]  size  Number of bytes to fill.
 */
static void get_data(uint8_t *data, uint16_t size)
{
    if (size == 0 || data == NULL)
    {
        return;
    }

    /* flash_read(*current_read, data, size);  // For production. */
    uint8_t aux = 0; /* Debug only. */

    do
    {
        *data++ = aux++;
    } while (--size);
}

/**
 * @brief Align the first CPU-blocking write to a page boundary.
 *
 * @param[in,out] current_addr  Write address — advanced past the
 *                              partial page.
 * @param[in]     buffer        Scratch buffer (at least @ref PAGE_SIZE).
 * @param[in,out] remaining     Bytes left — decremented by the partial
 *                              page size.
 *
 * @pre Caller must validate @a current_addr and @a remaining.
 */
static void page_alignment_dbg(uint32_t *current_addr, uint8_t *buffer,
                               uint32_t *remaining)
{
    uint16_t offset = (uint16_t)(*current_addr % PAGE_SIZE);

    if (offset == 0)
    {
        return;
    }

    uint16_t space_in_page = PAGE_SIZE - offset;
    uint16_t chunk = (*remaining > space_in_page) ? space_in_page : (uint16_t)*remaining;

    get_data(buffer, chunk);
    flash_write(*current_addr, buffer, chunk);

    *current_addr += chunk;
    *remaining -= chunk;
}

/**
 * @brief CPU-blocking write test using synthetic data.
 *
 * @details Handles page alignment, full pages, and a tail partial
 *          page — all using @ref flash_write (blocking).
 *
 * @param[in] addr_1st    Starting flash write address.
 * @param[in] total_size  Total bytes to write.
 *
 * @pre Target region must fall within @ref DATA_1ST .. @ref DATA_END.
 *
 * @see page_alignment_dbg, get_data
 */
static void write_data(uint32_t addr_1st, uint32_t total_size)
{
    if (addr_1st < DATA_1ST || (addr_1st + total_size - 1U) > DATA_END)
    {
        LOG("\n\r<?> Forbidden, bro.\n");
        return;
    }

    static uint8_t buffer[PAGE_SIZE];
    uint32_t current_addr = addr_1st;
    uint32_t remaining = total_size;

    /* Alignment (first partial page). */
    page_alignment_dbg(&current_addr, buffer, &remaining);

    /* Full pages. */
    while (remaining >= PAGE_SIZE)
    {
        get_data(buffer, PAGE_SIZE);
        flash_write(current_addr, buffer, PAGE_SIZE);

        current_addr += PAGE_SIZE;
        remaining -= PAGE_SIZE;
    }

    /* Tail (final partial page). */
    if (remaining > 0)
    {
        get_data(buffer, (uint16_t)remaining);
        flash_write(current_addr, buffer, (uint16_t)remaining);
    }

    LOG("\n\n\r <!> Write completed.\n");
}
#endif /* DEBUG_MODE */
