// Rig v2 pin checker for the ESP32-C6 DevKitC (main board).
// Pin map: docs/controller.md.
//
// Serial at 115200 on either USB port (UART or USB). Send a letter + Enter:
//   l  live view: inlets (volts), tach edge counts, MCP state   (default at boot)
//   s  short + pull-up test: every rig pin against every other (U1 modules and 12 V unplugged)
//   e  LED walk: lights each of the 8 lane LEDs in turn (MCP)
//   m  motor test: short pulses on both motors, counts motor tach edges (12 V on)
//   b  assist button test: one short press each on Feed, Adjust, Unload (asks first)
//   i  I2C scan
//   ?  help

#include <Wire.h>
#include "driver/gpio.h"
#include "HWCDC.h"

#if !HWCDC_SERIAL_IS_DEFINED
HWCDC HWCDCSerial;  // USB port (when CDC-on-boot is off, the core doesn't make one)
#endif

// ---------- Pin map (must match docs/controller.md) ----------
struct Feeder {
  const char *name;
  int inletR, inletL;     // ADC, 10k pull-up
  int motorTach;          // 10k pull-up
  int leftA, rightA;      // wheel tach A on C6, 10k pull-up
  int mcpLeftB, mcpRightB;  // wheel tach B on MCP port B bit
  int in1, in2;           // DRV8833 inputs
  int mcpLedBase;         // MCP port A bit of the first LED (U1 pins 6, 7, 12, 13)
};

const Feeder F[2] = {
  {"Feeder 1", 2, 3, 10, 18, 20, 0, 1, 22, 23, 0},
  {"Feeder 2", 0, 1, 11, 6, 7, 2, 3, 4, 5, 4},
};

// ---------- Lanes = the factory inlet stickers L1 L2 R3 R4 (bench-mapped 2026-11-08) ----------
// Each lane's motor direction, wheel tachs and inlet sensor, checked by feeding and pulling strands.
// Note: U1 pin 10 (Feeder.inletR) is the sensor for the U1 *left* lane, not the right (pin sweep was wrong).
struct Lane {
  const char *sticker;
  int feeder;        // index into F
  bool u1Right;      // true = IN1/IN3 high
  const char *pin;   // DRV input driven high
  int inlet;         // ADC pin of this lane's inlet sensor
  int tachA;         // C6 pin of this lane's wheel tach A
  int mcpB;          // MCP port B bit of this lane's wheel tach B
};
const Lane LANES[4] = {
  {"L1", 0, false, "IN2", 2, 18, 0},
  {"L2", 0, true, "IN1", 3, 20, 1},
  {"R3", 1, true, "IN3", 1, 7, 3},
  {"R4", 1, false, "IN4", 0, 6, 2},
};

const int PIN_SDA = 19, PIN_SCL = 21, PIN_SPARE = 15;
const int MCP_FEED_BIT = 4, MCP_UNLOAD_BIT = 5, MCP_ADJUST_BIT = 6;  // port B
const int LED_PINS[4] = {6, 7, 12, 13};  // U1 connector pins for GPA k (+4 for unit 2)
const char *LED_NAMES[4] = {"left white (U1 pin 6)", "left red (U1 pin 7)", "right red (U1 pin 12)",
                            "right white (U1 pin 13)"};

// ---------- MCP23017 (raw registers, BANK=0) ----------
const uint8_t REG_IODIRA = 0x00, REG_IODIRB = 0x01, REG_GPPUA = 0x0C, REG_GPPUB = 0x0D, REG_GPIOA = 0x12,
              REG_GPIOB = 0x13, REG_OLATA = 0x14, REG_OLATB = 0x15;
uint8_t mcpAddr = 0;  // 0 = not found

// ---------- Serial on both USB ports ----------
class Tee : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial0.write(c);
    HWCDCSerial.write(c);
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    Serial0.write(b, n);
    HWCDCSerial.write(b, n);
    return n;
  }
} out;

int readKey() {
  if (Serial0.available()) return Serial0.read();
  if (HWCDCSerial.available()) return HWCDCSerial.read();
  return -1;
}

int waitKey() {  // next non-whitespace key
  for (;;) {
    int c = readKey();
    if (c > ' ') return c;
    delay(5);
  }
}

void drainInput() {
  delay(20);
  while (readKey() >= 0) {}
}

bool confirm(const char *question) {
  out.printf("%s (y/n) ", question);
  int c = waitKey();
  drainInput();
  out.println((char)c);
  return c == 'y' || c == 'Y';
}

// ---------- I2C / MCP helpers ----------
bool mcpWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(mcpAddr);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

int mcpRead(uint8_t reg) {
  Wire.beginTransmission(mcpAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(mcpAddr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

void i2cStart() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.setTimeOut(20);
}

void mcpFind() {
  mcpAddr = 0;
  for (uint8_t a = 0x20; a <= 0x27; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      mcpAddr = a;
      return;
    }
  }
}

// Safe default state: LEDs off, buttons released, tach B inputs.
void mcpSafeInit() {
  if (!mcpAddr) return;
  mcpWrite(REG_OLATA, 0x00);
  mcpWrite(REG_OLATB, 0x00);  // set latches low BEFORE making B4-B6 outputs: no button glitch
  mcpWrite(REG_IODIRA, 0x00);  // A = 8 LED outputs
  mcpWrite(REG_IODIRB, 0x8F);  // B0-3 tach inputs, B4/B5/B6 button outputs, B7 spare input
  mcpWrite(REG_GPPUA, 0x00);
  mcpWrite(REG_GPPUB, 0x80);  // pull-up on the spare only; tachs have external 10k
}

void i2cScan() {
  out.println("\nI2C scan (SDA GPIO19, SCL GPIO21):");
  // Line check: bus released, weak internal pull-DOWN on. The MCP board's pull-ups should still win.
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLDOWN);
  pinMode(PIN_SCL, INPUT_PULLDOWN);
  delay(5);
  int sda = digitalRead(PIN_SDA), scl = digitalRead(PIN_SCL);
  out.printf("  SDA (GPIO19) %s, SCL (GPIO21) %s\n", sda ? "high: pull-up seen" : "LOW: no pull-up / not connected",
             scl ? "high: pull-up seen" : "LOW: no pull-up / not connected");
  int n = 0;
  for (uint32_t hz : {400000u, 100000u}) {
    Wire.begin(PIN_SDA, PIN_SCL, hz);
    Wire.setTimeOut(20);
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        out.printf("  %lu kHz: found 0x%02X%s\n", (unsigned long)(hz / 1000), a,
                   (a >= 0x20 && a <= 0x27) ? "  <- MCP23017" : "");
        n++;
      }
    }
    Wire.end();
  }
  i2cStart();
  if (!n) {
    out.println("  nothing found at 400 or 100 kHz.");
    if (!sda || !scl) out.println("  A line with no pull-up usually means that wire isn't reaching the MCP board, or the board has no power.");
    else out.println("  Lines are pulled up but nothing answers: check SDA/SCL swapped, MCP VCC to 3V3, GND, RESET pin high.");
  } else {
    mcpFind();
    mcpSafeInit();
  }
}

// ---------- Live view ----------
volatile uint32_t edges[32];
void IRAM_ATTR onEdge(void *arg) { edges[(uint32_t)arg]++; }

const int TACH_PINS[] = {10, 18, 20, 11, 6, 7};
uint32_t bEdges[4];
int lastB = -1;

void liveSetup() {
  for (int f = 0; f < 2; f++) {
    pinMode(F[f].in1, OUTPUT);
    digitalWrite(F[f].in1, LOW);
    pinMode(F[f].in2, OUTPUT);
    digitalWrite(F[f].in2, LOW);
    pinMode(F[f].inletR, INPUT);
    pinMode(F[f].inletL, INPUT);
  }
  pinMode(PIN_SPARE, INPUT_PULLUP);
  for (int p : TACH_PINS) {
    pinMode(p, INPUT);  // external 10k pull-up
    edges[p] = 0;
    attachInterruptArg(digitalPinToInterrupt(p), onEdge, (void *)(uint32_t)p, CHANGE);
  }
  memset(bEdges, 0, sizeof bEdges);
  lastB = -1;
}

void liveTeardown() {
  for (int p : TACH_PINS) detachInterrupt(digitalPinToInterrupt(p));
}

void pollB() {
  if (!mcpAddr) return;
  int b = mcpRead(REG_GPIOB);
  if (b < 0) return;
  if (lastB >= 0) {
    int ch = (b ^ lastB) & 0x0F;
    for (int i = 0; i < 4; i++)
      if (ch & (1 << i)) bEdges[i]++;
  }
  lastB = b;
}

const char *inletState(int mv) {
  if (mv < 400) return "LOADED";
  if (mv < 600) return "between?";
  if (mv < 1400) return "empty";
  if (mv > 2500) return "NO SENSOR (pull-up only)";
  return "odd?";
}

void printLaneLive(const Lane &ln) {
  int mv = analogReadMilliVolts(ln.inlet);
  out.printf(" | %s inlet %4d mV %-8s wheel A %5lu B ", ln.sticker, mv, inletState(mv), (unsigned long)edges[ln.tachA]);
  if (mcpAddr) out.printf("%5lu", (unsigned long)bEdges[ln.mcpB]);
  else out.print("  n/a");
}

void printLive() {
  for (int f = 0; f < 2; f++) {
    const Feeder &x = F[f];
    out.printf("Unit %d | motor tach %5lu (%s)", f + 1, (unsigned long)edges[x.motorTach], digitalRead(x.motorTach) ? "hi" : "lo");
    for (const Lane &ln : LANES)
      if (ln.feeder == f) printLaneLive(ln);
    out.println();
  }
  if (mcpAddr) {
    int b = mcpRead(REG_GPIOB);
    if (b < 0) out.printf("MCP23017 at 0x%02X: read failed\n\n", mcpAddr);
    else out.printf("MCP23017 at 0x%02X OK | tach B pins now: GPB0 %d  GPB1 %d  GPB2 %d  GPB3 %d (1 = high)\n\n", mcpAddr,
                    b & 1, (b >> 1) & 1, (b >> 2) & 1, (b >> 3) & 1);
  }
  else out.println("MCP23017 not found: B tachs, LEDs and buttons unavailable (type i to scan)\n");
}

void liveView() {
  out.println("\n== Live view. Turn each wheel by hand: its A and B counts should both climb.");
  out.println("   Push filament into an inlet: it should go 'empty' -> 'LOADED'. Any letter stops.\n");
  liveSetup();
  uint32_t lastPrint = 0;
  for (;;) {
    pollB();
    if (millis() - lastPrint > 500) {
      lastPrint = millis();
      printLive();
    }
    int k = readKey();
    if (k > ' ') {  // ignore stray newlines / control bytes
      out.printf("  (stopped by key 0x%02X)\n", k);
      break;
    }
    delayMicroseconds(800);  // ~1 kHz B-tach polling
  }
  drainInput();
  liveTeardown();
}

// ---------- Short + pull-up test ----------
// Every rig pin is driven low, then high, while every other pin is watched.
// A watched pin that follows the driven pin is shorted to it.
enum Kind { K_PULLUP, K_DRV, K_SPARE, K_I2C };
struct TPin {
  bool mcp;  // false = C6 GPIO, true = MCP bit (0-7 = A, 8-15 = B)
  int n;
  Kind kind;
  const char *label;
};

const TPin TP[] = {
    {false, 2, K_PULLUP, "F1 inlet R (GPIO2)"},    {false, 3, K_PULLUP, "F1 inlet L (GPIO3)"},
    {false, 10, K_PULLUP, "F1 motor tach (GPIO10)"}, {false, 18, K_PULLUP, "F1 L tach A (GPIO18)"},
    {false, 20, K_PULLUP, "F1 R tach A (GPIO20)"},  {false, 22, K_DRV, "K_DRV IN1 (GPIO22)"},
    {false, 23, K_DRV, "K_DRV IN2 (GPIO23)"},         {false, 0, K_PULLUP, "F2 inlet R (GPIO0)"},
    {false, 1, K_PULLUP, "F2 inlet L (GPIO1)"},     {false, 11, K_PULLUP, "F2 motor tach (GPIO11)"},
    {false, 6, K_PULLUP, "F2 L tach A (GPIO6)"},    {false, 7, K_PULLUP, "F2 R tach A (GPIO7)"},
    {false, 4, K_DRV, "K_DRV IN3 (GPIO4)"},           {false, 5, K_DRV, "K_DRV IN4 (GPIO5)"},
    {false, 15, K_SPARE, "spare (GPIO15)"},         {false, 19, K_I2C, "I2C SDA (GPIO19)"},
    {false, 21, K_I2C, "I2C SCL (GPIO21)"},         {true, 0, K_SPARE, "MCP GPA0 F1 L white"},
    {true, 1, K_SPARE, "MCP GPA1 F1 L red"},        {true, 2, K_SPARE, "MCP GPA2 F1 R red"},
    {true, 3, K_SPARE, "MCP GPA3 F1 R white"},      {true, 4, K_SPARE, "MCP GPA4 F2 L white"},
    {true, 5, K_SPARE, "MCP GPA5 F2 L red"},        {true, 6, K_SPARE, "MCP GPA6 F2 R red"},
    {true, 7, K_SPARE, "MCP GPA7 F2 R white"},      {true, 8, K_PULLUP, "MCP GPB0 F1 L tach B"},
    {true, 9, K_PULLUP, "MCP GPB1 F1 R tach B"},    {true, 10, K_PULLUP, "MCP GPB2 F2 L tach B"},
    {true, 11, K_PULLUP, "MCP GPB3 F2 R tach B"},   {true, 12, K_DRV, "MCP GPB4 assist Feed"},
    {true, 13, K_DRV, "MCP GPB5 assist Unload"},    {true, 14, K_DRV, "MCP GPB6 assist Adjust"},
    {true, 15, K_SPARE, "MCP GPB7 spare"},
};
const int NTP = sizeof TP / sizeof TP[0];

// MCP shadow registers for the test
uint8_t mDir[2], mPu[2], mLat[2];
void mcpFlush() {
  mcpWrite(REG_OLATA, mLat[0]);
  mcpWrite(REG_OLATB, mLat[1]);
  mcpWrite(REG_GPPUA, mPu[0]);
  mcpWrite(REG_GPPUB, mPu[1]);
  mcpWrite(REG_IODIRA, mDir[0]);
  mcpWrite(REG_IODIRB, mDir[1]);
}
int mcpPins() {  // 16-bit input levels, -1 on error
  int a = mcpRead(REG_GPIOA), b = mcpRead(REG_GPIOB);
  return (a < 0 || b < 0) ? -1 : (a | (b << 8));
}

// Watch modes: pull-up or pull-down (MCP has no pull-down: it floats)
void setAllInputs(bool pullUp) {
  for (int i = 0; i < NTP; i++) {
    if (TP[i].mcp) continue;
    if (TP[i].kind == K_I2C && mcpAddr) continue;  // bus stays live while the MCP is used
    pinMode(TP[i].n, pullUp ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  if (mcpAddr) {
    mDir[0] = mDir[1] = 0xFF;
    mLat[0] = mLat[1] = 0x00;
    mPu[0] = mPu[1] = pullUp ? 0xFF : 0x00;
    mcpFlush();
  }
}

void driveC6(int pin, int level) {
  pinMode(pin, OUTPUT);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT);  // so we can read the pad back
  gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);  // weakest drive (~5 mA)
  digitalWrite(pin, level);
}

void driveMcp(int bit, int level) {
  int port = bit >> 3, m = 1 << (bit & 7);
  if (level) mLat[port] |= m;
  else mLat[port] &= ~m;
  mDir[port] &= ~m;
  mcpFlush();
}

// Reads every test pin into lv[] (1/0, or -1 if not readable now)
void readAll(int *lv) {
  int m = mcpAddr ? mcpPins() : -1;
  for (int i = 0; i < NTP; i++) {
    if (TP[i].mcp) lv[i] = (m < 0) ? -1 : ((m >> TP[i].n) & 1);
    else lv[i] = gpio_get_level((gpio_num_t)TP[i].n);
  }
}

bool usable(int i) { return !TP[i].mcp || mcpAddr; }
// The assist button pins sit on a PC817 LED to GND, so they read low under a weak pull-up: not watchable.
bool watchable(int i) { return usable(i) && !(TP[i].mcp && TP[i].kind == K_DRV); }

void shortTest() {
  out.println("\n== Short + pull-up test");
  out.println("   Unplug both U1 modules from the breakouts and unplug the 12 V adapter first.");
  out.println("   (Pins are driven high and low; a plugged-in module or live motor rail could react.)");
  if (!confirm("Both U1 modules unplugged and 12 V unplugged?")) {
    out.println("Cancelled.");
    return;
  }
  i2cStart();
  mcpFind();
  if (!mcpAddr) out.println("MCP23017 not found: testing C6 pins only (and GPIO19/21 as plain pins).\n");
  int fails = 0;

  // 1. Pull-up check: with the C6's weak internal pull-DOWN on, pins with a 10k pull-up still read high.
  out.println("-- Pull-ups (expect 10k to 3V3 on every sensor pin)");
  if (!mcpAddr) Wire.end();
  setAllInputs(false);
  delay(5);
  int lv[NTP], lo[NTP], hi[NTP];
  readAll(lv);
  for (int i = 0; i < NTP; i++) {
    if (TP[i].mcp) continue;
    bool high = lv[i] == 1;
    if (TP[i].kind == K_PULLUP && !high) {
      out.printf("  FAIL %-24s no pull-up found (reads low)\n", TP[i].label);
      fails++;
    } else if ((TP[i].kind == K_DRV || TP[i].kind == K_SPARE) && high) {
      out.printf("  FAIL %-24s reads high: shorted to 3V3, or a pull-up fitted by mistake\n", TP[i].label);
      fails++;
    } else if (TP[i].kind == K_I2C && !high && !mcpAddr) {
      out.printf("  info %-24s no pull-up (fine if the MCP board isn't plugged in yet)\n", TP[i].label);
    }
  }
  if (mcpAddr) {  // MCP: internal pull-ups off, floating inputs. A 10k pull-up holds tach B pins high.
    for (int i = 0; i < NTP; i++) {
      if (!TP[i].mcp || TP[i].kind != K_PULLUP) continue;
      int highs = 0;
      for (int k = 0; k < 20; k++) {
        int m = mcpPins();
        if (m >= 0 && ((m >> TP[i].n) & 1)) highs++;
        delay(1);
      }
      if (highs < 20) {
        out.printf("  FAIL %-24s no pull-up found (reads low %d/20)\n", TP[i].label, 20 - highs);
        fails++;
      }
    }
  }

  // 2. Stuck pins: with pull-ups on everywhere, anything reading low is shorted to GND.
  out.println("-- Shorts to GND");
  setAllInputs(true);
  delay(5);
  readAll(lv);
  bool stuckLow[NTP];
  for (int i = 0; i < NTP; i++) {
    stuckLow[i] = usable(i) && TP[i].kind != K_DRV && lv[i] == 0;
    if (stuckLow[i]) {
      out.printf("  FAIL %-24s reads low with pull-up on: shorted to GND\n", TP[i].label);
      fails++;
    }
  }

  // 3. Pin-to-pin: drive each pin low (others pulled up) and high (others pulled down).
  out.println("-- Pin to pin");
  for (int d = 0; d < NTP; d++) {
    if (!usable(d) || stuckLow[d]) continue;
    bool isI2C = !TP[d].mcp && TP[d].kind == K_I2C;
    if (isI2C && mcpAddr) continue;  // done below, with the bus released
    bool canHigh = !(TP[d].mcp && TP[d].kind == K_DRV);  // never drive the assist buttons high

    setAllInputs(true);
    if (TP[d].mcp) driveMcp(TP[d].n, LOW);
    else driveC6(TP[d].n, LOW);
    delayMicroseconds(300);
    readAll(lo);

    if (canHigh) {
      setAllInputs(false);
      if (TP[d].mcp) driveMcp(TP[d].n, HIGH);
      else driveC6(TP[d].n, HIGH);
      delayMicroseconds(300);
      readAll(hi);
    }
    if (!TP[d].mcp) gpio_set_drive_capability((gpio_num_t)TP[d].n, GPIO_DRIVE_CAP_2);
    setAllInputs(true);

    if (!TP[d].mcp && lo[d] != 0) {
      out.printf("  FAIL %-24s can't pull it low: shorted to 3V3\n", TP[d].label);
      fails++;
      continue;
    }
    if (!TP[d].mcp && canHigh && hi[d] != 1) {
      out.printf("  FAIL %-24s can't drive it high: shorted to GND\n", TP[d].label);
      fails++;
      continue;
    }
    for (int w = 0; w < NTP; w++) {
      if (w == d || !watchable(w) || stuckLow[w] || lo[w] < 0) continue;
      bool followsLow = lo[w] == 0;
      // MCP inputs float when "pulled down", so only the low pass counts for them.
      // C6 pins with a 10k pull-up never read low under the internal pull-down; low pass decides.
      bool followsHigh = !canHigh || TP[w].mcp || TP[w].kind == K_PULLUP || TP[w].kind == K_I2C || hi[w] == 1;
      if (followsLow && followsHigh && (w > d || !watchable(d))) {
        out.printf("  FAIL %-24s shorted to %s\n", TP[d].label, TP[w].label);
        fails++;
      }
    }
  }

  // 4. I2C pins, with the bus released (only C6 pins can watch them then)
  if (mcpAddr) {
    Wire.end();
    for (int s : {PIN_SDA, PIN_SCL}) {
      for (int i = 0; i < NTP; i++)
        if (!TP[i].mcp) pinMode(TP[i].n, INPUT_PULLUP);
      driveC6(s, LOW);
      delayMicroseconds(300);
      readAll(lo);
      for (int i = 0; i < NTP; i++)
        if (!TP[i].mcp && TP[i].n != s) pinMode(TP[i].n, INPUT_PULLDOWN);
      digitalWrite(s, HIGH);
      delayMicroseconds(300);
      readAll(hi);
      gpio_set_drive_capability((gpio_num_t)s, GPIO_DRIVE_CAP_2);
      pinMode(s, INPUT_PULLUP);
      for (int w = 0; w < NTP; w++) {
        if (TP[w].mcp || TP[w].n == s || stuckLow[w]) continue;
        if (TP[w].kind == K_I2C && TP[w].n < s) continue;  // SDA-SCL pair already reported once
        bool followsHigh = TP[w].kind == K_PULLUP || TP[w].kind == K_I2C || hi[w] == 1;
        if (lo[w] == 0 && followsHigh) {
          out.printf("  FAIL GPIO%-2d (I2C)              shorted to %s\n", s, TP[w].label);
          fails++;
        }
      }
    }
    i2cStart();
  }

  if (mcpAddr) mcpSafeInit();
  else i2cStart();
  liveSetup();  // DRV inputs back to driven-low outputs, so a motor can't creep
  liveTeardown();
  out.printf("\n== %s: %d pins checked, %d problem%s\n", fails ? "PROBLEMS" : "ALL GOOD",
             mcpAddr ? NTP : NTP - 16, fails, fails == 1 ? "" : "s");
  out.println("   Not covered: the assist button pins are only driven low, and a wire landing on the");
  out.println("   wrong U1 pin looks fine here. Plug the modules back in and use the live view for that.");
}

// ---------- LED walk ----------
void ledWalk() {
  out.println("\n== LED walk (needs the U1 modules plugged in)");
  if (!mcpAddr) {
    out.println("MCP23017 not found.");
    return;
  }
  mcpSafeInit();
  // Walk in sticker order L1, L2, R3, R4. Bench 2026-11-08: U1 pins 6/7 = the U1 left lane's LEDs
  // (L1 on unit 1, R4 on unit 2), pins 12/13 = the right lane's (L2, R3).
  // Every lane white first, then red: left lane pin 6 (white), 7 (red); right lane pin 13 (white), 12 (red).
  for (const Lane &ln : LANES) {
    const int order[2] = {ln.u1Right ? 3 : 0, ln.u1Right ? 2 : 1};
    for (int i = 0; i < 2; i++) {
      int k = order[i];
      int bit = F[ln.feeder].mcpLedBase + k;
      out.printf("  GPA%d on: Unit %d %s %s (U1 pin %d)\n", bit, ln.feeder + 1, ln.sticker, i == 0 ? "white" : "red",
                 LED_PINS[k]);
      mcpWrite(REG_OLATA, 1 << bit);
      delay(1200);
    }
  }
  mcpWrite(REG_OLATA, 0);
  out.println("  done, all off.");
}

// ---------- Motor test ----------
void motorPulse(const Feeder &x, bool rightLane, int ms) {
  edges[x.motorTach] = 0;
  digitalWrite(x.in1, rightLane ? HIGH : LOW);
  digitalWrite(x.in2, rightLane ? LOW : HIGH);
  delay(ms);
  digitalWrite(x.in1, HIGH);  // brake
  digitalWrite(x.in2, HIGH);
  delay(50);
  digitalWrite(x.in1, LOW);  // coast
  digitalWrite(x.in2, LOW);
  out.printf("  %s %s-lane direction %d ms: motor tach %lu edges\n", x.name, rightLane ? "right" : "left", ms,
             (unsigned long)edges[x.motorTach]);
}

void motorTest() {
  out.println("\n== Motor test: 150 ms pulse each way on each feeder");
  if (!confirm("12 V plugged in, LM2596 checked at 6.0 V, U1 modules plugged in?")) {
    out.println("Cancelled.");
    return;
  }
  liveSetup();
  for (int f = 0; f < 2; f++) {
    motorPulse(F[f], true, 150);
    delay(600);
    motorPulse(F[f], false, 150);
    delay(600);
  }
  liveTeardown();
  out.println("  0 edges on a feeder = motor didn't turn (check DRV wiring/EEP) or tach not wired.");
}

// ---------- Feed test: 1 s on each lane, shows which wheels turn ----------
void feedPulse(const Lane &ln, int ms) {
  const Feeder &x = F[ln.feeder];
  for (int p : TACH_PINS) edges[p] = 0;
  memset(bEdges, 0, sizeof bEdges);
  lastB = -1;
  digitalWrite(x.in1, ln.u1Right ? HIGH : LOW);
  digitalWrite(x.in2, ln.u1Right ? LOW : HIGH);
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)ms) pollB();
  digitalWrite(x.in1, HIGH);  // brake
  digitalWrite(x.in2, HIGH);
  delay(50);
  digitalWrite(x.in1, LOW);  // coast
  digitalWrite(x.in2, LOW);
  out.printf("  %s (unit %d, %s high) %d ms: motor tach %lu", ln.sticker, ln.feeder + 1, ln.pin, ms,
             (unsigned long)edges[x.motorTach]);
  for (const Lane &o : LANES)
    if (o.feeder == ln.feeder) {
      int mv = analogReadMilliVolts(o.inlet);
      out.printf(" | %s wheel A %lu B %lu, inlet %s", o.sticker, (unsigned long)edges[o.tachA], (unsigned long)bEdges[o.mcpB],
                 inletState(mv));
    }
  out.println();
}

void feedTest() {
  out.println("\n== Feed test: 1 s on each lane in turn (L1, L2, R3, R4), filament in all 4 inlets");
  if (!confirm("12 V on, filament in all 4 inlets, outlets clear?")) {
    out.println("Cancelled.");
    return;
  }
  liveSetup();
  for (const Lane &ln : LANES) {
    feedPulse(ln, 1000);
    delay(800);
  }
  liveTeardown();
  out.println("  Only the lane being fed should count wheel edges.");
}

void laneFeed(int n) {
  const Lane &ln = LANES[n - 1];
  out.printf("\n== Lane %s: 1 s feed (feeder %d, %s high)\n", ln.sticker, ln.feeder + 1, ln.pin);
  if (!confirm("12 V on, filament in, outlets clear?")) {
    out.println("Cancelled.");
    return;
  }
  liveSetup();
  feedPulse(ln, 1000);
  liveTeardown();
}

// ---------- Assist buttons ----------
void buttonTest() {
  out.println("\n== Assist button test: Feed (GPB4), Adjust (GPB6), Unload (GPB5), 200 ms each, 3 s apart");
  if (!mcpAddr) {
    out.println("MCP23017 not found.");
    return;
  }
  if (!confirm("This presses the real buttons if the harness is on the assist. Go?")) {
    out.println("Cancelled.");
    return;
  }
  mcpSafeInit();
  const int bits[3] = {MCP_FEED_BIT, MCP_ADJUST_BIT, MCP_UNLOAD_BIT};
  const char *names[3] = {"Feed", "Adjust", "Unload"};
  for (int i = 0; i < 3; i++) {
    out.printf("  %s press (GPB%d)\n", names[i], bits[i]);
    mcpWrite(REG_OLATB, 1 << bits[i]);
    delay(200);
    mcpWrite(REG_OLATB, 0);
    if (i < 2) delay(3000);
  }
  out.println("  done, all released.");
}

// ---------- Main ----------
void help() {
  out.println("\nRig v2 pin checker. Send a letter + Enter:");
  out.println("  l  live view (sensors and tachs)");
  out.println("  s  short + pull-up test (U1 modules and 12 V unplugged)");
  out.println("  e  LED walk");
  out.println("  m  motor pulse test (12 V on)");
  out.println("  f  feed test, 1 s on each lane in turn (12 V on, filament in)");
  out.println("  1-4  feed one lane for 1 s (1 = L1, 2 = L2, 3 = R3, 4 = R4 stickers)");
  out.println("  b  assist button test");
  out.println("  i  I2C scan");
  out.println("  ?  this help");
}

void setup() {
  // Motors off first thing (GPIO4/5 are strapping pins; the DRV pull-downs already hold them low)
  for (int f = 0; f < 2; f++) {
    pinMode(F[f].in1, OUTPUT);
    digitalWrite(F[f].in1, LOW);
    pinMode(F[f].in2, OUTPUT);
    digitalWrite(F[f].in2, LOW);
  }
  Serial0.begin(115200);
  HWCDCSerial.begin();
  HWCDCSerial.setTxTimeoutMs(0);  // don't stall when the USB port isn't open
  delay(1500);
  i2cStart();
  mcpFind();
  mcpSafeInit();
  help();
  liveView();
}

void loop() {
  out.print("\n> ");
  int c = waitKey();
  drainInput();
  out.println((char)c);
  switch (c) {
    case 'l': liveView(); break;
    case 's': shortTest(); break;
    case 'e': ledWalk(); break;
    case 'm': motorTest(); break;
    case 'f': feedTest(); break;
    case '1': case '2': case '3': case '4': laneFeed(c - '0'); break;
    case 'b': buttonTest(); break;
    case 'i': i2cScan(); break;
    default: help();
  }
}
