"""EXIF metadata injector. C++ extension for speed with piexif as a pure-Python fallback."""

import os
import piexif
import io
import ctypes
from .. import config

logger = config.logging.get_logger(__name__)

# --- C++ Extension Loading ---
_lib_path = os.path.join(
    os.path.dirname(os.path.dirname(__file__)), "src", "libmetadata_injector.so"
)
_lib = None
if os.path.exists(_lib_path):
    try:
        _lib = ctypes.CDLL(_lib_path)
        # size_t inject_metadata(const uint8_t *in_buffer, size_t in_size, ... uint8_t *out_buffer, size_t out_max_size)
        _lib.inject_metadata.argtypes = [
            ctypes.c_char_p,  # in_buffer (can accept bytes directly)
            ctypes.c_size_t,  # in_size
            ctypes.c_char_p,  # software
            ctypes.c_char_p,  # date_time
            ctypes.c_char_p,  # user_comment
            ctypes.POINTER(ctypes.c_uint8),  # out_buffer
            ctypes.c_size_t,  # out_max_size
        ]
        _lib.inject_metadata.restype = ctypes.c_size_t
    except Exception as e:
        logger.warning(f"Failed to load C++ metadata extension: {e}")
        _lib = None


def append_metadata(
    image_bytes: bytes,
    metadata_string: str,
    software_tag: str,
) -> bytes:
    """Adds compact metadata string to image bytes using C++ extension or piexif fallback."""
    # Standard EXIF date format: YYYY:MM:DD HH:MM:SS
    import time

    exif_time = time.strftime("%Y:%m:%d %H:%M:%S")

    if _lib:
        try:
            in_len = len(image_bytes)
            # Standard string arguments
            sw_ptr = software_tag.encode("utf-8")
            dt_ptr = exif_time.encode("utf-8")
            uc_ptr = metadata_string.encode("utf-8")

            # 1. Dry Run: Get the exact size required for the output
            needed_size = _lib.inject_metadata(
                image_bytes, in_len, sw_ptr, dt_ptr, uc_ptr, None, 0
            )

            if needed_size > 0:
                # 2. Allocate exact buffer
                out_buffer = (ctypes.c_uint8 * needed_size)()

                # 3. Perform actual injection
                new_len = _lib.inject_metadata(
                    image_bytes,
                    in_len,
                    sw_ptr,
                    dt_ptr,
                    uc_ptr,
                    out_buffer,
                    needed_size,
                )

                if new_len > 0:
                    return bytes(out_buffer)

            logger.warning("C++ metadata injection failed, falling back to piexif")
        except Exception as e:
            logger.error(f"Error in C++ metadata injection: {e}", exc_info=True)

    # Fallback to piexif
    logger.debug(f"Adding compact metadata: {metadata_string}")
    try:
        # Load existing exif or create a fresh structure
        try:
            exif_dict = piexif.load(image_bytes)
        except Exception:
            exif_dict = {
                "0th": {},
                "Exif": {},
                "GPS": {},
                "Interop": {},
                "1st": {},
                "thumbnail": None,
            }

        # Update IFDs
        exif_dict["0th"].update(
            {
                piexif.ImageIFD.Software: software_tag.encode("utf-8"),
                piexif.ImageIFD.DateTime: exif_time.encode("utf-8"),
            }
        )

        exif_dict["Exif"].update(
            {
                piexif.ExifIFD.UserComment: b"ASCII\x00\x00\x00"
                + metadata_string.encode("utf-8"),
                piexif.ExifIFD.DateTimeOriginal: exif_time.encode("utf-8"),
            }
        )

        # Insert back into image
        output = io.BytesIO()
        piexif.insert(piexif.dump(exif_dict), image_bytes, output)
        return output.getvalue()

    except Exception as e:
        logger.error(f"Error adding EXIF metadata with piexif: {e}", exc_info=True)
        return image_bytes
