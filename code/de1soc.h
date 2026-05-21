#ifndef DE1SOC_H
#define DE1SOC_H

/* Definition of the different devices connected to the de1 SOC board
    * I/O
        - devices: Swich, LEDs, keys, HEX and GPIOs
        - Memory mapped
        - GPIO - 2x 40 PINs HEADER with 36 GPIOs each
            > Shared in MSB (35-32) and LSB (31-0)
            > Enabling a register means to set it as output
    * Sound - samples can be read and write to the AUDIO_FILE
    * Video
        - Memory mapped

    * Note: 'mmap' & 'munmap' can be used to map or unmap the IO into memory
 */

/* IO device path */
#define MEM_FILE      "/dev/mem"

/* I/O Base address */
#define DE1SOC_BASE_ADDR        0xFF24B000

/* Address offset of the different I/O */
#define IO_SWITCH               0x0
#define IO_KEYS                 0x4
#define IO_LEDS                 0x8
#define IO_HEX0                 0xC
#define IO_HEX1                 0x10
#define IO_HEX2                 0x14
#define IO_HEX3                 0x18
#define IO_HEX4                 0x1C
#define IO_HEX5                 0x20
#define IO_GPIO_EN_B0_0         0x24
#define IO_GPIO_EN_B0_1         0x28
#define IO_GPIO_VAL_B0_0        0x2C
#define IO_GPIO_VAL_B0_1        0x30
#define IO_GPIO_EN_B1_0         0x34
#define IO_GPIO_EN_B1_1         0x38
#define IO_GPIO_VAL_B1_0        0x3C
#define IO_GPIO_VAL_B1_1        0x40

/* HEX values */
#define HEX_DIGIT_0             (~0x40)
#define HEX_DIGIT_1             (~0xf9)
#define HEX_DIGIT_2             (~0x24)
#define HEX_DIGIT_3             (~0x30)
#define HEX_DIGIT_4             (~0x19)
#define HEX_DIGIT_5             (~0x12)
#define HEX_DIGIT_6             (~0x02)
#define HEX_DIGIT_7             (~0x78)
#define HEX_DIGIT_8             (~0x00)
#define HEX_DIGIT_9             (~0x10)
#define HEX_DIGIT_A             (~0x08)
#define HEX_DIGIT_B             (~0x03)
#define HEX_DIGIT_C             (~0x46)
#define HEX_DIGIT_D             (~0x21)
#define HEX_DIGIT_E             (~0x06)
#define HEX_DIGIT_F             (~0x0E)

/* Sound */

#define AUDIO_FILE      "/dev/snd"

/* Video */

#define VIDEO_FILE      "/dev/de1_video"
#define VIDEO_BUF_SIZE  0x4B000

#endif /* DE1SOC_H */