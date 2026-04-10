/**
 * @file flash.c
 *
 * @brief Low-level MCU-to-flash SPI interface for both Local Flash
 *        Memory (LFM/SPI2) and OBC Flash Memory (OBCFM/SPI1).
 *
 * @details Implements all read, write, erase, and status operations
 *          against external SPI NOR flash. Includes a non-blocking
 *          Timer5-driven WIP polling mechanism for LFM writes and a
 *          DMA TX path for OBC flash writes. The MUX is toggled
 *          per-transaction to share the LFM bus with the RPi.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 *
 * @ingroup driver_flash
 */
#include "globals.h"

#include "../../include/drivers/pin_map.h"
#include "../../include/drivers/flash.h"
#include "../../include/flash/flash_types.h"
#include "../../include/protocol/endian.h"
#include "../../include/protocol/conversions.h"

/* ─────────────────────  Internal configuration (do not export)  ───────────────────── */

/** SPI-DMA Register Mappings (PIC18F67J94, Section 20). */
#byte DMACON1 = 0xF60
#byte DMACON2 = 0xF00
#byte TXADDRL = 0xF70
#byte TXADDRH = 0xF6F
#byte RXADDRL = 0xF6E
#byte RXADDRH = 0xF6D
#byte DMABCL = 0xF6C
#byte DMABCH = 0xF6B
#byte SSP1BUF = 0xFC9

/** DMACON1 individual bits. */
#bit DMAEN = DMACON1.0     // 1 = start; HW clears when done
#bit DLYINTEN = DMACON1.1  // 0 = delay interrupt disabled
#bit DUPLEX0 = DMACON1.2
#bit DUPLEX1 = DMACON1.3  // DUPLEX1:0 → 01=TX only, 00=RX only, 10=Full
#bit RXINC = DMACON1.4    // 1 = auto-increment RX address
#bit TXINC = DMACON1.5    // 1 = auto-increment TX address
#bit SSCON0 = DMACON1.6
#bit SSCON1 = DMACON1.7  // SSCON1:0 -> 00 = DMA does NOT drive SSDMA pin

/** Timer5 reload value: interrupts every ~18 us until WIP clears. */
#define WIP_TICK_RATE 65248U /**< 65248 @ 16 MHz. 65460 @ 4 MHz. */

/**
 * @enum FlashCommand_e
 * @brief SPI opcodes for flash read/write/status operations.
 */
typedef enum FlashCommand_e
{
    READ_4B = 0x13,           ///< Read in 4-byte address mode.
    PROGRAM_4B = 0x12,        ///< Page program in 4-byte address mode.
    WRITE_EN = 0x06,          ///< Write enable latch.
    WRITE_DIS = 0x04,         ///< Write disable latch.
    FOUR_BYTES_MODE = 0xB7,   ///< Enter 4-byte address mode.
    THREE_BYTES_MODE = 0xE9,  ///< Enter 3-byte address mode.
    READ_STATUS_REG1 = 0x05,  ///< Read status register 1.
    ERASE_4KB = 0x20,         ///< Erase a 4 KiB sector.
    ERASE_32KB = 0x52,        ///< Erase a 32 KiB sector.
    ERASE_64KB = 0xD8,        ///< Erase a 64 KiB sector.
    ERASE_DIE = 0xC4,         ///< Erase an entire die. Device is double-die.
    DUMMY = 0x00              ///< Dummy byte for SPI clocking.
} FlashCommand;

/**
 * @enum WriteParameters_e
 * @brief WIP polling timing constants.
 */
typedef enum WriteParameters_e
{
    FLASH_WIP_BIT = 0x01,   ///< Status Reg1, bit 0.
    WIP_POLLING_RATE = 18,  ///< Polling interval in us.
    PAGE_MAX_RATIO = 100    ///< 18 us * 100 = 1800 us max page program.
} WriteParameters;

/**
 * @struct WipParameters_s
 * @brief State for the Timer5-driven non-blocking WIP poller.
 *
 * @invariant @c is_active is set by foreground code and cleared either
 *            by @ref poll_wip (WIP bit clear) or by the Timer5 ISR
 *            (timeout).
 */
typedef struct WipParameters_s
{
    volatile bool is_active;       ///< WIP lock currently held.
    volatile uint16_t tick_count;  ///< Timer5 ticks since lock started.
    uint16_t timeout;              ///< Max ticks before forced release.
} WipParameters;

static WipParameters wip;

/**
 * @brief Flash bus ownership -- private to this file.
 *
 * @see claim_flash_for_rpi, release_flash, is_flash_available
 */
static FlashOwner flash_holder = FLASH_FREE;

/** @brief DMA TX completion flag -- set by SSP1 ISR, polled by pipeline. */
static volatile bool dma_tx_complete = false;

/* Forward declarations. */
#inline
static void mux_to_mcu(void);
#inline
static void mux_to_rpi(void);
#inline
static void disable_mux(void);
#inline
static void send_address(uint32_t &address);
#inline
static void obc_send_address(uint32_t &address);
#inline
static void send_command(FlashCommand single_command);
#inline
static void obc_send_command(FlashCommand single_command);
#inline
static void entries_to_buffer(uint8_t *entry_bytes);
#inline
static void poll_wip(void);
#inline
static void init_wip_lock(uint16_t current_timeout);
#inline
static FlashCommand get_erase_command(DeletionKind kind);
static void wait_operation(uint32_t timeout_us); /**< @deprecated Busy-wait poller. */
static void dma_spi_write(uint8_t *data, uint16_t size);
#inline
static void assert_spi2(void);
#inline
static void deassert_spi2(void);
#inline
static void assert_spi(void);
#inline
static void deassert_spi(void);

/* ════════════════════════════════════════════════════════════════════════════════════ */
/*                                        Public                                        */
/* ════════════════════════════════════════════════════════════════════════════════════ */

/* ─────────────────────────────────────  Setup  ────────────────────────────────────── */

/**
 * @brief Configure both flash chips into 4-byte address mode.
 */
void setup_flash(void)
{
    send_command(FOUR_BYTES_MODE);
    spi_speed(LFM, 16000000);  // Probably 4000000 for production.
    spi_speed(OBCFM, 16000000);
}

/* ───────────────────────────  Local Flash Memory - SPI2  ──────────────────────────── */

/**
 * @brief Read the @ref DataState_e byte for a given state-map slot.
 *
 * @param[in] address  State-map address (0 .. @ref DATA_STAT_END).
 *
 * @return The @ref DataState_e value, or @ref DATA_STATE_ERROR if
 *         out of range.
 */
DataState flash_get_state(uint32_t address)
{
    if (address > DATA_STAT_END)
    {
        LOG("\n\rError when trying to get a state. Address out of range.");
        return DATA_STATE_ERROR;
    }

    uint8_t buffer = (uint8_t)DATA_STATE_ERROR;

    poll_wip();

    mux_to_mcu();

    assert_spi2();
    spi_write2(READ_4B);
    send_address(address);

    buffer = spi_xfer(LFM, DUMMY);

    deassert_spi2();

    disable_mux();

    return (DataState)buffer;
}

/**
 * @brief Read a start/end address pair from the index entry region.
 *
 * @param[in]  address  Index entry address
 *                      (@ref IDX_ENTRY_1ST .. @ref IDX_ENTRY_END).
 * @param[out] entry    Destination @ref Entry_s; set to
 *                      @ref INVALID_ADDR on range error.
 *
 * @warning It might need some fixing.
 */
void flash_get_entries(uint32_t address, Entry *entry)
{
    if (address < IDX_ENTRY_1ST || address > IDX_ENTRY_END)
    {
        LOG("\n\r Error trying to get an entry! Out of range.");
        entry->start = INVALID_ADDR;
        entry->end = INVALID_ADDR;
        return;
    }

    uint8_t entry_buffer[TWO_ADDRESS_SIZE] = {0};

    poll_wip();

    mux_to_mcu();

    assert_spi2();
    spi_write2(READ_4B);
    send_address(address);

    entries_to_buffer(entry_buffer);

    deassert_spi2();

    disable_mux();

    entry->start = bytes_to_uint32(entry_buffer);
    entry->end = bytes_to_uint32(entry_buffer + ADDRESS_SIZE);
}

/**
 * @brief Read bytes from LFM.
 *
 * @param[in]  address  Starting 4-byte LFM address.
 * @param[out] buffer   Destination buffer.
 * @param[in]  size     Number of bytes to read.
 *
 * @post CPU blocks until all bytes are clocked out.
 */
void flash_read(uint32_t address, uint8_t *buffer, uint16_t size)
{
    if (size == 0 || buffer == NULL)
    {
        LOG("\n\r <?> Flash reading failed (NULL).");
        return;
    }

    poll_wip();

    mux_to_mcu();

    assert_spi2();
    spi_write2(READ_4B);
    send_address(address);

    do
    {
        *buffer++ = spi_xfer(LFM, DUMMY);
    } while (--size);

    deassert_spi2();

    disable_mux();
}

/**
 * @brief Write (program) bytes to LFM (CPU-blocking).
 *
 * @param[in] address  Starting 4-byte LFM address.
 * @param[in] data     Source buffer.
 * @param[in] size     Bytes to write (1 .. @ref PAGE_SIZE).
 *
 * @pre Target region must be erased (all 0xFF).
 */
void flash_write(uint32_t address, uint8_t *data, uint16_t size)
{
    if (size == 0 || size > PAGE_SIZE || data == NULL)
    {
        LOG("\n\r <?> Flash write failed (NULL) or out of range.");
        return;
    }

    poll_wip();

    send_command(WRITE_EN);

    mux_to_mcu();

    assert_spi2();
    spi_write2(PROGRAM_4B);
    send_address(address);

    do
    {
        spi_write2(*data++);
    } while (--size);

    deassert_spi2();
    disable_mux();

    init_wip_lock((uint16_t)PAGE_MAX_RATIO);
}

/**
 * @brief Erase a flash sector starting at @p start.
 *
 * @param[in] start          Starting address (must be sector-aligned).
 * @param[in] erase_command  Sector granularity opcode.
 * @param[in] timeout_rate   WIP tick count limit for this erase size.
 *
 * @todo Change @p erase_command into a higher-level abstraction for
 *       better separation of concerns.
 */
void flash_erase(uint32_t start, DeletionKind kind, uint16_t timeout_rate)
{
    FlashCommand erase_command = get_erase_command(kind);

    poll_wip();

    send_command(WRITE_EN);  // Write enable to be able to erase.

    mux_to_mcu();

    assert_spi2();
    spi_write2(erase_command);
    send_address(start);

    deassert_spi2();
    disable_mux();

    init_wip_lock(timeout_rate);
}

/* ───────────────────────────  OBC Flash Memory -- SPI1  ──────────────────────────── */

/**
 * @brief Read bytes from OBC flash (SPI1).
 *
 * @param[in]  address  4-byte OBC flash address.
 * @param[out] data     Destination buffer.
 * @param[in]  size     Bytes to read.
 *
 * @warning Does not check or block on WIP -- caller must ensure OBC
 *          flash is idle.
 */
void obc_flash_read(uint32_t address, uint8_t *data, uint16_t size)
{
    if (size == 0 || data == NULL)
    {
        LOG("\n\r <?> OBC Flash reading failed (NULL).");
        return;
    }

    assert_spi();
    spi_write(READ_4B);
    obc_send_address(address);

    do
    {
        *data++ = spi_xfer(OBCFM, DUMMY);
    } while (--size);

    deassert_spi();
}

/**
 * @brief DMA-driven, non-CPU-blocking OBC flash write (SPI1).
 *
 * @param[in] address  4-byte OBC flash destination.
 * @param[in] data     SRAM source buffer.
 * @param[in] size     Bytes to write (1 .. @ref PAGE_SIZE).
 *
 * @note This MCU doesn't control a MUX on the OBC unit.
 * @note WIP locking is handled externally by the pipeline.
 *
 * @see pipeline_transfer
 */
void obc_flash_write_dma(uint32_t address, uint8_t *data, uint16_t size)
{
    if (size == 0 || data == NULL || size > PAGE_SIZE)
    {
        LOG("\n\r <?> OBC Flash write failed (NULL).");
        return;
    }

    obc_send_command(WRITE_EN);

    assert_spi();
    spi_write(PROGRAM_4B);
    obc_send_address(address);

    dma_spi_write(data, size);
}

/**
 * @brief Blocking OBC flash write (non-DMA).
 *
 * @param[in] address  4-byte OBC flash destination.
 * @param[in] data     Source buffer.
 * @param[in] size     Bytes to write.
 *
 * @deprecated No WIP locking -- use @ref obc_flash_write_dma instead.
 */
void obc_flash_write(uint32_t address, uint8_t *data, uint16_t size)
{
    if (size == 0 || data == NULL)
    {
        LOG("\n\r <?> OBC Flash write failed (NULL).");
        return;
    }

    obc_send_command(WRITE_EN);

    assert_spi();
    spi_write(PROGRAM_4B);
    obc_send_address(address);

    do
    {
        spi_write(*data++);
    } while (--size);

    deassert_spi();

    // WARNING: Deprecated
    // wait_operation(PAGE_MAX_TIME, FLASH_SELECT_OBC);  // Writing delay (Program).
}

/**
 * @brief Poll OBC flash status register 1 for the WIP bit.
 *
 * @return @c true if WIP bit is 1 (write in progress), @c false
 *         otherwise.
 *
 * @warning do not fucking forget to get rid of debug features.
 */
bool is_obc_wip(void)
{
    mux_to_rpi();  // Just to DEBUG with the same local flash.

    uint8_t status = 0xFF;

    assert_spi();
    spi_write(READ_STATUS_REG1);
    status = spi_xfer(OBCFM, DUMMY);
    deassert_spi();

    disable_mux();  // DEBUG ONLY.

    return (status & ((uint8_t)FLASH_WIP_BIT)) == 1;
}

/* ────────────────────────────────  Flash ownership  ───────────────────────────────── */

/** @brief Proactively lock flash for RPi before handing the address. */
void claim_flash_for_rpi(void)
{
    mux_to_rpi();
    flash_holder = FLASH_RPI;
}

/** @brief Release flash back to MCU after RPi write lifecycle ends. */
void release_flash(void)
{
    disable_mux();
    flash_holder = FLASH_FREE;
}

/** @brief Decline flash -- MUX off, no ownership was claimed. */
void decline_flash(void)
{
    disable_mux();
}

/** @brief Check whether the flash bus is available for MCU operations. */
bool is_flash_available(void)
{
    return flash_holder == FLASH_FREE;
}

#ifdef DEBUG_MODE
/** @brief Force MUX to RPi channel for single-flash debug setups. */
void debug_mux_to_rpi(void)
{
    mux_to_rpi();
}
#endif /* DEBUG_MODE */

/* ──────────────────────────────────────  DMA  ─────────────────────────────────────── */

/**
 * @brief Check whether SPI2 (LFM) is safe to use.
 *
 * @details SPI2 is blocked only while DMAEN=1 (SSP2BUF hijacked by
 *          the DMA engine).
 *
 * @return @c true if DMAEN is cleared and SPI2 is available.
 */
bool is_spi2_free(void)
{
    return !DMAEN;
}

/** @brief Check whether the last DMA TX has completed. */
bool is_dma_tx_complete(void)
{
    return dma_tx_complete;
}

/** @brief Clear the DMA TX completion flag after handling. */
void clear_dma_tx_complete(void)
{
    dma_tx_complete = false;
}

/* ════════════════════════════════════════════════════════════════════════════════════ */
/*                                        Private                                       */
/* ════════════════════════════════════════════════════════════════════════════════════ */

/* ────────────────────────────────  SPI assertion  ──────────────────────────────── */

/**
 * @brief Assert !CE2 -- begin SPI2 transaction (LFM).
 *
 * @pre @ref spi_bus_config "SPI MODE=0": idle-low clock, active-low CS.
 */
#inline
static void assert_spi2(void)
{
    output_low(SS2);
}

/**
 * @brief Deassert !CE2 -- finalise SPI2 transaction (LFM).
 *
 * @pre @ref spi_bus_config "SPI MODE=0": idle-low clock, active-low CS.
 */
#inline
static void deassert_spi2(void)
{
    output_high(SS2);
}

/**
 * @brief Assert !CE1 -- begin SPI1 transaction (OBCFM).
 *
 * @pre @ref spi_bus_config "SPI MODE=0": idle-low clock, active-low CS.
 */
#inline
static void assert_spi(void)
{
    output_low(SS1);
}

/**
 * @brief Deassert !CE1 -- finalise SPI1 transaction (OBCFM).
 *
 * @pre @ref spi_bus_config "SPI MODE=0": idle-low clock, active-low CS.
 */
#inline
static void deassert_spi(void)
{
    output_high(SS1);
}

/* ────────────────────────────────  MUX pin control  ───────────────────────────────── */

/**
 * @brief Switch the MUX to MCU <-> Flash path.
 *
 * @details MUX_EN LOW = activated, MUX_S LOW = MCU channel.
 */
#inline
static void mux_to_mcu(void)
{
    output_low(MUX_EN);
    output_low(MUX_S);
    delay_us(1);  // Confirm later if with a NOP is enough delay.
}

/**
 * @brief Switch the MUX to RPi <-> Flash path.
 *
 * @details MUX_EN LOW = activated, MUX_S HIGH = RPi channel.
 */
#inline
static void mux_to_rpi(void)
{
    output_low(MUX_EN);
    output_high(MUX_S);
    delay_us(1);  // NOP? Should be enough... I hope.
}

/**
 * @brief Deactivate the MUX -- no flash communication allowed.
 *
 * @details MUX_EN HIGH = deactivated. This is a fast-inlined low-level function not meant
 * to be seen externally.
 */
#inline
static void disable_mux(void)
{
    delay_us(1);  // NOP; same as delay_cycles(1); but without a pesky magic numbah.
    output_high(MUX_EN);
}

/* ────────────────────────────────  SPI bus helpers  ───────────────────────────────── */

/**
 * @brief Send a 4-byte address MSB-first over SPI2 (LFM).
 *
 * @param[in] address 32-bit flash address.
 *
 * @note Portable (slower on this particular hardware) alternative:
 * @code{.c}
 *  uint8_t byte = 3;
 *  do { spi_write2((uint8_t)(address >> (8 * byte))); } while (byte--);
 * @endcode
 */
#inline
static void send_address(uint32_t &address)
{
    spi_write2(make8(address, BIG_END_MSB));
    spi_write2(make8(address, BIG_END_B_1));
    spi_write2(make8(address, BIG_END_B_2));
    spi_write2(make8(address, BIG_END_LSB));
}

/**
 * @brief Send a 4-byte address MSB-first over SPI1 (OBCFM).
 *
 * @param[in] address  32-bit flash address.
 *
 * @note Portable (slower) alternative:
 * @code{.c}
 *  for (uint8_t i = 3; i >= 0; --i)
 *      spi_write((uint8_t)(address >> (8 * i)));
 * @endcode
 */
#inline
static void obc_send_address(uint32_t &address)
{
    spi_write(make8(address, BIG_END_MSB));
    spi_write(make8(address, BIG_END_B_1));
    spi_write(make8(address, BIG_END_B_2));
    spi_write(make8(address, BIG_END_LSB));
}

/**
 * @brief Send a single-byte command to LFM (no response expected).
 *
 * @param[in] single_command  @ref FlashCommand_e opcode.
 */
#inline
static void send_command(FlashCommand single_command)
{
    mux_to_mcu();

    assert_spi2();
    spi_write2(single_command);
    deassert_spi2();

    disable_mux();
}

/**
 * @brief Send a single-byte command to OBCFM (no response expected).
 *
 * @param[in] single_command  @ref FlashCommand_e opcode.
 */
#inline
static void obc_send_command(FlashCommand single_command)
{
    assert_spi();
    spi_write(single_command);
    deassert_spi();
}

/**
 * @brief Clock 8 bytes (head + tail address) out of SPI2 into a buffer.
 *
 * @param[out] entry_bytes  Destination buffer (>= 8 bytes).
 */
#inline
static void entries_to_buffer(uint8_t *entry_bytes)
{
    *entry_bytes++ = spi_xfer(LFM, DUMMY);  // HEAD MSB.
    *entry_bytes++ = spi_xfer(LFM, DUMMY);
    *entry_bytes++ = spi_xfer(LFM, DUMMY);
    *entry_bytes++ = spi_xfer(LFM, DUMMY);  // HEAD LSB.
    *entry_bytes++ = spi_xfer(LFM, DUMMY);  // TAIL MSB.
    *entry_bytes++ = spi_xfer(LFM, DUMMY);
    *entry_bytes++ = spi_xfer(LFM, DUMMY);
    *entry_bytes = spi_xfer(LFM, DUMMY);  // TAIL LSB.
}

/* ─────────────────────────────────  WIP management  ───────────────────────────────── */

/**
 * @brief Read LFM status register 1 raw.
 *
 * @return Raw status register byte.
 */
static uint8_t get_status_register(void)
{
    uint8_t status = 0xFF;

    assert_spi2();
    spi_write2(READ_STATUS_REG1);
    status = spi_xfer(LFM, DUMMY);
    deassert_spi2();

    return status;
}

/**
 * @brief Read WIP bit from LFM status register.
 *
 * @param[out] status  Raw status register 1 value.
 *
 * @post If WIP bit is cleared, @c wip.is_active is set to @c false.
 */
#inline
static void is_flash_wip(uint8_t &status)
{
    mux_to_mcu();

    assert_spi2();
    spi_write2(READ_STATUS_REG1);
    status = spi_xfer(LFM, DUMMY);
    deassert_spi2();

    disable_mux();

    if (!(status & ((uint8_t)FLASH_WIP_BIT)))
    {  // Avoid racing Timer5-watchdog-ISR ; only modify if WIP bit is truly cleared.
        wip.is_active = false;
    }
}

/**
 * @brief Block until any pending LFM write completes.
 *
 * @details Spins on @ref is_flash_wip with @ref WIP_POLLING_RATE
 *          delays until @c wip.is_active is cleared (by either the
 *          foreground read or the Timer5 timeout).
 */
#inline
static void poll_wip(void)
{
    uint8_t status = 0xFF;

    while (wip.is_active)
    {
        is_flash_wip(status);
        delay_us((uint8_t)WIP_POLLING_RATE);
    }
}

/**
 * @brief Arm the Timer5 WIP lock for a given tick count.
 *
 * @param[in] current_timeout  Max Timer5 ticks before forced release.
 *
 * @post Timer5 interrupt is enabled; @c wip.is_active == @c true.
 */
#inline
static void init_wip_lock(uint16_t current_timeout)
{
    wip.timeout = current_timeout;
    wip.is_active = true;
    wip.tick_count = 0;
    set_timer5(WIP_TICK_RATE);
    enable_interrupts(INT_TIMER5);
}

/**
 * @brief Some brief.
 *
 * @details Some details.
 *
 * @param[in] kind.
 * @param[out] idk.
 *
 * @todo return DUMMY is still dangerous, it'll still send address later in @ref
 *       flash_erase.
 */
#inline
static FlashCommand get_erase_command(DeletionKind kind)
{
    switch (kind)
    {
    case DELETE_4K:
        return ERASE_4KB;
    case DELETE_32K:
        return ERASE_32KB;
    case DELETE_64K:
        return ERASE_64KB;
    case DELETE_64M:
        return ERASE_DIE;
    default:
        return DUMMY;  // If `kind` gets SEU'd you better don't delete shit, buddy.
    }
}

/**
 * @brief Busy-wait WIP poller with timeout.
 *
 * @details Flash timing reference (from datasheet):
 *
 * | Operation              | Typical        | Max    | Unit |
 * |------------------------|----------------|--------|------|
 * | Page program (256 B)   | 120            | 1800   | us   |
 * | Page program (n B)     | 18 + 2.5*(n/6) | 1800   | us   |
 * | 64 KiB sector erase    | 0.15           | 1      | s    |
 * | 4 KiB subsector erase  | 50             | 400    | ms   |
 * | 32 KiB subsector erase | 0.1            | 1      | s    |
 *
 * @param[in] timeout_us  Maximum wait time in microseconds.
 *
 * @deprecated Use @ref init_wip_lock + @ref poll_wip (Timer5-driven)
 *             instead.
 */
static void wait_operation(uint32_t timeout_us)
{
    uint32_t elapsed = 0;

    do
    {
        if (!(get_status_register() & FLASH_WIP_BIT))
        {
            LOG("\n\r ->Done. Time elapsed: ~%Lu us\n", elapsed);
            return;
        }

        if (elapsed >= timeout_us)
        {
            LOG("\n\r <!> Flash timeout, elapsed: ~%Lu us\n", elapsed);
            return;
        }
        elapsed += WIP_POLLING_RATE;

        delay_us((uint16_t)WIP_POLLING_RATE);
    } while (true);
}

/* ──────────────────────────────────────  DMA  ─────────────────────────────────────── */

/**
 * @brief DMA TX-only write: RAM buffer -> SPI1 (half-duplex).
 *
 * @param[in] data  Source buffer in RAM.
 * @param[in] size  Bytes to transmit.
 *
 * @pre !CE1 (@ref SS1) must be asserted before calling.
 *
 * @post DMAEN is set; the SSP ISR will deassert !CE1 when done.
 */
static void dma_spi_write(uint8_t *data, uint16_t size)
{
    // Load source address. From RAM to DMA's registah.
    TXADDRH = (uint8_t)((uint16_t)data >> BITS_PER_BYTE);
    TXADDRL = (uint8_t)((uint16_t)data);

    // Load byte count (size - 1), DMABC register is base zero.
    uint16_t dma_count = size - 1;
    DMABCH = (uint8_t)(dma_count >> BITS_PER_BYTE);
    DMABCL = (uint8_t)(dma_count);

    // DUPLEX=01 (TX only), TXINC=1, RXINC=0, SSCON=00, DLYINTEN=0
    DMACON1 = 0b00100100;

    DMAEN = 1;                   // Start — DMAEN will be hardware-cleared when done.
    clear_interrupt(INT_SSP);    // Clear stale SSPIF from address/flush phase.
    enable_interrupts(INT_SSP);  // When its interrupt kicks in yo, it deasserts !CE1
}

/* ──────────────────────────────────────  ISR  ─────────────────────────────────────── */

/**
 * @brief Timer5 ISR: Raise a periodic poll flag during flash internal write phase. SPI
 *        is polled in foreground to avoid SPI re-entrancy from ISR context.
 *
 * @note Configure for @ref WIP_TICK_RATE period before starting
 *       an erase or write operation.
 */
#int_timer5
void wip_bit_watchdog(void)
{
    if (!wip.is_active)
    {  // Writing process done. Then, wip-bit-watchdogging's down.
        disable_interrupts(INT_TIMER5);
        return;
    }

    if (++wip.tick_count >= wip.timeout)
    {  // Time's out. WIP state MUST be cleared.
        disable_interrupts(INT_TIMER5);
        wip.is_active = false;
        return;
    }

    set_timer5(WIP_TICK_RATE);  // Keep wip-bit-watchdogging.
}

/**
 * @brief SSP1 DMA-complete ISR: deassert !CE1 and signal completion.
 *
 * @details When DMAEN is hardware-cleared (transfer finished), this ISR
 *          deasserts chip select and sets @ref dma_tx_complete for the
 *          foreground pipeline to pick up. All state machine logic stays
 *          in @ref image_transfer.c.
 *
 * @see is_dma_tx_complete, clear_dma_tx_complete
 */
#int_ssp
void ssp1_dma_isr(void)
{
    if (is_spi2_free())
    {
        delay_cycles(2);
        deassert_spi();
        dma_tx_complete = true;
        disable_interrupts(INT_SSP);
    }
}
