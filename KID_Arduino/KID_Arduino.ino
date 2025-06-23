#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/pgmspace.h>
#include "moon_image_96x64.h"

// Display
#define TFT_CS   10
#define TFT_RST  8
#define TFT_RS   9

// Joystick
#define VRx A0
#define VRy A1
#define SW  2

// Piezo buzzer
#define BUZZER_PIN 3

// Battery monitoring
#define BATTERY_PIN A2
#define VOLTAGE_MIN 3000  // 3.0V in mV
#define VOLTAGE_MAX 4200  // 4.2V in mV
#define VOLTAGE_DIVIDER_RATIO 2

// LEDs and peripherals
#define LED_WHITE 5
#define LED_RED   4
#define LED_YELLOW A3
#define LED_GREEN 7
#define POT_PIN A6
#define TRIG_PIN A4
#define ECHO_PIN A5

// Time until sleep on inactivity (in milliseconds)
#define INACTIVITY_TIMEOUT 120000UL

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_RS, TFT_RST);

enum Mode { SETUP, COUNTDOWN, PAUSE, OVERDUE };
Mode mode = SETUP;

int hours = 0, minutes = 0, seconds = 0;
int selected = 0;
bool negative = false;
unsigned long lastUpdate = 0;
bool blink = true;
unsigned long lastBlink = 0;

unsigned long lastBatteryCheck = 0;
const unsigned long batteryUpdateInterval = 60000;

unsigned long lastActivity = 0;
unsigned long lastSensorRead = 0;
const unsigned long sensorInterval = 100;

// For button debounce and long press handling
int lastButtonState = HIGH;
unsigned long buttonDownMillis = 0;
const unsigned long debounceDelay = 50;
const unsigned long longPressDuration = 1000;

void wakeUp() {}

void drawSleepImage() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRGBBitmap(32, 32, moonImage, 96, 64);
}

void goToSleep() {
  drawSleepImage();
  delay(300);
  tft.writeCommand(ST77XX_DISPOFF);
  tft.writeCommand(ST77XX_SLPIN);

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  attachInterrupt(digitalPinToInterrupt(SW), wakeUp, FALLING);
  sleep_mode();
  sleep_disable();
  detachInterrupt(digitalPinToInterrupt(SW));

  tft.writeCommand(ST77XX_SLPOUT);
  delay(120);
  tft.writeCommand(ST77XX_DISPON);
  tft.fillScreen(ST77XX_BLACK);
  drawStaticTime();
  updateBatteryIndicator();
}

void setup() {
  pinMode(SW, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(LED_WHITE, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3);

  drawStaticTime();
  updateBatteryIndicator();
  lastActivity = millis();
}

void loop() {
  static int lastSelected = -1;
  static int lastHours = -1, lastMinutes = -1, lastSeconds = -1;

  handlePotAndUltrasound();

  if (millis() - lastBatteryCheck >= batteryUpdateInterval) {
    lastBatteryCheck = millis();
    updateBatteryIndicator();
  }

  handleButton();

  if (mode == SETUP) {
    handleJoystick();

    if (selected != lastSelected || hours != lastHours || minutes != lastMinutes || seconds != lastSeconds) {
      drawStaticTime();
      lastSelected = selected;
      lastHours = hours;
      lastMinutes = minutes;
      lastSeconds = seconds;
    }

    if (millis() - lastBlink >= 500) {
      blink = !blink;
      lastBlink = millis();
      drawBlinkingPart();
    }

    if (millis() - lastActivity > INACTIVITY_TIMEOUT) {
      goToSleep();
      lastActivity = millis();
    }
  }
  else if (mode == COUNTDOWN || mode == OVERDUE) {
    if (millis() - lastUpdate >= 1000) {
      lastUpdate = millis();
      countdown();
      drawStaticTime();
    }
  }
}

void handleButton() {
  int buttonState = digitalRead(SW);
  unsigned long now = millis();

  // Button is pressed (LOW)
  if (lastButtonState == HIGH && buttonState == LOW) {
    buttonDownMillis = now;
  }

  // Button is released (HIGH)
  if (lastButtonState == LOW && buttonState == HIGH) {
    unsigned long pressDuration = now - buttonDownMillis;

    // Short press (except PAUSE/SETUP)
    if (pressDuration >= debounceDelay && pressDuration < longPressDuration) {
      switch (mode) {
        case SETUP:
          mode = COUNTDOWN;
          negative = false;
          lastUpdate = now;
          tft.fillScreen(ST77XX_BLACK);
          drawStaticTime();
          updateBatteryIndicator();
          break;
        case COUNTDOWN:
          mode = PAUSE;
          break;
        case PAUSE:
          mode = COUNTDOWN;
          lastUpdate = now;
          break;
        case OVERDUE:
          hours = minutes = seconds = 0;
          negative = false;
          mode = SETUP;
          tft.fillScreen(ST77XX_BLACK);
          drawStaticTime();
          updateBatteryIndicator();
          break;
      }
      lastActivity = now;
    }
    // Long press resets when paused
    else if (pressDuration >= longPressDuration) {
      if (mode == PAUSE) {
        hours = minutes = seconds = 0;
        negative = false;
        mode = SETUP;
        tft.fillScreen(ST77XX_BLACK);
        drawStaticTime();
        updateBatteryIndicator();
        lastActivity = now;
      }
    }
    buttonDownMillis = 0;
  }

  lastButtonState = buttonState;
}

void handleJoystick() {
  int x = analogRead(VRx);
  int y = analogRead(VRy);
  int sw = digitalRead(SW);
  static unsigned long lastMove = 0;

  if (millis() - lastMove > 200) {
    if (x < 400 && selected > 0) {
      selected--;
      lastMove = millis();
      lastActivity = millis();
    } else if (x > 600 && selected < 2) {
      selected++;
      lastMove = millis();
      lastActivity = millis();
    } else if (y < 400) {
      incrementSelected();
      lastMove = millis();
      lastActivity = millis();
    } else if (y > 600) {
      decrementSelected();
      lastMove = millis();
      lastActivity = millis();
    } else if (sw == LOW) {
      mode = COUNTDOWN;
      negative = false;
      lastUpdate = millis();
      tft.fillScreen(ST77XX_BLACK);
      drawStaticTime();
      updateBatteryIndicator();
      lastMove = millis();
      lastActivity = millis();
    }
  }
}

void handlePotAndUltrasound() {
  int potValue = analogRead(POT_PIN);
  int pwmValue = potValue >> 2;
  analogWrite(LED_WHITE, pwmValue);

  if (millis() - lastSensorRead >= sensorInterval) {
    lastSensorRead = millis();

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    long duration = pulseIn(ECHO_PIN, HIGH, 15000);
    int distance = duration * 343L / 20000;

    digitalWrite(LED_RED, distance < 10);
    digitalWrite(LED_YELLOW, distance >= 10 && distance < 20);
    digitalWrite(LED_GREEN, distance >= 20);
  }
}

void incrementSelected() {
  if (selected == 0 && hours < 23) hours++;
  if (selected == 1 && minutes < 59) minutes++;
  if (selected == 2 && seconds < 59) seconds++;
}

void decrementSelected() {
  if (selected == 0 && hours > 0) hours--;
  if (selected == 1 && minutes > 0) minutes--;
  if (selected == 2 && seconds > 0) seconds--;
}

void countdown() {
  if (hours == 0 && minutes == 0 && seconds == 0 && !negative && mode != PAUSE && mode != SETUP) {
    tone(BUZZER_PIN, 1000, 300);
    negative = true;
    mode = OVERDUE;
    return;
  }

  if (!negative) {
    if (seconds > 0) {
      seconds--;
    } else {
      seconds = 59;
      if (minutes > 0) {
        minutes--;
      } else {
        minutes = 59;
        if (hours > 0) hours--;
      }
    }
  } else {
    seconds++;
    if (seconds >= 60) {
      seconds = 0;
      minutes++;
      if (minutes >= 60) {
        minutes = 0;
        hours++;
      }
    }
  }
}

void drawStaticTime() {
  char buf[4];
  int xH = 20;
  int xM = xH + 48;
  int xS = xM + 48;
  int y = 50;

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);

  tft.fillRect(0, y, 160, 30, ST77XX_BLACK);

  if (negative) {
    tft.setCursor(0, y);
    tft.print("-");
  }

  sprintf(buf, "%02d", hours);
  tft.setCursor(xH, y);
  tft.print(buf);

  tft.setCursor(xH + 33, y);
  tft.print(":");

  sprintf(buf, "%02d", minutes);
  tft.setCursor(xM, y);
  tft.print(buf);

  tft.setCursor(xM + 33, y);
  tft.print(":");

  sprintf(buf, "%02d", seconds);
  tft.setCursor(xS, y);
  tft.print(buf);
}

void drawBlinkingPart() {
  int x = (selected == 0) ? 20 : (selected == 1 ? 68 : 116);
  int val = (selected == 0) ? hours : (selected == 1 ? minutes : seconds);
  char buf[4];
  int y = 50;

  sprintf(buf, "%02d", val);
  tft.setTextColor(blink ? ST77XX_WHITE : ST77XX_BLACK);
  tft.fillRect(x, y, 33, 24, ST77XX_BLACK);
  tft.setCursor(x, y);
  tft.print(buf);
}

void updateBatteryIndicator() {
  int analogValue = analogRead(BATTERY_PIN);
  long voltage_mv = analogValue * 5000L / 1023 * VOLTAGE_DIVIDER_RATIO;
  uint8_t percent = map(voltage_mv, VOLTAGE_MIN, VOLTAGE_MAX, 0, 100);
  percent = constrain(percent, 0, 100);
  int bars = (percent + 10) / 20;

  const int barWidth = 5;
  const int barHeight = 10;
  const int spacing = 2;
  const int totalBars = 5;

  const int totalWidth = totalBars * (barWidth + spacing) - spacing;
  const int startX = 160 - totalWidth - 5;
  const int startY = 5;

  tft.fillRect(startX - 2, startY - 2, totalWidth + 4, barHeight + 4, ST77XX_BLACK);

  for (int i = 0; i < totalBars; i++) {
    int x = startX + i * (barWidth + spacing);
    uint16_t color = (i < bars) ? ST77XX_GREEN : ST77XX_BLACK;
    tft.fillRect(x, startY, barWidth, barHeight, color);
    tft.drawRect(x, startY, barWidth, barHeight, ST77XX_WHITE);
  }
}
