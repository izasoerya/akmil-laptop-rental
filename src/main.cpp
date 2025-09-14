#include <Arduino.h>
#include <ESP32QRCodeReader.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>

// Define the pins for Software Serial
EspSoftwareSerial::UART sserial;
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&sserial);

WebServer server(80);
ESP32QRCodeReader reader(CAMERA_MODEL_WROVER_KITS);

// --- State Machine Definition ---
enum SystemState { LOCKED, UNLOCKED_SCANNING, SUCCESS };
volatile SystemState systemState = LOCKED;
SemaphoreHandle_t state_mutex;
// ------------------------------

// --- Task handles to control tasks ---
TaskHandle_t streamingTaskHandle = NULL;
TaskHandle_t qrCodeTaskHandle = NULL;
TaskHandle_t fingerprintTaskHandle = NULL;
// ----------------------------------------

// Globals for sharing data between tasks
SemaphoreHandle_t frame_mutex;
uint8_t* last_jpeg = nullptr;
size_t last_jpeg_len = 0;
String data = "";

// Fingerprint reading function
int getFingerprintIDez() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK)  return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK)  return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK)  return -2;
  return finger.fingerID; 
}

// --- NEW/MODIFIED Web Server Handlers ---

// NEW: This handler provides the complete system status as JSON
void handle_status() {
  xSemaphoreTake(state_mutex, portMAX_DELAY);
  SystemState currentState = systemState;
  String currentPayload = data;
  xSemaphoreGive(state_mutex);

  String stateStr = "LOCKED";
  if (currentState == UNLOCKED_SCANNING) {
    stateStr = "SCANNING";
  } else if (currentState == SUCCESS) {
    stateStr = "SUCCESS";
  }
  
  String json = "{\"state\": \"" + stateStr + "\", \"payload\": \"" + currentPayload + "\"}";
  server.send(200, "application/json", json);
}

// MODIFIED: This now serves a single page with all UI elements, controlled by JavaScript
void handle_root() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ESP32 Security System</title>
      <style>
        body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; }
        .ui-container { display: none; } /* Hide all UI containers by default */
        .payload { font-size: 2em; color: green; padding: 20px; border: 2px solid green; display: inline-block; margin-top: 20px; }
        img { border: 1px solid #ccc; }
      </style>
    </head>
    <body>
      <div id="locked-ui" class="ui-container">
        <h1>SYSTEM LOCKED</h1>
        <p>Please scan a valid fingerprint to unlock.</p>
      </div>

      <div id="scanning-ui" class="ui-container">
        <h2>Scan QR Code</h2>
        <img src='/stream'><br>
        <h3 id="status-text">Fingerprint OK. Scan QR Code...</h3>
      </div>

      <div id="success-ui" class="ui-container">
        <h1>SUCCESS!</h1>
        <h2>QR Code Payload:</h2>
        <div id="success-payload" class="payload"></div>
        <p style='margin-top: 30px;'>This device will automatically reset.</p>
      </div>

      <script>
        let currentState = '';

        function updateUI(status) {
          if (status.state === currentState) {
            // If state is the same, no need to change visibility, just update text if needed
            if (status.state === 'SCANNING') {
              document.getElementById('status-text').textContent = status.payload;
            }
            return;
          }

          // State has changed, hide all containers and show the correct one
          currentState = status.state;
          document.getElementById('locked-ui').style.display = 'none';
          document.getElementById('scanning-ui').style.display = 'none';
          document.getElementById('success-ui').style.display = 'none';

          if (status.state === 'LOCKED') {
            document.getElementById('locked-ui').style.display = 'block';
          } else if (status.state === 'SCANNING') {
            document.getElementById('scanning-ui').style.display = 'block';
            document.getElementById('status-text').textContent = status.payload;
          } else if (status.state === 'SUCCESS') {
            document.getElementById('success-ui').style.display = 'block';
            document.getElementById('success-payload').textContent = status.payload;
          }
        }

        function checkStatus() {
          fetch('/status')
            .then(response => response.json())
            .then(data => {
              updateUI(data);
            })
            .catch(error => {
              console.error('Failed to fetch status:', error);
              // Optionally show a disconnected message
            });
        }
        
        // Start polling for status updates
        setInterval(checkStatus, 1500);
        // Initial check to set the UI correctly on page load
        checkStatus();
      </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handle_jpg_stream() {
  if (systemState != UNLOCKED_SCANNING) {
    server.send(403, "text/plain", "Access Denied: Stream not active.");
    return;
  }
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);
  while (client.connected() && systemState == UNLOCKED_SCANNING) {
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (last_jpeg && last_jpeg_len > 0) {
        server.sendContent("--frame\r\nContent-Type: image/jpeg\r\n\r\n");
        client.write(last_jpeg, last_jpeg_len);
        server.sendContent("\r\n");
      }
      xSemaphoreGive(frame_mutex);
    }
    delay(30);
  }
}

void startCameraServers() {
  server.on("/", HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_jpg_stream);
  server.on("/status", HTTP_GET, handle_status); // NEW endpoint
  server.begin();
}


// --- FreeRTOS Tasks (Logic is unchanged) ---

void onFingerprintTask(void *pvParameters) {
  while (true) {
    int finger_id = getFingerprintIDez();
    if (finger_id > 0) {
      Serial.print("Found ID #"); Serial.println(finger_id);
      
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

void onStreamingTask(void *pvParameters) {
  while(true) {
    camera_fb_t *fb = reader.getLastFrameBuffer(); 
    if (!fb) { vTaskDelay(30 / portTICK_PERIOD_MS); continue; }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (frame2jpg(fb, 12, &jpg_buf, &jpg_len)) {
      if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (last_jpeg) free(last_jpeg);
        last_jpeg = (uint8_t*) malloc(jpg_len);
        if (last_jpeg) {
          memcpy(last_jpeg, jpg_buf, jpg_len);
          last_jpeg_len = jpg_len;
        } else { last_jpeg_len = 0; }
        xSemaphoreGive(frame_mutex);
      }
      free(jpg_buf);
    }
    vTaskDelay(30 / portTICK_PERIOD_MS);
  }
}

void onQrCodeTask(void *pvParameters) {
  struct QRCodeData qrCodeData;
  while (true) {
    if (reader.receiveQrCode(&qrCodeData, 100)) {
      if (qrCodeData.valid) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        data = String((const char *)qrCodeData.payload);
        systemState = SUCCESS;
        Serial.print("QR Payload: "); Serial.println(data);
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

void setup() {
  Serial.begin(9600);
  Serial.println("\n\nSystem Booting...");

  sserial.begin(57600, SWSERIAL_8N1, 32, 33);
  delay(100);
  if (finger.verifyPassword()) {
    Serial.println("Found fingerprint sensor!");
  } else {
    Serial.println("Did not find fingerprint sensor :(");
    while (1) { delay(1); }
  }

  frame_mutex = xSemaphoreCreateMutex();
  state_mutex = xSemaphoreCreateMutex();

  reader.setup();
  Serial.println("Setup QRCode Reader");
  reader.beginOnCore(1);
  Serial.println("Begin on Core 1");

  WiFi.begin("Subhanallah", "muhammadnabiyullah");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  startCameraServers();
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

void loop() {
  server.handleClient();
}