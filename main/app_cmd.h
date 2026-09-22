#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-line text console shared by the BLE characteristic and POST /api/cmd.
 * Writes a human-readable reply (never longer than out_size-1, always
 * NUL-terminated) and returns its length.
 *
 * Commands:
 *   help
 *   status
 *   reboot
 *   wifi <ssid> <password>        store credentials, effective next boot
 *   gpio list
 *   gpio mode <pin> <in|inpu|inpd|out|od|off>
 *   gpio set <pin> <0|1>
 *   gpio get <pin>
 *   gpio save
 *   uart baud <rate>
 *   uart send <text>
 *   uart read
 *   uart monitor <on|off>         mirror RX to the BLE notify channel
 *   mb read <slave> <fn> <start> <count>
 *   mb write <slave> <fn> <start> <value> [value ...]
 *   mb poll <on|off>
 *   mb jobs
 *   cam <control> <value>         e.g. "cam framesize 8", "cam quality 10"
 */
size_t app_cmd_execute(const char *line, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
