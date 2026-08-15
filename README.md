# InfoHub

InfoHub is a round/square ESP32-S3 info-display platform. Bambu Lab printer status is its flagship feature, but printer, weather (Tempest station or generic city/location) and stocks are each an independently toggleable plugin — pick the combination you want at build time.


![Mount](docs/1.png)

Current version: **Alpha v0.1.0**

## Supported hardware

Choose the correct hardware in the installer:

| Hardware | Installer name | Battery support |
| --- | --- | --- |
| Waveshare ESP32-S3 Touch AMOLED 1.75 | `1.75 AMOLED` | Yes, including charging and USB detection |
| Waveshare ESP32-S3 Touch LCD 2.8C | `2.8" LCD` | Yes, using the board's battery measurement |

Firmware for one variant must not be installed on the other variant.

## Plugins

InfoHub is built as a set of independently Kconfig-toggleable plugins, not a fixed feature set:

| Plugin | Default | What it shows |
| --- | --- | --- |
| Printer (Bambu Lab) | On | Print progress, temperatures, AMS, camera, cover previews, controls |
| Tempest | Off | Current conditions + hourly forecast, for owners of a WeatherFlow Tempest station |
| Weather (GeoWeather) | Off | Current conditions + hourly forecast for any city/location, via Open-Meteo — no API key needed |
| Stocks | Off | Live quotes for up to 4 tracked symbols, with a configurable daily API-request budget and last-refreshed display |

Enable any combination when building from source (see [Building InfoHub](docs/Build/README.md)); the pre-built installer images ship with all four enabled.

## Install InfoHub

You need a USB data cable and a desktop version of Chrome or Edge.

1. Open the **[InfoHub Web Installer](https://dreed47.github.io/InfoHub/flash/)**.
2. Select your hardware variant.
3. Connect the device by USB and choose **Install selected firmware**.
4. Keep the USB cable connected after installation.
5. The installer searches for nearby Wi-Fi networks and sends your Wi-Fi details directly to InfoHub over USB.
6. Open Web Config using the IP address shown by InfoHub and connect your Bambu printer.

### Wi-Fi fallback

If USB Wi-Fi setup is unavailable, connect to the fallback access point:

- Wi-Fi name: `InfoHub-Setup`
- Password: `infohub123`
- Setup page: [http://192.168.4.1](http://192.168.4.1)

Select your home Wi-Fi there. After InfoHub connects, continue through Web Config on its home-network IP address.

## Connect your printer

Web Config offers three connection modes:

- **Hybrid (recommended):** combines cloud and local data and automatically uses the better status path. Local camera snapshots remain available on supported printers.
- **Cloud only:** uses Bambu Cloud for status and cover previews. Local MQTT and local camera snapshots are disabled.
- **Local only:** connects directly to the printer without requiring a Bambu Cloud account. Cloud cover previews are unavailable.

For Bambu Cloud, enter your account details and complete an email-code or 2FA step if requested.

For a local connection, enter:

- Printer IP address or hostname
- Printer serial number
- Printer access code

Cloud and local printer settings can normally be connected without restarting InfoHub.

## Update without losing your settings

Use an **OTA update** for an already configured InfoHub. OTA keeps Wi-Fi, printer profiles and display settings.

1. Open the [InfoHub Web Installer](https://dreed47.github.io/InfoHub/flash/).
2. Select the same hardware variant currently installed on your device.
3. In **OTA Update**, enter the IP address shown by InfoHub.
4. Open the OTA updater and confirm **Flash from URL** in Web Config.

You can also open Web Config directly and use its **Firmware Update** section. It accepts either a matching OTA `.bin` file or a GitHub firmware URL.

The public installer performs the initial USB flash. OTA itself runs through Web Config so the firmware can safely update the inactive application slot and retain the configuration.

> InfoHub is in early alpha (v0.1.0). Expect rough edges and breaking changes between versions.

## Main features

- Print progress, remaining time, layers and temperatures
- AMS and external-spool information
- Cloud cover preview and project title
- Local JPEG camera snapshots on supported printers
- Pause, resume and stop controls
- Chamber-light control on supported printers
- Multi-printer profiles and live printer switching
- Bambu HMS and error descriptions on the device
- Adjustable display rotation and status colors
- Battery-aware dimming and screen-off settings
- Configurable sound notifications and custom WAV files
- Secure Web Config access using a temporary PIN
- OTA firmware updates that retain the device configuration
- Weather (Tempest station or generic city/location) and stocks plugins for non-printer use cases

## Printer and camera support

The printer plugin contains status paths for:

`A1`, `A1 Mini`, `A2L`, `P1P`, `P1S`, `P2S`, `H2C`, `H2D`, `H2D Pro`, `H2S`, `X1`, `X1C`, `X1E`, `X2D`

Local JPEG snapshots are available on `A1`, `A1 Mini`, `A2L`, `P1P` and `P1S`.

`P2S`, the `H2` family, the `X1` family and `X2D` use RTSP video. The ESP32-S3 cannot decode these streams, so their live camera view is not available in InfoHub. Cloud cover previews can still be shown.

The `H2` family and `X2D` require Developer Mode for local printer status. Availability of individual values can differ between printer models and firmware versions.

## Web Config

Web Config includes:

- Wi-Fi scanning and configuration
- Cloud and local printer connections
- Connection mode and printer selection
- Display rotation, colors and time zone
- Battery, USB and screen power-saving options
- AMS display behavior
- Sound notification settings
- Portal PIN protection
- Firmware updates by file upload or URL

After initial setup, Web Config can be protected with a temporary six-digit PIN. Long-press the InfoHub display for about one second to request a PIN.

## Known limitations

- RTSP camera streams cannot be displayed on the ESP32-S3.
- Newer printer families have received less real-world testing than `P1S` and `P1P`.
- Some local V2 protocol values can differ from the older V1 printer fields.
- Cloud features require internet access and a working Bambu Cloud account.

## Links

- [InfoHub Web Installer](https://dreed47.github.io/InfoHub/flash/)
- [MakerWorld model](https://makerworld.com/en/models/3175483-waveshare-amoled-1-75-test-development-mount)
- [Building, cloning and manual flashing](docs/Build/README.md)
- [License](LICENSE)
