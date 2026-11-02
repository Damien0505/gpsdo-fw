#ifndef _EEPROM_H_
#define _EEPROM_H_

#include "ee.h"
#include <stdint.h>

typedef struct
{
    uint16_t pwm;
    uint8_t  contrast;
    /* Reading boolean from EEPROM results in unpredictable behavior so we use a char and cast it to a boolean */
    uint8_t  pps_sync_on;
    uint32_t pps_sync_delay;
    uint32_t pps_sync_threshold;
    uint8_t  pps_ppm_auto_sync;
    uint8_t  pwm_auto_save;
    uint8_t  trend_auto_h;
    uint8_t  trend_auto_v;
    uint32_t trend_v_scale;
    uint32_t trend_h_scale;
    uint8_t  boot_menu;
    uint32_t gps_baudrate;
    uint32_t gps_time_offset;
    uint8_t  gps_date_format;
    uint8_t  gps_model;
    uint32_t ppb_lock_threshold;
    uint8_t  ocxo_model;
    uint8_t  correction_algorithm;
    uint32_t correction_factor;
    uint32_t warmup_time_seconds;
    /* New fields must be appended here so that existing units read 0xFF and fall back to defaults */
    uint32_t comm_baudrate;
} ee_storage_t;

extern ee_storage_t ee_storage;

#endif
