/**
 * @file eraser.h
 *
 * @brief Multi-phase flash erase with tiered sector sizing.
 *
 * @details Erases an arbitrary address range by stepping through
 *          progressively larger sector sizes (4 KB -> 32 KB -> 64 KB)
 *          for aligned portions, then falls back through 32 KB and
 *          4 KB for the tail cleanup. Index bookkeeping is handled
 *          separately by @ref update_index_after_erase.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2025-07-07
 *
 * @ingroup storage
 */
#ifndef ERASER_H
#define ERASER_H

/**
 * @struct EraseContext_s
 * @brief Context for an erase operation: address range + result flag.
 *
 * @see erase_data, deletion_bounds_adjustment
 */
typedef struct EraseContext_s
{
    uint32_t head_address; /**< Start address of the deletion range. */
    uint32_t tail_address; /**< End address of the deletion range. */
    bool success;          /**< @c true if the erase completed successfully. */
} EraseContext;

/**
 * @brief Erase flash from @c ctx->head_address to @c ctx->tail_address.
 *
 * @details Uses tiered logic to choose sector sizes by alignment:
 *          -# 4 KB sectors until aligned to 32 KB.
 *          -# 32 KB sectors until aligned to 64 KB.
 *          -# Bulk 64 KB erasing.
 *          -# 32 KB cleanup.
 *          -# 4 KB cleanup.
 *
 * @param[in,out] ctx  Erase context. On return, @c ctx->success
 *                     indicates whether the operation completed.
 *
 * @pre @c head_address and @c tail_address must lie within
 *      @ref DATA_1ST .. @ref DATA_END.
 *
 * @see deletion_bounds_adjustment to adjust limits before erasing.
 * @see update_index_after_erase to tombstone affected index entries.
 */
void erase_data(EraseContext *ctx);

#endif /* ERASER_H */
