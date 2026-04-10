UART Protocol Reference
=======================

This page documents the binary protocol between the Raspberry Pi Zero 2W
and the PIC18F67J94 MCU over UART at 115200 bps.

Frame Structure
---------------

All frames follow this byte layout:

.. code-block:: text

   +---------+---------+----------------+-----------+----------+-----------+-----------+
   | HEADER  | COMMAND | PAYLOAD_LENGTH | PAYLOAD   | CRC_MSB  | CRC_LSB   | STOP_BYTE |
   | 0x3E    | 1 byte  | 1 byte (0-39)  | 0-39 bytes| 1 byte   | 1 byte    | 0x0A      |
   +---------+---------+----------------+-----------+----------+-----------+-----------+

   Total frame size: 6 bytes (no payload) to 45 bytes (max payload).
   Fixed overhead: 6 bytes (HEADER + COMMAND + LENGTH + CRC_MSB + CRC_LSB + STOP).

Special Frames
--------------

These frames are absolute constants — they never change.

.. list-table::
   :header-rows: 1
   :widths: 20 50 30

   * - Name
     - Bytes
     - Meaning
   * - **ACK**
     - ``[0x3E, COMMAND, 0x00, CRC_MSB, CRC_LSB, 0x0A]``
     - Mirrors the sent command and its CRC
   * - **NACK_CRC**
     - ``[0x3E, 0xFF, 0x00, 0xFF, 0xFF, 0x0A]``
     - Checksum validation failed
   * - **NACK_BAD_FRAME**
     - ``[0x3E, 0x00, 0x00, 0x00, 0x00, 0x0A]``
     - Malformed or unrecognizable frame

Command Reference
-----------------

MCU to RPi (RxCommand)
^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 25 10 65

   * - Command
     - Code
     - Description
   * - ``CAM_ON``
     - ``0x32``
     - Start a capture mission. Payload byte 0 = number of captures (default 1).
   * - ``WRITE_FROM``
     - ``0x30``
     - Write an image to SPI flash. Payload contains the image index.
   * - ``ABORT_WRITE``
     - ``0x31``
     - Cancel an in-progress flash write.
   * - ``REPORT``
     - ``0x35``
     - Request a database status report (image counts by class).
   * - ``THUMBNAIL_CODE``
     - ``0x36``
     - Request a thumbnail for a specific image.
   * - ``FULL_CODE``
     - ``0x37``
     - Request the full image data for a specific image.
   * - ``POWEROFF``
     - ``0x33``
     - Shut down the RPi cleanly.
   * - ``REBOOT``
     - ``0x34``
     - Reboot the RPi.

RPi to MCU (TxCommand)
^^^^^^^^^^^^^^^^^^^^^^

.. list-table::
   :header-rows: 1
   :widths: 25 10 65

   * - Command
     - Code
     - Description
   * - ``BOOT_READY``
     - ``0x43``
     - Sent once after initialization. RPi is ready for commands.
   * - ``WRITE_REQ``
     - ``0x40``
     - Image metadata sent to MCU before a flash write.
   * - ``WRITE_BEG``
     - ``0x41``
     - Flash write has started at the given address.
   * - ``WRITE_END``
     - ``0x42``
     - Flash write completed. Payload contains the end address.
   * - ``PWROFF_ACK``
     - ``0x44``
     - RPi acknowledged power-off and is shutting down.
   * - ``DB_REPORT``
     - ``0x45``
     - Database telemetry: image counts by classification.

Communication Examples
----------------------

Boot and Capture
^^^^^^^^^^^^^^^^

.. mermaid::

   sequenceDiagram
       participant MCU
       participant RPi

       Note over RPi: Power on, init hardware
       RPi->>MCU: BOOT_READY (0x43)
       MCU->>RPi: ACK

       MCU->>RPi: CAM_ON (0x32), payload=[3]
       RPi->>MCU: ACK
       Note over RPi: Capture 3 images,<br/>classify, store valid ones

       MCU->>RPi: REPORT (0x35)
       RPi->>MCU: ACK
       RPi->>MCU: DB_REPORT (0x45), payload=[counts]

Flash Write Flow
^^^^^^^^^^^^^^^^

.. mermaid::

   sequenceDiagram
       participant MCU
       participant RPi
       participant Flash

       MCU->>RPi: WRITE_FROM (0x30), payload=[image_index]
       RPi->>MCU: ACK
       RPi->>MCU: WRITE_REQ (0x40), payload=[metadata]
       MCU->>RPi: ACK
       RPi->>MCU: WRITE_BEG (0x41), payload=[start_address]
       RPi->>Flash: write_bytes_async()
       Note over Flash: Background SPI write
       Flash-->>RPi: Complete
       RPi->>MCU: WRITE_END (0x42), payload=[end_address]
       MCU->>RPi: ACK

CRC-16 Checksum
---------------

The CRC covers bytes from COMMAND through the last PAYLOAD byte (excludes
HEADER, CRC, and STOP). Computed using CRC-16 with a table-driven C++
implementation for speed (see :ref:`processing:CRC-16`).
