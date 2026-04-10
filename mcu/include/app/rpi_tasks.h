/**
 * @file rpi_tasks.h
 *
 * @brief Task handlers for commands received from the RPi (SBC).
 *
 * @details Covers the RPi-side write lifecycle (space check, flash
 *          lock/unlock, index update), power-off sequencing, and
 *          report forwarding to downlink.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-02-05
 *
 * @ingroup app
 */
#ifndef RPI_TASKS_H
#define RPI_TASKS_H

/** @name RPi Write Lifecycle
 * @{
 */

/** @brief Check for free space in LFM and reply to RPi. */
void check_flash_space(void);

/** @brief Lock flash for an active RPi write session. */
void flash_in_use(void);

/** @brief Unlock flash after RPi write and update the index. */
void update_index(void);
/** @} */

/** @name RPi Command Forwarding
 * @{
 */

/**
 * @brief Forward an OBC-originated task to the RPi.
 *
 * @pre Command must have arrived from OBC first -- RPi never initiates
 *      these.
 */
void task_for_rpi(void);

/** @brief Forward the RPi database report to OBC for downlink. */
void report_to_downlink(void);
/** @} */

/** @name Power Management
 * @{
 */

/**
 * @brief Start the final power-off countdown for the RPi.
 *
 * @details Starts Timer1 to allow a graceful shutdown before cutting
 *          power via @ref RPI_EN.
 *
 * @pre Works safe if a power
 */
void kickoff_rpi_killer(void);
/** @} */

#endif /* RPI_TASKS_H */
