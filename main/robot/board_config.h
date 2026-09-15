#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

/*
 * MP4-ESP32-CORE  —  board pin map for the robot HAL
 * Source: MP4ESP32_CORE_SCH_0827.pdf (2026-08-27), sheets 01..03.
 *
 * Module: ESP32-S3-WROOM-1-N16R8  (16 MB flash, 8 MB octal PSRAM)
 *
 * This is the single source of truth for every GPIO the robot uses. Nothing
 * below this header repeats a pin number, so a schematic change lands in one
 * file and a mistake is a compile-time clash rather than a silent short.
 *
 *  ── Full allocation, sheet 02 ──────────────────────────────────────────
 *   IO0   BOOT button (SW2)          IO21  WAKE button (SW3)
 *   IO1   I2C SDA  (1k pull-up)      IO38  I2S MCLK   -> ES7210 MCLK
 *   IO2   I2C SCL  (1k pull-up)      IO14  I2S BCLK   -> ES7210 + MAX98357
 *   IO3   PI_3 (J3 pin 5)            IO13  I2S WS     -> ES7210 + MAX98357
 *   IO4   servo SPI CS3 (CN5)        IO12  I2S DI     <- ES7210 SDOUT
 *   IO5   servo SPI CS4 (CN6)        IO45  I2S DO     -> MAX98357 DIN
 *   IO6   servo SPI MOSI             IO39  LCD DC
 *   IO7   servo SPI CS2 (CN4)        IO40  LCD MOSI
 *   IO8   PI_2 (J3 pin 4)            IO41  LCD SCK
 *   IO9   LCD CS                     IO42  LCD backlight (via Q2)
 *   IO10  IMU INT1 (BMI270)          IO43  U0TXD   (console)
 *   IO11  PI_4 (J3 pin 6)            IO44  U0RXD   (console)
 *   IO15  servo SPI CS1 (CN3)        IO19  TP3 (also USB D-)
 *   IO16  servo SPI CLK              IO20  TP4 (also USB D+)
 *   IO17  servo SPI MISO             IO46  unused
 *   IO18  PI_1 (J3 pin 3)            IO47/IO48 unused
 *
 * Not on a GPIO: RESET is the module EN pin (SW1 + the CH340 DTR/RTS pair),
 * and the MAX98357A SD_MODE is strapped high through a 1 M resistor, so the
 * amplifier is always enabled and has no shutdown line.
 *
 * ── What this board is NOT ───────────────────────────────────────────────
 * The LCD, the audio codec and the Pi header above are fitted on the PCB but
 * this firmware does not drive them: md-mpx-dog is headless and uses the PWA
 * over Wi-Fi as its UI. They are listed here because their pins are real and
 * claimed — 9, 12, 13, 14, 38, 39, 40, 41 and 45 must not be reused by the
 * robot. Adding a panel or a microphone later means editing this table first.
 *
 * ── Why every robot pin moved ────────────────────────────────────────────
 * The Mini Pupper 2 board this firmware used to run on drove the Feetech
 * servos over UART1 on IO4/IO5 with a power-enable on IO8, and the QMI8658C
 * IMU over SPI2 on MOSI=11/MISO=13/CLK=12/CS=38/INT=39.
 *
 * On this board every single one of those pins is something else: IO4/IO5 are
 * servo chip selects, IO8 is a Pi header line, IO11 is a Pi header line, and
 * IO12/13/14/38/39/45 are the audio and LCD nets. Nothing carries over.
 */

/* gpio_num_t and GPIO_NUM_NC, SPI2_HOST, and UART_NUM_1. Included here
 * rather than relying on the includer so this header can be pulled in first
 * by any translation unit, C or C++. */
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <driver/uart.h>

/* ──────────────── Servo bus (4x AT32F413 driver boards) ─────────────── */
/*
 * CN3..CN6 on sheet 02. One driver board per leg, three servos each, so
 * twelve logical servos over one SPI bus with four chip selects.
 *
 * SPI2 is not an arbitrary choice: the ESP32-S3's GP-SPI2 peripheral has six
 * chip-select lines and GP-SPI3 has three, and four are needed. If a display
 * is ever added it must go on SPI3, which is what the LCD header above
 * already does.
 *
 * The bus is clocked at 12 MHz in mode 0. A frame is 36 bytes — 288 bits,
 * about 24 us of wire time — and check_sum is written as 0, not computed.
 */
#define SERVO_SPI_HOST            SPI2_HOST
#define SERVO_SPI_MOSI_PIN        GPIO_NUM_6
#define SERVO_SPI_MISO_PIN        GPIO_NUM_17
#define SERVO_SPI_CLK_PIN         GPIO_NUM_16

/* ── WHICH CONNECTOR EACH LEG IS ON ───────────────────────────────────────
 *
 * 1 = minipupperesp order.  FR=CN4(GPIO7)  FL=CN5(GPIO4)
 *                           RR=CN3(GPIO15) RL=CN6(GPIO5)
 *     Read out of minipupperesp/main/driver_board.c, the build these
 *     movements were taught on and one that demonstrably walks on a
 *     backflip-capable chassis.
 *
 * 2 = FR=CN3(GPIO15)  FL=CN4(GPIO7)  RR=CN5(GPIO4)  RL=CN6(GPIO5)
 *     Rotated by one connector against the above, with RL the only leg the
 *     two agree on.
 *
 * 3 = MAP 1 with the two sides exchanged: FR<->FL and RR<->RL.
 *
 * These cannot all be right, and which one is depends on the harness in
 * front of you, not on anything in this repository. Symptoms of the wrong
 * choice: the robot stands (every leg holds a similar pose, so it hides) but
 * walking is a scramble, and a single servo commanded from the calibration
 * page moves the wrong leg — or, if a connector is empty, no leg at all.
 *
 * A left/right mirror is the one error that hides best: every leg still
 * moves, the robot still stands, and a symmetric gait looks almost right.
 * It is only asymmetric motion, or a backflip, where it becomes the robot
 * fighting itself.
 *
 * TEST BEFORE TRUSTING THE GAIT: put the robot on blocks, nudge servo 1 from
 * the calibration page and watch which joint answers. Servo 1 is the FRONT
 * RIGHT abduction. Then check 2..12. Only then set this number.
 */
#define SERVO_CS_MAP 1

#if SERVO_CS_MAP == 1
#define SERVO_SPI_CS_FR_PIN       GPIO_NUM_7    /* CN4 */
#define SERVO_SPI_CS_FL_PIN       GPIO_NUM_4    /* CN5 */
#define SERVO_SPI_CS_RR_PIN       GPIO_NUM_15   /* CN3 */
#define SERVO_SPI_CS_RL_PIN       GPIO_NUM_5    /* CN6 */
#elif SERVO_CS_MAP == 2
#define SERVO_SPI_CS_FR_PIN       GPIO_NUM_15   /* CN3 */
#define SERVO_SPI_CS_FL_PIN       GPIO_NUM_7    /* CN4 */
#define SERVO_SPI_CS_RR_PIN       GPIO_NUM_4    /* CN5 */
#define SERVO_SPI_CS_RL_PIN       GPIO_NUM_5    /* CN6 */
#else   /* MAP 3 - left and right exchanged against MAP 1 */
#define SERVO_SPI_CS_FR_PIN       GPIO_NUM_4    /* CN5 */
#define SERVO_SPI_CS_FL_PIN       GPIO_NUM_7    /* CN4 */
#define SERVO_SPI_CS_RR_PIN       GPIO_NUM_5    /* CN6 */
#define SERVO_SPI_CS_RL_PIN       GPIO_NUM_15   /* CN3 */
#endif

/*
 * There is no servo power switch on this board. CN3..CN6 sit directly on
 * Vbat+ through OR-ing diodes, so the rail is live from the moment the
 * battery is connected and there is nothing to enable in software.
 *
 * This is a plain 0/1 macro rather than a GPIO_NUM_NC comparison because the
 * pin number ends up inside `1ULL << pin`: with pin == -1 that is a negative
 * shift, which is undefined behaviour and which GCC flags even when the
 * surrounding if can never be true. A #if keeps the expression from being
 * compiled at all.
 *
 * (On the Mini Pupper 2 board this was IO8 driven high, followed by a 1 s
 * settle. Nothing replaces that settle here — the rail is already up — but
 * robot::init() still waits before the first frame so the driver boards have
 * finished their own boot.)
 */
#define SERVO_HAS_POWER_EN        0
#define SERVO_POWER_EN_PIN        GPIO_NUM_NC   /* not present on this board */

/* ───────────────────────────── IMU ───────────────────────────── */
/*
 * BMI270 (accel + gyro) with a BMM150 magnetometer on the BMI270's auxiliary
 * I2C interface. Both sit on the same host I2C bus as the audio codec, which
 * is why the bus handle is created once and shared rather than each driver
 * opening its own — see platform/shared_i2c.h.
 *
 * ASDX/ASCX from the BMI270 go to the BMM150; the host never talks to the
 * BMM150 directly, it reads it through the BMI270's AUX data registers.
 *
 * There are no pull-ups on ASDX/ASCX anywhere on the schematic, so the
 * BMI270's internal 2k pull-ups have to be enabled in firmware
 * (BMI2_AUX_IF_TRIM) or the magnetometer never answers.
 *
 * R48 (10k to 3V3) is a pull-up on INT1, not on SDO — worth knowing if the
 * interrupt is ever used, since it means the pin can be driven open-drain.
 */
#define IMU_I2C_PORT              1
#define IMU_I2C_SDA_PIN           GPIO_NUM_1    /* 1k pull-up */
#define IMU_I2C_SCL_PIN           GPIO_NUM_2    /* 1k pull-up */
#define IMU_I2C_FREQ_HZ           400000

#define IMU_BMI270_I2C_ADDR       0x68   /* 7-bit; SDO tied to GND (U11 pin 1) */
#define IMU_BMM150_I2C_ADDR       0x10   /* 7-bit, on the BMI270 aux bus */
#define IMU_INT1_PIN              GPIO_NUM_10

/* ───────────────────── Raspberry Pi 5 header (J3) ───────────────
 * J3 carries switched VBUS, GND and four signals. PI_1/PI_2 are a UART pair;
 * PI_3/PI_4 are spare and left as plain GPIO. Every line is ESD-clamped but
 * NOT level-shifted — the Pi 5 GPIO bank is 3V3, same as the ESP32-S3.
 *
 * The robot does not talk to a Pi in this build: there is no pi/ link layer.
 * The pins are recorded so nothing else claims them, and so a link can be
 * added later without re-deriving the map. Note PI_2 is IO8, which was the
 * servo power-enable on the old board — it is not one here.
 */
#define PI_UART_PORT              UART_NUM_1
#define PI_UART_TX_PIN            GPIO_NUM_18   /* PI_1 - ESP TX -> Pi RX */
#define PI_UART_RX_PIN            GPIO_NUM_8    /* PI_2 - Pi TX -> ESP RX */
#define PI_SPARE_3_PIN            GPIO_NUM_3    /* PI_3 */
#define PI_SPARE_4_PIN            GPIO_NUM_11   /* PI_4 */

/* ─────────────────────── Buttons ───────────────────────
 * SW2/SW3 on the board. Nothing reads them today — the Mini Pupper 2 build
 * had no buttons either — but they are named here so a future bring-up mode
 * (hold WAKE at reset) has a pin to use. Note IO0 is a strapping pin: it must
 * read high at boot, which the button's pull-up provides.
 */
#define BOOT_BUTTON_GPIO          GPIO_NUM_0    /* SW2 */
#define WAKE_BUTTON_GPIO          GPIO_NUM_21   /* SW3 */

#endif /* _BOARD_CONFIG_H_ */
