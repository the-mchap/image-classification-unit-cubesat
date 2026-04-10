/**
 * @file metadata.h
 *
 * @brief EXIF UserComment extraction and metadata matching for
 *        JPEG images stored in flash.
 *
 * @details Parses APP1/EXIF segments directly from flash (via a small
 *          read cache) to pull the 7-char classification string baked
 *          into each image's UserComment field by the RPi. Also provides
 *          a comparator to match uplinked compact metadata codes against
 *          those extracted strings.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-07
 *
 * @ingroup storage
 */
#ifndef METADATA_H
#define METADATA_H

/**
 * @brief Extract up to 7 ASCII bytes from a JPEG's EXIF UserComment.
 *
 * @details Walks the APP1 segment, locates the ExifIFD pointer, then
 *          finds the UserComment tag. Reads are cached internally to
 *          minimize SPI transactions.
 *
 * @param[in]  jpeg_base_addr  Absolute flash address of the first JPEG byte.
 * @param[in]  jpeg_size       Total JPEG size in bytes.
 * @param[out] comment_out     Output buffer (>= 8 bytes: 7 chars + NUL).
 *
 * @return Bytes copied (1..7), 0 if the tag is absent, -1 on error.
 *
 * @see match_metadata_code
 */
int metadata_extract_user_comment(uint32_t jpeg_base_addr, uint32_t jpeg_size,
                                  char *comment_out);

/**
 * @brief Compare a 5-byte compact uplinked code against a 7-char
 *        flash-extracted UserComment string.
 *
 * @details Compact layout (5 bytes): `{count, class[0..2], confidence}`.
 *          Flash layout (7 chars): `"CCLLLDPP"` -- count hex, class ASCII,
 *          confidence hex.
 *
 * @param[in] compact    5-byte uplinked metadata code.
 * @param[in] flash_str  7-char string from flash (NUL not required).
 *
 * @return @c true if both represent the same metadata.
 *
 * @see metadata_extract_user_comment, nibble_to_hex
 */
bool match_metadata_code(uint8_t *compact, char *flash_str);

#endif /* METADATA_H */
