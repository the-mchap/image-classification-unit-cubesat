System Architecture
===================

Hardware Block Diagram
----------------------

.. graphviz::

   digraph system {
       rankdir=LR
       node [shape=box, style="rounded,filled", fontname="monospace", fontsize=10]
       edge [fontname="monospace", fontsize=9]

       subgraph cluster_rpi {
           label="Raspberry Pi Zero 2W"
           style="dashed"
           color="gray"
           fontname="monospace"

           orch [label="Orchestrator\n(Event Loop)", fillcolor="#d4e6f1"]
           pipe [label="Pipeline\n(Capture + Classify)", fillcolor="#d5f5e3"]
           nn [label="Neural Network\n(TFLite)", fillcolor="#d5f5e3"]
           db [label="Database\n(SQLite)", fillcolor="#fdebd0"]
           proto [label="UART Protocol\n(Rx/Tx)", fillcolor="#fadbd8"]
       }

       cam [label="HQ Camera\n(12MP, CSI)", fillcolor="#e8daef"]
       flash [label="128MiB NOR Flash\n(SPI)", fillcolor="#fdebd0"]
       mcu [label="PIC18F67J94\n(MCU)", fillcolor="#fadbd8"]
       sd [label="MicroSD\n(OS + Images)", fillcolor="#fdebd0"]

       cam -> pipe [label="  JPEG + lores"]
       pipe -> nn [label="  224x224 RGB"]
       pipe -> db [label="  classified image"]
       db -> sd [label="  .jpg files"]
       orch -> pipe [label="  CAM_ON"]
       orch -> proto [label="  commands"]
       proto -> mcu [label="  UART 115200bps"]
       mcu -> proto [label="  UART 115200bps"]
       orch -> flash [label="  SPI write"]
       flash -> orch [label="  write status"]
   }


Boot Sequence
-------------

.. mermaid::

   flowchart TD
       A[python3 main.py] --> B{hardware_sampling\nenabled?}
       B -->|yes| C[Start hardware sampler]
       B -->|no| D[bootstrap_system]
       C --> D
       D --> E[Init UART + Protocol]
       E --> F[Init Database]
       F --> G[Init SPI Flash]
       G --> H[Init Camera]
       H --> I[Init Neural Network]
       I --> J[Send BOOT_READY to MCU]
       J --> K[start_event_loop]


Event Loop
----------

The orchestrator runs a non-blocking loop that polls for UART commands
while monitoring background flash writes and pinging the watchdog.

.. mermaid::

   flowchart TD
       A[Tick Watchdog] --> B[Flush TX Queue]
       B --> C[Monitor Flash Write]
       C --> D{RX frame\navailable?}
       D -->|yes| E[Dispatch Command]
       D -->|no| F[Sleep IDLE_SLEEP]
       E --> A
       F --> A

       C --> C1{Write complete?}
       C1 -->|succeeded| C2[Queue WRITE_END]
       C1 -->|failed| C3[Log error]
       C1 -->|in progress| D


Command Dispatch
----------------

Commands received from the MCU are dispatched to the appropriate handler.

.. mermaid::

   flowchart LR
       RX[MCU Frame] --> PARSE[RxProtocol.get_command]
       PARSE --> DISPATCH{Command?}

       DISPATCH -->|CAM_ON| H1[capture_mission]
       DISPATCH -->|WRITE_FROM| H2[handle_image_on_flash]
       DISPATCH -->|ABORT_WRITE| H3[handle_abort_write]
       DISPATCH -->|REPORT| H4[send_database_report]
       DISPATCH -->|THUMBNAIL_CODE\nFULL_CODE| H5[handle_image_request]
       DISPATCH -->|POWEROFF\nREBOOT| H6[shutdown / reboot]


Image Capture Pipeline
----------------------

.. mermaid::

   sequenceDiagram
       participant MCU
       participant Orchestrator
       participant Camera
       participant NeuralNetwork
       participant MetadataInjector
       participant Database
       participant MicroSD

       MCU->>Orchestrator: CAM_ON (limit=N)
       loop For each capture (up to N)
           Orchestrator->>Camera: capture_dual()
           Camera-->>Orchestrator: JPEG bytes + 224x224 RGB
           Orchestrator->>NeuralNetwork: predict(lores_array)
           NeuralNetwork-->>Orchestrator: classification, confidence
           alt Valid class (LAND/SKY/WATER)
               Orchestrator->>MetadataInjector: append_metadata(jpeg, metadata)
               MetadataInjector-->>Orchestrator: JPEG with EXIF
               Orchestrator->>Database: save_image(jpeg, class, confidence)
               Database->>MicroSD: write .jpg file
           else Invalid class (BAD)
               Note over Orchestrator: Image discarded
           end
       end


SPI Flash Write Sequence
------------------------

When the MCU requests an image to be written to flash, the write runs
in a background thread while the orchestrator remains responsive.

.. mermaid::

   sequenceDiagram
       participant MCU
       participant Orchestrator
       participant FlashMemory
       participant SPIWorker

       MCU->>Orchestrator: WRITE_FROM (image index)
       Orchestrator->>FlashMemory: write_bytes_async(address, data)
       FlashMemory->>SPIWorker: spawn thread

       Note over SPIWorker: Alignment pass (partial first page)
       Note over SPIWorker: Bulk pass (full 256-byte pages)
       Note over SPIWorker: Leftover pass (partial last page)

       loop Each page
           SPIWorker->>FlashMemory: WREN + PAGE_PROGRAM (atomic)
           SPIWorker->>FlashMemory: Poll WIP bit until clear
       end

       SPIWorker-->>FlashMemory: write_successful = True
       Orchestrator->>Orchestrator: _monitor_flash_write()
       Orchestrator->>MCU: WRITE_END (end address)
