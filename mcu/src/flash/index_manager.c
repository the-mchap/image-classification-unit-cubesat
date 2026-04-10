/**
 * @file index_manager.c
 *
 * @brief Lazy garbage collection with tombstones: index scanning,
 *        bounds adjustment, and write/erase bookkeeping.
 *
 * @details Manages the state-map + entry-pair index that tracks every
 *          data block stored in the LFM. On deletion, entries follow
 *          the @ref DataState_e tombstone lifecycle rather than being
 *          erased immediately — bit-clearing lets us update flash
 *          without a costly erase cycle.
 *
 *          The core operations are:
 *          - **Bounds adjustment** (@ref deletion_bounds_adjustment):
 *            extend the requested erase range to 4 KiB alignment while
 *            protecting neighbor entries.
 *          - **Post-erase update** (@ref update_index_after_erase):
 *            walk the index and tombstone/shrink entries caught in the
 *            erased range — lazy GC for half-deleted entries plus full
 *            GC for fully erased ones.
 *          - **Post-write update** (@ref update_index_after_write):
 *            claim the first untouched slot for a new data block.
 *          - **Compaction check** (@ref is_tombstone_threshold):
 *            count tombstones to decide when a full compaction pass
 *            is worthwhile.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-06
 *
 * @ingroup storage
 */
#ignore_warnings 242 /* Ignore CCS' pedantic warning on switch-case fallthrough. */
#include "globals.h"

#include "../../include/flash/flash_types.h"
#include "../../include/flash/index_manager.h"
#include "../../include/drivers/flash.h"
#include "../../include/protocol/conversions.h"

/* ========================== Internal configuration ========================== */

/** @brief Number of bytes in one index state-map entry. */
#define INDEX_MAP_SIZE 1

/** @brief Value of an untouched/unwritten index entry (all-ones). */
#define UNTOUCHED_ENTRY 0xFFFFFFFFUL

/** @brief Sentinel for "no valid index entry exists." */
#define INVALID_INDEX_ENTRY 0

/** @brief Tombstone count that triggers a compaction pass. */
#define TOMBSTONE_THRESHOLD 32  // THIS CAN BE IN GLOBALS

/**
 * @def IS_SHARING_SECTOR(address_0, address_1)
 * @brief True when both addresses fall within the same aligned 4 KiB sector.
 *
 * @see round_down_4k
 */
#define IS_SHARING_SECTOR(address_0, address_1)                                          \
    (round_down_4k(address_0) == round_down_4k(address_1))

/**
 * @def DELETION_LIMITS_FINAL_CHECK(head, tail)
 * @brief Validate the adjusted deletion range after bounds adjustment.
 *
 * @return @c true if the range is invalid (head >= tail, either is
 *         @ref INVALID_ADDR, or either falls outside the data region).
 */
#define DELETION_LIMITS_FINAL_CHECK(head, tail)                                          \
    (((head) >= (tail)) || (head) == INVALID_ADDR || (tail) == INVALID_ADDR ||           \
     (head) < DATA_1ST || (tail) < DATA_1ST || (tail) > DATA_END)

/* --------------------------- Internal structs ----------------------------- */

/**
 * @struct NeighborCheckParams_s
 * @brief Scan result counters used during @ref deletion_bounds_adjustment.
 */
typedef struct NeighborCheckParams_s
{
    uint8_t neighbor_head;  ///< Entries sharing a 4 KiB sector with the head.
    uint8_t neighbor_tail;  ///< Entries sharing a 4 KiB sector with the tail.
    uint8_t current_match;  ///< Entries that exactly match the requested range.
} NeighborCheckParams;

/**
 * @struct TombstoneParams_s
 * @brief Head/tail bounds used during post-erase tombstone scanning.
 */
typedef struct TombstoneParams_s
{
    uint32_t head;  ///< Start of the erased range.
    uint32_t tail;  ///< End of the erased range (inclusive).
} TombstoneParams;

/* --------------------------- Private helpers ----------------------------- */

/** @brief Write a tombstone state + zeroed entry bytes to a slot. */
static void write_tombstone(uint16_t current, DataState type);

/** @brief Round an address to its 4 KiB boundary (up or down). */
static void adjust_boundary(uint32_t *address, bool round_up, bool is_tail);

/** @brief True if an entry's tail shares a 4 KiB sector with @a head. */
#inline
static bool is_head_neighbor(uint32_t head, Entry *entry);

/** @brief True if an entry's head shares a 4 KiB sector with @a tail. */
#inline
static bool is_tail_neighbor(uint32_t tail, Entry *entry);

/** @brief True when both entry addresses and state are untouched. */
#inline
static bool is_end_of_active_entries(Entry *entry, DataState current_status);

/** @brief True if entry has a tombstoned head inside the erased range. */
#inline
static bool is_head_tombstone(Entry *entry, TombstoneParams *params);

/** @brief True if entry has a tombstoned tail inside the erased range. */
#inline
static bool is_tail_tombstone(Entry *entry, TombstoneParams *params);

/** @brief True if the entire entry falls within the erased range. */
#inline
static bool is_full_entry(Entry *entry, TombstoneParams *params);

/** @brief True if entry head precedes the erase range (head garbage). */
#inline
static bool is_head_garbage(Entry *entry, TombstoneParams *params);

/** @brief True if entry tail exceeds the erase range (tail garbage). */
#inline
static bool is_tail_garbage(Entry *entry, TombstoneParams *params);

/** @brief True if entry spans beyond both ends of the erase range. */
#inline
static bool is_head_tail_garbage(Entry *entry, TombstoneParams *params);

/* ==================== Internal configuration end ==================== */

/* --------------------------- Public interface --------------------------- */

/**
 * @brief Adjust deletion limits by scanning all active index entries.
 *
 * @details Walks every slot in the state map looking for:
 *          1. An exact match of the requested deletion range.
 *          2. Neighbor entries sharing a 4 KiB sector with the head or
 *             tail boundary.
 *
 *          After scanning, the head and tail are rounded to 4 KiB
 *          alignment — rounding direction depends on whether neighbors
 *          were found (to protect them or absorb free space).
 *
 * @param[in,out] ctx  @ref EraseContext_s with head/tail to adjust.
 *
 * @pre @c ctx->head_address < @c ctx->tail_address.
 *
 * @post On invalid result, both addresses are set to @ref INVALID_ADDR.
 *
 * @see update_index_after_erase, round_down_4k, round_up_4k
 */
void deletion_bounds_adjustment(EraseContext *ctx)
{
    if (ctx == NULL || (ctx->head_address >= ctx->tail_address))
    {
        LOG("\n\r <?> Head and tail wrong values or null instances. Quitting:...");
        return;
    }

    /* Scan result counters for requested match + boundary neighbors. */
    NeighborCheckParams scan = {0, 0, 0};

    /* Index scanning. */
    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        Entry entry;
        DataState slot_state = flash_get_state(slot);
        flash_get_entries(data_state_to_index_entry(slot), &entry);

        /* Stop condition: end of active entries. */
        if (is_end_of_active_entries(&entry, slot_state))
        {
            LOG("\n\r Index scanning ends at slot 0x%03X (0x%04X)\n\r->Match found: "
                "%u\n\r->Head neighbor found: %u\n\r->Tail neighbor found: %u\n\n",
                slot, data_state_to_index_entry(slot), scan.current_match,
                scan.neighbor_head, scan.neighbor_tail);
            break;
        }

        if (slot_state != DATA_STATE_FULL_DATA)
        {
            continue;
        }

        /* Match desired index. */
        if (entry.start == ctx->head_address && entry.end == ctx->tail_address)
        {
            scan.current_match++;
            LOG("\n\r ->Entry for requested data found:\n\r Status slot 0x%03X\n\rEntry "
                "0x%04X\n",
                slot, data_state_to_index_entry(slot));
            continue;
        }

        /* Neighbor scanning: head side and tail side. */
        if (is_head_neighbor(ctx->head_address, &entry))
        {
            scan.neighbor_head++;
            LOG("\n\r ->Head neighbor found:\n\r Status slot 0x%03X\n\r Entry 0x%04X\n ",
                slot, data_state_to_index_entry(slot));
            continue;
        }
        else if (is_tail_neighbor(ctx->tail_address, &entry))
        {
            scan.neighbor_tail++;
            LOG("\n\r ->Tail neighbor found:\n\r Status slot 0x%03X\n\r Entry 0x%04X\n ",
                slot, data_state_to_index_entry(slot));
            continue;
        }
    }

    /* First head adjustment, then tail adjustment. */
    adjust_boundary(&ctx->head_address, scan.neighbor_head != 0, false);
    adjust_boundary(&ctx->tail_address, scan.neighbor_tail == 0, true);

    if (DELETION_LIMITS_FINAL_CHECK(ctx->head_address, ctx->tail_address))
    {
        LOG("\n\r <?> Adjusted range is invalid:\n\r\t< 0x%8LX - 0x%8LX >\n",
            ctx->head_address, ctx->tail_address);
        ctx->head_address = INVALID_ADDR;
        ctx->tail_address = INVALID_ADDR;
        return;
    }

    LOG("\n\r <!> Adjusted range:\n\r\t< 0x%8LX - 0x%8LX >\n", ctx->head_address,
        ctx->tail_address);
}

/**
 * @brief Tombstone or shrink index entries caught in an erased range.
 *
 * @details Walks the full index after a successful erase and applies
 *          the @ref DataState_e lifecycle transitions:
 *
 *          - **Lazy GC:** previously tombstoned entries (head or tail
 *            already zeroed) that now fall fully within the erased
 *            range → @ref DATA_STATE_DELETED.
 *          - **Full GC:** intact entries fully inside the range →
 *            @ref DATA_STATE_DELETED.
 *          - **Partial:** entries partially overlapping the range →
 *            @ref DATA_STATE_HEAD_GARBAGE, @ref DATA_STATE_TAIL_GARBAGE,
 *            or @ref DATA_STATE_SHRUNKEN.
 *
 * @param[in] ctx  @ref EraseContext_s describing the completed erase.
 *                 Ignored if @c ctx->success is false.
 *
 * @see deletion_bounds_adjustment, write_tombstone
 */
void update_index_after_erase(EraseContext *ctx)
{ /* Comment for PRINT_JSON placement (Event: erase_start/update) */
    if (!ctx || !ctx->success)
    {
        return;
    }

    TombstoneParams params = {0, 0};
    params.head = ctx->head_address;
    params.tail = ctx->tail_address;

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        Entry entry;
        DataState slot_state = flash_get_state(slot);

        /* Nothing else to keep checking. */
        flash_get_entries(data_state_to_index_entry(slot), &entry);
        if (is_end_of_active_entries(&entry, slot_state))
        {
            break;
        }

        /* Already deleted or untouched entries. */
        if (slot_state == DATA_STATE_DELETED || slot_state == DATA_STATE_UNTOUCHED)
        {
            continue;
        }

        /* ---- Lazy Garbage Collection ---- */
        if (is_head_tombstone(&entry, &params) || is_tail_tombstone(&entry, &params))
        {
            write_tombstone(slot, DATA_STATE_DELETED);
            continue;
        }

        /* ---- Garbage Collection ---- */
        if (is_full_entry(&entry, &params))
        {
            write_tombstone(slot, DATA_STATE_DELETED);
            continue;
        }

        /* ---- Cases where current data are partially deleted ---- */
        if (is_tail_garbage(&entry, &params))
        {
            write_tombstone(slot, DATA_STATE_TAIL_GARBAGE);
            continue;
        }

        if (is_head_garbage(&entry, &params))
        {
            write_tombstone(slot, DATA_STATE_HEAD_GARBAGE);
            continue;
        }

        if (is_head_tail_garbage(&entry, &params))
        {
            write_tombstone(slot, DATA_STATE_SHRUNKEN);
            continue;
        }
    }
}

/**
 * @brief Register a newly written data block in the first free slot.
 *
 * @details Scans the state map for the first @ref DATA_STATE_UNTOUCHED
 *          slot, then writes a @ref DATA_STATE_FULL_DATA byte and the
 *          start/end address pair as an @ref Entry_s.
 *
 * @param[in] start_addr  First byte of the written data.
 * @param[in] size        Number of bytes written.
 *
 * @return @c true if a free slot was found and updated, @c false if
 *         the index is full or the address range is invalid.
 *
 * @see @ref FlashSection_e, flash_write
 */
bool update_index_after_write(uint32_t start_addr, uint32_t size)
{
    uint32_t end_addr = start_addr + size - 1;

    if (start_addr < DATA_1ST || end_addr > DATA_END || start_addr > end_addr)
    {
        LOG("\n\r--FATAL ERROR--\n->head/tail address wrong.\n\r < 0x%8LX - 0x%8LX >\n",
            start_addr, end_addr);
        return false;
    }

    uint8_t entry_bytes[ENTRY_SIZE] = {0};
    uint8_t state_buffer = (uint8_t)DATA_STATE_FULL_DATA;

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState slot_state = flash_get_state(slot);

        /* Search for an untouched entry. */
        if (slot_state == DATA_STATE_UNTOUCHED)
        {
            uint32_to_bytes(entry_bytes, start_addr);
            uint32_to_bytes(entry_bytes + ADDRESS_SIZE, end_addr);

            flash_write(slot, &state_buffer, INDEX_MAP_SIZE);
            flash_write(data_state_to_index_entry(slot), entry_bytes, ENTRY_SIZE);

            /* Comment for PRINT_JSON placement (Event: write) */
            LOG("\n\r-> Slot to update current map found:\n\rState: 0x%02X, Index entry "
                ": 0x%04X\n",
                state_buffer, data_state_to_index_entry(slot));
            return true;
        }
    }

    LOG("\n\r<?> Could not find any index available to update!\n");
    return false;
}

/**
 * @brief Check whether tombstone accumulation warrants compaction.
 *
 * @details Counts @ref DATA_STATE_DELETED slots. Stops early once
 *          @ref TOMBSTONE_THRESHOLD is reached.
 *
 * @return @c true if the count meets or exceeds the threshold.
 */
bool is_tombstone_threshold(void)
{
    uint8_t tombstone_counter = 0;

    for (uint16_t slot = DATA_STAT_1ST; slot <= DATA_STAT_END; ++slot)
    {
        DataState slot_state = flash_get_state(slot);

        if (slot_state == DATA_STATE_DELETED)
        {
            tombstone_counter++;
        }

        /* Stop if threshold reached */
        if (tombstone_counter >= TOMBSTONE_THRESHOLD)
        {
            LOG("\n\r <!> Garbage collection must be executed:"
                "\n\r There are %d tombstones.\n",
                tombstone_counter);
            return true;
        }
    }

    return false;
}

/* * COMMENTED OUT PUBLIC FUNCTION: index_and_status_compaction (ram_index_compact)
 * * To restore functionality:
 * 1. Define index_compaction_params_t or specific variables.
 * 2. Loop DATA_STAT_1ST to DATA_STAT_END.
 * 3. Inside loop: check is_data_or_junk_info logic.
 * 4. Stop when is_last_index logic is true (DATA_STATE_UNTOUCHED).
 * 5. Perform the compaction/write-back logic.
 */

/* --------------------------- Small helpers --------------------------- */

/**
 * @brief Write a tombstone state byte + zeroed entry to a slot.
 *
 * @details What gets zeroed depends on @a state_to_write:
 *          - @ref DATA_STATE_DELETED — full 8-byte entry.
 *          - @ref DATA_STATE_HEAD_GARBAGE — second 4 bytes (end addr).
 *          - @ref DATA_STATE_TAIL_GARBAGE — first 4 bytes (start addr).
 *          - @ref DATA_STATE_SHRUNKEN — state byte only (no entry write).
 *
 * @param[in] current_slot   State-map slot index.
 * @param[in] state_to_write Target @ref DataState_e value.
 */
static void write_tombstone(uint16_t current_slot, DataState state_to_write)
{
    LOG("\n\r ->Tombstone update:\n\r  Status slot 0x%03X, Entry 0x%04X\n\r  0x%02X -> "
        "0x%02X\n",
        current_slot, data_state_to_index_entry(current_slot),
        (uint8_t)flash_get_state(current_slot), (uint8_t)state_to_write);

    uint8_t tombstone_bytes[ENTRY_SIZE] = {TOMBSTONE};
    uint8_t tombstone_size = 0;
    uint8_t offset = 0;

    switch (state_to_write)
    {
    case DATA_STATE_DELETED:
        tombstone_size = ENTRY_SIZE;
        break;

    case DATA_STATE_HEAD_GARBAGE:
        tombstone_size = ADDRESS_SIZE;
        offset = ADDRESS_SIZE; /* Write to second half */
        break;

    case DATA_STATE_TAIL_GARBAGE:
        tombstone_size = ADDRESS_SIZE;
        break;

    case DATA_STATE_SHRUNKEN:
        break;

    default:  // If other states appear here are straight up errors.
        LOG("\n\r <?> State to write was a wrong one.\n");
        return;
    }

    uint8_t status_buffer = (uint8_t)state_to_write;

    flash_write(current_slot, &status_buffer, INDEX_MAP_SIZE);
    flash_write(data_state_to_index_entry(current_slot) + offset, tombstone_bytes,
                tombstone_size);

    /* Comment for PRINT_JSON placement (Event: tombstone_write) */
}

/**
 * @brief Round an address to its 4 KiB boundary.
 *
 * @param[in,out] address   Address to adjust.
 * @param[in]     round_up  If true, round up; otherwise round down.
 * @param[in]     is_tail   If true, decrement by 1 to keep an inclusive
 *                          upper bound after alignment.
 */
static void adjust_boundary(uint32_t *address, bool round_up, bool is_tail)
{
    *address = round_up ? round_up_4k(*address) : round_down_4k(*address);

    if (is_tail && *address > DATA_1ST)
    {
        --(*address); /* Keep inclusive upper bound after round alignment. */
    }
}

/** @brief True if an entry's tail shares a 4 KiB sector with @a head. */
#inline
static bool is_head_neighbor(uint32_t head, Entry *entry)
{
    return IS_SHARING_SECTOR(head, entry->end) && (entry->start != INVALID_INDEX_ENTRY);
}

/** @brief True if an entry's head shares a 4 KiB sector with @a tail. */
#inline
static bool is_tail_neighbor(uint32_t tail, Entry *entry)
{
    return IS_SHARING_SECTOR(tail, entry->start) && (entry->end != INVALID_INDEX_ENTRY);
}

/** @brief True when both entry addresses and state are untouched (end of index). */
#inline
static bool is_end_of_active_entries(Entry *entry, DataState current_status)
{
    return entry->start == UNTOUCHED_ENTRY && entry->end == UNTOUCHED_ENTRY &&
           current_status == DATA_STATE_UNTOUCHED;
}

/** @brief True if entry has a tombstoned head and its tail falls within the erased range.
 */
#inline
static bool is_head_tombstone(Entry *entry, TombstoneParams *params)
{
    return (entry->start == TOMBSTONE) && (entry->end >= params->head) &&
           (entry->end <= params->tail);
}

/** @brief True if entry has a tombstoned tail and its head falls within the erased range.
 */
#inline
static bool is_tail_tombstone(Entry *entry, TombstoneParams *params)
{
    return (entry->end == TOMBSTONE) && (entry->start >= params->head) &&
           (entry->start <= params->tail);
}

/** @brief True if the entire entry (start..end) falls within the erased range. */
#inline
static bool is_full_entry(Entry *entry, TombstoneParams *params)
{
    return (entry->start >= params->head) && (entry->end <= params->tail) &&
           (entry->start != TOMBSTONE) && (entry->end != TOMBSTONE);
}

/** @brief True if entry head precedes the erase range but tail is inside it. */
#inline
static bool is_head_garbage(Entry *entry, TombstoneParams *params)
{
    return (entry->start < params->head) && (entry->end <= params->tail) &&
           (entry->end >= params->head) && (entry->start != TOMBSTONE);
}

/** @brief True if entry head is inside the erase range but tail exceeds it. */
#inline
static bool is_tail_garbage(Entry *entry, TombstoneParams *params)
{
    return (entry->start >= params->head) && (entry->start <= params->tail) &&
           (entry->end > params->tail);
}

/** @brief True if entry spans beyond both ends of the erase range. */
#inline
static bool is_head_tail_garbage(Entry *entry, TombstoneParams *params)
{
    return (entry->start < params->head) && (entry->end > params->tail) && (entry->start);
}
/* You hover like a hummingbird, haunt me in my sleep
 * You're sailing from another world, sinking in my sea
 * Oh, you're feeding on my energy
 * Letting go of it, she wants it
 *                            --Nanna et al, 2015 **/
