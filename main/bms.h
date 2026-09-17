#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// ====== Cau hinh chan UART, doi lai theo dây bạn đấu ======
#define BMS_UART_PORT   UART_NUM_2
#define BMS_TX_PIN      GPIO_NUM_17
#define BMS_RX_PIN      GPIO_NUM_16
#define BMS_BAUD_RATE   115200

#define BMS_ACTIVE_CELLS  10 // So luong cell cua he pin (10S)

// Khoi tao UART cho BMS. Goi 1 lan trong app_main() truoc khi dung cac ham khac.
void bms_init(void);
bool bms_update(void);
float bms_getTotalVoltage(void);      // V
float bms_getCurrent(void);           // A (duong = sac, am = xa)
float bms_getPowerW(void);            // W

uint8_t bms_getSOC(void);             // %
float bms_getRemainingAh(void);       // Ah
float bms_getFullChargeAh(void);      // Ah
uint32_t bms_getCycleCount(void);
float bms_getCycleCapAh(void);        // Ah

float bms_getTempMosfet(void);        // C
float bms_getTempBat1(void);          // C
float bms_getTempBat2(void);          // C

bool bms_isChargeOn(void);
bool bms_isDischargeOn(void);

float bms_getCellVoltage(int index);  // index 0..BMS_ACTIVE_CELLS-1, tra ve V
float bms_getCellAvgV(void);          // V
float bms_getCellVDiffMax(void);      // V
int   bms_getMaxCellIndex(void);      // so thu tu cell co ap cao nhat (1-based, theo BMS bao ve)
int   bms_getMinCellIndex(void);      // so thu tu cell co ap thap nhat (1-based)

// true neu lan bms_update() gan nhat thanh cong (frame hop le, CRC dung)
bool bms_isDataValid(void);

// ====== Dieu khien BAT/TAT (ghi thanh ghi qua Modbus function 0x10) ======
// Tra ve true neu BMS xac nhan da ghi thanh cong.
bool bms_setChargeSwitch(bool on);     // bat/tat SAC
bool bms_setDischargeSwitch(bool on);  // bat/tat XA

// Tien loi: bat/tat CA HAI cung luc = "bat/tat pin" theo nghia thong thuong.
// on=true  -> bat ca sac va xa
// on=false -> tat ca sac va xa (pin ngung hoat dong hoan toan voi tai/nguon ngoai)
bool bms_setPower(bool on);

#ifdef __cplusplus
}
#endif