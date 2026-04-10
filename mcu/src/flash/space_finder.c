/**
 * @file space_finder.c
 *
 * @brief Free-space allocation: scans the flash index to find the
 *        largest contiguous gap for incoming data.
 *
 * @details Two strategies are tried in order:
 *          1. **Boundary check** — measure the gap between @ref DATA_1ST
 *             and the lowest entry, and between the highest entry and
 *             @ref DATA_END.
 *          2. **Full index scan** — for each active entry, iterate over
 *             every other entry to compute the tightest upper/lower
 *             neighbor gaps, then keep the largest.
 *
 *          The first strategy that yields a gap >= @a data_size wins.
 *          Tombstoned entries are skipped; half-tombstones are rounded
 *          to their surviving 4 KiB boundary before comparison.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-06
 *
 * @ingroup storage
 */
#ignore_warnings 242 /* Avoid CCS warning about switch-case fallthrough. */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/flash/space_finder.h"
#include "../../include/drivers/flash.h"
#include "../../include/protocol/conversions.h"
#include "../../include/flash/index_manager.h"

/* ------------------ Internal configuration ------------------- */

/** @brief Maximum accepted data size in bytes. */
#define MAX_DATA_SIZE_BYTES 0xA00000UL  // OJO

/**
 * @struct GapInfo_s
 * @brief A measured gap: size in bytes + starting address.
 */
typedef struct GapInfo_s
{
    uint32_t space;    ///< Gap size in bytes.
    uint32_t address;  ///< Start address of the gap.
} GapInfo;

/**
 * @struct BoundaryGaps_s
 * @brief Upper and lower gaps around a reference entry.
 */
typedef struct BoundaryGaps_s
{
    GapInfo upper;  ///< Closest gap above the reference entry.
    GapInfo lower;  ///< Closest gap below the reference entry.
} BoundaryGaps;

/**
 * @struct LargestGapResult_s
 * @brief The largest gap found across the entire scan.
 */
typedef struct LargestGapResult_s
{
    uint32_t space;    ///< Largest gap size in bytes.
    uint32_t address;  ///< Start address of the largest gap.
} LargestGapResult;

/**
 * @struct BoundarySlots_s
 * @brief Slot indices that produced the start/end boundary values.
 */
typedef struct BoundarySlots_s
{
    uint16_t start_slot;  ///< Slot that contributed the start boundary.
    uint16_t end_slot;    ///< Slot that contributed the end boundary.
} BoundarySlots;

/**
 * @struct BoundaryExtrema_s
 * @brief Min/max boundary addresses plus their source slots.
 */
typedef struct BoundaryExtrema_s
{
    uint32_t start_addr;  ///< Extremal start address found.
    uint32_t end_addr;    ///< Extremal end address found.
    BoundarySlots slots;  ///< Slots that produced these values.
} BoundaryExtrema;

/* --------------------------- Private prototypes --------------------------- */

/** @brief Measure a gap between two boundaries and update if smallest. */
static void check_and_update_gap(uint32_t larger_boundary, uint32_t smaller_boundary,
                                 GapInfo *current_min_gap);

/** @brief Compare one entry against a reference and update local gaps. */
static void check_for_gaps(Entry *iterator, Entry *ref_entry, BoundaryGaps *local_gaps);

/** @brief Promote local gaps to the overall largest if they beat it. */
static void update_largest_space_found(uint32_t *largest_space, uint32_t *largest_address,
                                       BoundaryGaps *gap);

/** @brief Find tightest upper/lower gaps around a single reference entry. */
static void find_largest_gap_around_entry(uint16_t ref_entry_index,
                                          LargestGapResult *largest_gap);

/** @brief Check if the scan has reached the end of active entries. */
static void is_scanning_done(uint16_t ref_entry_index, uint8_t *is_done);

/** @brief Measure the internal hole of a shrunken entry. */
static void measure_a_hole(uint16_t ref_entry_index, LargestGapResult *biggest);

/** @brief Track min/max start/end addresses across all entries. */
static void evaluate_entry_boundaries(BoundaryExtrema *start_bound,
                                      BoundaryExtrema *end_bound, Entry *entry,
                                      uint16_t index);

/** @brief True if start and end came from the same index slot. */
static bool is_boundary_clean(BoundarySlots *slots);

/** @brief Measure the gap between @ref DATA_1ST and the lowest entry. */
static void evaluate_space_at_start(BoundaryExtrema *start_bound,
                                    LargestGapResult *biggest);

/** @brief Measure the gap between the highest entry and @ref DATA_END. */
static void evaluate_space_at_end(BoundaryExtrema *end_bound, LargestGapResult *biggest);

/** @brief Strategy 1: check gaps at the data region boundaries. */
static bool look_for_space_at_boundaries(uint32_t data_size, LargestGapResult *biggest);

/** @brief Strategy 2: full pairwise index scan for the largest gap. */
static bool scan_index_for_gap(uint32_t data_size, LargestGapResult *biggest);

/** @brief Adjust half-tombstone entries to their surviving 4 KiB boundary. */
#inline
static void adjust_entries_if_needed(Entry *entry);

/** @brief Validate that @a data_size is within the accepted range. */
#inline
static bool is_data_size_valid(uint32_t data_size);

/** @brief Log and return the best address found. */
#inline
static uint32_t report_available_space(LargestGapResult *biggest);

/** @brief True when both start and end are zero (fully tombstoned). */
#inline
static bool is_full_tombstone_entry(Entry *entry);

/* ================== Internal configuration end ================== */

/* --------------------------- Public interface --------------------------- */

/**
 * @brief Find a free block large enough for @a data_size bytes.
 *
 * @details Tries boundary gaps first (fast path), then falls back to a
 *          full pairwise index scan. Returns as soon as a suitable gap
 *          is found.
 *
 * @param[in] data_size  Bytes to allocate.
 *
 * @return Starting address of a suitable gap, or @ref INVALID_ADDR
 *         if no block is large enough.
 *
 * @see update_index_after_write, @ref FlashSection_e
 */
uint32_t get_available_address(uint32_t data_size)
{
    if (!is_data_size_valid(data_size))
    {
        return INVALID_ADDR;
    }

    LargestGapResult biggest = {0, 0};

    if (look_for_space_at_boundaries(data_size, &biggest))
    {
        return report_available_space(&biggest);
    }

    if (scan_index_for_gap(data_size, &biggest))
    {
        return report_available_space(&biggest);
    }

    LOG("\n\r <!> Declined. Not enough space available.");

    return INVALID_ADDR;
}

/* --------------------------- Small helpers --------------------------- */

/** @brief True when both start and end are zero (fully tombstoned). */
#inline
static bool is_full_tombstone_entry(Entry *entry)
{
    return (entry->start == 0U) && (entry->end == 0U);
}

/**
 * @brief Measure the gap between two boundaries and update the minimum.
 *
 * @param[in]     larger_boundary   Upper boundary of the potential gap.
 * @param[in]     smaller_boundary  Lower boundary of the potential gap.
 * @param[in,out] current_min_gap   Updated if this gap is tighter.
 */
static void check_and_update_gap(uint32_t larger_boundary, uint32_t smaller_boundary,
                                 GapInfo *current_min_gap)
{
    if (larger_boundary <= smaller_boundary)
    {
        return;
    }

    uint32_t new_space = larger_boundary - smaller_boundary - 1;

    if (new_space < current_min_gap->space)
    {
        current_min_gap->space = new_space;
        current_min_gap->address = smaller_boundary + 1;
    }
}

/**
 * @brief Compare one entry against a reference and update local gaps.
 *
 * @param[in]     iterator    Entry under evaluation.
 * @param[in]     ref_entry   Center point for the comparison.
 * @param[in,out] local_gaps  Closest upper/lower gaps around @a ref_entry.
 */
static void check_for_gaps(Entry *iterator, Entry *ref_entry, BoundaryGaps *local_gaps)
{
    /* Determine the boundaries for comparison based on the type of iterator entry. */
    uint32_t lower_bound;
    uint32_t upper_bound;

    if (iterator->start && iterator->end)
    { /* A full entry (non-tombstone in any form). */
        lower_bound = iterator->start;
        upper_bound = iterator->end;
    }
    else
    { /* A half tombstone, use the valid part for both comparisons. */
        lower_bound = (iterator->start != 0U) ? iterator->start : iterator->end;
        upper_bound = lower_bound;
    }

    /* Helper to check for smaller gaps. */
    check_and_update_gap(ref_entry->start, lower_bound, &local_gaps->lower);
    check_and_update_gap(upper_bound, ref_entry->end, &local_gaps->upper);
}

/**
 * @brief Adjust half-tombstone entries to their surviving 4 KiB boundary.
 *
 * @param[in,out] entry  Entry whose zeroed address gets reconstructed
 *                       via @ref round_down_4k / @ref round_up_4k.
 */
#inline
static void adjust_entries_if_needed(Entry *entry)
{
    if (entry->start == 0U) /* If it's a tail-garbage-marked entry. */
    {
        entry->start = round_down_4k(entry->end);
    }
    if (entry->end == 0U) /* If it's a head-garbage-marked entry. */
    {
        entry->end = round_up_4k(entry->start) - 1;
    }
}

/**
 * @brief Promote local gaps to the overall largest if they beat it.
 *
 * @param[in,out] largest_space    Current best gap size.
 * @param[in,out] largest_address  Current best gap start.
 * @param[in]     gap              Candidate lower/upper gaps.
 */
static void update_largest_space_found(uint32_t *largest_space, uint32_t *largest_address,
                                       BoundaryGaps *gap)
{
    if (gap->lower.space != INVALID_ADDR && gap->lower.space > *largest_space)
    {
        *largest_space =
            gap->lower.space; /* Considerable gap between current ref & low data. */
        *largest_address = gap->lower.address;
    }
    if (gap->upper.space != INVALID_ADDR && gap->upper.space > *largest_space)
    {
        *largest_space =
            gap->upper.space; /* Interesting gap between current ref & high data. */
        *largest_address = gap->upper.address;
    }
}

/**
 * @brief Find the tightest upper/lower gaps around a single reference
 *        entry, then update the overall largest.
 *
 * @param[in]     ref_entry_index  State-map slot of the reference entry.
 * @param[in,out] largest_gap      Running best gap across all calls.
 */
static void find_largest_gap_around_entry(uint16_t ref_entry_index,
                                          LargestGapResult *largest_gap)
{
    /* Read the reference entry that we will compare all others against. */
    Entry ref_entry = {0, 0};
    uint16_t index_slot = data_state_to_index_entry(ref_entry_index);
    flash_get_entries(index_slot, &ref_entry);

    index_slot += ENTRY_SIZE; /* Loop MUST start from next-up index. */

    adjust_entries_if_needed(&ref_entry); /* If entry is half-tombstone, adjust. */

    if (is_full_tombstone_entry(&ref_entry))
    {
        return; /* Skip full tombstone reference entries. */
    }

    BoundaryGaps local_gaps = {{INVALID_ADDR, INVALID_ADDR},
                               {INVALID_ADDR, INVALID_ADDR}};
    /* Iterate over all other higher index entries to find the closest neighbors. */
    for (uint16_t j = index_slot; j < IDX_ENTRY_END; j += ENTRY_SIZE)
    {
        Entry iterator = {0, 0};
        flash_get_entries(j, &iterator);

        if (iterator.start == INVALID_ADDR) /* End of valid entries. */
        {
            break;
        }
        if (is_full_tombstone_entry(&iterator)) /* Ignore full tombstones. */
        {
            continue;
        }

        /* Determine the boundaries for comparison based on the type of iterator entry. */
        check_for_gaps(&iterator, &ref_entry, &local_gaps);
    }

    /* Update largest gap if a better candidate was found. */
    update_largest_space_found(&largest_gap->space, &largest_gap->address, &local_gaps);
}

/**
 * @brief Check if the scan has reached the end of active entries.
 *
 * @details Verifies that an untouched state-map byte corresponds to an
 *          unwritten index entry (all 0xFF). Logs a warning on mismatch.
 *
 * @param[in]  ref_entry_index  State-map slot to check.
 * @param[out] is_done          Set to 1 if both state and entry are unwritten.
 */
static void is_scanning_done(uint16_t ref_entry_index, uint8_t *is_done)
{
    Entry entry = {0, 0};

    flash_get_entries(data_state_to_index_entry(ref_entry_index), &entry);

    if (entry.start == INVALID_ADDR && entry.end == INVALID_ADDR)
    {
        *is_done = 1; /* Expected "end of entries" condition. */
        return;
    }

    LOG("\n\r <?> Inconsistency between status & index found at 0x%03X.",
        ref_entry_index);
}

/**
 * @brief Measure the internal hole of a shrunken entry.
 *
 * @details For @ref DATA_STATE_SHRUNKEN entries, the surviving data
 *          sits at both ends — the middle may be free. Rounds both
 *          boundaries inward to 4 KiB alignment and measures the gap.
 *
 * @param[in]     ref_entry_index  State-map slot of the shrunken entry.
 * @param[in,out] biggest          Updated if this hole exceeds the current best.
 */
static void measure_a_hole(uint16_t ref_entry_index, LargestGapResult *biggest)
{
    Entry entry = {0, 0};

    flash_get_entries(data_state_to_index_entry(ref_entry_index), &entry);

    entry.start = round_up_4k(entry.start);
    entry.end = round_down_4k(entry.end) - 1;

    if (!entry.start || !entry.end)
    { /* If rounding messed up, the hole probably wasn't big enough. */
        LOG("\n\r <?> WARNING: Rounding error detected for hole at status.");
        return;
    }
    if (entry.end < entry.start) /* Underflow defensive. */
    {
        return;
    }

    uint32_t gap = entry.end - entry.start;

    if (gap > biggest->space)
    { /* If this gap is big enough, it must be considered. */
        biggest->space = gap;
        biggest->address = entry.start;
    }
}

/**
 * @brief Track min/max start/end addresses across all entries.
 *
 * @param[in,out] start_bound  Tracks the lowest start and end addresses.
 * @param[in,out] end_bound    Tracks the highest start and end addresses.
 * @param[in]     entry        Current entry under evaluation.
 * @param[in]     index        Raw index slot of the entry.
 */
static void evaluate_entry_boundaries(BoundaryExtrema *start_bound,
                                      BoundaryExtrema *end_bound, Entry *entry,
                                      uint16_t index)
{
    /* Find the entry with the smallest start address (closest to DATA_1ST) */
    if ((entry->start < start_bound->start_addr) && entry->start)
    {
        start_bound->start_addr = entry->start;
        start_bound->slots.start_slot = index;
    }
    /* Find the entry with the largest start address (closest to DATA_END) */
    if (entry->start > end_bound->start_addr)
    {
        end_bound->start_addr = entry->start;
        end_bound->slots.start_slot = index;
    }
    /* Find the entry with the smallest end address (closest to DATA_1ST) */
    if ((entry->end < start_bound->end_addr) && entry->end)
    {
        start_bound->end_addr = entry->end;
        start_bound->slots.end_slot = index;
    }
    /* Find the entry with the largest end address (closest to DATA_END) */
    if (entry->end > end_bound->end_addr)
    {
        end_bound->end_addr = entry->end;
        end_bound->slots.end_slot = index;
    }
}

/**
 * @brief True if start and end came from the same index slot.
 *
 * @param[in] slots  Slot pair to check.
 *
 * @return @c true if the boundary is defined by a single clean entry,
 *         @c false if adjacent tombstones contribute different parts.
 */
static bool is_boundary_clean(BoundarySlots *slots)
{
    return slots->start_slot == slots->end_slot;
}

/**
 * @brief Measure the gap between @ref DATA_1ST and the lowest entry.
 *
 * @param[in]     start_bound  Lowest boundary addresses found.
 * @param[in,out] biggest      Updated if this gap is the new champion.
 */
static void evaluate_space_at_start(BoundaryExtrema *start_bound,
                                    LargestGapResult *biggest)
{
    uint32_t lower_space = 0;

    if (is_boundary_clean(&start_bound->slots))
    {
        /* The space is simply from the start of the data area to the first entry. */
        lower_space = start_bound->start_addr - DATA_1ST;
    }
    else
    {
        /* This logic handles complex garbage/tombstone scenarios at the start. */
        if (start_bound->end_addr < start_bound->start_addr)
        {
            lower_space = round_down_4k(start_bound->end_addr) - DATA_1ST;
        }
    }

    if (lower_space > biggest->space)
    {
        biggest->space = lower_space;
        biggest->address = DATA_1ST;
    }
}

/**
 * @brief Measure the gap between the highest entry and @ref DATA_END.
 *
 * @param[in]     end_bound  Highest boundary addresses found.
 * @param[in,out] biggest    Updated if this gap is the new champion.
 */
static void evaluate_space_at_end(BoundaryExtrema *end_bound, LargestGapResult *biggest)
{
    uint32_t upper_space = 0;
    uint32_t start_of_gap = INVALID_ADDR;

    if (is_boundary_clean(&end_bound->slots))
    {
        start_of_gap = end_bound->end_addr + 1;
        upper_space = DATA_END - end_bound->end_addr;
    }
    else
    {
        /* This logic handles complex garbage/tombstone scenarios at the end. */
        if (end_bound->end_addr < end_bound->start_addr)
        {
            start_of_gap = round_up_4k(end_bound->start_addr);
            upper_space = DATA_END - start_of_gap;
        }
    }

    if (upper_space > biggest->space)
    {
        biggest->space = upper_space;
        biggest->address = start_of_gap;
    }
}

/**
 * @brief Strategy 1: check gaps at the data region boundaries.
 *
 * @details Scans every entry to find the min/max start and end
 *          addresses, then measures the gaps between those extrema
 *          and the data region edges (@ref DATA_1ST / @ref DATA_END).
 *          Handles the empty-index case (entire region is free).
 *
 * @param[in]     data_size  Required gap size.
 * @param[in,out] biggest    Updated with the best gap found.
 *
 * @return @c true if @a biggest->space >= @a data_size.
 */
static bool look_for_space_at_boundaries(uint32_t data_size, LargestGapResult *biggest)
{
    BoundaryExtrema start_bound = {INVALID_ADDR, INVALID_ADDR, {0, 0}};
    BoundaryExtrema end_bound = {0, 0, {0, 0}};
    Entry entry = {0, 0};
    uint16_t i = IDX_ENTRY_1ST;

    for (; i < IDX_ENTRY_END; i += ENTRY_SIZE)
    {
        flash_get_entries(i, &entry);

        if (entry.start == INVALID_ADDR)
        {
            break; /* End of index scanning. */
        }
        if (is_full_tombstone_entry(&entry))
        {
            continue; /* Skip full tombstones. */
        }

        evaluate_entry_boundaries(&start_bound, &end_bound, &entry, i);
    }

    if (i == IDX_ENTRY_1ST) /* Case 1: The entire index is empty. */
    {
        biggest->space = DATA_END - DATA_1ST;
        biggest->address = DATA_1ST;
    }
    else /* Case 2: The index has entries, evaluate the boundaries. */
    {
        evaluate_space_at_start(&start_bound, biggest);
        evaluate_space_at_end(&end_bound, biggest);
    }

    return (biggest->space >= data_size);
}

/** @brief Validate that @a data_size is within the accepted range. */
#inline
static bool is_data_size_valid(uint32_t data_size)
{
    if (data_size == 0U || data_size >= MAX_DATA_SIZE_BYTES)
    {
        LOG("\n\r <!> Invalid data size provided.");
        return false;
    }

    return true;
}

/** @brief Log and return the best address found. */
#inline
static uint32_t report_available_space(LargestGapResult *biggest)
{
    LOG("\n\r ->Space available: %Lu Bytes at 0x%8LX", biggest->space, biggest->address);
    return biggest->address;
}

/**
 * @brief Strategy 2: full pairwise index scan for the largest gap.
 *
 * @details Walks the state map slot by slot. For each active entry
 *          type, calls @ref find_largest_gap_around_entry to compute
 *          pairwise distances. @ref DATA_STATE_SHRUNKEN entries also
 *          get their internal hole measured via @ref measure_a_hole.
 *          Exits early once a gap >= @a data_size is found.
 *
 * @param[in]     data_size  Required gap size.
 * @param[in,out] biggest    Updated with the best gap found.
 *
 * @return @c true if @a biggest->space >= @a data_size.
 */
static bool scan_index_for_gap(uint32_t data_size, LargestGapResult *biggest)
{
    uint8_t is_done = 0;
    DataState status_buffer = DATA_STATE_ERROR;

    /** DATA_STAT_END is used as sentinel in this scan, so it is excluded. */
    for (uint16_t i = DATA_STAT_1ST; i < DATA_STAT_END; ++i)
    {
        status_buffer = flash_get_state(i);

        switch (status_buffer)
        {
        case DATA_STATE_UNTOUCHED:
            is_scanning_done(i, &is_done);
            break;

        case DATA_STATE_DELETED:
            continue; /* No need to evaluate something that's gone. */

        case DATA_STATE_FULL_DATA:
        case DATA_STATE_TAIL_GARBAGE:
        case DATA_STATE_HEAD_GARBAGE: /* Yes, lack of break is intentional. */
            find_largest_gap_around_entry(i, biggest);
            break;

        case DATA_STATE_SHRUNKEN:
            measure_a_hole(i, biggest);
            find_largest_gap_around_entry(i, biggest);
            break;

        default:
            LOG("\n\r Unknown status pattern: 0x%02X", status_buffer);
            find_largest_gap_around_entry(i, biggest);
            break;
        }

        if (biggest->space >= data_size)
        {
            return true;
        }

        if (is_done)
        {
            break;
        }
    }

    return false;
}
/* The teenage queen, the loaded gun
 * The drop dead dream, the chosen one
 * A southern drawl, a world unseen
 * A city wall and a trampoline
 * Well, I don't mind, if you don't mind
 * Because I don't shine if you don't shine
 * Before you jump, tell me what you find
 * When you read my mind
 *        --Brandon Flowers et al, 2007 **/
