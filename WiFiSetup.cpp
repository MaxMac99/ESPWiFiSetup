//
// Created by Max Vissing on 2019-04-16.
//

#include "WiFiSetup.h"


WiFiSetup::WiFiSetup() : connected(false), lastUpdate(0) {
}

WiFiSetup::WiFiSetup(String ssid, String password, String hostname,
                     std::function<void(String, String)> callback)
        : ssid(std::move(ssid)), password(std::move(password)), hostname(std::move(hostname)),
        changeCallback(std::move(callback)), connected(false), lastUpdate(0) {
}

void WiFiSetup::start(bool untilConnected) {
    WiFi.hostname(hostname);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    if (ssid != "") {
        if (connectWifi(ssid, password) == WL_CONNECTED) {
            return;
        }
    }

    startConfigPortal(hostname);

    while(untilConnected && !connected) {
        if (millis() - lastUpdate >= RETRY_INVERTVAL) {
            lastUpdate = millis();
            if (connectWifi(ssid, password) == WL_CONNECTED) {
                connected = true;
            }
        }
        handleConfigPortal();
        yield();
    }
}

void WiFiSetup::update() {
    if (millis() - lastUpdate >= REFETCH_INTERVAL && connected) {
        lastUpdate = millis();
        if (WiFi.status() == WL_CONNECTED) {
            return;
        }
        if (connectWifi(ssid, password) == WL_CONNECTED) {
            return;
        }

        startConfigPortal(hostname);
    } else if (!connected) {
        if (millis() - lastUpdate >= RETRY_INVERTVAL) {
            lastUpdate = millis();
            if (connectWifi(ssid, password) == WL_CONNECTED) {
                connected = true;
            }
        }
        if (server.get() != nullptr) {
            handleConfigPortal();
        }
    }
}

void WiFiSetup::startConfigPortal(const String& apName) {
    WiFi.mode(WIFI_AP_STA);
    connected = false;

    dns.reset(new DNSServer());
    server.reset(new ESP8266WebServer(80));

    WiFi.softAP(apName);

    const byte DNS_PORT = 53;
    dns->setErrorReplyCode(DNSReplyCode::NoError);
    dns->start(DNS_PORT, "*", WiFi.softAPIP());

    server->on(String(F("/")), std::bind(&WiFiSetup::handleRoot, this));
    server->on(String(F("/wifisave")), HTTP_POST, std::bind(&WiFiSetup::handleWifiSave, this));
    server->onNotFound (std::bind(&WiFiSetup::handleNotFound, this));
    server->begin();
}

void WiFiSetup::handleConfigPortal() {
    dns->processNextRequest();
    server->handleClient();

    if (connected) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);

            server.reset();
            dns.reset();
        }
    }
}

uint8_t WiFiSetup::connectWifi(const String& ssid, const String& password) {
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        return WL_CONNECTED;
    }

    if (ssid != "") {
        WiFi.begin(ssid, password);
    } else {
        if (WiFi.SSID()) {
            ETS_UART_INTR_DISABLE();
            wifi_station_disconnect();
            ETS_UART_INTR_ENABLE();

            WiFi.begin();
        }
    }

    uint8_t result = WiFi.waitForConnectResult();
    return result;
}

void WiFiSetup::handleRoot() {
    String page = FPSTR(HTML_HEAD);
    page.replace("{v}", "Set up LED Controller");
    page += FPSTR(HTML_STYLE);
    page += FPSTR(HTML_SCRIPT);
    page += FPSTR(HTML_HEAD_END);

    auto n = static_cast<int>(WiFi.scanNetworks());
    if (n == 0) {
        page += F("No networks found!");
    } else {
        int indices[n];
        for (int i = 0; i < n; i++) {
            indices[i] = i;
        }

        std::sort(indices, indices + n, [](const int &a, const int &b) -> bool
        {
            return WiFi.RSSI((uint8_t) a) > WiFi.RSSI((uint8_t) b);
        });

        String cssid;
        for (int i = 0; i < n; i++) {
            if (indices[i] == -1) continue;
            cssid = WiFi.SSID(static_cast<uint8_t>(indices[i]));
            for (int j = i + 1; j < n; j++) {
                if (cssid == WiFi.SSID(static_cast<uint8_t>(indices[j]))) {
                    indices[j] = -1; // set dup aps to index -1
                }
            }
        }

        page += FPSTR(HTML_SSIDS_START);
        for (int i = 0; i < n; i++) {
            if (indices[i] == -1) {
                continue;
            }

            auto item = static_cast<uint8_t>(indices[i]);
            int rssi = WiFi.RSSI(item);
            int quality = 0;
            if (rssi >= -50) {
                quality = 100;
            } else if (rssi > -100) {
                quality = 2 * (rssi + 100);
            }

            String httpItem = FPSTR(HTML_SSID_ITEM);
            httpItem.replace("{v}", WiFi.SSID(item));
            httpItem.replace("{r}", String(quality) + " %");
            if (WiFi.encryptionType(item) != ENC_TYPE_NONE) {
                httpItem.replace("{i}", "&#128274;");
            } else {
                httpItem.replace("{i}", "&#128275;");
            }
            page += httpItem;
        }
    }

    page += FPSTR(HTTP_SSIDS_END);
    page += FPSTR(HTML_FORM);

    page += FPSTR(HTML_END);

    server->sendHeader("Content-Length", String(page.length()));
    server->send(200, "text/html", page);
}

void WiFiSetup::handleWifiSave() {
    String newSSID = server->arg("ssid");
    String newPassword = server->arg("password");

    uint8_t result;
    if (newSSID == "") {
        result = WL_DISCONNECTED;
    } else {
        result = connectWifi(newSSID, newPassword);
    }

    if (result == WL_CONNECTED) {
        server->send(200);

        changeCallback(newSSID, newPassword);
        connected = true;
    } else {
        String response = "Could not connect to network \"" + newSSID + "\".";
        server->sendHeader("Content-Length", String(response.length()));
        server->send(400, "text", response);
    }
}

void WiFiSetup::handleNotFound() {
    if (captivePortal()) { // If captive portal redirect instead of displaying the error page.
        return;
    }
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server->uri();
    message += "\nMethod: ";
    message += ( server->method() == HTTP_GET ) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server->args();
    message += "\n";

    for ( uint8_t i = 0; i < server->args(); i++ ) {
        message += " " + server->argName ( i ) + ": " + server->arg ( i ) + "\n";
    }
    server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server->sendHeader("Pragma", "no-cache");
    server->sendHeader("Expires", "-1");
    server->sendHeader("Content-Length", String(message.length()));
    server->send (404, "text/plain", message);
}

bool WiFiSetup::captivePortal() {
    if (!isIp(server->hostHeader()) ) {
        server->sendHeader("Location", String("http://") + toStringIp(server->client().localIP()), true);
        server->send(302, "text/plain", ""); // Empty content inhibits Content-length header so we have to close the socket ourselves.
        server->client().stop(); // Stop is needed because we sent no content length
        return true;
    }
    return false;
}

bool WiFiSetup::isIp(const String& str) {
    for (unsigned int i = 0; i < str.length(); i++) {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

String WiFiSetup::toStringIp(const IPAddress& ip) {
    String res = "";
    for (int i = 0; i < 3; i++) {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

void WiFiSetup::setSsid(const String &ssid) {
    WiFiSetup::ssid = ssid;
}

void WiFiSetup::setPassword(const String &password) {
    WiFiSetup::password = password;
}

void WiFiSetup::setHostname(const String &hostname) {
    WiFiSetup::hostname = hostname;
}

void WiFiSetup::setChangeCallback(const std::function<void(String, String)> &changeCallback) {
    WiFiSetup::changeCallback = changeCallback;
}
