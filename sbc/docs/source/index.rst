Image Classification Unit (ICU) Documentation
==============================================

Flight software for the CubeSat Image Classification Unit,
running on a Raspberry Pi Zero 2W. Captures images, classifies
them with a TFLite neural network, and downlinks selected images
over UART/SPI to the mission MCU.

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   overview
   architecture
   protocol
   config
   core
   processing
   hardware
   debug
