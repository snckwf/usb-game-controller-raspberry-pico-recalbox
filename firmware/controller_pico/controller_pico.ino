/*
  controller_pico.ino - 90s-style USB controller, 1 joystick + 4 buttons
  Board: Raspberry Pi Pico RP2040 (TZT clone with USB-C). It shows up as a standard USB gamepad, no drivers needed:
  Windows/Linux/Mac PCs, Recalbox, RetroPie, Batocera, Android.

  Arduino IDE:
    - Boards Manager: "Raspberry Pi Pico/RP2040/RP2350" by Earle F. Philhower
    - Board: "Raspberry Pi Pico"  (also valid for TZT clones with 2, 4 or 16 MB of flash)
    - Tools > USB Stack: "Pico SDK"   (the default)
    No libraries to install: the USB gamepad (Joystick.h) is already part of the board package.
    First upload: hold BOOTSEL while plugging in the cable, then Upload.

  Wiring (buttons to GND, internal pull-ups enabled):
    KY-023 joystick   VRx -> GP26 (ADC0)   VRy -> GP27 (ADC1)   SW -> GP6
                      module +5V -> 3V3(OUT) (pin 36)   GND -> GND
    Button A (bottom)  GP2 --[button]-- GND
    Button B (right)   GP3 --[button]-- GND
    Button X (left)    GP4 --[button]-- GND
    Button Y (top)     GP5 --[button]-- GND
  WARNING: power the joystick from 3V3(OUT), NOT from VBUS/VSYS: the Pico inputs are 3.3 V.
  (4-pin 12x12 buttons: use two diagonally opposite pins, e.g. 1 and 4)

  FUNCTION key = joystick click (L3) held down:
    L3 + A = L1      L3 + B = R1      L3 + X = SELECT (Recalbox hotkey)      L3 + Y = START
    L3 pressed and released on its own = normal L3 click
    L3 held on its own for 5 seconds = recalibrates the joystick centre (the LED blinks)

  HID buttons: 1 A, 2 B, 3 X, 4 Y, 5 L1, 6 R1, 7 SELECT, 8 START, 9 L3
  Axes: X/Y = joystick.  Digital d-pad (hat): copy of the stick, useful in retro games (DPAD_FROM_STICK).

  Debug: Serial Monitor (any baud rate), send "d" to see the raw joystick values.
*/

#include <Joystick.h>

// ======================= CONFIGURATION =======================
#define PIN_X    26
#define PIN_Y    27
#define PIN_LED  LED_BUILTIN

const uint8_t BUTTON_PINS[5] = {2, 3, 4, 5, 6};   // A, B, X, Y, L3
enum { B_A = 0, B_B, B_X, B_Y, B_L3 };

#define DPAD_FROM_STICK  1       // 1 = also send the digital d-pad derived from the stick
#define DPAD_THRESHOLD   45      // % of travel beyond which the d-pad triggers
#define L3_TAP_MS        80      // length of the L3 click sent on release
#define L3_TAP_MAX_MS    600     // beyond this time releasing L3 does not generate the click
#define RECALIBRATE_MS   5000    // L3 held on its own for this time = recalibration
#define DEAD_ZONE        120     // dead zone around the centre (out of 2048, 12-bit ADC)
#define DEBOUNCE_MS      10
#define PERIOD_MS        4       // 250 updates per second

// If an axis is reversed or swapped, change these values (check with joy.cpl or "d" on the Serial Monitor).
#define SWAP_XY    0
#define INVERT_X   0
#define INVERT_Y   0
// ==============================================================

int centre[2] = {2048, 2048};
const uint8_t AXIS_PINS[2] = {PIN_X, PIN_Y};

uint8_t buttonState = 0, prevReading = 0;
unsigned long tChange = 0, tSend = 0, tDebug = 0;
bool l3Down = false, comboUsed = false;
unsigned long tL3 = 0, l3TapEnd = 0;
bool recalibrated = false;
bool debug = false;

int readAxis(uint8_t pin) {
  long s = 0;
  for (uint8_t i = 0; i < 8; i++) s += analogRead(pin);
  return (int)(s / 8);
}

void calibrate() {
  for (uint8_t a = 0; a < 2; a++) {
    long s = 0;
    for (uint8_t i = 0; i < 64; i++) {
      s += analogRead(AXIS_PINS[a]);
      delay(1);
    }
    centre[a] = (int)(s / 64);
  }
}

// raw 0..4095 -> -511..511 with a dead zone and separate scaling on each side of the centre
int scaleAxis(int raw, int c, bool invert) {
  int v = raw - c;
  long r;
  if (abs(v) <= DEAD_ZONE) {
    r = 0;
  } else if (v > 0) {
    r = (long)(v - DEAD_ZONE) * 511L / max(1, 4095 - c - DEAD_ZONE);
  } else {
    r = (long)(v + DEAD_ZONE) * 511L / max(1, c - DEAD_ZONE);
  }
  r = constrain(r, -511L, 511L);
  return (int)(invert ? -r : r);
}

uint8_t readButtons() {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 5; i++)
    if (digitalRead(BUTTON_PINS[i]) == LOW) b |= (1 << i);
  return b;
}

void blink(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_LED, HIGH);
    delay(80);
    digitalWrite(PIN_LED, LOW);
    delay(80);
  }
}

// d-pad angle (-1 = centred), 0 = up, 90 = right, 180 = down, 270 = left
int dpadAngle(int x, int y) {
  const int s = 511 * DPAD_THRESHOLD / 100;
  bool up = y < -s, down = y > s, left = x < -s, right = x > s;
  if (up && right) return 45;
  if (down && right) return 135;
  if (down && left) return 225;
  if (up && left) return 315;
  if (up) return 0;
  if (right) return 90;
  if (down) return 180;
  if (left) return 270;
  return -1;
}

void setup() {
  for (uint8_t i = 0; i < 5; i++) pinMode(BUTTON_PINS[i], INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  analogReadResolution(12);
  Serial.begin(115200);

  Joystick.begin();
  Joystick.useManualSend(true);

  delay(300);          // let the potentiometer settle
  calibrate();         // do not touch the joystick while plugging in the cable
  blink(2);
}

void loop() {
  unsigned long now = millis();

  // ---- button debounce
  uint8_t r = readButtons();
  if (r != prevReading) {
    prevReading = r;
    tChange = now;
  }
  if (now - tChange >= DEBOUNCE_MS) buttonState = r;

  bool a  = buttonState & (1 << B_A);
  bool b  = buttonState & (1 << B_B);
  bool x  = buttonState & (1 << B_X);
  bool y  = buttonState & (1 << B_Y);
  bool l3 = buttonState & (1 << B_L3);

  // ---- L3 function key
  if (l3) {
    if (!l3Down) {
      l3Down = true;
      comboUsed = false;
      recalibrated = false;
      tL3 = now;
    }
    if (a || b || x || y) comboUsed = true;
    // recalibration: L3 held on its own for 5 seconds
    if (!comboUsed && !recalibrated && now - tL3 >= RECALIBRATE_MS) {
      blink(4);
      delay(400);
      calibrate();
      blink(4);
      recalibrated = true;
      comboUsed = true;   // no L3 click on release
    }
  } else if (l3Down) {
    l3Down = false;
    if (!comboUsed && now - tL3 <= L3_TAP_MAX_MS) l3TapEnd = now + L3_TAP_MS;
  }

  if (now - tSend < PERIOD_MS) return;
  tSend = now;

  // ---- buttons (numbered from 1)
  bool hid[10] = {false};
  if (l3) {
    hid[5] = a;      // L1
    hid[6] = b;      // R1
    hid[7] = x;      // SELECT
    hid[8] = y;      // START
  } else {
    hid[1] = a;
    hid[2] = b;
    hid[3] = x;
    hid[4] = y;
  }
  hid[9] = (long)(l3TapEnd - now) > 0;   // L3
  for (uint8_t i = 1; i <= 9; i++) Joystick.button(i, hid[i]);

  // ---- axes
  int g[2];
  for (uint8_t i = 0; i < 2; i++) g[i] = readAxis(AXIS_PINS[i]);
#if SWAP_XY
  int sx = scaleAxis(g[1], centre[1], INVERT_X), sy = scaleAxis(g[0], centre[0], INVERT_Y);
#else
  int sx = scaleAxis(g[0], centre[0], INVERT_X), sy = scaleAxis(g[1], centre[1], INVERT_Y);
#endif
  Joystick.X(sx + 512);          // 0..1023, centre 512
  Joystick.Y(sy + 512);
  Joystick.Z(512);
  Joystick.Zrotate(512);
  Joystick.sliderLeft(0);
  Joystick.sliderRight(0);
#if DPAD_FROM_STICK
  Joystick.hat(dpadAngle(sx, sy));
#else
  Joystick.hat(-1);
#endif
  Joystick.send_now();

  // ---- LED: on while a button is pressed
  digitalWrite(PIN_LED, buttonState ? HIGH : LOW);

  // ---- debug on the Serial Monitor
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') debug = !debug;
  }
  if (debug && now - tDebug >= 100) {
    tDebug = now;
    Serial.printf("raw X %4d Y %4d | centre %4d %4d | X %4d Y %4d | buttons ", g[0], g[1], centre[0], centre[1], sx, sy);
    Serial.println(buttonState, BIN);
  }
}
