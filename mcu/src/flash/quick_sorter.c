/**
 * @file quick_sorter.c
 *
 * @brief Iterative Quick Sort for index entries — no recursion, no
 *        dynamic allocation.
 *
 * @details Sorts an array of @ref Entry_s structs by their @c start
 *          address using a stack-simulated Quick Sort. The explicit
 *          stack (@ref SIZE_QUICK deep) replaces recursive calls,
 *          keeping stack usage bounded and deterministic.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2025-08-17
 *
 * @version 1.0
 *
 * @ingroup storage
 */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/drivers/flash.h"
#include "../../include/flash/quick_sorter.h"

/* ─────────────────── Internal configuration (do not export) ─────────────────── */

/**
 * @brief Maximum number of entries the sort can handle (also the
 *        explicit stack depth).
 */
#define SIZE_QUICK 128

/** @brief Partition around the pivot and return the pivot index. */
static uint8_t partition(Entry array[], uint8_t low_index, uint8_t high_index);

/** @brief Stack-simulated iterative Quick Sort. */
static void quick_sort_iterative(Entry *array, uint8_t start_index, uint8_t end_index);

/** @brief Dump the Entry array to the debug log. */
#inline
static void print_array(Entry *array);

/** @brief Swap two @ref Entry_s values (CCS reference params). */
#inline
static void swap_entry(Entry *a_ptr, Entry *b_ptr);

/** @brief Fill unused tail of the array with @ref INVALID_ADDR. */
#inline
static void fill_array(Entry *array, uint16_t array_size);

/* ──────────── Internal configuration end ──────────── */

/* ──────────── Public ──────────── */

/**
 * @brief Sort index entries by start address (ascending).
 *
 * @details Pads unused slots with @ref INVALID_ADDR, counts valid
 *          elements, then delegates to @ref quick_sort_iterative.
 *
 * @param[in,out] index_entry_buffer  Entry array to sort in-place.
 * @param[in]     q_size              Number of pre-filled entries.
 *
 * @warning Experimental — not yet battle-tested in production.
 */
void quick_sort(Entry *index_entry_buffer, uint16_t q_size)
{
    uint8_t valid_elements_count = 0;

    fill_array(index_entry_buffer, q_size);

#ifdef DEBUG_MODE
    LOG("\n\r -- Unsorted array:\n");
    print_array(index_entry_buffer);
#endif /* DEBUG_MODE */

    for (uint8_t i = 0; i < SIZE_QUICK; ++i)
    {
        if (index_entry_buffer[i].start == INVALID_ADDR)
        {
            break;
        }
        valid_elements_count++;
    }

    if (valid_elements_count > 0)
    {
        quick_sort_iterative(index_entry_buffer, 0, valid_elements_count - 1);
    }

#ifdef DEBUG_MODE
    LOG("\n\r -- Quick-sorted array:\n");
    print_array(index_entry_buffer);
#endif /* DEBUG_MODE */
}

/* ──────────── Private ──────────── */

/**
 * @brief Iterative Quick Sort using an explicit stack.
 *
 * @details Simulates recursive calls by pushing sub-array index pairs
 *          onto a fixed-size stack. Pops pairs, partitions, and pushes
 *          left/right halves until the stack is empty.
 *
 * @param[in,out] array        Entry array to sort.
 * @param[in]     start_index  First index (typically 0).
 * @param[in]     end_index    Last index (typically count - 1).
 */
static void quick_sort_iterative(Entry *array, uint8_t start_index, uint8_t end_index)
{
    // Create an auxiliary stack to manage sub-array indices.
    // A fixed-size array is used to adhere to the "no dynamic allocation" constraint.
    uint32_t stack[SIZE_QUICK];

    // initialize top of stack
    int8_t top_of_stack = -1;

    // push initial values of start_index and end_index to stack
    stack[++top_of_stack] = start_index;
    stack[++top_of_stack] = end_index;

    // Keep popping from stack while it is not empty
    while (top_of_stack >= 0)
    {
        // Pop end_index and start_index
        end_index = stack[top_of_stack--];
        start_index = stack[top_of_stack--];

        // Partition the sub-array and get the pivot index
        uint8_t pivot_index = partition(array, start_index, end_index);

        // If there are elements on the left side, push their indices to stack
        if (pivot_index - 1 > start_index)
        {
            stack[++top_of_stack] = start_index;
            stack[++top_of_stack] = pivot_index - 1;
        }

        // If there are elements on the right side, push their indices to stack
        if (pivot_index + 1 < end_index)
        {
            stack[++top_of_stack] = pivot_index + 1;
            stack[++top_of_stack] = end_index;
        }
    }
}

/**
 * @brief Partition the array around the last element as pivot.
 *
 * @details Places the pivot at its correct sorted position by
 *          @c start address, with smaller elements to its left and
 *          larger to its right.
 *
 * @param[in,out] array       Entry array to partition.
 * @param[in]     low_index   Start of the sub-array.
 * @param[in]     high_index  End of the sub-array (pivot element).
 *
 * @return Final index of the pivot element.
 */
static uint8_t partition(Entry array[], uint8_t low_index, uint8_t high_index)
{
    uint32_t pivot_value = array[high_index].start;  // Pivot based on the 'start' member
    int8_t i = (low_index - 1);                      // Index of smaller element

    for (uint8_t j = low_index; j <= high_index - 1; j++)
    {
        // Compare based on the 'start' member
        if (array[j].start < pivot_value)
        {
            i++;
            swap_entry(&array[i], &array[j]);  // Swap entire Entry structs
        }
    }
    swap_entry(array[i + 1], array[high_index]);  // Swap entire Entry structs
    return (i + 1);
}

/**
 * @brief Swap two @ref Entry_s values.
 *
 * @param[in,out] a_ptr  First entry (CCS reference).
 * @param[in,out] b_ptr  Second entry (CCS reference).
 */
#inline
static void swap_entry(Entry *a_ptr, Entry *b_ptr)
{
    Entry *temp_entry = a_ptr;
    a_ptr = b_ptr;
    b_ptr = temp_entry;
}

/**
 * @brief Dump the Entry array to the debug log.
 *
 * @param[in] array  Entry array to print.
 */
#inline
static void print_array(Entry *array)
{
    for (uint8_t i = 0; i < SIZE_QUICK; i++)
    {
        LOG("[0x%8LX-"
            "0x%8LX]  ",
            array[i].start, array[i].end);
        if (!((i - 4) % 5) && i > 3)  // Magic numbers don't matter, just printing.
        {
            LOG("\n");  // New line for readability.
        }
    }

    LOG("\n\r -- End.");
}

/**
 * @brief Fill unused tail of the array with 0xFF (INVALID_ADDR).
 *
 * @details Uses @c memset for a fast byte-level fill — more efficient
 *          than a manual loop on this target.
 *
 * @param[in,out] array       Entry array to pad.
 * @param[in]     array_size  Number of valid entries already present.
 *
 * @warning Check for off-by-one on the boundary between valid and
 *          padded entries.
 */
#inline
static void fill_array(Entry *array, uint16_t array_size)
{  // WARNING: Check again an off-by-one possible miss!
    memset(array + array_size, 0xFF, sizeof(Entry) * (SIZE_QUICK - array_size));
}
