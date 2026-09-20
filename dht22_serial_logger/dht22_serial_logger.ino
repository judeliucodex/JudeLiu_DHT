/*
 * ROV DHT22 Mini Project — Phase 1 prototype: serial logger
 *
 * Board:   ESP32 DevKitC (ESP32-WROOM-32UE)
 * Sensor:  DHT22 (3-pin module = built-in 10k pull-up;
 *          bare 4-pin: add 10k resistor between VCC and DATA)
 *
 * Wiring:
 *   DHT22 VCC  -> 3V3
 *   DHT22 DATA -> GPIO4  (silkscreen "D4")
 *   DHT22 GND  -> GND
 *   OLED SDA   -> GPIO21 (D21)      I2C data
 *   OLED SCL   -> GPIO22 (D22)      I2C clock
 *   OLED VCC   -> 3V3
 *   OLED GND   -> GND
 *
 * behaviour:
 *   - type "start"  -> begins logging one reading every 3 s, CSV format
 *   - type "stop"   -> pauses logging and prints session statistics
 *   - type "help"   -> lists commands
 *   - onboard LED is ON while logging
 *   - OLED shows live values + units, sample number, and start/stop state
 *
 * Arduino IDE settings:
 *   Board:  ESP32 Dev Module
 *   Port:   usbserial-XXXX (CP2102 USB-serial chip; if no port appears on
 *           macOS, install the CP210x driver from SiLabs)
 *
 * NOTE: the DHT22 datasheet allows at most one read per 2 s; 3000 ms keeps a
 * margin. Reading faster than 2 s produces occasional failed reads.
 */

#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define DHTPIN  4        // GPIO4 — not a strapping pin, free to use
#define DHTTYPE DHT22
#define LED_PIN 2        // DevKitC onboard LED, active-HIGH (HIGH = on)

#define OLED_SDA  21     // ESP32's default I2C pins
#define OLED_SCL  22
#define OLED_W    128    // 0.96" panel = 128x64 SSD1306
#define OLED_H    64
#define OLED_ADDR 0x3C   // nearly all 0.96" modules; a few use 0x3D

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);   // -1 = no reset pin

const unsigned long READ_INTERVAL_MS = 3000;  // comfortably within the DHT22's 2 s minimum

DHT dht(DHTPIN, DHTTYPE);

bool logging = false;
unsigned long lastRead = 0;
unsigned long sampleNo = 0;
uint32_t totalReads = 0;
uint32_t goodReads  = 0;

void printHelp() {
  Serial.println("commands: start | stop | help");
}

void startLogging() {
  if (logging) { Serial.println("already running"); return; }
  logging = true;
  sampleNo = 0;
  lastRead = millis() - READ_INTERVAL_MS;   // first reading fires immediately
  digitalWrite(LED_PIN, HIGH);              // LED on
  Serial.println("started, reading every " + String(READ_INTERVAL_MS) + " ms");
  Serial.println("sample,temp_C,humidity_RH");
}

void stopLogging() {
  if (!logging) { Serial.println("already stopped"); return; }
  logging = false;
  digitalWrite(LED_PIN, LOW);               // LED off
  Serial.println("stopped");
  if (totalReads > 0) {
    uint32_t rate = (goodReads * 100) / totalReads;
    Serial.printf("session: %lu reads, %lu ok (%lu%% success)\n",
                  (unsigned long)totalReads, (unsigned long)goodReads,
                  (unsigned long)rate);
  }
}

void runCommand(String cmd) {
  if      (cmd == "start") startLogging();
  else if (cmd == "stop")  stopLogging();
  else if (cmd == "help")  printHelp();
  else { Serial.println("unknown command: " + cmd); printHelp(); }
}

void handleSerial() {
  static String line = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();                // tolerates "Both NL & CR" or either alone
      line.toLowerCase();
      if (line.length()) runCommand(line);
      line = "";
    } else {
      line += c;
    }
  }
}

// redraws the OLED with the latest values; called on every reading
void updateDisplay(float t, float h, bool ok) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.cp437(true);                       // enables the degree glyph

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("ROV DHT22  #");
  display.print(sampleNo);

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(ok ? String(t, 1) : "--.-");
  display.print((char)247);                  // degree sign
  display.print("C  ");
  display.setCursor(0, 32);
  display.print(ok ? String(h, 1) : "--.-");
  display.print("%RH");

  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print(logging ? "LOGGING   type stop" : "STOPPED   type start");
  display.display();
}

void takeReading() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  totalReads++;
  sampleNo++;

  bool ok = !isnan(h) && !isnan(t);

  if (!ok) {
    Serial.printf("%lu,ERROR,ERROR\n", (unsigned long)sampleNo);
    // hint after the first failure only, to keep the log clean
    if (totalReads - goodReads == 1) {
      Serial.println("(first failed read: check wiring/pull-up)");
    }
  } else {
    goodReads++;
    Serial.printf("%lu,%.1f,%.1f\n", (unsigned long)sampleNo, t, h);
  }

  updateDisplay(t, h, ok);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);             // LED off

  Serial.begin(115200);
  // wait briefly for the USB-serial link so the banner isn't cut off
  for (int i = 0; i < 30 && !Serial; i++) delay(100);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found — check SDA=D21/SCL=D22, or try addr 0x3D");
  } else {
    updateDisplay(NAN, NAN, false);   // shows the "STOPPED" splash
    Serial.println("OLED ready");
  }

  dht.begin();
  Serial.println();
  Serial.println("ESP32 DevKitC + DHT22 + OLED logger");
  printHelp();
}

void loop() {
  handleSerial();                         // commands stay responsive between reads

  if (logging && millis() - lastRead >= READ_INTERVAL_MS) {
    lastRead = millis();
    takeReading();
  }
}
