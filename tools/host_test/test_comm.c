// Host tests for the serial protocol: the real firmware sources (comm, gps, menu, int, frequency) run
// against a fake HAL, with simulated time. This checks logic only: real UART, DMA and flash timing need
// the bench tests from doc/PROTOCOL.md.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comm.h"
#include "eeprom.h"
#include "frequency.h"
#include "gps.h"
#include "int.h"
#include "menu.h"
#include "hal_stub.h"

// ---------------------------------------------------------------- fake HAL
GPIO_TypeDef stub_gpio;
TIM_TypeDef stub_tim1, stub_tim2, stub_tim3;
TIM_HandleTypeDef htim1, htim2, htim3;
USART_TypeDef stub_usart2, stub_usart3;
static DMA_HandleTypeDef dma2, dma3;
UART_HandleTypeDef huart2 = { .hdmarx = &dma2 }, huart3 = { .hdmarx = &dma3 };
ee_storage_t ee_storage;

static ee_storage_t flash;
static uint32_t now = 1;
static int ee_writes, resets, reinit_usart2, reinit_usart3, lcd_clears;
static bool button_up = true;

static uint8_t* rx_buf[2];
static uint16_t rx_size[2];

#define OUT_MAX 65536
static char link_out[OUT_MAX]; // USART2 TX: what the ESP32 receives
static size_t link_out_len;
static char gps_out[OUT_MAX];  // USART3 TX: what the GPS receives
static size_t gps_out_len;

// Time needed to send a byte, in 1/1000 ms
static uint32_t byte_us2 = 87, byte_us3 = 87;
// gps_sendcommand() waits for its transmission to end, which time passing in step() can't provide
static bool tx3_instant = false;
static uint32_t tx2_busy_until_us, tx3_busy_until_us;
static uint64_t now_us(void) { return (uint64_t)now * 1000; }

uint32_t HAL_GetTick(void) { return now; }
void HAL_Delay(uint32_t ms) { now += ms; }
uint32_t HAL_RCC_GetHCLKFreq(void) { return 70000000; }
uint32_t HAL_TIM_ReadCapturedValue(TIM_HandleTypeDef* h, uint32_t c) { return 0; }
HAL_StatusTypeDef HAL_TIM_Base_Start_IT(TIM_HandleTypeDef* h) { return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef* h, uint32_t c) { return HAL_OK; }
HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef* h, uint32_t c) { return HAL_OK; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef* p, uint16_t pin) { return button_up ? GPIO_PIN_SET : GPIO_PIN_RESET; }
void HAL_GPIO_WritePin(GPIO_TypeDef* p, uint16_t pin, GPIO_PinState s) { }
void Error_Handler(void) { resets++; }
void LCD_Init(void) { }
void LCD_Clear(void) { lcd_clears++; }
void LCD_Puts(uint8_t x, uint8_t y, const char* t) { }
void LCD_PutCustom(uint8_t x, uint8_t y, uint8_t c) { }
void LCD_CreateChar(uint8_t c, uint8_t* d) { }
bool EE_Init(void* d, uint32_t s) { return true; }
void EE_Read(void) { }
bool EE_Write(void) { ee_writes++; flash = ee_storage; now += 30; return true; }
bool eeprom_matches_flash(void) { return memcmp(&flash, &ee_storage, sizeof(flash)) == 0; }

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef* h)
{
    h->gState = HAL_UART_STATE_READY;
    h->RxState = HAL_UART_STATE_READY;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef* h)
{
    if (h == &huart2) reinit_usart2++; else reinit_usart3++;
    h->gState = HAL_UART_STATE_RESET;
    h->RxState = HAL_UART_STATE_RESET;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef* h, const uint8_t* d, uint16_t n)
{
    if (h->gState != HAL_UART_STATE_READY) { resets += 100; return HAL_BUSY; }
    if (h == &huart2) {
        memcpy(link_out + link_out_len, d, n); link_out_len += n;
        tx2_busy_until_us = (uint32_t)now_us() + n * byte_us2;
    } else {
        memcpy(gps_out + gps_out_len, d, n); gps_out_len += n;
        tx3_busy_until_us = (uint32_t)now_us() + n * byte_us3;
    }
    h->gState = (h == &huart3 && tx3_instant) ? HAL_UART_STATE_READY : HAL_UART_STATE_BUSY;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_UART_Receive_DMA(UART_HandleTypeDef* h, uint8_t* d, uint16_t n)
{
    if (h->RxState != HAL_UART_STATE_READY) return HAL_BUSY;
    int i = (h == &huart2) ? 0 : 1;
    rx_buf[i] = d; rx_size[i] = n;
    h->hdmarx->counter = n;
    h->RxState = HAL_UART_STATE_BUSY;
    return HAL_OK;
}

// ---------------------------------------------------------------- simulation
static void dma_receive(UART_HandleTypeDef* h, uint8_t c)
{
    int i = (h == &huart2) ? 0 : 1;
    if (h->RxState != HAL_UART_STATE_BUSY) return; // Not receiving: byte lost
    rx_buf[i][rx_size[i] - h->hdmarx->counter] = c;
    if (--h->hdmarx->counter == 0) {
        h->RxState = HAL_UART_STATE_READY;
        HAL_UART_RxCpltCallback(h);
    }
}

// One main loop pass, 1 ms later
static void step(void)
{
    now++;
    if (now_us() >= tx2_busy_until_us && huart2.gState == HAL_UART_STATE_BUSY) huart2.gState = HAL_UART_STATE_READY;
    if (now_us() >= tx3_busy_until_us && huart3.gState == HAL_UART_STATE_BUSY) huart3.gState = HAL_UART_STATE_READY;
    gps_read();
    menu_run();
}
static void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) step(); }

static void gps_feed(const char* s) { for (; *s; s++) dma_receive(&huart3, (uint8_t)*s); }
// As the wire does: bytes spread in time
static void gps_send_timed(const char* s, int ms_per_byte)
{
    for (; *s; s++) { dma_receive(&huart3, (uint8_t)*s); run(ms_per_byte); }
}
static void link_feed(const char* s)
{
    for (; *s; s++) { dma_receive(&huart2, (uint8_t)*s); step(); }
}

static void clear_out(void) { link_out_len = 0; link_out[0] = 0; gps_out_len = 0; gps_out[0] = 0; }

// Sends a command with a correct checksum
static void cmd(const char* body)
{
    uint8_t cs = 0;
    for (const char* p = body; *p; p++) cs ^= (uint8_t)*p;
    char line[256];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
    link_feed(line);
    run(20);
}
static bool out_has(const char* text)
{
    link_out[link_out_len] = 0;
    return strstr(link_out, text) != NULL;
}
static int out_count(const char* text)
{
    link_out[link_out_len] = 0;
    int n = 0;
    for (const char* p = link_out; (p = strstr(p, text)) != NULL; p += strlen(text)) n++;
    return n;
}

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; if (_a != _b) { failures++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __func__, __LINE__, #a, _a, _b); } } while (0)

// Every line on the link must be well formed: right checksum for all our sentences and for NMEA ones
static int bad_lines(void)
{
    int bad = 0;
    link_out[link_out_len] = 0;
    char* copy = strdup(link_out);
    for (char* line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        char* star = strrchr(line, '*');
        if (line[0] != '$' || !star || strlen(star) != 3 || strchr(line + 1, '$')) { bad++; printf("  bad line: %s\n", line); continue; }
        uint8_t cs = 0;
        for (char* p = line + 1; p < star; p++) cs ^= (uint8_t)*p;
        if (cs != strtol(star + 1, NULL, 16)) { bad++; printf("  bad checksum: %s\n", line); }
    }
    free(copy);
    return bad;
}

static const char GGA[] = "$GPGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.9,545.4,M,46.9,M,,*69\r\n";
static const char RMC[] = "$GPRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*44\r\n";
static const char GSA[] = "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\r\n";

// Fresh state, as after boot with the defaults main.c applies
static void boot(void)
{
    memset(&ee_storage, 0xff, sizeof(ee_storage));
    ee_storage.contrast = contrast = 80;
    ee_storage.pps_sync_on = 1; pps_sync_on = true;
    ee_storage.pps_sync_delay = pps_sync_delay = 10;
    ee_storage.pps_sync_threshold = pps_sync_threshold = 30000;
    ee_storage.pps_ppm_auto_sync = 1; pps_ppm_auto_sync = true;
    ee_storage.pwm_auto_save = 1; pwm_auto_save = false; // auto save would disturb write counts
    ee_storage.pwm_auto_save = 0;
    ee_storage.trend_auto_v = 1; trend_auto_v = true;
    ee_storage.trend_auto_h = 1; trend_auto_h = true;
    ee_storage.trend_v_scale = trend_v_scale = 70;
    ee_storage.trend_h_scale = trend_h_scale = 1;
    ee_storage.boot_menu = 0;
    ee_storage.gps_baudrate = 9600; menu_set_gps_baudrate(9600);
    ee_storage.gps_time_offset = 14; gps_time_offset = 0;
    ee_storage.gps_date_format = 0; gps_date_format = DATE_FORMAT_UTC;
    ee_storage.gps_model = GPS_MODEL_UNKNOWN; gps_model = GPS_MODEL_UNKNOWN;
    ee_storage.ppb_lock_threshold = ppb_lock_threshold = 50;
    ee_storage.ocxo_model = OCXO_MODEL_ISOTEMP; ocxo_model = OCXO_MODEL_ISOTEMP;
    ee_storage.correction_algorithm = CORRECTION_ALGO_ERIC_H; correction_algorithm = CORRECTION_ALGO_ERIC_H;
    menu_set_correction_algorithm(CORRECTION_ALGO_ERIC_H);
    ee_storage.correction_factor = correction_factor = 300;
    ee_storage.warmup_time_seconds = warmup_time_seconds = 70;
    ee_storage.comm_baudrate = COMM_DEFAULT_BAUDRATE; menu_set_comm_baudrate(COMM_DEFAULT_BAUDRATE);
    flash = ee_storage;
    gps_init_comm_uart(ee_storage.comm_baudrate);
    HAL_UART_Init(&huart3);
    gps_start_it();
    comm_init();
    run(50);
    cmd("PGPSDO,CMD,TLM,1");
    clear_out();
    // Far from any earlier save, and with no leftover rate limiting
    run(11000);
    clear_out();
    ee_writes = resets = reinit_usart2 = reinit_usart3 = 0;
}
// Periodic TLM would add noise to exact output checks
static void telemetry_off(void) { cmd("PGPSDO,CMD,TLM,0"); clear_out(); }

// ---------------------------------------------------------------- tests
static void test_brief_examples(void)
{
    // Exact bytes from the protocol document
    boot(); telemetry_off();
    cmd("PGPSDO,GET,VER");
    CHECK(out_has("$PGPSDO,ACK,GET,VER,OK*"));
    CHECK(out_has("$PGPSDO,VER,1,v0.1.16-web1*5F\r\n"));
    clear_out();
    cmd("PGPSDO,SET,pps_sync_delay,30");
    CHECK(out_has("$PGPSDO,ACK,SET,pps_sync_delay,OK*11\r\n"));
    CHECK_EQ(pps_sync_delay, 30);
    clear_out();
    cmd("PGPSDO,SET,contrast,101");
    CHECK(out_has("$PGPSDO,ACK,SET,contrast,ERR,RANGE*20\r\n"));
    clear_out();
    link_feed("$PGPSDO,CMD,SAVEPWM*1E\r\n"); run(50);
    CHECK(out_has("$PGPSDO,ACK,CMD,SAVEPWM,OK*53\r\n"));
    link_feed("$PGPSDO,GET,CFG*0B\r\n"); run(50);
    CHECK(out_has("$PGPSDO,ACK,GET,CFG,OK*"));
    clear_out();
    sync_pps_out = false;
    link_feed("$PGPSDO,CMD,FORCESYNC*0F\r\n"); run(50);
    CHECK(out_has("$PGPSDO,ACK,CMD,FORCESYNC,OK*"));
    CHECK(sync_pps_out);
    CHECK_EQ(gps_out_len, 0);
    sync_pps_out = false;
}

static void test_telemetry(void)
{
    boot();
    // Values from the document's TLM example
    device_uptime = 1233;
    run(2);
    clear_out();
    gps_lock_status = true; ppb_lock_status = true; num_sats = 9;
    ppb_frequency = 70000000; ppb_error = 0; ppb_correction = -1; TIM1->CCR2 = 38212; ppb_millis = 0;
    pps_error = 12; pps_millis = 0; pps_sync_count = 4;
    frequency_allow_adjustment(true);
    for (int i = 0; i < CIRCULAR_BUFFER_LEN; i++) circbuf_add(&circular_buffer, 0);
    num_samples = CIRCULAR_BUFFER_LEN;
    device_uptime = 1234;
    run(5);
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 1);
    CHECK(out_has("$PGPSDO,TLM,1234,1,1,9,0,0,70000000,0,-1,38212,0,12,0,4,1*"));
    // Cadence: one per uptime change, none in between
    clear_out();
    for (int s = 0; s < 10; s++) { device_uptime++; run(1000); }
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 10);
    // Off and on
    clear_out(); cmd("PGPSDO,CMD,TLM,0"); clear_out();
    device_uptime++; run(1100);
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 0);
    cmd("PGPSDO,CMD,TLM,1"); clear_out();
    device_uptime++; run(100);
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 1);
    // No samples yet: the value the LCD shows as "?"
    clear_out(); num_samples = 0; device_uptime++; run(10);
    CHECK(out_has(",65535,"));
    // Negative instant ppb, longest values still fit
    clear_out(); num_samples = 1; ppb_error = -7; device_uptime++; run(10);
    CHECK(out_has(",-10000,")); // -7 Hz at 70 MHz = -100.00 ppb
    frequency_allow_adjustment(false);
    CHECK_EQ(bad_lines(), 0);
    ppb_error = 0;
}

static void test_passthrough_not_spliced(void)
{
    boot();
    // A second of NMEA at 9600 baud, telemetry and commands falling anywhere: all lines stay intact
    for (int offset = 0; offset < 60; offset++) {
        clear_out();
        for (int s = 0; s < 3; s++) {
            device_uptime++;
            run(offset);
            gps_send_timed(GGA, 1);
            cmd("PGPSDO,GET,CFG");
            gps_send_timed(RMC, 1);
            // Telemetry and a reply both become due in the middle of this line: they must wait for its end
            char first_half[32] = { 0 };
            strncpy(first_half, GSA, 20);
            gps_send_timed(first_half, 1);
            device_uptime++;
            cmd("PGPSDO,GET,VER");
            gps_send_timed(GSA + 20, 1);
            run(100);
        }
        CHECK_EQ(bad_lines(), 0);
        CHECK_EQ(out_count("$GPGGA"), 3);
        CHECK_EQ(out_count("$GPRMC"), 3);
        CHECK_EQ(out_count("$GPGSA"), 3);
        CHECK(out_count("$PGPSDO,TLM,") >= 3);
    }
    // A GGA was parsed from it, with partial DMA blocks too
    CHECK_EQ(num_sats, 8);
    // Final bytes of a burst arrive without waiting for 20 more
    clear_out();
    gps_feed("$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39\r\n");
    run(3);
    CHECK(out_has("2.1*39\r\n"));
}

static void test_commands_to_gps(void)
{
    boot(); telemetry_off();
    // Other NMEA passes through, starting like ours or not
    link_feed("$PCAS01,5*19\r\n$PGPS,hello\r\n$PGPSDX,1\r\n");
    run(100);
    CHECK_EQ(gps_out_len, strlen("$PCAS01,5*19\r\n$PGPS,hello\r\n$PGPSDX,1\r\n"));
    CHECK(strstr(gps_out, "$PCAS01,5*19\r\n$PGPS,hello\r\n$PGPSDX,1\r\n") != NULL);
    CHECK_EQ(link_out_len, 0);
    // Binary UBX, with zeros and a newline in it
    clear_out();
    static const uint8_t ubx[] = { 0xB5, 0x62, 0x06, 0x00, 0x0A, 0x00, 0x01, 0x24, 0x50, 0x47 };
    for (size_t i = 0; i < sizeof(ubx); i++) { dma_receive(&huart2, ubx[i]); step(); }
    run(100);
    CHECK_EQ(gps_out_len, sizeof(ubx));
    CHECK(memcmp(gps_out, ubx, sizeof(ubx)) == 0);
    run(1100); // Line never ended: back to line start
    clear_out();
    // Our sentences never reach the GPS, even between other traffic
    link_feed("$PCAS00*01\r\n");
    cmd("PGPSDO,SET,contrast,50");
    link_feed("$PCAS00*01\r\n");
    run(50);
    CHECK_EQ(contrast, 50);
    CHECK(strstr(gps_out, "PGPSDO") == NULL);
    CHECK_EQ(gps_out_len, 2 * strlen("$PCAS00*01\r\n"));
    // Partial prefix then silence: was not for us
    clear_out();
    link_feed("$PGP");
    run(1100);
    CHECK_EQ(gps_out_len, 4);
    // Burst larger than a GPS UART chunk, link much faster than the GPS: nothing lost up to the FIFO size
    boot(); telemetry_off();
    byte_us3 = 1042; // 9600 baud
    char burst[201]; memset(burst, 'x', 200); burst[200] = 0;
    gps_out_len = 0;
    for (int i = 0; i < 200; i++) dma_receive(&huart2, (uint8_t)burst[i]);
    run(400);
    CHECK_EQ(gps_out_len, 200);
    byte_us3 = 87;
    run(1100);
}

static void test_errors(void)
{
    boot(); telemetry_off();
    int writes = ee_writes;
    link_feed("$PGPSDO,SET,contrast,50*00\r\n"); run(5);
    CHECK(out_has("$PGPSDO,ACK,ERR,CHECKSUM*"));
    CHECK_EQ(contrast, 80);
    clear_out(); link_feed("$PGPSDO,SET,contrast,50\r\n"); run(5);
    CHECK(out_has("$PGPSDO,ACK,ERR,CHECKSUM*"));
    clear_out(); link_feed("$PGPSDO,SET,contrast,50*ZZ\r\n"); run(5);
    CHECK(out_has("$PGPSDO,ACK,ERR,CHECKSUM*"));
    clear_out(); cmd("PGPSDO,SET,nope,1");
    CHECK(out_has("$PGPSDO,ACK,SET,nope,ERR,KEY*"));
    clear_out(); cmd("PGPSDO,SET,fw_version,1");
    CHECK(out_has("$PGPSDO,ACK,SET,fw_version,ERR,KEY*"));
    clear_out(); cmd("PGPSDO,SET,gps_model,1");
    CHECK(out_has("ERR,KEY*"));
    clear_out(); cmd("PGPSDO,SET,contrast");
    CHECK(out_has("$PGPSDO,ACK,SET,contrast,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,SET,contrast,abc");
    CHECK(out_has("ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,SET,contrast,");
    CHECK(out_has("ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,SET,contrast,12345678901");
    CHECK(out_has("ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,SET,contrast,50,9");
    CHECK(out_has("$PGPSDO,ACK,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,SET");
    CHECK(out_has("$PGPSDO,ACK,SET,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,BOGUS,1");
    CHECK(out_has("$PGPSDO,ACK,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,GET,NOPE");
    CHECK(out_has("$PGPSDO,ACK,GET,NOPE,ERR,KEY*"));
    clear_out(); cmd("PGPSDO,CMD,NOPE");
    CHECK(out_has("$PGPSDO,ACK,CMD,NOPE,ERR,KEY*"));
    clear_out(); cmd("PGPSDO,CMD,TLM");
    CHECK(out_has("$PGPSDO,ACK,CMD,TLM,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,CMD,TLM,2");
    CHECK(out_has("$PGPSDO,ACK,CMD,TLM,ERR,RANGE*"));
    // Long key: reply stays bounded
    clear_out(); cmd("PGPSDO,SET,aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,1");
    CHECK(out_has("ERR,KEY*"));
    CHECK_EQ(bad_lines(), 0);
    // Too long line: one error, and the next line works
    clear_out();
    char long_line[400] = "$PGPSDO,";
    memset(long_line + 8, 'a', 300); strcat(long_line, "\r\n");
    link_feed(long_line); run(5);
    CHECK_EQ(out_count("$PGPSDO,ACK,ERR,FORMAT*"), 1);
    clear_out(); cmd("PGPSDO,GET,VER");
    CHECK(out_has("ACK,GET,VER,OK"));
    // Line cut before its end, then a new one after a pause
    clear_out();
    link_feed("$PGPSDO,SET,contrast,5");
    run(1100);
    CHECK(out_has("$PGPSDO,ACK,ERR,FORMAT*"));
    clear_out(); cmd("PGPSDO,GET,VER");
    CHECK(out_has("ACK,GET,VER,OK"));
    CHECK_EQ(contrast, 80);
    run(15000);
    CHECK_EQ(ee_writes, writes);
}

typedef struct { const char* key; const char* good; const char* bad; } key_case;

static void test_all_keys(void)
{
    static const key_case cases[] = {
        { "contrast", "42", "101" }, { "contrast", "0", "-1" },
        { "pwm_auto_save", "1", "2" }, { "pps_auto_sync", "0", "2" },
        { "ppb_lock_threshold", "1000", "1001" },
        { "ocxo_model", "1", "3" }, { "warmup_time", "1000", "1001" }, { "warmup_time", "0", "-1" },
        { "correction_algo", "1", "3" }, { "correction_factor", "60", "61" }, { "correction_factor", "1", "0" },
        { "pps_sync_on", "0", "2" }, { "pps_sync_delay", "30", "4294967295" },
        { "pps_sync_threshold", "1000", "-1" },
        { "trend_auto_v", "0", "2" }, { "trend_auto_h", "0", "2" },
        { "trend_v_scale", "150", "4294967295" }, { "trend_h_scale", "8", "4294967295" },
        { "gps_time_offset", "-14", "-15" }, { "gps_time_offset", "14", "15" },
        { "date_format", "4", "5" }, { "gps_baudrate", "38400", "38401" },
        { "boot_menu", "3", "4" }, { "comm_baudrate", "57600", "1234" },
    };
    boot(); telemetry_off();
    // Fredzo limits apply to the factor cases below
    cmd("PGPSDO,SET,correction_algo,1");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char body[80], expect[80];
        // Valid value: OK, reported, saved, and setting it again changes nothing
        clear_out();
        snprintf(body, sizeof(body), "PGPSDO,SET,%s,%s", cases[i].key, cases[i].good);
        cmd(body);
        snprintf(expect, sizeof(expect), "$PGPSDO,ACK,SET,%s,OK*", cases[i].key);
        CHECK(out_has(expect));
        if (!out_has(expect)) printf("    key %s\n", cases[i].key);
        run(14000);
        CHECK(eeprom_matches_flash());
        int writes = ee_writes;
        clear_out();
        cmd(body);
        run(14000);
        CHECK(out_has(expect));
        CHECK_EQ(ee_writes, writes);
        CHECK_EQ(out_count("$PGPSDO,CFG,"), 0);
        // Out of range: rejected, nothing changes
        ee_storage_t before = ee_storage;
        clear_out();
        snprintf(body, sizeof(body), "PGPSDO,SET,%s,%s", cases[i].key, cases[i].bad);
        cmd(body);
        snprintf(expect, sizeof(expect), "$PGPSDO,ACK,SET,%s,ERR,RANGE*", cases[i].key);
        CHECK(out_has(expect));
        if (!out_has(expect)) printf("    key %s: %s\n", cases[i].key, link_out);
        CHECK(memcmp(&before, &ee_storage, sizeof(before)) == 0);
    }
    CHECK_EQ(resets, 0);
    // Stored encodings
    CHECK_EQ(gps_time_offset, 14);
    CHECK_EQ(ee_storage.gps_time_offset, 28);
    cmd("PGPSDO,SET,gps_time_offset,-14");
    CHECK_EQ(ee_storage.gps_time_offset, 0);
}

static void test_side_effects(void)
{
    boot(); telemetry_off();
    // OCXO model resets warm-up time
    cmd("PGPSDO,SET,warmup_time,123");
    cmd("PGPSDO,SET,ocxo_model,1");
    CHECK_EQ(ocxo_model, OCXO_MODEL_OX256B);
    CHECK_EQ(warmup_time_seconds, 100);
    CHECK_EQ(ee_storage.warmup_time_seconds, 100);
    // Correction algorithm resets the factor, and limits follow the algorithm
    cmd("PGPSDO,SET,correction_algo,1");
    CHECK_EQ(correction_algorithm, CORRECTION_ALGO_FREDZO);
    CHECK_EQ(correction_factor, 10);
    CHECK_EQ(ee_storage.correction_factor, 10);
    clear_out(); cmd("PGPSDO,SET,correction_factor,600");
    CHECK(out_has("ERR,RANGE"));
    cmd("PGPSDO,SET,correction_algo,2");
    CHECK_EQ(correction_factor, 300);
    clear_out(); cmd("PGPSDO,SET,correction_factor,600");
    CHECK(out_has("correction_factor,OK"));
    // Trend scales: same rounding as the encoder, refused while auto-scale would overwrite them
    clear_out(); cmd("PGPSDO,SET,trend_v_scale,150");
    CHECK(out_has("ERR,BUSY"));
    cmd("PGPSDO,SET,trend_auto_v,0");
    cmd("PGPSDO,SET,trend_v_scale,144");
    CHECK_EQ(trend_v_scale, 140);
    cmd("PGPSDO,SET,trend_auto_h,0");
    cmd("PGPSDO,SET,trend_h_scale,9");
    CHECK_EQ(trend_h_scale, 8);
    // Screen refresh, so the LCD shows the new value
    refresh_screen = false;
    uint8_t cs = 0; for (const char* p = "PGPSDO,SET,contrast,33"; *p; p++) cs ^= (uint8_t)*p;
    char line[64]; snprintf(line, sizeof(line), "$PGPSDO,SET,contrast,33*%02X\r\n", cs);
    for (const char* p = line; *p; p++) dma_receive(&huart2, (uint8_t)*p);
    now++; gps_read(); // Without menu_run(), which consumes the flag
    CHECK(refresh_screen);
    CHECK_EQ(contrast, 33);
    CHECK_EQ(TIM1->CCR3, 0xFFFF - (33 * 0xFFFF / 100));
    run(5);
    // SAVEPWM: saves the current PWM, only if different
    run(14000);
    TIM1->CCR2 = 40000;
    int writes = ee_writes;
    cmd("PGPSDO,CMD,SAVEPWM");
    run(14000);
    CHECK_EQ(ee_storage.pwm, 40000);
    CHECK_EQ(flash.pwm, 40000);
    CHECK_EQ(ee_writes, writes + 1);
    cmd("PGPSDO,CMD,SAVEPWM");
    run(14000);
    CHECK_EQ(ee_writes, writes + 1);
}

static void test_flash_policy(void)
{
    boot(); telemetry_off();
    // Save happens soon after a PPS pulse, away from the next one and from GPS data
    last_pps_out = now;
    cmd("PGPSDO,SET,contrast,1");
    run(5);
    CHECK_EQ(ee_writes, 1);
    CHECK(eeprom_matches_flash());
    // A burst of changes right after: applied at once, one erase
    for (int v = 2; v < 12; v++) {
        char body[64]; snprintf(body, sizeof(body), "PGPSDO,SET,contrast,%d", v);
        cmd(body);
        CHECK_EQ(contrast, v);
    }
    CHECK_EQ(ee_writes, 1);
    run(10000);
    last_pps_out = now;
    run(10);
    CHECK_EQ(ee_writes, 2);
    CHECK_EQ(flash.contrast, 11);
    // No more than one per interval, however long this goes on
    int writes = ee_writes;
    for (int v = 0; v < 60; v++) {
        char body[64]; snprintf(body, sizeof(body), "PGPSDO,SET,contrast,%d", 20 + v % 2);
        last_pps_out = now;
        cmd(body);
        run(1000);
    }
    CHECK(ee_writes - writes <= 7);
    CHECK(ee_writes - writes >= 5);
    run(14000);
    CHECK(eeprom_matches_flash());
    // Bad timing, too close to the next pulse: waits for a good moment
    last_pps_out = now - 950;
    writes = ee_writes;
    cmd("PGPSDO,SET,contrast,2");
    run(10);
    CHECK_EQ(ee_writes, writes);
    last_pps_out = now;
    run(3);
    CHECK_EQ(ee_writes, writes + 1);
    // GPS data flowing: same. And never later than the maximum wait, whatever the timing.
    run(11000);
    writes = ee_writes;
    last_pps_out = now - 950;
    cmd("PGPSDO,SET,contrast,3");
    uint32_t start = now;
    while (ee_writes == writes && now - start < 5000) step();
    CHECK_EQ(ee_writes, writes + 1);
    CHECK(now - start <= 3100);
    CHECK(now - start >= 2900);
    // GPS busy within a good window: no save while bytes arrive, saved once quiet
    run(11000);
    writes = ee_writes;
    last_pps_out = now;
    gps_feed("$GPGSA,");
    {
        // GPS bytes keep arriving while the command does
        uint8_t cs = 0; for (const char* p = "PGPSDO,SET,contrast,4"; *p; p++) cs ^= (uint8_t)*p;
        char line[64]; snprintf(line, sizeof(line), "$PGPSDO,SET,contrast,4*%02X\r\n", cs);
        for (const char* p = line; *p; p++) { dma_receive(&huart2, (uint8_t)*p); gps_feed("0"); step(); }
    }
    CHECK_EQ(contrast, 4);
    for (int i = 0; i < 300; i++) { gps_feed("0"); step(); }
    CHECK_EQ(ee_writes, writes);
    gps_feed("\r\n");
    last_pps_out = now;
    run(40);
    CHECK_EQ(ee_writes, writes + 1);
    // An encoder save in between leaves nothing to do
    run(11000);
    last_pps_out = now - 950;
    cmd("PGPSDO,SET,contrast,6");
    EE_Write();
    writes = ee_writes;
    run(4000);
    CHECK_EQ(ee_writes, writes);
}

static void test_baudrate_independent(void)
{
    boot(); telemetry_off();
    gps_model = GPS_MODEL_NEO6M;
    // Remote change: USART3 only, USART2 receive untouched and no reset from restarting it
    cmd("PGPSDO,SET,gps_baudrate,38400");
    CHECK_EQ(gps_baudrate, 38400);
    CHECK_EQ(huart3.Init.BaudRate, 38400);
    CHECK_EQ(huart2.Init.BaudRate, COMM_DEFAULT_BAUDRATE);
    CHECK_EQ(reinit_usart2, 0);
    CHECK_EQ(reinit_usart3, 1);
    CHECK_EQ(resets, 0);
    // GPS watchdog path: same
    gps_reconfigure_uart(gps_baudrate);
    CHECK_EQ(reinit_usart2, 0);
    CHECK_EQ(resets, 0);
    clear_out();
    cmd("PGPSDO,GET,VER");
    CHECK(out_has("ACK,GET,VER,OK"));
    // ATGM336H limit, as with the encoder
    gps_model = GPS_MODEL_ATGM336H;
    tx3_instant = true;
    clear_out(); cmd("PGPSDO,SET,gps_baudrate,230400");
    CHECK(out_has("ERR,RANGE"));
    cmd("PGPSDO,SET,gps_baudrate,9600");
    tx3_instant = false;
    CHECK(strstr(gps_out, "$PCAS01,1*1D") != NULL); // Module is told
    gps_model = GPS_MODEL_UNKNOWN;
    // Link baudrate: saved, applied at next boot
    clear_out(); cmd("PGPSDO,SET,comm_baudrate,9600");
    run(50);
    CHECK_EQ(ee_storage.comm_baudrate, 9600);
    CHECK_EQ(huart2.Init.BaudRate, COMM_DEFAULT_BAUDRATE);
    CHECK(out_has("comm_baudrate=9600"));
    // Unset value from units upgraded from an older firmware
    CHECK(!settings_is_valid_baudrate(0xFFFFFFFF));
    CHECK(settings_is_valid_baudrate(115200));
    CHECK(!settings_is_valid_baudrate(0));
}

static void test_rx_error_recovery(void)
{
    boot(); telemetry_off();
    // What the HAL does on a framing error with DMA reception
    huart2.RxState = HAL_UART_STATE_READY;
    clear_out();
    step();
    CHECK(huart2.RxState == HAL_UART_STATE_BUSY);
    CHECK_EQ(resets, 0);
    cmd("PGPSDO,GET,VER");
    CHECK(out_has("ACK,GET,VER,OK"));
}

static void test_stuck_gps_line(void)
{
    boot();
    // GPS stops mid-line: after the silence, sentences go out on their own line
    run(300);
    clear_out();
    // Not a GGA: gps_parse() dereferences NULL on a truncated one (upstream, harmless on the MCU but not here)
    gps_send_timed("$GPGSA,123", 1);
    device_uptime++;
    run(100);
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 0);
    run(300);
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 1);
    CHECK(out_has("$GPGSA,123\r\n$PGPSDO,TLM,"));
    // Binary traffic mid-line, never quiet: telemetry dropped after 500 ms, replies kept, no splice
    gps_send_timed("\r\n", 1);
    clear_out();
    gps_send_timed("X", 1);
    device_uptime++;
    for (int i = 0; i < 800; i++) { gps_send_timed("X", 1); }
    CHECK_EQ(out_count("$PGPSDO,TLM,"), 0);
    device_uptime++;
    link_feed("$PGPSDO,GET,VER*0D\r\n");
    gps_send_timed("X", 1);
    CHECK_EQ(out_count("$PGPSDO,ACK"), 0); // Reply waits for the end of the line
    gps_send_timed("\r\n", 1);
    run(5);
    CHECK_EQ(out_count("$PGPSDO,ACK"), 1);
    // That sentence came after the line end
    CHECK(strstr(link_out, "X$PGPSDO") == NULL);
    run(1100);
}

static void test_slow_link_does_not_hurt_gps(void)
{
    boot(); telemetry_off();
    byte_us2 = 5000; // Very slow link
    num_sats = 0;
    int before = gga_frames;
    // Faster GPS than link: more data than the FIFO holds in a burst
    for (int i = 0; i < 20; i++) {
        gps_feed(GSA); run(1);
        gps_feed(RMC); run(1);
        gps_feed(GSA); run(1);
        gps_feed(GGA); run(1);
    }
    CHECK_EQ(gga_frames - before, 20);
    CHECK_EQ(num_sats, 8);
    byte_us2 = 87;
    run(3000);
    CHECK_EQ(resets, 0);
    run(1100);
}

// --- encoder helpers: drive the real menu_run()
void HAL_GPIO_EXTI_Callback(uint16_t pin);

static void encoder(int steps)
{
    TIM3->CNT += steps * 2;
    step();
    run(2);
}
static void click(void)
{
    button_up = false; HAL_GPIO_EXTI_Callback(ROTARY_PRESS_Pin);
    run(60);
    button_up = true; HAL_GPIO_EXTI_Callback(ROTARY_PRESS_Pin);
    run(60);
}

static void test_encoder(void)
{
    boot(); telemetry_off();
    // Open the GPS menu, reach the Comm entry right after Baudrate (index 10 -> 11)
    menu_set_current_menu(6);
    click();                // Level 1, GPS Time
    encoder(11);            // Moves one screen per pass whatever the steps
    for (int i = 1; i < 11; i++) encoder(1);
    click();                // Level 2: edit
    int writes = ee_writes;
    clear_out();
    encoder(1);             // 115200 -> 230400, not saved yet
    CHECK_EQ(comm_baudrate, 230400);
    CHECK_EQ(ee_storage.comm_baudrate, COMM_DEFAULT_BAUDRATE);
    CHECK_EQ(out_count("$PGPSDO,CFG,"), 0);
    encoder(-1); encoder(-1); // 57600
    CHECK_EQ(comm_baudrate, 57600);
    click();                // Save
    CHECK_EQ(ee_storage.comm_baudrate, 57600);
    CHECK_EQ(ee_writes, writes + 1);
    run(100);
    // Encoder saves are reported
    CHECK_EQ(out_count("$PGPSDO,CFG,"), 3); // One report, on three lines
    CHECK(out_has("comm_baudrate=57600"));
    CHECK_EQ(huart2.Init.BaudRate, COMM_DEFAULT_BAUDRATE); // After reboot only
    // Same for a menu entry that was already there, and wrap around from the first value
    clear_out();
    encoder(-1); // Back to GPS Baudrate
    click();
    encoder(1);  // 19200
    click();
    run(100);
    CHECK_EQ(ee_storage.gps_baudrate, 19200);
    CHECK(out_has("gps_baudrate=19200"));
    CHECK_EQ(reinit_usart2, 0);
    CHECK_EQ(huart2.Init.BaudRate, COMM_DEFAULT_BAUDRATE);
    CHECK_EQ(resets, 0);
    // A remote change while the encoder is elsewhere: saves stay consistent
    clear_out();
    cmd("PGPSDO,SET,pps_sync_delay,77");
    run(14000);
    CHECK_EQ(flash.pps_sync_delay, 77);
}

static void test_cfg(void)
{
    boot(); telemetry_off();
    cmd("PGPSDO,GET,CFG");
    run(50);
    CHECK_EQ(bad_lines(), 0);
    static const char* const keys[] = {
        "contrast=80", "pwm_auto_save=", "pps_auto_sync=1", "ppb_lock_threshold=50", "ocxo_model=0",
        "warmup_time=70", "correction_algo=2", "correction_factor=300", "pps_sync_on=1", "pps_sync_delay=10",
        "pps_sync_threshold=30000", "trend_auto_v=1", "trend_auto_h=1", "trend_v_scale=70", "trend_h_scale=1",
        "gps_time_offset=0", "date_format=0", "gps_baudrate=9600", "boot_menu=0", "comm_baudrate=115200",
        "gps_model=3", "fw_version=v0.1.16-web1",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CHECK(out_has(keys[i]));
        if (!out_has(keys[i])) printf("    missing %s\n", keys[i]);
    }
    // Several lines, none over the limit
    CHECK(out_count("$PGPSDO,CFG,") >= 2);
    link_out[link_out_len] = 0;
    for (char* p = link_out; (p = strstr(p, "$PGPSDO,CFG,")) != NULL; p++) {
        CHECK(strchr(p, '\n') - p + 1 <= 160);
    }
    // Negative value
    clear_out(); cmd("PGPSDO,SET,gps_time_offset,-5"); run(50);
    CHECK(out_has("gps_time_offset=-5"));
    // Module detection, not a setting change from anyone: still reported
    clear_out();
    gps_send_timed("$GPTXT,01,01,02,HW UBX-G70xx*50\r\n", 1);
    run(50);
    CHECK(out_has("gps_model=1"));
    // CFG lines sit between GPS lines
    clear_out();
    cmd("PGPSDO,GET,CFG");
    gps_send_timed(GGA, 1);
    run(50);
    CHECK_EQ(bad_lines(), 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    TIM3->CNT = 1000;
    struct { const char* name; void (*fn)(void); } tests[] = {
        { "protocol document examples", test_brief_examples },
        { "telemetry", test_telemetry },
        { "passthrough never spliced", test_passthrough_not_spliced },
        { "commands to GPS", test_commands_to_gps },
        { "errors", test_errors },
        { "all setting keys", test_all_keys },
        { "side effects", test_side_effects },
        { "flash write policy", test_flash_policy },
        { "baud rates independent", test_baudrate_independent },
        { "RX error recovery", test_rx_error_recovery },
        { "stuck GPS line", test_stuck_gps_line },
        { "slow link leaves GPS alone", test_slow_link_does_not_hurt_gps },
        { "encoder menus", test_encoder },
        { "CFG", test_cfg },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("%-34s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
