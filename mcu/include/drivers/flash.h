/**
 * @file flash.h
 *
 * @brief Low-level MCU-to-flash SPI interface for both Local Flash
 *        Memory (LFM) and OBC Flash Memory (OBCFM).
 *
 * @details Provides read, write, and erase operations over two independent
 *          SPI buses: SPI2 for the on-board LFM and SPI1 for the OBC-side
 *          flash. Includes DMA and non-blocking write paths, MUX control,
 *          and a flash data-state tombstone system.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 */
#ifndef FLASH_INTERFACE_H
#define FLASH_INTERFACE_H

/**
 * @defgroup driver_flash Flash Driver
 * @brief Low-level SPI flash read/write/erase for LFM and OBCFM.
 *
 * @see @ref globals_flash for flash geometry and section map constants.
 * @see image_transfer.c for the DMA pipeline built on this driver.
 * @{
 */

/**
 * @enum DataState_e
 * @brief Tombstone states for flash data lifecycle management.
 *
 * @details Flash can only clear bits (1 -> 0) without a full sector
 *          erase, so the main lifecycle values are chosen to allow
 *          progressive state transitions by overwriting the same byte:
 *
 *          `0xFF` -> `0x7E` -> `0x7C` / `0x3E` -> `0x3C` -> `0x00`
 *
 *          The special values (@ref DATA_STATE_IDX_COMPACT and
 *          @ref DATA_STATE_ERROR) live outside this progression.
 *
 * @see flash_get_state
 */
typedef enum DataState_e
{
    DATA_STATE_DELETED = 0x00,       ///< Full tombstone -- dead data.
    DATA_STATE_SHRUNKEN = 0x3C,      ///< Both head and tail are garbage.
    DATA_STATE_TAIL_GARBAGE = 0x3E,  ///< Head valid, tail is garbage.
    DATA_STATE_HEAD_GARBAGE = 0x7C,  ///< Tail valid, head is garbage.
    DATA_STATE_FULL_DATA = 0x7E,     ///< Fully valid data.
    DATA_STATE_UNTOUCHED = 0xFF,     /**< Erased / never written. Serves
                                          as a scan-stop sentinel. */
    DATA_STATE_IDX_COMPACT = 0x55,   ///< Index entry compaction garbage.
    DATA_STATE_ERROR = 0x0F          /**< Value doesn't match any known state
                                          -- something went wrong. */
} DataState;

/**
 * @enum FlashOwner_e
 * @brief Flash ownership flag
 */
typedef enum FlashOwner_e
{  // true and false underneath an enum sugarcoat while still keeping it fast as fuck.
    FLASH_FREE,  ///< Free for MCU.
    FLASH_RPI    ///< Raspberry borrows the flash to transfer image data.
} FlashOwner;

/**
 * @enum FlashStream_e
 * @brief SPI stream selector for flash operations.
 *
 * @deprecated No longer used. Slated for removal.
 */
typedef enum FlashStream_e
{
    FLASH_SELECT_OBC = 1,  ///< SPI1 -- OBC Flash Memory (OBCFM).
    FLASH_STREAM_LFM = 2   ///< SPI2 -- Local Flash Memory (LFM).
} FlashStream;

/**
 * @struct Entry_s
 * @brief Start/end address pair delimiting one data entry in flash.
 *
 * @see flash_get_entries
 */
typedef struct Entry_s
{
    uint32_t start;  ///< First byte address of the entry.
    uint32_t end;    ///< Last byte address of the entry.
} Entry;

/** @name Local Flash Memory (LFM) -- SPI2
 * @{
 */
void flash_read(uint32_t address, uint8_t *buffer, uint16_t size);
void flash_write(uint32_t address, uint8_t *data, uint16_t size);
void flash_erase(uint32_t start, DeletionKind kind, uint16_t timeout_rate);
DataState flash_get_state(uint32_t address);
void flash_get_entries(uint32_t address, Entry *entry);
/** @} */

/** @name OBC Flash Memory (OBCFM) -- SPI1
 * @{
 */
void obc_flash_read(uint32_t raw_address, uint8_t *data, uint16_t size);
void obc_flash_write(uint32_t raw_address, uint8_t *data, uint16_t size);

/** @brief DMA-powered write to OBC flash (SPI2 blocked while active). */
void obc_flash_write_dma(uint32_t address, uint8_t *data, uint16_t size);

/** @brief Poll OBC flash status register 1 for the Write-In-Progress bit. */
bool is_obc_wip(void);
/** @} */

/** @name MUX & Bus Utilities
 * @{
 */
bool is_spi2_free(void);
void setup_flash(void);
/** @} */

/** @name DMA Completion
 * @{
 */
bool is_dma_tx_complete(void);
void clear_dma_tx_complete(void);
/** @} */

/** @name Flash Ownership
 * @{
 */
void claim_flash_for_rpi(void);
void release_flash(void);
void decline_flash(void);
bool is_flash_available(void);
#ifdef DEBUG_MODE
void debug_mux_to_rpi(void);
#endif /* DEBUG_MODE */
/** @} */

/** @} */ /* driver_flash */

#endif /* FLASH_INTERFACE_H */
