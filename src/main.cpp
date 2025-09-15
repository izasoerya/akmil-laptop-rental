#include <Arduino.h>
#include <ESP32QRCodeReader.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>

#include "controller_api.h"

EspSoftwareSerial::UART sserial;
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&sserial);

WebServer server(80);
ControllerAPI api;
ESP32QRCodeReader reader(CAMERA_MODEL_WROVER_KITS);

volatile SystemState systemState = LOCKED;
SemaphoreHandle_t state_mutex;

TaskHandle_t streamingTaskHandle = NULL;
TaskHandle_t qrCodeTaskHandle = NULL;
TaskHandle_t fingerprintTaskHandle = NULL;

SemaphoreHandle_t frame_mutex;
uint8_t *last_jpeg = nullptr;
size_t last_jpeg_len = 0;
String data = "";

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

void handle_jpg_stream()
{
  if (systemState != UNLOCKED_SCANNING)
  {
    server.send(403, "text/plain", "Access Denied: Stream not active.");
    return;
  }
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);
  while (client.connected() && systemState == UNLOCKED_SCANNING)
  {
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      if (last_jpeg && last_jpeg_len > 0)
      {
        client.print("--frame\r\nContent-Type: image/jpeg\r\n\r\n");
        client.write(last_jpeg, last_jpeg_len);
        client.print("\r\n");
      }
      xSemaphoreGive(frame_mutex);
    }
    delay(30);
  }
}

void onFingerprintTask(void *pvParameters)
{
  while (true)
  {
    int finger_id = getFingerprintIDez();
    if (finger_id > 0)
    {
      Serial.print("Found ID #");
      Serial.println(finger_id);

      xSemaphoreTake(state_mutex, portMAX_DELAY);
      systemState = UNLOCKED_SCANNING;
      data = "Fingerprint OK. Scan QR Code...";
      xSemaphoreGive(state_mutex);

      Serial.println("Resuming Streaming and QR Code tasks...");
      vTaskResume(streamingTaskHandle);
      vTaskResume(qrCodeTaskHandle);

      vTaskSuspend(NULL);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void onStreamingTask(void *pvParameters)
{
  while (true)
  {
    camera_fb_t *fb = reader.getLastFrameBuffer();
    if (!fb)
    {
      vTaskDelay(30 / portTICK_PERIOD_MS);
      continue;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, 12, &jpg_buf, &jpg_len))
    {
      if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
      {
        if (last_jpeg)
          free(last_jpeg);
        last_jpeg = (uint8_t *)malloc(jpg_len);
        if (last_jpeg)
        {
          memcpy(last_jpeg, jpg_buf, jpg_len);
          last_jpeg_len = jpg_len;
        }
        else
        {
          last_jpeg_len = 0;
        }
        xSemaphoreGive(frame_mutex);
      }
      free(jpg_buf);
    }
    vTaskDelay(30 / portTICK_PERIOD_MS);
  }
}

void onQrCodeTask(void *pvParameters)
{
  struct QRCodeData qrCodeData;
  while (true)
  {
    if (reader.receiveQrCode(&qrCodeData, 100))
    {
      if (qrCodeData.valid)
      {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        data = String((const char *)qrCodeData.payload);
        systemState = SUCCESS;
        Serial.print("QR Payload: ");
        Serial.println(data);
        xSemaphoreGive(state_mutex);

        vTaskSuspend(streamingTaskHandle);

        Serial.println("SUCCESS! Displaying payload for 30 seconds, then resetting.");
        vTaskDelay(30000 / portTICK_PERIOD_MS);

        Serial.println("Resetting MCU now...");
        ESP.restart();
      }
    }
    vTaskDelay(250 / portTICK_PERIOD_MS);
  }
}

void setup()
{
  Serial.begin(9600);
  Serial.println("\n\nSystem Booting...");

  sserial.begin(57600, SWSERIAL_8N1, 32, 33);
  if (finger.verifyPassword())
  {
    Serial.println("Found fingerprint sensor!");
  }
  else
  {
    Serial.println("Did not find fingerprint sensor :(");
    return;
  }

  SPIFFS.begin(true);
  api.addStaticSite(server);
  api.startWebServer(server, systemState);

  frame_mutex = xSemaphoreCreateMutex();
  state_mutex = xSemaphoreCreateMutex();

  reader.setup();
  Serial.println("Setup QRCode Reader");
  reader.beginOnCore(1);
  Serial.println("Begin on Core 1");

  WiFi.begin("Subhanallah", "muhammadnabiyullah");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  Serial.print("Web Server Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  Serial.println("System is LOCKED. Waiting for fingerprint...");

  xTaskCreatePinnedToCore(onFingerprintTask, "Fingerprint", 4 * 1024, NULL, 4, &fingerprintTaskHandle, 0);
  xTaskCreate(onStreamingTask, "Streaming", 4 * 1024, NULL, 5, &streamingTaskHandle);
  xTaskCreate(onQrCodeTask, "QRCode", 4 * 1024, NULL, 3, &qrCodeTaskHandle);

  vTaskSuspend(streamingTaskHandle);
  vTaskSuspend(qrCodeTaskHandle);
}

void loop()
{
  server.handleClient();
}