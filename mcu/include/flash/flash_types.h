/**
 * @file flash_types.h
 *
 * @brief Flash geometry, memory map, and erase opcodes for the
 *        external flash.
 *
 * @see flash.c for the low-level SPI driver that consumes these.
 * @see index_manager.c for the indexing logic built on @ref FlashSection_e.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup globals_flash
 */
#ifndef FLASH_TYPES_H
#define FLASH_TYPES_H

/**
 * @defgroup globals_flash Flash Memory Types
 * @brief Geometry, memory map, and erase opcodes for the external flash.
 * @{
 */

#define TWO_ADDRESS_SIZE ((uint8_t)8)

/**
 * @def INVALID_ADDR
 * @brief Sentinel value for an invalid or uninitialized flash address.
 */
#define INVALID_ADDR 0xFFFFFFFFUL

/**
 * @enum FlashSize_e
 * @brief Commonly used flash geometry constants.
 */
typedef enum FlashSize_e
{
    ADDRESS_SIZE = 4,          ///< Bytes per flash address.
    PAGE_SIZE = 256U,          ///< Bytes per page (write unit).
    SECTOR_4K_SIZE = 4096U,    ///< 4 KB sector.
    SECTOR_32K_SIZE = 32768U,  ///< 32 KB sector.
    SECTOR_64K_SIZE = 65536UL  ///< 64 KB sector.
} FlashSize;

/**
 * @enum FlashSection_e
 * @brief Logical flash memory map -- boundary addresses for each section.
 *
 * @details The 128 MiB (double-die) flash is partitioned as follows:
 *
 * | Section          | Start        | End            | Purpose              |
 * |------------------|--------------|----------------|----------------------|
 * | DATA_STAT        | `0x0000`     | `0x01FF`       | Index map            |
 * | ADDR_PTR         | `0x0200`     | `0x0FFF`       | Address pointers     |
 * | IDX_ENTRY        | `0x1000`     | `0x1FFF`       | Index/root entries   |
 * | DATA             | `0x2000`     | `0x07FF_FFFF`  | Bulk data storage    |
 */
typedef enum FlashSection_e
{
    DATA_STAT_1ST = 0x00,     ///< Index map start.
    DATA_STAT_END = 0x01FFU,  ///< Index map end.
    ADDR_PTR_1ST = 0x0200U,   ///< Address pointer section begins.
    ADDR_PTR_END = 0x0FFFU,   ///< Address pointer section ends.
    IDX_ENTRY_1ST = 0x1000U,  ///< Index/root section begins.
    IDX_ENTRY_END = 0x1FFFU,  ///< Index/root section ends.
    DATA_1ST = 0x2000U,       ///< Data section begins.
    DATA_END = 0x07FFFFFFUL   ///< Data section ends.
} FlashSection;

/**
 * @enum DeletionKind_e
 * @brief SPI opcodes for the various erase granularities.
 * @todo This belongs to flash.c internal for a better separations
 * of concerns. If used externally to it, just figure an architechtural
 * strategy/pattern for it.
 */
typedef enum DeletionKind_e
{
    DELETE_4K,
    DELETE_32K,
    DELETE_64K,
    DELETE_64M  // Die deletion. Double-die device.
} DeletionKind;

/** @} */ /* globals_flash */

#endif /* FLASH_TYPES_H */
