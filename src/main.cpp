#include <Arduino.h>
#include <ESP32QRCodeReader.h>
#include <WiFi.h>
#include <WebServer.h>

WebServer server(80);
ESP32QRCodeReader reader(CAMERA_MODEL_WROVER_KITS);
SemaphoreHandle_t frame_mutex;
uint8_t* last_jpeg = nullptr;
size_t last_jpeg_len = 0;

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
    delay(100);
    if (!client.connected()) break;
  }
}

void handle_root() {
  String html = R"rawliteral(
    <html>
    <body>
      <h2>ESP32-CAM Stream</h2>
      <img src='/stream'><br>
      <h3>Last QR Payload:</h3>
      <div id='qr' style='font-size:1.5em;color:green;'>Waiting...</div>
      <script>
        async function updateQR() {
          try {
            let r = await fetch('/qr?nocache=' + Date.now());
            let t = await r.text();
            document.getElementById('qr').textContent = t || "No QR detected";
          } catch(e) {
            console.error(e);
          }
        }
        setInterval(updateQR, 2000);
        updateQR();
      </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

SemaphoreHandle_t data_mutex;
String data = "";

void handle_qr() {
  if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    Serial.print("[/qr] Served: ");
    Serial.println(data);
    server.send(200, "text/plain", data);
    xSemaphoreGive(data_mutex);
  } else {
    Serial.println("[/qr] Busy, could not take mutex");
    server.send(200, "text/plain", "Busy");
  }
}


// In startCameraServerPlain():
void startCameraServerPlain() {
  server.on("/", HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_jpg_stream);
  server.on("/qr", HTTP_GET, handle_qr); // <-- Add this line
  server.begin();
}

void onQrCodeTask(void *pvParameters)
{
  struct QRCodeData qrCodeData;
  while (true)
  {
    camera_fb_t *fb = reader.getLastFrameBuffer();
    if (!fb) {
      vTaskDelay(30 / portTICK_PERIOD_MS);
      continue;
    }

    // Convert to JPEG for streaming
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, 40, &jpg_buf, &jpg_len)) {
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

    // Get latest QR result
    if (reader.receiveQrCode(&qrCodeData, 100)) {
      if (qrCodeData.valid) {
        if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          data = String((const char *)qrCodeData.payload);
          xSemaphoreGive(data_mutex);
        }
        Serial.print("Payload: ");
        Serial.println(data);
      }
    }

    vTaskDelay(80 / portTICK_PERIOD_MS); // keep things responsive
  }
}


void setup()
{
  Serial.begin(115200);
  Serial.println();

  frame_mutex = xSemaphoreCreateMutex();
  data_mutex = xSemaphoreCreateMutex(); 

  reader.setup();
  Serial.println("Setup QRCode Reader");
  reader.beginOnCore(1);
  Serial.println("Begin on Core 1");

  WiFi.begin("Subhanallah5", "muhammadnabiyullah");
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

  xTaskCreate(onQrCodeTask, "onQrCode", 4 * 1024, NULL, 4, NULL);
}

void loop()
{
  server.handleClient();
}