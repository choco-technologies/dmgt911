#ifndef DMGT911_H
#define DMGT911_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmgt911_defs.h"
#include "dmgt911_types.h"

/**
 * @brief Check a configuration without touching hardware
 *
 * @return true when the screen size, the address and the number of contacts make sense.
 */
dmod_dmgt911_api(1.0, bool, _validate_config, ( const dmgt911_config_t *config ));

/** Size of the report of the chip: the status byte (register 0x814E) and DMGT911_MAX_CONTACTS contacts of 8 bytes (from 0x814F) */
#define DMGT911_REPORT_SIZE         (1U + 8U * DMGT911_MAX_CONTACTS)

/**
 * @brief Decode the report of the chip
 *
 * @param report        DMGT911_REPORT_SIZE bytes read from the register 0x814E
 * @param size          Bytes in report
 * @param contacts      Output
 * @param max_contacts  Size of contacts
 * @param ready         Output: the chip says the report is a new one (bit 7 of the status); when it is not, the result is 0 and
 *                      says nothing about the fingers
 *
 * @return Contacts decoded; 0 when none are touched (with ready set) or the report is not valid (ready is cleared)
 */
dmod_dmgt911_api(1.0, uint8_t, _parse_report, ( const uint8_t *report, size_t size, dmgt911_raw_contact_t *contacts, uint8_t max_contacts, bool *ready ));

/**
 * @brief Map a raw point to screen coordinates (scaling, swap, mirroring), the result is clipped to the screen
 *
 * @param config  The configuration; raw_width and raw_height must be set (not 0)
 */
dmod_dmgt911_api(1.0, void, _map_point, ( const dmgt911_config_t *config, uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y ));

#endif // DMGT911_H
