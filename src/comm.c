#include "comm.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gps.h"
#include "main.h"
#include "usart.h"

// RAM is scarce on this MCU (20K, with 14K used by the trend buffer), so there is no outgoing
// sentence queue: sentences are formatted when the link is ready to send them, using the latest
// values, and input is left in the comm FIFO while it can't be handled.

#define COMM_PREFIX         "$PGPSDO,"
#define COMM_PREFIX_LEN     (sizeof(COMM_PREFIX) - 1)
// Maximum length of a sentence, including CRLF
#define COMM_MAX_LINE       160
// Maximum length of a sentence before "*CS\r\n"
#define COMM_MAX_BODY       (COMM_MAX_LINE - 5)

// Give up on a partial line from the external link after this time without any byte
#define COMM_RX_TIMEOUT_MS  1000
// A GPS line unfinished after this time without any byte is not going to finish:
// terminate it, otherwise a silent GPS would block our sentences
#define COMM_GPS_SILENCE_MS 250

typedef enum {
    RX_PREFIX,      // Start of line, comparing with COMM_PREFIX
    RX_COMMAND,     // Buffering one of our sentences
    RX_PASSTHROUGH, // Not for us, forwarding to the GPS until end of line
    RX_DISCARD      // Line too long, ignoring it until end of line
} comm_rx_state;

static comm_rx_state rx_state = RX_PREFIX;
static char          rx_line[COMM_MAX_LINE + 1];
static size_t        rx_len = 0;
static uint32_t      rx_last_byte_time = 0;

// In use by HAL_UART_Transmit_IT() as long as the UART is not ready
static uint8_t       gps_tx_buf[64];
static uint8_t       comm_tx_buf[COMM_MAX_LINE + 2];

// Last byte sent on the link was not the end of a line
static bool          tx_mid_line = false;

// Reply waiting to be sent. We stop reading commands while there is one.
static char          ack_buf[72];
static size_t        ack_len = 0;

// Completes a sentence with its checksum and CRLF, returns its total length
static size_t comm_finish_sentence(char* sentence, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 1; i < len; i++) {
        checksum ^= (uint8_t)sentence[i];
    }
    return len + snprintf(sentence + len, 6, "*%02X\r\n", checksum);
}

static void comm_queue_ack(const char* echo, const char* key, const char* error)
{
    int len = snprintf(ack_buf, sizeof(ack_buf) - 5, COMM_PREFIX "ACK%s%s%s%.32s%s%s",
                       echo ? "," : "", echo ? echo : "",
                       key ? "," : "", key ? key : "",
                       error ? ",ERR," : (echo ? ",OK" : ""), error ? error : "");
    ack_len = comm_finish_sentence(ack_buf, len);
}

static int comm_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Checks the checksum and cuts it from the sentence
static bool comm_check_sentence(char* line)
{
    char* star = strrchr(line, '*');
    if (star == NULL || comm_hex_digit(star[1]) < 0 || comm_hex_digit(star[2]) < 0) {
        return false;
    }
    if (star[3] != '\0' && !(star[3] == '\r' && star[4] == '\0')) {
        return false;
    }
    uint8_t checksum = 0;
    for (const char* p = line + 1; p < star; p++) {
        checksum ^= (uint8_t)*p;
    }
    if (checksum != (comm_hex_digit(star[1]) << 4 | comm_hex_digit(star[2]))) {
        return false;
    }
    *star = '\0';
    return true;
}

static void comm_handle_line(char* line)
{
    if (!comm_check_sentence(line)) {
        comm_queue_ack(NULL, NULL, "CHECKSUM");
        return;
    }
    comm_queue_ack(NULL, NULL, "FORMAT");
}

// Takes bytes from the external link: our sentences are handled here, everything else goes to the GPS.
// Nothing is read while the GPS UART or the reply slot is busy, bytes wait in the comm FIFO. With a
// link faster than the GPS, this can overflow the FIFO: see doc/PROTOCOL.md.
static void comm_process_rx(uint32_t now)
{
    if (huart3.gState != HAL_UART_STATE_READY) {
        return;
    }
    size_t  out = 0;
    bool    got_byte = false;
    uint8_t c;
    while (ack_len == 0 && (out + COMM_PREFIX_LEN + 1) <= sizeof(gps_tx_buf) && gps_comm_read(&c)) {
        got_byte = true;
        switch (rx_state) {
            case RX_PREFIX:
                if (c == (uint8_t)COMM_PREFIX[rx_len]) {
                    rx_line[rx_len++] = c;
                    if (rx_len == COMM_PREFIX_LEN) {
                        rx_state = RX_COMMAND;
                    }
                } else {
                    memcpy(gps_tx_buf + out, rx_line, rx_len);
                    out += rx_len;
                    gps_tx_buf[out++] = c;
                    rx_len = 0;
                    if (c != '\n') {
                        rx_state = RX_PASSTHROUGH;
                    }
                }
                break;
            case RX_COMMAND:
                if (c == '\n') {
                    rx_line[rx_len] = '\0';
                    comm_handle_line(rx_line);
                    rx_len = 0;
                    rx_state = RX_PREFIX;
                } else if (rx_len >= COMM_MAX_LINE) {
                    comm_queue_ack(NULL, NULL, "FORMAT");
                    rx_len = 0;
                    rx_state = RX_DISCARD;
                } else {
                    rx_line[rx_len++] = c;
                }
                break;
            case RX_PASSTHROUGH:
                gps_tx_buf[out++] = c;
                if (c == '\n') {
                    rx_state = RX_PREFIX;
                }
                break;
            case RX_DISCARD:
                if (c == '\n') {
                    rx_state = RX_PREFIX;
                }
                break;
        }
    }
    if (got_byte) {
        rx_last_byte_time = now;
    } else if (ack_len == 0 && (now - rx_last_byte_time) >= COMM_RX_TIMEOUT_MS) {
        // Line never ended: don't let it swallow the next one
        if (rx_state == RX_PREFIX && rx_len > 0) {
            // Was not for us after all
            memcpy(gps_tx_buf, rx_line, rx_len);
            out = rx_len;
        } else if (rx_state == RX_COMMAND) {
            comm_queue_ack(NULL, NULL, "FORMAT");
        }
        rx_len = 0;
        rx_state = RX_PREFIX;
    }
    if (out > 0) {
        HAL_UART_Transmit_IT(&huart3, gps_tx_buf, out);
    }
}

// Formats the next sentence to send at offset in comm_tx_buf, returns its length (0 if none)
static size_t comm_next_sentence(size_t offset)
{
    if (ack_len > 0) {
        memcpy(comm_tx_buf + offset, ack_buf, ack_len);
        size_t len = ack_len;
        ack_len = 0;
        return len;
    }
    return 0;
}

// Sends to the external link. Our sentences only go out between GPS lines, so that they are never
// spliced into one. Never waits: if the UART is busy, there is nothing to do until next time.
static void comm_process_tx(uint32_t now)
{
    if (huart2.gState != HAL_UART_STATE_READY) {
        return;
    }
    bool silent = gps_forward_available() == 0 && (now - gps_last_byte_time) >= COMM_GPS_SILENCE_MS;
    if (!tx_mid_line || silent) {
        size_t offset = tx_mid_line ? 2 : 0;
        size_t len = comm_next_sentence(offset);
        if (len > 0) {
            if (tx_mid_line) {
                comm_tx_buf[0] = '\r';
                comm_tx_buf[1] = '\n';
                tx_mid_line = false;
            }
            HAL_UART_Transmit_IT(&huart2, comm_tx_buf, offset + len);
            return;
        }
    }
    size_t len = gps_forward_read(comm_tx_buf, sizeof(comm_tx_buf));
    if (len > 0) {
        tx_mid_line = comm_tx_buf[len - 1] != '\n';
        HAL_UART_Transmit_IT(&huart2, comm_tx_buf, len);
    }
}

void comm_init(void)
{
    rx_last_byte_time = HAL_GetTick();
}

void comm_task(void)
{
    uint32_t now = HAL_GetTick();
    gps_comm_rx_check();
    comm_process_rx(now);
    comm_process_tx(now);
}
