/**
 * @file pin_map.h
 *
 * @brief PIC18F67J94 I/O pin-to-function assignments.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup globals
 */
#ifndef PIN_MAP_H
#define PIN_MAP_H

/** @name Pin Assignments
 *
 * Directly mapped to the PIC18F67J94 I/O pins.
 *
 * | Define   | Pin     | Function                                  |
 * |----------|---------|-------------------------------------------|
 * | SS2      | PIN_D7  | !CE2 -- Local Flash Memory (LFM)          |
 * | SS1      | PIN_A2  | !CE1 -- OBC Flash Memory (OBCFM)          |
 * | RPI_EN   | PIN_C3  | Raspberry Pi power supply enabler         |
 * | MUX_EN   | PIN_B3  | MUX !CE                                   |
 * | MUX_S    | PIN_B4  | MUX channel select (1-bit, 2 channels)    |
 * | LED_1    | PIN_E3  | Test LED 1                                |
 * | LED_2    | PIN_E2  | Test LED 2                                |
 * @{
 */
#define SS2 PIN_D7    /**< !CE2 -- Local Flash Memory (LFM). */
#define SS1 PIN_A2    /**< !CE1 -- OBC Flash Memory (OBCFM). */
#define RPI_EN PIN_C3 /**< Raspberry Pi power supply enabler. */
#define MUX_EN PIN_B3 /**< MUX !CE. */
#define MUX_S PIN_B4  /**< MUX channel select, 1-bit, 2 channels. */
#define LED_1 PIN_E3  /**< Test LED 1. */
#define LED_2 PIN_E2  /**< Test LED 2. */
/** @} */

#endif /* PIN_MAP_H */
