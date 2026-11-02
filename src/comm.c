#include "comm.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "eeprom.h"
#include "frequency.h"
#include "gps.h"
#include "int.h"
#include "main.h"
#include "menu.h"
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
// Telemetry unsent after this time (GPS stuck mid-line) is dropped
#define COMM_TLM_MAX_WAIT_MS 500

#define COMM_PROTOCOL_VERSION 1

// Saving settings erases a flash page, which stalls the CPU for 20-40 ms. TIM1 overflows every
// 0.94 ms, so this always costs a bad frequency sample (frequency_get_error() filters it out), and
// the PPS output pulse is late if its interrupt falls in the stall. So remote changes apply to RAM
// at once but are saved at most once per interval (a burst of changes gives one erase), preferably:
//  - well before the next PPS output pulse,
//  - when the GPS is quiet: the UART overruns if its data is not read during a stall.
#define COMM_SAVE_MIN_INTERVAL_MS   10000
#define COMM_SAVE_MAX_WAIT_MS       3000
#define COMM_SAVE_PPS_WINDOW_MS     900
#define COMM_SAVE_GPS_QUIET_MS      30

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
// Room for a sentence, an extra CRLF before it and the terminator written by snprintf
static uint8_t       comm_tx_buf[COMM_MAX_LINE + 3];

// Last byte sent on the link was not the end of a line
static bool          tx_mid_line = false;

// Reply waiting to be sent. We stop reading commands while there is one.
static char          ack_buf[72];
static size_t        ack_len = 0;

// Sentences waiting to be sent, formatted when the link is ready
static bool          ver_pending = true;
static bool          tlm_enabled = true;
static bool          tlm_pending = false;
static uint32_t      tlm_pending_since = 0;
static uint32_t      tlm_last_uptime = 0;
static bool          cfg_pending = false;
static uint8_t       cfg_next_key = 0;

// Settings as last reported by CFG: any change, whatever its origin, gets reported
static ee_storage_t  cfg_reported;

static bool          save_pending = false;
static bool          save_done_once = false;
static uint32_t      save_last_time = 0;
static bool          save_eligible = false;
static uint32_t      save_eligible_since = 0;

// In CFG order. Read-only keys are last.
typedef enum {
    KEY_CONTRAST, KEY_PWM_AUTO_SAVE, KEY_PPS_AUTO_SYNC, KEY_PPB_LOCK_THRESHOLD, KEY_OCXO_MODEL,
    KEY_WARMUP_TIME, KEY_CORRECTION_ALGO, KEY_CORRECTION_FACTOR, KEY_PPS_SYNC_ON, KEY_PPS_SYNC_DELAY,
    KEY_PPS_SYNC_THRESHOLD, KEY_TREND_AUTO_V, KEY_TREND_AUTO_H, KEY_TREND_V_SCALE, KEY_TREND_H_SCALE,
    KEY_GPS_TIME_OFFSET, KEY_DATE_FORMAT, KEY_GPS_BAUDRATE, KEY_BOOT_MENU, KEY_COMM_BAUDRATE,
    KEY_FIRST_READ_ONLY, KEY_GPS_MODEL = KEY_FIRST_READ_ONLY, KEY_FW_VERSION, KEY_COUNT
} comm_key;

static const char* const key_names[KEY_COUNT] = {
    "contrast", "pwm_auto_save", "pps_auto_sync", "ppb_lock_threshold", "ocxo_model",
    "warmup_time", "correction_algo", "correction_factor", "pps_sync_on", "pps_sync_delay",
    "pps_sync_threshold", "trend_auto_v", "trend_auto_h", "trend_v_scale", "trend_h_scale",
    "gps_time_offset", "date_format", "gps_baudrate", "boot_menu", "comm_baudrate",
    "gps_model", "fw_version"
};

static const char ERR_KEY[]    = "KEY";
static const char ERR_RANGE[]  = "RANGE";
static const char ERR_FORMAT[] = "FORMAT";
static const char ERR_BUSY[]   = "BUSY";

// Largest value for 32 bit settings: 0xFFFFFFFF in flash means unset
#define VALUE_MAX 0xFFFFFFFELL

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

static void comm_request_save(void)
{
    save_pending = true;
}

// Saves settings if needed, see COMM_SAVE_MIN_INTERVAL_MS
static void comm_process_save(uint32_t now)
{
    if (!save_pending || (save_done_once && (now - save_last_time) < COMM_SAVE_MIN_INTERVAL_MS)) {
        return;
    }
    if (!save_eligible) {
        save_eligible = true;
        save_eligible_since = now;
    }
    bool good_time = (now - last_pps_out) < COMM_SAVE_PPS_WINDOW_MS
                     && (now - gps_last_byte_time) >= COMM_SAVE_GPS_QUIET_MS;
    if (!good_time && (now - save_eligible_since) < COMM_SAVE_MAX_WAIT_MS) {
        return;
    }
    save_pending = false;
    save_eligible = false;
    // Encoder saves write everything: often there is nothing left to do
    if (!eeprom_matches_flash()) {
        if (!EE_Write()) {
            save_pending = true;
        }
        save_done_once = true;
        save_last_time = HAL_GetTick();
    }
}

static uint32_t comm_key_value(comm_key key)
{
    switch (key) {
        case KEY_CONTRAST:           return (uint32_t)(int32_t)contrast;
        case KEY_PWM_AUTO_SAVE:      return pwm_auto_save;
        case KEY_PPS_AUTO_SYNC:      return pps_ppm_auto_sync;
        case KEY_PPB_LOCK_THRESHOLD: return ppb_lock_threshold;
        case KEY_OCXO_MODEL:         return ocxo_model;
        case KEY_WARMUP_TIME:        return warmup_time_seconds;
        case KEY_CORRECTION_ALGO:    return correction_algorithm;
        case KEY_CORRECTION_FACTOR:  return correction_factor;
        case KEY_PPS_SYNC_ON:        return pps_sync_on;
        case KEY_PPS_SYNC_DELAY:     return pps_sync_delay;
        case KEY_PPS_SYNC_THRESHOLD: return pps_sync_threshold;
        case KEY_TREND_AUTO_V:       return trend_auto_v;
        case KEY_TREND_AUTO_H:       return trend_auto_h;
        case KEY_TREND_V_SCALE:      return trend_v_scale;
        case KEY_TREND_H_SCALE:      return trend_h_scale;
        case KEY_GPS_TIME_OFFSET:    return (uint32_t)(int32_t)gps_time_offset;
        case KEY_DATE_FORMAT:        return gps_date_format;
        case KEY_GPS_BAUDRATE:       return gps_baudrate;
        case KEY_BOOT_MENU:          return ee_storage.boot_menu;
        case KEY_COMM_BAUDRATE:      return comm_baudrate;
        case KEY_GPS_MODEL:          return gps_model;
        default:                     return 0;
    }
}

// Applies a setting with the same limits and side effects as the encoder menus.
// Returns NULL if accepted, the error otherwise.
static const char* comm_apply_setting(comm_key key, int64_t value)
{
#define CHECK_RANGE(min, max) if (value < (min) || value > (max)) return ERR_RANGE
// Live value, then ee_storage if it differs
#define STORE(variable, field, stored)                  \
    do {                                                \
        variable = (__typeof__(variable))value;         \
        if (ee_storage.field != (stored)) {             \
            ee_storage.field = (stored);                \
            changed = true;                             \
        }                                               \
    } while (0)

    uint32_t u = (uint32_t)value;
    bool changed = false;
    switch (key) {
        case KEY_CONTRAST:
            CHECK_RANGE(0, 100);
            changed = settings_set_contrast((int8_t)value);
            break;
        case KEY_PWM_AUTO_SAVE:
            CHECK_RANGE(0, 1);
            STORE(pwm_auto_save, pwm_auto_save, u);
            break;
        case KEY_PPS_AUTO_SYNC:
            CHECK_RANGE(0, 1);
            STORE(pps_ppm_auto_sync, pps_ppm_auto_sync, u);
            break;
        case KEY_PPB_LOCK_THRESHOLD:
            CHECK_RANGE(0, MAX_PPB_LOCK_THRESHOLD);
            STORE(ppb_lock_threshold, ppb_lock_threshold, u);
            break;
        case KEY_OCXO_MODEL:
            CHECK_RANGE(0, OCXO_MODEL_UNKNOWN);
            changed = settings_set_ocxo_model((ocxo_model_type)value);
            break;
        case KEY_WARMUP_TIME:
            CHECK_RANGE(0, 1000);
            STORE(warmup_time_seconds, warmup_time_seconds, u);
            break;
        case KEY_CORRECTION_ALGO:
            CHECK_RANGE(0, CORRECTION_ALGO_ERIC_H);
            changed = settings_set_correction_algorithm((correction_algo_type)value);
            break;
        case KEY_CORRECTION_FACTOR:
            CHECK_RANGE(0, VALUE_MAX);
            // Limits depend on the algorithm: not moving the value only applies them
            if (increment_correction_factor_value(correction_algorithm, u, 0) != u) {
                return ERR_RANGE;
            }
            STORE(correction_factor, correction_factor, u);
            break;
        case KEY_PPS_SYNC_ON:
            CHECK_RANGE(0, 1);
            STORE(pps_sync_on, pps_sync_on, u);
            break;
        case KEY_PPS_SYNC_DELAY:
            CHECK_RANGE(0, VALUE_MAX);
            STORE(pps_sync_delay, pps_sync_delay, u);
            break;
        case KEY_PPS_SYNC_THRESHOLD:
            CHECK_RANGE(0, VALUE_MAX);
            STORE(pps_sync_threshold, pps_sync_threshold, u);
            break;
        case KEY_TREND_AUTO_V:
            CHECK_RANGE(0, 1);
            STORE(trend_auto_v, trend_auto_v, u);
            break;
        case KEY_TREND_AUTO_H:
            CHECK_RANGE(0, 1);
            STORE(trend_auto_h, trend_auto_h, u);
            break;
        case KEY_TREND_V_SCALE:
            CHECK_RANGE(0, VALUE_MAX);
            // As with the encoder, no manual scale while auto-scale is on: it would be overwritten
            if (trend_auto_v) {
                return ERR_BUSY;
            }
            changed = settings_set_trend_v_scale(u);
            break;
        case KEY_TREND_H_SCALE:
            CHECK_RANGE(0, VALUE_MAX);
            if (trend_auto_h) {
                return ERR_BUSY;
            }
            changed = settings_set_trend_h_scale(u);
            break;
        case KEY_GPS_TIME_OFFSET:
            CHECK_RANGE(MIN_TIME_OFFSET, MAX_TIME_OFFSET);
            STORE(gps_time_offset, gps_time_offset, (uint32_t)(value - MIN_TIME_OFFSET));
            break;
        case KEY_DATE_FORMAT:
            CHECK_RANGE(DATE_FORMAT_UTC, DATE_FORMAT_ISO_DASH);
            STORE(gps_date_format, gps_date_format, u);
            break;
        case KEY_GPS_BAUDRATE:
            CHECK_RANGE(0, VALUE_MAX);
            if (!settings_is_valid_gps_baudrate(u)) {
                return ERR_RANGE;
            }
            changed = settings_set_gps_baudrate(u);
            break;
        case KEY_BOOT_MENU:
            CHECK_RANGE(0, VALUE_MAX);
            if (!settings_is_valid_boot_menu(u)) {
                return ERR_RANGE;
            }
            if (ee_storage.boot_menu != u) {
                ee_storage.boot_menu = u;
                changed = true;
            }
            break;
        case KEY_COMM_BAUDRATE:
            CHECK_RANGE(0, VALUE_MAX);
            if (!settings_is_valid_baudrate(u)) {
                return ERR_RANGE;
            }
            changed = settings_set_comm_baudrate(u);
            break;
        default:
            return ERR_KEY;
    }
    if (changed) {
        comm_request_save();
    }
    refresh_screen = true;
    return NULL;
#undef CHECK_RANGE
#undef STORE
}

// Decimal integers only. Limited to 10 digits, which covers 32 bit values and can't overflow.
static bool comm_parse_int(const char* text, int64_t* value)
{
    bool negative = (*text == '-');
    if (negative) {
        text++;
    }
    int64_t result = 0;
    int digits = 0;
    for (; *text != '\0'; text++, digits++) {
        if (*text < '0' || *text > '9' || digits == 10) {
            return false;
        }
        result = result * 10 + (*text - '0');
    }
    if (digits == 0) {
        return false;
    }
    *value = negative ? -result : result;
    return true;
}

// Splits the next comma separated field, NULL if there is none
static char* comm_next_field(char** cursor)
{
    char* field = *cursor;
    if (field != NULL) {
        char* comma = strchr(field, ',');
        if (comma != NULL) {
            *comma = '\0';
            *cursor = comma + 1;
        } else {
            *cursor = NULL;
        }
    }
    return field;
}

static void comm_handle_set(const char* name, const char* text)
{
    if (name == NULL) {
        comm_queue_ack("SET", NULL, ERR_FORMAT);
        return;
    }
    comm_key key = KEY_COUNT;
    for (int i = 0; i < KEY_FIRST_READ_ONLY; i++) {
        if (strcmp(name, key_names[i]) == 0) {
            key = (comm_key)i;
            break;
        }
    }
    if (key == KEY_COUNT) {
        comm_queue_ack("SET", name, ERR_KEY);
        return;
    }
    int64_t value;
    if (text == NULL || !comm_parse_int(text, &value)) {
        comm_queue_ack("SET", name, ERR_FORMAT);
        return;
    }
    comm_queue_ack("SET", name, comm_apply_setting(key, value));
}

static void comm_handle_cmd(const char* name, const char* text)
{
    const char* error = NULL;
    if (name == NULL) {
        error = ERR_FORMAT;
    } else if (strcmp(name, "TLM") == 0) {
        if (text == NULL) {
            error = ERR_FORMAT;
        } else if (strcmp(text, "0") == 0 || strcmp(text, "1") == 0) {
            tlm_enabled = (text[0] == '1');
            tlm_pending = false;
        } else {
            error = ERR_RANGE;
        }
    } else if (text != NULL) {
        error = strcmp(name, "SAVEPWM") == 0 || strcmp(name, "FORCESYNC") == 0 ? ERR_FORMAT : ERR_KEY;
    } else if (strcmp(name, "SAVEPWM") == 0) {
        if (ee_storage.pwm != TIM1->CCR2) {
            ee_storage.pwm = TIM1->CCR2;
            comm_request_save();
        }
    } else if (strcmp(name, "FORCESYNC") == 0) {
        sync_pps_out = true;
    } else {
        error = ERR_KEY;
    }
    comm_queue_ack("CMD", name, error);
}

static void comm_handle_get(const char* name, const char* extra)
{
    const char* error = NULL;
    if (name == NULL || extra != NULL) {
        error = ERR_FORMAT;
    } else if (strcmp(name, "VER") == 0) {
        ver_pending = true;
    } else if (strcmp(name, "CFG") == 0) {
        cfg_pending = true;
        cfg_next_key = 0;
    } else {
        error = ERR_KEY;
    }
    comm_queue_ack("GET", name, error);
}

static void comm_handle_line(char* line)
{
    if (!comm_check_sentence(line)) {
        comm_queue_ack(NULL, NULL, "CHECKSUM");
        return;
    }
    char* cursor = line + COMM_PREFIX_LEN;
    const char* type = comm_next_field(&cursor);
    const char* name = comm_next_field(&cursor);
    const char* text = comm_next_field(&cursor);
    if (cursor != NULL) {
        // Too many fields
        comm_queue_ack(NULL, NULL, ERR_FORMAT);
    } else if (strcmp(type, "SET") == 0) {
        comm_handle_set(name, text);
    } else if (strcmp(type, "CMD") == 0) {
        comm_handle_cmd(name, text);
    } else if (strcmp(type, "GET") == 0) {
        comm_handle_get(name, text);
    } else {
        comm_queue_ack(NULL, NULL, ERR_FORMAT);
    }
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

// Field sources are the expressions used by menu_draw(), so that link and LCD agree
static size_t comm_format_tlm(char* out)
{
    int32_t ppb_inst = (int64_t)ppb_error * 1000000000 * 100 / ((int64_t)HAL_RCC_GetHCLKFreq());
    int len = snprintf(out, COMM_MAX_BODY + 1,
                       COMM_PREFIX "TLM,%lu,%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%lu,%ld,%ld,%ld,%lu,%d",
                       (unsigned long)device_uptime, gps_lock_status ? 1 : 0, ppb_lock_status ? 1 : 0, (int)num_sats,
                       (long)frequency_get_ppb(), (long)ppb_inst, (long)ppb_frequency, (long)ppb_error,
                       (long)ppb_correction, (unsigned long)TIM1->CCR2, (long)ppb_millis, (long)pps_error,
                       (long)pps_millis, (unsigned long)pps_sync_count, frequency_adjustment_allowed() ? 1 : 0);
    return comm_finish_sentence(out, len);
}

// One CFG line, as many keys as fit
static size_t comm_format_cfg(char* out)
{
    int len = snprintf(out, COMM_MAX_BODY + 1, COMM_PREFIX "CFG");
    while (cfg_next_key < KEY_COUNT) {
        comm_key key = (comm_key)cfg_next_key;
        size_t room = COMM_MAX_BODY + 1 - len;
        int added;
        if (key == KEY_FW_VERSION) {
            added = snprintf(out + len, room, ",%s=%s", key_names[key], FIRMWARE_VERSION);
        } else if (key == KEY_GPS_TIME_OFFSET) {
            added = snprintf(out + len, room, ",%s=%ld", key_names[key], (long)(int32_t)comm_key_value(key));
        } else {
            added = snprintf(out + len, room, ",%s=%lu", key_names[key], (unsigned long)comm_key_value(key));
        }
        if ((size_t)added >= room) {
            break;
        }
        len += added;
        cfg_next_key++;
    }
    if (cfg_next_key == KEY_COUNT) {
        cfg_pending = false;
    }
    return comm_finish_sentence(out, len);
}

// Formats the next sentence to send at offset in comm_tx_buf, returns its length (0 if none)
static size_t comm_next_sentence(size_t offset)
{
    char* out = (char*)comm_tx_buf + offset;
    if (ack_len > 0) {
        memcpy(out, ack_buf, ack_len);
        size_t len = ack_len;
        ack_len = 0;
        return len;
    }
    if (ver_pending) {
        ver_pending = false;
        int len = snprintf(out, COMM_MAX_BODY + 1, COMM_PREFIX "VER,%d,%s", COMM_PROTOCOL_VERSION, FIRMWARE_VERSION);
        return comm_finish_sentence(out, len);
    }
    if (tlm_pending) {
        tlm_pending = false;
        return comm_format_tlm(out);
    }
    if (cfg_pending) {
        return comm_format_cfg(out);
    }
    return 0;
}

// Queues the periodic and change-driven sentences
static void comm_update_pending(uint32_t now)
{
    uint32_t uptime = device_uptime;
    if (tlm_enabled && uptime != tlm_last_uptime) {
        tlm_pending = true;
        tlm_pending_since = now;
    }
    tlm_last_uptime = uptime;
    if (tlm_pending && (now - tlm_pending_since) > COMM_TLM_MAX_WAIT_MS) {
        tlm_pending = false;
    }
    // Saved settings changed, from the encoder or from here
    if (memcmp(&cfg_reported, &ee_storage, sizeof(ee_storage_t)) != 0) {
        memcpy(&cfg_reported, &ee_storage, sizeof(ee_storage_t));
        cfg_pending = true;
        cfg_next_key = 0;
    }
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
    tlm_last_uptime = device_uptime;
    memcpy(&cfg_reported, &ee_storage, sizeof(ee_storage_t));
}

void comm_task(void)
{
    uint32_t now = HAL_GetTick();
    gps_comm_rx_check();
    comm_process_rx(now);
    comm_update_pending(now);
    comm_process_tx(now);
    comm_process_save(now);
}
