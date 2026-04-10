/**
 * @file eraser.c
 *
 * @brief Tiered flash erase: automatic promotion and relegation across
 *        4 KiB, 32 KiB, and 64 KiB sector sizes.
 *
 * @details @ref erase_data orchestrates a five-phase erasure that
 *          maximises throughput by using the largest aligned sector
 *          size available at each step:
 *
 *          1. **4 KiB** until the address aligns to a 32 KiB boundary.
 *          2. **32 KiB** until the address aligns to a 64 KiB boundary.
 *          3. **64 KiB** bulk erase for the widest span.
 *          4. **32 KiB** cleanup (promotion disabled).
 *          5. **4 KiB** cleanup (promotion disabled).
 *
 *          Each phase is implemented by a single @ref erase_pass call
 *          with different @ref DeletionParams_s — no function pointers
 *          needed.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2024-11-04
 *
 * @version 4.0 (Refactored to remove function pointers)
 *
 * @ingroup storage
 */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/flash/eraser.h"
#include "../../include/drivers/flash.h"

/* ───────────────────────────────────────────── Internal configuration (do not export) */

/**
 * @def IS_LIMIT_REACHED(addr, size, last)
 * @brief True if erasing @a size bytes at @a addr would exceed @a last.
 */
#define IS_LIMIT_REACHED(addr, size, last) (((addr) + ((size) - 1)) > (last))

/**
 * @def IS_ALIGNED(addr, size)
 * @brief True if @a addr is aligned to @a size bytes.
 */
#define IS_ALIGNED(addr, size) (((addr) % (size)) == 0)

/**
 * @enum DeletionRate_e
 * @brief Timeout tick counts for each erase tier (based on 18 µs tick).
 */
typedef enum DeletionRate_e
{
    TIMEOUT_RATE_4KB = 22223U,   ///< ~0.4 s / 18 µs tick.
    TIMEOUT_RATE_32KB = 55556U,  ///< ~1.0 s / 18 µs tick.
    TIMEOUT_RATE_64KB = 55556U   ///< ~1.0 s / 18 µs tick.
} DeletionRate;

/**
 * @struct DeletionParams_s
 * @brief Parameters that drive a single erase-pass tier.
 *
 * @details Bundles sector size, SPI command, timeout, and the
 *          alignment boundary that triggers promotion to the next
 *          larger tier. Set @c next_boundary to 0 to disable
 *          promotion (cleanup phases).
 */
typedef struct DeletionParams_s
{
    FlashSize sector_size;   ///< Erase granularity: 4 KiB, 32 KiB, or 64 KiB.
    DeletionKind kind_size;  ///< SPI opcode for this erase size.
    DeletionRate timeout;    ///< Timeout for the erase operation.
    char *label;             ///< Human-readable tier label for debug logs.
    FlashSize next_boundary; /**< Alignment size that triggers promotion to the
                                  next tier. 0 disables promotion. */
} DeletionParams;

/** @brief Execute one erase tier until promotion or limit. */
static void erase_pass(uint32_t *current, uint32_t last, DeletionParams *params);

/** @brief True if the range falls outside the erasable data region. */
#inline
static bool is_boundary_forbidden(uint32_t head, uint32_t tail);

/* ───────────────────────────────────────────────────────── Internal configuration end */

/* ────────── Public ────────── */

/**
 * @brief Perform a five-phase tiered erase over an address range.
 *
 * @details Phases 1–3 promote (4 KiB → 32 KiB → 64 KiB) as alignment
 *          permits. Phases 4–5 relegate (32 KiB → 4 KiB) with
 *          promotion disabled to mop up the remainder.
 *
 * @param[in,out] ctx  @ref EraseContext_s with head/tail addresses.
 *                     On success, @c ctx->success is set to @c true.
 *
 * @pre Head and tail must fall within @ref DATA_1ST .. @ref DATA_END.
 *
 * @see deletion_bounds_adjustment, update_index_after_erase
 */
void erase_data(EraseContext *ctx)
{
    if (is_boundary_forbidden(ctx->head_address, ctx->tail_address))
    {
        LOG("\n\r <?> Invalid erase range for image.");
        return;
    }

    DeletionParams params_4kb = {SECTOR_4K_SIZE, DELETE_4K, TIMEOUT_RATE_4KB, NULL,
                                 SECTOR_32K_SIZE};
    DeletionParams params_32kb = {SECTOR_32K_SIZE, DELETE_32K, TIMEOUT_RATE_32KB, NULL,
                                  SECTOR_64K_SIZE};
    DeletionParams params_64kb = {SECTOR_64K_SIZE, DELETE_64K, TIMEOUT_RATE_64KB, NULL,
                                  0};
    char str_4kb[] = "4";
    char str_32kb[] = "32";
    char str_64kb[] = "64";
    params_4kb.label = str_4kb;
    params_32kb.label = str_32kb;
    params_64kb.label = str_64kb;

    uint32_t current_address = ctx->head_address;

    erase_pass(&current_address, ctx->tail_address, &params_4kb);
    erase_pass(&current_address, ctx->tail_address, &params_32kb);
    erase_pass(&current_address, ctx->tail_address, &params_64kb);

    params_32kb.next_boundary = 0;  // Deactivate promotion.
    params_4kb.next_boundary = 0;

    erase_pass(&current_address, ctx->tail_address, &params_32kb);
    erase_pass(&current_address, ctx->tail_address, &params_4kb);

    ctx->success = true;
}

/* ────────── Private ────────── */

/**
 * @brief Execute one erase tier until promotion or limit is reached.
 *
 * @details Loops at the configured sector size, issuing
 *          @ref flash_erase for each step. Breaks when:
 *          - The current address aligns to @c params->next_boundary
 *            (promotion to a larger tier).
 *          - The next erase would exceed @a last (limit reached).
 *
 * @param[in,out] current  Running erase address — advanced by
 *                         @c sector_size per iteration.
 * @param[in]     last     Inclusive upper bound of the erase range.
 * @param[in]     params   Tier-specific erase parameters.
 */
static void erase_pass(uint32_t *current, uint32_t last, DeletionParams *params)
{
    LOG("\n\rStarting %sKiB erase tier...", params->label);
#ifdef DEBUG_MODE
    uint16_t count = 0;  // Up to ~4GiB without changing tier, safe.
#endif                   /* DEBUG_MODE */

    for (; *current < last; *current += params->sector_size)
    {
        if ((params->next_boundary != 0) && IS_ALIGNED(*current, params->next_boundary))
        {
            LOG("\n\rSwitching erase mode at address: 0x%8LX (%Lu)", *current, *current);
            break;
        }

        if (IS_LIMIT_REACHED(*current, params->sector_size, last))
        {
            LOG("\n\rLimit reached at address: 0x%8LX (%Lu)", *current, *current);
            break;
        }

        flash_erase(*current, params->kind_size, params->timeout);

#ifdef DEBUG_MODE
        count++;
#endif /* DEBUG_MODE */
    }

    LOG("\n\r%sKiB erase executed %Lu times.\n\r-> Current address: 0x%8LX",
        params->label, count, *current);
}

/**
 * @brief Guard against erasing outside the data region.
 *
 * @param[in] head  Start of the requested erase range.
 * @param[in] tail  End of the requested erase range (inclusive).
 *
 * @return @c true if the range is invalid (out of bounds or inverted),
 *         @c false if safe to proceed.
 */
#inline
static bool is_boundary_forbidden(uint32_t head, uint32_t tail)
{
    return (head < DATA_1ST || head > DATA_END || tail < DATA_1ST || tail > DATA_END ||
            tail <= head);
}
