/**
 * @file obc_command.h
 *
 * @brief All OBC's in-and-out commands (TxOrRxBuff[CMD_ID]) with their usage doc.
 *
 * @details Pedantic corner:
 *
 *  Let `OBC` be the OBC command set and `DBG` the debug command set,
 *
 *  Therefore, `OBC` ⊂ `DBG`; OBC command set is a subset of debug command set.
 *
 *  Translation: `DBG` has extra test-oriented-stuff.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @date 2026-03-02
 *
 * @ingroup app
 */
#ifndef OBC_COMMAND_H
#define OBC_COMMAND_H

/**
 * @enum ObcTxCommand_e
 * @brief ICU-to-OBC transmission commands.
 */
typedef enum ObcTxCommand_e
{
    DL_REPO = 0x30,  ///< 5-byte-compacted repo to DL to Ground Control to Major Tom™.
    ASSERT_WRI,      ///< Successful write-to-OBC request.
    ASSERT_DEL,      ///< Successful delete request.
    ASSERT_PWROFF,   ///< Unit ready to be powered off, ICU stating "I'm ready to go 🥺".
    NAK_REQ,         ///< Unsuccessful (WRI/DEL) process.
} ObcTxCommand;

/**
 * @enum ObcRxCommand_e
 * @brief OBC Rx command subset expected by ICU. Command naming notation is: <em> action +
 *        module target + emphasis </em>.
 *
 * @note Each of these can be sent directly from Ground, so OBC acts as a middleman. Or
 * automate a subprocess where OBC uses them without a direct Ground intervention.
 */
typedef enum ObcRxCommand_e
{
    PWR_RPI_ON = 0x40,  ///< Flip RPi's power supply on, rise and shine.
    PWR_RPI_OFF,        ///< Cut RPi's power supply, lights out.
    REQ_RPI_OFF,        ///< Politely ask RPi to `sudo poweroff` itself.
    REQ_RPI_RESET,      ///< Ask RPi to `sudo reboot`, power-hungry but effective.
    REQ_RPI_PHCAP,      ///< Request photo capture(s) from RPi [quantity].
    REQ_RPI_HQIMG,      ///< Request full-resolution image by code [code].
    REQ_RPI_LQIMG,      ///< Request thumbnail image by code [code].
    REQ_RPI_REPO,       ///< Request RPi's database report for DL.
    WRI_OBC_PTR,        ///< Write from last address pointer to OBC FM [dst + size].
    WRI_OBC_META,  ///< Write from metadata-matched source to OBC FM [dst + meta + size].
    WRI_OBC_SRC,   ///< Write from explicit source address to OBC FM [dst + src + size].
    DEL_OBC_ADDR,  ///< Delete data by single address lookup [address].
    DEL_OBC_META,  ///< Delete data by metadata search [meta].
    DEL_OBC_RANGE  ///< Delete data by explicit address range [from, to].
} ObcRxCommand;

#ifdef DEBUG_MODE
/**
 * @enum DbgRxCommand_e
 *
 * @brief An Rx extension command subset expected by ICU to debug with a third-party
 * serial terminal software.
 *
 * @note I use [tio](https://github.com/tio/tio), btw (it's FOSS ❤️).
 *       These commands complement the main OBC command set (see @ref ObcRxCommand_e).
 *
 * @todo Tidy up the refs, as Brit-posh as fanciah.
 */
typedef enum DbgRxCommand_e
{
    TEST_RPI_FRAME = 0x50,  ///< Send a test frame to RPi [payload]
    DUMP_LOG_FROM,          ///< Read and dump data from [addr + size]
    SHOW_YA_BONES,          ///< Shows index map and index entry sections
    DUMP_LOG_JPEGS,         ///< Dump storaged images HEX strings
    DUMMY_DUMP,             ///< Write dummy data to flash with given size [size]
    IAMSPEED,               ///< Trigger a DMA-powered write [source + destinatary + size]
    META_DUMP_ME,           ///< Dump all images' metadata stored in flash
    RPI_DB_REPORT,          ///< Request a RPi report. @todo ICU report?
    RUBBER_ON_DELETE,  ///< Nuke data securely (internal-safe-bounded nets) [from, to].
    RAW_DELETE,        /**< Hard-nuke flash (prophylacticless)
                            [address + @ref FlashEraseCommand_e]. */
} DbgRxCommand;
#endif /* DEBUG_MODE */

#endif /* OBC_COMMAND_H */
