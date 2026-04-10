/**
 * @file rpi_command.h
 *
 * @brief All RPi in-and-out commands (TxOrRxBuff[CMD_ID]) with their
 *        usage doc.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @warning To add more commands to @ref RpiTxCommand_e, make sure any of its command
 * overlay with @ref RpiRxCommand_e values. Tx and Rx command of the same stream/channel
 * must not be equal because of how the internal frame protocol (specifically ACK) works.
 *
 * @date 2026-03-02
 *
 * @ingroup app
 */
#ifndef RPI_COMMAND_H
#define RPI_COMMAND_H

/**
 * @enum RpiTxCommand_e
 * @brief ICU-to-RPi transmission commands.
 * @note I suggest placing every flash-borrower™ function adjacently if more is added to
 * this list.
 * @see IS_FLASH_BORROWING(tx_cmd)
 */
typedef enum RpiTxCommand_e
{
    ACCEPT_ACTION = 0x30,  ///< Accept operation -- RPi request acknowledged.
    DECLINE_ACTION,        ///< Decline operation -- RPi request rejected.
    SUDO_POWEROFF,         ///< Order RPi to power off politely.
    SUDO_REBOOT,           ///< Order RPi to reboot politely.
    CAPTURE_PHOTO,         ///< Trigger camera capture.
    REQUEST_REPORT,        ///< Request database report to downlink to Ground.
    TRANSFER_LQ,           ///< Request low quality image.
    TRANSFER_HQ            ///< Request full-resolution image.
} RpiTxCommand;

/**
 * @enum RpiRxCommand_e
 * @brief RPi-to-ICU reception commands.
 */
typedef enum RpiRxCommand_e
{
    TRANSFER_REQ = 0x40,  ///< Write request -- RPi wants to store data.
    TRANSFER_BEG,         ///< Write begin -- data transfer starting.
    TRANSFER_END,         ///< Write end -- data transfer complete.
    BOOT_ASSERTION,       ///< RPi has booted and is ready.
    PWROFF_ASSERTION,     ///< RPi confirms it is powering off.
    DATABASE_REPORT       ///< RPi database report payload incoming.
} RpiRxCommand;

/**
 * @def IS_FLASH_BORROWING(tx_cmd)
 * @brief True if @a tx_cmd is a Flash-borrowing RPi command (image transfer).
 */
#define IS_FLASH_BORROWING(tx_cmd) ((tx_cmd) >= TRANSFER_LQ && (tx_cmd) <= TRANSFER_HQ)

#endif /* RPI_COMMAND_H */
