# ESP_AMR — ESP32 IoT Gateway cho AMR

Firmware ESP32 (ESP-IDF) đóng vai trò **gateway trung tâm** cho AMR: giao tiếp với IPC qua Ethernet (W5500), đọc BMS pin (JK-BMS), và điều khiển IO (LED/nút nhấn/NeoPixel/xi-nhan/sạc-xả) thông qua STM32F103 mở rộng.

```
        UDP (v2.0, CRC16)         UART (Modbus RTU)
IPC  ─────────────────────  ESP32  ─────────────────────  BMS (JK-BMS)
                               │
                               │ UART + DMA (frame checksum)
                               ▼
                          STM32F103 (Bluepill)
                               │
                    6 nút + 6 LED, 3 dải NeoPixel, xi-nhan
```

## 1. Cấu trúc project

- `ethernet_w5050_udp/` — Firmware ESP32 (ESP-IDF v6.0.1)
  - `main.c` — khởi tạo Ethernet, BMS task, console
  - `udp_api.c/h` — giao thức UDP v2.0 với IPC (đóng/mở frame, CRC16, trạng thái)
  - `uart.c/h` — cầu nối UART tới STM32F103
  - `bms.c/h` — đọc/ghi JK-BMS qua Modbus RTU
  - `crc16.c/h` — CRC16 Modbus dùng chung
  - `bms_cmd.h` — struct lệnh sạc/xả dùng chung giữa `main.c` và `udp_api.c`
- `stm32_io/` (file `.ino`, nạp qua Arduino IDE + STM32duino core) — firmware STM32F103

## 2. Bảng chân GPIO — ESP32 (board ESP32-2432S024 / CYD)

⚠️ Board này có nhiều chân **đã bị chiếm bởi phần cứng onboard** — không dùng GPIO26 (audio amp FM8002A), GPIO4/16/17 (RGB LED, dù 16/17 đang tận dụng cho UART BMS), GPIO5/18/19/23 (khe SD — hiện đang tận dụng cho W5500 vì không dùng SD card).

| Chức năng | Chân | Ghi chú |
|---|---|---|
| W5500 MOSI | GPIO23 | Dùng chung bus SPI với khe SD (không cắm SD) |
| W5500 MISO | GPIO19 | |
| W5500 SCK | GPIO18 | |
| W5500 CS | GPIO5 | |
| W5500 RST | GPIO4 | Điều khiển chủ động qua `phy_config.reset_gpio_num` |
| UART → STM32 TX | GPIO22 | |
| UART → STM32 RX | GPIO21 | |
| UART → BMS TX | GPIO17 | |
| UART → BMS RX | GPIO16 | |

**⚠️ Tham số bắt buộc, dễ quên nhất:** `eth_phy_config_t.phy_addr = 1;` — thiếu dòng này khiến W5500 luôn đọc version/PHYCFGR = `0x00`, tưởng nhầm là chip hỏng. Đây là nguyên nhân của gần như toàn bộ quá trình debug Ethernet ban đầu.

## 3. Bảng chân GPIO — STM32F103 (Bluepill)

| Chức năng | Chân |
|---|---|
| UART1 (↔ESP32), remap | PB6 (TX), PB7 (RX) |
| NeoPixel Trái / Phải / Trước | PB12 / PB13 / PB14 |
| Xi-nhan | PB4 |
| Nút 1–6 | PA8, PA9, PA10, PA11, PB3, PA15 |
| LED 1–6 (tương ứng nút) | PA4, PA5, PA6, PA7, PB0, PB1 |

⚠️ **Không dùng PA13/PA14** cho GPIO — đây là SWDIO/SWCLK, cấu hình chúng thành output sẽ tự khóa cổng nạp ST-Link (phải giải cứu bằng "Connect Under Reset").

## 4. Giao thức UDP v2.0 (ESP32 ↔ IPC)

Frame nhị phân, CRC16 Modbus (poly `0xA001`, init `0xFFFF`):

```
[0xAA][0x55][SEQ][TYPE][LEN][PAYLOAD...][CRC_L][CRC_H]
```

| TYPE | Tên | Chiều | Payload |
|---|---|---|---|
| `0x01` | `CMD_CONTROL` | IPC→ESP32 | 10 byte: led_mask(toggle), turn_signal, np_left(mode+color), np_right(mode+color), np_front(mode+color), charge_enable, discharge_enable |
| `0x81` | `MSG_HELLO` | ESP32→IPC | 1 byte: protocol_version |
| `0x82` | `MSG_STATUS` | ESP32→IPC | 15 byte: voltage_x100(2), current_x100(2), soc, button_mask, turn_signal, np_left/right/front_mode, led_mask, warning_code, error_code, charge_state, discharge_state |
| `0x84` | `MSG_SHUTDOWN_REQUEST` | ESP32→IPC | 4 byte: reason_code, voltage_x100(2), soc |

**Chu kỳ gửi:** `MSG_HELLO` lúc khởi động + mỗi 30s. `MSG_STATUS` mỗi 2s. `MSG_SHUTDOWN_REQUEST` gửi ngay lập tức khi SOC≤5% (không đợi chu kỳ).

**`led_mask` trong `CMD_CONTROL` là bitmask TOGGLE** (đảo trạng thái từng bit, không phải set tuyệt đối) — vì vậy IPC **không được lặp lại giá trị này trong cơ chế heartbeat/resend**, chỉ gửi đúng 1 lần cho mỗi hành động bật/tắt. Các field còn lại (turn_signal, NeoPixel, charge/discharge) là kiểu "set", an toàn để lặp lại.

### Mã hiệu ứng NeoPixel (Mode)
`0`=OFF, `1`=SOLID, `2`=CHASE, `3`=BLINK, `4`=AUTO

### Bảng màu (Color palette)
`0`=Đen, `1`=Đỏ, `2`=Xanh lá, `3`=Xanh dương, `4`=Vàng, `5`=Trắng, `6`=Cam, `7`=Tím

### Mã Warning (`payload[11]`, bitmask)
| Bit | Tên | Ý nghĩa |
|---|---|---|
| `0x01` | `WARN_LOW_BATTERY` | SOC ≤ 20% (>5%) |

### Mã Error (`payload[12]`, bitmask — gộp chung lỗi BMS và lỗi kết nối)
| Bit | Tên | Ý nghĩa |
|---|---|---|
| `0x01` | `ERR_BMS_READ_FAIL` | Lần đọc BMS gần nhất thất bại |
| `0x02` | `ERR_BATTERY_CRITICAL` | SOC ≤ 5%, đã gửi shutdown request |
| `0x04` | `ERR_IPC_TIMEOUT` | Không nhận gói tin nào từ IPC > 10 giây |

## 5. Giao thức UART (ESP32 ↔ STM32)

Frame: `[0xAA][CMD][LEN][DATA...][CHECKSUM]` (checksum = tổng cộng dồn các byte CMD+LEN+DATA, mod 256).

| CMD | Chiều | Ý nghĩa |
|---|---|---|
| `0x01` | ESP32→STM32 | Toggle LED (1 byte: bitmask 6-bit các đèn cần đảo) |
| `0x02` | ESP32→STM32 | NeoPixel (3 byte: strip_id, mode, color) |
| `0x03` | ESP32→STM32 | Xi-nhan (1 byte: 0/1) |
| `0x10` | STM32→ESP32 | Báo cáo trạng thái thật (9 byte: led_state_mask, turn_signal, 3×(mode+color), button_mask) — gửi mỗi 200ms |

STM32 nhận UART qua **DMA circular buffer** (không dùng ngắt `onReceive`) — bắt buộc, vì `strip.show()` của NeoPixel tắt ngắt toàn cục ~1-4ms mỗi lần cập nhật, UART ngắt thường sẽ mất byte trong lúc đó.

## 6. Build & nạp

**ESP32 (ESP-IDF v6.0.1):**
```bash
cd ethernet_w5050_udp
idf.py build
idf.py -p <COMx> flash monitor
```

**STM32F103 (Arduino IDE):**
- Board: `Generic STM32F1 series` → `BluePill F103C8`
- Upload method: `STLink` (khuyến nghị) hoặc `Serial` (cần BOOT0=HIGH lúc nạp, về LOW + reset sau khi xong)
- Thư viện cần cài: `Adafruit NeoPixel STM32` (bản fork cho STM32, không dùng bản gốc AVR)

## 7. Lỗi đã gặp & cách sửa (tránh lặp lại)

| Triệu chứng | Nguyên nhân | Cách sửa |
|---|---|---|
| W5500 version luôn đọc `0x00` | Thiếu `phy_config.phy_addr = 1` | Thêm dòng này trong `eth_init()` |
| `implausible frame length 65533`, RX corruption | CS đặt vào GPIO26 (dính audio amp onboard) hoặc SCK đặt vào GPIO0 (dính boot-strap) | Dùng đúng bộ chân 23/19/18/5 (mượn bus SD không dùng) |
| `esp-idf` báo "W5500 version mismatched" dù chip thật hoạt động tốt | Bug driver `esp_eth`, nhiều chip W5500 chính hãng trả version≠0x04 | Có thể bypass check trong `managed_components/espressif__w5500` nếu cần, nhưng ưu tiên sửa `phy_addr` trước |
| Ping "Destination host unreachable" | Nhiều card mạng cùng dải IP trên PC (WiFi+LAN+VMware) gây rối route | Tắt bớt card mạng không dùng, chỉ giữ đúng 1 card |
| Nối thẳng ESP32↔PC không lên Link, qua switch/hub thì lên | PHY giá rẻ (OEM) tương thích Auto-MDIX kém với 1 số card USB-LAN | Luôn qua switch/hub trung gian, không nối thẳng point-to-point |
| STM32 mất lệnh UART ngẫu nhiên khi NeoPixel đang chạy hiệu ứng | `strip.show()` tắt ngắt toàn cục, UART ngắt thường mất byte | Chuyển UART RX sang DMA circular buffer |
| Nạp STM32 báo "Unable to get core ID" | Lỡ cấu hình PA13/PA14 thành GPIO thường, tự khóa SWD | Nạp lại bằng "Connect Under Reset" trong STM32CubeProgrammer, rồi đổi chân tránh PA13/PA14 |
| ESP32 liên tục reset khi cấp nguồn ngoài cho W5500 | Chân SPI bị driven vào chip chưa có nguồn (backfeed qua diode bảo vệ), hoặc nguồn chung quá tải khi thêm nhiều LED/NeoPixel | Cấp nguồn đồng thời cho mọi module dùng chung bus tín hiệu; tách nguồn riêng nếu cần |
| `arduino-esp32` build lỗi `-Werror` ở các thư viện không dùng tới (LittleFS...) | `arduino-esp32` 3.3.11 chưa tương thích đầy đủ ESP-IDF v6.0.1 | Đã bỏ hướng `arduino-esp32`, quay về driver `esp_eth` gốc sau khi tìm ra `phy_addr` |
| Linker báo "undefined reference" dù hàm đã khai báo trong `.h` | Thiếu định nghĩa thật trong file `.c`, hoặc biến dùng `extern` giữa 2 file nhưng bên kia khai `static` | Kiểm tra bằng `Select-String -Pattern <ten_ham>` trên cả `.c` lẫn `.h`; bỏ `static` nếu cần chia sẻ biến |
