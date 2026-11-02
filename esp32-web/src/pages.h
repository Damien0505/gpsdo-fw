// Pages that must work without the web interface on LittleFS.
#pragma once

#include <Arduino.h>

namespace pages {

#define GPSDO_PAGE_STYLE \
    "<meta name=viewport content='width=device-width,initial-scale=1'><style>" \
    ":root{color-scheme:light dark;--bg:#f6f7f7;--fg:#1a1d1e;--muted:#626a6d;--line:#c9cfd1;--accent:#0b6e4f;--err:#b3261e}" \
    "@media(prefers-color-scheme:dark){:root{--bg:#121516;--fg:#e4e8e9;--muted:#9aa3a6;--line:#3a4144;--accent:#4cc38a;--err:#f2867e}}" \
    "body{font:16px system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0 auto;padding:16px;max-width:440px}" \
    "h1{font-size:20px;margin:8px 0 16px}label{display:block;margin:14px 0 4px;font-weight:600}small{color:var(--muted);font-weight:400}" \
    "input[type=text],input[type=password],input[type=file]{box-sizing:border-box;width:100%;padding:10px;font:inherit;background:transparent;color:inherit;border:1px solid var(--line);border-radius:6px}" \
    "button{margin-top:20px;padding:10px 18px;font:inherit;font-weight:600;background:var(--accent);color:var(--bg);border:0;border-radius:6px;cursor:pointer}" \
    "a{color:var(--accent)}.err{color:var(--err);font-weight:600}fieldset{border:1px solid var(--line);border-radius:6px;margin:20px 0 0;padding:4px 12px 14px}" \
    "legend{color:var(--muted);padding:0 4px}.check{display:flex;gap:8px;align-items:center;font-weight:400}" \
    "</style>"

const char SETUP[] PROGMEM =
    "<!doctype html><html lang=en><meta charset=utf-8><title>GPSDO setup</title>" GPSDO_PAGE_STYLE
    "<h1>GPSDO setup</h1>%MESSAGE%"
    "<form method=post action=/setup>"
    "<label for=ssid>WiFi network</label><input type=text id=ssid name=ssid value=\"%SSID%\" maxlength=32 required autocapitalize=off>"
    "<label for=password>WiFi password <small>%KEEP%</small></label><input type=password id=password name=password maxlength=63 autocomplete=off>"
    "<label for=hostname>Hostname <small>reached at name.local</small></label><input type=text id=hostname name=hostname value=\"%HOSTNAME%\" maxlength=32 required autocapitalize=off>"
    "<label for=admin>Admin password <small>%KEEP%</small></label><input type=password id=admin name=admin autocomplete=new-password>"
    "<small>Needed to change settings, with the user name <b>admin</b>. This guards against accidents on a trusted network: it is sent unencrypted.</small>"
    "<fieldset><legend>Static IP <small>optional</small></legend>"
    "<label class=check><input type=checkbox name=static %STATIC%>Use a static IP instead of DHCP</label>"
    "<label for=ip>Address</label><input type=text id=ip name=ip value=\"%IP%\" inputmode=decimal>"
    "<label for=gateway>Gateway</label><input type=text id=gateway name=gateway value=\"%GATEWAY%\" inputmode=decimal>"
    "<label for=subnet>Subnet mask</label><input type=text id=subnet name=subnet value=\"%SUBNET%\" inputmode=decimal>"
    "<label for=dns>DNS <small>the gateway if empty</small></label><input type=text id=dns name=dns value=\"%DNS%\" inputmode=decimal>"
    "</fieldset><button>Save and restart</button></form>";

const char SAVED[] PROGMEM =
    "<!doctype html><html lang=en><meta charset=utf-8><title>GPSDO setup</title>" GPSDO_PAGE_STYLE
    "<h1>Saved, restarting</h1><p>The GPSDO interface is joining <b>%SSID%</b>. Connect to that network and open "
    "<a href='http://%HOSTNAME%.local/'>http://%HOSTNAME%.local/</a>.</p>"
    "<p>If it can't join within a minute, the <b>GPSDO-Setup</b> network comes back so you can correct the settings.</p>";

const char UPDATE[] PROGMEM =
    "<!doctype html><html lang=en><meta charset=utf-8><title>GPSDO update</title>" GPSDO_PAGE_STYLE
    "<h1>Update the ESP32</h1>"
    "<form method=post action=/update enctype=multipart/form-data>"
    "<label for=fw>Firmware <small>firmware.bin</small></label><input type=file id=fw name=file accept=.bin required>"
    "<button>Upload firmware</button></form>"
    "<form method=post action='/update?target=fs' enctype=multipart/form-data>"
    "<label for=fs>Web interface <small>littlefs.bin</small></label><input type=file id=fs name=file accept=.bin required>"
    "<button>Upload web interface</button></form>"
    "<p><small>This updates the ESP32 only. The GPSDO itself is flashed with an ST-Link.</small></p>";

const char NO_UI[] PROGMEM =
    "<!doctype html><html lang=en><meta charset=utf-8><title>GPSDO</title>" GPSDO_PAGE_STYLE
    "<h1>Web interface not installed</h1><p>The ESP32 firmware is running, but its filesystem is empty. "
    "<a href=/update>Upload littlefs.bin</a>, or run <code>pio run -t uploadfs</code>.</p>"
    "<p><a href=/api/status>Status as JSON</a> &middot; <a href=/setup>Setup</a></p>";

}  // namespace pages
