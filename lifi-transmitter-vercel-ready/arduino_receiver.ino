/*
  =====================================================================
  Li-Fi Receiver v2.1  -  Manchester coded, self-clocking, CRC checked
  Live letters on the LCD, full message for 5 s, then ready again
  =====================================================================
  Works with the Li-Fi Transmitter web app (any text, numbers, symbols,
  spaces, even UTF-8 bytes). Speed is detected automatically from the
  start header, so Fast / Normal / Safe speeds on the website all work.

  Hardware:
    LDR module DO  -> D2        (VCC -> 5V, GND -> GND)
    I2C LCD 16x2   -> SDA = A4, SCL = A5 (address auto-detected)

  Libraries: "LiquidCrystal I2C" (Frank de Brabander), Wire (built-in)

  FIRST TIME SETUP (important!):
    1. Set CALIBRATE to 1, upload, open Serial Monitor (9600).
    2. Room lighting: LCD must show DARK. Phone torch 2-5 cm from the
       LDR: LCD must show LIGHT. Turn the blue potentiometer on the LDR
       module until both are true.
    3. If it shows the opposite (LIGHT when dark), change LIGHT_LEVEL
       from LOW to HIGH.
    4. Set CALIBRATE back to 0 and upload again.

  Frame format (all bits Manchester coded, T = half-bit time):
    HEADER : light ON for 8T            -> receiver measures T from this
    GAP    : light OFF for 2T
    BYTES  : 0xA5 (sync) | LEN | payload bytes... | CRC8(LEN+payload)
    bit 1  = ON then OFF   (falling edge in the middle of the bit)
    bit 0  = OFF then ON   (rising  edge in the middle of the bit)
  =====================================================================
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ------------------------- SETTINGS ---------------------------------
#define LDR_PIN       2
#define LIGHT_LEVEL   LOW     // pin level when LIGHT is detected (most LM393 LDR modules: LOW)
#define CALIBRATE     0       // 1 = sensor test mode, 0 = normal receiver
// --------------------------------------------------------------------

#define DEBOUNCE_MS   4       // ignore glitches shorter than this
#define MAX_PAYLOAD   64      // maximum message length in bytes
#define SYNC_BYTE     0xA5
#define T_MIN_MS      40      // slowest/fastest half-bit accepted
#define T_MAX_MS      400
#define GUARD_MS      1500    // darkness needed after a frame before re-arming

enum { RX_OK = 0, E_HEADER, E_TIMING, E_SYNC, E_LEN, E_CRC };

void liveShow(byte count);   // defined below (live letter display)

LiquidCrystal_I2C *lcd;

// ----- filtered sensor state -----
bool          lightOn    = false;
unsigned long lastEdgeMs = 0;     // time the last confirmed change started
bool          candActive = false;
unsigned long candMs     = 0;

// ----- frame data -----
byte          payload[MAX_PAYLOAD + 1];
byte          payloadLen = 0;
unsigned long T = 150;

bool armed = false;

// ----- edge log (printed on errors to help debugging) -----
#define EDGE_LOG_SIZE 40
unsigned long edgeT[EDGE_LOG_SIZE];
bool          edgeR[EDGE_LOG_SIZE];
byte          edgeN = 0;

// =====================================================================
//  Sensor handling
// =====================================================================
void sensorUpdate() {
  bool raw = (digitalRead(LDR_PIN) == LIGHT_LEVEL);
  if (raw == lightOn) {
    candActive = false;
    return;
  }
  unsigned long now = millis();
  if (!candActive) {
    candActive = true;
    candMs = now;
    return;
  }
  if (now - candMs >= DEBOUNCE_MS) {
    lightOn = raw;
    lastEdgeMs = candMs;          // real time the change began
    candActive = false;
    digitalWrite(LED_BUILTIN, lightOn ? HIGH : LOW);
    if (edgeN < EDGE_LOG_SIZE) { edgeT[edgeN] = lastEdgeMs; edgeR[edgeN] = lightOn; edgeN++; }
  }
}

void waitUntilMs(unsigned long t) {
  while ((long)(millis() - t) < 0) sensorUpdate();
}

// =====================================================================
//  CRC-8 (poly 0x07, init 0x00)  - same as the website
// =====================================================================
byte crc8Update(byte crc, byte data) {
  crc ^= data;
  for (byte i = 0; i < 8; i++) {
    crc = (crc & 0x80) ? (byte)((crc << 1) ^ 0x07) : (byte)(crc << 1);
  }
  return crc;
}

// =====================================================================
//  Manchester decoding (edge based, self-calibrating)
//
//  Edges always alternate rise/fall. After a "mid-bit" edge the next edge
//  comes T later (a bit-boundary edge) or 2T later (the next mid-bit edge).
//  An LDR usually turns OFF slower than it turns ON, which shifts falling
//  edges later than rising edges. D = that difference. It is measured from
//  the header and then tracked, so it cancels out.
// =====================================================================
long dT;               // half-bit time (ms)
long dD;               // falling-edge delay minus rising-edge delay (ms)
unsigned long dTp;     // time of previous edge
bool dPrevMid;         // was the previous edge a mid-bit edge?

// Wait for the next confirmed light change. Returns false on timeout.
bool waitEdge(unsigned long deadline, unsigned long &te, bool &rising) {
  bool before = lightOn;
  while ((long)(millis() - deadline) < 0) {
    sensorUpdate();
    if (lightOn != before) {
      te = lastEdgeMs;
      rising = lightOn;
      return true;
    }
  }
  return false;
}

// Returns the next decoded bit (0/1) or -1 on error.
int nextBit() {
  for (byte guard = 0; guard < 3; guard++) {
    unsigned long te;
    bool rising;
    if (!waitEdge(dTp + 3UL * dT, te, rising)) return -1;

    long I  = (long)(te - dTp);                 // time since previous edge
    long Ic = rising ? (I + dD) : (I - dD);     // remove rise/fall delay difference
    long n  = (Ic + dT / 2) / dT;               // 1 -> one T, 2 -> two T
    if (Ic < 0) n = 0;

    if (n < 1 || n > 2) return -1;
    if (!dPrevMid && n != 1) return -1;         // after a boundary edge a mid edge must follow in T

    // refine the rise/fall delay estimate
    long sample = rising ? (n * dT - I) : (I - n * dT);
    dD += (sample - dD) / 8;

    dTp = te;
    if (dPrevMid && n == 1) {                   // bit-boundary edge: not data
      dPrevMid = false;
      continue;
    }
    dPrevMid = true;                            // mid-bit edge = data
    return rising ? 0 : 1;                      // rising = 0, falling = 1
  }
  return -1;
}

int readByte() {
  byte v = 0;
  for (byte i = 0; i < 8; i++) {
    int b = nextBit();
    if (b < 0) return -1;
    v = (v << 1) | (byte)b;
  }
  return v;
}

// tRise = time the light first turned ON (start of header)
byte receiveFrame(unsigned long tRise) {
  // ---- header: light stays ON for 8T (+ delay difference), then goes OFF ----
  while (lightOn) {
    sensorUpdate();
    if (millis() - tRise > (unsigned long)T_MAX_MS * 8UL + 300UL) return E_HEADER;
  }
  unsigned long tFall = lastEdgeMs;
  long H = (long)(tFall - tRise);
  if (H < (long)T_MIN_MS * 8 || H > (long)T_MAX_MS * 8 + 200) return E_HEADER;
  long T0 = H / 8;

  // ---- first edge: light ON again after the 2T gap (first sync bit is 1) ----
  unsigned long te;
  bool rising;
  if (!waitEdge(tFall + 4UL * T0, te, rising)) return E_TIMING;
  long G = (long)(te - tFall);
  if (G < T0 / 2 || G > (T0 * 5) / 2) return E_SYNC;

  // H = 8T + D,  G = 2T - D   ->   T = (H + G) / 10,  D = H - 8T
  dT = (H + G) / 10;
  dD = H - 8 * dT;
  if (dD > dT) dD = dT;
  if (dD < -dT) dD = -dT;
  T = (unsigned long)dT;
  dTp = te;
  dPrevMid = false;                              // that rising edge was a bit boundary

  int b = readByte();
  if (b < 0) return E_TIMING;
  if (b != SYNC_BYTE) return E_SYNC;

  int len = readByte();
  if (len < 0) return E_TIMING;
  if (len < 1 || len > MAX_PAYLOAD) return E_LEN;

  byte crc = crc8Update(0, (byte)len);
  for (int i = 0; i < len; i++) {
    int v = readByte();
    if (v < 0) return E_TIMING;
    payload[i] = (byte)v;
    crc = crc8Update(crc, (byte)v);
    liveShow((byte)(i + 1));
  }

  int rxCrc = readByte();
  if (rxCrc < 0) return E_TIMING;
  if ((byte)rxCrc != crc) return E_CRC;

  payload[len] = 0;
  payloadLen = (byte)len;
  return RX_OK;
}

// =====================================================================
//  LCD helpers
// =====================================================================
void lcdRow(byte row, const char *s) {           // prints exactly 16 chars
  lcd->setCursor(0, row);
  byte i = 0;
  for (; s[i] && i < 16; i++) lcd->write(s[i]);
  for (; i < 16; i++) lcd->write(' ');
}

void lcdPayloadRow(byte row, byte from) {         // 16 chars of payload from index 'from'
  lcd->setCursor(0, row);
  for (byte i = 0; i < 16; i++) {
    byte idx = from + i;
    if (idx < payloadLen) {
      byte c = payload[idx];
      lcd->write((c >= 32 && c <= 126) ? c : '?');
    } else {
      lcd->write(' ');
    }
  }
}

void printEdgeLog() {
  Serial.print(F("  edges (ms from first light): "));
  for (byte i = 0; i < edgeN; i++) {
    Serial.print(edgeR[i] ? F("R+") : F("F+"));
    Serial.print(edgeT[i] - edgeT[0]);
    Serial.print(' ');
  }
  Serial.println();
}

// Live display while a message is arriving: one new letter at a time.
// Called right after a byte is decoded (the next edge is at least T ms away,
// so the short LCD write cannot disturb the decoding).
void showReceiving() {
  lcdRow(0, "Receiving:");
  lcdRow(1, "");
}

void liveShow(byte count) {                      // count = letters decoded so far
  if (count <= 16) {
    byte c = payload[count - 1];
    lcd->setCursor(count - 1, 1);
    lcd->write((c >= 32 && c <= 126) ? c : '?');
  } else {                                       // scroll: show the last 16 letters
    lcd->setCursor(0, 1);
    for (byte i = count - 16; i < count; i++) {
      byte c = payload[i];
      lcd->write((c >= 32 && c <= 126) ? c : '?');
    }
  }
}

void showWaiting() {
  lcdRow(0, "LiFi Receiver");
  lcdRow(1, "Waiting data...");
}

void showPage(byte page) {
  if (payloadLen <= 16) {
    lcdRow(0, "LiFi Received:");
    lcdPayloadRow(1, 0);
  } else {
    lcdPayloadRow(0, page * 32);
    lcdPayloadRow(1, page * 32 + 16);
  }
}

void showError(byte e) {
  char line[17];
  const char *name = "Unknown";
  switch (e) {
    case E_HEADER: name = "Bad start";  break;
    case E_TIMING: name = "Lost sync";  break;
    case E_SYNC:   name = "No sync";    break;
    case E_LEN:    name = "Bad length"; break;
    case E_CRC:    name = "CRC error";  break;
  }
  snprintf(line, sizeof(line), "ERR: %s", name);
  lcdRow(0, line);
  lcdRow(1, "Send again...");
}

// =====================================================================
//  Re-arm: wait until it has been dark for GUARD_MS
// =====================================================================
void waitForQuiet() {
  // Longest dark stretch inside a frame is about 2T, so 3T + 300 ms is safe
  unsigned long guard = 3UL * T + 300UL;
  if (guard > GUARD_MS) guard = GUARD_MS;
  unsigned long darkSince = millis();
  while (true) {
    sensorUpdate();
    if (lightOn) darkSince = millis();
    else if (millis() - darkSince >= guard) break;
  }
}

// Wait 'ms' milliseconds; returns true early if a new transmission starts.
bool holdMs(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) {
    sensorUpdate();
    if (lightOn) return true;
  }
  return false;
}

// =====================================================================
//  Setup
// =====================================================================
byte findLcdAddress() {
  const byte list[] = {0x27, 0x3F, 0x26, 0x20, 0x38};
  for (byte i = 0; i < sizeof(list); i++) {
    Wire.beginTransmission(list[i]);
    if (Wire.endTransmission() == 0) return list[i];
  }
  return 0x27;
}

void setup() {
  pinMode(LDR_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(9600);
  Wire.begin();

  byte addr = findLcdAddress();
  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();
  lcd->clear();

  lightOn = (digitalRead(LDR_PIN) == LIGHT_LEVEL);
  lastEdgeMs = millis();

  Serial.println(F("=========================================="));
  Serial.println(F(" Li-Fi Receiver v2 (Manchester + CRC)"));
  Serial.print(F(" LCD address: 0x"));
  Serial.println(addr, HEX);
#if CALIBRATE
  Serial.println(F(" MODE: CALIBRATION (set CALIBRATE 0 when done)"));
#endif
  Serial.println(F("=========================================="));

#if !CALIBRATE
  showWaiting();
#endif
}

// =====================================================================
//  Main loop
// =====================================================================
void loop() {
#if CALIBRATE
  // ---- Sensor test: shows LIGHT / DARK live ----
  static int lastShown = -1;
  sensorUpdate();
  if ((int)lightOn != lastShown) {
    lastShown = lightOn;
    lcdRow(0, "SENSOR TEST MODE");
    lcdRow(1, lightOn ? "State: LIGHT" : "State: DARK");
    Serial.println(lightOn ? F("LIGHT") : F("DARK"));
  }
#else
  sensorUpdate();

  // Must see darkness first, so a lamp that is already on never triggers us.
  if (!lightOn) {
    if (!armed && millis() - lastEdgeMs > 300) armed = true;
    return;
  }
  if (!armed) return;

  // Light just turned ON -> start of a frame header
  armed = false;
  unsigned long tRise = lastEdgeMs;
  edgeN = 1;                       // keep the header rise already logged
  edgeT[0] = tRise; edgeR[0] = true;
  T = 150;
  showReceiving();                 // header lasts >= 0.8 s, plenty of time for this
  byte result = receiveFrame(tRise);

  if (result == RX_OK) {
    Serial.print(F("RX OK  T="));
    Serial.print(T);
    Serial.print(F("ms  len="));
    Serial.print(payloadLen);
    Serial.print(F("  msg: "));
    Serial.write(payload, payloadLen);
    Serial.println();

    // Full message on the LCD, then back to "Waiting" after 5 seconds
    showPage(0);
    waitForQuiet();

    bool newMsg = false;
    byte pages = (payloadLen + 31) / 32;
    if (payloadLen > 16) {                       // long message: 32-letter pages
      for (byte p = 1; p < pages && !newMsg; p++) {
        newMsg = holdMs(3000);
        if (!newMsg) showPage(p);
      }
    }
    if (!newMsg) newMsg = holdMs(5000);
    if (!newMsg) showWaiting();                  // ready for the next message
  } else {
    Serial.print(F("RX ERROR code "));
    Serial.print(result);
    Serial.print(F("  (T="));
    Serial.print(T);
    Serial.println(F("ms)"));
    printEdgeLog();
    showError(result);
    waitForQuiet();
    if (!holdMs(3000)) showWaiting();
  }
  armed = true;
#endif
}
