/**
 * @file main.c
 *
 * @brief ICU firmware entry point — main-loop executive.
 *
 * @details After @ref setup, the main loop polls each channel's
 *          @ref Dataframe_s flag and dispatches complete frames to
 *          @ref frame_parsing. Timeout recovery runs every iteration
 *          via @ref process_rx_timeout_events.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-03
 *
 * @see setup, frame_parsing, process_rx_timeout_events
 */
#include "globals.h"

#include "./include/drivers/uart.h"
#include "./include/app/parser.h"
#include "./include/drivers/setup.h"

/**
 * @brief Firmware entry point.
 *
 * @details Runs @ref setup once, then enters an infinite polling loop.
 *          Each iteration:
 *          1. @ref process_rx_timeout_events — recover stale frames.
 *          2. Check @c obc.flag -> @ref frame_parsing.
 *          3. Check @c rpi.flag -> @ref frame_parsing.
 *          4. Check @c dbg.flag -> @ref frame_parsing.
 *             (@ref DEBUG_MODE only).
 *
 * @return
 */
int main(void)
{
    setup();

    while (true)
    {
        process_rx_timeout_events();

        if (obc.flag)
        {
            frame_parsing(&obc);
        }

        if (rpi.flag)
        {
            frame_parsing(&rpi);
        }

#ifdef DEBUG_MODE
        if (dbg.flag)
        {
            frame_parsing(&dbg);
        }
#endif /* DEBUG_MODE */
    }

    return 0;
}
/*
 * Earth below us, drifting, falling
 * Floating weightless, coming, coming home
 *              --Peter Schilling, 1983 **/
