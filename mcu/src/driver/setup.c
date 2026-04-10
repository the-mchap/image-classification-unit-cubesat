/**
 * @file setup.c
 *
 * @brief Power-on initialization: GPIO, interrupts, flash, frame state,
 *        and debug banner.
 *
 * @details Called once from @c main after reset. Configures every I/O
 *          port, sets up all timers and UART interrupts, initialises
 *          flash SPI mode, and flushes the receive frame buffers.
 *          Under @ref DEBUG_MODE an ASCII art banner and the command
 *          cheat-sheet are printed to the debug terminal.
 *
 * @author [the-mchap](https://github.com/the-mchap)
 *
 * @ingroup app
 */
#include "globals.h"

#include "../../include/drivers/setup.h"
#include "../../include/drivers/pin_map.h"
#include "../../include/drivers/flash.h"
#include "../../include/drivers/uart.h"

/* ─────────────────── Internal configuration (do not export) ─────────────────── */

/** @brief Configure GPIO direction and default output levels. */
#inline
static void setup_outputs(void);

/** @brief Configure timers and enable/disable UART + global interrupts. */
#inline
static void setup_interrupts(void);

/** @brief Flush all @ref Dataframe_s instances to a clean state. */
#inline
static void init_frame(void);

#ifdef DEBUG_MODE
/** @brief Print the ICU ASCII art login banner. */
#inline
static void print_login(void);
#endif /* DEBUG_MODE */

/* ──────────── Internal configuration end ──────────── */

/* ──────────── Public ──────────── */

/**
 * @brief Run all power-on initialization.
 *
 * @details Execution order:
 *          1. GPIO outputs (@ref setup_outputs).
 *          2. Timer + interrupt config (@ref setup_interrupts).
 *          3. Flash SPI mode (@ref setup_flash).
 *          4. Frame buffers (@ref init_frame).
 *          5. Debug banner + command list (@ref DEBUG_MODE only).
 *
 * @see setup_flash, ready_cue
 */
void setup(void)
{
    /* Just comment this if no PLL involved: */
    // setup_oscillator(OSC_PRIMARY_PLL | OSC_CPDIV_1 | OSC_PLL_ON);
    setup_outputs();
    setup_interrupts();
    setup_flash();

    init_frame();

#ifdef DEBUG_MODE
    print_login();
    ready_cue();
#endif /* DEBUG_MODE */
}

/**
 * @brief Configure timers and enable UART + global interrupts.
 *
 * @details Timer0 configuration for ~1 s interrupt:
 *
 *          | Crystal    | Fs            | Prescaler | Period                |
 *          |------------|---------------|-----------|-----------------------|
 *          | 16 MHz     | Fosc/4=4 MHz  | 1:64      | 64 x 65536/4M ≈ 1 s   |
 *          | PLL 64 MHz | Fosc/4=16 MHz | 1:256     | 256 x 65536/16M ≈ 1 s |
 */
#inline
static void setup_interrupts(void)
{
    setup_timer_0(RTCC_INTERNAL | RTCC_DIV_256); /* RTCC_DIV_64 if no PLL. */
    setup_timer_1(T1_INTERNAL | T1_DIV_BY_8);    /* For later. */
    setup_timer_3(T3_INTERNAL | T3_DIV_BY_1);    /* Preconfigure T3 for later. */
    setup_timer_5(T5_INTERNAL | T5_DIV_BY_1);    /* T5 for later. */

    enable_interrupts(INT_TIMER0); /* Always enabled. */
    disable_interrupts(INT_TIMER1);
    disable_interrupts(INT_TIMER3);
    disable_interrupts(INT_TIMER5);

    enable_interrupts(INT_RDA2); /* OBC channel. */
    enable_interrupts(INT_RDA3); /* RPi channel. */

#ifdef DEBUG_MODE
    enable_interrupts(INT_RDA);
#else
    disable_interrupts(INT_RDA);
#endif /* DEBUG_MODE */

    enable_interrupts(GLOBAL);
}

#ifdef DEBUG_MODE
/**
 * @brief Print ICU ASCII art login banner to serial terminal.
 */
#inline
static void print_login(void)
{
    logger(
        "\n\r... --- -- . - .. -- . ... -.-- --- ..- --. --- - - .- .-. ..- -. -... ."
        "\n\r..-. --- .-. . -.-- --- ..- -.-. .- -. .-- .- .-.. -.-"
        "\n\r                                                                            "
        "                       ¡"
        "\n\r<---*_____,.____,.., __,,,    .                         .                   "
        "                   !"
        "\n\r   //_,  _/ ____/\\ | | //        --for GS-X"
        "\n\r      |:| |:|    |:| |:||         SpaceLab-GIEM                             "
        "                   ."
        "\n\r      |*| |*|    |*| |*||   * 2024-2026        .                            "
        "               ¡"
        "\n\r   .__|:|_|:|____|:|_|:||"
        "\n\r   \\\\_____\\\\_____/\\_____\\\\*->           * .                          "
        "                   |"
        "\n\r             .             gh: @the-mchap         .");
    logger(
        "\n\r     * |"
        "\n\r .. .-.. --- ...- . -.-- --- ..- ...-- ----- ----- -----                    "
        "                   |"
        "\n\r                               .               * |"
        "\n\r     * .             .               .            ."
        "\n\r                   * ."
        "\n\r        .                                --* ."
        "\n\r           .-.   . | This is Major Tom to Ground Control                    "
        "                   ¡"
        "\n\r          ( (      | I'm stepping through the door                          "
        "                   :"
        "\n\r           `-'     |                 --David Bowie, 1969"
        "\n\r   * .                       .               * |"
        "\n\r             .           *--                                                "
        "                   |"
        "\n\r        .                                    .                              "
        "                   ¡");
    logger(
        "\n\r                    * \\\\_________\\\\"
        "\n\r,______,,                  /           /\\          .          ,,_________  "
        "                    ."
        "\n\r/  -   \\__________________/   GS-X    / \\\\_____________________/   --    "
        "\\                      ¡"
        "\n\r\\       \\       \\        /     PY    / / \\\\          \\          \\    "
        "      \\"
        "\n\r \\       \\       \\      \\\\__________\\\\    \\\\          \\          "
        "\\          \\                     ."
        "\n\r  \\       \\   ---------------====-------------------  \\          \\      "
        "    \\"
        "\n\r   \\      \\\\        \\\\      \\\\ /''''''\" \\\\    \\\\        \\\\    "
        "      \\\\         \\               ¡"
        "\n\r    \\       \\       \\       \\\\\\  _____\\ \\\\    \\\\         \\      "
        "    \\          \\               :"
        "\n\r     \\       \\       \\       \\\\\\ \\.--*.\\ \\\\    \\\\         \\    "
        "      \\          \\"
        "\n\r     \\       \\    ___\\_____ \\\\\\ \\\\(o)\\\\ \\\\  _ \\\\   "
        "______\\_____     \\          \\               ."
        "\n\r      \\       \\   \\________\\ \\\\\\ \\.___.\\ \\\\ \\\\ \\\\  "
        "\\___________\\     \\          \\"
        "\n\r       \\       \\      \\\\      \\\\\\ \\_____\\ \\\\    \\\\         "
        "\\\\         \\          \\             ¡");
    logger(
        "\n\r        \\       \\\\      \\      \\\\  ,____| \\\\    \\\\         \\     "
        "    \\\\          \\"
        "\n\r     "
        "-------------------------------====---------------------------------------     "
        "\\"
        "\n\r          \\       \\       \\      \\\\'|by   '\\ \\\\    \\\\         \\  "
        "        \\          \\"
        "\n\r           \\   -   \\_______\\______\\\\\\   the-\\ \\\\    "
        "\\\\_________\\__________\\    --    \\"
        "\n\r            \\______/        .      \\\\\\mchap  \\ \\\\    \\\\         .  "
        "        \\_________/."
        "\n\r             ..    .                \\\\\\______/  \\\\  / /\\              "
        "      .        . ."
        "\n\r              ..    .      ---------------====-------------------           "
        "  .        ."
        "\n\r               .                       \\\\__________\\\\/   ..             "
        "                .");
    logger("\n\r                                        \\.          .\\     ."
           "\n\r        .      ____                      ..          .."
           "\n\r    |      .-'\"\"p 6o\"\"`-.                 ..          .."
           "\n\r  --*-- .-'6969P'Y.`Y[ ' `-.               .            ."
           "\n\r    | ,']69696b.J6oo_      '`."
           "\n\r    ,' ,69696969696[\"        Y`."
           "\n\r   /   6969696969P            Y6\\   -o-"
           "\n\r  /    Y6969696P'             ]69\\"
           "\n\r :     `Y69'   P              `696:"
           "\n\r :       Y6.oP '- >             Y69:"
           "\n\r |          `Yb  __             `'|"
           "\n\r :            `'d6969bo.          :"
           "\n\r :             d69696969ooo.      ;"
           "\n\r  \\            Y69696969696P     /"
           "\n\r   \\            `Y69696969P     /"
           "\n\r    `.            d69696P'    ,'"
           "\n\r      `.          696PP'    ,' -CJ-"
           "\n\r      * `-.      d6P'    ,-'"
           "\n\r      ,    `-.,,_'__,,.-'"
           "\n\r"
           "\n\r.-.. . - ... ..-. .- -.-. . .. - - .... .. ... .. ... -. --- -"
           "\n\r- .... . .-- --- .-. ... - - .... .. -. --. -.-- --- ..- .... .- ...- . "
           "\n\r-.-. .- ..- --. .... - -- . -.. --- .. -. --."
           "\n\r ");
}

/**
 * @brief Print the available debug command set with hex frame sequences.
 *
 * @warning Exclude from production builds.
 */
void ready_cue(void)
{
    LOG("\n\r\tLocked and loaded for next task!");
}
#endif /* DEBUG_MODE */

/**
 * @brief Flush all receive frame buffers to a clean initial state.
 *
 * @see reset_frame
 */
#inline
static void init_frame(void)
{
    reset_frame(&obc);
    reset_frame(&rpi);

#ifdef DEBUG_MODE
    reset_frame(&dbg);
#endif /* DEBUG_MODE */
}

/**
 * @brief Configure GPIO direction registers and default output levels.
 *
 * @details Pin assignments per port:
 *
 * | Port | Bit | Function        | Dir |
 * |------|-----|-----------------|-----|
 * | A0   |     | SDI1 (SPI1 in)  | IN  |
 * | A1   |     | SDO1 (SPI1 out) | OUT |
 * | A2   |     | SS1             | OUT |
 * | A3   |     | SCK1            | OUT |
 * | B0   |     | SDO2 (SPI2 out) | OUT |
 * | B1   |     | SDI2 (SPI2 in)  | IN  |
 * | B3   |     | MUX_EN          | OUT |
 * | B4   |     | MUX_S           | OUT |
 * | C3   |     | RPI_EN          | OUT |
 * | C4   |     | U3TX            | OUT |
 * | C5   |     | U3RX            | IN  |
 * | D6   |     | SCK2            | OUT |
 * | D7   |     | SS2             | OUT |
 * | E2   |     | LED_2           | OUT |
 * | E3   |     | LED_1           | OUT |
 * | G0   |     | U2RX (OBC)      | IN  |
 * | G1   |     | U2TX (OBC)      | OUT |
 * | G2   |     | U1TX (DBG)      | OUT |
 * | G3   |     | U1RX (DBG)      | IN  |
 *
 * @note SPI/UART TRIS bits match peripheral directions
 *       (SDI/RX=1, SDO/TX/SCK=0).
 */
#inline
static void setup_outputs(void)
{
    /* Port A: A0=SDI1(in) A1=SDO1 A2=SS1 A3=SCK1 | A4-A7 unused */
    set_tris_a(0x01);
    output_a(0x04); /* SS1 high */

    /* Port B: B0=SDO2 B1=SDI2(in) B3=MUX_EN B4=MUX_S | B2,B5-B7 unused */
    set_tris_b(0x02);
    output_b(0x08); /* MUX_EN high */

    /* Port C: C3=RPI_EN C4=U3TX C5=U3RX(in) | C0-C2,C6-C7 unused */
    set_tris_c(0x20);
    output_c(0x00); /* RPI_EN low */

    /* Port D: D6=SCK2 D7=SS2 | D0-D5 unused */
    set_tris_d(0x00);
    output_d(0x80); /* SS2 high */

    /* Port E: E2=LED_2 E3=LED_1 | E0-E1,E4-E7 unused */
    set_tris_e(0x00);
    output_e(0x00); /* LEDs off */

    /* Port F: F0-F1 don't exist in current device | F2-F7 unused */
    set_tris_f(0x00);
    output_f(0x00);

#ifdef DEBUG_MODE     /* Port G: G0=U2RX(in) G1=U2TX G2=U1TX G3=U1RX(in) | G4 unused */
    set_tris_g(0x09); /* 0000 1001 */
#else                 /* Port G: G0=U2RX(in) G1=U2TX | G4 unused */
    set_tris_g(0x01); /* 0000 0001 */
#endif                /* DEBUG_MODE */
    output_g(0x00);
}
/* It's not what you look like
 * When you're doing what you're doing
 * It's what you're doing when you're doing
 * What you look like you're doing!
 * Express yourself
 *            --Charles W. Wright, 1970 **/
