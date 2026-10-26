#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>

#define UART_RX_PIN 44   // ESP32-S3 U0RXD (the RX pin next to the board's pin 1 area)
#define UART_TX_PIN 43   // ESP32-S3 U0TXD, kept defined for valid UART setup; meter is read-only
#define SERIAL_BAUD 9600

const char* AP_SSID = "VOM-703S";
const char* AP_PASSWORD = "zoyi703s123";
const char* WIFI_SSID = "2G_Viettel";
const char* WIFI_PASSWORD = "......";
const IPAddress LAN_STATIC_IP(192, 168, 88, 50); //ip lan 192.168.88.50
const IPAddress LAN_GATEWAY(192, 168, 88, 1);
const IPAddress LAN_SUBNET(255, 255, 255, 0);
const IPAddress AP_IP(192, 168, 4, 1); //AP of esp32
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

String currentWebAddress() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP()) {
    return WiFi.localIP().toString();
  }
  return WiFi.softAPIP().toString();
}

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

struct MeterReading {
  String mode = "UNKNOWN";
  String unit = "--";
  float value = NAN;
  String raw = "";
  String rawHex = "";
  String label = "";
  String freq = "";
  String freqUnit = "";
  unsigned long ts = 0;
};

MeterReading latestReading;
uint8_t frameBuffer[18];
size_t frameIndex = 0;
String serialBuffer;
bool readingDirty = false;
constexpr uint32_t UART_HISTORY_INTERVAL_MS = 200;
constexpr size_t UART_HISTORY_SIZE = 5;
String uartHistory[UART_HISTORY_SIZE];
size_t uartHistoryNext = 0;
size_t uartHistoryCount = 0;
uint32_t uartSequence = 0;

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() * 2 + 2);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input.charAt(i);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

void recordUartLine(const String& input) {
  String line = input;
  line.trim();
  if (line.length() == 0) return;
  if (line.length() > 96) line = line.substring(0, 93) + "...";
  uartHistory[uartHistoryNext] = line;
  uartHistoryNext = (uartHistoryNext + 1) % UART_HISTORY_SIZE;
  if (uartHistoryCount < UART_HISTORY_SIZE) ++uartHistoryCount;
  ++uartSequence;
}

String uartHistoryJson() {
  String json = "{\"seq\":" + String(uartSequence) + ",\"lines\":[";
  for (size_t index = 0; index < uartHistoryCount; ++index) {
    size_t slot = (uartHistoryNext + UART_HISTORY_SIZE - 1 - index) % UART_HISTORY_SIZE;
    if (index > 0) json += ',';
    json += '"';
    json += jsonEscape(uartHistory[slot]);
    json += '"';
  }
  return json + "]}";
}

String extendedFrameText(const uint8_t* frame, size_t length) {
  static const char hexDigits[] = "0123456789ABCDEF";
  String text = "BIN ";
  for (size_t index = 0; index < length; ++index) {
    if (index > 0) text += ' ';
    text += hexDigits[(frame[index] >> 4) & 0x0F];
    text += hexDigits[frame[index] & 0x0F];
  }
  return text;
}

void recordExtendedFrameHistory(const uint8_t* frame, size_t length) {
  static uint32_t lastRecordedAt = 0;
  const uint32_t now = millis();
  if (now - lastRecordedAt < UART_HISTORY_INTERVAL_MS) return;
  lastRecordedAt = now;
  recordUartLine(extendedFrameText(frame, length));
}

String trimWhitespace(const String& in) {
  String out = in;
  while (out.length() > 0 && (out.charAt(0) == ' ' || out.charAt(0) == '\t' || out.charAt(0) == '\r' || out.charAt(0) == '\n')) {
    out.remove(0, 1);
  }
  while (out.length() > 0 && (out.charAt(out.length() - 1) == ' ' || out.charAt(out.length() - 1) == '\t' || out.charAt(out.length() - 1) == '\r' || out.charAt(out.length() - 1) == '\n')) {
    out.remove(out.length() - 1, 1);
  }
  return out;
}

String extractFirstFloat(const String& text) {
  String result;
  bool started = false;
  bool hasDot = false;

  for (size_t i = 0; i < text.length(); ++i) {
    char c = text.charAt(i);
    bool isDigit = (c >= '0' && c <= '9');
    bool isSign = (c == '-' || c == '+');

    if (isDigit || (c == '.' && !hasDot) || (isSign && !started)) {
      if (!started && (c == '-' || c == '+')) {
        result += c;
        started = true;
        continue;
      }
      if (c == '.') {
        hasDot = true;
      }
      result += c;
      started = true;
      continue;
    }

    if (started) {
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') {
        break;
      }
      if (c == 'k' || c == 'm' || c == 'M' || c == 'u' || c == 'n' || c == 'V' || c == 'A' || c == 'O' || c == 'H' || c == 'F' || c == 'C' || c == '%') {
        break;
      }
    }
  }

  result.trim();
  if (result.length() > 0) {
    return result;
  }
  return "";
}

String statusJson() {
  String valuePart = "null";
  if (!isnan(latestReading.value)) {
    valuePart = String(latestReading.value, 4);
  }
  String json = "{\"type\":\"meter\",\"mode\":\"" + latestReading.mode + "\",\"value\":" + valuePart + ",\"unit\":\"" + latestReading.unit + "\",\"raw\":\"" + jsonEscape(latestReading.raw) + "\",\"label\":\"" + jsonEscape(latestReading.label) + "\",\"freq\":\"" + jsonEscape(latestReading.freq) + "\",\"freqUnit\":\"" + jsonEscape(latestReading.freqUnit) + "\",\"uartSeq\":" + String(uartSequence) + ",\"ts\":" + String(latestReading.ts) + ",\"wifi\":\"" + currentWebAddress() + "\"}";
  return json;
}

String streamJson() {
  String valuePart = "null";
  if (!isnan(latestReading.value)) {
    valuePart = String(latestReading.value, 4);
  }

  return "{\"type\":\"meter\",\"mode\":\"" + latestReading.mode +
         "\",\"value\":" + valuePart + ",\"unit\":\"" + latestReading.unit +
         "\",\"uartSeq\":" + String(uartSequence) + ",\"ts\":" + String(latestReading.ts) + ",\"wifi\":\"" +
         currentWebAddress() + "\"}";
}

void broadcastMeter() {
  if (webSocket.connectedClients() == 0) {
    return;
  }
  String payload = streamJson();
  webSocket.broadcastTXT(payload.c_str(), payload.length());
}

static uint8_t decodeDigitFromByte(uint8_t b) {
  switch (b) {
    case 0xEB: return 0;
    case 0x0A: return 1;
    case 0xAD: return 2;
    case 0x8F: return 3;
    case 0x4E: return 4;
    case 0xC7: return 5;
    case 0xE7: return 6;
    case 0x8A: return 7;
    case 0xEF: return 8;
    case 0xCF: return 9;
    default: return 0;
  }
}

String decodeExtendedFrameToString(const uint8_t* frame, size_t len) {
  if (len < 18) return "";

  uint8_t work[18];
  memcpy(work, frame, 18);

  uint8_t bit4 = 0x10;
  String value = "";
  bool decimalFound = false;

  for (int i = 5; i > 0; i--) {
    if ((work[i] & bit4) > 0) {
      if (i == 5) {
        value += '-';
      } else {
        value += '.';
        decimalFound = true;
      }
      work[i] ^= bit4;
    }
    value += String(decodeDigitFromByte(work[i]));
  }

  if (!decimalFound && value.length() > 2) {
    value = value.substring(0, value.length() - 1) + "." + value.substring(value.length() - 1);
  }

  return value;
}

String decodeExtendedUnit(const uint8_t* frame) {
  uint8_t unit1 = frame[15];
  uint8_t unit2 = frame[16];
  String unit = "";

  switch (unit1) {
    case 0x40: unit = "n"; break;
    case 0x80: unit = "u"; break;
    case 0x0C: unit = "m"; break;
    default: break;
  }

  switch (unit2) {
    case 0x01: unit += "A"; break;
    case 0x02: unit += "V"; break;
    case 0x04: unit += "F"; break;
    case 0x09: unit += "mA"; break;
    case 0x0A: unit += "mV"; break;
    case 0x0C: unit += "mF"; break;
    case 0x40: unit += "Ω"; break;
    case 0x50: unit += "MΩ"; break;
    case 0x60: unit += "kΩ"; break;
    default: break;
  }

  if (unit == "F") {
    if (unit1 == 0x40) unit = "nF";
    else if (unit1 == 0x80) unit = "uF";
    else if (unit1 == 0x0C) unit = "mF";
  }
  return unit;
}

String decodeExtendedMode(const uint8_t* frame) {
  uint8_t mode1 = frame[10];
  uint8_t mode2 = frame[11];

  String mode = "UNKNOWN";
  if ((mode1 & 0x20) > 0) mode = "REL";
  else if ((mode1 & 0x40) > 0) mode = "CONT";
  else if ((mode1 & 0x80) > 0) mode = "DIODE";
  else if ((mode2 & 0x04) > 0) mode = "AUTO";

  if ((mode2 & 0x10) > 0) return "DC";
  if ((mode2 & 0x40) > 0) return "AC";
  return mode;
}

bool isExtendedOLFrame(const uint8_t* frame) {
  static const uint8_t olPatterns[][5] = {
    {0x00, 0x00, 0x61, 0xEB, 0x00},
    {0x00, 0x00, 0x61, 0xDB, 0x00},
    {0x00, 0x00, 0x61, 0x8B, 0x00},
    {0x00, 0xEB, 0x61, 0x00, 0x00},
    {0xEB, 0x00, 0x61, 0x00, 0x00},
    {0x61, 0xEB, 0x00, 0x00, 0x00},
    {0x00, 0x61, 0xEB, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x61, 0xEB}
  };

  for (size_t i = 0; i < 8; ++i) {
    bool match = true;
    for (int j = 0; j < 5; ++j) {
      if ((frame[j + 1] & ~0x10) != olPatterns[i][j]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

void parseExtendedFrame() {
  if (frameIndex < 18) return;

  uint8_t* frame = frameBuffer;
  latestReading.raw = "";
  latestReading.rawHex = "";
  latestReading.label = "";
  latestReading.freq = "";
  latestReading.freqUnit = "";
  latestReading.unit = "--";
  latestReading.mode = "UNKNOWN";
  latestReading.value = NAN;

  if (isExtendedOLFrame(frame)) {
    latestReading.mode = "OL";
    latestReading.unit = "--";
    latestReading.raw = "OL";
    readingDirty = true;
    return;
  }

  String value = decodeExtendedFrameToString(frame, 18);
  String mode = decodeExtendedMode(frame);
  String unit = decodeExtendedUnit(frame);

  if (value.length() > 0 && value != "") {
    float rawValue = value.toFloat();
    latestReading.value = rawValue;
  }

  latestReading.unit = unit;
  latestReading.mode = mode;

  if ((frame[10] & 0x40) > 0) {
    latestReading.mode = "CONT";
    latestReading.unit = "Ω";
  } else if ((frame[10] & 0x80) > 0) {
    latestReading.mode = "DIODE";
    latestReading.unit = "V";
  } else if (unit.startsWith("V") || unit.startsWith("mV")) {
    if ((frame[11] & 0x40) > 0) latestReading.mode = "VAC";
    else if ((frame[11] & 0x10) > 0) latestReading.mode = "VDC";
  } else if (unit.startsWith("A") || unit.startsWith("mA")) {
    if ((frame[11] & 0x40) > 0) latestReading.mode = "AAC";
    else if ((frame[11] & 0x10) > 0) latestReading.mode = "ADC";
  }

  if ((frame[10] & 0x02) > 0) latestReading.freqUnit = "HZ";
  else if ((frame[10] & 0x03) > 0) latestReading.freqUnit = "kHz";

  latestReading.label = latestReading.mode;
  readingDirty = true;
}

void classifyReading(const String& text) {
  String line = trimWhitespace(text);
  if (line.length() == 0) {
    return;
  }

  String lower = line;
  lower.toLowerCase();

  String numText = extractFirstFloat(line);
  float parsedValue = NAN;
  if (numText.length() > 0) {
    parsedValue = numText.toFloat();
  }

  latestReading.raw = line;
  latestReading.ts = millis();
  latestReading.value = parsedValue;
  latestReading.mode = "UNKNOWN";
  latestReading.unit = "--";

  // Special meter values.
  if (lower.indexOf("ol") >= 0 || lower.indexOf("open") >= 0 || lower.indexOf("over") >= 0) {
    latestReading.mode = "OVER";
    latestReading.unit = "--";
    latestReading.value = NAN;
    return;
  }

  if (lower.indexOf("beep") >= 0 || lower.indexOf("buzz") >= 0 || lower.indexOf("continuity") >= 0 || lower.indexOf("short") >= 0) {
    latestReading.mode = "CONT";
    latestReading.unit = "Ω";
    latestReading.value = 0.0f;
    return;
  }

  if (lower.indexOf("diode") >= 0 || lower.indexOf("diod") >= 0) {
    latestReading.mode = "DIODE";
    latestReading.unit = "V";
  } else if (lower.indexOf("ohm") >= 0 || lower.indexOf("resis") >= 0 || lower.indexOf("ohms") >= 0 || lower.indexOf("omega") >= 0 || lower.indexOf("kohm") >= 0 || lower.indexOf("mohm") >= 0) {
    latestReading.mode = "OHM";
    if (lower.indexOf("mohm") >= 0 || lower.indexOf("meg") >= 0) {
      latestReading.unit = "MΩ";
    } else if (lower.indexOf("kohm") >= 0 || lower.indexOf("k") >= 0) {
      latestReading.unit = "kΩ";
    } else {
      latestReading.unit = "Ω";
    }
  } else if (lower.indexOf("hz") >= 0 || lower.indexOf("khz") >= 0 || lower.indexOf("mhz") >= 0 || lower.indexOf("frequency") >= 0) {
    latestReading.mode = "Hz";
    latestReading.unit = "Hz";
  } else if (lower.indexOf("cap") >= 0 || lower.indexOf("nf") >= 0 || lower.indexOf("uf") >= 0 || (lower.indexOf("c") >= 0 && lower.indexOf("f") >= 0)) {
    latestReading.mode = "CAP";
    latestReading.unit = "nF";
  } else if (lower.indexOf("temp") >= 0 || lower.indexOf("celsius") >= 0 || lower.indexOf("°c") >= 0 || lower.indexOf("deg") >= 0) {
    latestReading.mode = "TEMP";
    latestReading.unit = "°C";
  } else if (lower.indexOf("vac") >= 0 || lower.indexOf("acv") >= 0 || lower.indexOf("voltage ac") >= 0 || lower.indexOf("ac voltage") >= 0 || (lower.indexOf("v") >= 0 && lower.indexOf("ac") >= 0)) {
    latestReading.mode = "VAC";
    latestReading.unit = (lower.indexOf("mv") >= 0) ? "mV" : "V";
  } else if (lower.indexOf("vdc") >= 0 || lower.indexOf("dcv") >= 0 || lower.indexOf("voltage dc") >= 0 || lower.indexOf("dc voltage") >= 0 || (lower.indexOf("v") >= 0 && lower.indexOf("dc") >= 0)) {
    latestReading.mode = "VDC";
    latestReading.unit = (lower.indexOf("mv") >= 0) ? "mV" : "V";
  } else if (lower.indexOf("adc") >= 0 || lower.indexOf("dc a") >= 0 || lower.indexOf("cur dc") >= 0 || lower.indexOf("current dc") >= 0 || (lower.indexOf("a") >= 0 && lower.indexOf("dc") >= 0)) {
    latestReading.mode = "ADC";
    latestReading.unit = (lower.indexOf("ma") >= 0) ? "mA" : "A";
  } else if (lower.indexOf("aac") >= 0 || lower.indexOf("ac a") >= 0 || lower.indexOf("cur ac") >= 0 || lower.indexOf("current ac") >= 0 || (lower.indexOf("a") >= 0 && lower.indexOf("ac") >= 0)) {
    latestReading.mode = "AAC";
    latestReading.unit = (lower.indexOf("ma") >= 0) ? "mA" : "A";
  } else if (lower.indexOf("mv") >= 0 && lower.indexOf("v") >= 0) {
    latestReading.mode = "VDC";
    latestReading.unit = "mV";
  } else if (lower.indexOf("ma") >= 0 && lower.indexOf("a") >= 0) {
    latestReading.mode = "ADC";
    latestReading.unit = "mA";
  } else if (lower.indexOf("v") >= 0 || lower.indexOf("volt") >= 0) {
    latestReading.mode = "VDC";
    latestReading.unit = "V";
  } else if (lower.indexOf("a") >= 0) {
    latestReading.mode = "ADC";
    latestReading.unit = "A";
  }

  if (latestReading.mode == "UNKNOWN" && !isnan(latestReading.value)) {
    if (lower.indexOf("mv") >= 0) {
      latestReading.mode = "VDC";
      latestReading.unit = "mV";
    } else if (lower.indexOf("v") >= 0) {
      latestReading.mode = "VDC";
      latestReading.unit = "V";
    } else if (lower.indexOf("ma") >= 0) {
      latestReading.mode = "ADC";
      latestReading.unit = "mA";
    } else if (lower.indexOf("a") >= 0) {
      latestReading.mode = "ADC";
      latestReading.unit = "A";
    } else if (lower.indexOf("ohm") >= 0 || lower.indexOf("omega") >= 0) {
      latestReading.mode = "OHM";
      latestReading.unit = "Ω";
    }
  }

  if (isnan(latestReading.value)) {
    latestReading.mode = "UNKNOWN";
    latestReading.unit = "--";
  }

  readingDirty = true;
}

void processSerialBuffer() {
  while (Serial1.available()) {
    uint8_t c = static_cast<uint8_t>(Serial1.read());

    if (c == 0xFF) {
      frameIndex = 0;
      memset(frameBuffer, 0, sizeof(frameBuffer));
      frameBuffer[frameIndex++] = c;
      continue;
    }

    if (frameIndex > 0 && frameIndex < 18) {
      frameBuffer[frameIndex++] = c;
      if (frameIndex == 18) {
        recordExtendedFrameHistory(frameBuffer, frameIndex);
        parseExtendedFrame();
        latestReading.ts = millis();
        readingDirty = true;
        frameIndex = 0;
      }
      continue;
    }

    if (frameIndex == 0) {
      if (c == '\r' || c == '\n') {
        if (serialBuffer.length() > 0) {
          recordUartLine(serialBuffer);
          classifyReading(serialBuffer);
          readingDirty = true;
          serialBuffer = "";
        }
        continue;
      }

      if (c >= 0x20 && c < 0x7F) {
        serialBuffer += static_cast<char>(c);
      }

      if (serialBuffer.length() > 256) {
        serialBuffer = "";
      }
    }
  }
}

const char HTML_PAGE[] PROGMEM = R"rawl(
<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ZOYI ZT-703S</title>
  <style>
    :root {
      --bg-1: #06140e;
      --bg-2: #010a07;
      --panel: rgba(7, 18, 13, 0.96);
      --panel-2: rgba(3, 8, 6, 0.98);
      --line: rgba(102, 255, 180, 0.42);
      --line-soft: rgba(132, 255, 198, 0.18);
      --green: #78ffb6;
      --green-strong: #9bf7c7;
      --green-2: #d9ffe8;
      --muted: #9ec7b0;
      --text: #effff8;
      --shadow: none;
      --btn-bg: rgba(10, 19, 14, 0.9);
      --btn-border: rgba(120, 255, 182, 0.28);
      --screen-bg: linear-gradient(180deg, rgba(7, 17, 12, 0.95), rgba(3, 9, 6, 0.96));
      --screen-glow: 0 0 18px rgba(94,255,170,0.22);
    }
    body[data-theme="light"] {
      --bg-1: #dce6e0;
      --bg-2: #bfcfc5;
      --panel: #ffffff;
      --panel-2: #edf3ef;
      --line: #8aa898;
      --line-soft: #bdd0c4;
      --green: #0b5c37;
      --green-strong: #084c2e;
      --green-2: #123f2c;
      --muted: #40594b;
      --text: #10251a;
      --shadow: none;
      --btn-bg: rgba(255, 255, 255, 0.86);
      --btn-border: rgba(21, 80, 49, 0.28);
      --screen-bg: #ffffff;
      --screen-glow: none;
    }
    body[data-theme="light"] .header { background: rgba(248, 252, 248, 0.9); }
    body[data-theme="light"] .panel { background: linear-gradient(180deg, rgba(255,255,255,0.97), rgba(238,245,239,0.98)); }
    body[data-theme="light"] .canvas-wrap { background: #f4f8f0; }
    body[data-theme="light"] .chart-canvas { background: linear-gradient(180deg, #fbfdf9, #e9f0e4); }
    body[data-theme="light"] .led-screen { border-color: #2c8055; box-shadow: none; }
    body[data-theme="light"] .led-screen::before { background: none; }
    body[data-theme="light"] .seven-display { --segment-off: rgba(11, 92, 55, 0.14); --segment-on: #0b5c37; }
    * { box-sizing: border-box; }
    html, body {
      margin: 0;
      min-height: 100%;
      background: linear-gradient(180deg, var(--bg-1), var(--bg-2));
      color: var(--text);
      font-family: "Segoe UI", Arial, sans-serif;
    }
    body {
      min-height: 100dvh;
      padding: 18px;
    }
    .shell {
      width: min(980px, 100%);
      border: 1px solid var(--line);
      border-radius: 0;
      background: linear-gradient(180deg, var(--panel), var(--panel-2));
      box-shadow: inset 0 0 20px rgba(88,255,170,0.05), 0 0 22px rgba(80,255,170,0.08);
      overflow: hidden;
      margin: 0 auto;
    }
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding: 12px 18px;
      border-bottom: 1px solid var(--line);
      background: rgba(8, 15, 11, 0.92);
      letter-spacing: 0.12em;
      text-transform: uppercase;
      font-size: 11px;
    }
    .brand { color: var(--green); font-weight: 700; }
    .toolbar { display: flex; flex: 0 0 auto; }
    .theme-toggle {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      border: 1px solid var(--btn-border);
      background: var(--btn-bg);
      color: var(--green-2);
      padding: 5px 7px;
      border-radius: 0;
      cursor: pointer;
      user-select: none;
      transition: 0.2s ease;
    }
    .theme-toggle input {
      position: absolute;
      opacity: 0;
      pointer-events: none;
    }
    .theme-toggle .toggle-icon {
      font-size: 14px;
      line-height: 1;
      opacity: 0.7;
    }
    .theme-toggle .slider {
      position: relative;
      width: 42px;
      height: 22px;
      background: rgba(136, 255, 194, 0.12);
      border: 1px solid var(--btn-border);
      border-radius: 0;
      display: inline-block;
      transition: 0.2s ease;
    }
    .theme-toggle .slider::before {
      content: "";
      position: absolute;
      width: 16px;
      height: 16px;
      left: 2px;
      top: 2px;
      background: var(--green-strong);
      box-shadow: none;
      border-radius: 0;
      transition: 0.2s ease;
    }
    .theme-toggle input:checked + .slider {
      background: rgba(130, 170, 255, 0.18);
      border-color: rgba(160, 200, 255, 0.5);
    }
    .theme-toggle input:checked + .slider::before {
      transform: translateX(20px);
      background: #dfeeff;
      box-shadow: none;
    }
    .mode-pill {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 7px 12px;
      border-radius: 0;
      border: 1px solid var(--line);
      background: rgba(18, 31, 22, 0.9);
      color: var(--green-2);
      min-width: 86px;
      font-family: "Consolas", "Courier New", monospace;
      letter-spacing: 0.12em;
      text-transform: uppercase;
    }
    .status-danger { color: #ff2020 !important; }
    .status-warn { color: #ffdc6a !important; }
    .status-ok { color: #7af4b1 !important; }
    .status { color: var(--green-2); }
    .content { padding: 16px; }
    .layout {
      display: grid;
      grid-template-columns: 1.2fr 0.8fr;
      gap: 16px;
      align-items: start;
    }
    .panel {
      border: 1px solid var(--line);
      border-radius: 0;
      background: linear-gradient(180deg, rgba(9, 18, 13, 0.96), rgba(5, 10, 7, 0.98));
      box-shadow: inset 0 0 12px rgba(90,255,170,0.03);
      padding: 10px;
    }
    .display-panel {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .display-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      font-family: "Consolas", "Courier New", monospace;
      font-size: 11px;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: var(--muted);
    }
    .led-screen {
      position: relative;
      background: var(--screen-bg);
      border: 1px solid rgba(104, 255, 172, 0.8);
      border-radius: 0;
      box-shadow: 0 0 22px rgba(50, 255, 160, 0.08), inset 0 0 16px rgba(65,255,150,0.04);
      padding: 8px 10px 6px;
      min-height: 84px;
      display: flex;
      flex-direction: column;
      justify-content: center;
      overflow: hidden;
    }
    .led-screen::before {
      content: "";
      position: absolute;
      inset: 0;
      background: linear-gradient(180deg, rgba(128,255,191,0.02), transparent 30%, transparent 70%, rgba(128,255,191,0.02));
      pointer-events: none;
    }
    .led-value {
      position: relative;
      z-index: 1;
      display: flex;
      align-items: end;
      justify-content: center;
      gap: 10px;
      font-family: "DSEG7 Classic", "Digital-7 Mono", "Courier New", monospace;
      font-weight: 700;
      letter-spacing: 0.06em;
      color: var(--green-strong);
      text-shadow: none;
      line-height: 1;
    }
    .sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0; }
    .seven-display { display: flex; align-items: center; gap: 4px; min-height: 84px; --segment-off: rgba(63, 97, 69, 0.14); --segment-on: #e8fff3; }
    .seven-digit { position: relative; width: clamp(26px, 4.6vw, 47px); height: clamp(52px, 8.4vw, 88px); }
    .seven-segment { position: absolute; border-radius: 4px; background: var(--segment-off); box-shadow: none; }
    .seven-segment.active { background: var(--segment-on); box-shadow: none; }
    .seven-segment.a, .seven-segment.g, .seven-segment.d { left: 15%; width: 70%; height: 7%; }
    .seven-segment.a { top: 1%; } .seven-segment.g { top: 46.5%; } .seven-segment.d { bottom: 1%; }
    .seven-segment.b, .seven-segment.c, .seven-segment.e, .seven-segment.f { width: 9%; height: 42%; }
    .seven-segment.b { right: 1%; top: 5%; } .seven-segment.c { right: 1%; bottom: 5%; }
    .seven-segment.e { left: 1%; bottom: 5%; } .seven-segment.f { left: 1%; top: 5%; }
    .seven-digit.decimal { width: 9px; height: 88px; }
    .seven-digit.decimal::after { content: ""; position: absolute; bottom: 2%; width: 8px; height: 8px; border-radius: 0; background: var(--segment-on); box-shadow: none; }
    .seven-digit.minus .seven-segment { display: none; } .seven-digit.minus .seven-segment.g { display: block; top: 46.5%; }
    .led-screen[data-state="warn"] .seven-display { --segment-on: #ffd75a; }
    .led-screen[data-state="contact"] .seven-display { --segment-on: #ff2020; }
    .led-value #unitText {
      font-size: clamp(0.9rem, 1.6vw, 1.35rem);
      margin-bottom: 0.2rem;
      color: var(--green-2);
      letter-spacing: 0.08em;
    }
    .raw-reading {
      position: relative;
      z-index: 1;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      min-width: 0;
      margin-top: 4px;
      padding-top: 5px;
      border-top: 1px solid rgba(126, 255, 183, 0.18);
      color: #9fd5b2;
      font: 0.72rem "Consolas", "Courier New", monospace;
      letter-spacing: 0.06em;
    }
    .raw-reading .raw-label { flex: 0 0 auto; color: var(--muted); }
    .raw-seven-display { flex: 1 1 auto; justify-content: flex-end; gap: 2px; min-width: 0; min-height: 42px; }
    .raw-seven-display .seven-digit { width: clamp(15px, 2.8vw, 27px); height: clamp(31px, 5vw, 50px); }
    .raw-seven-display .seven-digit.decimal { width: 5px; height: clamp(31px, 5vw, 50px); }
    .raw-seven-display .seven-digit.decimal::after { width: 5px; height: 5px; }
    .raw-seven-display .seven-digit.space { width: 5px; }
    .uart-history {
      min-height: 92px;
      margin: 0;
      padding: 7px 8px;
      border: 1px solid var(--line-soft);
      background: rgba(3, 10, 6, 0.66);
      color: var(--muted);
      font: 0.68rem/1.45 "Consolas", "Courier New", monospace;
      overflow: hidden;
      white-space: pre-wrap;
      word-break: break-all;
    }
    body[data-theme="light"] .uart-history { background: #f5f9f5; }
    .metrics-strip {
      display: none;
    }
    .metric label {
      display: none;
    }
    .metric strong {
      font-family: "Consolas", "Courier New", monospace;
      font-size: 0.8rem;
      letter-spacing: 0.08em;
      color: var(--green-2);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .telemetry-panel {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .panel-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 8px;
      font-size: 11px;
      font-family: "Consolas", "Courier New", monospace;
      letter-spacing: 0.14em;
      text-transform: uppercase;
      color: var(--muted);
    }
    .canvas-wrap {
      position: relative;
      border: 1px solid var(--line-soft);
      border-radius: 0;
      background: rgba(6, 11, 8, 0.9);
      overflow: hidden;
      min-height: 180px;
    }
    .chart-canvas {
      display: block;
      width: 100%;
      height: 220px;
      background: linear-gradient(180deg, rgba(8,15,11,0.96), rgba(4,9,7,0.96));
    }
    .scope-controls { display: flex; gap: 8px; }
    .scope-button {
      border: 1px solid var(--btn-border);
      background: var(--btn-bg);
      color: var(--green-strong);
      cursor: pointer;
      font: inherit;
      font-size: 0.72rem;
      letter-spacing: 0.08em;
      padding: 5px 8px;
      text-transform: uppercase;
    }
    .scope-button:hover, .scope-button.active { background: rgba(97, 255, 169, 0.16); }
    .scope-meta { color: var(--muted); font: 0.72rem Consolas, monospace; letter-spacing: 0.04em; }
    .meter-visual {
      display: none !important;
    }
    .terminal-log {
      display: none !important;
    }
    @media (max-width: 900px) {
      body { padding: 12px; }
      .layout { grid-template-columns: 1fr; }
      .header {
        padding: 12px 14px;
      }
      .content { padding: 12px; }
      .panel { padding: 12px; }
      .led-screen { min-height: 170px; }
    }
    @media (max-width: 480px) {
      .metrics-strip { grid-template-columns: 1fr; }
      .led-value #valueText { font-size: 2.8rem; }
      .display-header, .panel-header { letter-spacing: 0.08em; }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="header">
      <div class="brand">ZOYI ZT-703S</div>
      <div class="status" id="status">BOOT</div>
      <div class="toolbar">
        <label class="theme-toggle" aria-label="Theme toggle">
          <span class="toggle-icon">☾</span>
          <input type="checkbox" id="themeToggle">
          <span class="slider"></span>
          <span class="toggle-icon">☼</span>
        </label>
      </div>
    </div>

    <div class="content">
      <div class="layout">
        <div class="panel display-panel">
          <div class="display-header">
            <span>Meter</span>
            <span id="modeLabel">WAIT</span>
          </div>

          <div class="led-screen">
            <div class="led-value">
              <div class="seven-display" id="sevenDisplay" aria-hidden="true"></div>
              <span class="sr-only" id="valueText">--</span>
              <span id="unitText">--</span>
            </div>
            <div class="raw-reading" aria-label="Original meter reading">
              <span class="raw-label">RAW</span>
              <div class="seven-display raw-seven-display" id="rawSevenDisplay" aria-hidden="true"></div>
              <span class="sr-only" id="rawValueText">--</span>
            </div>
          </div>
          <div class="display-header"><span>UART LAST 5</span></div>
          <pre class="uart-history" id="uartHistory">WAITING FOR UART DATA</pre>
        </div>

        <div class="panel telemetry-panel">
          <div class="panel-header">
            <span>RANGE GRAPH</span>
            <div class="scope-controls">
              <button class="scope-button" id="scopePause" type="button">Pause</button>
              <button class="scope-button" id="scopeClear" type="button">Clear</button>
            </div>
          </div>
          <div class="canvas-wrap">
            <canvas class="chart-canvas" id="chartCanvas" width="420" height="220"></canvas>
          </div>
          <div class="scope-meta" id="chartStatus">WAITING FOR METER DATA</div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const modeLabel = document.getElementById('modeLabel');
    const valueText = document.getElementById('valueText');
    const sevenDisplay = document.getElementById('sevenDisplay');
    const unitText = document.getElementById('unitText');
    const rawValueText = document.getElementById('rawValueText');
    const rawSevenDisplay = document.getElementById('rawSevenDisplay');
    const uartHistoryEl = document.getElementById('uartHistory');
    const ledScreen = document.querySelector('.led-screen');
    const logBox = document.getElementById('logBox');
    const statusEl = document.getElementById('status');
    const chartStatus = document.getElementById('chartStatus');
    const themeToggle = document.getElementById('themeToggle');
    const chartCanvas = document.getElementById('chartCanvas');
    const scopePause = document.getElementById('scopePause');
    const scopeClear = document.getElementById('scopeClear');
    const history = [];
    let pendingReading = null;
    let renderQueued = false;
    let lastChartSampleAt = 0;
    let scopePaused = false;
    let scopePausedAt = 0;
    let scopeFamily = '';
    let activeRange = 1;
    let lastSevenValue = '';
    let lastRawSevenValue = '';
    let lastUartSequence = -1;
    let uartRequestInFlight = false;
    const CHART_SAMPLE_INTERVAL_MS = 125;
    const UART_HISTORY_REFRESH_MS = 500;
    const SCOPE_WINDOW_MS = 6000;

    function applyTheme(theme) {
      const selectedTheme = theme === 'light' ? 'light' : 'dark';
      document.body.setAttribute('data-theme', selectedTheme);
      localStorage.setItem('zoyiTheme', selectedTheme);
      if (themeToggle) {
        themeToggle.checked = selectedTheme === 'light';
      }
    }

    function formatNumber(value) {
      if (value === '--') return '--';
      const n = Number(value);
      if (!Number.isFinite(n)) return '--';
      const abs = Math.abs(n);
      const formatted = abs >= 1000 ? n.toFixed(1) : n.toFixed(3);
      const compact = formatted.replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1');
      return compact === '-0' ? '0' : compact;
    }

    function formatScaledUnit(value, unit) {
      if (value === '--' || !Number.isFinite(Number(value))) {
        return { value: '--', unit: unit || '--' };
      }
      let numeric = Number(value);
      let suffix = unit || '';
      const rx = ['MΩ', 'kΩ', 'Ω', 'mV', 'V', 'mA', 'A', 'uF', 'nF', 'Hz', 'kHz', '°C'];
      if (!rx.includes(suffix) && suffix !== '--' && suffix !== '') {
        return { value: formatNumber(numeric), unit: suffix };
      }
      if (suffix === 'MΩ' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'MΩ' };
      }
      if (suffix === 'kΩ' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'MΩ' };
      }
      if (suffix === 'Ω' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'kΩ' };
      }
      if (suffix === 'kΩ' && Math.abs(numeric) < 1) {
        return { value: formatNumber(numeric * 1000), unit: 'Ω' };
      }
      if (suffix === 'mV' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'V' };
      }
      if (suffix === 'V' && Math.abs(numeric) < 1) {
        return { value: formatNumber(numeric * 1000), unit: 'mV' };
      }
      if (suffix === 'mA' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'A' };
      }
      if (suffix === 'A' && Math.abs(numeric) < 1) {
        return { value: formatNumber(numeric * 1000), unit: 'mA' };
      }
      if (suffix === 'Hz' && Math.abs(numeric) >= 1000) {
        return { value: formatNumber(numeric / 1000), unit: 'kHz' };
      }
      if (suffix === 'kHz' && Math.abs(numeric) < 1) {
        return { value: formatNumber(numeric * 1000), unit: 'Hz' };
      }
      return { value: formatNumber(numeric), unit: suffix };
    }

    function formatRawReading(value, unit) {
      const numeric = Number(value);
      if (!Number.isFinite(numeric)) return '--';
      const absolute = Math.abs(numeric);
      const decimals = absolute >= 100000 ? 0 : absolute >= 1000 ? 1 : absolute >= 1 ? 4 : 6;
      let text = numeric.toFixed(decimals);
      if (text.includes('.')) text = text.replace(/0+$/, '').replace(/\.$/, '');
      return `${text} ${unit || ''}`.trim();
    }

    function rawLedText(value, unit) {
      return formatRawReading(value, unit)
        .replaceAll('\u03a9', 'O')
        .replaceAll('\u00b0', '');
    }

    function valueToBaseUnit(value, unit) {
      if (!Number.isFinite(Number(value))) return 0;
      const numeric = Number(value);
      const suffix = String(unit || '').trim();
      if (suffix === 'MΩ') return numeric * 1000000;
      if (suffix === 'kΩ') return numeric * 1000;
      if (suffix === 'Ω') return numeric;
      if (suffix === 'V') return numeric;
      if (suffix === 'mV') return numeric / 1000;
      if (suffix === 'A') return numeric;
      if (suffix === 'mA') return numeric / 1000;
      if (suffix === 'uF') return numeric * 1e-6;
      if (suffix === 'nF') return numeric * 1e-9;
      if (suffix === 'Hz') return numeric;
      if (suffix === 'kHz') return numeric * 1000;
      return numeric;
    }

    function measurementFamily(unit, mode) {
      const suffix = String(unit || '');
      const modeName = String(mode || '').toUpperCase();
      if (suffix === 'V' || suffix === 'mV' || modeName.includes('VDC') || modeName.includes('VAC') || modeName === 'DIODE') return 'voltage';
      if (suffix === 'A' || suffix === 'mA' || modeName.includes('ADC') || modeName.includes('AAC')) return 'current';
      if (suffix.includes('Î©') || modeName === 'OHM' || modeName === 'CONT') return 'resistance';
      if (suffix === 'nF' || suffix === 'uF' || suffix === 'mF') return 'capacitance';
      if (suffix === 'Hz' || suffix === 'kHz') return 'frequency';
      if (suffix.includes('Â°C')) return 'temperature';
      return suffix || 'unknown';
    }

    function scopeUnit(family, peak) {
      if (family === 'voltage') return peak < 1 ? { multiplier: 1000, label: 'mV' } : { multiplier: 1, label: 'V' };
      if (family === 'current') return peak < 1 ? { multiplier: 1000, label: 'mA' } : { multiplier: 1, label: 'A' };
      if (family === 'resistance') return peak >= 1000000 ? { multiplier: 1e-6, label: 'MÎ©' } : peak >= 1000 ? { multiplier: 1e-3, label: 'kÎ©' } : { multiplier: 1, label: 'Î©' };
      if (family === 'capacitance') return peak < 1e-6 ? { multiplier: 1e9, label: 'nF' } : peak < 1e-3 ? { multiplier: 1e6, label: 'uF' } : { multiplier: 1e3, label: 'mF' };
      if (family === 'frequency') return peak >= 1000 ? { multiplier: 1e-3, label: 'kHz' } : { multiplier: 1, label: 'Hz' };
      return { multiplier: 1, label: family === 'temperature' ? 'Â°C' : '' };
    }

    function formatScopeValue(value) {
      const abs = Math.abs(value);
      if (abs >= 100) return value.toFixed(0);
      if (abs >= 10) return value.toFixed(1);
      if (abs >= 1) return value.toFixed(2);
      if (abs >= 0.01) return value.toFixed(3);
      return value.toFixed(4);
    }

    function rangeForFamily(family, value) {
      const ranges = {
        voltage: [0.6, 6, 60, 600, 1000],
        current: [0.06, 0.6, 6, 10],
        resistance: [600, 6000, 60000, 600000, 6000000, 60000000],
        capacitance: [1e-9, 1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3],
        frequency: [60, 600, 6000, 60000, 600000],
        temperature: [60, 200, 600]
      };
      const candidates = ranges[family] || [1, 10, 100, 1000];
      return candidates.find(range => value <= range) || candidates[candidates.length - 1];
    }

    function renderSegments(value, target, previousValue, maximumLength) {
      const text = String(value || '--').slice(-maximumLength);
      if (text === previousValue) return previousValue;
      const segments = {
        '0': 'ab cdef'.replace(' ', ''), '1': 'bc', '2': 'abdeg', '3': 'abcdg', '4': 'bcfg',
        '5': 'acdfg', '6': 'acdefg', '7': 'abc', '8': 'abcdefg', '9': 'abcdfg', '-': 'g',
        'A': 'abcefg', 'b': 'cdefg', 'C': 'adef', 'd': 'bcdeg', 'E': 'adefg', 'F': 'aefg',
        'H': 'bcefg', 'I': 'bc', 'L': 'def', 'm': 'ceg', 'n': 'ceg', 'o': 'cdeg', 'O': 'abcdef',
        'P': 'abefg', 'r': 'eg', 't': 'defg', 'U': 'bcdef', 'V': 'bcdef', 'u': 'cde'
      };
      const fragment = document.createDocumentFragment();
      text.split('').forEach(character => {
        const digit = document.createElement('span');
        if (character === ' ') {
          digit.className = 'seven-digit space';
        } else if (character === '.') {
          digit.className = 'seven-digit decimal';
        } else {
          digit.className = `seven-digit${character === '-' ? ' minus' : ''}`;
          'abcdefg'.split('').forEach(name => {
            const segment = document.createElement('i');
            segment.className = `seven-segment ${name}${(segments[character] || '').includes(name) ? ' active' : ''}`;
            digit.appendChild(segment);
          });
        }
        fragment.appendChild(digit);
      });
      target.replaceChildren(fragment);
      return text;
    }

    function renderSevenSegment(value) {
      lastSevenValue = renderSegments(value, sevenDisplay, lastSevenValue, 8);
    }

    function renderRawSevenSegment(value) {
      lastRawSevenValue = renderSegments(value, rawSevenDisplay, lastRawSevenValue, 12);
    }

    function updateDisplayState(value, unit, mode) {
      const family = measurementFamily(unit, mode);
      const baseValue = Math.abs(valueToBaseUnit(value, unit));
      const displayRange = rangeForFamily(family, baseValue);
      const ratio = Math.min(baseValue / Math.max(displayRange, 0.000001), 1);
      const state = String(mode).toUpperCase() === 'CONT' ? 'contact' : ratio >= 0.8 ? 'warn' : 'normal';
      ledScreen.dataset.state = state;
      unitText.style.color = state === 'contact' ? '#ff2020' : state === 'warn' ? '#c88900' : '';
    }

    function refreshUartHistory(sequence) {
      if (uartRequestInFlight || sequence === lastUartSequence) return;
      const now = performance.now();
      if (now - lastUartHistoryFetchAt < UART_HISTORY_REFRESH_MS) return;
      lastUartHistoryFetchAt = now;
      uartRequestInFlight = true;
      fetch('/api/uart')
        .then(response => response.json())
        .then((data) => {
          const lines = Array.isArray(data.lines) ? data.lines : [];
          uartHistoryEl.textContent = lines.length ? lines.join('\n') : 'WAITING FOR UART DATA';
          lastUartSequence = Number(data.seq);
        })
        .catch(() => {})
        .finally(() => { uartRequestInFlight = false; });
    }

    function appendLog(line) {
      if (!logBox) return;
      const now = new Date().toLocaleTimeString();
      const msg = `${now}  ${line}`;
      const current = logBox.textContent || '';
      logBox.textContent = `${msg}\n${current}`.split('\n').slice(0, 36).join('\n');
    }

    function render(data) {
      const mode = data.mode || 'UNKNOWN';
      const val = (typeof data.value === 'number' && Number.isFinite(data.value)) ? data.value : '--';
      const unit = data.unit || '--';
      const display = formatScaledUnit(val, unit);
      if (mode.toUpperCase() === 'CONT' || mode.toUpperCase() === 'DIODE') {
        if (!Number.isFinite(Number(display.value))) {
          valueText.textContent = '0';
        }
      }
      modeLabel.textContent = mode;
      valueText.textContent = display.value;
      unitText.textContent = display.unit;
      rawValueText.textContent = formatRawReading(val, unit);
      renderRawSevenSegment(rawLedText(val, unit));
      if (Number.isFinite(Number(data.uartSeq))) refreshUartHistory(Number(data.uartSeq));
      statusEl.textContent = 'Live';

      const meterState = mode.toUpperCase();
      if (meterState === 'DIODE' || meterState === 'CONT' || meterState === 'OHM') {
        valueText.style.color = meterState === 'DIODE' ? '#ffdc6a' : meterState === 'CONT' ? '#ff2020' : '#ffb366';
        unitText.style.color = valueText.style.color;
        if (meterState === 'CONT') {
          playBeep();
        }
      } else {
        valueText.style.color = '';
        unitText.style.color = '';
      }
      if ((meterState === 'CONT' || meterState === 'DIODE') && Number.isFinite(Number(display.value))) {
        const safeValue = Math.abs(Number(display.value));
        if (safeValue > 5000) {
          valueText.textContent = 'OL';
          unitText.textContent = '--';
        }
      }
      renderSevenSegment(valueText.textContent);
      if (Number.isFinite(Number(val))) {
        updateDisplayState(Number(val), unit, mode);
      } else {
        ledScreen.dataset.state = 'normal';
      }
    }

    function queueReading(data) {
      pendingReading = data;
      if (renderQueued) return;

      renderQueued = true;
      requestAnimationFrame((now) => {
        renderQueued = false;
        const newestReading = pendingReading;
        pendingReading = null;
        if (!newestReading) return;

        render(newestReading);
        if (now - lastChartSampleAt >= CHART_SAMPLE_INTERVAL_MS) {
          lastChartSampleAt = now;
          renderTelemetry(newestReading);
        }
      });
    }

    themeToggle.addEventListener('change', () => {
      applyTheme(themeToggle.checked ? 'light' : 'dark');
    });

    const savedTheme = localStorage.getItem('zoyiTheme') || 'dark';
    applyTheme(savedTheme);

    scopePause.addEventListener('click', () => {
      scopePaused = !scopePaused;
      scopePausedAt = Date.now();
      scopePause.textContent = scopePaused ? 'Resume' : 'Pause';
      scopePause.classList.toggle('active', scopePaused);
      drawOscilloscope();
    });

    scopeClear.addEventListener('click', () => {
      history.length = 0;
      chartStatus.textContent = 'CLEARED · WAITING FOR METER DATA';
      chartCanvas.getContext('2d').clearRect(0, 0, chartCanvas.width, chartCanvas.height);
    });

    const resizeScope = () => {
      const width = Math.max(320, Math.floor(chartCanvas.clientWidth));
      if (width === chartCanvas.width) return;
      chartCanvas.width = width;
      chartCanvas.height = 220;
      if (history.length) drawOscilloscope();
    };
    new ResizeObserver(resizeScope).observe(chartCanvas);
    resizeScope();

    function niceCeil(value) {
      if (!Number.isFinite(value) || value <= 0) return 1;
      const exponent = Math.floor(Math.log10(value));
      const magnitude = 10 ** exponent;
      const normalized = value / magnitude;
      let nice = 1;
      if (normalized <= 1) nice = 1;
      else if (normalized <= 2) nice = 2;
      else if (normalized <= 5) nice = 5;
      else nice = 10;
      return nice * magnitude;
    }

    function drawGrid(ctx, width, height, yMax, timeWindowMs) {
      ctx.strokeStyle = 'rgba(140, 255, 196, 0.14)';
      ctx.lineWidth = 1;
      ctx.fillStyle = 'rgba(165, 255, 209, 0.7)';
      ctx.font = '11px Consolas';

      const leftPad = 44;
      const rightPad = 12;
      const topPad = 16;
      const bottomPad = 26;

      const plotWidth = width - leftPad - rightPad;
      const plotHeight = height - topPad - bottomPad;

      for (let i = 0; i <= 5; i++) {
        const y = topPad + (plotHeight / 5) * i;
        ctx.beginPath();
        ctx.moveTo(leftPad, y);
        ctx.lineTo(width - rightPad, y);
        ctx.stroke();

        const labelValue = formatAxisValue(yMax * (1 - i / 5));
        ctx.fillText(labelValue, 4, y + 4);
      }

      for (let i = 0; i <= 6; i++) {
        const x = leftPad + (plotWidth / 6) * i;
        ctx.beginPath();
        ctx.moveTo(x, topPad);
        ctx.lineTo(x, height - bottomPad);
        ctx.stroke();

        const t = ((i / 6) * timeWindowMs) - timeWindowMs;
        const label = `${(t / 1000).toFixed(1)}s`;
        ctx.fillText(label, x - 12, height - 8);
      }
    }

    function drawTrendChart(ctx, width, height, data, mode, unit) {
      ctx.clearRect(0, 0, width, height);
      const leftPad = 44;
      const rightPad = 12;
      const topPad = 16;
      const bottomPad = 26;
      const plotWidth = width - leftPad - rightPad;
      const plotHeight = height - topPad - bottomPad;

      const entries = data.filter(item => Number.isFinite(Number(item.value)));
      if (!entries.length) return;

      const recent = entries.slice(-80);
      const modeName = (mode || '').toUpperCase();
      const resistanceLike = ['CONT', 'DIODE', 'OHM'].includes(modeName) || (unit || '').includes('Ω');
      const usableValues = recent
        .map(item => Math.abs(valueToBaseUnit(Number(item.value), item.unit)))
        .filter(value => Number.isFinite(value));
      const peak = usableValues.length ? Math.max(...usableValues, 1) : 1;
      let yMax = niceCeil(peak * 1.2);

      if (resistanceLike) {
        if (peak < 1) yMax = 1;
        else if (peak < 10) yMax = 10;
        else if (peak < 100) yMax = 100;
        else if (peak < 1000) yMax = 1000;
        else if (peak < 10000) yMax = 10000;
        else yMax = Math.min(niceCeil(peak * 1.2), 1000000);
      }

      if (Number.isFinite(peak) && peak < 0.1) {
        yMax = 1;
      }

      const timeWindowMs = 6000;
      const startTs = Date.now() - timeWindowMs;
      const visible = recent.filter(item => item.ts >= startTs);
      if (!visible.length) return;

      drawGrid(ctx, width, height, yMax, timeWindowMs);

      ctx.beginPath();
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      const hasSinglePoint = visible.length === 1;
      visible.forEach((point, index) => {
        const x = hasSinglePoint
          ? leftPad + plotWidth * 0.5
          : leftPad + (index / Math.max(visible.length - 1, 1)) * plotWidth;
        const chartValue = Math.abs(valueToBaseUnit(Number(point.value), point.unit));
        const safeValue = Math.min(Math.max(chartValue, 0), yMax);
        const normalized = safeValue > 0 ? safeValue / Math.max(yMax, 0.0001) : 0.08;
        const y = topPad + plotHeight - normalized * plotHeight;
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      if (hasSinglePoint) {
        const lastX = leftPad + plotWidth * 0.5;
        const lastY = topPad + plotHeight - 0.08 * plotHeight;
        ctx.moveTo(lastX - 4, lastY);
        ctx.lineTo(lastX + 4, lastY);
      }
      ctx.strokeStyle = '#f8d55a';
      ctx.lineWidth = 2;
      ctx.shadowColor = 'rgba(248, 213, 90, 0.85)';
      ctx.shadowBlur = 10;
      ctx.stroke();
      ctx.shadowBlur = 0;
    }

    function drawOscilloscope() {
      const ctx = chartCanvas.getContext('2d');
      const width = chartCanvas.width;
      const height = chartCanvas.height;
      const leftPad = 52;
      const rightPad = 12;
      const topPad = 14;
      const bottomPad = 28;
      const plotWidth = width - leftPad - rightPad;
      const plotHeight = height - topPad - bottomPad;
      const now = scopePaused ? scopePausedAt : Date.now();
      const visible = history.filter(point => point.ts >= now - SCOPE_WINDOW_MS && point.ts <= now);
      const isLightTheme = document.body.dataset.theme === 'light';
      const gridColor = isLightTheme ? 'rgba(11, 92, 55, 0.22)' : 'rgba(126, 255, 183, 0.16)';
      const labelColor = isLightTheme ? '#315645' : 'rgba(184, 255, 215, 0.78)';
      const traceColor = isLightTheme ? '#0b6b40' : '#f8d55a';
      const traceGlow = isLightTheme ? 'rgba(11, 107, 64, 0.28)' : 'rgba(248, 213, 90, 0.85)';

      ctx.clearRect(0, 0, width, height);
      if (!visible.length) return;

      const scale = scopeUnit(scopeFamily, activeRange);
      const values = visible.map(point => point.baseValue * scale.multiplier);
      const minimum = Math.min(...values);
      const yMin = Math.min(0, minimum);
      const yMax = activeRange * scale.multiplier;
      const toX = timestamp => leftPad + ((timestamp - (now - SCOPE_WINDOW_MS)) / SCOPE_WINDOW_MS) * plotWidth;
      const toY = value => topPad + (1 - (value - yMin) / Math.max(yMax - yMin, 0.0001)) * plotHeight;

      ctx.strokeStyle = gridColor;
      ctx.fillStyle = labelColor;
      ctx.font = '11px Consolas';
      for (let tick = 0; tick <= 6; tick++) {
        const y = topPad + (plotHeight * tick) / 6;
        const value = yMax - ((yMax - yMin) * tick) / 6;
        ctx.beginPath(); ctx.moveTo(leftPad, y); ctx.lineTo(width - rightPad, y); ctx.stroke();
        ctx.fillText(formatScopeValue(value), 3, y + 4);
      }
      for (let tick = 0; tick <= 6; tick++) {
        const x = leftPad + (plotWidth * tick) / 6;
        ctx.beginPath(); ctx.moveTo(x, topPad); ctx.lineTo(x, height - bottomPad); ctx.stroke();
        const label = tick === 6 ? 'now' : `-${6 - tick}s`;
        ctx.fillText(label, x - 10, height - 8);
      }

      ctx.beginPath();
      visible.forEach((point, index) => {
        const x = toX(point.ts);
        const y = toY(point.baseValue * scale.multiplier);
        if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      });
      ctx.strokeStyle = traceColor;
      ctx.lineWidth = 2;
      ctx.shadowColor = traceGlow;
      ctx.shadowBlur = 8;
      ctx.stroke();
      ctx.shadowBlur = 0;
      const last = visible[visible.length - 1];
      ctx.fillStyle = isLightTheme ? '#075630' : '#fff2a8';
      ctx.beginPath(); ctx.arc(toX(last.ts), toY(last.baseValue * scale.multiplier), 3, 0, Math.PI * 2); ctx.fill();
      chartStatus.textContent = `${scopePaused ? 'PAUSED' : 'LIVE'} · ${scopeFamily.toUpperCase()} · RANGE ${formatScopeValue(yMax)} ${scale.label} · 6s`;
    }

    function renderTelemetry(data) {
      const mode = data.mode || 'UNKNOWN';
      const unit = data.unit || '--';
      const val = Number(data.value);
      if (!Number.isFinite(val) || scopePaused) return;
      const family = measurementFamily(unit, mode);
      if (scopeFamily && scopeFamily !== family) history.length = 0;
      scopeFamily = family;
      const baseValue = valueToBaseUnit(val, unit);
      activeRange = rangeForFamily(family, Math.abs(baseValue));
      history.push({ baseValue, ts: Date.now() });
      if (history.length > 240) history.shift();
      drawOscilloscope();
    }

    const wsUrl = `ws://${location.hostname}:81/`;
    let socket;
    let lastBeepAt = 0;
    let lastUartHistoryFetchAt = -UART_HISTORY_REFRESH_MS;
    let audioUnlocked = false;
    const beepAudio = new Audio('/beep.wav');
    beepAudio.preload = 'auto';
    beepAudio.volume = 0.8;

    function unlockBeep() {
      if (audioUnlocked) return;
      const savedVolume = beepAudio.volume;
      beepAudio.volume = 0;
      const playPromise = beepAudio.play();
      if (!playPromise) return;
      playPromise.then(() => {
        beepAudio.pause();
        beepAudio.currentTime = 0;
        beepAudio.volume = savedVolume;
        audioUnlocked = true;
      }).catch(() => {
        beepAudio.volume = savedVolume;
      });
    }

    ['pointerdown', 'touchstart', 'keydown'].forEach((eventName) => {
      document.addEventListener(eventName, unlockBeep, { once: true });
    });

    function playBeep() {
      const now = performance.now();
      if (now - lastBeepAt < 90) return;
      lastBeepAt = now;
      try {
        beepAudio.pause();
        beepAudio.currentTime = 0;
        beepAudio.play().catch(() => {});
      } catch (error) {
        // ignore audio failures in unsupported browsers
      }
    }

    function connectSocket() {
      socket = new WebSocket(wsUrl);
      socket.onopen = () => {
        statusEl.textContent = 'Connected';
        appendLog('WebSocket connected.');
      };
      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          queueReading(data);
        } catch (error) {
          appendLog('JSON parse error: ' + error);
        }
      };
      socket.onerror = () => {
        statusEl.textContent = 'Socket error';
      };
      socket.onclose = () => {
        statusEl.textContent = 'Reconnecting...';
        setTimeout(connectSocket, 1200);
      };
    }

    connectSocket();

    fetch('/api/status')
      .then(response => response.json())
      .then((data) => {
        queueReading(data);
      })
      .catch(() => {
        appendLog('Initial API fetch failed.');
      });
  </script>
</body>
</html>
)rawl";

void handleRoot() {
  server.send(200, "text/html", HTML_PAGE);
}

void handleStatus() {
  server.send(200, "application/json", statusJson());
}

void handleUartHistory() {
  server.send(200, "application/json", uartHistoryJson());
}

void handleBeep() {
  if (!LittleFS.exists("/beep.wav")) {
    server.send(404, "text/plain", "beep.wav not found");
    return;
  }

  File file = LittleFS.open("/beep.wav", "r");
  if (!file) {
    server.send(500, "text/plain", "Unable to open beep.wav");
    return;
  }

  server.streamFile(file, "audio/wav");
  file.close();
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WS] client #%u connected\n", num);
    {
      String payloadText = statusJson();
      webSocket.sendTXT(num, payloadText.c_str(), payloadText.length());
    }
      break;
    case WStype_TEXT:
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[WS] client #%u disconnected\n", num);
      break;
    default:
      break;
  }
}

void initWiFi() {
    WiFi.setSleep(false);
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false, false);
    WiFi.setAutoReconnect(true);

    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
    if (apOk) {
      Serial.printf("AP ready. SSID=%s IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
      Serial.println("AP start failed.");
    }

    WiFi.setHostname("VOM-703S");
    WiFi.config(LAN_STATIC_IP, LAN_GATEWAY, LAN_SUBNET);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("Connecting to LAN: %s\n", WIFI_SSID);

    for (int attempt = 0; attempt < 60; ++attempt) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("LAN connected. IP=%s\n", WiFi.localIP().toString().c_str());
        return;
      }
      delay(250);
    }

    Serial.println("LAN connect timeout. Keeping AP+STA and retrying in background.");
}

void maintainWiFi() {
  static uint32_t lastAttemptAt = 0;
  if (WiFi.status() == WL_CONNECTED || millis() - lastAttemptAt < 10000) {
    return;
  }

  lastAttemptAt = millis();
  WiFi.reconnect();
  Serial.printf("Retrying LAN connection: %s\n", WIFI_SSID);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(SERIAL_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  // UART TX is intentionally left unused because the meter only transmits data.
  delay(200);

  Serial.println("ESP32-S3 VOM bridge booting...");
  Serial.printf("UART RX=%d TX=%d Baud=%d\n", UART_RX_PIN, UART_TX_PIN, SERIAL_BAUD);

  initWiFi();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted");
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/uart", handleUartHistory);
  server.on("/beep.wav", HTTP_GET, handleBeep);
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  latestReading.mode = "WAITING";
  latestReading.raw = "Waiting for UART stream";
  latestReading.ts = millis();
}

void loop() {
  maintainWiFi();
  processSerialBuffer();
  webSocket.loop();
  server.handleClient();

  if (readingDirty) {
    readingDirty = false;
    broadcastMeter();
  }
}
