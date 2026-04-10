/**
 * @file space_finder.h
 *
 * @brief Free-space allocation in the flash data region.
 *
 * @details Scans the index table to find the largest contiguous gap
 *          between existing entries and returns a starting address
 *          that can fit the requested data size.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-06
 *
 * @ingroup storage
 */
#ifndef SPACE_FINDER_H
#define SPACE_FINDER_H

/** @brief Total number of slots in the data-state map region. */
#define DATA_STAT_LENGTH 512

/**
 * @brief Find a free block large enough for @p data_size bytes.
 *
 * @details Walks every active index slot, computes upper/lower gaps
 *          around each entry, and returns the start of the largest
 *          contiguous free region.
 *
 * @param[in] data_size  Bytes to allocate.
 *
 * @return Starting address of a suitable gap, or @ref INVALID_ADDR
 *         if no block is large enough.
 *
 * @see @ref FlashSection_e for the valid data region boundaries.
 * @see update_index_after_write to register the allocation once
 *      the write completes.
 */
uint32_t get_available_address(uint32_t data_size);

#endif /* SPACE_FINDER_H */
