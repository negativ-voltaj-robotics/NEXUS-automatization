/*
  SMART BEDROOM 2.0 PHANTOM — Arduino Nano slave
  Board: Arduino Nano ATmega328P, 5V
  Protocol over SoftwareSerial @ 19200 baud:
    ESP -> Nano: PING
    Nano -> ESP: PONG
    ESP -> Nano: SET,<channel 5..16>,<0|1>
    Nano -> ESP: OK,<channel>,<0|1>
    ESP -> Nano: GET
    Nano -> ESP: STATE,<state ch5>,...,<state ch16>
*/
#include <SoftwareSerial.h>

// Nano D10 receives ESP TX; Nano D11 transmits to ESP RX.
// Nano TX (5V) MUST go through a divider/level shifter to ESP RX.
SoftwareSerial espSerial(10, 11); // RX, TX

// Logical channels 5..16 map to D2..D9 and A0..A3.
const uint8_t relayPins[12] = {
  2, 3, 4, 5, 6, 7, 8, 9,
  A0, A1, A2, A3
};
const bool RELAY_ACTIVE_LOW = true;
bool states[12] = {false};
String inputLine;

void writeRelay(uint8_t i, bool on) {
  if (i >= 12) return;
  digitalWrite(relayPins[i], RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  states[i] = on;
}

void reportState() {
  espSerial.print(F("STATE"));
  for (uint8_t i=0; i<12; i++) {
    espSerial.print(',');
    espSerial.print(states[i] ? 1 : 0);
  }
  espSerial.print('\n');
}

void processLine(String line) {
  line.trim();
  if (line == "PING") {
    espSerial.print(F("PONG\n"));
    return;
  }
  if (line == "GET") {
    reportState();
    return;
  }
  if (line.startsWith("SET,")) {
    int first = line.indexOf(',');
    int second = line.indexOf(',', first + 1);
    if (second < 0) return;
    int channel = line.substring(first + 1, second).toInt();
    int value = line.substring(second + 1).toInt();
    if (channel < 5 || channel > 16 || (value != 0 && value != 1)) {
      espSerial.print(F("ERR,ARGS\n"));
      return;
    }
    writeRelay(channel - 5, value == 1);
    espSerial.print(F("OK,"));
    espSerial.print(channel);
    espSerial.print(',');
    espSerial.print(states[channel - 5] ? 1 : 0);
    espSerial.print('\n');
    return;
  }
  espSerial.print(F("ERR,UNKNOWN\n"));
}

void setup() {
  // Preload inactive level before switching pins to OUTPUT.
  for (uint8_t i=0; i<12; i++) {
    digitalWrite(relayPins[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(relayPins[i], OUTPUT);
    states[i] = false;
  }
  Serial.begin(115200); // USB diagnostics
  espSerial.begin(19200);
  Serial.println(F("PHANTOM Nano slave ready"));
}

void loop() {
  while (espSerial.available()) {
    char c = (char)espSerial.read();
    if (c == '\n') {
      processLine(inputLine);
      inputLine = "";
    } else if (c != '\r' && inputLine.length() < 80) {
      inputLine += c;
    } else if (inputLine.length() >= 80) {
      inputLine = "";
    }
  }
}
