import socket
import struct

LISTEN_IP = "0.0.0.0"
LISTEN_PORT = 5000

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
def decode_status(payload: bytes):
    voltage_x100, current_x100, soc, button, turn, np_l, np_r, np_f, led_mask, \
        warning, error, charge_state, discharge_state = \
        struct.unpack(">HhBBBBBBBBBBB", payload)   
    print(f"    Dien ap    : {voltage_x100/100:.2f} V")
    print(f"    Dong dien  : {current_x100/100:.2f} A")
    print(f"    SOC        : {soc}%")
    print(f"    Nut nhan   : {button}")
    print(f"    Xi-nhan    : {turn}")
    print(f"    NeoPixel   : trai={np_l} phai={np_r} truoc={np_f}")
    print(f"    LED mask   : {bin(led_mask)}"),
    print(f"    Sac/Xa     : Sac={'ON' if charge_state else 'OFF'}  Xa={'ON' if discharge_state else 'OFF'}")
    if warning:
        print(f"    !! WARNING : 0b{warning:08b}" + (" [LOW_BATTERY]" if warning & 0x01 else ""))
    if error:
        print(f"    !! ERROR   : 0b{error:08b}" +
              (" [BMS_READ_FAIL]" if error & 0x01 else "") +
              (" [BATTERY_CRITICAL]" if error & 0x02 else ""))
    if button:
        pressed_list = [str(i+1) for i in range(6) if button & (1 << i)]
        print(f"    Nut dang nhan: {', '.join(pressed_list)}")


def decode_shutdown(payload: bytes):
    reason, voltage_x100, soc = struct.unpack(">BHB", payload)
    print(f"    !!! SHUTDOWN REQUEST !!!  reason=0x{reason:02X} volt={voltage_x100/100:.2f}V soc={soc}%")

def decode_hello(payload: bytes):
    version = payload[0]
    print(f"    Protocol version = {version}")

TYPE_NAMES = {
    0x81: ("MSG_HELLO", decode_hello),
    0x82: ("MSG_STATUS", decode_status),
    0x84: ("MSG_SHUTDOWN_REQUEST", decode_shutdown),
}



def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((LISTEN_IP, LISTEN_PORT))
    print(f"Dang lang nghe UDP tai port {LISTEN_PORT} ... (Ctrl+C de dung)")

    while True:
        data, addr = sock.recvfrom(1024)
        print(f"\n[{addr[0]}:{addr[1]}] Nhan {len(data)} byte: {data.hex(' ')}")

        if len(data) < 7 or data[0] != 0xAA or data[1] != 0x55:
            print("  -> Sai sync byte, bo qua")
            continue

        seq = data[2]
        msg_type = data[3]
        length = data[4]
        payload = data[5:5+length]
        crc_received = data[5+length] | (data[6+length] << 8)
        crc_calc = crc16_modbus(data[2:5+length])

        name, decoder = TYPE_NAMES.get(msg_type, (f"UNKNOWN(0x{msg_type:02X})", None))
        print(f"  SEQ={seq} TYPE={name} LEN={length} CRC={'OK' if crc_received==crc_calc else 'SAI!'}")

        if decoder:
            decoder(payload)

if __name__ == "__main__":
    main()