# Chuyển sang socket cứng W5500 bằng ioLibrary_Driver (không dùng esp_eth/lwIP)

## Cấu trúc thư mục trong gói này

```
wiznet_component/
├── main/
│   ├── CMakeLists.txt
│   ├── main.c
│   ├── ethernet.c        <-- thay cho ethernet.c cũ (bản esp_eth)
│   └── ethernet.h
└── wiznet_driver/         <-- component mới, chứa lớp adapter + ioLibrary_Driver
    ├── CMakeLists.txt
    ├── wiznet_port.c       <-- lớp SPI/GPIO adapter, dùng đúng trình tự reset đã test OK
    ├── include/
    │   └── wiznet_port.h
    └── ioLibrary_Driver/
        └── Ethernet/
            ├── socket.c, socket.h           (API socket cứng)
            ├── wizchip_conf.c, wizchip_conf.h (lõi cấu hình chip)
            └── W5500/w5500.c, w5500.h        (thanh ghi riêng W5500)
```

## Cách tích hợp vào project của bạn

1. Copy cả thư mục `wiznet_driver/` vào `components/` của project (ngang hàng với `main/`).
2. Copy `main/ethernet.c`, `main/ethernet.h`, `main/main.c` đè lên bản cũ (bản dùng `esp_eth`).
3. Xoá các dòng include/gọi liên quan `esp_eth.h`, `esp_eth_mac_w5500.h`, `esp_eth_phy_w5500.h`,
   `esp_netif.h`, `lwip/...` trong `main/CMakeLists.txt` cũ (không cần nữa vì không dùng lwIP/MAC driver kiểu Ethernet nữa).
4. Trong `idf_component.yml` (nếu có) của `main`, gỡ dependency `espressif/w5500` — không cần nữa.
5. `idf.py fullclean && idf.py build`.

## Vì sao cách này né được lỗi "reset timeout"

- `esp_eth` + `esp_eth_mac_new_w5500` coi W5500 là một NIC thuần, dùng lwIP chạy TCP/IP
  hoàn toàn bằng phần mềm trên ESP32. Bug "reset timeout" nằm trong chính bước khởi tạo
  MAC của driver này (nhiều dự án khác trên GitHub cũng gặp, kể cả khi phần cứng đã xác
  nhận chạy tốt).
- `ioLibrary_Driver` (giống hệt cách Arduino `Ethernet.h` hoạt động) dùng thẳng 8 socket
  TCP/UDP có sẵn trong phần cứng W5500 — ESP32 chỉ gửi lệnh mức cao (OPEN/LISTEN/SEND/RECV)
  qua SPI. Không có bước "reset MAC" kiểu Ethernet driver nên tránh được hẳn lỗi trên.
- Lớp `wiznet_port.c` dùng lại đúng chân GPIO, trình tự reset, và cấu hình SPI mode 0 mà
  bạn đã kiểm chứng đọc đúng `VERSIONR = 0x04` trước đó.

## Những điểm cần chỉnh theo board thực tế

- `wiznet_port.h`: đổi `W5500_SPI_HOST`, các chân GPIO, và `W5500_SPI_CLOCK_HZ` nếu cần
  (đang để 8MHz, có thể thử nâng lên tới ~20-33MHz tuỳ board/độ dài dây).
- `ethernet.c`: đổi MAC/IP/gateway/subnet cho khớp mạng của bạn.
- `tcp_server_task`: hiện xử lý 1 client tại 1 thời điểm trên socket 0 (đúng với nhu cầu
  server đơn giản). Nếu cần nhiều client đồng thời, có thể mở thêm nhiều socket (0-7) chạy
  song song, mỗi socket một trạng thái riêng.

## Lưu ý về `wizchip_delay_ms` / hàm tuỳ chọn khác

`ioLibrary_Driver` bản gốc có thể cần thêm hàm delay tuỳ nền tảng ở một số ví dụ
(`Application/loopback`), nhưng phần lõi `socket.c` + `wizchip_conf.c` + `w5500.c` trong
gói này không phụ thuộc hàm đó — không cần thêm gì khác.
