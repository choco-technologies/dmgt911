# dmgt911 configuration

The section of the touch panel (`driver_name=dmgt911`):

| Key | Default | |
|---|---|---|
| `instance` | 0 | the node is `/dev/dmgt911<instance>` |
| `width`, `height` | 800, 480 | the screen the contacts are reported in: the display as shown (after its rotation) |
| `raw_width`, `raw_height` | 0 | the range of the chip's x and y; 0 = what the chip says (its configuration, registers 0x8048 / 0x804A) |
| `address` | 0x5D | the 7-bit I2C address of the chip: 0x5D, or 0x14 (see below) |
| `max_contacts` | 5 | contacts reported, 1..5 |
| `swap_xy` | no | exchange the axes (before the mirroring) |
| `mirror_x`, `mirror_y` | no | count x / y from the other side |
| `poll_ms` | 10 | period of the polling of the chip while a client waits (`WAIT_EVENT`) |
| `release_ms` | 300 | a contact is lifted when the chip has reported nothing for this long (a report without it lifts it at once) |
| `reset` | no | `yes`: the driver waits for a `reset` friend and resets the chip before it starts |

The axes are turned in this order: swap, mirror, then the scaling from the range of the chip to `width` x `height`.

## The address

The chip has two addresses and takes one of them from the level of its INT pin when the reset is released: 0x5D when it is low, 0x14 when
it is high. The driver tries `address` first and the other one when the chip does not answer, and keeps the one that does (it logs
which).

## Friends

`dmdevfs` tells every driver of a `friends_group` about the others (`friend_role=`), whatever order they are created in. The chip is
started once the required ones are there:

| `friend_role` | |
|---|---|
| `i2c` | the dmi2c node the chip is on (required) |
| `reset` | the reset line of the chip, low = reset (with `reset=yes`): a dmgpio pin, or a pin behind an expander (dmpca9557); any node that takes `dmgpio_ioctl_cmd_set_pins_state` |

The interrupt line of the chip is not used: the driver polls. Declare the touch panel last in the file: the order is the order of the
creation and, reversed, of the teardown.

## The node

`read()` gives the state of the contacts (`dmdrvi_input_state_t`, the generic input interface of dmdrvi), `DMDRVI_IOCTL_INPUT_GET_STATE`
the same, `DMDRVI_IOCTL_INPUT_WAIT_EVENT` sleeps until it changes (an argument: the timeout in ms). The chip is asked when a client
asks and every `poll_ms` while one waits.
