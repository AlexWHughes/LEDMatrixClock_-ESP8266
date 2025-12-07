/*
  ------------------------------------------------------------
  LED Matrix WiFi Clock (ESP8266)
  Author: Alex Hughes @alexwhughes
  
  Hardware:
    - ESP8266 (e.g. Wemos D1 mini / NodeMCU / custom ESP8266MOD)
    - 32x8 LED matrix (4× MAX7219 modules in a chain)

  Features:
    - NTP time synchronization
    - WiFiManager config portal for initial WiFi setup
    - Web UI for configuration (brightness, time format, sleep times, etc.)
    - Sleep mode: display off during configured hours (e.g., 18:30 to 9:00)
    - 12/24 hour time format
    - Brightness control (0-100)
    - Stores all settings in EEPROM

  Config behaviour:
    - WiFi fails => WiFiManager config portal (standard)
    - Reset during 5s "CONFIG?" window => next boot triggers config portal
  ------------------------------------------------------------
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <time.h>
#include <string.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ------------------------ MATRIX CONFIG ---------------------

#define HARDWARE_TYPE MD_MAX72XX::PAROLA_HW
#define MAX_DEVICES   4
// Note: Each MAX7219 is 8x8, so 4 devices = 32 columns x 8 rows

// Pins for ESP8266 (numeric GPIOs)
#define PIN_CS   15    // D8 on many dev boards
#define PIN_CLK  14    // D5
#define PIN_DIN  13    // D7
#define PIN_ORIENTATION  16    // D0 - Orientation detection pin (LOW = flip display vertically)

MD_Parola P = MD_Parola(HARDWARE_TYPE, PIN_CS, MAX_DEVICES);

// Global text buffer that Parola will use (must stay valid!)
char matrixText[96];

// Track whether current text is scrolling or static
bool textScrolls = false;

// Parola timing
const uint16_t scrollSpeed = 40;
const uint16_t scrollPause = 0;

// Boot prompt timing
const unsigned long CONFIG_PROMPT_MS = 5000;

// Display mode enum
enum DisplayMode {
  MODE_CLOCK,
  MODE_WEATHER,
  MODE_DATE,
  MODE_CUSTOM_MESSAGE
};

// ------------------------ CONFIGURATION ---------------------

WiFiManager wifiManager;
ESP8266WebServer server(80);

// Device ID and hostname
String deviceID;

// AP password for config portal (blank = open)
const char* AP_password = "";

// EEPROM layout
// [0-1] = 'L', 'M' magic
// [2] = version (3 = with weather/messages)
// [3] = brightness (0-100)
// [4] = timeFormat (0=24h, 1=12h)
// [5] = invertDisplay (0=false, 1=true)
// [6] = sleepEnabled (0=false, 1=true)
// [7] = sleepStartHour (0-23)
// [8] = sleepStartMinute (0-59)
// [9] = sleepEndHour (0-23)
// [10] = sleepEndMinute (0-59)
// [11-50] = ntpServer (40 bytes)
// [51-90] = ntpServer2 (40 bytes, secondary NTP server)
// [91-94] = timezone offset (int32_t, seconds)
// [95-102] = last known timestamp (8 bytes, time_t as uint64_t)
// [103] = bootCounter (moved from 100 to avoid overlapping with timestamp)
// [104] = showDayOfWeek (0=false, 1=true)
// [105] = showDate (0=false, 1=true)
// [106] = blinkingColon (0=false, 1=true)
// [107-108] = clockDisplayDuration (uint16_t, seconds)
// [109-110] = weatherDisplayDuration (uint16_t, seconds)
// [111-150] = weatherAPIKey (40 bytes)
// [151-190] = weatherLocation (40 bytes, lat,lon or city name)
// [191-230] = customMessage (40 bytes)
// [231] = customMessageEnabled (0=false, 1=true)
// [232-233] = customMessageScrollSpeed (uint16_t, 10-200)
// [234] = clockScrollEnabled (0=false, 1=true) - version 4+
const uint16_t EEPROM_SIZE = 256;
const uint16_t EEPROM_TIME_ADDR = 95;
const uint16_t EEPROM_BOOT_ADDR = 103;

// Default configuration
struct Config {
  uint8_t brightness;
  uint8_t timeFormat;      // 0=24h, 1=12h
  bool invertDisplay;
  bool sleepEnabled;
  uint8_t sleepStartHour;
  uint8_t sleepStartMinute;
  uint8_t sleepEndHour;
  uint8_t sleepEndMinute;
  char ntpServer[40];
  char ntpServer2[40];  // Secondary NTP server
  int32_t timezoneOffset;  // seconds offset from UTC
  bool showDayOfWeek;
  bool showDate;
  bool blinkingColon;
  bool clockScrollEnabled;  // Allow clock to scroll if text doesn't fit
  uint16_t clockDisplayDuration;  // seconds
  uint16_t weatherDisplayDuration;  // seconds
  char weatherAPIKey[40];
  char weatherLocation[40];
  char customMessage[40];
  bool customMessageEnabled;
  uint16_t customMessageScrollSpeed;
};

Config config;

void initConfigDefaults() {
  config.brightness = 50;
  config.timeFormat = 0;  // 24h
  config.invertDisplay = false;
  config.sleepEnabled = true;
  config.sleepStartHour = 18;
  config.sleepStartMinute = 30;
  config.sleepEndHour = 9;
  config.sleepEndMinute = 0;
  strcpy(config.ntpServer, "pool.ntp.org");
  strcpy(config.ntpServer2, "");
  config.timezoneOffset = 0;
  config.showDayOfWeek = false;  // Disabled by default
  config.showDate = false;
  config.blinkingColon = true;
  config.clockScrollEnabled = false;  // Disable scrolling by default (static display only)
  config.clockDisplayDuration = 10;
  config.weatherDisplayDuration = 5;
  strcpy(config.weatherAPIKey, "");
  strcpy(config.weatherLocation, "");
  strcpy(config.customMessage, "");
  config.customMessageEnabled = false;
  config.customMessageScrollSpeed = 40;
}

// Boot counter for config portal trigger
const uint8_t BOOT_FAIL_LIMIT = 1;
uint8_t bootCountCached = 0;

// Time sync
unsigned long lastTimeUpdate = 0;
const unsigned long timeUpdateInterval = 3600000; // 1 hour
bool timeSynced = false;

// Stored time tracking (for battery-backed operation)
time_t storedTime = 0;           // Last known time from NTP
unsigned long storedTimeMillis = 0;  // millis() when storedTime was valid
bool hasStoredTime = false;       // Whether we have a valid stored time

// Display state
bool displaySleeping = false;
unsigned long lastTimeDisplay = 0;
const unsigned long timeDisplayInterval = 1000; // Update every second
DisplayMode currentDisplayMode = MODE_CLOCK;
unsigned long modeStartTime = 0;
  // Track last displayed time to avoid resetting scroll animation unnecessarily
int lastDisplayedHour = -1;
int lastDisplayedMinute = -1;
bool colonBlinkState = false;
unsigned long lastColonBlink = 0;
const unsigned long colonBlinkInterval = 500; // 500ms blink
bool displayFlipped = false;  // Orientation flip state (read from GPIO16)
unsigned long lastOrientationCheck = 0;
const unsigned long orientationCheckInterval = 2000; // Check every 2 seconds

// Time save interval (save to EEPROM every 5 minutes to preserve time across power loss)
unsigned long lastTimeSave = 0;
const unsigned long timeSaveInterval = 300000; // 5 minutes

// Weather state
float currentTemperature = 0.0;
bool weatherAvailable = false;
unsigned long lastWeatherUpdate = 0;
const unsigned long weatherUpdateInterval = 300000; // 5 minutes
String weatherDescription = "";

// ------------------------ EEPROM HELPERS --------------------

void eepromLoadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  uint8_t magic1 = EEPROM.read(0);
  uint8_t magic2 = EEPROM.read(1);
  
  if (magic1 == 'L' && magic2 == 'M') {
    uint8_t version = EEPROM.read(2);
    
    // Valid header - load and validate values
    uint8_t brightness = EEPROM.read(3);
    config.brightness = (brightness <= 100) ? brightness : 50;  // Validate 0-100
    
    uint8_t timeFormat = EEPROM.read(4);
    config.timeFormat = (timeFormat <= 1) ? timeFormat : 0;  // Validate 0-1
    
    config.invertDisplay = EEPROM.read(5) != 0;
    config.sleepEnabled = EEPROM.read(6) != 0;
    
    uint8_t sleepStartHour = EEPROM.read(7);
    config.sleepStartHour = (sleepStartHour <= 23) ? sleepStartHour : 18;
    
    uint8_t sleepStartMinute = EEPROM.read(8);
    config.sleepStartMinute = (sleepStartMinute <= 59) ? sleepStartMinute : 30;
    
    uint8_t sleepEndHour = EEPROM.read(9);
    config.sleepEndHour = (sleepEndHour <= 23) ? sleepEndHour : 9;
    
    uint8_t sleepEndMinute = EEPROM.read(10);
    config.sleepEndMinute = (sleepEndMinute <= 59) ? sleepEndMinute : 0;
    
    // NTP server
    for (int i = 0; i < 40; i++) {
      config.ntpServer[i] = (char)EEPROM.read(11 + i);
    }
    config.ntpServer[39] = '\0';
    
    // Secondary NTP server (version 3+)
    if (version >= 3) {
      for (int i = 0; i < 40; i++) {
        config.ntpServer2[i] = (char)EEPROM.read(51 + i);
      }
      config.ntpServer2[39] = '\0';
    } else {
      config.ntpServer2[0] = '\0';
    }
    
    // Timezone offset
    int32_t offset = 0;
    uint16_t offsetAddr = (version >= 3) ? 91 : 51;
    for (int i = 0; i < 4; i++) {
      offset |= ((int32_t)EEPROM.read(offsetAddr + i)) << (i * 8);
    }
    config.timezoneOffset = offset;
    
    // New fields (version 3+)
    if (version >= 3) {
      config.showDayOfWeek = EEPROM.read(104) != 0;
      config.showDate = EEPROM.read(105) != 0;
      config.blinkingColon = EEPROM.read(106) != 0;
      
      // Clock/weather display durations
      uint16_t clockDur = ((uint16_t)EEPROM.read(107)) | (((uint16_t)EEPROM.read(108)) << 8);
      config.clockDisplayDuration = (clockDur >= 1 && clockDur <= 60) ? clockDur : 10;
      
      uint16_t weatherDur = ((uint16_t)EEPROM.read(109)) | (((uint16_t)EEPROM.read(110)) << 8);
      config.weatherDisplayDuration = (weatherDur >= 1 && weatherDur <= 60) ? weatherDur : 5;
      
      // Weather API key
      for (int i = 0; i < 40; i++) {
        config.weatherAPIKey[i] = (char)EEPROM.read(111 + i);
      }
      config.weatherAPIKey[39] = '\0';
      
      // Weather location
      for (int i = 0; i < 40; i++) {
        config.weatherLocation[i] = (char)EEPROM.read(151 + i);
      }
      config.weatherLocation[39] = '\0';
      
      // Custom message
      for (int i = 0; i < 40; i++) {
        config.customMessage[i] = (char)EEPROM.read(191 + i);
      }
      config.customMessage[39] = '\0';
      
      config.customMessageEnabled = EEPROM.read(231) != 0;
      config.customMessageScrollSpeed = ((uint16_t)EEPROM.read(232)) | (((uint16_t)EEPROM.read(233)) << 8);
      if (config.customMessageScrollSpeed < 10 || config.customMessageScrollSpeed > 200) {
        config.customMessageScrollSpeed = 40; // default
      }
      
      // Version 4+ fields
      if (version >= 4) {
        config.clockScrollEnabled = EEPROM.read(234) != 0;
      } else {
        config.clockScrollEnabled = false; // default for old configs (match new default)
      }
    } else {
      config.clockScrollEnabled = false; // default (match initConfigDefaults)
    }
  }
  
  EEPROM.end();
}

void eepromSaveConfig() {
  EEPROM.begin(EEPROM_SIZE);
  
  EEPROM.write(0, 'L');
  EEPROM.write(1, 'M');
  EEPROM.write(2, 4); // version 4 (with clock scroll option)
  
  EEPROM.write(3, config.brightness);
  EEPROM.write(4, config.timeFormat);
  EEPROM.write(5, config.invertDisplay ? 1 : 0);
  EEPROM.write(6, config.sleepEnabled ? 1 : 0);
  EEPROM.write(7, config.sleepStartHour);
  EEPROM.write(8, config.sleepStartMinute);
  EEPROM.write(9, config.sleepEndHour);
  EEPROM.write(10, config.sleepEndMinute);
  
  // NTP server
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(config.ntpServer)) ? config.ntpServer[i] : 0;
    EEPROM.write(11 + i, (uint8_t)c);
  }
  
  // Secondary NTP server
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(config.ntpServer2)) ? config.ntpServer2[i] : 0;
    EEPROM.write(51 + i, (uint8_t)c);
  }
  
  // Timezone offset
  int32_t offset = config.timezoneOffset;
  for (int i = 0; i < 4; i++) {
    EEPROM.write(91 + i, (uint8_t)((offset >> (i * 8)) & 0xFF));
  }
  
  // New fields (version 3+)
  EEPROM.write(104, config.showDayOfWeek ? 1 : 0);
  EEPROM.write(105, config.showDate ? 1 : 0);
  EEPROM.write(106, config.blinkingColon ? 1 : 0);
  
  // Clock/weather display durations
  EEPROM.write(107, (uint8_t)(config.clockDisplayDuration & 0xFF));
  EEPROM.write(108, (uint8_t)((config.clockDisplayDuration >> 8) & 0xFF));
  EEPROM.write(109, (uint8_t)(config.weatherDisplayDuration & 0xFF));
  EEPROM.write(110, (uint8_t)((config.weatherDisplayDuration >> 8) & 0xFF));
  
  // Weather API key
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(config.weatherAPIKey)) ? config.weatherAPIKey[i] : 0;
    EEPROM.write(111 + i, (uint8_t)c);
  }
  
  // Weather location
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(config.weatherLocation)) ? config.weatherLocation[i] : 0;
    EEPROM.write(151 + i, (uint8_t)c);
  }
  
  // Custom message
  for (int i = 0; i < 40; i++) {
    char c = (i < (int)strlen(config.customMessage)) ? config.customMessage[i] : 0;
    EEPROM.write(191 + i, (uint8_t)c);
  }
  
  EEPROM.write(231, config.customMessageEnabled ? 1 : 0);
  EEPROM.write(232, (uint8_t)(config.customMessageScrollSpeed & 0xFF));
  EEPROM.write(233, (uint8_t)((config.customMessageScrollSpeed >> 8) & 0xFF));
  
  // Version 4+ fields
  EEPROM.write(234, config.clockScrollEnabled ? 1 : 0);
  
  EEPROM.commit();
  EEPROM.end();
  
}

uint8_t eepromReadBootCounter() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t c = EEPROM.read(EEPROM_BOOT_ADDR);
  EEPROM.end();
  return c;
}

void eepromWriteBootCounter(uint8_t c) {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_BOOT_ADDR, c);
  EEPROM.commit();
  EEPROM.end();
}

// Store last known time in EEPROM
void eepromSaveTime(time_t t) {
  EEPROM.begin(EEPROM_SIZE);
  
  // Store as 8 bytes (uint64_t) for future-proofing
  uint64_t time64 = (uint64_t)t;
  for (int i = 0; i < 8; i++) {
    EEPROM.write(EEPROM_TIME_ADDR + i, (uint8_t)((time64 >> (i * 8)) & 0xFF));
  }
  
  EEPROM.commit();
  EEPROM.end();
}

// Load last known time from EEPROM
time_t eepromLoadTime() {
  EEPROM.begin(EEPROM_SIZE);
  
  // Check if we have valid magic bytes
  if (EEPROM.read(0) != 'L' || EEPROM.read(1) != 'M') {
    EEPROM.end();
    return 0;
  }
  
  // Read 8 bytes as uint64_t
  uint64_t time64 = 0;
  for (int i = 0; i < 8; i++) {
    time64 |= ((uint64_t)EEPROM.read(EEPROM_TIME_ADDR + i)) << (i * 8);
  }
  
  EEPROM.end();
  
  time_t t = (time_t)time64;
  
  // Validate: time should be reasonable (after 2020, before 2100)
  if (t > 1577836800 && t < 4102444800) {  // 2020-01-01 to 2100-01-01
    return t;
  }
  
  return 0;
}

// ------------------------ MATRIX HELPERS --------------------

// Read orientation pin with debouncing (returns true if pin is LOW/flipped)
bool readOrientationPin(int samples = 10) {
  int highCount = 0;
  int lowCount = 0;
  for (int i = 0; i < samples; i++) {
    int state = digitalRead(PIN_ORIENTATION);
    if (state == HIGH) {
      highCount++;
    } else {
      lowCount++;
    }
    if (samples > 5) {
      delay(5);  // Longer delay for initial setup
    } else {
      delay(2);  // Shorter delay for periodic checks
    }
  }
  return (lowCount > highCount);  // LOW (GND) = flipped
}

void setMatrixBrightnessFromPercent(int percent) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  
  uint8_t intensity = map(percent, 0, 100, 0, 15);
  P.setIntensity(intensity);
}

void setTextNow(const String& txt, bool forceStatic = false) {
  Serial.printf("[Display] setTextNow: input='%s' (len:%d, forceStatic:%s)\n", 
                 txt.c_str(), txt.length(), forceStatic ? "YES" : "NO");
  
  if (txt.length() == 0) {
    P.displayClear();
    textScrolls = false;
    Serial.println("[Display] setTextNow: Empty text, cleared");
    return;
  }
  
  // Copy into global matrixText buffer
  size_t len = txt.length();
  if (len >= sizeof(matrixText)) {
    len = sizeof(matrixText) - 1;
    Serial.printf("[Display] WARNING: Text truncated from %d to %d\n", txt.length(), len);
  }
  for (size_t i = 0; i < len; i++) {
    matrixText[i] = txt[i];
  }
  matrixText[len] = '\0';
  
  Serial.printf("[Display] Buffer: '%s' (len:%d, maxCols:%d)\n", 
                 matrixText, len, MAX_DEVICES * 8);
  
  P.setFont(nullptr);
  P.setInvert(config.invertDisplay);
  
  uint16_t maxCols = MAX_DEVICES * 8;
  uint16_t charWidth = 6; // approximate
  uint16_t textWidth = len * charWidth;
  bool textFits = (textWidth <= maxCols) || (len <= 5);
  
  // If forceStatic is true, always display as static (truncate if needed)
  bool willScroll = !forceStatic && !textFits;
  
  Serial.printf("[Display] textWidth:%d, textFits:%s, forceStatic:%s, willScroll:%s\n",
                 textWidth, textFits ? "YES" : "NO", forceStatic ? "YES" : "NO", willScroll ? "YES" : "NO");
  
  P.displayClear();
  
  if (!willScroll) {
    textScrolls = false;
    P.setTextAlignment(PA_CENTER);
    // If text doesn't fit and we're forcing static, truncate it
    if (forceStatic && !textFits && len > 0) {
      // Truncate to fit - remove day icon and space if present, or truncate from end
      size_t displayLen = (maxCols / charWidth) - 1; // Leave some margin
      if (displayLen < len) {
        matrixText[displayLen] = '\0';
        Serial.printf("[Display] Truncated to '%s' (len:%d) for static display\n", matrixText, displayLen);
      }
    }
    P.print(matrixText);
    Serial.println("[Display] Displaying static centered text");
  } else {
    textScrolls = true;
    P.displayText(
      matrixText,
      PA_LEFT,
      scrollSpeed,
      scrollPause,
      PA_SCROLL_RIGHT,
      PA_SCROLL_RIGHT
    );
    P.displayReset();
    Serial.println("[Display] Displaying scrolling text");
  }
}

void showBootMessage(const String& msg) {
  setTextNow(msg);
}

// ------------------------ TIME FUNCTIONS --------------------

// Get current time, using stored time if NTP hasn't synced yet
time_t getCurrentTime() {
  if (timeSynced) {
    // NTP is synced, use system time
    time_t now = time(nullptr);
    if (now > 100000) {
      return now;
    } else {
      static int logCount = 0;
      if (logCount++ % 30 == 0) {
        Serial.printf("[Time] getCurrentTime: NTP synced but time too small: %lu\n", now);
      }
    }
  }
  
  // Use stored time + elapsed milliseconds
  if (hasStoredTime && storedTime > 0) {
    unsigned long elapsedMillis = millis() - storedTimeMillis;
    // Handle millis() overflow (wraps every ~49 days)
    if (elapsedMillis < 0x7FFFFFFF) {  // Only if no overflow
      unsigned long elapsedSeconds = elapsedMillis / 1000;
      time_t currentTime = storedTime + elapsedSeconds;
      
      // Validate: don't use if more than 7 days old (likely wrong)
      if (elapsedSeconds < 604800) {  // 7 days
        return currentTime;
      }
    }
  }
  
  static int noTimeCount = 0;
  if (noTimeCount++ % 60 == 0) {
    Serial.printf("[Time] getCurrentTime: No valid time (synced:%s, hasStored:%s)\n", 
                   timeSynced ? "yes" : "no", hasStoredTime ? "yes" : "no");
  }
  
  return 0;  // No valid time available
}

void syncTime() {
  // Only try to sync if WiFi is connected
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[NTP] WiFi not connected, skipping sync");
    return;
  }
  
  // Use primary NTP server first (secondary will be used as fallback if primary fails)
  const char* ntpServerToUse = config.ntpServer;
  
  configTime(config.timezoneOffset, 0, ntpServerToUse);
  Serial.print("[NTP] Syncing with ");
  Serial.print(ntpServerToUse);
  if (strlen(config.ntpServer2) > 0) {
    Serial.print(" (backup: ");
    Serial.print(config.ntpServer2);
    Serial.print(")");
  }
  Serial.print(" (offset: ");
  Serial.print(config.timezoneOffset);
  Serial.println(" seconds)");
  
  // Wait for time to be set
  int retries = 0;
  Serial.println("[NTP] Waiting for time sync...");
  while (time(nullptr) < 100000 && retries < 20) {
    delay(500);
    retries++;
    time_t current = time(nullptr);
    Serial.printf("[NTP] Retry %d/%d, time value: %lu\n", retries, 20, current);
  }
  
  time_t now = time(nullptr);
  Serial.printf("[NTP] Final time check: %lu (threshold: 100000)\n", now);
  
  if (now > 100000) {
    timeSynced = true;
    
    // Store the synced time
    storedTime = now;
    storedTimeMillis = millis();
    hasStoredTime = true;
    eepromSaveTime(now);
    
    struct tm* timeinfo = localtime(&now);
    Serial.printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d (wday:%d)\n",
                  timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
                  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_wday);
  } else {
    // Try secondary server if primary failed
    if (strlen(config.ntpServer2) > 0 && ntpServerToUse == config.ntpServer) {
      Serial.println("[NTP] ✗ Primary server failed, trying secondary...");
      configTime(config.timezoneOffset, 0, config.ntpServer2);
      Serial.printf("[NTP] Waiting 2 seconds for secondary server...\n");
      delay(2000);
      now = time(nullptr);
      Serial.printf("[NTP] Secondary server time check: %lu\n", now);
      if (now > 100000) {
        timeSynced = true;
        storedTime = now;
        storedTimeMillis = millis();
        hasStoredTime = true;
        eepromSaveTime(now);
        Serial.print("[NTP] Synced (secondary)\n");
      } else {
        Serial.print("[NTP] Failed\n");
        timeSynced = false;
      }
    } else {
      Serial.print("[NTP] Failed\n");
      timeSynced = false;
    }
  }
}

// Get day of week icon character
char getDayOfWeekIcon(int dayOfWeek) {
  // dayOfWeek: 0=Sunday, 1=Monday, ..., 6=Saturday
  // Using simple ASCII characters as icons
  char icons[] = {'@', '=', '+', '!', '#', 'F', '%'};
  if (dayOfWeek >= 0 && dayOfWeek <= 6) {
    return icons[dayOfWeek];
  }
  return '?';
}

String formatTime() {
  time_t now = getCurrentTime();
  
  if (now == 0) {
    Serial.println("[Time] formatTime: No time available, returning 'NO TIME'");
    return "NO TIME";
  }
  
  struct tm* timeinfo = localtime(&now);
  
  int hour = timeinfo->tm_hour;
  int minute = timeinfo->tm_min;
  int wday = timeinfo->tm_wday;
  bool isPM = false;
  
  if (config.timeFormat == 1) { // 12h format
    isPM = (hour >= 12);
    if (hour == 0) {
      hour = 12;
    } else if (hour > 12) {
      hour -= 12;
    }
  }
  
  char timeStr[16];
  String result = "";
  
  // Add day of week icon if enabled
  if (config.showDayOfWeek) {
    char icon = getDayOfWeekIcon(wday);
    result += icon;
    result += " ";
    Serial.printf("[Time] formatTime: Day icon='%c' (wday=%d, showDayOfWeek=%s)\n", 
                   icon, wday, config.showDayOfWeek ? "true" : "false");
  }
  
  // Format time with blinking colon
  char colon = config.blinkingColon && colonBlinkState ? ' ' : ':';
  
  if (config.timeFormat == 1) {
    snprintf(timeStr, sizeof(timeStr), "%d%c%02d%s", hour, colon, minute, isPM ? "PM" : "AM");
  } else {
    snprintf(timeStr, sizeof(timeStr), "%02d%c%02d", hour, colon, minute);
  }
  result += String(timeStr);
  
  Serial.printf("[Time] formatTime: raw(h:%02d m:%02d wday:%d) -> result='%s' (len:%d)\n",
                 timeinfo->tm_hour, minute, wday, result.c_str(), result.length());
  
  return result;
}

String formatDate() {
  time_t now = getCurrentTime();
  
  if (now == 0) {
    return "NO DATE";
  }
  
  struct tm* timeinfo = localtime(&now);
  
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d/%02d", timeinfo->tm_mon + 1, timeinfo->tm_mday);
  
  return String(dateStr);
}

bool isSleepTime() {
  if (!config.sleepEnabled) {
    return false;
  }
  
  time_t now = getCurrentTime();
  
  if (now == 0) {
    return false;
  }
  
  struct tm* timeinfo = localtime(&now);
  
  int currentHour = timeinfo->tm_hour;
  int currentMinute = timeinfo->tm_min;
  int currentMinutes = currentHour * 60 + currentMinute;
  
  int sleepStartMinutes = config.sleepStartHour * 60 + config.sleepStartMinute;
  int sleepEndMinutes = config.sleepEndHour * 60 + config.sleepEndMinute;
  
  // Handle sleep period that crosses midnight (e.g., 18:30 to 9:00)
  if (sleepStartMinutes > sleepEndMinutes) {
    // Sleep period crosses midnight
    return (currentMinutes >= sleepStartMinutes || currentMinutes < sleepEndMinutes);
  } else {
    // Sleep period within same day
    return (currentMinutes >= sleepStartMinutes && currentMinutes < sleepEndMinutes);
  }
}

// ------------------------ WEATHER FUNCTIONS --------------------

void fetchWeather() {
  if (strlen(config.weatherAPIKey) == 0 || strlen(config.weatherLocation) == 0) {
    weatherAvailable = false;
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Weather] WiFi not connected, skipping fetch");
    return;
  }
  
  WiFiClient client;
  HTTPClient http;
  
  // Build OpenWeatherMap API URL
  String url = "http://api.openweathermap.org/data/2.5/weather?";
  url += "q=" + String(config.weatherLocation);
  url += "&appid=" + String(config.weatherAPIKey);
  url += "&units=metric";
  
  Serial.println("[Weather] Fetching: " + url);
  
  http.begin(client, url);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    // Parse JSON response
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      if (doc.containsKey("main") && doc["main"].containsKey("temp")) {
        currentTemperature = doc["main"]["temp"];
        weatherAvailable = true;
        
        if (doc.containsKey("weather") && doc["weather"].is<JsonArray>() && doc["weather"].size() > 0) {
          weatherDescription = doc["weather"][0]["description"].as<String>();
          weatherDescription.toUpperCase();
        }
        
        Serial.printf("[Weather] Temperature: %.1f°C\n", currentTemperature);
        Serial.println("[Weather] Description: " + weatherDescription);
      } else {
        weatherAvailable = false;
        Serial.println("[Weather] Invalid response format");
      }
    } else {
      weatherAvailable = false;
      Serial.println("[Weather] JSON parse error");
    }
  } else {
    weatherAvailable = false;
    Serial.printf("[Weather] HTTP error: %d\n", httpCode);
  }
  
  http.end();
}

// ------------------------ WEB SERVER ------------------------

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>LED Matrix Clock Config</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; max-width: 600px; margin: 20px auto; padding: 20px; }";
  html += "h1 { color: #333; }";
  html += ".form-group { margin: 15px 0; }";
  html += "label { display: block; margin-bottom: 5px; font-weight: bold; }";
  html += "input, select { width: 100%; padding: 8px; box-sizing: border-box; }";
  html += "button { background: #4CAF50; color: white; padding: 10px 20px; border: none; cursor: pointer; font-size: 16px; }";
  html += "button:hover { background: #45a049; }";
  html += ".status { margin: 20px 0; padding: 10px; background: #f0f0f0; border-radius: 5px; }";
  html += "</style></head><body>";
  html += "<h1>LED Matrix Clock Configuration</h1>";
  
  // Current time status
  html += "<div class='status'>";
  html += "<strong>Current Time:</strong> " + formatTime() + "<br>";
  if (timeSynced) {
    html += "<strong>Time Source:</strong> NTP Synced<br>";
  } else if (hasStoredTime) {
    html += "<strong>Time Source:</strong> Stored Time (from previous session)<br>";
  } else {
    html += "<strong>Time Source:</strong> No Time Available<br>";
  }
  html += "<strong>WiFi Status:</strong> " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "<br>";
  html += "<strong>Display:</strong> " + String(displaySleeping ? "Sleeping" : "Active");
  html += "</div>";
  
  html += "<form method='POST' action='/save'>";
  
  // Brightness
  html += "<div class='form-group'>";
  html += "<label>Brightness (0-100):</label>";
  html += "<input type='number' name='brightness' min='0' max='100' value='" + String(config.brightness) + "'>";
  html += "</div>";
  
  // Time format
  html += "<div class='form-group'>";
  html += "<label>Time Format:</label>";
  html += "<select name='timeFormat'>";
  html += "<option value='0'" + String(config.timeFormat == 0 ? " selected" : "") + ">24 Hour</option>";
  html += "<option value='1'" + String(config.timeFormat == 1 ? " selected" : "") + ">12 Hour</option>";
  html += "</select>";
  html += "</div>";
  
  // Invert display
  html += "<div class='form-group'>";
  html += "<label>Invert Display:</label>";
  html += "<select name='invertDisplay'>";
  html += "<option value='0'" + String(!config.invertDisplay ? " selected" : "") + ">Normal</option>";
  html += "<option value='1'" + String(config.invertDisplay ? " selected" : "") + ">Inverted</option>";
  html += "</select>";
  html += "</div>";
  
  // Sleep enabled
  html += "<div class='form-group'>";
  html += "<label>Sleep Mode Enabled:</label>";
  html += "<select name='sleepEnabled'>";
  html += "<option value='0'" + String(!config.sleepEnabled ? " selected" : "") + ">Disabled</option>";
  html += "<option value='1'" + String(config.sleepEnabled ? " selected" : "") + ">Enabled</option>";
  html += "</select>";
  html += "</div>";
  
  // Sleep start time
  html += "<div class='form-group'>";
  html += "<label>Sleep Start Time (HH:MM):</label>";
  html += "<input type='time' name='sleepStart' value='";
  char startTime[6];
  snprintf(startTime, sizeof(startTime), "%02d:%02d", config.sleepStartHour, config.sleepStartMinute);
  html += String(startTime) + "'>";
  html += "</div>";
  
  // Sleep end time
  html += "<div class='form-group'>";
  html += "<label>Sleep End Time (HH:MM):</label>";
  html += "<input type='time' name='sleepEnd' value='";
  char endTime[6];
  snprintf(endTime, sizeof(endTime), "%02d:%02d", config.sleepEndHour, config.sleepEndMinute);
  html += String(endTime) + "'>";
  html += "</div>";
  
  // NTP Server
  html += "<div class='form-group'>";
  html += "<label>Primary NTP Server:</label>";
  html += "<input type='text' name='ntpServer' value='" + String(config.ntpServer) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Secondary NTP Server (optional, used as fallback):</label>";
  html += "<input type='text' name='ntpServer2' value='" + String(config.ntpServer2) + "' placeholder='Leave blank to disable'>";
  html += "</div>";
  
  // Timezone offset
  html += "<div class='form-group'>";
  html += "<label>Timezone Offset (hours, e.g., -5 for EST, +1 for CET):</label>";
  html += "<input type='number' name='timezoneOffset' step='0.5' value='" + String(config.timezoneOffset / 3600.0) + "'>";
  html += "</div>";
  
  // Display options
  html += "<h2>Display Options</h2>";
  
  html += "<div class='form-group'>";
  html += "<label>Show Day of Week Icon:</label>";
  html += "<select name='showDayOfWeek'>";
  html += "<option value='0'" + String(!config.showDayOfWeek ? " selected" : "") + ">Disabled</option>";
  html += "<option value='1'" + String(config.showDayOfWeek ? " selected" : "") + ">Enabled</option>";
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Show Date:</label>";
  html += "<select name='showDate'>";
  html += "<option value='0'" + String(!config.showDate ? " selected" : "") + ">Disabled</option>";
  html += "<option value='1'" + String(config.showDate ? " selected" : "") + ">Enabled</option>";
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Blinking Colon:</label>";
  html += "<select name='blinkingColon'>";
  html += "<option value='0'" + String(!config.blinkingColon ? " selected" : "") + ">Disabled</option>";
  html += "<option value='1'" + String(config.blinkingColon ? " selected" : "") + ">Enabled</option>";
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Clock Scroll Enabled:</label>";
  html += "<select name='clockScrollEnabled'>";
  html += "<option value='0'" + String(!config.clockScrollEnabled ? " selected" : "") + ">Disabled (static only)</option>";
  html += "<option value='1'" + String(config.clockScrollEnabled ? " selected" : "") + ">Enabled (scroll if needed)</option>";
  html += "</select>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Clock Display Duration (seconds):</label>";
  html += "<input type='number' name='clockDisplayDuration' min='1' max='60' value='" + String(config.clockDisplayDuration) + "'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Weather Display Duration (seconds):</label>";
  html += "<input type='number' name='weatherDisplayDuration' min='1' max='60' value='" + String(config.weatherDisplayDuration) + "'>";
  html += "</div>";
  
  // Weather configuration
  html += "<h2>Weather Configuration</h2>";
  html += "<p style='font-size: 12px; color: #666;'>Get your API key from <a href='https://openweathermap.org/api' target='_blank'>openweathermap.org</a></p>";
  
  html += "<div class='form-group'>";
  html += "<label>OpenWeatherMap API Key:</label>";
  html += "<input type='text' name='weatherAPIKey' value='" + String(config.weatherAPIKey) + "' placeholder='Enter your API key'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Weather Location (city name, e.g., 'London' or 'New York'):</label>";
  html += "<input type='text' name='weatherLocation' value='" + String(config.weatherLocation) + "' placeholder='e.g., London'>";
  html += "</div>";
  
  if (weatherAvailable) {
    html += "<div class='status' style='background: #d4edda; color: #155724;'>";
    html += "<strong>Weather:</strong> " + String(currentTemperature, 1) + "°C";
    if (weatherDescription.length() > 0) {
      html += " - " + weatherDescription;
    }
    html += "</div>";
  }
  
  // Custom message
  html += "<h2>Custom Message</h2>";
  
  html += "<div class='form-group'>";
  html += "<label>Custom Message (persistent until cleared):</label>";
  html += "<input type='text' name='customMessage' value='" + String(config.customMessage) + "' maxlength='39' placeholder='Enter message'>";
  html += "</div>";
  
  html += "<div class='form-group'>";
  html += "<label>Message Scroll Speed (10-200, lower = faster):</label>";
  html += "<input type='number' name='customMessageScrollSpeed' min='10' max='200' value='" + String(config.customMessageScrollSpeed) + "'>";
  html += "</div>";
  
  html += "<button type='submit'>Save Configuration</button>";
  html += "</form>";
  
  html += "<h2>REST API Endpoints</h2>";
  html += "<div class='status' style='font-size: 12px;'>";
  html += "<strong>Set Message:</strong> POST /set_message?message=YOUR_TEXT&speed=40<br>";
  html += "<strong>Clear Message:</strong> GET /clear_message<br>";
  html += "<strong>Set Brightness:</strong> POST /set_brightness?value=0-15 (or -1 to turn off)<br>";
  html += "</div>";
  
  html += "<p><a href='/'>Refresh</a> | <a href='/restart'>Restart Device</a></p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("brightness")) {
    int b = server.arg("brightness").toInt();
    if (b >= 0 && b <= 100) {
      config.brightness = b;
    }
  }
  
  if (server.hasArg("timeFormat")) {
    int tf = server.arg("timeFormat").toInt();
    if (tf == 0 || tf == 1) {
      config.timeFormat = tf;
    }
  }
  
  if (server.hasArg("invertDisplay")) {
    config.invertDisplay = (server.arg("invertDisplay").toInt() != 0);
  }
  
  if (server.hasArg("sleepEnabled")) {
    config.sleepEnabled = (server.arg("sleepEnabled").toInt() != 0);
  }
  
  if (server.hasArg("sleepStart")) {
    String start = server.arg("sleepStart");
    int colonPos = start.indexOf(':');
    if (colonPos > 0) {
      int hour = start.substring(0, colonPos).toInt();
      int minute = start.substring(colonPos + 1).toInt();
      if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
        config.sleepStartHour = hour;
        config.sleepStartMinute = minute;
      }
    }
  }
  
  if (server.hasArg("sleepEnd")) {
    String end = server.arg("sleepEnd");
    int colonPos = end.indexOf(':');
    if (colonPos > 0) {
      int hour = end.substring(0, colonPos).toInt();
      int minute = end.substring(colonPos + 1).toInt();
      if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
        config.sleepEndHour = hour;
        config.sleepEndMinute = minute;
      }
    }
  }
  
  if (server.hasArg("ntpServer")) {
    String ntp = server.arg("ntpServer");
    ntp.trim();
    size_t len = (ntp.length() < sizeof(config.ntpServer)) ? ntp.length() : sizeof(config.ntpServer) - 1;
    ntp.toCharArray(config.ntpServer, len + 1);
    config.ntpServer[len] = '\0';
  }
  
  if (server.hasArg("ntpServer2")) {
    String ntp2 = server.arg("ntpServer2");
    ntp2.trim();
    size_t len = (ntp2.length() < sizeof(config.ntpServer2)) ? ntp2.length() : sizeof(config.ntpServer2) - 1;
    ntp2.toCharArray(config.ntpServer2, len + 1);
    config.ntpServer2[len] = '\0';
  }
  
  if (server.hasArg("timezoneOffset")) {
    float hours = server.arg("timezoneOffset").toFloat();
    // Validate timezone offset: -12 to +14 hours
    if (hours >= -12.0 && hours <= 14.0) {
      config.timezoneOffset = (int32_t)(hours * 3600);
    }
  }
  
  if (server.hasArg("showDayOfWeek")) {
    config.showDayOfWeek = (server.arg("showDayOfWeek").toInt() != 0);
  }
  
  if (server.hasArg("showDate")) {
    config.showDate = (server.arg("showDate").toInt() != 0);
  }
  
  if (server.hasArg("blinkingColon")) {
    config.blinkingColon = (server.arg("blinkingColon").toInt() != 0);
  }
  
  if (server.hasArg("clockScrollEnabled")) {
    config.clockScrollEnabled = (server.arg("clockScrollEnabled").toInt() != 0);
  }
  
  if (server.hasArg("clockDisplayDuration")) {
    config.clockDisplayDuration = server.arg("clockDisplayDuration").toInt();
    if (config.clockDisplayDuration < 1) config.clockDisplayDuration = 1;
    if (config.clockDisplayDuration > 60) config.clockDisplayDuration = 60;
  }
  
  if (server.hasArg("weatherDisplayDuration")) {
    config.weatherDisplayDuration = server.arg("weatherDisplayDuration").toInt();
    if (config.weatherDisplayDuration < 1) config.weatherDisplayDuration = 1;
    if (config.weatherDisplayDuration > 60) config.weatherDisplayDuration = 60;
  }
  
  if (server.hasArg("weatherAPIKey")) {
    String key = server.arg("weatherAPIKey");
    key.trim();
    size_t len = (key.length() < sizeof(config.weatherAPIKey)) ? key.length() : sizeof(config.weatherAPIKey) - 1;
    key.toCharArray(config.weatherAPIKey, len + 1);
    config.weatherAPIKey[len] = '\0';
  }
  
  if (server.hasArg("weatherLocation")) {
    String loc = server.arg("weatherLocation");
    loc.trim();
    size_t len = (loc.length() < sizeof(config.weatherLocation)) ? loc.length() : sizeof(config.weatherLocation) - 1;
    loc.toCharArray(config.weatherLocation, len + 1);
    config.weatherLocation[len] = '\0';
  }
  
  if (server.hasArg("customMessage")) {
    String msg = server.arg("customMessage");
    msg.trim();
    msg.toUpperCase();
    size_t len = (msg.length() < sizeof(config.customMessage)) ? msg.length() : sizeof(config.customMessage) - 1;
    msg.toCharArray(config.customMessage, len + 1);
    config.customMessage[len] = '\0';
    config.customMessageEnabled = (strlen(config.customMessage) > 0);
  }
  
  if (server.hasArg("customMessageScrollSpeed")) {
    uint16_t speed = server.arg("customMessageScrollSpeed").toInt();
    if (speed >= 10 && speed <= 200) {
      config.customMessageScrollSpeed = speed;
    }
  }
  
  eepromSaveConfig();
  setMatrixBrightnessFromPercent(config.brightness);
  P.setInvert(config.invertDisplay);
  
  // Re-sync time if NTP server or timezone changed
  syncTime();
  
  // Fetch weather if API key/location changed
  if (strlen(config.weatherAPIKey) > 0 && strlen(config.weatherLocation) > 0) {
    fetchWeather();
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='2;url=/'>";
  html += "<title>Saved</title></head><body>";
  html += "<h1>Configuration Saved!</h1>";
  html += "<p>Redirecting...</p>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleRestart() {
  server.send(200, "text/html", "<!DOCTYPE html><html><head><title>Restarting</title></head><body><h1>Restarting device...</h1></body></html>");
  delay(1000);
  ESP.restart();
}

// REST API: Set custom message
void handleSetMessage() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  
  String message = "";
  uint16_t speed = config.customMessageScrollSpeed;
  
  if (server.hasArg("message")) {
    message = server.arg("message");
    message.toUpperCase();
    
    // Sanitize: only allow A-Z, 0-9, spaces, and simple punctuation
    String sanitized = "";
    for (int i = 0; i < message.length(); i++) {
      char c = message.charAt(i);
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || 
          c == ' ' || c == ':' || c == '!' || c == '\'' || c == '-' || 
          c == '.' || c == ',' || c == '_' || c == '+' || c == '%' || 
          c == '/' || c == '?') {
        sanitized += c;
      }
    }
    message = sanitized;
    
    // Store message
    size_t len = (message.length() < sizeof(config.customMessage)) ? message.length() : sizeof(config.customMessage) - 1;
    message.toCharArray(config.customMessage, len + 1);
    config.customMessage[len] = '\0';
    config.customMessageEnabled = (strlen(config.customMessage) > 0);
    
    if (server.hasArg("speed")) {
      speed = server.arg("speed").toInt();
      if (speed >= 10 && speed <= 200) {
        config.customMessageScrollSpeed = speed;
      }
    }
    
    eepromSaveConfig();
    
    server.send(200, "text/plain", "OK");
    Serial.println("[API] Custom message set: " + message);
  } else {
    server.send(400, "text/plain", "Bad Request: message parameter required");
  }
}

// REST API: Clear custom message
void handleClearMessage() {
  config.customMessage[0] = '\0';
  config.customMessageEnabled = false;
  eepromSaveConfig();
  server.send(200, "text/plain", "OK");
  Serial.println("[API] Custom message cleared");
}

// REST API: Set brightness
void handleSetBrightness() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }
  
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    
    if (value == -1) {
      // Turn display off
      P.displayClear();
      server.send(200, "text/plain", "OK");
      Serial.println("[API] Display turned off");
    } else if (value >= 0 && value <= 15) {
      // Set brightness 0-15
      P.setIntensity(value);
      config.brightness = map(value, 0, 15, 0, 100);
      eepromSaveConfig();
      server.send(200, "text/plain", "OK");
      Serial.printf("[API] Brightness set to %d\n", value);
    } else {
      server.send(400, "text/plain", "Bad Request: value must be 0-15 or -1");
    }
  } else {
    server.send(400, "text/plain", "Bad Request: value parameter required");
  }
}

// Export configuration as JSON
void handleExport() {
  String json = "{";
  json += "\"brightness\":" + String(config.brightness) + ",";
  json += "\"timeFormat\":" + String(config.timeFormat) + ",";
  json += "\"invertDisplay\":" + String(config.invertDisplay ? "true" : "false") + ",";
  json += "\"sleepEnabled\":" + String(config.sleepEnabled ? "true" : "false") + ",";
  json += "\"sleepStartHour\":" + String(config.sleepStartHour) + ",";
  json += "\"sleepStartMinute\":" + String(config.sleepStartMinute) + ",";
  json += "\"sleepEndHour\":" + String(config.sleepEndHour) + ",";
  json += "\"sleepEndMinute\":" + String(config.sleepEndMinute) + ",";
  json += "\"ntpServer\":\"" + String(config.ntpServer) + "\",";
  json += "\"ntpServer2\":\"" + String(config.ntpServer2) + "\",";
  json += "\"timezoneOffset\":" + String(config.timezoneOffset) + ",";
  json += "\"showDayOfWeek\":" + String(config.showDayOfWeek ? "true" : "false") + ",";
  json += "\"showDate\":" + String(config.showDate ? "true" : "false") + ",";
  json += "\"blinkingColon\":" + String(config.blinkingColon ? "true" : "false") + ",";
  json += "\"clockScrollEnabled\":" + String(config.clockScrollEnabled ? "true" : "false") + ",";
  json += "\"clockDisplayDuration\":" + String(config.clockDisplayDuration) + ",";
  json += "\"weatherDisplayDuration\":" + String(config.weatherDisplayDuration) + ",";
  // Mask weatherAPIKey for security - show only last 4 characters
  String maskedKey = "";
  int keyLen = strlen(config.weatherAPIKey);
  if (keyLen > 0) {
    if (keyLen <= 4) {
      maskedKey = String(config.weatherAPIKey);
    } else {
      maskedKey = "****";
      maskedKey += String(config.weatherAPIKey + keyLen - 4);
    }
  }
  json += "\"weatherAPIKey\":\"" + maskedKey + "\",";
  json += "\"weatherLocation\":\"" + String(config.weatherLocation) + "\",";
  json += "\"customMessage\":\"" + String(config.customMessage) + "\",";
  json += "\"customMessageEnabled\":" + String(config.customMessageEnabled ? "true" : "false") + ",";
  json += "\"customMessageScrollSpeed\":" + String(config.customMessageScrollSpeed);
  json += "}";
  
  server.sendHeader("Content-Disposition", "attachment; filename=config.json");
  server.send(200, "application/json", json);
  Serial.println("[API] Config exported");
}

// Factory reset (only in AP mode for safety)
void handleFactoryReset() {
  // Only allow in AP mode for safety
  if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
    server.send(403, "text/plain", "Factory reset only available in AP mode");
    return;
  }
  
  // Clear EEPROM
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  EEPROM.end();
  
  server.send(200, "text/html", "<!DOCTYPE html><html><head><title>Factory Reset</title></head><body><h1>Factory Reset Complete</h1><p>Device will restart in 3 seconds...</p></body></html>");
  delay(3000);
  ESP.restart();
}

// ------------------------ WIFI MANAGER ---------------------

void showConfigModeMessage() {
  P.displayClear();
  P.setFont(nullptr);
  P.setInvert(false);
  P.setTextAlignment(PA_CENTER);
  P.print("CFG 192.168.4.1");
  textScrolls = false;
}

void startConfigPortal() {
  Serial.println("[WiFi] Entering CONFIG PORTAL mode (boot counter)");
  showConfigModeMessage();
  
  wifiManager.setConfigPortalTimeout(0);
  wifiManager.startConfigPortal(deviceID.c_str(), AP_password);
  
  eepromWriteBootCounter(0);
  showBootMessage("CFG SAVED");
  delay(1000);
}

void connectToNetwork() {
  // Disconnect any existing WiFi connection and clear saved state
  // This ensures WiFiManager can properly use saved credentials
  WiFi.disconnect(true);
  delay(100);
  
  WiFi.mode(WIFI_STA);
  delay(100);
  
  Serial.printf("[WiFi] connectToNetwork: Starting WiFi connection\n");
  Serial.printf("[WiFi] Boot counter (prev boot) = %u, BOOT_FAIL_LIMIT = %u\n", bootCountCached, BOOT_FAIL_LIMIT);
  Serial.printf("[WiFi] Device ID: %s\n", deviceID.c_str());
  
  // Check if WiFiManager has saved credentials
  Serial.println("[WiFi] Checking for saved WiFi credentials...");
  
  wifiManager.setConfigPortalTimeout(180);
  Serial.println("[WiFi] Config portal timeout set to 180 seconds");
  
  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("[WiFi] Config portal started");
    Serial.printf("[WiFi] AP SSID: %s\n", wm->getConfigPortalSSID().c_str());
    showConfigModeMessage();
  });
  
  if (bootCountCached >= BOOT_FAIL_LIMIT) {
    Serial.println("[WiFi] Boot counter >= limit, forcing config portal");
    startConfigPortal();
  } else {
    Serial.println("[WiFi] Boot counter OK, attempting auto-connect");
  }
  
  Serial.println("[WiFi] Calling wifiManager.autoConnect()...");
  bool res = wifiManager.autoConnect(deviceID.c_str(), AP_password);
  
  if (!res) {
    Serial.println("[WiFi] autoConnect FAILED - restarting device");
    showBootMessage("WiFi ERR");
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("[WiFi] autoConnect SUCCESS");
    Serial.printf("[WiFi] SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());
    showBootMessage("WiFi OK");
    delay(1000);
  }
  
  eepromWriteBootCounter(0);
  Serial.println("[WiFi] Boot counter reset to 0 (WiFi OK)");
}

// ------------------------ SETUP ----------------------------

void setup() {
  // Initialize serial with longer delay to ensure stability
  Serial.begin(115200);
  delay(500);  // Longer delay for serial to stabilize
  Serial.flush();
  
  // Simple test pattern to verify serial is working
  Serial.print("\n\n=== BOOT ===\n");
  delay(50);
  
  // Build deviceID from MAC
  // Set persistent mode BEFORE setting WiFi mode to ensure credentials are saved/loaded
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  delay(100);
  
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  char macBuf[13];
  sprintf(macBuf, "%02X%02X%02X%02X%02X%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  deviceID = "LED_Clock_";
  deviceID += macBuf;
  deviceID.toUpperCase();
  
  Serial.print("[ID] ");
  Serial.println(deviceID);
  delay(50);
  WiFi.hostname(deviceID);
  
  // Initialize default config values
  initConfigDefaults();
  Serial.print("[CFG] Defaults loaded\n");
  delay(50);
  
  // Load config from EEPROM (will overwrite defaults if valid config exists)
  eepromLoadConfig();
  
  Serial.print("[CFG] EEPROM loaded\n");
  delay(50);
  
  // Load stored time from EEPROM
  storedTime = eepromLoadTime();
  if (storedTime > 0) {
    storedTimeMillis = millis();
    hasStoredTime = true;
  } else {
    hasStoredTime = false;
  }
  
  // Check orientation pin (GPIO16) to determine if display should be flipped
  // GPIO16 on ESP8266: Connect to GND (LOW) to flip display, leave floating for normal
  // Note: GPIO16 has special characteristics - no internal pull-up, used for deep sleep
  pinMode(PIN_ORIENTATION, INPUT);
  delay(50);  // Allow pin to stabilize
  
  displayFlipped = readOrientationPin(10);  // Use 10 samples for initial setup
  
  Serial.printf("[Orientation] GPIO16: flip=%s\n", displayFlipped ? "YES (LOW)" : "NO (HIGH)");
  
  // Matrix init
  P.begin();
  setMatrixBrightnessFromPercent(config.brightness);
  P.setInvert(config.invertDisplay);
  P.displayClear();
  
  // Configure zone to use all 4 devices (32 columns x 8 rows)
  P.setZone(0, 0, MAX_DEVICES - 1);
  // Always apply horizontal flip to fix backwards text (hardware wiring requirement)
  // Apply vertical flip based on GPIO16 pin state (if needed)
  // LOW (GND) = flip display vertically (upside down), HIGH (floating) = normal
  zoneEffect_t flipEffect = PA_FLIP_LR;  // Always flip horizontally
  if (displayFlipped) {
    // Pin is LOW (GND) - also flip display vertically (upside down)
    flipEffect = (zoneEffect_t)(PA_FLIP_LR | PA_FLIP_UD);
    Serial.println("[Orientation] Display FLIPPED HORIZONTALLY + VERTICALLY (GPIO16=LOW)");
  } else {
    Serial.println("[Orientation] Display FLIPPED HORIZONTALLY only (GPIO16=HIGH)");
  }
  P.setZoneEffect(0, true, flipEffect);
  
  // Boot counter logic
  bootCountCached = eepromReadBootCounter();
  uint8_t newCount = bootCountCached;
  if (newCount < 255) {
    newCount++;
  }
  eepromWriteBootCounter(newCount);
  
  showBootMessage("BOOT");
  delay(1000);
  
  showBootMessage("CONFIG?");
  unsigned long cfgPromptStart = millis();
  while (millis() - cfgPromptStart < CONFIG_PROMPT_MS) {
    if (P.displayAnimate()) {
      if (textScrolls) {
        P.displayReset();
      }
    }
    yield();
  }
  
  // WiFi connection
  connectToNetwork();
  
  // Show IP
  String ipMsg = "IP " + WiFi.localIP().toString();
  showBootMessage(ipMsg);
  // Wait for IP message to display, but allow animation
  unsigned long ipStart = millis();
  while (millis() - ipStart < 2000) {
    if (P.displayAnimate()) {
      if (textScrolls) {
        P.displayReset();
      }
    }
    yield();
  }
  // Clear any scrolling state before transitioning to clock
  textScrolls = false;
  P.displayClear();
  
  // Setup web server
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/restart", handleRestart);
  server.on("/set_message", handleSetMessage);
  server.on("/clear_message", handleClearMessage);
  server.on("/set_brightness", handleSetBrightness);
  server.on("/export", handleExport);
  server.on("/factory_reset", handleFactoryReset);
  server.begin();
  Serial.println("[Web] Server started on http://" + WiFi.localIP().toString());
  
  // Try to sync time if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
    
    // Fetch weather if configured
    if (strlen(config.weatherAPIKey) > 0 && strlen(config.weatherLocation) > 0) {
      fetchWeather();
    }
  } else if (hasStoredTime) {
    Serial.println("[Boot] WiFi not connected, using stored time");
  }
  
  // Initialize display mode
  currentDisplayMode = MODE_CLOCK;
  modeStartTime = millis();
  
  // Force display update after boot - clear any scrolling text and show clock
  textScrolls = false;
  lastTimeDisplay = 0; // Force immediate update in loop
  P.displayClear();
  
  Serial.print("[SYS] Ready\n");
  delay(50);
}

// ------------------------ LOOP -----------------------------

void loop() {
  unsigned long now = millis();
  
  // Handle web server
  server.handleClient();
  
  // Update time sync periodically (only if WiFi is connected)
  if (WiFi.status() == WL_CONNECTED && (now - lastTimeUpdate >= timeUpdateInterval)) {
    lastTimeUpdate = now;
    syncTime();
  }
  
  // Also try initial sync if we haven't synced yet and WiFi is connected
  // Check if lastTimeUpdate is 0 (first boot) or enough time has passed
  if (!timeSynced && WiFi.status() == WL_CONNECTED) {
    if (lastTimeUpdate == 0 || (now - lastTimeUpdate >= 5000)) {
      lastTimeUpdate = now;
      syncTime();
    }
  }
  
  // Fetch weather periodically
  if (WiFi.status() == WL_CONNECTED && strlen(config.weatherAPIKey) > 0 && 
      (now - lastWeatherUpdate >= weatherUpdateInterval)) {
    lastWeatherUpdate = now;
    fetchWeather();
  }
  
  // Update blinking colon
  if (config.blinkingColon && (now - lastColonBlink >= colonBlinkInterval)) {
    lastColonBlink = now;
    colonBlinkState = !colonBlinkState;
    // For scrolling text, don't force update - let the animation continue
    // Only update if text is static (fits on screen)
    if (currentDisplayMode == MODE_CLOCK && !displaySleeping && !textScrolls) {
      lastTimeDisplay = 0; // Force update only for static text
    }
  }
  
  // Check sleep time
  bool shouldSleep = isSleepTime();
  if (shouldSleep != displaySleeping) {
    displaySleeping = shouldSleep;
    if (displaySleeping) {
      P.displayClear();
      Serial.println("[Sleep] Entering sleep mode");
    } else {
      Serial.println("[Sleep] Exiting sleep mode");
      modeStartTime = now; // Reset mode timer
    }
  }
  
  if (!displaySleeping) {
    // Determine which mode to display
    unsigned long modeDuration = 0;
    bool shouldSwitchMode = false;
    
    // Check if we should switch modes based on duration
    switch (currentDisplayMode) {
      case MODE_CLOCK:
        modeDuration = config.clockDisplayDuration * 1000;
        break;
      case MODE_WEATHER:
        modeDuration = config.weatherDisplayDuration * 1000;
        break;
      case MODE_DATE:
        modeDuration = 3000; // 3 seconds for date
        break;
      case MODE_CUSTOM_MESSAGE:
        // Custom messages stay until cleared
        modeDuration = 0;
        break;
    }
    
    if (modeDuration > 0 && (now - modeStartTime >= modeDuration)) {
      shouldSwitchMode = true;
    }
    
    // Switch to next mode if needed
    if (shouldSwitchMode) {
      DisplayMode oldMode = currentDisplayMode;
      switch (currentDisplayMode) {
        case MODE_CLOCK:
          if (config.showDate) {
            currentDisplayMode = MODE_DATE;
          } else if (weatherAvailable) {
            currentDisplayMode = MODE_WEATHER;
          } else if (config.customMessageEnabled) {
            currentDisplayMode = MODE_CUSTOM_MESSAGE;
          }
          break;
        case MODE_DATE:
          if (weatherAvailable) {
            currentDisplayMode = MODE_WEATHER;
          } else if (config.customMessageEnabled) {
            currentDisplayMode = MODE_CUSTOM_MESSAGE;
          } else {
            currentDisplayMode = MODE_CLOCK;
          }
          break;
        case MODE_WEATHER:
          if (config.customMessageEnabled) {
            currentDisplayMode = MODE_CUSTOM_MESSAGE;
          } else {
            currentDisplayMode = MODE_CLOCK;
          }
          break;
        case MODE_CUSTOM_MESSAGE:
          currentDisplayMode = MODE_CLOCK;
          break;
      }
      Serial.printf("[Display] Mode switch: %d -> %d\n", oldMode, currentDisplayMode);
      modeStartTime = now;
      // Reset tracked time and scroll state when switching modes
      lastDisplayedHour = -1;
      lastDisplayedMinute = -1;
      textScrolls = false;  // Reset scroll state on mode switch
    }
    
    // Display current mode
    if (now - lastTimeDisplay >= timeDisplayInterval || shouldSwitchMode) {
      Serial.printf("[Display] Updating mode %d (interval:%lu, shouldSwitch:%s)\n",
                     currentDisplayMode, now - lastTimeDisplay, shouldSwitchMode ? "YES" : "NO");
      lastTimeDisplay = now;
      
      switch (currentDisplayMode) {
        case MODE_CLOCK: {
          // Get current time to check if it actually changed
          time_t nowTime = getCurrentTime();
          bool timeChanged = false;
          
          if (nowTime > 0) {
            struct tm* timeinfo = localtime(&nowTime);
            int currentHour = timeinfo->tm_hour;
            int currentMinute = timeinfo->tm_min;
            
            // Always update when switching modes, or if time changed
            if (shouldSwitchMode || currentHour != lastDisplayedHour || currentMinute != lastDisplayedMinute) {
              timeChanged = true;
              lastDisplayedHour = currentHour;
              lastDisplayedMinute = currentMinute;
              Serial.printf("[Display] MODE_CLOCK: Time changed to %02d:%02d (switch:%s)\n", 
                           currentHour, currentMinute, shouldSwitchMode ? "YES" : "NO");
            } else if (!textScrolls) {
              // For static text, update on colon blink
              timeChanged = true;
              Serial.println("[Display] MODE_CLOCK: Colon blink update (static text)");
            } else {
              Serial.println("[Display] MODE_CLOCK: Skipping update (scrolling, time unchanged)");
            }
          } else {
            // No time available, always update
            timeChanged = true;
            Serial.println("[Display] MODE_CLOCK: No time, forcing update");
          }
          
          if (timeChanged) {
            // Pass forceStatic=true if clock scrolling is disabled
            setTextNow(formatTime(), !config.clockScrollEnabled);
          }
          break;
        }
        case MODE_WEATHER:
          // Only update weather when switching modes (weather doesn't change frequently)
          if (shouldSwitchMode) {
            if (weatherAvailable) {
              char tempStr[16];
              snprintf(tempStr, sizeof(tempStr), "%.0f°C", currentTemperature);
              setTextNow(String(tempStr));
            } else {
              setTextNow("! TEMP");
            }
          }
          break;
        case MODE_DATE:
          // Only update date when switching modes or if date might have changed
          if (shouldSwitchMode) {
            setTextNow(formatDate());
          }
          break;
        case MODE_CUSTOM_MESSAGE:
          // Check if custom message is still enabled/valid (always check, not just on mode switch)
          if (!config.customMessageEnabled || strlen(config.customMessage) == 0) {
            // Message was disabled/cleared, switch back to clock
            currentDisplayMode = MODE_CLOCK;
            modeStartTime = now;
            lastDisplayedHour = -1;
            lastDisplayedMinute = -1;
            textScrolls = false;
            Serial.println("[Display] Custom message cleared, switching to clock");
          } else if (shouldSwitchMode) {
            // Only update display when switching modes (message is static)
            // Custom messages use their own scroll speed, so handle separately
            String msg = String(config.customMessage);
            size_t len = msg.length();
            if (len >= sizeof(matrixText)) {
              len = sizeof(matrixText) - 1;
            }
            for (size_t i = 0; i < len; i++) {
              matrixText[i] = msg[i];
            }
            matrixText[len] = '\0';
            
            P.setFont(nullptr);
            P.setInvert(config.invertDisplay);
            P.displayClear();
            
            uint16_t maxCols = MAX_DEVICES * 8;
            uint16_t charWidth = 6;
            uint16_t textWidth = len * charWidth;
            bool textFits = (textWidth <= maxCols) || (len <= 5);
            
            if (textFits) {
              textScrolls = false;
              P.setTextAlignment(PA_CENTER);
              P.print(matrixText);
            } else {
              textScrolls = true;
              P.displayText(
                matrixText,
                PA_LEFT,
                config.customMessageScrollSpeed,  // Use custom scroll speed
                scrollPause,
                PA_SCROLL_RIGHT,
                PA_SCROLL_RIGHT
              );
              P.displayReset();
            }
          }
          break;
      }
    }
  }
  
  // Periodically save time to EEPROM (so it persists across power loss)
  if (hasStoredTime && (now - lastTimeSave >= timeSaveInterval)) {
    time_t currentTime = getCurrentTime();
    if (currentTime > 0) {
      eepromSaveTime(currentTime);
      storedTime = currentTime;
      storedTimeMillis = millis();
      lastTimeSave = now;
    }
  }
  
  // Animate display
  if (P.displayAnimate()) {
    if (textScrolls) {
      P.displayReset();
    }
  }
  
  // Periodically check orientation pin for dynamic changes
  if (now - lastOrientationCheck >= orientationCheckInterval) {
    lastOrientationCheck = now;
    
    bool newFlipped = readOrientationPin(5);  // Use 5 samples for periodic checks
    
    // Only update if state changed
    if (newFlipped != displayFlipped) {
      displayFlipped = newFlipped;
      // Horizontal flip is always enabled, combine with vertical flip if needed
      zoneEffect_t flipEffect = PA_FLIP_LR;  // Always flip horizontally
      if (displayFlipped) {
        flipEffect = (zoneEffect_t)(PA_FLIP_LR | PA_FLIP_UD);  // Also flip vertically
        Serial.println("[Orientation] Changed: FLIPPED HORIZONTALLY + VERTICALLY (GPIO16=LOW)");
      } else {
        Serial.println("[Orientation] Changed: FLIPPED HORIZONTALLY only (GPIO16=HIGH)");
      }
      P.setZoneEffect(0, true, flipEffect);
    }
  }
  
  yield();
}
