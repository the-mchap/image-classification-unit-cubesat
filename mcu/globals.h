/**
 * @file globals.h
 *
 * @brief Project-wide definitions, pin mappings, and hardware peripheral
 *        configuration for the PIC18F67J94-based ICU.
 *
 * @details This header is the single point of truth for every global
 *          constant, type, and hardware peripheral setup. It is included
 *          (directly or transitively) by every compilation unit.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-03
 */
#ifndef GLOBALS_H
#define GLOBALS_H

#include <18F67J94.h>

#device PASS_STRINGS = IN_RAM

/* #use delay(crystal = 16MHz)  // This will be it in production. */
#fuses PR_PLL, PLL4, NOWDT
#use delay(clock = 64000000, crystal = 16000000)

/**
 * @defgroup globals Global Definitions
 * @brief Project-wide constants, types, pin mappings, and peripheral setup.
 * @{
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/**
 * @def DEBUG_MODE
 * @brief Enables debug-only code paths (DBG UART, extra logging, LEDs).
 *
 * @see dbg_tasks.c for the debug command handlers this flag unlocks.
 */
#define DEBUG_MODE

#include "./include/logger.h"
#include "./include/drivers/pin_map.h"
#include "./include/protocol/frame.h"
#include "./include/protocol/endian.h"
#include "./include/flash/flash_types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif /* NULL */

#define BITS_PER_BYTE 8

/* =================== Hardware Peripheral Configuration =================== */

/**
 * @defgroup globals_hw Hardware Peripheral Configuration
 * @brief CCS C compiler directives for UART and SPI pin selection
 *        and peripheral instantiation.
 * @{
 */

/** @name Debug UART (UART1)
 * @anchor uart_debug_config
 * User's device COM port (Serial Terminal) & ICU_MCU -- Debug only.
 * @{
 */
#ifdef DEBUG_MODE
#pin_select U1TX = PIN_G2
#pin_select U1RX = PIN_G3
#use rs232(UART1, baud = 115200, parity = N, bits = 8, ERRORS, stream = DBG_STREAM)
#endif /* DEBUG_MODE */
/** @} */

/** @name OBC UART (UART2)
 * @anchor uart_obc_config
 * OBC & ICU_MCU link -- 9600 baud.
 * @{
 */
#pin_select U2TX = PIN_G1
#pin_select U2RX = PIN_G0
#use rs232(UART2, baud = 9600, parity = N, bits = 8, ERRORS, stream = OBC_STREAM)
/** @} */

/** @name RPI UART (UART3)
 * @anchor uart_rpi_config
 * RPi & ICU_MCU link -- 115200 baud.
 * @{
 */
#pin_select U3TX = PIN_C4
#pin_select U3RX = PIN_C5
#use rs232(UART3, baud = 115200, parity = N, bits = 8, ERRORS, stream = RPI_STREAM)
/** @} */

/** @name SPI Buses
 * @anchor spi_bus_config
 * @{
 */
/** SPI1 (ICU_MCU & OBCFM) -- SS1 = PIN_A2 (!CE1). */
#pin_select SDI1 = PIN_A0
#pin_select SDO1 = PIN_A1
#pin_select SCK1OUT = PIN_A3
#use spi(MASTER, SPI1, MODE = 0, BITS = 8, MSB_FIRST, STREAM = OBCFM)

/** SPI2 (ICU_MCU & LFM) -- SS2 = PIN_D7 (!CE2). */
#pin_select SDI2 = PIN_B1
#pin_select SDO2 = PIN_B0
#pin_select SCK2OUT = PIN_D6
#use spi(MASTER, SPI2, MODE = 0, BITS = 8, MSB_FIRST, STREAM = LFM)

/** @} */

/** @} */ /* globals_hw */

/** @} */ /* globals */

#endif /* GLOBALS_H */
