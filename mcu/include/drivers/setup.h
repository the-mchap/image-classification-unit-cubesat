/**
 * @file setup.h
 *
 * @brief Power-on initialization: pin config, interrupts, flash mode,
 *        and debug login banner.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup app
 */
#ifndef SETUP_H
#define SETUP_H

/**
 * @brief Run all power-on initialization (outputs, interrupts, flash,
 *        frames, and debug banner).
 *
 * @see setup_flash, ready_cue
 */
void setup(void);

/**
 * @brief Print the available command set to the debug terminal.
 *
 * @note Only meaningful under @ref DEBUG_MODE.
 */
void ready_cue(void);

#endif /* SETUP_H */
