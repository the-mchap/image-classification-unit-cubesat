/**
 * @file metadata.c
 *
 * @brief EXIF UserComment extraction from JPEG images in flash.
 *
 * @details Parses JPEG marker segments to locate the APP1/EXIF block,
 *          walks the IFD0 → ExifIFD chain to find the UserComment tag,
 *          strips the 8-byte charset prefix, and copies up to 7 ASCII
 *          payload bytes. A small forward-read cache
 *          (@ref ReadCache_s) amortises SPI flash transactions while
 *          the parser traverses IFD tables.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-07
 *
 * @ingroup storage
 */
#include "globals.h"

#include "../../include/flash/metadata.h"
#include "../../include/drivers/flash.h"
#include "../../include/protocol/conversions.h"

/* ────────────  Internal configuration (do not export) ──────────── */

/** @name JPEG Marker Constants
 * @{ */
#define JPEG_SOI 0xFFD8U   ///< Start Of Image.
#define JPEG_APP1 0xFFE1U  ///< APP1 (EXIF) segment.
#define JPEG_SOS 0xFFDAU   ///< Start Of Scan — image data begins here.
#define JPEG_EOI 0xFFD9U   ///< End Of Image.
/** @} */

/** @name EXIF / TIFF Constants
 * @{ */
#define EXIF_ID_LEN 6u                 ///< Length of the "Exif\0\0" identifier.
#define EXIF_TAG_EXIF_IFD 0x8769U      ///< IFD0 tag pointing to the ExifIFD.
#define EXIF_TAG_USER_COMMENT 0x9286U  ///< ExifIFD tag for UserComment.
#define TIFF_MAGIC 42u                 ///< TIFF header magic number.
/** @} */

/**
 * @brief Length of the UserComment charset prefix ("ASCII\0\0\0").
 *
 * @details The actual comment payload starts after this prefix.
 *          We read at most @ref MAX_COMMENT_BYTES from the payload,
 *          fitting them into the 8-byte output buffer (7 chars + NUL).
 */
#define EXIF_CHARSET_PREFIX_LEN 8u

/** @brief Maximum comment payload bytes to extract (7 chars + NUL). */
#define MAX_COMMENT_BYTES 7u

/**
 * @brief IFD entries read per batch (each entry is 12 bytes → 48 bytes/batch).
 *
 * @note Tune down to 1 or 2 if the stack budget gets tight.
 */
#define IFD_BATCH_ENTRIES 4u

/**
 * @brief Forward-read cache size in bytes.
 *
 * @details One cache line covers most small EXIF segments, keeping
 *          SPI transactions to a minimum while the parser walks IFD
 *          tables.
 */
#define READ_CACHE_SIZE 64u

#define BYTE_ORDER_BE 0u  ///< Big-endian TIFF byte order ("MM").
#define BYTE_ORDER_LE 1u  ///< Little-endian TIFF byte order ("II").

/* ── Types ── */

/** @brief Canonical "Exif\0\0" identifier for APP1 segment matching. */
static uint8_t k_exif_id[EXIF_ID_LEN] = {'E', 'x', 'i', 'f', 0x00u, 0x00u};

/**
 * @struct ReadCache_s
 * @brief Forward-read cache over @ref flash_read.
 *
 * @details Caches up to @ref READ_CACHE_SIZE bytes from flash to
 *          reduce SPI transactions during sequential EXIF parsing.
 */
typedef struct ReadCache_s
{
    uint32_t start;                 ///< JPEG-relative offset of the cached window.
    uint8_t len;                    ///< Valid bytes in @c data (≤ @ref READ_CACHE_SIZE).
    uint8_t valid;                  ///< 0 when the cache holds no data yet.
    uint8_t data[READ_CACHE_SIZE];  ///< Cached flash bytes.
} ReadCache;

/**
 * @struct JpegCtx_s
 * @brief Bundles the three values threaded through every internal function.
 */
typedef struct JpegCtx_s
{
    uint32_t base;       ///< Absolute flash address of the first JPEG byte.
    uint32_t jpeg_size;  ///< Total JPEG size in bytes.
    ReadCache cache;     ///< Forward-read cache state.
} JpegCtx;

/**
 * @struct ExifWindow_s
 * @brief Boundaries of the EXIF APP1 segment and its embedded TIFF payload.
 */
typedef struct ExifWindow_s
{
    uint32_t tiff_offset;  ///< Start of TIFF header, relative to JPEG start.
    uint32_t app1_end;     ///< Byte past the last APP1 byte, relative to JPEG start.
} ExifWindow;

/**
 * @struct TiffInfo_s
 * @brief Byte order and IFD0 pointer decoded from the TIFF header.
 */
typedef struct TiffInfo_s
{
    uint8_t byte_order;    ///< @ref BYTE_ORDER_BE or @ref BYTE_ORDER_LE.
    uint32_t ifd0_offset;  ///< IFD0 offset relative to TIFF header.
} TiffInfo;

/* ── Forward declarations ── */

/** @brief Cached flash read — reduces SPI calls during sequential parsing. */
static int meta_read_cached(JpegCtx *ctx, uint32_t rel, uint8_t *dst, uint16_t size);

/** @brief Locate the EXIF APP1 segment and record TIFF boundaries. */
static int find_exif_window(JpegCtx *ctx, ExifWindow *out_window);

/** @brief Walk IFD0 → ExifIFD → UserComment and return payload location. */
static int resolve_user_comment(JpegCtx *ctx, ExifWindow *window,
                                uint32_t *comment_addr_out, uint32_t *bytes_to_copy_out);

/* ──────────── Internal configuration end ──────────── */

/**
 * @brief Compare a 5-byte compact uplinked metadata code against a 7-char
 *        flash string extracted from EXIF UserComment.
 *
 * @details Compact layout (5 bytes): {count, class[0], class[1], class[2], confidence}.
 *          Flash layout  (7 chars):  "CCLLLDPP" where CC = count hex, LLL = class ASCII,
 *          PP = confidence hex.
 *
 * @param[in] compact   Pointer to the 5-byte uplinked code.
 * @param[in] flash_str Pointer to the 7-char string from flash (no NUL required).
 *
 * @return true if they represent the same metadata, false otherwise.
 *
 * @see nibble_to_hex, metadata_extract_user_comment
 */
bool match_metadata_code(uint8_t *compact, char *flash_str)
{
    char expanded[MAX_COMMENT_BYTES];

    /* Count: byte -> 2 hex ASCII chars */
    expanded[0] = nibble_to_hex(compact[0] >> 4);
    expanded[1] = nibble_to_hex(compact[0] & 0x0F);

    /* Class: 3 bytes are already ASCII */
    expanded[2] = compact[1];
    expanded[3] = compact[2];
    expanded[4] = compact[3];

    /* Confidence: byte -> 2 hex ASCII chars */
    expanded[5] = nibble_to_hex(compact[4] >> 4);
    expanded[6] = nibble_to_hex(compact[4] & 0x0F);

    return memcmp(expanded, flash_str, MAX_COMMENT_BYTES) == 0;
}

/**
 * @brief Extract up to 7 ASCII bytes from EXIF UserComment.
 *
 * @param[in]  jpeg_base_addr Base flash address where JPEG starts.
 * @param[in]  jpeg_size      JPEG size in bytes.
 * @param[out] comment_out    Output buffer (must fit 8 bytes including '\0').
 *
 * @return Bytes copied (1..7), 0 if comment/tag is absent, -1 on parse/read error.
 *
 * @see match_metadata_code, find_exif_window, resolve_user_comment
 */
int metadata_extract_user_comment(uint32_t jpeg_base_addr, uint32_t jpeg_size,
                                  char *comment_out)
{
    ExifWindow window;
    int status;
    uint32_t comment_addr;
    uint32_t bytes_to_copy;
    JpegCtx ctx = {0, 0, {0u, 0u, 0u, {0u}}};
    ctx.base = jpeg_base_addr;
    ctx.jpeg_size = jpeg_size;

    if (comment_out == NULL || jpeg_size < 12u)
    {
        return -1;
    }

    comment_out[0] = '\0';

    if (find_exif_window(&ctx, &window) != 0)
    {
        return -1;
    }

    status = resolve_user_comment(&ctx, &window, &comment_addr, &bytes_to_copy);
    if (status != 1)
    {
        return status;
    }

    if (meta_read_cached(&ctx, comment_addr, (uint8_t *)comment_out,
                         (uint16_t)bytes_to_copy) != 0)
    {
        return -1;
    }
    comment_out[bytes_to_copy] = '\0';

    return (int)bytes_to_copy;
}

/* ──────────── Private ──────────── */

/**
 * @brief Read bytes from JPEG using a small forward cache to reduce SPI calls.
 *
 * On a cache miss the next READ_CACHE_SIZE bytes are fetched in one SPI
 * transaction.  Reads larger than the cache bypass it entirely.
 *
 * @param[in,out] ctx   JPEG context (base address, size, cache state).
 * @param[in]     rel   Relative offset from JPEG base.
 * @param[out]    dst   Destination buffer.
 * @param[in]     size  Number of bytes to read.
 *
 * @return 0 on success, -1 on out-of-bounds or bad arguments.
 */
static int meta_read_cached(JpegCtx *ctx, uint32_t rel, uint8_t *dst, uint16_t size)
{
    ReadCache *cache = &ctx->cache;
    uint32_t cache_end;
    uint32_t end_rel;
    uint16_t cache_offset;
    uint32_t remain;
    uint16_t chunk_len;

    if (dst == NULL || ctx == NULL || size == 0u || rel > ctx->jpeg_size)
    {
        return -1;
    }

    if ((uint32_t)size > (ctx->jpeg_size - rel))
    {
        return -1;
    }

    end_rel = rel + (uint32_t)size;

    /* Cache hit: requested range lies entirely within the cached window. */
    if (cache->valid != 0u)
    {
        cache_end = cache->start + (uint32_t)cache->len;
        if (rel >= cache->start && end_rel <= cache_end)
        {
            cache_offset = (uint16_t)(rel - cache->start);
            memcpy(dst, &cache->data[cache_offset], size);
            return 0;
        }
    }

    /* Oversized read: bypass the cache and fetch directly. */
    if (size > READ_CACHE_SIZE)
    {
        flash_read(ctx->base + rel, dst, size);
        cache->valid = 0u;
        return 0;
    }

    /* Cache miss: refill from `rel` up to READ_CACHE_SIZE bytes. */
    remain = ctx->jpeg_size - rel;
    chunk_len = READ_CACHE_SIZE;
    if ((uint32_t)chunk_len > remain)
    {
        chunk_len = (uint16_t)remain;
    }

    flash_read(ctx->base + rel, cache->data, chunk_len);
    cache->start = rel;
    cache->len = (uint8_t)chunk_len;
    cache->valid = 1u;

    memcpy(dst, cache->data, size);
    return 0;
}

/**
 * @brief Decode a 16-bit value respecting TIFF byte order.
 *
 * @param[in] data        Pointer to 2 raw bytes.
 * @param[in] byte_order  @ref BYTE_ORDER_BE or @ref BYTE_ORDER_LE.
 *
 * @return Decoded 16-bit value.
 */
static uint16_t read_u16_ordered(uint8_t *data, uint8_t byte_order)
{
    if (byte_order == BYTE_ORDER_BE)
    {
        return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
    }
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

/**
 * @brief Decode a 32-bit value respecting TIFF byte order.
 *
 * @param[in] data        Pointer to 4 raw bytes.
 * @param[in] byte_order  @ref BYTE_ORDER_BE or @ref BYTE_ORDER_LE.
 *
 * @return Decoded 32-bit value.
 */
static uint32_t read_u32_ordered(uint8_t *data, uint8_t byte_order)
{
    if (byte_order == BYTE_ORDER_BE)
    {
        return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
               ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    }
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/**
 * @brief Scan an IFD for a specific EXIF tag.
 *
 * IFD entries are processed in batches of IFD_BATCH_ENTRIES to amortise
 * the cache-miss cost across multiple 12-byte entries.
 *
 * @param[in,out] ctx         JPEG context (cache may be updated).
 * @param[in]     tiff_info   Parsed TIFF header (byte order, IFD0 offset).
 * @param[in]     tiff_offset TIFF header offset within the JPEG.
 * @param[in]     ifd_offset  IFD offset relative to the TIFF header.
 * @param[in]     target_tag  Tag ID to search for.
 * @param[out]    value_out   Receives the tag value/offset field on match.
 * @param[out]    count_out   Receives the tag count field on match.
 *
 * @return 1 if found, 0 if not found, -1 on malformed data or read error.
 */
static int find_ifd_tag(JpegCtx *ctx, TiffInfo *tiff_info, uint32_t tiff_offset,
                        uint32_t ifd_offset, uint16_t target_tag, uint32_t *value_out,
                        uint32_t *count_out)
{
    uint8_t buf[IFD_BATCH_ENTRIES * 12u];
    uint16_t entry_count;
    uint16_t entries_left;
    uint16_t batch_entries;
    uint32_t pos;
    uint16_t bytes_to_read;
    uint16_t offset;
    uint16_t i;

    if (value_out == NULL || count_out == NULL || ctx == NULL || tiff_info == NULL)
    {
        return -1;
    }

    if (tiff_offset > ctx->jpeg_size || ifd_offset > (ctx->jpeg_size - tiff_offset - 2u))
    {
        return -1;
    }

    pos = tiff_offset + ifd_offset;
    if (meta_read_cached(ctx, pos, buf, 2u) != 0)
    {
        return -1;
    }
    entry_count = read_u16_ordered(buf, tiff_info->byte_order);
    pos += 2u;
    entries_left = entry_count;

    while (entries_left > 0u)
    {
        batch_entries =
            (entries_left < IFD_BATCH_ENTRIES) ? entries_left : IFD_BATCH_ENTRIES;
        bytes_to_read = (uint16_t)(batch_entries * 12u);

        if (pos > (ctx->jpeg_size - (uint32_t)bytes_to_read))
        {
            return -1;
        }

        if (meta_read_cached(ctx, pos, buf, bytes_to_read) != 0)
        {
            return -1;
        }

        for (i = 0u, offset = 0u; i < batch_entries; i++, offset += 12u)
        {
            if (read_u16_ordered(&buf[offset], tiff_info->byte_order) == target_tag)
            {
                *count_out = read_u32_ordered(&buf[offset + 4u], tiff_info->byte_order);
                *value_out = read_u32_ordered(&buf[offset + 8u], tiff_info->byte_order);
                return 1;
            }
        }

        pos += (uint32_t)bytes_to_read;
        entries_left -= batch_entries;
    }

    return 0;
}

/**
 * @brief Locate the EXIF APP1 segment and record the TIFF payload boundaries.
 *
 * Walks JPEG markers from the SOI.  Stops at SOS/EOI (image data start).
 * Recognises and skips standalone markers (RST0-RST7, SOI, EOI, TEM) which
 * carry no length field.
 *
 * @param[in,out] ctx        JPEG context (cache may be updated).
 * @param[out]    out_window Receives the located APP1/TIFF boundaries.
 *
 * @return 0 on success, -1 when EXIF APP1 is absent or data is malformed.
 */
static int find_exif_window(JpegCtx *ctx, ExifWindow *out_window)
{
    uint8_t soi[2];
    uint8_t seg_header[4];
    uint16_t marker16;
    uint16_t seg_len;
    uint32_t pos;
    uint32_t seg_end;

    if (ctx == NULL || out_window == NULL)
    {
        return -1;
    }

    /* Verify JPEG SOI marker at offset 0. */
    if (meta_read_cached(ctx, 0u, soi, 2u) != 0)
    {
        return -1;
    }
    if ((uint16_t)(((uint16_t)soi[0] << 8) | soi[1]) != JPEG_SOI)
    {
        return -1;
    }

    out_window->tiff_offset = 0u;
    out_window->app1_end = 0u;
    pos = 2u;

    while (pos + 4u <= ctx->jpeg_size)
    {
        if (meta_read_cached(ctx, pos, seg_header, 4u) != 0)
        {
            return -1;
        }

        marker16 = (uint16_t)(((uint16_t)seg_header[0] << 8) | (uint16_t)seg_header[1]);
        seg_len = (uint16_t)(((uint16_t)seg_header[2] << 8) | (uint16_t)seg_header[3]);

        /* Image data begins here — no more metadata segments follow. */
        if (marker16 == JPEG_SOS || marker16 == JPEG_EOI)
        {
            break;
        }

        if (seg_header[0] != 0xFFu)
        {
            pos++;
            continue;
        }

        /* Standalone markers (RST0-RST7, SOI, EOI, TEM) carry no length field. */
        if ((seg_header[1] >= 0xD0u && seg_header[1] <= 0xD7u) ||
            seg_header[1] == 0xD8u || seg_header[1] == 0xD9u || seg_header[1] == 0x01u)
        {
            pos += 2u;
            continue;
        }

        if (seg_len < 2u)
        {
            return -1;
        }

        if (pos > (ctx->jpeg_size - 2u) ||
            (uint32_t)seg_len > (ctx->jpeg_size - pos - 2u))
        {
            return -1;
        }

        seg_end = pos + 2u + (uint32_t)seg_len;

        if (marker16 == JPEG_APP1 && seg_len >= (uint16_t)(2u + EXIF_ID_LEN))
        {
            uint8_t exif_id[EXIF_ID_LEN];

            if (meta_read_cached(ctx, pos + 4u, exif_id, EXIF_ID_LEN) != 0)
            {
                return -1;
            }

            if (memcmp(exif_id, k_exif_id, EXIF_ID_LEN) == 0)
            {
                out_window->tiff_offset = pos + 4u + EXIF_ID_LEN;
                out_window->app1_end = seg_end;
                return 0;
            }
        }

        pos = seg_end;
    }

    return -1;
}

/**
 * @brief Parse the TIFF header: byte order and IFD0 offset.
 *
 * @param[in,out] ctx         JPEG context (cache may be updated).
 * @param[in]     tiff_offset Offset of the TIFF header within the JPEG.
 * @param[out]    out         Receives byte order and IFD0 offset.
 *
 * @return 0 on success, -1 on malformed header or read error.
 */
static int parse_tiff_info(JpegCtx *ctx, uint32_t tiff_offset, TiffInfo *out)
{
    uint8_t tiff_header[8];

    if (ctx == NULL || out == NULL)
    {
        return -1;
    }

    if (tiff_offset + 8u > ctx->jpeg_size)
    {
        return -1;
    }

    if (meta_read_cached(ctx, tiff_offset, tiff_header, 8u) != 0)
    {
        return -1;
    }

    /* Byte-order mark: "MM" = big-endian, "II" = little-endian. */
    if (tiff_header[0] == 0x4Du && tiff_header[1] == 0x4Du)
    {
        out->byte_order = BYTE_ORDER_BE;
    }
    else if (tiff_header[0] == 0x49u && tiff_header[1] == 0x49u)
    {
        out->byte_order = BYTE_ORDER_LE;
    }
    else
    {
        return -1;
    }

    if (read_u16_ordered(&tiff_header[2], out->byte_order) != TIFF_MAGIC)
    {
        return -1;
    }

    out->ifd0_offset = read_u32_ordered(&tiff_header[4], out->byte_order);
    return 0;
}

/**
 * @brief Locate the UserComment payload and compute its bounded copy length.
 *
 * Walks IFD0 → ExifIFD → UserComment, strips the 8-byte charset prefix,
 * and clamps the result to MAX_COMMENT_BYTES.
 *
 * @param[in,out] ctx               JPEG context (cache may be updated).
 * @param[in]     window            Valid EXIF APP1/TIFF boundaries.
 * @param[out]    comment_addr_out  Receives the JPEG-relative address of the payload.
 * @param[out]    bytes_to_copy_out Receives the clamped payload length (≤ @ref
 * MAX_COMMENT_BYTES).
 *
 * @return 1 if payload is present, 0 if the tag is absent or empty, -1 on error.
 */
static int resolve_user_comment(JpegCtx *ctx, ExifWindow *window,
                                uint32_t *comment_addr_out, uint32_t *bytes_to_copy_out)
{
    TiffInfo tiff_info;
    uint32_t exif_ifd_offset;
    uint32_t comment_value_offset;
    uint32_t comment_count;
    uint32_t bytes_to_copy;
    int tag_status;

    if (ctx == NULL || window == NULL || comment_addr_out == NULL ||
        bytes_to_copy_out == NULL)
    {
        return -1;
    }

    if (window->tiff_offset == 0u || window->app1_end > ctx->jpeg_size)
    {
        return -1;
    }

    if (parse_tiff_info(ctx, window->tiff_offset, &tiff_info) != 0)
    {
        return -1;
    }

    /* Step 1: find the ExifIFD pointer in IFD0. */
    tag_status = find_ifd_tag(ctx, &tiff_info, window->tiff_offset, tiff_info.ifd0_offset,
                              EXIF_TAG_EXIF_IFD, &exif_ifd_offset, &comment_count);
    if (tag_status != 1)
    {
        return (tag_status == 0) ? 0 : -1;
    }

    /* Step 2: find UserComment within the ExifIFD. */
    tag_status =
        find_ifd_tag(ctx, &tiff_info, window->tiff_offset, exif_ifd_offset,
                     EXIF_TAG_USER_COMMENT, &comment_value_offset, &comment_count);
    if (tag_status != 1)
    {
        return (tag_status == 0) ? 0 : -1;
    }

    /* Strip the 8-byte charset prefix ("ASCII\0\0\0") and clamp to MAX_COMMENT_BYTES. */
    if (comment_count <= EXIF_CHARSET_PREFIX_LEN)
    {
        return 0;
    }

    bytes_to_copy = comment_count - EXIF_CHARSET_PREFIX_LEN;
    if (bytes_to_copy > MAX_COMMENT_BYTES)
    {
        bytes_to_copy = MAX_COMMENT_BYTES;
    }

    *comment_addr_out =
        window->tiff_offset + comment_value_offset + EXIF_CHARSET_PREFIX_LEN;
    if (*comment_addr_out > window->app1_end ||
        bytes_to_copy > (window->app1_end - *comment_addr_out))
    {
        return -1;
    }

    *bytes_to_copy_out = bytes_to_copy;
    return 1;
}
