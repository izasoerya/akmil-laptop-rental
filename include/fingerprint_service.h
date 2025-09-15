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

    bool enrollFinger(int id)
    {
        Serial.println("Waiting for valid finger to enroll as ID #" + String(id));
        int p = -1;
        Serial.println("Place finger on sensor...");
        while (p != FINGERPRINT_OK)
        {
            p = finger.getImage();
            if (p == FINGERPRINT_NOFINGER)
            {
                delay(50);
            }
            else if (p != FINGERPRINT_OK)
            {
                Serial.println("Error capturing image. Try again.");
                return false;
            }
        }

        p = finger.image2Tz(1);
        if (p != FINGERPRINT_OK)
        {
            Serial.println("Error converting image. Try again.");
            return false;
        }

        Serial.println("Remove finger...");
        while (finger.getImage() != FINGERPRINT_NOFINGER)
        {
            delay(50);
        }

        Serial.println("Place same finger again...");
        p = -1;
        while (p != FINGERPRINT_OK)
        {
            p = finger.getImage();
            if (p == FINGERPRINT_NOFINGER)
            {
                delay(50);
            }
            else if (p != FINGERPRINT_OK)
            {
                Serial.println("Error capturing image. Try again.");
                return false;
            }
        }

        p = finger.image2Tz(2);
        if (p != FINGERPRINT_OK)
        {
            Serial.println("Error converting image. Try again.");
            return false;
        }

        p = finger.createModel();
        if (p != FINGERPRINT_OK)
        {
            Serial.println("Error creating model. Try again.");
            return false;
        }

        p = finger.storeModel(id);
        if (p == FINGERPRINT_OK)
        {
            Serial.println("Fingerprint enrolled successfully!");
            return true;
        }
        else
        {
            Serial.println("Error storing fingerprint. Try again.");
            return false;
        }
    }

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