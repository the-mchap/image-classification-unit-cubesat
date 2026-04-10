/**
 * @file obc_tasks.h
 *
 * @brief Task handlers for commands received from the OBC.
 *
 * @details Each function maps 1:1 to an @ref ObcRxCommand_e value and is
 *          called by the parser after CRC validation. Functions that
 *          interact with the RPi forward commands via
 *          @ref construct_and_send; storage operations go through the
 *          @ref storage layer.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2025-07-08
 *
 * @ingroup app
 */
#ifndef OBC_TASKS_H
#define OBC_TASKS_H

/** @name RPi Power & Request Commands
 * @{
 */
void rpi_on(Dataframe *module);
void rpi_off(Dataframe *module);
void rpi_request_poweroff(Dataframe *module);
void rpi_request_reboot(Dataframe *module);
void rpi_request_capture(Dataframe *module);
void rpi_request_image(Dataframe *module, bool is_full_quality);
void rpi_report_request(Dataframe *module);
/** @} */

/** @name OBC Flash Write Commands
 * @{
 */
void obc_write_pointer(Dataframe *module);
void obc_write_meta(Dataframe *module);
void obc_write_from_to(Dataframe *module);
/** @} */

/** @name Deletion Commands
 * @{
 */
void delete_range(Dataframe *module);
void delete_address(Dataframe *module);
void delete_metadata(Dataframe *module);
/** @} */

#endif /* OBC_TASKS_H */
