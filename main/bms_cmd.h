#ifndef BMS_CMD_H
#define BMS_CMD_H

#include <stdbool.h>

typedef struct {
    bool charge_on;
    bool discharge_on;
} bms_cmd_t;

#endif