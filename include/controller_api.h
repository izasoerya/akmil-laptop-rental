#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "SPIFFS.h"

enum SystemState
{
    LOCKED,
    UNLOCKED_SCANNING,
    SUCCESS
};

class ControllerAPI
{
public:
    ControllerAPI() {}
    ~ControllerAPI() {}

    void addStaticSite(WebServer &server)
    {
        server.serveStatic("/", SPIFFS, "/index.html");
        server.serveStatic("/locked_page.html", SPIFFS, "/locked_page.html");
        server.serveStatic("/qr_recognition.html", SPIFFS, "/qr_recognition.html");
        server.serveStatic("/success_page.html", SPIFFS, "/success_page.html");
    }

    void startWebServer(WebServer &server, SystemState state)
    {
        server.on("/status", HTTP_GET, [&]()
                  {
        String json = "{\"state\":\"" + String(state) + "\",\"payload\":\"" + "Success Registration" + "\"}";
        server.send(200, "application/json", json); });
        server.begin();
    }
};
