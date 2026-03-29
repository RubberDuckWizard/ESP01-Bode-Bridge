#ifndef ESP_PARSER_H
#define ESP_PARSER_H

#include <stdint.h>

typedef enum {
	SCPI_READ_NONE = 0,
	SCPI_READ_FIXED_ID,
	SCPI_READ_TEXT
} ScpiReadMode;

/*
 * esp_parser.h – Table-driven SCPI command parser for SDS800X-HD Bode
 * =====================================================================
 * Handles the minimal Siglent SCPI set needed during a Bode plot:
 *
 *   IDN-SGLT-PRI?
 *   C1:BSWV?  / C2:BSWV?
 *   C1:BSWV WVTP,SINE  (and PHSE,x  FRQ,x  AMP,x  OFST,x)
 *   C1:OUTP ON|OFF
 *   C1:OUTP LOAD,50    (accepted/ignored – load impedance not sent to AWG)
 *   Multi-command sequences separated by ';'
 *
 * Responses are stored in the global read buffer and retrieved by
 * scpi_read_response() when the scope issues a VXI-11 DEVICE_READ.
 */

/* Call this when a DEVICE_WRITE command arrives.
 * buf must be null-terminated (the network layer ensures this). */
bool scpi_handle_write(char *buf);

/* Call this when a DEVICE_READ arrives to get the pending response.
 * Copies at most dst_max-1 bytes into dst, null-terminates.
 * Returns number of bytes copied (0 = nothing pending). */
int  scpi_read_response(char *dst, int dst_max, uint32_t *declared_len, ScpiReadMode *mode);

#endif /* ESP_PARSER_H */
