# dmgt911 API

```c
#include "dmgt911.h"
```

| Function | |
|---|---|
| `bool dmgt911_validate_config(const dmgt911_config_t *config)` | the screen, the address (0x5D / 0x14) and the number of contacts make sense |
| `uint8_t dmgt911_parse_report(report, size, contacts, max_contacts, &ready)` | decode the 41 bytes read from register 0x814E: the status and up to 5 contacts of 8 bytes; `ready` says the chip has a new report |
| `void dmgt911_map_point(config, raw_x, raw_y, &x, &y)` | scale, swap and mirror a raw point to the screen, clipped to it |

The node is described in [configuration.md](configuration.md); `dmgt911_ioctl_cmd_get_config` returns the configuration as the driver uses it.
