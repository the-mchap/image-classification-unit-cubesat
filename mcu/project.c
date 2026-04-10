/**
 * @file project.c
 *
 * @brief MCU linker script -- imports all compiled object files.
 *
 * @details This file is compiled last (without +EXPORT) to link every
 *          separately compiled unit into the final hex image. Each
 *          @c #import pulls in the relocatable .o file produced by
 *          the corresponding source file's +EXPORT compilation pass.
 *
 * @see Using Multiple Compilation Units Guide.txt
 */

/* ─── Logger ─── */
#import(FILE = logger.o)

/* ─── Drivers ─── */
#import(FILE = uart.o)
#import(FILE = flash.o)
#import(FILE = setup.o)

/* ─── Protocol ─── */
#import(FILE = conversions.o)
#import(FILE = crc.o)

/* ─── Flash (Data Management) ─── */
#import(FILE = image_transfer.o)
#import(FILE = quick_sorter.o)
#import(FILE = metadata.o)
#import(FILE = eraser.o)
#import(FILE = index_manager.o)
#import(FILE = space_finder.o)

/* ─── Application ─── */
#import(FILE = task_helper.o)
#import(FILE = obc_tasks.o)
#import(FILE = rpi_tasks.o)
#import(FILE = dbg_tasks.o)
#import(FILE = parser.o)

/* ─── Entry point ─── */
#import(FILE = main.o)
