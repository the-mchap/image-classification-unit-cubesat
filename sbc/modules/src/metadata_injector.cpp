#include <chrono>
#include <cstring>
#include <iostream>
#include <libexif/exif-data.h>
#include <libexif/exif-loader.h>
#include <vector>

extern "C" {

// ─────────────────────────────────────────────────────────────────────────────
// JPEG / EXIF structural constants
// ─────────────────────────────────────────────────────────────────────────────

/// First byte of every JPEG marker prefix.
static constexpr uint8_t JPEG_MARKER_PREFIX = 0xFF;

/// JPEG Start-of-Image marker (SOI): the two bytes that open every JPEG file.
static constexpr uint8_t JPEG_MARKER_SOI = 0xD8;

/// JPEG End-of-Image marker.
static constexpr uint8_t JPEG_MARKER_EOI = 0xD9;

/// JPEG Start-of-Scan marker; raw entropy-coded image data follows immediately.
/// Segment length rules do NOT apply past this point.
static constexpr uint8_t JPEG_MARKER_SOS = 0xDA;

/// APP0 marker — typically holds JFIF metadata.
static constexpr uint8_t JPEG_MARKER_APP0 = 0xE0;

/// APP1 marker — holds EXIF or XMP metadata.
static constexpr uint8_t JPEG_MARKER_APP1 = 0xE1;

/// Temporary/private marker (0x01) that carries no segment length field.
static constexpr uint8_t JPEG_MARKER_TEM = 0x01;

/// First restart marker (RST0); RST0–RST7 are 0xD0–0xD7.
static constexpr uint8_t JPEG_MARKER_RST_FIRST = 0xD0;

/// Last restart marker (RST7).
static constexpr uint8_t JPEG_MARKER_RST_LAST = 0xD7;

/// Number of bytes occupied by the SOI at the very start of a JPEG stream.
static constexpr size_t JPEG_SOI_SIZE = 2;

/// Number of bytes used by a JPEG segment length field.
static constexpr size_t JPEG_SEGMENT_LENGTH_SIZE = 2;

/// Number of bytes occupied by a JPEG marker (0xFF + type byte).
static constexpr size_t JPEG_MARKER_SIZE = 2;

/// Minimum number of bytes a JPEG segment length field may encode (the field
/// itself counts toward the value, so 2 is the smallest valid length).
static constexpr uint16_t JPEG_MIN_SEGMENT_LENGTH = 2;

/// Byte count of the EXIF identifier string "Exif\0\0" that immediately
/// follows the APP1 length field.
static constexpr size_t EXIF_IDENTIFIER_SIZE = 6;

/// Minimum APP1 segment length required to hold at least the "Exif\0\0"
/// identifier plus the 2-byte length field itself.
static constexpr size_t EXIF_APP1_MIN_SEGMENT_LEN = 8;

/// The canonical EXIF identifier that opens every EXIF APP1 payload.
static constexpr char EXIF_IDENTIFIER[] = "Exif\0\0";

/// Size of the ASCII character-set prefix in a UserComment EXIF field.
/// The spec mandates exactly 8 bytes: "ASCII\0\0\0".
static constexpr size_t EXIF_USER_COMMENT_CHARSET_SIZE = 8;

/// ASCII charset token written into the first 8 bytes of a UserComment field.
static constexpr char EXIF_USER_COMMENT_ASCII_TOKEN[] = "ASCII\0\0\0";

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Returns true when @p marker carries no segment length field.
 *
 * Per the JPEG specification the following marker types are stand-alone:
 *  - TEM  (0x01)
 *  - RST0–RST7 (0xD0–0xD7)
 *  - SOI  (0xD8)
 *  - EOI  (0xD9)
 *
 * @param marker  The single marker-type byte (the byte that follows 0xFF).
 * @return @c true if the marker has no associated length field.
 */
static inline bool is_standalone_marker(uint8_t marker) {
    return marker == JPEG_MARKER_TEM ||
           (marker >= JPEG_MARKER_RST_FIRST && marker <= JPEG_MARKER_RST_LAST) ||
           marker == JPEG_MARKER_SOI || marker == JPEG_MARKER_EOI;
}

/**
 * @brief Adds a single null-terminated ASCII string tag to an EXIF IFD.
 *
 * Allocates a fresh ExifEntry, attaches it to @p ifd, and populates it with
 * the supplied @p value.  Ownership of the entry is transferred to the IFD;
 * the local reference is released via exif_entry_unref().
 *
 * @param ed     The ExifData container that owns the IFD.
 * @param ifd    Which EXIF IFD (e.g. EXIF_IFD_0 or EXIF_IFD_EXIF) to write to.
 * @param tag    The EXIF tag identifier (e.g. EXIF_TAG_SOFTWARE).
 * @param value  Null-terminated string value to store in the tag.
 */
static void add_ascii_tag(ExifData *ed, ExifIfd ifd, ExifTag tag, const char *value) {
    ExifEntry *entry = exif_entry_new();
    exif_content_add_entry(ed->ifd[ifd], entry);
    exif_entry_initialize(entry, tag);

    // Replace whatever default data libexif allocated with our own string.
    free(entry->data);
    entry->size = strlen(value) + 1; // +1 for the null terminator
    entry->data = (unsigned char *)malloc(entry->size);
    entry->components = entry->size;
    memcpy(entry->data, value, entry->size);

    exif_entry_unref(entry); // IFD now holds the sole reference
}

/**
 * @brief Adds a UserComment tag to the EXIF_IFD_EXIF IFD.
 *
 * The EXIF UserComment field (tag 0x9286) requires an 8-byte character-set
 * identifier prefix before the actual text.  This helper writes the standard
 * "ASCII\0\0\0" prefix followed by @p comment.
 *
 * @param ed       The ExifData container.
 * @param comment  Null-terminated UTF-8/ASCII comment string to embed.
 */
static void add_user_comment_tag(ExifData *ed, const char *comment) {
    ExifEntry *entry = exif_entry_new();
    exif_content_add_entry(ed->ifd[EXIF_IFD_EXIF], entry);
    exif_entry_initialize(entry, EXIF_TAG_USER_COMMENT);

    size_t comment_len = strlen(comment);

    free(entry->data);
    entry->size = EXIF_USER_COMMENT_CHARSET_SIZE + comment_len;
    entry->data = (unsigned char *)malloc(entry->size);
    entry->components = entry->size;

    // First 8 bytes: charset identifier ("ASCII\0\0\0")
    memcpy(entry->data, EXIF_USER_COMMENT_ASCII_TOKEN, EXIF_USER_COMMENT_CHARSET_SIZE);
    // Remaining bytes: the comment text itself (no null terminator required by spec)
    memcpy(entry->data + EXIF_USER_COMMENT_CHARSET_SIZE, comment, comment_len);

    exif_entry_unref(entry);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Builds an in-memory ExifData block populated with the supplied fields.
 *
 * Constructs a minimal EXIF structure in Intel (little-endian) byte order and
 * populates the subset of tags described by the non-null parameters.
 *
 * | Parameter      | EXIF IFD       | EXIF Tag                     |
 * |----------------|----------------|------------------------------|
 * | software       | IFD0           | Software (0x0131)            |
 * | date_time      | IFD0 + IFD Exif| DateTime / DateTimeOriginal  |
 * | user_comment   | IFD Exif       | UserComment (0x9286)         |
 *
 * @param software      Null-terminated software name string, or @c nullptr to omit.
 * @param date_time     Null-terminated date/time string in EXIF format
 *                      ("YYYY:MM:DD HH:MM:SS"), or @c nullptr to omit.
 * @param user_comment  Null-terminated ASCII comment string, or @c nullptr to omit.
 *
 * @return A heap-allocated ExifData on success; the caller must release it
 *         with exif_data_unref().  Returns @c nullptr on allocation failure.
 */
static ExifData *
create_exif_data(const char *software, const char *date_time, const char *user_comment) {
    ExifData *ed = exif_data_new();
    if (!ed)
        return nullptr;

    exif_data_set_byte_order(ed, EXIF_BYTE_ORDER_INTEL);

    if (software)
        add_ascii_tag(ed, EXIF_IFD_0, EXIF_TAG_SOFTWARE, software);

    if (date_time) {
        add_ascii_tag(ed, EXIF_IFD_0, EXIF_TAG_DATE_TIME, date_time);
        add_ascii_tag(ed, EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, date_time);
    }

    if (user_comment)
        add_user_comment_tag(ed, user_comment);

    return ed;
}

/**
 * @brief Injects (or replaces) an EXIF APP1 segment into a JPEG byte stream.
 *
 * Parses the JPEG markers in @p in_buffer to locate the correct insertion
 * point for the new APP1 block, then writes the modified image to
 * @p out_buffer.
 *
 * **Insertion rules (EXIF / JFIF ordering):**
 * - If an existing EXIF APP1 segment is found it is replaced in-place.
 * - Otherwise the new APP1 is inserted immediately after an APP0 (JFIF)
 *   segment when one is present, or right after the SOI when there is none.
 *
 * **Two-pass usage** — call once with @p out_buffer = @c nullptr to obtain
 * the required output size, allocate, then call again with the real buffer:
 * @code
 *   size_t needed = inject_metadata(jpg, jpg_len, sw, dt, cmt, nullptr, 0);
 *   uint8_t *buf  = (uint8_t *)malloc(needed);
 *   size_t written = inject_metadata(jpg, jpg_len, sw, dt, cmt, buf, needed);
 * @endcode
 *
 * @param in_buffer     Pointer to the source JPEG data.  Must begin with the
 *                      JPEG SOI marker (0xFF 0xD8).
 * @param in_size       Byte length of @p in_buffer.
 * @param software      Null-terminated software string for the EXIF block,
 *                      or @c nullptr to omit the tag.
 * @param date_time     Null-terminated date/time string ("YYYY:MM:DD HH:MM:SS"),
 *                      or @c nullptr to omit.
 * @param user_comment  Null-terminated ASCII comment, or @c nullptr to omit.
 * @param out_buffer    Destination buffer for the modified JPEG, or @c nullptr
 *                      to perform a dry-run that returns the required size.
 * @param out_max_size  Capacity of @p out_buffer in bytes (ignored when
 *                      @p out_buffer is @c nullptr).
 *
 * @return Number of bytes written to @p out_buffer on success.
 *         When @p out_buffer is @c nullptr, returns the number of bytes that
 *         *would* be written (useful for pre-allocation).
 *         Returns @c 0 on any error (bad SOI, allocation failure, output
 *         buffer too small).
 */
size_t inject_metadata(const uint8_t *in_buffer,
                       size_t in_size,
                       const char *software,
                       const char *date_time,
                       const char *user_comment,
                       uint8_t *out_buffer,
                       size_t out_max_size) {
    // ── Validate that the input looks like a JPEG ────────────────────────────
    if (!in_buffer || in_size < 4)
        return 0;
    if (in_buffer[0] != JPEG_MARKER_PREFIX || in_buffer[1] != JPEG_MARKER_SOI)
        return 0;

    // ── Build the new EXIF block ─────────────────────────────────────────────
    ExifData *ed = create_exif_data(software, date_time, user_comment);
    if (!ed)
        return 0;

    unsigned char *exif_raw_data = nullptr;
    unsigned int exif_raw_size = 0;
    exif_data_save_data(ed, &exif_raw_data, &exif_raw_size);
    exif_data_unref(ed);

    if (!exif_raw_data)
        return 0;

    // APP1 segment layout: [0xFF 0xE1] [2-byte length] [exif_raw_data]
    // exif_raw_data already contains the "Exif\0\0" identifier + TIFF header.
    const size_t app1_size = JPEG_MARKER_SIZE + JPEG_SEGMENT_LENGTH_SIZE + exif_raw_size;

    // ── Scan JPEG markers to find the insertion/replacement position ─────────

    /// Byte offset in in_buffer where we will write the new APP1.
    size_t insert_pos = JPEG_SOI_SIZE; // default: right after SOI

    /// Number of bytes in in_buffer to overwrite (non-zero only when replacing
    /// an existing EXIF APP1 segment).
    size_t replace_len = 0;

    size_t pos = JPEG_SOI_SIZE; // begin after the SOI (0xFF 0xD8)

    while (pos + 1 < in_size) {
        // ── Step 1: Locate the next marker prefix byte (0xFF) ────────────────
        // In a well-formed JPEG this is always the current byte, but we advance
        // defensively if the stream is noisy.
        if (in_buffer[pos] != JPEG_MARKER_PREFIX) {
            pos++;
            continue;
        }

        // ── Step 2: Skip consecutive 0xFF padding bytes ──────────────────────
        // The JPEG spec allows one or more 0xFF fill bytes before the actual
        // marker type byte.
        while (pos + 1 < in_size && in_buffer[pos + 1] == JPEG_MARKER_PREFIX) {
            pos++;
        }

        // Make sure we haven't run off the end after skipping padding.
        if (pos + 1 >= in_size)
            break;

        uint8_t marker = in_buffer[pos + 1];
        pos += JPEG_MARKER_SIZE; // advance past [0xFF, marker-type]

        // ── Step 3: Dispatch on marker type ─────────────────────────────────

        // Stand-alone markers carry no length field; just continue scanning.
        if (is_standalone_marker(marker))
            continue;

        // SOS marks the start of the compressed image bitstream.
        // Nothing after this point follows the standard segment-length layout,
        // so we must stop parsing here.
        if (marker == JPEG_MARKER_SOS)
            break;

        // ── Step 4: Read the 2-byte segment length ───────────────────────────
        // The length value includes the 2 bytes of the field itself but not
        // the preceding marker bytes.
        if (pos + JPEG_SEGMENT_LENGTH_SIZE > in_size)
            break;

        uint16_t segment_len = (uint16_t)((in_buffer[pos] << 8) | in_buffer[pos + 1]);

        // Reject obviously corrupt length values.
        if (segment_len < JPEG_MIN_SEGMENT_LENGTH || (pos + segment_len) > in_size)
            break;

        // Record the byte offset of the 0xFF that opened this segment.
        size_t segment_start = pos - JPEG_MARKER_SIZE;

        if (marker == JPEG_MARKER_APP0) {
            // APP0 (JFIF): insert the new APP1 immediately after this segment
            // so that it follows the JFIF header as the spec recommends.
            insert_pos = segment_start + JPEG_MARKER_SIZE + segment_len;

        } else if (marker == JPEG_MARKER_APP1) {
            // APP1: check whether this is an existing EXIF segment by looking
            // for the "Exif\0\0" identifier right after the length field.
            bool has_exif_id = (segment_len >= EXIF_APP1_MIN_SEGMENT_LEN) &&
                               (pos + JPEG_SEGMENT_LENGTH_SIZE + EXIF_IDENTIFIER_SIZE <= in_size) &&
                               (memcmp(&in_buffer[pos + JPEG_SEGMENT_LENGTH_SIZE],
                                       EXIF_IDENTIFIER,
                                       EXIF_IDENTIFIER_SIZE) == 0);
            if (has_exif_id) {
                // Mark the existing EXIF APP1 for replacement and stop scanning.
                insert_pos = segment_start;
                replace_len = JPEG_MARKER_SIZE + segment_len;
                break;
            }
        }

        // Advance to the start of the next segment.
        pos += segment_len;
    }

    // ── Assemble the output JPEG ─────────────────────────────────────────────

    size_t total_size = in_size - replace_len + app1_size;

    // Dry-run: caller just wants to know the required buffer size.
    if (!out_buffer) {
        free(exif_raw_data);
        return total_size;
    }

    if (total_size > out_max_size) {
        free(exif_raw_data);
        return 0; // not enough space
    }

    size_t out_pos = 0;

    // 1. Copy everything up to the insertion/replacement point.
    memcpy(out_buffer, in_buffer, insert_pos);
    out_pos = insert_pos;

    // 2. Write the new APP1 marker (0xFF 0xE1).
    out_buffer[out_pos++] = JPEG_MARKER_PREFIX;
    out_buffer[out_pos++] = JPEG_MARKER_APP1;

    // 3. Write the 2-byte APP1 length (length field + exif_raw_data).
    //    The length field counts itself but not the preceding marker bytes.
    uint16_t app1_payload_len = (uint16_t)(JPEG_SEGMENT_LENGTH_SIZE + exif_raw_size);
    out_buffer[out_pos++] = (app1_payload_len >> 8) & 0xFF; // big-endian MSB
    out_buffer[out_pos++] = app1_payload_len & 0xFF;        // big-endian LSB

    // 4. Write the raw EXIF payload (includes "Exif\0\0" + TIFF header + IFDs).
    memcpy(out_buffer + out_pos, exif_raw_data, exif_raw_size);
    out_pos += exif_raw_size;

    // 5. Copy the remainder of the original image, skipping any replaced bytes.
    size_t tail_start = insert_pos + replace_len;
    memcpy(out_buffer + out_pos, in_buffer + tail_start, in_size - tail_start);
    out_pos += in_size - tail_start;

    free(exif_raw_data);
    return out_pos;
}

} // extern "C"
