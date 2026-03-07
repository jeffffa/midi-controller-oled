#include "driver/gpio.h"
#include "index_html.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// --- KONFIGURATION ---
const int NUM_BUTTONS = 4;
const int NUM_COMBOS = 3; // 1+2, 2+3, 3+4
const int btnPins[NUM_BUTTONS] = {2, 3, 4, 5};
const int ledPin = 8;
int wifiChannel = 1;        // Current WiFi channel (1-11)
int autoChannelEnabled = 1; // 0 = manual, 1 = auto-scan

// --- GLOBALA TIDSINSTÄLLNINGAR (Standardvärden) ---
unsigned long doublePressTime = 200; // Standard 200ms
unsigned long longPressTime = 1500;  // Standard 1500ms
const unsigned long comboDelay = 45; // HUR LÄNGE VI VÄNTAR PÅ COMBO (ms)

// --- DATASTRUKTURER ---
struct ButtonConfig {
  // SHORT PRESS (Standard / Combo Action)
  uint8_t type; // 0=CC, 1=PC, 2=TAP, 3=CLK, 4=CC+, 5=NOTE
  uint8_t mode; // 0=Trigger, 1=Momentary, 2=Latch
  uint8_t btCh;
  uint8_t btVal;
  uint8_t espCh;
  uint8_t espVal;
  uint8_t value;

  // LONG PRESS
  bool lp_enabled;
  uint8_t lp_type;
  uint8_t lp_btCh;
  uint8_t lp_btVal;
  uint8_t lp_espCh;
  uint8_t lp_espVal;
  uint8_t lp_value;

  // DOUBLE PRESS
  bool dp_enabled;
  uint8_t dp_type;
  uint8_t dp_btCh;
  uint8_t dp_btVal;
  uint8_t dp_espCh;
  uint8_t dp_espVal;
  uint8_t dp_value;
};

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
  void onConnect(NimBLEServer *pServer) {
    Serial.println("BLE Device Connected");
    deviceConnected = true;
    updateLed(false);
  };
  void onDisconnect(NimBLEServer *pServer) {
    Serial.println("BLE Device Disconnected");
    deviceConnected = false;
    updateLed(false);
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

enum Mode { MODE_LIVE, MODE_CONFIG };
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
    ledcWrite(PWM_CHAN, 255); // ON
    delay(300);
    ledcWrite(PWM_CHAN, 0); // OFF
    delay(300);
  }
  uint64_t mask = (1ULL << btnPins[0]) | (1ULL << btnPins[3]);
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
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
      // Om vi hållit in båda i mer än 1500ms -> STANDBY
      if (millis() - bothHeldStart > 1500) {
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
  myData.id = 1;
  myData.type = type;
  myData.number = number;
  myData.value = value;
  myData.channel = channel;
  esp_now_send(receiverAddress, (uint8_t *)&myData, sizeof(myData));
}

void sendMidiClockByte(uint8_t msg) {
  if (currentMode == MODE_LIVE && pCharacteristic != NULL) {
    uint8_t pkt[] = {0x80, 0x80, msg};
    pCharacteristic->setValue(pkt, 3);
    pCharacteristic->notify();
  }
  myData.id = 1;
  myData.type = 8;
  myData.number = 0;
  myData.value = msg;
  myData.channel = 0;
  esp_now_send(receiverAddress, (uint8_t *)&myData, sizeof(myData));
}

void updateMidiClock() {
  if (!midiClockEnabled)
    return;
  unsigned long currentMicros = micros();
  if (currentMicros - lastClockMicros >= clockIntervalMicros) {
    lastClockMicros = currentMicros;
    sendMidiClockByte(0xF8);
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
        sendMidiClockByte(0xFA);
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
    html +=
        "<div class='btn-card'><div class='btn-header'>BUTTON " + n + "</div>";

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
            "' onchange='sendData(this)'></span></div></div>";

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
            "' onchange='sendData(this)'></span></div></div>";

    html += "</div>";
  }

  for (int i = 0; i < NUM_COMBOS; i++) {
    String n = String(i + 1);
    String label = (i == 0) ? "1 + 2" : (i == 1) ? "2 + 3" : "3 + 4";
    String c_checked = combos[i].enabled ? "checked" : "";
    String c_display = combos[i].enabled ? "block" : "none";

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
    html += "</div></div>";
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
    s.replace("%PAIR_STATUS%", "<p style='color:#666; font-style:italic'>Press "
                               "pair button on receiver...</p>");
  }
  server.send(200, "text/html", s);
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

  saveActiveToNVS();
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
  // Any ESP-NOW packet means we found a device on this channel
  scanFoundDevice = true;
  memcpy((void *)scanFoundMAC, mac_addr, 6);
}

int scanForAutostomp() {
  Serial.println("Scanning for Autostomp...");
  scanFoundDevice = false;

  // Save current ESP-NOW state
  esp_now_deinit();

  for (int ch = 1; ch <= 11; ch++) {
    Serial.printf("Trying channel %d...\n", ch);

    // Set channel
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    // Init ESP-NOW on this channel
    if (esp_now_init() != ESP_OK)
      continue;
    esp_now_register_recv_cb(esp_now_recv_cb_t(onScanRecv));

    // Send broadcast to trigger response
    uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcastAddr, 6);
    peer.channel = ch;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(broadcastAddr)) {
      esp_now_add_peer(&peer);
    }

    // Send a ping
    struct_pairing ping;
    ping.msgType = 99; // Scan ping
    strncpy(ping.name, "MIDI-SCAN", 31);
    esp_now_send(broadcastAddr, (uint8_t *)&ping, sizeof(ping));

    // Wait for response
    unsigned long startWait = millis();
    while (millis() - startWait < 200) {
      delay(10);
      if (scanFoundDevice) {
        Serial.printf("Found device on channel %d!\n", ch);
        esp_now_deinit();
        return ch;
      }
    }

    esp_now_del_peer(broadcastAddr);
    esp_now_deinit();
  }

  Serial.println("No device found during scan");
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
  if (len == sizeof(pairingData)) {
    memcpy(&pairingData, incomingData, sizeof(pairingData));
    if (pairingData.msgType == 1) {
      newDeviceFound = true;
      memcpy(foundMAC, mac_addr, 6);
      foundName = String(pairingData.name);
    }
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
  delay(1000);
  Serial.begin(115200);
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
      uint64_t mask = (1ULL << btnPins[0]) | (1ULL << btnPins[3]);
      esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  }

  loadActiveSettings();
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
    WiFi.softAP("MIDI-PEDAL-PRO", NULL, wifiChannel, 0, 4);
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
    server.on("/reboot", handleReboot);
    server.begin();

    // Tre långsamma fadeande blink vid start av setup
    fadeLed(3, 10);
  } else {
    currentMode = MODE_LIVE;
    Serial.println("LIVE MODE");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    if (esp_now_init() == ESP_OK) {
      memcpy(peerInfo.peer_addr, receiverAddress, 6);
      peerInfo.channel = wifiChannel;
      peerInfo.encrypt = false;
      if (!esp_now_is_peer_exist(receiverAddress))
        esp_now_add_peer(&peerInfo);
    }
    NimBLEDevice::init("MIDI_PEDAL_PRO");
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
    NimBLEDevice::getAdvertising()->addServiceUUID(
        "03b80e5a-ede8-4b33-a751-6ce34ec4c700");
    NimBLEDevice::getAdvertising()->start();
    for (int i = 0; i < 3; i++) {
      updateLed(true);
      delay(300);
      updateLed(false);
      delay(300);
    }
  }
}

void loop() {
  if (currentMode == MODE_CONFIG) {
    dnsServer.processNextRequest();
    server.handleClient();
    breathingLed(); // "Andas" i setup-läge
  }
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
