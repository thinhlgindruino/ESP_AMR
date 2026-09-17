#include <Adafruit_NeoPixel.h>
#include "stm32f1xx_hal.h"

#define PIN_LEFT PB12
#define PIN_RIGHT PB13
#define PIN_FRONT PB14
#define NUM_LEDS 24
#define NUM_LEDS_LONG 48
#define TURN_SIGNAL_PIN PB4  
#define BTN_LED_PAIRS 6
const uint8_t btnPins[BTN_LED_PAIRS]    = {PA8, PA9, PA10, PA11, PB3, PA15};
const uint8_t btnLedPins[BTN_LED_PAIRS] = {PA4, PA5, PA6, PA7, PB0, PB1};

uint8_t ledMask = 0;  
uint8_t turnSignalState = 0;
UART_HandleTypeDef huart1;
DMA_HandleTypeDef hdma_usart1_rx;
#define DMA_RX_BUF_SIZE 64
uint8_t dmaRxBuf[DMA_RX_BUF_SIZE];
uint16_t dmaRxReadIdx = 0;

Adafruit_NeoPixel strips[3] = {
    Adafruit_NeoPixel(NUM_LEDS, PIN_LEFT, NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NUM_LEDS, PIN_RIGHT, NEO_GRB + NEO_KHZ800),
    Adafruit_NeoPixel(NUM_LEDS_LONG, PIN_FRONT, NEO_GRB + NEO_KHZ800),
};



uint32_t colorTable[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0x0000FF,
    0xFFFF00, 0xFFFFFF, 0xFF8C00, 0xA020F0};
#define MODE_OFF 0
#define MODE_SOLID 1
#define MODE_CHASE 2
#define MODE_BLINK 3
#define MODE_AUTO 4

uint8_t stripMode[3] = {0, 0, 0};
uint8_t stripColor[3] = {0, 0, 0};

enum ParseState { WAIT_START, WAIT_CMD, WAIT_LEN, WAIT_DATA, WAIT_CHECKSUM };
ParseState pstate = WAIT_START;
uint8_t rxCmd, rxLen, rxData[16], rxIdx;

bool ledState[BTN_LED_PAIRS] = {false, false, false, false, false, false};
bool lastBtnRaw[BTN_LED_PAIRS] = {false, false, false, false, false, false};

uint8_t getLedStateMask()
{
  uint8_t mask = 0;
  for (int i = 0; i < BTN_LED_PAIRS; i++) {
    if (ledState[i]) mask |= (1 << i);
  }
  return mask;
}

void handleFrame(uint8_t cmd, uint8_t* d, uint8_t len) {
  if (cmd == 0x01 && len >= 1) {
    uint8_t toggleMask = d[0];   
    for (int i = 0; i < BTN_LED_PAIRS; i++) {
      if ((toggleMask >> i) & 0x01) {
        ledState[i] = !ledState[i];  
        digitalWrite(PC13, !digitalRead(PC13));
      }
    }
  }
  else if (cmd == 0x02 && len >= 3) {
    uint8_t id = d[0];
    if (id < 3) { stripMode[id] = d[1]; stripColor[id] = d[2]; }
  }
  else if (cmd == 0x03 && len >= 1) {
    turnSignalState = d[0];
    digitalWrite(TURN_SIGNAL_PIN, turnSignalState ? HIGH : LOW);
  }
}

void UART1_DMA_Init(void)
{
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_AFIO_REMAP_USART1_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_6;              // PB6 = TX
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_7;              // PB7 = RX
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart1);

    hdma_usart1_rx.Instance = DMA1_Channel5;
    hdma_usart1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart1_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart1_rx.Init.Priority = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&hdma_usart1_rx);
    __HAL_LINKDMA(&huart1, hdmarx, hdma_usart1_rx);

    HAL_UART_Receive_DMA(&huart1, dmaRxBuf, DMA_RX_BUF_SIZE);
}

void UART1_DMA_Process(void)
{
    uint16_t dmaWriteIdx = DMA_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    while (dmaRxReadIdx != dmaWriteIdx) {
        uint8_t b = dmaRxBuf[dmaRxReadIdx];
        switch (pstate) {
            case WAIT_START:  if (b == 0xAA) pstate = WAIT_CMD; break;
            case WAIT_CMD:    rxCmd = b; pstate = WAIT_LEN; break;
            case WAIT_LEN:    rxLen = b; rxIdx = 0; pstate = rxLen ? WAIT_DATA : WAIT_CHECKSUM; break;
            case WAIT_DATA:   rxData[rxIdx++] = b; if (rxIdx >= rxLen) pstate = WAIT_CHECKSUM; break;
            case WAIT_CHECKSUM: {
                uint8_t sum = rxCmd + rxLen;
                for (int i = 0; i < rxLen; i++) sum += rxData[i];
                if (sum == b) handleFrame(rxCmd, rxData, rxLen);
                pstate = WAIT_START;
                break;
            }
        }
        dmaRxReadIdx = (dmaRxReadIdx + 1) % DMA_RX_BUF_SIZE;
    }
}

void updateNeopixels()
{
  static uint32_t last = 0;
  if (millis() - last < 20) return;
  last = millis();
  for (int s = 0; s < 3; s++)
  {
    uint32_t c = colorTable[stripColor[s]];
    uint16_t n = strips[s].numPixels();
    switch (stripMode[s])
    {
    case MODE_OFF:   strips[s].fill(0); break;
    case MODE_SOLID: strips[s].fill(c); break;
    case MODE_CHASE: {
      static uint8_t pos[3] = {0, 0, 0};
      strips[s].fill(0);
      strips[s].setPixelColor(pos[s], c);
      pos[s] = (pos[s] + 1) % n;
      break;
    }
    case MODE_BLINK: {
      bool on = (millis() / 300) % 2;
      strips[s].fill(on ? c : 0);
      break;
    }
    }
    strips[s].show();
  }
}

uint8_t getButtonMask()
{
  uint8_t mask = 0;
  for (int i = 0; i < BTN_LED_PAIRS; i++) {
    if (digitalRead(btnPins[i]) == HIGH) {   
      mask |= (1 << i);
    }
  }
  return mask;
}

void sendStatusReport()
{
    uint8_t payload[9] = {
        getLedStateMask(), turnSignalState,
        stripMode[0], stripColor[0],
        stripMode[1], stripColor[1],
        stripMode[2], stripColor[2],
        getButtonMask(),
    };
    uint8_t frame[4] = {0xAA, 0x10, sizeof(payload), 0};
    uint8_t checksum = 0x10 + sizeof(payload);
    for (uint8_t b : payload) checksum += b;

    HAL_UART_Transmit(&huart1, &frame[0], 3, 10);
    HAL_UART_Transmit(&huart1, payload, sizeof(payload), 10);
    HAL_UART_Transmit(&huart1, &checksum, 1, 10);
}

void updateButtonLeds()
{
  for (int i = 0; i < BTN_LED_PAIRS; i++) {
    bool pressed = (digitalRead(btnPins[i]) == HIGH);
    if (pressed && !lastBtnRaw[i]) {   
      ledState[i] = !ledState[i];
    }
    lastBtnRaw[i] = pressed;
    digitalWrite(btnLedPins[i], ledState[i] ? LOW : HIGH);
  }
}

void setup()
{
  UART1_DMA_Init();

  for (int i = 0; i < 3; i++) { strips[i].begin(); strips[i].show(); }

  pinMode(TURN_SIGNAL_PIN, OUTPUT);
  digitalWrite(TURN_SIGNAL_PIN, LOW);
  pinMode(PC13, OUTPUT); digitalWrite(PC13, HIGH);
  for (int i = 0; i < BTN_LED_PAIRS; i++) {
    pinMode(btnPins[i], INPUT_PULLDOWN);
    pinMode(btnLedPins[i], OUTPUT);
    digitalWrite(btnLedPins[i], LOW);
  }
}

void loop()
{
  UART1_DMA_Process();
  updateNeopixels();
  updateButtonLeds();

  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 200) {
    lastReport = millis();
    sendStatusReport();
  }
}