/*
 * SPDX-License-Identifier: MIT
 *
 * Text console on the USB CDC ACM port (status, settings, recovery).
 */

#ifndef SP1M_CONSOLE_H_
#define SP1M_CONSOLE_H_

int console_init(void);

/* USB: the host changed DTR/RTS. */
void console_line_state_changed(void);

#endif /* SP1M_CONSOLE_H_ */
