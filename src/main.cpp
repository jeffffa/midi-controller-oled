#include "driver/gpio.h"
#include "index_html.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/TomThumb.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_coexist.h"

// --- OLED ---
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// Etiketter för varje knapp (index 0=btn1 … 3=btn4), max 20 tecken lagrat
char btnLabel[4][21]   = {"", "", "", ""};
char btnLabelLP[4][21] = {"", "", "", ""};
char btnLabelDP[4][21] = {"", "", "", ""};
// Etiketter för kombos (index 0=1+2, 1=2+3, 2=3+4)
char comboLabel[3][21] = {"", "", ""};

// Statusflaggor för rubrikrad
static bool oled_bt  = false;  // BLE-klient ansluten
static bool oled_esp = false;  // ESP-NOW har måladress
static volatile bool oledNeedsUpdate = false;
// Batteri: spänningsdelare på D10/GPIO9
// Justera ADC_BAT_MIN/MAX för din delare (LiPo 3.0-4.2V)
#define BAT_PIN     9
#define ADC_BAT_MIN 1861  // ADC vid 1.5V (3.0V bat, 100k+100k delare)
#define ADC_BAT_MAX 2607  // ADC vid 2.1V (4.2V bat, 100k+100k delare)
static int oledBatBars = 10; // 0-10 staplar baserat på spänningskurva

// Layout-konstanter
// Header:      y=0..7   (streck vid y=7,  8px)
// Knapprader:  y=8..53  → delat i mitten vid y=31 → 23px/22px per rad
// Komborad:    y=54..63 → streck vid y=54, text vid y=61
#define HDR_H  8    // innehåll startar här
#define MID_Y  31   // horisontellt kors mellan knapprader
#define CMB_Y  54   // överkant komborad (linje här)

extern bool oledBtnPressed[];
extern bool oledComboPressed[];
struct ButtonConfig {
  uint8_t type;
  uint8_t mode;
  uint8_t btCh;
  uint8_t btVal;
  uint8_t espCh;
  uint8_t espVal;
  uint8_t value;
  bool lp_enabled;
  uint8_t lp_type;
  uint8_t lp_btCh;
  uint8_t lp_btVal;
  uint8_t lp_espCh;
  uint8_t lp_espVal;
  uint8_t lp_value;
  bool dp_enabled;
  uint8_t dp_type;
  uint8_t dp_btCh;
  uint8_t dp_btVal;
  uint8_t dp_espCh;
  uint8_t dp_espVal;
  uint8_t dp_value;
};
extern ButtonConfig buttons[];
enum Mode { MODE_LIVE, MODE_CONFIG };
extern Mode currentMode;
static void drawQuadrant(int btnIdx, int qx, int qy, int w, int h) {
  bool inv = oledBtnPressed[btnIdx];
  if (inv) oled.fillRect(qx, qy, w, h, SSD1306_WHITE);
  oled.setTextColor(inv ? SSD1306_BLACK : SSD1306_WHITE);

  int numFuncs = 1;
  if (buttons[btnIdx].lp_enabled) numFuncs++;
  if (buttons[btnIdx].dp_enabled) numFuncs++;

  if (numFuncs == 1) {
    // Stor text: inbyggd 5×7-font, vertikalt centrerad
    oled.setFont(NULL);
    oled.setTextSize(1);
    oled.setCursor(qx + 1, qy + (h - 8) / 2);
    oled.print(btnLabel[btnIdx]);
  } else if (numFuncs == 2) {
    // Medium: inbyggd 5×7-font, två rader centrerade i rutan
    oled.setFont(NULL);
    oled.setTextSize(1);
    int y1 = qy + (h - 18) / 2;
    oled.setCursor(qx + 1, y1);
    oled.print(btnLabel[btnIdx]);
    oled.setCursor(qx + 1, y1 + 10);
    if (buttons[btnIdx].lp_enabled) oled.print(btnLabelLP[btnIdx]);
    else                            oled.print(btnLabelDP[btnIdx]);
  } else {
    // Liten: TomThumb, tre rader (ascent≈5px, line height≈6px)
    oled.setFont(&TomThumb);
    oled.setTextSize(1);
    oled.setCursor(qx + 1, qy + 6);  oled.print(btnLabel[btnIdx]);
    if (buttons[btnIdx].lp_enabled)
      { oled.setCursor(qx + 1, qy + 12); oled.print(btnLabelLP[btnIdx]); }
    if (buttons[btnIdx].dp_enabled)
      { oled.setCursor(qx + 1, qy + 18); oled.print(btnLabelDP[btnIdx]); }
  }

  // Återställ font för resten av oledDraw (combo-raden)
  oled.setFont(&TomThumb);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
}

void oledDraw() {
  oled.clearDisplay();
  oled.setFont(&TomThumb);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // === RUBRIKRAD (y=0..7, 8px) ===

  // ESP-NOW: 3 signalstaplar, x=0..7, y=1..6 (6px)
  if (oled_esp) {
    oled.fillRect(0, 5, 2, 2, SSD1306_WHITE);
    oled.fillRect(3, 3, 2, 4, SSD1306_WHITE);
    oled.fillRect(6, 1, 2, 6, SSD1306_WHITE);
  } else {
    oled.drawRect(0, 5, 2, 2, SSD1306_WHITE);
    oled.drawRect(3, 3, 2, 4, SSD1306_WHITE);
    oled.drawRect(6, 1, 2, 6, SSD1306_WHITE);
  }

  // Bluetooth: lodrätt + gaffellinjer, x=106..108, y=1..6
  oled.drawLine(106, 1, 106, 6, SSD1306_WHITE);
  if (oled_bt) {
    oled.drawLine(106, 1, 108, 2, SSD1306_WHITE);
    oled.drawLine(108, 2, 106, 4, SSD1306_WHITE);
    oled.drawLine(106, 4, 108, 5, SSD1306_WHITE);
    oled.drawLine(108, 5, 106, 6, SSD1306_WHITE);
  }

  // WiFi: visas bara i setup-läge
  if (currentMode == MODE_CONFIG) {
    oled.fillCircle(94, 6, 1, SSD1306_WHITE);
    oled.drawCircle(94, 6, 3, SSD1306_WHITE);
    oled.drawCircle(94, 6, 5, SSD1306_WHITE);
    oled.fillRect(86, 7, 17, 7, SSD1306_BLACK);
  }

  // Batteri, x=113..124, y=1..6 (inneryta 10×4px, 1 pixel = 10%)
  oled.drawRect(113, 1, 12, 6, SSD1306_WHITE);
  oled.drawRect(125, 2,  2, 4, SSD1306_WHITE);
  { int fill = oledBatBars;
    if (fill > 0) oled.fillRect(114, 2, fill, 4, SSD1306_WHITE); }

  // Rubriktext centrerad
  { const char* modeStr = (currentMode == 1) ? "Setup" : "Live";
    int16_t x1, y1; uint16_t tw, th;
    oled.getTextBounds(modeStr, 0, 6, &x1, &y1, &tw, &th);
    oled.setCursor((OLED_WIDTH - tw) / 2, 6);
    oled.print(modeStr); }

  // Rubrikstreck
  oled.drawFastHLine(0, 7, OLED_WIDTH, SSD1306_WHITE);

  // === KNAPPRADER (y=HDR_H..CMB_Y-1) ===
  // Horisontellt kors vid MID_Y, lodrätt x=64 från HDR_H till CMB_Y
  oled.drawFastHLine(0, MID_Y, OLED_WIDTH, SSD1306_WHITE);
  oled.drawFastVLine(OLED_WIDTH / 2, HDR_H, CMB_Y - HDR_H, SSD1306_WHITE);
  // btn2=uppe-vänster  btn3=uppe-höger
  // btn1=nere-vänster  btn4=nere-höger
  drawQuadrant(1,  0, HDR_H,    63, MID_Y - HDR_H);
  drawQuadrant(2, 65, HDR_H,    63, MID_Y - HDR_H);
  drawQuadrant(0,  0, MID_Y+1,  63, CMB_Y - MID_Y - 1);
  drawQuadrant(3, 65, MID_Y+1,  63, CMB_Y - MID_Y - 1);

  // === KOMBORAD (y=CMB_Y..63) ===
  oled.drawFastHLine(0, CMB_Y, OLED_WIDTH, SSD1306_WHITE);
  // 2 lodräta delare: x=42 och x=85
  oled.drawFastVLine(42, CMB_Y, OLED_HEIGHT - CMB_Y, SSD1306_WHITE);
  oled.drawFastVLine(85, CMB_Y, OLED_HEIGHT - CMB_Y, SSD1306_WHITE);
  // Combo-celler: fyll vit + svart text när aktiv
  int cmbX[3] = {0, 43, 86};
  int cmbW[3] = {42, 42, 42};
  for (int i = 0; i < 3; i++) {
    if (oledComboPressed[i]) oled.fillRect(cmbX[i], CMB_Y+1, cmbW[i], OLED_HEIGHT-CMB_Y-1, SSD1306_WHITE);
    oled.setTextColor(oledComboPressed[i] ? SSD1306_BLACK : SSD1306_WHITE);
    oled.setCursor(cmbX[i]+1, CMB_Y+7); oled.print(comboLabel[i]);
  }
  oled.setTextColor(SSD1306_WHITE);

  oled.display();
}

void oledInit() {
  Wire.begin(5, 6); // SDA=GPIO5, SCL=GPIO6
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED: begin failed");
    return;
  }
  oledDraw();
  Serial.println("OLED: OK");
}

// --- KONFIGURATION ---
const int NUM_BUTTONS = 4;
const int NUM_COMBOS = 3; // 1+2, 2+3, 3+4
const int btnPins[NUM_BUTTONS] = {1, 2, 3, 4};
const int ledPin = 7;
int wifiChannel = 1;        // Current WiFi channel (1-11)
int autoChannelEnabled = 1; // 0 = manual, 1 = auto-scan

// --- GLOBALA TIDSINSTÄLLNINGAR (Standardvärden) ---
unsigned long doublePressTime = 200; // Standard 200ms
unsigned long longPressTime = 1500;  // Standard 1500ms
const unsigned long comboDelay = 45; // HUR LÄNGE VI VÄNTAR PÅ COMBO (ms)

// --- DATASTRUKTURER ---

// Vi återanvänder ButtonConfig structen men använder bara "Short Press" delen
// för combos
struct ComboConfig {
  bool enabled;
  uint8_t type;
  uint8_t mode;
  uint8_t btCh;
  uint8_t btVal;
  uint8_t espCh;
  uint8_t espVal;
  uint8_t value;
};

ButtonConfig buttons[NUM_BUTTONS];
ComboConfig combos[NUM_COMBOS]; // 0=(1+2), 1=(2+3), 2=(3+4)

bool buttonLatchState[NUM_BUTTONS] = {false};
bool comboLatchState[NUM_COMBOS] = {false};

typedef struct struct_message {
  int id;
  int type;
  int number;
  int value;
  int channel;
} struct_message;
struct_message myData;

typedef struct struct_pairing {
  int msgType;
  char name[32];
} struct_pairing;
struct_pairing pairingData;

// --- GLOBALA VARIABLER ---
uint8_t receiverAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool newDeviceFound = false;
String foundName = "";
uint8_t foundMAC[6];
esp_now_peer_info_t peerInfo;

// Deferred save: NVS-skrivning från WiFi-callback är osäker — gör det i loop() istället
static volatile bool s_saveReceiverPending = false;
static uint8_t       s_pendingReceiver[6]  = {};

NimBLECharacteristic *pCharacteristic = NULL;
NimBLEServer *pGlobalServer = NULL; // GLOBAL SERVER INSTANCE
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

// --- LED PWM & BT STATE ---
const int PWM_CHAN = 0;
const int PWM_FREQ = 5000;
const int PWM_RES = 8;
const int LED_BRIGHT_MAX = 255;
const int LED_BRIGHT_DIM = 5; // Mycket svagare ljus

bool deviceConnected = false;

void updateLed(bool on) {
  if (on) {
    ledcWrite(PWM_CHAN, LED_BRIGHT_MAX);
  } else {
    if (deviceConnected) {
      ledcWrite(PWM_CHAN, LED_BRIGHT_DIM);
    } else {
      ledcWrite(PWM_CHAN, 0);
    }
  }
}

void blinkLedFast() {
  updateLed(true);
  delay(50);
  updateLed(false);
}

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.println("BLE Device Connected");
    deviceConnected = true;
    oled_bt = true;
    oledNeedsUpdate = true;
    updateLed(false);
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    Serial.println("BLE Device Disconnected");
    deviceConnected = false;
    oled_bt = false;
    oledNeedsUpdate = true;
    updateLed(false);
    NimBLEDevice::getAdvertising()->start();
  }
};

// --- LED FX HELPERS ---
void fadeLed(int loops, int speedDelay) {
  for (int k = 0; k < loops; k++) {
    for (int i = 0; i <= 255; i += 5) {
      ledcWrite(PWM_CHAN, i);
      delay(speedDelay);
    }
    for (int i = 255; i >= 0; i -= 5) {
      ledcWrite(PWM_CHAN, i);
      delay(speedDelay);
    }
  }
}

void breathingLed() {
  // "Andas" mellan svagt (5) och lite starkare (150)
  // Periodtid ca 3-4 sekunder
  float val = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
  // Mappa om val (0-255ish) till önskat intervall (5-150)
  int mapVal = map((int)val, 0, 255, 5, 150);
  ledcWrite(PWM_CHAN, constrain(mapVal, 5, 150));
}

// --- KNAPP TILLSTÅND (STATE MACHINE) ---
enum BtnState {
  IDLE,       // Väntar på tryck
  WAIT_COMBO, // Väntar en kort stund för att se om grannknappen trycks (Combo
              // detection)
  WAIT_DBL,   // Har tryckt en gång, väntar på nästa eller timeout
  WAIT_LONG,  // Håller inne knappen, väntar på gränsen
  ACTIVE_HOLD,     // Har triggat en funktion, väntar på release
  BLOCKED_BY_COMBO // Knappen är nere men hanteras av en combo, skicka inget
                   // eget
};

BtnState btnState[NUM_BUTTONS] = {IDLE};
unsigned long stateTimer[NUM_BUTTONS] = {0};
bool activeWasStandard[NUM_BUTTONS] = {false};
bool oledBtnPressed[NUM_BUTTONS] = {false};
bool oledComboPressed[3] = {false};

// Debounce-variabler
int stableState[NUM_BUTTONS] = {HIGH, HIGH, HIGH, HIGH};
int lastRawState[NUM_BUTTONS] = {HIGH, HIGH, HIGH, HIGH};
unsigned long lastDebounceTime[NUM_BUTTONS] = {0};
unsigned long debounceDelay = 15; // Snabbare debounce för att hinna med combos

// Combo State
bool comboActive[NUM_COMBOS] = {false};
bool comboWasStandard[NUM_COMBOS] = {false};

// System Combos
// System Combos (replaced by checkSystemCombos variables below)

// MIDI CLOCK & TEMPO
bool midiClockEnabled = false;
float currentBPM = 120.0;
unsigned long lastClockMicros = 0;
unsigned long clockIntervalMicros = 20833;
unsigned long lastTapTime = 0;
bool clockRunning = false;
int clockTickCount = 0;
unsigned long ledBlinkTime = 0;
bool ledState = false;
bool tempoModeActive = false;
unsigned long tempoModeExitTimer = 0;
int tempoModeButtonIndex = -1;

Mode currentMode;

extern const char *htmlPage;

// --- FUNKTIONER ---

String formatMac(uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void enterStandby() {
  Serial.println("Går in i Standby (Deep Sleep)...");
  // Tre tydliga blink (Full ljusstyrka)
  for (int i = 0; i < 3; i++) {
    ledcWrite(PWM_CHAN, 255);
    delay(300);
    ledcWrite(PWM_CHAN, 0);
    delay(300);
  }
  // Släck OLED
  oled.clearDisplay();
  oled.display();
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
  gpio_wakeup_enable((gpio_num_t)btnPins[0], GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)btnPins[3], GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_deep_sleep_start();
}

// System Combo Variables
unsigned long bothHeldStart = 0;
bool waitingForSetupRelease = false;

void checkSystemCombos() {
  bool b1 = (digitalRead(btnPins[0]) == LOW);
  bool b4 = (digitalRead(btnPins[3]) == LOW);

  // --- STANDBY LOGIC (Hold 1+4 for 1.5s) ---
  if (b1 && b4) {
    if (bothHeldStart == 0) {
      bothHeldStart = millis();
      waitingForSetupRelease = true; // Mark att vi börjat hålla in båda
    } else {
      // Om vi hållit in båda i mer än 500ms -> STANDBY
      if (millis() - bothHeldStart > 500) {
        enterStandby();
      }
    }
  } else {
    // --- SETUP LOGIC (Hold 1, Tap 4) ---
    // Om vi släpper knapparna (dvs inte b1 && b4 längre)

    // Scenario: Vi släppte 4:an, men håller fortfarande 1:an
    if (b1 && !b4 && bothHeldStart != 0 && waitingForSetupRelease) {
      unsigned long duration = millis() - bothHeldStart;

      // Om trycket var kort (ett "klick" på 4:an) och B1 hålls -> SETUP
      if (duration > 50 && duration < 500) {
        Serial.println("Setup Combo detected (Hold 1, Tap 4) -> Rebooting");
        preferences.begin("midi_cfg", false);
        preferences.putBool("force_cfg", true);
        preferences.end();
        // Feedback fade
        fadeLed(3, 10);
        ESP.restart();
      }
    }

    // Återställ om vi inte håller båda
    if (!b1 || !b4) {
      bothHeldStart = 0;
      waitingForSetupRelease = false;
    }
  }
}

void checkMidiClockStatus() {
  midiClockEnabled = false;
  for (int i = 0; i < NUM_BUTTONS; i++) {
    if (buttons[i].type == 3 || buttons[i].type == 4) {
      midiClockEnabled = true;
      break;
    }
  }
}

void loadActiveSettings() {
  preferences.begin("midi_cfg", false);
  if (preferences.isKey("rx"))
    preferences.getBytes("rx", receiverAddress, 6);

  if (preferences.isKey("btn_cfg_v4")) {
    preferences.getBytes("btn_cfg_v4", &buttons, sizeof(buttons));
  } else {
    for (int i = 0; i < NUM_BUTTONS; i++) {
      buttons[i] = {0,     0, 1, (uint8_t)(i + 1),  1, (uint8_t)(35 + i), 127,
                    false, 0, 1, (uint8_t)(i + 10), 1, (uint8_t)(50 + i), 127,
                    false, 0, 1, (uint8_t)(i + 20), 1, (uint8_t)(60 + i), 127};
    }
  }

  // Load Combos
  if (preferences.isKey("cmb_cfg")) {
    preferences.getBytes("cmb_cfg", &combos, sizeof(combos));
  } else {
    for (int i = 0; i < NUM_COMBOS; i++) {
      combos[i] = {false, 0, 0, 1, (uint8_t)(i + 50), 1, (uint8_t)(i + 50),
                   127};
    }
  }

  currentBPM = preferences.getFloat("bpm", 120.0);
  clockIntervalMicros = 60000000 / (currentBPM * 24);

  // Ladda Tider
  doublePressTime = preferences.getUInt("dp_time", 200);
  longPressTime = preferences.getUInt("lp_time", 1500);

  // Ladda Wi-Fi kanal
  wifiChannel = preferences.getInt("wifi_ch", 1);
  autoChannelEnabled = preferences.getInt("auto_ch", 1);

  // Ladda OLED-etiketter
  for (int i = 0; i < NUM_BUTTONS; i++) {
    String val = preferences.getString(("lbl"  + String(i + 1)).c_str(), "");
    strncpy(btnLabel[i], val.c_str(), 20); btnLabel[i][20] = '\0';
    val = preferences.getString(("llp" + String(i + 1)).c_str(), "");
    strncpy(btnLabelLP[i], val.c_str(), 20); btnLabelLP[i][20] = '\0';
    val = preferences.getString(("ldp" + String(i + 1)).c_str(), "");
    strncpy(btnLabelDP[i], val.c_str(), 20); btnLabelDP[i][20] = '\0';
  }
  for (int i = 0; i < NUM_COMBOS; i++) {
    String val = preferences.getString(("lcmb" + String(i + 1)).c_str(), "");
    strncpy(comboLabel[i], val.c_str(), 20); comboLabel[i][20] = '\0';
  }

  preferences.end();
  checkMidiClockStatus();
}

void saveActiveToNVS() {
  preferences.begin("midi_cfg", false);
  preferences.putBytes("btn_cfg_v4", &buttons, sizeof(buttons));
  preferences.putBytes("cmb_cfg", &combos, sizeof(combos));
  preferences.putUInt("dp_time", doublePressTime);
  preferences.putUInt("lp_time", longPressTime);
  preferences.putInt("wifi_ch", wifiChannel);
  preferences.putInt("auto_ch", autoChannelEnabled);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    preferences.putString(("lbl"  + String(i + 1)).c_str(), String(btnLabel[i]));
    preferences.putString(("llp"  + String(i + 1)).c_str(), String(btnLabelLP[i]));
    preferences.putString(("ldp"  + String(i + 1)).c_str(), String(btnLabelDP[i]));
  }
  for (int i = 0; i < NUM_COMBOS; i++)
    preferences.putString(("lcmb" + String(i + 1)).c_str(), String(comboLabel[i]));
  preferences.end();
  checkMidiClockStatus();
}
// Initialize Autostomp preset (slot 4) with default values - ALWAYS overwrites
void initAutostompPreset() {
  preferences.begin("midi_cfg", false);
  // Always write Autostomp defaults (slot 4 is a factory preset)
  // Button 1: PC 0 (Sound Select)
  // Button 2: CC 64 (Auto Toggle)
  // Button 3: CC 60 (Tempo Down)
  // Button 4: CC 62 (Tempo Up)
  ButtonConfig autoBtns[NUM_BUTTONS] = {
      {1, 0, 1,   0,     1, 0, 127, false, 0, 1,  0,
       1, 0, 127, false, 0, 1, 0,   1,     0, 127}, // Btn1: PC 0
      {0, 0, 1,   64,    1, 64, 127, false, 0, 1,  0,
       1, 0, 127, false, 0, 1,  0,   1,     0, 127}, // Btn2: CC 64 (Auto)
      {0, 0, 1,   60,    1, 60, 127, false, 0, 1,  0,
       1, 0, 127, false, 0, 1,  0,   1,     0, 127}, // Btn3: CC 60 (Tempo-)
      {0, 0, 1,   62,    1, 62, 127, false, 0, 1,  0,
       1, 0, 127, false, 0, 1,  0,   1,     0, 127} // Btn4: CC 62 (Tempo+)
  };
  ComboConfig autoCombos[NUM_COMBOS] = {{false, 0, 0, 1, 50, 1, 50, 127},
                                        {false, 0, 0, 1, 51, 1, 51, 127},
                                        {false, 0, 0, 1, 52, 1, 52, 127}};
  preferences.putBytes("p4_data", &autoBtns, sizeof(autoBtns));
  preferences.putBytes("p4_cmb", &autoCombos, sizeof(autoCombos));
  preferences.putString("p4_name", "Autostomp");
  Serial.println("Autostomp preset set");
  preferences.end();
}

void saveBPM() {
  preferences.begin("midi_cfg", false);
  preferences.putFloat("bpm", currentBPM);
  preferences.end();
}

// --- MIDI SÄNDNING ---
void sendToBLE(int type, int channel, int number, int value) {
  if (currentMode == MODE_LIVE && pCharacteristic != NULL) {
    uint8_t status = 0;
    if (type == 0 || type == 2)
      status = 0xB0;
    else if (type == 1)
      status = 0xC0;
    else if (type == 5) {
      if (value > 0)
        status = 0x90;
      else
        status = 0x80;
    }

    if (status != 0) {
      status |= (channel - 1);
      uint8_t pkt[5];
      int len = 0;
      if (type == 1) {
        pkt[0] = 0x80;
        pkt[1] = 0x80;
        pkt[2] = status;
        pkt[3] = number;
        len = 4;
      } else {
        pkt[0] = 0x80;
        pkt[1] = 0x80;
        pkt[2] = status;
        pkt[3] = number;
        pkt[4] = value;
        len = 5;
      }
      pCharacteristic->setValue(pkt, len);
      pCharacteristic->notify();
    }
  }
}

void sendToESP(int type, int channel, int number, int value) {
  uint8_t status = 0;
  if (type == 0 || type == 2)      status = 0xB0;
  else if (type == 1)              status = 0xC0;
  else if (type == 5) status = (value > 0) ? 0x90 : 0x80;
  if (status == 0) return;
  status |= (channel - 1);
  if (type == 1) {
    uint8_t pkt[2] = {status, (uint8_t)number};
    esp_now_send(receiverAddress, pkt, 2);
  } else {
    uint8_t pkt[3] = {status, (uint8_t)number, (uint8_t)value};
    esp_now_send(receiverAddress, pkt, 3);
  }
}

void updateMidiClock() {
  if (!midiClockEnabled)
    return;
  unsigned long currentMicros = micros();
  if (currentMicros - lastClockMicros >= clockIntervalMicros) {
    lastClockMicros = currentMicros;
    clockTickCount++;
    if (clockTickCount >= 24) {
      clockTickCount = 0;
      if (!tempoModeActive) {
        updateLed(true);
        ledBlinkTime = millis();
        ledState = true;
      }
    }
  }
  if (ledState && !tempoModeActive && millis() - ledBlinkTime > 30) {
    updateLed(false);
    ledState = false;
  }
}

void registerTap() {
  unsigned long now = millis();
  if (lastTapTime > 0) {
    unsigned long diff = now - lastTapTime;
    if (diff > 100 && diff < 2000) {
      float newBPM = 60000.0 / diff;
      currentBPM = (currentBPM * 0.4) + (newBPM * 0.6);
      clockIntervalMicros = 60000000 / (currentBPM * 24);
      if (!clockRunning) {
        clockRunning = true;
      }
    } else if (diff > 2000) {
      clockRunning = false;
    }
  }
  lastTapTime = now;
  updateLed(false);
  delay(5);
  updateLed(true);
  ledBlinkTime = millis();
  ledState = true;
}

// --- JSON & HTML ---
String getPresetJSON(int slot) {
  preferences.begin("midi_cfg", true);
  String keyData = "p" + String(slot) + "_data";
  String keyCombo = "p" + String(slot) + "_cmb";
  String keyName = "p" + String(slot) + "_name";
  ButtonConfig tempBtns[NUM_BUTTONS];
  ComboConfig tempCombos[NUM_COMBOS];
  String name = preferences.getString(keyName.c_str(), "Empty");

  if (preferences.isKey(keyData.c_str())) {
    preferences.getBytes(keyData.c_str(), &tempBtns, sizeof(tempBtns));
  } else {
    memset(&tempBtns, 0, sizeof(tempBtns));
  }

  if (preferences.isKey(keyCombo.c_str())) {
    preferences.getBytes(keyCombo.c_str(), &tempCombos, sizeof(tempCombos));
  } else {
    memset(&tempCombos, 0, sizeof(tempCombos));
  }

  preferences.end();

  String s = "{name:'" + name + "',";
  for (int i = 0; i < NUM_BUTTONS; i++) {
    String n = String(i + 1);
    s += "t" + n + ":" + String(tempBtns[i].type) + ",m" + n + ":" +
         String(tempBtns[i].mode) + ",cb" + n + ":" + String(tempBtns[i].btCh) +
         ",vb" + n + ":" + String(tempBtns[i].btVal) + ",ce" + n + ":" +
         String(tempBtns[i].espCh) + ",ve" + n + ":" +
         String(tempBtns[i].espVal) + ",v" + n + ":" +
         String(tempBtns[i].value);
    s += ",lpe" + n + ":" + String(tempBtns[i].lp_enabled) + ",lpt" + n + ":" +
         String(tempBtns[i].lp_type) + ",lpcb" + n + ":" +
         String(tempBtns[i].lp_btCh) + ",lpvb" + n + ":" +
         String(tempBtns[i].lp_btVal) + ",lpce" + n + ":" +
         String(tempBtns[i].lp_espCh) + ",lpve" + n + ":" +
         String(tempBtns[i].lp_espVal) + ",lpv" + n + ":" +
         String(tempBtns[i].lp_value);
    s += ",dpe" + n + ":" + String(tempBtns[i].dp_enabled) + ",dpt" + n + ":" +
         String(tempBtns[i].dp_type) + ",dpcb" + n + ":" +
         String(tempBtns[i].dp_btCh) + ",dpvb" + n + ":" +
         String(tempBtns[i].dp_btVal) + ",dpce" + n + ":" +
         String(tempBtns[i].dp_espCh) + ",dpve" + n + ":" +
         String(tempBtns[i].dp_espVal) + ",dpv" + n + ":" +
         String(tempBtns[i].dp_value);
    s += ",";
  }
  for (int i = 0; i < NUM_COMBOS; i++) {
    String n = String(i + 1);
    s += "cen" + n + ":" + String(tempCombos[i].enabled) + ",ct" + n + ":" +
         String(tempCombos[i].type) + ",cm" + n + ":" +
         String(tempCombos[i].mode) + ",ccb" + n + ":" +
         String(tempCombos[i].btCh) + ",cvb" + n + ":" +
         String(tempCombos[i].btVal) + ",cce" + n + ":" +
         String(tempCombos[i].espCh) + ",cve" + n + ":" +
         String(tempCombos[i].espVal) + ",cv" + n + ":" +
         String(tempCombos[i].value);
    if (i < NUM_COMBOS - 1)
      s += ",";
  }
  s += "}";
  return s;
}

String generateButtonCards() {
  String html = "";
  for (int i = 0; i < NUM_BUTTONS; i++) {
    String n = String(i + 1);
    html += "<div id='tab_" + n + "' class='tab-panel'>";
    html += "<div class='btn-card'><div class='btn-header'>BUTTON " + n + "</div>";

    html += "<div class='settings-row'><div class='input-group' style='width:100%'>"
            "<label>OLED Label</label>"
            "<input name='lbl" + n + "' maxlength='20' value='" +
            String(btnLabel[i]) + "' onchange='sendData(this)' "
            "style='width:100%;box-sizing:border-box'></div></div>";

    html += "<div class='settings-row'><div "
            "class='input-group'><label>Type</label><select name='t" +
            n + "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(buttons[i].type == 0 ? "selected" : "") + ">CC</option>";
    html += "<option value='1' " +
            String(buttons[i].type == 1 ? "selected" : "") + ">PC</option>";
    html += "<option value='2' " +
            String(buttons[i].type == 2 ? "selected" : "") + ">TAP</option>";
    html += "<option value='3' " +
            String(buttons[i].type == 3 ? "selected" : "") + ">CLK</option>";
    html += "<option value='4' " +
            String(buttons[i].type == 4 ? "selected" : "") + ">CC+</option>";
    html += "<option value='5' " +
            String(buttons[i].type == 5 ? "selected" : "") +
            ">NOTE</option></select></div>";
    html += "<div class='input-group'><label>Mode</label><select name='m" + n +
            "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(buttons[i].mode == 0 ? "selected" : "") + ">Trig</option>";
    html += "<option value='1' " +
            String(buttons[i].mode == 1 ? "selected" : "") + ">Mom</option>";
    html += "<option value='2' " +
            String(buttons[i].mode == 2 ? "selected" : "") +
            ">Lat</option></select></div>";
    html += "<div class='input-group'><label>Value</label><input name='v" + n +
            "' value='" + String(buttons[i].value) +
            "' onchange='sendData(this)'></div></div>";

    html += "<div class='conn-row'><span><b>BT:</b> Ch <input class='sm' "
            "name='cb" +
            n + "' value='" + String(buttons[i].btCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='vb" + n + "' value='" +
            String(buttons[i].btVal) + "' onchange='sendData(this)'></span>";
    html += "<span style='margin-left:10px'><b>ESP:</b> Ch <input class='sm' "
            "name='ce" +
            n + "' value='" + String(buttons[i].espCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='ve" + n + "' value='" +
            String(buttons[i].espVal) +
            "' onchange='sendData(this)'></span></div>";

    String lp_checked = buttons[i].lp_enabled ? "checked" : "";
    String lp_display = buttons[i].lp_enabled ? "block" : "none";
    html += "<div class='opt-header'><label><input type='checkbox' id='lpe_cb" +
            n + "' onchange='toggleBox(\"lp_box" + n +
            "\", this.checked); sendDataRaw(\"lpe" + n +
            "\", this.checked?1:0)' " + lp_checked +
            "> Enable Long Press</label></div>";
    html += "<div id='lp_box" + n +
            "' class='opt-settings' style='display:" + lp_display +
            "; border-color:#FF9800'><div class='settings-row'>";
    html += "<div class='input-group'><label>Type</label><select name='lpt" +
            n + "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(buttons[i].lp_type == 0 ? "selected" : "") + ">CC</option>";
    html += "<option value='1' " +
            String(buttons[i].lp_type == 1 ? "selected" : "") + ">PC</option>";
    html += "<option value='5' " +
            String(buttons[i].lp_type == 5 ? "selected" : "") +
            ">NOTE</option></select></div>";
    html += "<div class='input-group'><label>Value</label><input name='lpv" +
            n + "' value='" + String(buttons[i].lp_value) +
            "' onchange='sendData(this)'></div></div>";
    html += "<div class='conn-row'><span><b>BT:</b> Ch <input class='sm' "
            "name='lpcb" +
            n + "' value='" + String(buttons[i].lp_btCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='lpvb" + n + "' value='" +
            String(buttons[i].lp_btVal) + "' onchange='sendData(this)'></span>";
    html += "<span style='margin-left:10px'><b>ESP:</b> Ch <input class='sm' "
            "name='lpce" +
            n + "' value='" + String(buttons[i].lp_espCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='lpve" + n + "' value='" +
            String(buttons[i].lp_espVal) +
            "' onchange='sendData(this)'></span></div>";
    html += "<div class='settings-row'><div class='input-group' style='width:100%'>"
            "<label>OLED Label</label>"
            "<input name='llp" + n + "' maxlength='20' value='" +
            String(btnLabelLP[i]) + "' onchange='sendData(this)' "
            "style='width:100%;box-sizing:border-box'></div></div></div>";

    String dp_checked = buttons[i].dp_enabled ? "checked" : "";
    String dp_display = buttons[i].dp_enabled ? "block" : "none";
    html += "<div class='opt-header'><label><input type='checkbox' id='dpe_cb" +
            n + "' onchange='toggleBox(\"dp_box" + n +
            "\", this.checked); sendDataRaw(\"dpe" + n +
            "\", this.checked?1:0)' " + dp_checked +
            "> Enable Double Press</label></div>";
    html += "<div id='dp_box" + n +
            "' class='opt-settings' style='display:" + dp_display +
            "; border-color:#2196F3'><div class='settings-row'>";
    html += "<div class='input-group'><label>Type</label><select name='dpt" +
            n + "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(buttons[i].dp_type == 0 ? "selected" : "") + ">CC</option>";
    html += "<option value='1' " +
            String(buttons[i].dp_type == 1 ? "selected" : "") + ">PC</option>";
    html += "<option value='5' " +
            String(buttons[i].dp_type == 5 ? "selected" : "") +
            ">NOTE</option></select></div>";
    html += "<div class='input-group'><label>Value</label><input name='dpv" +
            n + "' value='" + String(buttons[i].dp_value) +
            "' onchange='sendData(this)'></div></div>";
    html += "<div class='conn-row'><span><b>BT:</b> Ch <input class='sm' "
            "name='dpcb" +
            n + "' value='" + String(buttons[i].dp_btCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='dpvb" + n + "' value='" +
            String(buttons[i].dp_btVal) + "' onchange='sendData(this)'></span>";
    html += "<span style='margin-left:10px'><b>ESP:</b> Ch <input class='sm' "
            "name='dpce" +
            n + "' value='" + String(buttons[i].dp_espCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='dpve" + n + "' value='" +
            String(buttons[i].dp_espVal) +
            "' onchange='sendData(this)'></span></div>";
    html += "<div class='settings-row'><div class='input-group' style='width:100%'>"
            "<label>OLED Label</label>"
            "<input name='ldp" + n + "' maxlength='20' value='" +
            String(btnLabelDP[i]) + "' onchange='sendData(this)' "
            "style='width:100%;box-sizing:border-box'></div></div></div>";

    html += "</div></div>"; // stänger btn-card + tab-panel
  }

  for (int i = 0; i < NUM_COMBOS; i++) {
    String n = String(i + 1);
    String label = (i == 0) ? "1 + 2" : (i == 1) ? "2 + 3" : "3 + 4";
    String c_checked = combos[i].enabled ? "checked" : "";
    String c_display = combos[i].enabled ? "block" : "none";

    html += "<div id='tab_c" + n + "' class='tab-panel'>";
    html += "<div class='btn-card' style='border-color:#9C27B0'>";
    html += "<div class='btn-header' style='color:#E040FB'>COMBO " + label +
            " <input type='checkbox' style='float:right' "
            "onchange='toggleBox(\"c_box" +
            n + "\", this.checked); sendDataRaw(\"cen" + n +
            "\", this.checked?1:0)' " + c_checked + "></div>";
    html += "<div id='c_box" + n + "' style='display:" + c_display + "'>";

    html += "<div class='settings-row'><div "
            "class='input-group'><label>Type</label><select name='ct" +
            n + "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(combos[i].type == 0 ? "selected" : "") + ">CC</option>";
    html += "<option value='1' " +
            String(combos[i].type == 1 ? "selected" : "") + ">PC</option>";
    html += "<option value='5' " +
            String(combos[i].type == 5 ? "selected" : "") +
            ">NOTE</option></select></div>";
    html += "<div class='input-group'><label>Mode</label><select name='cm" + n +
            "' onchange='sendData(this)'>";
    html += "<option value='0' " +
            String(combos[i].mode == 0 ? "selected" : "") + ">Trig</option>";
    html += "<option value='1' " +
            String(combos[i].mode == 1 ? "selected" : "") + ">Mom</option>";
    html += "<option value='2' " +
            String(combos[i].mode == 2 ? "selected" : "") +
            ">Lat</option></select></div>";
    html += "<div class='input-group'><label>Value</label><input name='cv" + n +
            "' value='" + String(combos[i].value) +
            "' onchange='sendData(this)'></div></div>";

    html += "<div class='conn-row'><span><b>BT:</b> Ch <input class='sm' "
            "name='ccb" +
            n + "' value='" + String(combos[i].btCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='cvb" + n + "' value='" +
            String(combos[i].btVal) + "' onchange='sendData(this)'></span>";
    html += "<span style='margin-left:10px'><b>ESP:</b> Ch <input class='sm' "
            "name='cce" +
            n + "' value='" + String(combos[i].espCh) +
            "' onchange='sendData(this)'>";
    html += " # <input class='sm' name='cve" + n + "' value='" +
            String(combos[i].espVal) +
            "' onchange='sendData(this)'></span></div>";
    html += "<div class='settings-row'><div class='input-group' style='width:100%'>"
            "<label>OLED Label</label>"
            "<input name='lcmb" + n + "' maxlength='20' value='" +
            String(comboLabel[i]) + "' onchange='sendData(this)' "
            "style='width:100%;box-sizing:border-box'></div></div>";
    html += "</div></div></div>"; // stänger c_box + btn-card + tab-panel
  }

  return html;
}

void handleRoot() {
  String s = htmlPage;
  s.replace("%SELF_MAC%", WiFi.macAddress());
  s.replace("%CURr_MAC%", formatMac(receiverAddress));
  s.replace("%BUTTON_CARDS%", generateButtonCards());

  // Replace Global Time values
  s.replace("%DP_TIME%", String(doublePressTime));
  s.replace("%LP_TIME%", String(longPressTime));

  // Replace Channel values
  s.replace("%WIFI_CH%", String(wifiChannel));
  s.replace("%AUTO_CH%", String(autoChannelEnabled));

  String json = "{" + String("1:") + getPresetJSON(1) + "," + String("2:") +
                getPresetJSON(2) + "," + String("3:") + getPresetJSON(3) + "," +
                String("4:") + getPresetJSON(4) + "}";
  s.replace("%PRESET_JSON%", json);
  preferences.begin("midi_cfg", true);
  s.replace("%P1_NAME%", preferences.getString("p1_name", "Empty"));
  s.replace("%P2_NAME%", preferences.getString("p2_name", "Empty"));
  s.replace("%P3_NAME%", preferences.getString("p3_name", "Empty"));
  s.replace("%P4_NAME%", preferences.getString("p4_name", "Autostomp"));
  preferences.end();
  if (newDeviceFound) {
    String msg =
        "<div style='background:#111; padding:10px; border-radius:6px; "
        "margin-top:5px;'>Found: <b style='color:#fff'>" +
        foundName + "</b><br><small>" + formatMac(foundMAC) + "</small></div>";
    msg += "<a href='/pair' class='btn-pair'>PAIR DEVICE</a>";
    s.replace("%PAIR_STATUS%", msg);
  } else {
    s.replace("%PAIR_STATUS%", "<p style='color:#666; font-style:italic'>No device found.</p>"
                               "<a href='/scan' class='btn-pair'>SCAN FOR AUTOSTOMP</a>");
  }
  server.send(200, "text/html", s);
}

void OnPairingRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len);
int scanForAutostomp();

void handleScan() {
  newDeviceFound = false;
  int ch = scanForAutostomp();
  if (ch > 0 && newDeviceFound) {
    wifiChannel = ch;
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    server.send(200, "text/html", "<p>No Autostomp found.</p><a href='/'>Back</a>");
  }
}

void handleSave() {
  if (server.hasArg("dp_time"))
    doublePressTime = server.arg("dp_time").toInt();
  if (server.hasArg("lp_time"))
    longPressTime = server.arg("lp_time").toInt();

  for (int i = 0; i < NUM_BUTTONS; i++) {
    String n = String(i + 1);
    if (server.hasArg("t" + n))
      buttons[i].type = server.arg("t" + n).toInt();
    if (server.hasArg("m" + n))
      buttons[i].mode = server.arg("m" + n).toInt();
    if (server.hasArg("cb" + n))
      buttons[i].btCh = server.arg("cb" + n).toInt();
    if (server.hasArg("vb" + n))
      buttons[i].btVal = server.arg("vb" + n).toInt();
    if (server.hasArg("ce" + n))
      buttons[i].espCh = server.arg("ce" + n).toInt();
    if (server.hasArg("ve" + n))
      buttons[i].espVal = server.arg("ve" + n).toInt();
    if (server.hasArg("v" + n))
      buttons[i].value = server.arg("v" + n).toInt();

    if (server.hasArg("lpe" + n))
      buttons[i].lp_enabled = (server.arg("lpe" + n).toInt() == 1);
    if (server.hasArg("lpt" + n))
      buttons[i].lp_type = server.arg("lpt" + n).toInt();
    if (server.hasArg("lpcb" + n))
      buttons[i].lp_btCh = server.arg("lpcb" + n).toInt();
    if (server.hasArg("lpvb" + n))
      buttons[i].lp_btVal = server.arg("lpvb" + n).toInt();
    if (server.hasArg("lpce" + n))
      buttons[i].lp_espCh = server.arg("lpce" + n).toInt();
    if (server.hasArg("lpve" + n))
      buttons[i].lp_espVal = server.arg("lpve" + n).toInt();
    if (server.hasArg("lpv" + n))
      buttons[i].lp_value = server.arg("lpv" + n).toInt();

    if (server.hasArg("dpe" + n))
      buttons[i].dp_enabled = (server.arg("dpe" + n).toInt() == 1);
    if (server.hasArg("dpt" + n))
      buttons[i].dp_type = server.arg("dpt" + n).toInt();
    if (server.hasArg("dpcb" + n))
      buttons[i].dp_btCh = server.arg("dpcb" + n).toInt();
    if (server.hasArg("dpvb" + n))
      buttons[i].dp_btVal = server.arg("dpvb" + n).toInt();
    if (server.hasArg("dpce" + n))
      buttons[i].dp_espCh = server.arg("dpce" + n).toInt();
    if (server.hasArg("dpve" + n))
      buttons[i].dp_espVal = server.arg("dpve" + n).toInt();
    if (server.hasArg("dpv" + n))
      buttons[i].dp_value = server.arg("dpv" + n).toInt();
  }

  for (int i = 0; i < NUM_COMBOS; i++) {
    String n = String(i + 1);
    if (server.hasArg("cen" + n))
      combos[i].enabled = (server.arg("cen" + n).toInt() == 1);
    if (server.hasArg("ct" + n))
      combos[i].type = server.arg("ct" + n).toInt();
    if (server.hasArg("cm" + n))
      combos[i].mode = server.arg("cm" + n).toInt();
    if (server.hasArg("ccb" + n))
      combos[i].btCh = server.arg("ccb" + n).toInt();
    if (server.hasArg("cvb" + n))
      combos[i].btVal = server.arg("cvb" + n).toInt();
    if (server.hasArg("cce" + n))
      combos[i].espCh = server.arg("cce" + n).toInt();
    if (server.hasArg("cve" + n))
      combos[i].espVal = server.arg("cve" + n).toInt();
    if (server.hasArg("cv" + n))
      combos[i].value = server.arg("cv" + n).toInt();
  }

  auto parseLabel = [&](const String& key, char* dst) {
    if (server.hasArg(key)) {
      String val = server.arg(key).substring(0, 20);
      strncpy(dst, val.c_str(), 20); dst[20] = '\0';
    }
  };
  for (int i = 0; i < NUM_BUTTONS; i++) {
    parseLabel("lbl"  + String(i + 1), btnLabel[i]);
    parseLabel("llp"  + String(i + 1), btnLabelLP[i]);
    parseLabel("ldp"  + String(i + 1), btnLabelDP[i]);
  }
  for (int i = 0; i < NUM_COMBOS; i++)
    parseLabel("lcmb" + String(i + 1), comboLabel[i]);

  saveActiveToNVS();
  oledDraw();
  server.send(200, "text/plain", "OK");
}

void handlePreset() {
  if (server.hasArg("act") && server.hasArg("slot")) {
    int slot = server.arg("slot").toInt();
    String act = server.arg("act");
    String keyData = "p" + String(slot) + "_data";
    String keyCombo = "p" + String(slot) + "_cmb";
    String keyName = "p" + String(slot) + "_name";
    preferences.begin("midi_cfg", false);
    if (act == "save") {
      if (server.hasArg("name"))
        preferences.putString(keyName.c_str(), server.arg("name"));
      preferences.putBytes(keyData.c_str(), &buttons, sizeof(buttons));
      preferences.putBytes(keyCombo.c_str(), &combos, sizeof(combos));
    } else if (act == "load") {
      if (preferences.isKey(keyData.c_str())) {
        preferences.getBytes(keyData.c_str(), &buttons, sizeof(buttons));
        if (preferences.isKey(keyCombo.c_str())) {
          preferences.getBytes(keyCombo.c_str(), &combos, sizeof(combos));
        }
        saveActiveToNVS();
      }
    }
    preferences.end();
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}
void handlePair() {
  if (newDeviceFound) {
    preferences.begin("midi_cfg", false);
    preferences.putBytes("rx", foundMAC, 6);
    preferences.end();
    memcpy(receiverAddress, foundMAC, 6);
    memcpy(peerInfo.peer_addr, receiverAddress, 6);
    peerInfo.channel = wifiChannel;
    peerInfo.encrypt = false;
    if (!esp_now_is_peer_exist(receiverAddress))
      esp_now_add_peer(&peerInfo);
    server.send(200, "text/html",
                "<h1>Paired!</h1><p><a href='/'>Back</a></p>");
  } else
    server.send(200, "text/html", "None found.");
}

// --- CHANNEL SCANNING ---
volatile bool scanFoundDevice = false;
volatile uint8_t scanFoundMAC[6];

void onScanRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
  if (len == sizeof(struct_pairing)) {
    struct_pairing pkt;
    memcpy(&pkt, incomingData, sizeof(pkt));
    if (pkt.msgType == 1) {
      scanFoundDevice = true;
      memcpy((void *)scanFoundMAC, mac_addr, 6);
      foundName = String(pkt.name);
      memcpy(foundMAC, mac_addr, 6);
      newDeviceFound = true;
    }
  } else {
    // Fallback: accept any packet as a hit (äldre firmware)
    scanFoundDevice = true;
    memcpy((void *)scanFoundMAC, mac_addr, 6);
  }
}

int scanForAutostomp() {
  // Autostomp-BLE kör alltid ESP-NOW på kanal 1 (låst i espnow_init).
  // AP-läge tillåter inte kanalhopp — pinga bara kanal 1.
  Serial.println("Scanning for Autostomp on ch 1...");
  scanFoundDevice = false;
  newDeviceFound = false;

  esp_now_register_recv_cb(esp_now_recv_cb_t(onScanRecv));

  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 1;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(broadcastAddr))
    esp_now_add_peer(&peer);

  // Skicka flera pingar för att öka chansen att Autostomp tar emot
  for (int i = 0; i < 5; i++) {
    struct_pairing ping;
    ping.msgType = 99;
    strncpy(ping.name, "MIDI-SCAN", 31);
    esp_now_send(broadcastAddr, (uint8_t *)&ping, sizeof(ping));

    unsigned long startWait = millis();
    while (millis() - startWait < 200) {
      delay(10);
      if (scanFoundDevice) {
        Serial.println("Found Autostomp!");
        esp_now_register_recv_cb(esp_now_recv_cb_t(OnPairingRecv));
        return 1;
      }
    }
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(OnPairingRecv));
  Serial.println("No Autostomp found");
  return -1;
}

void handleSaveChannel() {
  if (server.hasArg("ch")) {
    int ch = server.arg("ch").toInt();
    if (ch >= 0 && ch <= 11) {
      if (ch == 0) {
        autoChannelEnabled = 1;
        Serial.println("Auto channel enabled");
      } else {
        autoChannelEnabled = 0;
        wifiChannel = ch;
        Serial.printf("Manual channel set to %d\n", ch);
      }
      saveActiveToNVS();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(500);
  ESP.restart();
}
void OnPairingRecv(const uint8_t *mac_addr, const uint8_t *incomingData,
                   int len) {
  Serial.printf("ESP-NOW rx: len=%d\n", len);
  if (len != sizeof(pairingData)) return;
  memcpy(&pairingData, incomingData, sizeof(pairingData));
  if (pairingData.msgType == 1) {
    // Autostomp svarade på vår scan
    newDeviceFound = true;
    memcpy(foundMAC, mac_addr, 6);
    foundName = String(pairingData.name);
    memcpy(s_pendingReceiver, mac_addr, 6);
    s_saveReceiverPending = true;  // spara i loop() istället för här
    Serial.printf("Autostomp found: %02X:%02X:%02X:%02X:%02X:%02X\n",
      mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
  } else if (pairingData.msgType == 99) {
    // Autostomp scannar efter oss — svara och schemalägga sparning
    memcpy(foundMAC, mac_addr, 6);
    newDeviceFound = true;
    memcpy(s_pendingReceiver, mac_addr, 6);
    s_saveReceiverPending = true;  // spara i loop() istället för här
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac_addr, 6);
    peer.channel = 1;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(mac_addr))
      esp_now_add_peer(&peer);
    struct_pairing resp;
    resp.msgType = 1;
    strncpy(resp.name, "MIDI-PEDAL-PRO", sizeof(resp.name) - 1);
    resp.name[sizeof(resp.name) - 1] = '\0';
    esp_now_send(mac_addr, (uint8_t*)&resp, sizeof(resp));
    Serial.printf("Svarade på Autostomp-scan: %02X:%02X:%02X:%02X:%02X:%02X\n",
      mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],mac_addr[4],mac_addr[5]);
  }
}

// --- KNAPP LOGIK & ROUTING ---
void sendPacket(int type, int btCh, int btVal, int espCh, int espVal,
                int value) {
  sendToBLE(type, btCh, btVal, value);
  sendToESP(type, espCh, espVal, value);
}

void triggerShortAction(int btnIndex, bool pressed) {
  ButtonConfig cfg = buttons[btnIndex];
  int valToSend = 0;

  if (cfg.mode == 2 && pressed) {
    buttonLatchState[btnIndex] = !buttonLatchState[btnIndex];
    valToSend = buttonLatchState[btnIndex] ? cfg.value : 0;
  } else if (cfg.mode == 1) {
    valToSend = pressed ? cfg.value : 0;
  } else {
    if (pressed)
      valToSend = cfg.value;
    else
      return;
  }

  if (cfg.type == 1 && pressed)
    valToSend = 0;
  sendPacket(cfg.type, cfg.btCh, cfg.btVal, cfg.espCh, cfg.espVal, valToSend);

  if (pressed) {
    blinkLedFast();
  }
}

void triggerComboAction(int comboIndex, bool pressed) {
  ComboConfig cfg = combos[comboIndex];
  int valToSend = 0;

  if (cfg.mode == 2 && pressed) {
    comboLatchState[comboIndex] = !comboLatchState[comboIndex];
    valToSend = comboLatchState[comboIndex] ? cfg.value : 0;
  } else if (cfg.mode == 1) {
    valToSend = pressed ? cfg.value : 0;
  } else {
    if (pressed)
      valToSend = cfg.value;
    else
      return;
  }

  if (cfg.type == 1 && pressed)
    valToSend = 0;
  sendPacket(cfg.type, cfg.btCh, cfg.btVal, cfg.espCh, cfg.espVal, valToSend);

  if (pressed) {
    blinkLedFast();
  }
}

void triggerLongAction(int btnIndex) {
  ButtonConfig cfg = buttons[btnIndex];
  Serial.printf("Long Press Triggered on BTN %d\n", btnIndex + 1);
  int val = (cfg.lp_type == 1) ? 0 : cfg.lp_value;
  sendPacket(cfg.lp_type, cfg.lp_btCh, cfg.lp_btVal, cfg.lp_espCh,
             cfg.lp_espVal, val);
  blinkLedFast();
}

void triggerDoubleAction(int btnIndex) {
  ButtonConfig cfg = buttons[btnIndex];
  Serial.printf("Double Press Triggered on BTN %d\n", btnIndex + 1);
  int val = (cfg.dp_type == 1) ? 0 : cfg.dp_value;
  sendPacket(cfg.dp_type, cfg.dp_btCh, cfg.dp_btVal, cfg.dp_espCh,
             cfg.dp_espVal, val);
  blinkLedFast();
}

void checkCombos() {
  // 1. Släpp combo om knapparna släpps
  for (int i = 0; i < NUM_COMBOS; i++) {
    int btnA = i;
    int btnB = i + 1;

    if (comboActive[i]) {
      if (stableState[btnA] == HIGH || stableState[btnB] == HIGH) {
        if (comboWasStandard[i] && combos[i].mode == 1) {
          triggerComboAction(i, false);
        }
        comboActive[i] = false;
        comboWasStandard[i] = false;
        oledComboPressed[i] = false;
        oledDraw();
        // Återställ till IDLE så att "release" i checkButton inte flippar ur
        btnState[btnA] = IDLE;
        btnState[btnB] = IDLE;
      }
    }
  }

  // 2. Detektera ny combo
  for (int i = 0; i < NUM_COMBOS; i++) {
    if (!combos[i].enabled)
      continue;

    int btnA = i;
    int btnB = i + 1;

    // Vi kollar om båda knapparna är fysiskt nere
    bool aDown = (stableState[btnA] == LOW);
    bool bDown = (stableState[btnB] == LOW);

    if (aDown && bDown && !comboActive[i]) {
      // Kontrollera att ingen av dem redan är upptagna i en ANNAN combo
      if (btnState[btnA] != BLOCKED_BY_COMBO &&
          btnState[btnB] != BLOCKED_BY_COMBO) {
        // VIKTIGT: Förhindra att singel-press skickas!
        // Om de ligger i WAIT_COMBO eller IDLE är det lugnt.
        // Om de redan gått till ACTIVE_HOLD (för sent) så missar vi combon,
        // men tack vare latensen (WAIT_COMBO) hinner vi oftast fånga dem här.

        Serial.printf("Combo %d Triggered\n", i + 1);
        triggerComboAction(i, true);
        comboActive[i] = true;
        comboWasStandard[i] = true;
        oledBtnPressed[btnA] = false;
        oledBtnPressed[btnB] = false;
        oledComboPressed[i] = true;
        oledDraw();

        btnState[btnA] = BLOCKED_BY_COMBO;
        btnState[btnB] = BLOCKED_BY_COMBO;
      }
    }
  }
}

void checkButton(int btnIndex) {
  int pin = btnPins[btnIndex];
  int reading = digitalRead(pin);
  unsigned long now = millis();
  ButtonConfig cfg = buttons[btnIndex];

  if (reading != lastRawState[btnIndex]) {
    lastDebounceTime[btnIndex] = now;
  }
  lastRawState[btnIndex] = reading;

  if ((now - lastDebounceTime[btnIndex]) > debounceDelay) {
    if (reading != stableState[btnIndex]) {
      stableState[btnIndex] = reading;
      oledBtnPressed[btnIndex] = (reading == LOW);
      oledDraw();

      // --- PRESS (HIGH -> LOW) ---
      if (stableState[btnIndex] == LOW) {

        if (btnState[btnIndex] == BLOCKED_BY_COMBO)
          return;

        // 1. SINGLE PRESS -> GÅ TILL WAIT_COMBO ISTÄLLET FÖR DIREKT TRIGG
        if (btnState[btnIndex] == IDLE && !cfg.dp_enabled && !cfg.lp_enabled) {
          stateTimer[btnIndex] = now;
          btnState[btnIndex] = WAIT_COMBO; // <--- HÄR ÄR NYCKELN
        }

        // 2. LONG PRESS ONLY
        else if (btnState[btnIndex] == IDLE && !cfg.dp_enabled &&
                 cfg.lp_enabled) {
          stateTimer[btnIndex] = now;
          btnState[btnIndex] = WAIT_LONG;
        }

        // 3. DOUBLE PRESS
        else if (btnState[btnIndex] == IDLE && cfg.dp_enabled) {
          stateTimer[btnIndex] = now;
          btnState[btnIndex] = WAIT_DBL;
        } else if (btnState[btnIndex] == WAIT_DBL) {
          triggerDoubleAction(btnIndex);
          btnState[btnIndex] = ACTIVE_HOLD;
        }

        if (cfg.type == 4) { /* Special logic handled elsewhere if needed */
        }
      }

      // --- RELEASE (LOW -> HIGH) ---
      else if (stableState[btnIndex] == HIGH) {
        if (btnState[btnIndex] == BLOCKED_BY_COMBO) {
          btnState[btnIndex] = IDLE;
          return;
        }

        // Om vi släpper knappen INNAN combo-tiden gått ut -> Skicka Singel
        if (btnState[btnIndex] == WAIT_COMBO) {
          triggerShortAction(btnIndex, true);
          if (cfg.mode == 1) {
            delay(10);
            triggerShortAction(btnIndex, false);
          }
          btnState[btnIndex] = IDLE;
        }

        else if (btnState[btnIndex] == ACTIVE_HOLD) {
          if (activeWasStandard[btnIndex] && cfg.mode == 1) {
            triggerShortAction(btnIndex, false);
          }
          activeWasStandard[btnIndex] = false;
          btnState[btnIndex] = IDLE;
        } else if (btnState[btnIndex] == WAIT_LONG) {
          triggerShortAction(btnIndex, true);
          if (cfg.mode == 1) {
            delay(20);
            triggerShortAction(btnIndex, false);
          }
          btnState[btnIndex] = IDLE;
        }
      }
    }
  }

  // --- TIME BASED CHECKS ---
  if (btnState[btnIndex] == BLOCKED_BY_COMBO)
    return;

  // 1. WAIT_COMBO TIMEOUT -> Skicka Singel
  if (btnState[btnIndex] == WAIT_COMBO) {
    // Om tiden gått och vi inte blivit blockerad av en combo -> SKICKA NU
    if (now - stateTimer[btnIndex] > comboDelay) {
      triggerShortAction(btnIndex, true);
      activeWasStandard[btnIndex] = true;
      btnState[btnIndex] = ACTIVE_HOLD;
    }
  }

  // 2. LONG PRESS TIMEOUT
  if (btnState[btnIndex] == WAIT_LONG) {
    if (now - stateTimer[btnIndex] > longPressTime) {
      triggerLongAction(btnIndex);
      activeWasStandard[btnIndex] = false;
      btnState[btnIndex] = ACTIVE_HOLD;
    }
  }

  // 3. DOUBLE PRESS TIMEOUT
  if (btnState[btnIndex] == WAIT_DBL) {
    if (now - stateTimer[btnIndex] > doublePressTime) {
      if (stableState[btnIndex] == HIGH) {
        triggerShortAction(btnIndex, true);
        if (cfg.mode == 1) {
          delay(20);
          triggerShortAction(btnIndex, false);
        }
        btnState[btnIndex] = IDLE;
      } else {
        if (cfg.lp_enabled) {
          btnState[btnIndex] = WAIT_LONG;
        } else {
          triggerShortAction(btnIndex, true);
          activeWasStandard[btnIndex] = true;
          btnState[btnIndex] = ACTIVE_HOLD;
        }
      }
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(3000);
  Serial.println("=== BOOT ===");
  ledcSetup(PWM_CHAN, PWM_FREQ, PWM_RES);
  ledcAttachPin(ledPin, PWM_CHAN);
  updateLed(false);
  for (int i = 0; i < NUM_BUTTONS; i++)
    pinMode(btnPins[i], INPUT_PULLUP);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("Wakeup detected... Checking keys.");

    bool authorized = false;
    unsigned long checkStart = millis();

    // Vi ger användaren 500ms på sig att ha BÅDA knapparna nere samtidigt
    while (millis() - checkStart < 500) {
      if (digitalRead(btnPins[0]) == LOW && digitalRead(btnPins[3]) == LOW) {
        authorized = true;
        break;
      }
      delay(10);
    }

    if (authorized) {
      Serial.println("Wakeup Confirmed (Both Keys pressed)");
    } else {
      Serial.println("False Wakeup -> Sleeping again");
      for (int i = 0; i < 5; i++) {
        updateLed(true);
        delay(30);
        updateLed(false);
        delay(30);
      }
      gpio_wakeup_enable((gpio_num_t)btnPins[0], GPIO_INTR_LOW_LEVEL);
      gpio_wakeup_enable((gpio_num_t)btnPins[3], GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      esp_deep_sleep_start();
    }
  }

  loadActiveSettings();
  { static const uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    oled_esp = (memcmp(receiverAddress, bcast, 6) != 0); }
  initAutostompPreset(); // Initialize Autostomp preset if it doesn't exist
  preferences.begin("midi_cfg", false);
  bool forceConfig = preferences.getBool("force_cfg", false);
  if (forceConfig)
    preferences.putBool("force_cfg", false);
  preferences.end();

  if (digitalRead(btnPins[1]) == LOW || forceConfig) {
    currentMode = MODE_CONFIG;
    Serial.println("CONFIG MODE");
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("MIDI-PEDAL-PRO", NULL, 1, 0, 4); // kanal 1 = samma som Autostomp ESP-NOW
    if (esp_now_init() == ESP_OK) {
      esp_now_register_recv_cb(esp_now_recv_cb_t(OnPairingRecv));
      memcpy(peerInfo.peer_addr, receiverAddress, 6);
      peerInfo.channel = wifiChannel;
      peerInfo.encrypt = false;
      if (!esp_now_is_peer_exist(receiverAddress))
        esp_now_add_peer(&peerInfo);
    }
    dnsServer.start(53, "*", WiFi.softAPIP());
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/savech", handleSaveChannel);
    server.on("/preset", handlePreset);
    server.on("/pair", handlePair);
    server.on("/scan", handleScan);
    server.on("/reboot", handleReboot);
    server.begin();

    oledInit();
    // Tre långsamma fadeande blink vid start av setup
    fadeLed(3, 10);
  } else {
    currentMode = MODE_LIVE;
    Serial.println("LIVE MODE");
    // BLE måste initas före WiFi/ESP-NOW på ESP32-C3 (delar radio)
    NimBLEDevice::init("MIDI_PEDAL_PRO");
    NimBLEDevice::deleteAllBonds(); // Rensa gamla bonds — ska inte visas i iOS BT-inställningar
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(false, false, false);
    pGlobalServer = NimBLEDevice::createServer();
    pGlobalServer->setCallbacks(new MyServerCallbacks());
    NimBLEService *pService =
        pGlobalServer->createService("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    pCharacteristic = pService->createCharacteristic(
        "7772e5db-3868-4112-a1a9-f2669d106bf3", NIMBLE_PROPERTY::READ |
                                                    NIMBLE_PROPERTY::NOTIFY |
                                                    NIMBLE_PROPERTY::WRITE_NR);
    pService->start();
    NimBLEDevice::getAdvertising()->addServiceUUID("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    NimBLEDevice::getAdvertising()->start();

    // WiFi/ESP-NOW efter BLE — coex kräver denna ordning på ESP32-C3
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    // WIFI_PS_NONE aborterar om BLE är igång på ESP32-C3 — använd MIN_MODEM
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (esp_now_init() == ESP_OK) {
      esp_now_register_recv_cb(esp_now_recv_cb_t(OnPairingRecv));
      memcpy(peerInfo.peer_addr, receiverAddress, 6);
      peerInfo.channel = 1;
      peerInfo.ifidx = WIFI_IF_STA;
      peerInfo.encrypt = false;
      if (!esp_now_is_peer_exist(receiverAddress))
        esp_now_add_peer(&peerInfo);
      Serial.println("ESP-NOW OK");
    } else {
      Serial.println("ESP-NOW FAILED");
    }

    oledInit();
    for (int i = 0; i < 3; i++) {
      updateLed(true);
      delay(300);
      updateLed(false);
      delay(300);
    }
  }
}

void loop() {
  // Batteriavläsning var 10:e sekund
  static unsigned long lastBatCheck = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastBatCheck > 10000) {
    lastBatCheck = nowMs;
    int raw = analogRead(BAT_PIN);
    float v = 3.0f + (float)(raw - ADC_BAT_MIN) * 1.2f / (ADC_BAT_MAX - ADC_BAT_MIN);
    int bars;
    if      (v >= 4.15f) bars = 10;
    else if (v >= 4.08f) bars = 9;
    else if (v >= 4.00f) bars = 8;
    else if (v >= 3.92f) bars = 7;
    else if (v >= 3.86f) bars = 6;
    else if (v >= 3.80f) bars = 5;
    else if (v >= 3.74f) bars = 4;
    else if (v >= 3.68f) bars = 3;
    else if (v >= 3.58f) bars = 2;
    else if (v >= 3.45f) bars = 1;
    else                 bars = 0;
    if (bars != oledBatBars) { oledBatBars = bars; oledDraw(); }
  }

  if (currentMode == MODE_CONFIG) {
    dnsServer.processNextRequest();
    server.handleClient();
    breathingLed(); // "Andas" i setup-läge
  }
  if (oledNeedsUpdate) { oledNeedsUpdate = false; oledDraw(); }
  checkSystemCombos();
  updateMidiClock();

  // POLL CONNECTION STATUS (Backup if callback fails)
  if (currentMode == MODE_LIVE && pGlobalServer != NULL) {
    if (pGlobalServer->getConnectedCount() > 0) {
      if (!deviceConnected) {
        deviceConnected = true;
        updateLed(false); // Update to DIM
      }
    } else {
      if (deviceConnected) {
        deviceConnected = false;
        updateLed(false); // Update to OFF
      }
    }
  }

  // Spara Autostomps MAC i NVS (deferred från WiFi-callback)
  if (s_saveReceiverPending) {
    s_saveReceiverPending = false;
    memcpy(receiverAddress, s_pendingReceiver, 6);
    preferences.begin("midi_cfg", false);
    preferences.putBytes("rx", receiverAddress, 6);
    preferences.end();
    // Uppdatera ESP-NOW peer: ta bort broadcast, lägg till rätt adress
    uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (esp_now_is_peer_exist(bcast)) esp_now_del_peer(bcast);
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, receiverAddress, 6);
    peer.channel = 1;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(receiverAddress))
      esp_now_add_peer(&peer);
    Serial.printf("Autostomp MAC sparad: %02X:%02X:%02X:%02X:%02X:%02X\n",
      receiverAddress[0],receiverAddress[1],receiverAddress[2],
      receiverAddress[3],receiverAddress[4],receiverAddress[5]);
    oled_esp = true;
    oledDraw();
  }

  // ESP-NOW discovery: skicka broadcast-ping tills Autostomp svarat
  static const uint8_t bcastMac[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  if (currentMode == MODE_LIVE && memcmp(receiverAddress, bcastMac, 6) == 0) {
    static uint32_t lastPingMs = 0;
    if (millis() - lastPingMs > 2000) {
      lastPingMs = millis();
      uint8_t bcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, bcast, 6);
      peer.channel = 1;
      peer.encrypt = false;
      if (!esp_now_is_peer_exist(bcast))
        esp_now_add_peer(&peer);
      struct_pairing ping;
      ping.msgType = 99;
      strncpy(ping.name, "MIDI-PEDAL-PRO", sizeof(ping.name) - 1);
      esp_now_send(bcast, (uint8_t*)&ping, sizeof(ping));
      Serial.println("ESP-NOW ping...");
    }
  }

  if (tempoModeActive) {
    if (millis() - tempoModeExitTimer > 3000) {
      tempoModeActive = false;
      saveBPM();
      Serial.println("EXIT TEMPO MODE");
      updateLed(true);
      delay(500);
      updateLed(false);
    } else {
      if (millis() % 200 < 50)
        updateLed(true);
      else
        updateLed(false);
    }
  }

  // Kör combo check innan knappcheck för att kunna blockera
  checkCombos();

  for (int i = 0; i < NUM_BUTTONS; i++)
    checkButton(i);
}
