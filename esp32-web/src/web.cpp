#include "web.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/reports.h"
#include "network.h"
#include "pages.h"

namespace {

constexpr size_t MAX_BODY = 1024;
constexpr uint32_t PUSH_PERIOD_MS = 1000;
constexpr uint32_t REBOOT_DELAY_MS = 1000;
const char ADMIN_USER[] = "admin";
const char REALM[] = "GPSDO";

AsyncWebServer server(80);
AsyncWebSocket socket("/ws");
Gpsdo* gpsdo = nullptr;
DeviceSettings* settings = nullptr;
SemaphoreHandle_t mutex = nullptr;

uint32_t last_push = 0;
volatile bool reboot_requested = false;
uint32_t reboot_at = 0;
bool update_authorized = false;
String update_error;

struct Guard {
    Guard() { web::lock(); }
    ~Guard() { web::unlock(); }
};

HostInfo hostInfo()
{
    HostInfo host;
    host.uptime_s = millis() / 1000;
    host.free_heap = ESP.getFreeHeap();
    host.rssi = WiFi.RSSI();
    host.ip = network::ipAddress().c_str();
    host.hostname = settings->hostname.c_str();
    host.ap_mode = network::accessPointActive();
    return host;
}

// Read-only views stay open. There is no password to ask for before the device is set up.
bool authorized(AsyncWebServerRequest* request)
{
    if (!settings->provisioned()) {
        return true;
    }
    return request->authenticate(ADMIN_USER, settings->admin.c_str());
}

bool requireAdmin(AsyncWebServerRequest* request)
{
    if (authorized(request)) {
        return true;
    }
    request->requestAuthentication(AsyncAuthType::AUTH_BASIC, REALM);
    return false;
}

void sendJson(AsyncWebServerRequest* request, int code, const std::string& json)
{
    AsyncWebServerResponse* response = request->beginResponse(code, "application/json", json.c_str());
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void sendError(AsyncWebServerRequest* request, int code, const char* message)
{
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = message;
    std::string json;
    serializeJson(doc, json);
    sendJson(request, code, json);
}

// Keeps the body of a request until the handler runs. The server frees _tempObject.
void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total)
{
    if (total > MAX_BODY) {
        return;
    }
    if (index == 0) {
        request->_tempObject = malloc(total + 1);
    }
    if (request->_tempObject != nullptr) {
        char* body = static_cast<char*>(request->_tempObject);
        memcpy(body + index, data, len);
        if (index + len == total) {
            body[total] = '\0';
        }
    }
}

// Waits for the replies of commands without blocking the server: as long as replies are missing, the
// response is asked for its data again later. A client leaving just frees this.
struct PendingReply {
    std::vector<std::pair<std::string, CommandResultPtr>> commands;
    std::string json;
    size_t sent = 0;
    bool ready = false;

    bool allDone()
    {
        Guard guard;
        for (auto& command : commands) {
            if (command.second->state == CommandResult::PENDING) {
                return false;
            }
        }
        return true;
    }

    void buildJson()
    {
        Guard guard;
        JsonDocument doc;
        bool all_ok = true;
        JsonObject results = doc["results"].to<JsonObject>();
        for (auto& command : commands) {
            JsonObject result = results[command.first].to<JsonObject>();
            bool ok = command.second->state == CommandResult::OK;
            result["ok"] = ok;
            if (!ok) {
                result["error"] = command.second->error;
                all_ok = false;
            }
        }
        doc["ok"] = all_ok;
        serializeJson(doc, json);
        ready = true;
    }
};

void sendWhenReplied(AsyncWebServerRequest* request, std::shared_ptr<PendingReply> reply)
{
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json", [reply](uint8_t* buffer, size_t max, size_t) -> size_t {
            if (!reply->ready) {
                if (!reply->allDone()) {
                    return RESPONSE_TRY_AGAIN;
                }
                reply->buildJson();
            }
            size_t count = std::min(max, reply->json.size() - reply->sent);
            memcpy(buffer, reply->json.data() + reply->sent, count);
            reply->sent += count;
            return count;
        });
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

// Goes in a command as is, so nothing that could break out of its field
bool validKey(const char* key)
{
    size_t len = strlen(key);
    if (len == 0 || len > 31) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

// JSON content type: not something a form on another web site can send
bool isJsonRequest(AsyncWebServerRequest* request)
{
    return request->contentType().startsWith("application/json");
}

void handlePostConfig(AsyncWebServerRequest* request)
{
    if (!requireAdmin(request)) {
        return;
    }
    if (!isJsonRequest(request)) {
        sendError(request, 415, "Expected application/json");
        return;
    }
    JsonDocument doc;
    const char* body = static_cast<const char*>(request->_tempObject);
    if (body == nullptr || deserializeJson(doc, body) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        sendError(request, 400, "Expected a JSON object of settings");
        return;
    }
    JsonObject values = doc.as<JsonObject>();
    if (values.size() == 0) {
        sendError(request, 400, "No setting");
        return;
    }
    for (JsonPair pair : values) {
        if (!validKey(pair.key().c_str()) || !pair.value().is<long long>()) {
            sendError(request, 400, "Keys are lowercase names, values are integers");
            return;
        }
    }
    auto reply = std::make_shared<PendingReply>();
    {
        Guard guard;
        uint32_t now = millis();
        for (JsonPair pair : values) {
            reply->commands.emplace_back(pair.key().c_str(), gpsdo->set(pair.key().c_str(), pair.value().as<long long>(), now));
        }
    }
    sendWhenReplied(request, reply);
}

void handlePostAction(AsyncWebServerRequest* request)
{
    if (!requireAdmin(request)) {
        return;
    }
    if (!isJsonRequest(request)) {
        sendError(request, 415, "Expected application/json");
        return;
    }
    JsonDocument doc;
    const char* body = static_cast<const char*>(request->_tempObject);
    if (body == nullptr || deserializeJson(doc, body) != DeserializationError::Ok) {
        sendError(request, 400, "Expected JSON");
        return;
    }
    const char* action = doc["action"] | "";
    const char* command = nullptr;
    if (strcmp(action, "savepwm") == 0) {
        command = "SAVEPWM";
    } else if (strcmp(action, "forcesync") == 0) {
        command = "FORCESYNC";
    } else {
        sendError(request, 400, "Unknown action");
        return;
    }
    auto reply = std::make_shared<PendingReply>();
    {
        Guard guard;
        reply->commands.emplace_back(action, gpsdo->command(command, millis()));
    }
    sendWhenReplied(request, reply);
}

void handleHistory(AsyncWebServerRequest* request)
{
    History::Range range = History::RANGE_1H;
    if (request->hasParam("range")) {
        const String& value = request->getParam("range")->value();
        if (value == "24h") {
            range = History::RANGE_24H;
        } else if (value != "1h") {
            sendError(request, 400, "range is 1h or 24h");
            return;
        }
    }
    auto reader = std::make_shared<History::Reader>();
    {
        Guard guard;
        *reader = gpsdo->history().startReader(range);
    }
    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json", [reader](uint8_t* buffer, size_t max, size_t) -> size_t {
            if (max < 64) {
                return RESPONSE_TRY_AGAIN;
            }
            Guard guard;
            return gpsdo->history().read(*reader, reinterpret_cast<char*>(buffer), max);
        });
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

String htmlEscape(const String& text)
{
    String out;
    out.reserve(text.length());
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

void scheduleReboot()
{
    reboot_at = millis() + REBOOT_DELAY_MS;
    reboot_requested = true;
}

String formValue(AsyncWebServerRequest* request, const char* name)
{
    return request->hasParam(name, true) ? request->getParam(name, true)->value() : String();
}

void sendSetupPage(AsyncWebServerRequest* request, const String& message)
{
    String page = FPSTR(pages::SETUP);
    page.replace("%MESSAGE%", message.length() ? "<p class=err>" + htmlEscape(message) + "</p>" : String());
    page.replace("%SSID%", htmlEscape(settings->ssid));
    page.replace("%HOSTNAME%", htmlEscape(settings->hostname));
    page.replace("%STATIC%", settings->static_ip ? "checked" : "");
    page.replace("%IP%", htmlEscape(settings->ip));
    page.replace("%GATEWAY%", htmlEscape(settings->gateway));
    page.replace("%SUBNET%", htmlEscape(settings->subnet));
    page.replace("%DNS%", htmlEscape(settings->dns));
    page.replace("%KEEP%", settings->provisioned() ? "leave empty to keep the current one" : "");
    request->send(200, "text/html", page);
}

void handleSetupPost(AsyncWebServerRequest* request)
{
    DeviceSettings updated = *settings;
    updated.ssid = formValue(request, "ssid");
    String password = formValue(request, "password");
    if (password.length() > 0) {
        updated.password = password;
    }
    String admin = formValue(request, "admin");
    if (admin.length() > 0) {
        updated.admin = admin;
    }
    updated.hostname = formValue(request, "hostname");
    updated.hostname.toLowerCase();
    updated.static_ip = request->hasParam("static", true);
    updated.ip = formValue(request, "ip");
    updated.gateway = formValue(request, "gateway");
    updated.subnet = formValue(request, "subnet");
    updated.dns = formValue(request, "dns");

    bool valid_host = updated.hostname.length() > 0 && updated.hostname.length() <= 32;
    for (size_t i = 0; valid_host && i < updated.hostname.length(); i++) {
        char c = updated.hostname[i];
        valid_host = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i > 0 && i + 1 < updated.hostname.length());
    }
    IPAddress check;
    String error;
    if (updated.ssid.length() == 0 || updated.ssid.length() > 32) {
        error = "A network name is needed, 32 characters at most.";
    } else if (updated.password.length() > 63) {
        error = "The WiFi password is 63 characters at most.";
    } else if (!valid_host) {
        error = "The hostname takes letters, digits and hyphens.";
    } else if (updated.admin.length() == 0) {
        error = "An admin password is needed.";
    } else if (updated.static_ip && (!check.fromString(updated.ip) || !check.fromString(updated.gateway) || !check.fromString(updated.subnet))) {
        error = "A static IP needs an address, a gateway and a subnet mask.";
    }
    if (error.length() > 0) {
        // Keeps what was entered out of the live settings
        sendSetupPage(request, error);
        return;
    }
    updated.save();
    String page = FPSTR(pages::SAVED);
    page.replace("%HOSTNAME%", htmlEscape(updated.hostname));
    page.replace("%SSID%", htmlEscape(updated.ssid));
    request->send(200, "text/html", page);
    scheduleReboot();
}

void handleUpload(AsyncWebServerRequest* request, const String&, size_t index, uint8_t* data, size_t len, bool final)
{
    if (index == 0) {
        update_error = "";
        update_authorized = authorized(request);
        if (!update_authorized) {
            return;
        }
        bool filesystem = request->hasParam("target") && request->getParam("target")->value() == "fs";
        if (filesystem) {
            LittleFS.end();
        }
        Serial.printf("Update of the %s\n", filesystem ? "filesystem" : "firmware");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, filesystem ? U_SPIFFS : U_FLASH)) {
            update_error = Update.errorString();
        }
    }
    if (!update_authorized || update_error.length() > 0) {
        return;
    }
    if (Update.write(data, len) != len) {
        update_error = Update.errorString();
        Update.abort();
        return;
    }
    if (final && !Update.end(true)) {
        update_error = Update.errorString();
    }
}

void handleUpdateDone(AsyncWebServerRequest* request)
{
    if (!requireAdmin(request)) {
        return;
    }
    if (update_error.length() > 0 || !Update.isFinished()) {
        String message = update_error.length() > 0 ? update_error : String("Nothing was received");
        request->send(500, "text/plain", "Update failed: " + message);
        // The filesystem may have been unmounted for this
        LittleFS.begin(false);
        return;
    }
    request->send(200, "text/plain", "Update done, restarting.");
    scheduleReboot();
}

void onSocketEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type, void*, uint8_t*, size_t)
{
    if (type == WS_EVT_CONNECT) {
        std::string json;
        {
            Guard guard;
            json = configJson(*gpsdo);
        }
        client->text(json.c_str(), json.size());
    }
}

void redirectToSetup(AsyncWebServerRequest* request)
{
    request->redirect("http://" + WiFi.softAPIP().toString() + "/setup");
}

}  // namespace

namespace web {

void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mutex); }
void updateMutexInit() { mutex = xSemaphoreCreateMutex(); }

void begin(Gpsdo& gpsdo_ref, DeviceSettings& settings_ref)
{
    gpsdo = &gpsdo_ref;
    settings = &settings_ref;

    socket.onEvent(onSocketEvent);
    server.addHandler(&socket);

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        std::string json;
        {
            Guard guard;
            json = statusJson(*gpsdo, hostInfo(), millis());
        }
        sendJson(request, 200, json);
    });
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        std::string json;
        {
            Guard guard;
            json = configJson(*gpsdo);
        }
        sendJson(request, 200, json);
    });
    server.on("/api/config", HTTP_POST, handlePostConfig, nullptr, collectBody);
    server.on("/api/action", HTTP_POST, handlePostAction, nullptr, collectBody);
    server.on("/api/history", HTTP_GET, handleHistory);
    server.on("/api/console", HTTP_GET, [](AsyncWebServerRequest* request) {
        std::string text;
        {
            Guard guard;
            text = gpsdo->console().text();
        }
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain", text.c_str());
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest* request) {
        std::string text;
        {
            Guard guard;
            text = metricsText(*gpsdo, hostInfo(), millis());
        }
        request->send(200, "text/plain; version=0.0.4", text.c_str());
    });

    server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (requireAdmin(request)) {
            sendSetupPage(request, String());
        }
    });
    server.on("/setup", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (requireAdmin(request)) {
            handleSetupPost(request);
        }
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (requireAdmin(request)) {
            request->send(200, "text/html", FPSTR(pages::UPDATE));
        }
    });
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);

    // Until the web interface is uploaded, or when the device isn't set up yet
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (!settings->provisioned()) {
            redirectToSetup(request);
        } else if (LittleFS.exists("/index.html.gz") || LittleFS.exists("/index.html")) {
            AsyncWebServerResponse* response = LittleFS.exists("/index.html.gz")
                ? request->beginResponse(LittleFS, "/index.html.gz", "text/html")
                : request->beginResponse(LittleFS, "/index.html", "text/html");
            if (LittleFS.exists("/index.html.gz")) {
                response->addHeader("Content-Encoding", "gzip");
            }
            response->addHeader("Cache-Control", "no-cache");
            request->send(response);
        } else {
            request->send(200, "text/html", FPSTR(pages::NO_UI));
        }
    });
    server.serveStatic("/", LittleFS, "/").setCacheControl("no-cache");

    server.onNotFound([](AsyncWebServerRequest* request) {
        // Captive portal: phones and computers probe a known page right after joining
        if (network::accessPointActive() && request->host() != WiFi.softAPIP().toString()) {
            redirectToSetup(request);
        } else {
            request->send(404, "text/plain", "Not found");
        }
    });

    server.begin();
}

void loop(uint32_t now)
{
    if (reboot_requested && (int32_t)(now - reboot_at) >= 0) {
        ESP.restart();
    }
    if ((now - last_push) < PUSH_PERIOD_MS) {
        return;
    }
    last_push = now;
    socket.cleanupClients();
    bool has_clients = socket.count() > 0;
    std::string status, config;
    {
        Guard guard;
        bool config_changed = gpsdo->takeConfigChanged();
        if (has_clients) {
            status = statusJson(*gpsdo, hostInfo(), now);
            if (config_changed) {
                config = configJson(*gpsdo);
            }
        }
    }
    if (has_clients) {
        socket.textAll(status.c_str(), status.size());
        if (!config.empty()) {
            socket.textAll(config.c_str(), config.size());
        }
    }
}

}  // namespace web
