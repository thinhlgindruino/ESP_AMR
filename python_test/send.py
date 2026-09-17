import socket
import struct
import threading
import time

ESP32_IP = "192.168.1.50"
ESP32_PORT = 5001

def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

_seq = 0
_seq_lock = threading.Lock()

def build_frame(msg_type: int, payload: bytes) -> bytes:
    global _seq
    with _seq_lock:
        seq = _seq
        _seq = (_seq + 1) % 256
    body = bytes([seq, msg_type, len(payload)]) + payload
    crc = crc16_modbus(body)
    return bytes([0xAA, 0x55]) + body + bytes([crc & 0xFF, (crc >> 8) & 0xFF])

MODE_OFF, MODE_SOLID, MODE_CHASE, MODE_BLINK, MODE_AUTO = 0, 1, 2, 3, 4
COLOR_OFF, COLOR_RED, COLOR_GREEN, COLOR_BLUE = 0, 1, 2, 3
COLOR_YELLOW, COLOR_WHITE, COLOR_ORANGE, COLOR_PURPLE = 4, 5, 6, 7

current_state = {
    "turn_signal": 0,
    "left_mode": MODE_OFF, "left_color": COLOR_OFF,
    "right_mode": MODE_OFF, "right_color": COLOR_OFF,
    "front_mode": MODE_OFF, "front_color": COLOR_OFF,
    "charge_enable": 0,    
    "discharge_enable": 0, 
}
state_lock = threading.Lock()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_current_state():
    """Dung cho heartbeat -- led_mask LUON = 0 (khong dao gi ca), chi lap lai xi-nhan/neopixel."""
    with state_lock:
        s = dict(current_state)
    payload = bytes([0, s["turn_signal"],          # <-- led_mask=0: khong toggle gi khi resend
                      s["left_mode"], s["left_color"],
                      s["right_mode"], s["right_color"],
                      s["front_mode"], s["front_color"],
                      s["charge_enable"], s["discharge_enable"]])
    frame = build_frame(0x01, payload)
    sock.sendto(frame, (ESP32_IP, ESP32_PORT))
    return frame

def heartbeat_loop():
    """Gui lai CMD_CONTROL moi 5s, du trang thai khong doi -- giu ket noi 'tuoi'
       de ESP32 khong bao nham 'mat ket noi IPC' (timeout 10s ben ESP32)."""
    while True:
        frame = send_current_state()
        print(f"[heartbeat] Da gui lai trang thai hien tai ({frame.hex(' ')})")
        time.sleep(5)

def toggle_led(index: int):
    """Gui lenh TOGGLE 1 lan duy nhat cho den thu index (0-5) -- KHONG luu vao current_state,
       nen heartbeat se khong bao gio lap lai lenh nay."""
    with state_lock:
        s = dict(current_state)
    payload = bytes([1 << index, s["turn_signal"],
                      s["left_mode"], s["left_color"],
                      s["right_mode"], s["right_color"],
                      s["front_mode"], s["front_color"],
                      s["charge_enable"], s["discharge_enable"]]) 
    frame = build_frame(0x01, payload)
    sock.sendto(frame, (ESP32_IP, ESP32_PORT))
    print(f"Da gui lenh TOGGLE den {index + 1}: {frame.hex(' ')}")

def set_state(**kwargs):
    """Van dung cho xi-nhan/NeoPixel -- gia tri 'set' binh thuong, an toan de heartbeat lap lai."""
    with state_lock:
        current_state.update(kwargs)
    frame = send_current_state()
    print(f"Da gui trang thai moi: {current_state}")

def print_menu():
    print("""
=== Menu dieu khien ===
1 - Bat xi-nhan + NeoPixel
2 - Tat het xi-nhan/NeoPixel
t1..t6 - Toggle den 1..6
c1/c0 - Bat/tat SAC tu xa
d1/d0 - Bat/tat XA tu xa
q - Thoat
""")

def main():
    t = threading.Thread(target=heartbeat_loop, daemon=True)
    t.start()
    print(f"Da bat dau heartbeat -- tu dong gui lai trang thai moi 5s toi {ESP32_IP}:{ESP32_PORT}")

    while True:
        print_menu()
        choice = input("Chon: ").strip()

        if choice == "1":
            set_state(led_mask=0b00010101, turn_signal=1,
                       left_mode=MODE_CHASE, left_color=COLOR_RED,
                       right_mode=MODE_CHASE, right_color=COLOR_RED,
                       front_mode=MODE_BLINK, front_color=COLOR_BLUE)
        elif choice == "2":
            set_state(led_mask=0, turn_signal=0,
                       left_mode=MODE_OFF, left_color=COLOR_OFF,
                       right_mode=MODE_OFF, right_color=COLOR_OFF,
                       front_mode=MODE_OFF, front_color=COLOR_OFF)
        elif choice.lower() == "q":
            print("Thoat.")
            break
        elif choice.lower().startswith("t") and choice[1:].isdigit():
            idx = int(choice[1:]) - 1
            if 0 <= idx < 6:
                toggle_led(idx)
            else:
                print("So den khong hop le (1-6).")
        elif choice in ("c1", "c0"):
            set_state(charge_enable=1 if choice == "c1" else 0)
        elif choice in ("d1", "d0"):
            set_state(discharge_enable=1 if choice == "d1" else 0)
        else:
            print("Lua chon khong hop le.")

if __name__ == "__main__":
    main()