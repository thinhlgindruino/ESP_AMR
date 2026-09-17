#include "bms.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "BMS";

// Block 0x1200 = du lieu THOI GIAN THUC, khac voi 0x1000 la block cau hinh/nguong bao ve
#define REALTIME_START_REG   0x1200
#define REALTIME_REG_COUNT   100   // du de phu het cac field can dung (den offset ~0xC2)

typedef struct {
    float cell_voltages[BMS_ACTIVE_CELLS];
    float cell_avg_v;
    float cell_vdiff_max;
    int   max_cell_idx;
    int   min_cell_idx;

    float temp_mosfet;
    float total_voltage;      // V
    float power_w;            // W
    float current;            // A (duong = sac, am = xa, theo dinh nghia goc cua thanh ghi)
    float temp_bat1;
    float temp_bat2;

    uint8_t soc_percent;      // %
    float remaining_ah;
    float full_charge_ah;
    uint32_t cycle_count;
    float cycle_cap_ah;

    bool charge_status;
    bool discharge_status;

    bool valid;
} JkBmsRealtime;

static JkBmsRealtime s_bms = {0};

static uint16_t calculate_crc16(const uint8_t *buffer, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= buffer[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// ==== Cac ham doc theo BYTE OFFSET truc tiep ====
static inline uint16_t get_u16(const uint8_t *buf, int byte_offset) {
    return ((uint16_t)buf[byte_offset] << 8) | (uint16_t)buf[byte_offset + 1];
}
static inline int16_t get_i16(const uint8_t *buf, int byte_offset) {
    return (int16_t)get_u16(buf, byte_offset);
}
static inline uint32_t get_u32(const uint8_t *buf, int byte_offset) {
    return ((uint32_t)buf[byte_offset] << 24) | ((uint32_t)buf[byte_offset + 1] << 16) |
           ((uint32_t)buf[byte_offset + 2] << 8) | (uint32_t)buf[byte_offset + 3];
}
static inline int32_t get_i32(const uint8_t *buf, int byte_offset) {
    return (int32_t)get_u32(buf, byte_offset);
}

void bms_init(void) {
    const uart_config_t uart_config = {
        .baud_rate = BMS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_is_driver_installed(BMS_UART_PORT)) uart_driver_delete(BMS_UART_PORT);
    ESP_ERROR_CHECK(uart_driver_install(BMS_UART_PORT, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BMS_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(BMS_UART_PORT, BMS_TX_PIN, BMS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "Khoi dong BMS UART (TX=%d, RX=%d, baud=%d)", BMS_TX_PIN, BMS_RX_PIN, BMS_BAUD_RATE);
}

bool bms_update(void) {
    uint8_t frame[8] = {
        0x01, 0x03,
        (uint8_t)(REALTIME_START_REG >> 8), (uint8_t)(REALTIME_START_REG & 0xFF),
        (uint8_t)(REALTIME_REG_COUNT >> 8), (uint8_t)(REALTIME_REG_COUNT & 0xFF),
        0x00, 0x00
    };
    uint16_t crc = calculate_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uart_flush_input(BMS_UART_PORT);
    uart_write_bytes(BMS_UART_PORT, (const char *)frame, 8);
    uart_wait_tx_done(BMS_UART_PORT, pdMS_TO_TICKS(100));

    uint8_t rx_buf[300];
    int expected_len = 5 + (REALTIME_REG_COUNT * 2);
    int len = uart_read_bytes(BMS_UART_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(400));

    if (len < expected_len || rx_buf[0] != 0x01 || rx_buf[1] != 0x03) {
        ESP_LOGW(TAG, "Frame khong hop le hoac chua du byte (len=%d)", len);
        s_bms.valid = false;
        return false;
    }

    uint16_t recv_crc = rx_buf[len - 2] | (rx_buf[len - 1] << 8);
    uint16_t calc_crc = calculate_crc16(rx_buf, len - 2);
    if (recv_crc != calc_crc) {
        ESP_LOGW(TAG, "CRC sai: nhan=0x%04X, tinh=0x%04X", recv_crc, calc_crc);
        s_bms.valid = false;
        return false;
    }

    uint8_t *d = &rx_buf[3]; // payload thuc su, byte offset khop truc tiep voi bang thanh ghi 0x1200

    for (int i = 0; i < BMS_ACTIVE_CELLS; i++) {
        s_bms.cell_voltages[i] = get_u16(d, i * 2) / 1000.0f;
    }

    s_bms.cell_avg_v     = get_u16(d, 0x44) / 1000.0f;
    s_bms.cell_vdiff_max = get_u16(d, 0x46) / 1000.0f;
    s_bms.max_cell_idx   = d[0x48];
    s_bms.min_cell_idx   = d[0x49];

    s_bms.temp_mosfet    = get_i16(d, 0x8A) / 10.0f;

    s_bms.total_voltage  = get_u32(d, 0x90) / 1000.0f;   // mV -> V
    s_bms.power_w        = get_u32(d, 0x94) / 1000.0f;   // mW -> W
    s_bms.current        = get_i32(d, 0x98) / 1000.0f;   // mA -> A
    s_bms.temp_bat1      = get_i16(d, 0x9C) / 10.0f;
    s_bms.temp_bat2      = get_i16(d, 0x9E) / 10.0f;

    // 0xA6: byte cao = balancing state, byte thap = SOC %
    s_bms.soc_percent    = d[0xA7];

    s_bms.remaining_ah   = get_i32(d, 0xA8) / 1000.0f;   // mAh -> Ah
    s_bms.full_charge_ah = get_u32(d, 0xAC) / 1000.0f;
    s_bms.cycle_count    = get_u32(d, 0xB0);
    s_bms.cycle_cap_ah   = get_u32(d, 0xB4) / 1000.0f;

    s_bms.charge_status    = (d[0xC0] == 1);
    s_bms.discharge_status = (d[0xC1] == 1);

    s_bms.valid = true;
    return true;
}

float bms_getTotalVoltage(void) { return s_bms.total_voltage; }
float bms_getCurrent(void)      { return s_bms.current; }
float bms_getPowerW(void)       { return s_bms.power_w; }

uint8_t bms_getSOC(void)         { return s_bms.soc_percent; }
float bms_getRemainingAh(void)   { return s_bms.remaining_ah; }
float bms_getFullChargeAh(void)  { return s_bms.full_charge_ah; }
uint32_t bms_getCycleCount(void) { return s_bms.cycle_count; }
float bms_getCycleCapAh(void)    { return s_bms.cycle_cap_ah; }

float bms_getTempMosfet(void) { return s_bms.temp_mosfet; }
float bms_getTempBat1(void)   { return s_bms.temp_bat1; }
float bms_getTempBat2(void)   { return s_bms.temp_bat2; }

bool bms_isChargeOn(void)    { return s_bms.charge_status; }
bool bms_isDischargeOn(void) { return s_bms.discharge_status; }

float bms_getCellVoltage(int index) {
    if (index < 0 || index >= BMS_ACTIVE_CELLS) return 0.0f;
    return s_bms.cell_voltages[index];
}
float bms_getCellAvgV(void)     { return s_bms.cell_avg_v; }
float bms_getCellVDiffMax(void) { return s_bms.cell_vdiff_max; }
int   bms_getMaxCellIndex(void) { return s_bms.max_cell_idx; }
int   bms_getMinCellIndex(void) { return s_bms.min_cell_idx; }

bool bms_isDataValid(void) { return s_bms.valid; }

// ====== Ghi thanh ghi (function 0x10 - Write Multiple Registers) ======
// DA XAC NHAN theo tai lieu cong dong (esphome-jk-bms esp32-jk-pb-modbus-example.yaml,
// khop 10/10 truong voi block cau hinh 0x1000 doc duoc tu chinh thiet bi nay):
//   0x1070 = Charging switch (bat/tat SAC)
//   0x1074 = Discharging switch (bat/tat XA)   <- code cu gan nham thanh CHARGE
//   0x1078 = Balancing switch (bat/tat CAN BANG) <- code cu gan nham thanh DISCHARGE
#define REG_CHARGE_SWITCH     0x1070
#define REG_DISCHARGE_SWITCH  0x1074
#define REG_BALANCE_SWITCH    0x1078

static bool write_u32_register(uint16_t address, uint32_t value) {
    uint8_t frame[13];
    frame[0] = 0x01;                 // slave id
    frame[1] = 0x10;                 // function: write multiple registers
    frame[2] = (uint8_t)(address >> 8);
    frame[3] = (uint8_t)(address & 0xFF);
    frame[4] = 0x00;                 // so luong thanh ghi (hi) = 2
    frame[5] = 0x02;                 // so luong thanh ghi (lo)
    frame[6] = 0x04;                 // so byte du lieu = 4 (UINT32)
    frame[7] = (uint8_t)(value >> 24);
    frame[8] = (uint8_t)(value >> 16);
    frame[9] = (uint8_t)(value >> 8);
    frame[10] = (uint8_t)(value & 0xFF);

    uint16_t crc = calculate_crc16(frame, 11);
    frame[11] = (uint8_t)(crc & 0xFF);
    frame[12] = (uint8_t)(crc >> 8);

    uart_flush_input(BMS_UART_PORT);
    uart_write_bytes(BMS_UART_PORT, (const char *)frame, sizeof(frame));
    uart_wait_tx_done(BMS_UART_PORT, pdMS_TO_TICKS(100));

    uint8_t rx[16];
    int len = uart_read_bytes(BMS_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(400));

    if (len < 8 || rx[0] != 0x01 || rx[1] != 0x10) {
        ESP_LOGW(TAG, "Ghi lenh that bai hoac khong co phan hoi (len=%d)", len);
        return false;
    }

    uint16_t recv_crc = rx[len - 2] | (rx[len - 1] << 8);
    uint16_t calc_crc = calculate_crc16(rx, len - 2);
    if (recv_crc != calc_crc) {
        ESP_LOGW(TAG, "CRC phan hoi ghi sai");
        return false;
    }

    // Phan hoi cua FC 0x10 la echo lai dia chi + so luong da ghi -> doi chieu cho chac
    uint16_t recv_addr = (rx[2] << 8) | rx[3];
    uint16_t recv_qty  = (rx[4] << 8) | rx[5];
    if (recv_addr != address || recv_qty != 2) {
        ESP_LOGW(TAG, "Phan hoi ghi khong khop yeu cau");
        return false;
    }

    return true;
}

bool bms_setChargeSwitch(bool on) {
    bool ok = write_u32_register(REG_CHARGE_SWITCH, on ? 1 : 0);
    ESP_LOGI(TAG, "Ghi Charge switch = %d -> %s", on, ok ? "OK" : "THAT BAI");
    return ok;
}

bool bms_setDischargeSwitch(bool on) {
    bool ok = write_u32_register(REG_DISCHARGE_SWITCH, on ? 1 : 0);
    ESP_LOGI(TAG, "Ghi Discharge switch = %d -> %s", on, ok ? "OK" : "THAT BAI");
    return ok;
}

bool bms_setPower(bool on) {
    bool ok1 = bms_setChargeSwitch(on);
    vTaskDelay(pdMS_TO_TICKS(100)); // cho BMS xu ly xong lenh truoc, tranh gui lien tiep qua nhanh
    bool ok2 = bms_setDischargeSwitch(on);
    return ok1 && ok2;
}