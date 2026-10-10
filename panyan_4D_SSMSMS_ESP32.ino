/*
 * ESP32 攀岩计时器
 *
 * 硬件：
 *   74HC595:  enable=25, latch=27, clock=14, data=26
 *   START  = 32
 *   STOP   = 33
 *   RESET  = 16
 *   FOOT   = 17
 *   BUZZER = 22
 *   LED    = 23
 *
 * 流程：
 *   上电 → 显示 00:00，LED 灭
 *   按 RESET → 任意状态都回到初始状态（00:00，LED 灭，蜂鸣器停）
 *   准备状态下按 START → 显示 -- -- -- 3，每秒递减 + 蜂鸣
 *   倒计时期间松开 FOOT = 抢跑 → 蜂鸣器响 3 秒，显示 FF:FF，计时器不工作
 *   倒计时到 0 → 长鸣 300ms → 自动计时
 *   按 STOP → 计时停止，LED 开始闪烁（胜利）
 *   按 RESET → 清零回 00:00，LED 灭，蜂鸣器停
 */

#include <Arduino.h>
#include "esp_timer.h"

// ==================== 引脚 ====================
const int enablePin = 25;
const int latchPin  = 27;
const int clockPin  = 14;
const int dataPin   = 26;

#define START_BTN_PIN  32
#define STOP_BTN_PIN   33
#define RESET_BTN_PIN  18
#define FOOT_SW_PIN    17
#define BUZZER_PIN     22
#define LED_PIN        23

// ==================== 状态 ====================
// 0=待机/准备  1=倒计时  2=计时中  3=已停止(胜利)  4=抢跑犯规
byte state = 0;

int      se1 = 0, ms1 = 0;
int      se2 = 0, ms2 = 0;
int      n_start = 3;
int64_t  lastMicros = 0;

unsigned long lastCountdownMs = 0;
unsigned long buzzerOffMs = 0;
bool buzzerOn = false;

// 抢跑检测：倒计时开始时脚踏是否已踩下
bool footWasPressed = false;

// LED 闪烁
bool          ledOn = false;
unsigned long lastLedToggleMs = 0;
#define LED_BLINK_INTERVAL 250

unsigned long lastPrintMs = 0;

// ==================== 数码管字模 ====================
const byte numbers1dec[] = {
  0B01111111,
  0B00011100,
  0B10111011,
  0B10111110,
  0B11011100,
  0B11101110,
  0B11101111,
  0B00111100,
  0B11111111,
  0B11111110,
  0B00001000,  // 10 = 空白
  0B11101001   // 11 = F
};
const byte numbers2dec[] = {
  0B11110111,
  0B11000001,
  0B10111011,
  0B11101011,
  0B11001101,
  0B11101110,
  0B11111110,
  0B11000011,
  0B11111111,
  0B11101111,
  0B10000000,  // 10 = 空白
  0B10011110   // 11 = F
};

// ==================== 函数声明 ====================
void shiftOut(byte data);
void showDisplay();
void updateTimer();
void updateCountdown();
void updateBuzzer();
void updateLed();
void scanButtons();
void beep(unsigned long durationMs);
void falseStart();
void doReset();

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Climbing Timer ===");

  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(enablePin, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  pinMode(START_BTN_PIN, INPUT_PULLUP);
  pinMode(STOP_BTN_PIN,  INPUT_PULLUP);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  pinMode(FOOT_SW_PIN,   INPUT_PULLUP);

  digitalWrite(enablePin, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  lastMicros = esp_timer_get_time();
}

// ==================== loop ====================
void loop() {
  scanButtons();
  updateCountdown();
  updateTimer();
  updateBuzzer();
  updateLed();
  showDisplay();

  if (millis() - lastPrintMs >= 1000) {
    lastPrintMs = millis();
    Serial.printf("state=%d T=%lu | %02d:%02d | FOOT=%d LED=%d\n",
                  state, millis(), se1, ms1,
                  digitalRead(FOOT_SW_PIN), digitalRead(LED_PIN));
  }
}

// ==================== 按键 + 脚踏 ====================
void scanButtons() {
  static bool lastStart = HIGH, lastStop = HIGH, lastReset = HIGH;
  static bool lastFoot  = HIGH;

  bool curStart = digitalRead(START_BTN_PIN);
  bool curStop  = digitalRead(STOP_BTN_PIN);
  bool curReset = digitalRead(RESET_BTN_PIN);
  bool curFoot  = digitalRead(FOOT_SW_PIN);

  // ---------- RESET：最高优先级，任何状态都回初始 ----------
  if (lastReset == HIGH && curReset == LOW) {
    delay(20);
    if (digitalRead(RESET_BTN_PIN) == LOW) {
      doReset();
    }
    lastStart = curStart;
    lastStop  = curStop;
    lastReset = curReset;
    lastFoot  = curFoot;
    return;   // 本次扫描不再处理其他按键
  }

  // ---------- START：准备状态下按 START 才启动倒计时 ----------
  if (lastStart == HIGH && curStart == LOW) {
    delay(20);
    if (digitalRead(START_BTN_PIN) == LOW && state == 0) {
      state = 1;
      n_start = 3;
      se1 = ms1 = se2 = ms2 = 0;
      lastCountdownMs = millis();
      beep(400);
      footWasPressed = (digitalRead(FOOT_SW_PIN) == LOW);
      Serial.printf("[START] countdown, foot=%d\n", footWasPressed);
    }
  }

  // ---------- 抢跑检测 ----------
  if (state == 1) {
    if (footWasPressed && curFoot == HIGH) {
      falseStart();
    }
  }

  // ---------- STOP：计时中停止 → 胜利 ----------
  if (lastStop == HIGH && curStop == LOW) {
    delay(20);
    if (digitalRead(STOP_BTN_PIN) == LOW && state == 2) {
      state = 3;
      Serial.printf("[STOP] %02d:%02d\n", se1, ms1);
    }
  }

  lastStart = curStart;
  lastStop  = curStop;
  lastReset = curReset;
  lastFoot  = curFoot;
}

// ==================== 复位 ====================
void doReset() {
  state = 0;
  se1 = ms1 = se2 = ms2 = 0;
  n_start = 3;

  buzzerOn = false;
  digitalWrite(BUZZER_PIN, LOW);

  ledOn = false;
  digitalWrite(LED_PIN, LOW);

  footWasPressed = false;
  lastCountdownMs = 0;
  buzzerOffMs = 0;
  lastLedToggleMs = 0;
  lastMicros = esp_timer_get_time();

  Serial.println("[RESET] back to initial");
}

// ==================== 抢跑犯规 ====================
void falseStart() {
  state = 4;
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerOn = true;
  buzzerOffMs = millis() + 3000;
  Serial.println("[FALSE START] Buzzer 3s, display FF:FF, timer disabled");
}

// ==================== 倒计时 ====================
void updateCountdown() {
  if (state != 1) return;

  if (millis() - lastCountdownMs >= 1000) {
    lastCountdownMs += 1000;
    n_start--;

    if (n_start > 0) {
      beep(400);
      Serial.printf("[COUNT] %d\n", n_start);
    } else {
      beep(300);
      Serial.println("[GO]");
      state = 2;
      se1 = ms1 = se2 = ms2 = 0;
      lastMicros = esp_timer_get_time();
    }
  }
}

// ==================== 计时 ====================
void updateTimer() {
  if (state != 2) {
    lastMicros = esp_timer_get_time();
    return;
  }

  int64_t now = esp_timer_get_time();
  int64_t elapsed_us = now - lastMicros;

  if (elapsed_us >= 10000) {
    int elapsed_ms = (int)(elapsed_us / 10000);
    lastMicros += (int64_t)elapsed_ms * 10000;

    ms1 += elapsed_ms;
    while (ms1 >= 100) { ms1 -= 100; se1++; }
    if (se1 >= 99 && ms1 >= 99) { se1 = 99; ms1 = 99; state = 3; }

    ms2 = ms1;
    se2 = se1;
  }
}

// ==================== 蜂鸣器 ====================
void beep(unsigned long durationMs) {
  if (state == 4) return;
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerOn = true;
  buzzerOffMs = millis() + durationMs;
}

void updateBuzzer() {
  if (buzzerOn && millis() >= buzzerOffMs) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerOn = false;
  }
}

// ==================== 胜利 LED 闪烁 ====================
void updateLed() {
  if (state == 3) {
    if (millis() - lastLedToggleMs >= LED_BLINK_INTERVAL) {
      lastLedToggleMs = millis();
      ledOn = !ledOn;
      digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    }
  } else {
    if (ledOn) {
      ledOn = false;
      digitalWrite(LED_PIN, LOW);
    }
  }
}

// ==================== 显示 ====================
void showDisplay() {
  digitalWrite(enablePin, HIGH);

  if (state == 1) {
    // 倒计时：前 6 位空白 + 末位数字 3/2/1
    shiftOut(numbers2dec[10]);
    shiftOut(numbers1dec[10]);
    shiftOut(numbers2dec[10]);
    shiftOut(numbers1dec[10]);
    shiftOut(numbers2dec[10]);
    shiftOut(numbers1dec[10]);
    shiftOut(numbers2dec[10]);
    shiftOut(numbers1dec[n_start % 10]);
  } else if (state == 4) {
   
    shiftOut(numbers2dec[11]);
    shiftOut(numbers1dec[11]);
    shiftOut(numbers2dec[11]);
    shiftOut(numbers1dec[11]);
    shiftOut(numbers2dec[11]);
    shiftOut(numbers1dec[11]);
    shiftOut(numbers2dec[11]);
    shiftOut(numbers1dec[11]);
  } else {
    // 待机 / 计时 / 停止：SS MS + 镜像 SS MS
    shiftOut(numbers2dec[se1 / 10]);
    shiftOut(numbers1dec[se1 % 10]);
    shiftOut(numbers2dec[ms1 / 10]);
    shiftOut(numbers1dec[ms1 % 10]);
    shiftOut(numbers2dec[se2 / 10]);
    shiftOut(numbers1dec[se2 % 10]);
    shiftOut(numbers2dec[ms2 / 10]);
    shiftOut(numbers1dec[ms2 % 10]);
  }

  digitalWrite(enablePin, LOW);
  delay(2);
}

// ==================== 74HC595 ====================
void shiftOut(byte data) {
  digitalWrite(latchPin, LOW);
  for (int k = 0; k <= 7; k++) {
    digitalWrite(clockPin, LOW);
    if (data & (1 << k)) {
      digitalWrite(dataPin, HIGH);
    } else {
      digitalWrite(dataPin, LOW);
    }
    digitalWrite(clockPin, HIGH);
  }
  digitalWrite(clockPin, LOW);
  digitalWrite(latchPin, HIGH);
}