#include <SPI.h>
#include <Ethernet.h>

// Đổi MAC khác mẫu mặc định để tránh trùng/conflict trên mạng
byte mac[] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01 };

#define W5500_CS 5

// Cấu hình IP tĩnh theo đúng dải mạng của TOTOLINK
IPAddress ip(192, 168, 1, 177);        // chưa dùng, khác với 192.168.1.65 của laptop
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

const char* host = "google.com";
EthernetClient client;

void setup() {
  Serial.begin(115200);
  while (!Serial);

  SPI.begin();
  Ethernet.init(W5500_CS);

  Serial.println("Bat dau Ethernet voi IP tinh...");

  Ethernet.begin(mac, ip, dns, gateway, subnet);

  // Kiểm tra hardware/link ngay cả khi dùng IP tĩnh (Ethernet.begin ở dạng này không trả về 0/1)
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println(">>> Khong tim thay chip W5500 qua SPI");
    while (true);
  }

  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.println(">>> LINK OFF - PHY khong thay tin hieu");
  } else if (Ethernet.linkStatus() == LinkON) {
    Serial.println(">>> LINK ON");
  }

  Serial.print("My IP address: ");
  Serial.println(Ethernet.localIP());
  Serial.print("Gateway: ");
  Serial.println(Ethernet.gatewayIP());
}

void loop() {
  Serial.print("Dang ket noi toi ");
  Serial.println(host);

  if (client.connect(host, 80)) {
    Serial.println(">>> Ket noi TCP thanh cong, dang gui HTTP GET...");

    client.println("GET / HTTP/1.1");
    client.print("Host: ");
    client.println(host);
    client.println("Connection: close");
    client.println();

    unsigned long startTime = millis();
    while (client.connected() && millis() - startTime < 5000) {
      while (client.available()) {
        char c = client.read();
        Serial.write(c);
      }
    }

    client.stop();
    Serial.println("\n>>> Da dong ket noi.");
  } else {
    Serial.println(">>> Ket noi TCP that bai (khong tiep can duoc server)");
  }

  delay(5000);
}