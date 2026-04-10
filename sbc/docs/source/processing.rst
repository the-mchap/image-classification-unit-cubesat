Data Processing
===============

UART Protocol
-------------

.. automodule:: modules.processing.protocol
   :members:

Database
--------

.. automodule:: modules.processing.database
   :members:

Neural Network
--------------

.. automodule:: modules.processing.neural_network
   :members:

CRC-16
------

.. automodule:: modules.processing.crc
   :members:

The CRC-16 uses a table-driven algorithm with polynomial ``0x1021`` and
seed ``0x1D0F``. The C++ extension provides ~2300x speedup over the
Python fallback for 4KB blocks.

.. tabs::

   .. tab:: C++ (libcrc16.so)

      .. code-block:: cpp

         // Table-driven CRC-16, polynomial 0x1021, seed 0x1D0F
         extern "C"
         uint16_t calculate_crc16(const uint8_t *data, size_t length) {
             uint16_t crc = 0x1d0f;
             for (size_t i = 0; i < length; ++i) {
                 uint8_t table_index = ((crc >> 8) ^ data[i]) & 0xff;
                 crc = crc_table[table_index] ^ (crc << 8);
             }
             return crc;
         }

   .. tab:: Python fallback

      .. code-block:: python

         # Bitwise CRC-16 — used when the .so is not available
         def calculate_bitwise(data: bytes) -> int:
             crc = 0x1D0F
             for byte in data:
                 crc ^= byte << 8
                 for _ in range(8):
                     if crc & 0x8000:
                         crc = (crc << 1) ^ 0x1021
                     else:
                         crc <<= 1
                     crc &= 0xFFFF
             return crc

Metadata
--------

.. automodule:: modules.processing.metadata
   :members:

EXIF metadata is injected directly into the JPEG byte buffer in RAM.
The C++ extension uses ``libexif`` for robust JPEG marker parsing with
a two-pass dry-run pattern for zero-waste memory allocation. The Python
fallback uses ``piexif``.

.. tabs::

   .. tab:: C++ (libmetadata_injector.so)

      Two-pass API — first call with ``out_buffer = nullptr`` to get the
      required size, then allocate and call again:

      .. code-block:: cpp

         // Dry run: get required output size
         size_t needed = inject_metadata(
             jpg, jpg_len, software, datetime, comment,
             nullptr, 0  // out_buffer = NULL -> returns needed size
         );

         // Allocate and inject
         uint8_t *buf = (uint8_t *)malloc(needed);
         size_t written = inject_metadata(
             jpg, jpg_len, software, datetime, comment,
             buf, needed
         );

   .. tab:: Python fallback (piexif)

      .. code-block:: python

         # In-memory EXIF injection using piexif
         exif_dict = {"0th": {}, "Exif": {}}
         exif_dict["0th"][piexif.ImageIFD.Software] = software_tag
         exif_dict["0th"][piexif.ImageIFD.DateTime] = date_time
         exif_dict["Exif"][piexif.ExifIFD.UserComment] = metadata

         exif_bytes = piexif.dump(exif_dict)
         return piexif.insert(exif_bytes, image_bytes)

Thumbnail
---------

.. automodule:: modules.processing.thumbnail
   :members:
