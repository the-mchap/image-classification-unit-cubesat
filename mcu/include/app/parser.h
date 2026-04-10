/**
 * @file parser.h
 *
 * @brief Central command router -- dispatches validated frames to their
 *        OBC, RPi, or DBG task handlers.
 *
 * @details Receives a @ref Dataframe_s whose @c flag is set, validates
 *          its CRC, checks for ACK/NACK, then routes by @ref UNIT_ID
 *          and @ref CMD_ID to the appropriate task function.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-08
 *
 * @ingroup app
 */
#ifndef PARSER_H
#define PARSER_H

/**
 * @defgroup app Application Layer
 * @brief Frame parsing, power-on setup, and per-channel task handlers.
 *
 * @see @ref protocol for CRC validation and frame construction.
 * @see @ref driver_uart for the ISRs that feed frames into this layer.
 * @{
 */

/**
 * @brief Parse and route an incoming frame to its handler.
 *
 * @param[in] module  @ref Dataframe_s with a complete, flagged frame.
 *
 * @pre @ref IS_FRAME_COMPLETE must have evaluated true for this module.
 *
 * @see evaluate_dataframe, obc_tasks.h, rpi_tasks.h, dbg_tasks.h
 */
void frame_parsing(Dataframe *module);

/** @} */ /* app */

#endif /* PARSER_H */
