/**
 * @file quick_sorter.h
 *
 * @brief Iterative quicksort for index entry arrays -- no recursion,
 *        no dynamic allocation.
 *
 * @details Uses an explicit stack to avoid recursion (forbidden per
 *          project safety rules). Sorts @ref Entry_s arrays by start
 *          address so that sequential flash scans can be performed
 *          in address order.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2025-08-17
 *
 * @ingroup storage
 */
#ifndef QUICK_SORTER_H
#define QUICK_SORTER_H

#include "../drivers/flash.h"

/**
 * @brief Sort an array of index entries by start address (ascending).
 *
 * @param[in,out] index_entry_buffer  Array of @ref Entry_s to sort in place.
 * @param[in]     q_size              Number of valid entries in the array.
 *
 * @warning Experimental -- under active testing.
 */
void quick_sort(Entry *index_entry_buffer, uint16_t q_size);

#endif /* QUICK_SORTER_H */
