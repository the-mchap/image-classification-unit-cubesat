/**
 * @file index_manager.h
 *
 * @brief Index table management: lazy garbage collection with tombstones,
 *        bounds adjustment, and write/erase bookkeeping.
 *
 * @details The index lives in the @ref DATA_STAT_1ST .. @ref DATA_STAT_END
 *          region (state map) with corresponding entries in
 *          @ref IDX_ENTRY_1ST .. @ref IDX_ENTRY_END. Each slot pairs a
 *          @ref DataState_e byte with an @ref Entry_s (start + end address).
 *
 *          On deletion, entries are tombstoned progressively via the
 *          @ref DataState_e bit-clearing lifecycle rather than erased
 *          immediately. A compaction pass is triggered when tombstone
 *          count exceeds a configurable threshold.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-06
 *
 * @ingroup storage
 */
#ifndef INDEX_MANAGER_H
#define INDEX_MANAGER_H

#include "./eraser.h"

/** @brief Number of bytes in one index entry (start + end address). */
#define ENTRY_SIZE 8

/** @brief Value used to mark a tombstoned index entry address. */
#define TOMBSTONE 0

/**
 * @brief Adjust deletion limits by scanning all active index entries.
 *
 * @details Walks every slot in the state map, extends the erase range
 *          to protect neighbor entries, and applies lazy-GC tombstoning
 *          -- all without recursion or callbacks.
 *
 * @param[in,out] ctx  @ref EraseContext_s with head/tail addresses
 *                     to adjust.
 *
 * @pre @c ctx->head_address < @c ctx->tail_address.
 *
 * @see update_index_after_erase, @ref DataState_e
 */
void deletion_bounds_adjustment(EraseContext *ctx);

/**
 * @brief Tombstone or shrink index entries that fell inside an erased range.
 *
 * @param[in] ctx  @ref EraseContext_s describing the completed erase.
 *
 * @see deletion_bounds_adjustment
 */
void update_index_after_erase(EraseContext *ctx);

/**
 * @brief Register a newly written data block in the first free index slot.
 *
 * @param[in] start_address  First byte of the written data.
 * @param[in] size           Number of bytes written.
 *
 * @return @c true if a free slot was found and updated, @c false if the
 *         index is full.
 *
 * @see @ref FlashSection_e for valid address ranges.
 */
bool update_index_after_write(uint32_t start_address, uint32_t size);

/**
 * @brief Check whether tombstone accumulation warrants a compaction pass.
 *
 * @return @c true if the tombstone count exceeds the internal threshold.
 */
bool is_tombstone_threshold(void);

#endif /* INDEX_MANAGER_H */
