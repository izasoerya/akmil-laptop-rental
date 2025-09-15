#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "SPIFFS.h"

enum SystemState
{
    ENROLL,
    WAITING_ENROLL,
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
        server.serveStatic("/", SPIFFS, "/root.html");
        server.serveStatic("/enroll_fingerprint.html", SPIFFS, "/enroll_fingerprint.html");
        server.serveStatic("/waiting_fingerprint.html", SPIFFS, "/waiting_fingerprint.html");
        server.serveStatic("/locked_page.html", SPIFFS, "/locked_page.html");
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
