/**
 * @file dbg_tasks.h
 *
 * @brief Debug-only task handlers for serial terminal testing.
 *
 * @details These extend the OBC command set with test-oriented
 *          operations accessible from a serial terminal (see
 *          @ref DbgRxCommand_e). The entire file is compiled out
 *          when @ref DEBUG_MODE is not defined.
 *
 * @note The author uses [tio](https://github.com/tio/tio), btw.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup app
 */
#ifdef DEBUG_MODE

#ifndef DBG_TASKS_H
#define DBG_TASKS_H

/** @name RPi Debug Controls
 * @{
 */
void rpi_on_dbg(void);
void rpi_off_dbg(void);
void rpi_communication_test(void);
void rpi_send_poweroff(void);
void take_a_pic(void);
void request_photo(bool is_thumbnail);
void debug_report_request(void);
/** @} */

/** @name Flash Debug Operations
 * @{
 */
void debug_read_from(void);
void debug_log_entry(void);
void debug_log_image_data(void);
void debug_write(void);
void debug_write_dma(void);
void debug_metadata(void);
void debug_delete(void);
void erase_manual(void);
void read_image(uint32_t addr_1st, uint32_t addr_end);
/** @} */

#endif /* DBG_TASKS_H */

#endif /* DEBUG_MODE */
