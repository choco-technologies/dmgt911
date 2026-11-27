#ifndef DMGT911_TYPES_H
#define DMGT911_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "dmdrvi_ioctl.h"

/** Most contacts the controller reports */
#define DMGT911_MAX_CONTACTS        5U

/** The addresses the chip can have (the level of its INT pin when the reset is released decides) */
#define DMGT911_ADDRESS_LOW         0x5DU
#define DMGT911_ADDRESS_HIGH        0x14U

/**
 * @brief Configuration of a touch panel (the ini section of the driver)
 *
 * The coordinates the node reports are screen coordinates: x in 0..width-1, y in 0..height-1, after the raw ones (0..raw_width-1,
 * 0..raw_height-1, as the chip counts them) were scaled, swapped and mirrored.
 */
typedef struct
{
    uint16_t width;             /**< Screen width the contacts are reported in (as shown, after the rotation of the display) */
    uint16_t height;            /**< Screen height */
    uint16_t raw_width;         /**< Range of the chip's x, 0 = what the chip says (its configuration) */
    uint16_t raw_height;        /**< Range of the chip's y, 0 = what the chip says */
    uint8_t  address;           /**< 7-bit I2C address of the chip (0x5D, or 0x14 when INT was high at the reset) */
    uint8_t  max_contacts;      /**< Contacts reported, 1..DMGT911_MAX_CONTACTS */
    bool     swap_xy;           /**< Exchange the axes (before the mirroring) */
    bool     mirror_x;          /**< x counted from the other side */
    bool     mirror_y;          /**< y counted from the other side */
    uint16_t poll_ms;           /**< Period of the polling of the chip while a client waits (WAIT_EVENT) */
    uint16_t release_ms;        /**< A contact is lifted when the chip has not reported anything for this long */
} dmgt911_config_t;

/** A contact as the chip reports it, before the mapping */
typedef struct
{
    uint16_t x;                 /**< Raw column */
    uint16_t y;                 /**< Raw line */
    uint8_t  id;                /**< Number of the finger the chip tracks */
    uint8_t  pressure;          /**< The size of the contact, cut to 255 */
} dmgt911_raw_contact_t;

/** Commands of the node, besides the generic input ones (DMDRVI_IOCTL_INPUT_*) */
typedef enum
{
    dmgt911_ioctl_cmd_get_config = DMDRVI_IOCTL_CUSTOM_BASE,   /**< arg: dmgt911_config_t* (raw_width/raw_height and address as the driver uses them) */
    dmgt911_ioctl_cmd_max
} dmgt911_ioctl_cmd_t;

#endif /* DMGT911_TYPES_H */
