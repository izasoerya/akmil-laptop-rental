#include <Arduino.h>
#include <ESP32QRCodeReader.h>
#include <WiFi.h>
#include <WebServer.h>

// For the common black AI-Thinker boards, use:
// #define CAMERA_MODEL_AI_THINKER
// Ensure this model is correct for your specific board.
WebServer server(80);
ESP32QRCodeReader reader(CAMERA_MODEL_WROVER_KITS);

// Globals for sharing data between tasks
SemaphoreHandle_t frame_mutex;
uint8_t* last_jpeg = nullptr;
size_t last_jpeg_len = 0;
String data = ""; // For QR code payload

void handle_jpg_stream() {
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client.connected()) {
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (last_jpeg && last_jpeg_len > 0) {
        String part = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
        server.sendContent(part);
        client.write(last_jpeg, last_jpeg_len);
        server.sendContent("\r\n");
      }
      xSemaphoreGive(frame_mutex);
    }
    delay(30); // Small delay to prevent this loop from starving other processes
  }
}

void handle_root() {
  String html = R"rawliteral(
    <html>
    <head><title>ESP32-CAM Interface</title></head>
    <body>
      <h2>ESP32-CAM Stream</h2>
      <img src='/stream'><br>
      <h3>Last QR Payload:</h3>
      <div id='qr' style='font-size:1.5em;color:green;'></div>
      <script>
        function updateQR() {
          fetch('/qr').then(r => r.text()).then(t => {
            document.getElementById('qr').textContent = t;
          });
        }
        setInterval(updateQR, 2000);
        updateQR();
      </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handle_qr() {
  server.send(200, "text/plain", data);
}

void startCameraServerPlain() {
  server.on("/", HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_jpg_stream);
  server.on("/qr", HTTP_GET, handle_qr);
  server.begin();
}

void onStreamingTask(void *pvParameters) {
  while(true) {
    camera_fb_t *fb = reader.getLastFrameBuffer(); 
    if (!fb) {
      vTaskDelay(30 / portTICK_PERIOD_MS);
      continue;
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, 60, &jpg_buf, &jpg_len)) {
      if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (last_jpeg) free(last_jpeg);
        last_jpeg = (uint8_t*) malloc(jpg_len);
        if (last_jpeg) {
          memcpy(last_jpeg, jpg_buf, jpg_len);
          last_jpeg_len = jpg_len;
        } else {
          last_jpeg_len = 0;
        }
        xSemaphoreGive(frame_mutex);
      }
      free(jpg_buf);
    }
    vTaskDelay(30 / portTICK_PERIOD_MS); // Control frame rate
  }
}

void onQrCodeTask(void *pvParameters)
{
  struct QRCodeData qrCodeData;
  while (true)
  {
    if (reader.receiveQrCode(&qrCodeData, 100)) {
      if (qrCodeData.valid) {
        data = String((const char *)qrCodeData.payload);
        Serial.print("Payload: ");
        Serial.println(data);
      }
    }
    vTaskDelay(250 / portTICK_PERIOD_MS); 
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println();

  frame_mutex = xSemaphoreCreateMutex();

  reader.setup();
  Serial.println("Setup QRCode Reader");
  reader.beginOnCore(1);
  Serial.println("Begin on Core 1");

  WiFi.begin("Subhanallah", "muhammadnabiyullah");
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServerPlain();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  xTaskCreate(onStreamingTask, "Streaming", 4 * 1024, NULL, 5, NULL); // Higher priority
  xTaskCreate(onQrCodeTask, "QRCode", 4 * 1024, NULL, 4, NULL);      // Lower priority
}

void loop()
{
  server.handleClient();
}