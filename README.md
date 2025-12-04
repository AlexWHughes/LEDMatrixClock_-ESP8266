# LED Matrix WiFi Clock (ESP8266)

ESP8266-based LED matrix clock that syncs with NTP and displays the time on a **32×8 LED matrix** (4× MAX7219 8×8 modules in a row).

Buy the units here: https://www.aliexpress.com/item/1005006038630745.html

## Features

- ✅ **NTP Time Synchronization** - Automatically syncs with NTP servers (primary + secondary fallback)
- ✅ **Weather Display** - Shows temperature from OpenWeatherMap API, alternates with clock
- ✅ **Custom Messages** - Persistent scroll messages via web UI or REST API
- ✅ **12/24 Hour Time Format** - Configurable time display format
- ✅ **Day of Week Icons** - Visual day indicators (@, =, +, !, #, $, %)
- ✅ **Date Display** - Optional date display (MM/DD format)
- ✅ **Blinking Colon** - Optional animated colon in time display
- ✅ **Sleep Mode** - Display turns off during configured hours (e.g., 18:30 to 9:00)
- ✅ **Brightness Control** - Adjustable brightness (0-100) with REST API support
- ✅ **Display Inversion** - Optional inverted display mode
- ✅ **WiFiManager Config Portal** - Easy initial WiFi setup
- ✅ **Enhanced Web Configuration UI** - Configure all settings via web browser
- ✅ **REST API Endpoints** - Home Assistant integration (set_message, set_brightness)
- ✅ **Config Export/Factory Reset** - Backup and restore configuration
- ✅ **EEPROM Storage** - All settings persist across reboots
- ✅ **Boot Recovery** - Reset during boot to enter config mode
- ✅ **Orientation Detection** - Automatic display flip via GPIO16 pin
- ✅ **Designed for cheap LED Matrix clocks based on ESP8266 from AliExpress**

---

## Hardware

- ESP8266 (Wemos D1 mini / NodeMCU / custom ESP8266MOD)
- 32×8 LED matrix (4× MAX7219 modules in a chain)
- Pins: CS=15 (D8), CLK=14 (D5), DIN=13 (D7)
- Optional: GPIO16 (D0) connected to GND to flip display orientation

Hardware link: https://www.aliexpress.com/item/1005006038630745.html

---

## Initial Setup

### WiFi Configuration

1. On first boot, the device will create a WiFi access point:
   - **SSID**: `LED_CLOCK_<MAC>` (e.g., `LED_CLOCK_A1B2C3D4E5F6`)
   - **Password**: (blank - open network)

2. Connect to this network and navigate to `http://192.168.4.1`

3. Configure your WiFi credentials in the WiFiManager portal

4. Once connected to WiFi, the device will automatically sync with NTP and start displaying the time

### Web Configuration UI

After connecting to WiFi, access the web configuration interface:

1. Find the device's IP address (displayed on the matrix during boot, or check your router)
2. Open a web browser and navigate to `http://<device-ip>`
3. Configure all settings:
   - **Brightness** (0-100)
   - **Time Format** (12-hour or 24-hour)
   - **Invert Display** (normal or inverted)
   - **Sleep Mode** (enabled/disabled)
   - **Sleep Start/End Times** (when display turns off/on)
   - **Primary/Secondary NTP Servers** (default: pool.ntp.org)
   - **Timezone Offset** (hours from UTC, e.g., -5 for EST, +1 for CET)
   - **Show Day of Week Icon** (enabled/disabled)
   - **Show Date** (enabled/disabled)
   - **Blinking Colon** (enabled/disabled)
   - **Clock/Weather Display Durations** (seconds)
   - **Weather API Key & Location** (OpenWeatherMap)
   - **Custom Message** (persistent scroll message)

All settings are saved to EEPROM and persist across reboots.

---

## Display Behavior

The display automatically cycles between different modes:

1. **Clock Mode** - Shows current time with optional day icon and blinking colon
2. **Date Mode** - Shows date (MM/DD) if enabled
3. **Weather Mode** - Shows temperature (°C) if weather API is configured
4. **Custom Message Mode** - Shows persistent custom message if set

### Display Modes

- **Time Display**: Shows current time in configured format (12h or 24h)
  - Optional day of week icon prefix (@=Sunday, ==Monday, +=Tuesday, !=Wednesday, #=Thursday, $=Friday, %=Saturday)
  - Optional blinking colon (blinks every 500ms)
  - Centered on display
- **Date Display**: Shows date in MM/DD format (if enabled)
- **Weather Display**: Shows temperature in °C (if OpenWeatherMap API configured)
- **Custom Message**: Scrolls custom text message (persistent until cleared)
- **Sleep Mode**: Display turns off completely during configured sleep hours
- **Auto-sync**: Time syncs with NTP every hour, weather updates every 5 minutes
- **Smooth Updates**: No flicker, smooth transitions between modes

---

## Sleep Mode

Sleep mode allows you to turn off the display during specific hours to save power and reduce light pollution.

**Example Configuration:**
- Sleep Start: 18:30 (6:30 PM)
- Sleep End: 09:00 (9:00 AM)

This means the display will be off from 6:30 PM until 9:00 AM the next day.

Sleep periods that cross midnight are fully supported.

---

## Boot Recovery / Config Portal

### Entering Config Portal

During boot, the device displays "CONFIG?" for 5 seconds. If you reset the device during this window, the next boot will force the WiFiManager config portal.

Alternatively, you can trigger the config portal by:
1. Resetting during the "CONFIG?" prompt
2. Failing to connect to WiFi (automatic after 3 minutes)

### Config Portal Access

- **SSID**: `LED_CLOCK_<MAC>`
- **Password**: (blank)
- **URL**: `http://192.168.4.1`

---

## Weather Display

The device can display weather information from OpenWeatherMap:

1. **Get API Key**: Sign up at [openweathermap.org](https://openweathermap.org/api) (free tier available)
2. **Configure**: Enter your API key and location (city name) in the web UI
3. **Display**: Weather alternates with clock display automatically
   - Updates every 5 minutes
   - Shows temperature in °C
   - Falls back to clock if weather unavailable

## Custom Messages

You can set persistent custom messages that scroll on the display:

### Via Web UI
- Enter message in the "Custom Message" field
- Message persists until manually cleared
- Supports A-Z, 0-9, spaces, and basic punctuation

### Via REST API
```
POST http://<device-ip>/set_message?message=YOUR_TEXT&speed=40
GET  http://<device-ip>/clear_message
```

The message will display between clock/weather cycles and remains until cleared.

## Time Synchronization

The device uses NTP (Network Time Protocol) to synchronize time:

- **Primary NTP Server**: `pool.ntp.org` (default, configurable)
- **Secondary NTP Server**: Optional fallback server
- **Sync Interval**: Every hour
- **Timezone**: Configurable via web UI (offset in hours from UTC)
- **Automatic Fallback**: If primary server fails, automatically tries secondary

### Timezone Examples

- **EST (Eastern Standard Time)**: -5 hours
- **PST (Pacific Standard Time)**: -8 hours
- **CET (Central European Time)**: +1 hour
- **GMT (Greenwich Mean Time)**: 0 hours

---

## REST API Endpoints

The device provides REST API endpoints for automation integration (e.g., Home Assistant):

### Set Custom Message
```
POST http://<device-ip>/set_message
Parameters:
  - message: Text to display (A-Z, 0-9, spaces, basic punctuation)
  - speed: Scroll speed (10-200, optional, default: 40, lower = faster)
```

### Clear Custom Message
```
GET http://<device-ip>/clear_message
```

### Set Brightness
```
POST http://<device-ip>/set_brightness
Parameters:
  - value: 0-15 (brightness level) or -1 (turn display off)
```

### Export Configuration
```
GET http://<device-ip>/export
```
Downloads current configuration as JSON file.

### Factory Reset
```
GET http://<device-ip>/factory_reset
```
⚠️ **Warning**: Only available in AP mode. Erases all configuration and restarts device.

## Configuration via Web UI

All settings can be configured through the web interface:

### Basic Settings
1. **Brightness**: 0-100 (maps to LED intensity 0-15)
2. **Time Format**: 12-hour (with AM/PM) or 24-hour
3. **Invert Display**: Normal or inverted polarity
4. **Sleep Mode**: Enable/disable sleep functionality
5. **Sleep Times**: Start and end times for sleep period

### Time Settings
6. **Primary NTP Server**: Custom NTP server address (default: pool.ntp.org)
7. **Secondary NTP Server**: Optional fallback NTP server
8. **Timezone Offset**: Hours offset from UTC (supports decimals like 5.5 for IST)

### Display Options
9. **Show Day of Week Icon**: Enable/disable day icon prefix
10. **Show Date**: Enable/disable date display
11. **Blinking Colon**: Enable/disable animated colon
12. **Clock Display Duration**: Seconds to show clock (1-60)
13. **Weather Display Duration**: Seconds to show weather (1-60)

### Weather Configuration
14. **OpenWeatherMap API Key**: Your API key from openweathermap.org
15. **Weather Location**: City name (e.g., "London", "New York")

### Custom Messages
16. **Custom Message**: Persistent scroll message (max 39 characters)
17. **Message Scroll Speed**: 10-200 (lower = faster scrolling)

Changes take effect immediately and are saved to EEPROM.

---

## Technical Details

### EEPROM Layout

- Magic bytes: 'L', 'M'
- Version: 3 (with weather/messages support)
- Brightness, time format, invert, sleep settings
- Sleep start/end times
- Primary/secondary NTP servers (40 bytes each)
- Timezone offset (4 bytes, int32_t)
- Display options (day icon, date, blinking colon)
- Clock/weather display durations
- Weather API key and location (40 bytes each)
- Custom message and settings (40 bytes + flags)

### Libraries Required

- `ESP8266WiFi`
- `WiFiManager`
- `ESP8266WebServer`
- `ESP8266HTTPClient`
- `MD_Parola`
- `MD_MAX72xx`
- `ArduinoJson` (v6.x recommended)
- `SPI`
- `time.h` (built-in)

### Device ID

- Format: `LED_Clock_<MAC>` (no colons, uppercase)
- Used as WiFi hostname and AP SSID

---

## Troubleshooting

### Time Not Syncing

- Check WiFi connection
- Verify NTP server is accessible
- Check timezone offset is correct
- Wait a few minutes for initial sync

### Display Not Showing Time

- Check if sleep mode is active
- Verify brightness is not set to 0
- Check serial output for errors

### Can't Access Web UI

- Verify device is connected to WiFi
- Check device IP address (shown on display during boot)
- Ensure you're on the same network
- Try accessing via `http://<device-ip>`

### Entering Config Portal

- Reset device during "CONFIG?" prompt (first 5 seconds of boot)
- Or wait for WiFi connection failure (3 minute timeout)

---

## Credits & Acknowledgments

This project integrates code and features from the following excellent open-source projects:

### [ESPTimeCast](https://github.com/mfactory-osaka/ESPTimeCast)
**By mfactory-osaka**

This project was heavily inspired by ESPTimeCast, from which we've integrated:
- Weather display functionality (OpenWeatherMap integration)
- Custom scroll messages with REST API support
- Day of week icons and date display
- Blinking colon animation
- REST API endpoints for Home Assistant integration
- Enhanced web configuration UI
- Config export/backup functionality
- Secondary NTP server support
- Display duration controls

ESPTimeCast is a sleek, WiFi-connected LED matrix clock and weather display that combines real-time NTP sync, live weather updates, and a modern web-based configuration interface.

### [LEDMatrix_Clock8001Client](https://github.com/themusicnerd/LEDMatrix_Clock8001Client)
**By themusicnerd / Adrian Davis**

From this project, we've integrated:
- Orientation detection via GPIO16 pin
- Display flip functionality based on hardware pin state

LEDMatrix_Clock8001Client is a network-synced LED matrix clock with web configuration, RTC support, and Clock8001 UDP time integration.

### Original Project
**By Adrian Davis**

The base LED Matrix WiFi Clock functionality was originally developed by Adrian Davis, providing the foundation for NTP synchronization, WiFiManager integration, sleep mode, and EEPROM-based configuration storage.

---

## License

See LICENSE file for details.
