#pragma once

#include <Arduino.h>
#include <Adafruit_Fingerprint.h>

class FingerprintService
{
private:
    String id;
    Adafruit_Fingerprint finger;

    int getFingerprintIDez()
    {
        uint8_t p = finger.getImage();
        if (p != FINGERPRINT_OK)
            return -1;
        p = finger.image2Tz();
        if (p != FINGERPRINT_OK)
            return -1;
        p = finger.fingerFastSearch();
        if (p != FINGERPRINT_OK)
            return -2;
        return finger.fingerID;
    }

public:
    FingerprintService::FingerprintService() : finger(&Serial2) {}
    ~FingerprintService() {}

    void begin()
    {
        if (finger.verifyPassword())
        {
            Serial.println("Found fingerprint sensor!");
        }
        else
        {
            Serial.println("Did not find fingerprint sensor :(");
            while (1)
            {
                delay(1);
            }
        }
    }
    void enrollFinger() {}
    int matchFinger()
    {
        int finger_id = getFingerprintIDez();
        if (finger_id == 0)
        {
            return false;
        }
        return finger_id;
    }
};