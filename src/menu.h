#ifndef _MENU_H_
#define _MENU_H_

#include <stdbool.h>
#include "int.h"

// Char code for sat icons
#define SAT_ICON_1_CODE         '-'//0x7F
#define SAT_ICON_2_CODE         '='//'-'
#define SAT_ICON_3_CODE         0xC6//0x7E
// Char code for no sat icon
#define NO_SAT_ICON_CODE        0x04
#define NO_SAT_STD_ICON_CODE    '!'
// Char codes for trend view
#define TREND_LEFT_CODE         0x7F
#define TREND_RIGHT_CODE        0x7E

// Min and max values for time offset
#define MIN_TIME_OFFSET     -14
#define MAX_TIME_OFFSET     14

// Default PPB lock threshold (*100)
#define DEFAULT_PPB_LOCK_THRESHOLD  50
#define MAX_PPB_LOCK_THRESHOLD      1000

extern bool trend_auto_h;
extern bool trend_auto_v;
extern uint32_t trend_v_scale; 
extern uint32_t trend_h_scale; 

extern uint32_t gps_baudrate;
extern uint32_t comm_baudrate;

extern uint32_t ppb_lock_threshold; 

void menu_set_current_menu(uint8_t current_menu);
void menu_set_gps_baudrate(uint32_t baudrate);
void menu_set_comm_baudrate(uint32_t baudrate);
bool settings_is_valid_baudrate(uint32_t baudrate);
void menu_set_correction_algorithm(correction_algo_type algo);
bool rotary_get_click();
void menu_run();
void lcd_create_chars();
void init_trend_values();

#endif
