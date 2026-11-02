#include "eeprom.h"
#include <string.h>

ee_storage_t ee_storage;

extern EE_HandleTypeDef eeHandle;

bool eeprom_matches_flash(void)
{
    return memcmp((const void*)eeHandle.Address, &ee_storage, sizeof(ee_storage)) == 0;
}
