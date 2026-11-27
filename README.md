# dmgt911

A driver of the capacitive touch controller **GT911** (Goodix) on I2C: up to 5 contacts, the generic input interface of `dmdrvi`
(`DMDRVI_IOCTL_INPUT_*`), so what reads a touch panel (`inputd`, `libinput`, `dmview`) reads this one. The chip is polled, nothing runs
in the background.

Made for the Elecrow DIS08070H (CrowPanel 7.0", 800x480), where the reset of the chip is wired to an expander (`dmpca9557`).

* [docs/configuration.md](docs/configuration.md) - the ini section, the friends, the address
* [docs/api-reference.md](docs/api-reference.md) - the functions
* [configs/board/elecrow-dis08070h/touch.ini](configs/board/elecrow-dis08070h/touch.ini) - the bus, the reset and the panel of that board

```ini
[touch]
driver_name=dmgt911
friends_group=touch
width=800
height=480
reset=yes
```
