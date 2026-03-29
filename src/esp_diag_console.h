#ifndef ESP_DIAG_CONSOLE_H
#define ESP_DIAG_CONSOLE_H

#include <stdint.h>

void diag_begin_early(void);
void diag_setup(void);
void diag_poll(void);
bool diag_trace_enabled(void);
void diag_trace_set(bool enabled);
void diag_tracef(const char *fmt, ...);

#endif /* ESP_DIAG_CONSOLE_H */