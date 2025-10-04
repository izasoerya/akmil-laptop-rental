#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP32QRCodeReader.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <WiFiClientSecure.h> // Required for insecure client

// Define the pins for Software Serial
EspSoftwareSerial::UART sserial;
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&sserial);
int authenticatedUserId = -1;

// --- ASYNC WEB SERVER SETUP ---
AsyncWebServer server(80);
AsyncEventSource events("/events");

ESP32QRCodeReader reader(CAMERA_MODEL_WROVER_KITS);

// --- State Machine Definition ---
enum SystemState
{
	LOCKED,
	UNLOCKED_SCANNING,
	SUCCESS,
	ENROLLING
};
volatile SystemState systemState = LOCKED;
SemaphoreHandle_t state_mutex;

// --- Task handles to control tasks ---
TaskHandle_t streamingTaskHandle = NULL;
TaskHandle_t qrCodeTaskHandle = NULL;
TaskHandle_t fingerprintTaskHandle = NULL;
TaskHandle_t enrollmentTaskHandle = NULL;

// Globals for sharing data between tasks
SemaphoreHandle_t frame_mutex;
uint8_t *last_jpeg = nullptr;
size_t last_jpeg_len = 0;
String data = "";
volatile int enrollId = 0;

// Function for getting fingerprint
int getFingerprintIDez()
{
	uint8_t p = finger.getImage();
	if (p != FINGERPRINT_OK)
		return -1;
	p = finger.image2Tz();
	if (p != FINGERPRINT_OK)
		return -2;
	p = finger.fingerSearch();
	if (p == FINGERPRINT_OK)
		return finger.fingerID;
	return -3;
}

// --- ASYNC WEB HANDLERS ---
void handle_root(AsyncWebServerRequest *request)
{
	String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>ESP32 Security System</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background-color: #f0f0f0; }
        .ui-container { display: none; padding: 20px; border-radius: 8px; background-color: white; box-shadow: 0 4px 8px rgba(0,0,0,0.1); max-width: 500px; margin: auto; }
        .payload { font-size: 1.5em; color: green; padding: 20px; border: 2px solid green; display: inline-block; margin-top: 20px; word-wrap: break-word; }
        img { border: 1px solid #ccc; max-width: 100%; height: auto; }
        h1 { color: #333; }
        button { padding: 10px 20px; font-size: 1em; cursor: pointer; margin-top: 20px; }
      </style>
    </head>
    <body>
      <div id="locked-ui" class="ui-container">
        <h1>SYSTEM LOCKED</h1>
        <p>Please scan a valid fingerprint to unlock.</p>
        <button onclick="window.location.href='/enroll'">Enroll New Fingerprint</button>
      </div>
      <div id="scanning-ui" class="ui-container">
        <h2>Scan QR Code</h2>
        <img id="stream-img" src=""><br>
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
        let streamInterval = null;

        function startStream() {
          if (!streamInterval) {
            streamInterval = setInterval(() => {
              document.getElementById('stream-img').src = '/jpg?' + new Date().getTime();
            }, 200);
          }
        }

        function stopStream() {
          if (streamInterval) {
            clearInterval(streamInterval);
            streamInterval = null;
            document.getElementById('stream-img').src = "";
          }
        }

        function updateUI(status) {
          if (status.state === currentState) {
             if (status.state === 'SCANNING') {
               document.getElementById('status-text').textContent = status.payload;
             }
            return;
          }

          currentState = status.state;
          document.getElementById('locked-ui').style.display = 'none';
          document.getElementById('scanning-ui').style.display = 'none';
          document.getElementById('success-ui').style.display = 'none';

          if (status.state === 'LOCKED') {
            document.getElementById('locked-ui').style.display = 'block';
            stopStream();
          } else if (status.state === 'SCANNING') {
            document.getElementById('scanning-ui').style.display = 'block';
            document.getElementById('status-text').textContent = status.payload;
            startStream();
          } else if (status.state === 'SUCCESS') {
            document.getElementById('success-ui').style.display = 'block';
            document.getElementById('success-payload').textContent = status.payload;
            stopStream();
          }
        }

        const eventSource = new EventSource('/events');
        eventSource.addEventListener('status', (event) => {
          const statusData = JSON.parse(event.data);
          if (window.location.pathname !== '/enroll') {
              updateUI(statusData);
          }
        });
        eventSource.onerror = (err) => { console.error('EventSource failed:', err); };
        
        updateUI({state: 'LOCKED', payload: ''});
      </script>
    </body>
    </html>
  )rawliteral";
	request->send(200, "text/html", html);
}

void handle_enroll_page(AsyncWebServerRequest *request)
{
	String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <title>Enroll Fingerprint</title>
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background-color: #f0f0f0; }
        #enroll-container { padding: 20px; border-radius: 8px; background-color: white; box-shadow: 0 4px 8px rgba(0,0,0,0.1); max-width: 500px; margin: auto; }
        #status-message { margin-top: 20px; font-size: 1.2em; font-weight: bold; }
        .status-wait { color: #333; } .status-go { color: #2a9d8f; } .status-error { color: #e76f51; } .status-success { color: #4CAF50; }
        input { padding: 10px; font-size: 1em; margin-right: 10px; }
        button { padding: 10px 20px; font-size: 1em; cursor: pointer; }
      </style>
    </head>
    <body>
      <div id="enroll-container">
        <h1>Enroll New Fingerprint</h1>
        <form id="enrollForm">
          <label for="fingerId">Fingerprint ID (1-127):</label>
          <input type="number" id="fingerId" name="id" min="1" max="127" required>
          <button type="submit">Start Enrollment</button>
        </form>
        <div id="status-message" class="status-wait">Please enter an ID and click Start.</div>
      </div>

      <script>
        const form = document.getElementById('enrollForm');
        const statusMessage = document.getElementById('status-message');

        form.addEventListener('submit', async (e) => {
          e.preventDefault();
          const id = document.getElementById('fingerId').value;
          form.style.display = 'none';
          statusMessage.className = 'status-wait';
          statusMessage.textContent = 'Starting enrollment process for ID ' + id + '...';

          try {
            const response = await fetch('/start_enroll', {
              method: 'POST',
              headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
              body: 'id=' + id
            });
            const result = await response.text();
            if (!response.ok) { throw new Error(result); }
            statusMessage.textContent = result;
          } catch (error) {
            statusMessage.className = 'status-error';
            statusMessage.textContent = 'Error: ' + error.message;
            form.style.display = 'block';
          }
        });

        const eventSource = new EventSource('/events');
        eventSource.addEventListener('enroll_status', (event) => {
          const data = JSON.parse(event.data);
          statusMessage.className = 'status-' + data.status;
          statusMessage.textContent = data.message;
          
          if (data.status === 'success') {
            setTimeout(() => { window.location.href = '/'; }, 3000);
          } else if (data.status === 'error') {
            setTimeout(() => { window.location.href = '/enroll'; }, 3000);
          }
        });
      </script>
    </body>
    </html>
  )rawliteral";
	request->send(200, "text/html", html);
}

void onEnrollmentTask(void *pvParameters);
void handle_start_enroll(AsyncWebServerRequest *request)
{
	if (systemState != LOCKED)
	{
		request->send(409, "text/plain", "System is busy. Please wait and try again.");
		return;
	}
	if (request->hasParam("id", true))
	{
		String idStr = request->getParam("id", true)->value();
		enrollId = idStr.toInt();
		if (enrollId > 0 && enrollId < 128)
		{
			xSemaphoreTake(state_mutex, portMAX_DELAY);
			systemState = ENROLLING;
			xSemaphoreGive(state_mutex);

			vTaskSuspend(fingerprintTaskHandle);
			xTaskCreatePinnedToCore(onEnrollmentTask, "Enrollment", 4 * 1024, NULL, 2, &enrollmentTaskHandle, 1);

			request->send(200, "text/plain", "Enrollment process started. Please check the status message.");
		}
		else
		{
			request->send(400, "text/plain", "Invalid ID. Please provide a number between 1 and 127.");
		}
	}
	else
	{
		request->send(400, "text/plain", "Missing ID parameter.");
	}
}

void handle_jpg(AsyncWebServerRequest *request)
{
	xSemaphoreTake(state_mutex, portMAX_DELAY);
	bool isScanning = (systemState == UNLOCKED_SCANNING);
	xSemaphoreGive(state_mutex);

	if (!isScanning)
	{
		request->send(403, "text/plain", "Access Denied: Stream not active.");
		return;
	}

	uint8_t *jpeg_copy = nullptr;
	size_t jpeg_len = 0;

	if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
	{
		if (last_jpeg && last_jpeg_len > 0)
		{
			jpeg_copy = (uint8_t *)malloc(last_jpeg_len);
			if (jpeg_copy)
			{
				memcpy(jpeg_copy, last_jpeg, last_jpeg_len);
				jpeg_len = last_jpeg_len;
			}
		}
		xSemaphoreGive(frame_mutex);
	}

	if (jpeg_copy && jpeg_len > 0)
	{
		AsyncWebServerResponse *response = request->beginResponse("image/jpeg", jpeg_len, [jpeg_copy, jpeg_len](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
																  {
            size_t len = jpeg_len - index;
            if (len > maxLen) len = maxLen;
            memcpy(buffer, jpeg_copy + index, len);
            return len; });
		request->onDisconnect([jpeg_copy]()
							  { free(jpeg_copy); });
		request->send(response);
	}
	else
	{
		request->send(503, "text/plain", "Service Unavailable: Frame not available or memory error.");
	}
}

void startCameraServers()
{
	server.on("/", HTTP_GET, handle_root);
	server.on("/jpg", HTTP_GET, handle_jpg);
	server.on("/enroll", HTTP_GET, handle_enroll_page);
	server.on("/start_enroll", HTTP_POST, handle_start_enroll);

	events.onConnect([](AsyncEventSourceClient *client)
					 {
        if(client->lastId()){
          Serial.printf("Client reconnected! Last message ID: %u\n", client->lastId());
        }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        SystemState currentState = systemState;
        String currentPayload = data;
        xSemaphoreGive(state_mutex);
        
        String stateStr = "LOCKED";
        if (currentState == UNLOCKED_SCANNING) stateStr = "SCANNING";
        else if (currentState == SUCCESS) stateStr = "SUCCESS";
        else if (currentState == ENROLLING) stateStr = "ENROLLING";

        String json_status = "{\"state\":\"" + stateStr + "\", \"payload\":\"" + currentPayload + "\"}";
        client->send(json_status.c_str(), "status", millis());

        if (currentState == ENROLLING) {
            client->send("{\"status\":\"wait\", \"message\":\"Enrollment in progress...\"}", "enroll_status", millis());
        } });

	server.addHandler(&events);
	server.begin();
}

void onEnrollmentTask(void *pvParameters)
{
	int id_to_enroll = enrollId;
	Serial.printf("Starting enrollment for ID #%d\n", id_to_enroll);

	events.send("{\"status\":\"go\", \"message\":\"Place a finger on the sensor...\"}", "enroll_status", millis());
	while (finger.getImage() != FINGERPRINT_OK)
	{
		vTaskDelay(50 / portTICK_PERIOD_MS);
	}
	uint8_t p = finger.image2Tz(1);
	if (p != FINGERPRINT_OK)
	{
		events.send("{\"status\":\"error\", \"message\":\"Error imaging finger. Please try again.\"}", "enroll_status", millis());
		goto cleanup_enroll;
	}
	Serial.println("Image 1 taken and converted.");
	events.send("{\"status\":\"wait\", \"message\":\"Remove finger...\"}", "enroll_status", millis());
	vTaskDelay(1000);
	while (finger.getImage() != FINGERPRINT_NOFINGER)
	{
		vTaskDelay(50 / portTICK_PERIOD_MS);
	}

	events.send("{\"status\":\"go\", \"message\":\"Place the same finger again...\"}", "enroll_status", millis());
	while (finger.getImage() != FINGERPRINT_OK)
	{
		vTaskDelay(50 / portTICK_PERIOD_MS);
	}
	p = finger.image2Tz(2);
	if (p != FINGERPRINT_OK)
	{
		events.send("{\"status\":\"error\", \"message\":\"Error imaging finger. Please try again.\"}", "enroll_status", millis());
		goto cleanup_enroll;
	}
	Serial.println("Image 2 taken and converted.");

	p = finger.createModel();
	if (p != FINGERPRINT_OK)
	{
		events.send("{\"status\":\"error\", \"message\":\"Fingers do not match. Please try again.\"}", "enroll_status", millis());
		goto cleanup_enroll;
	}
	Serial.println("Model created.");

	p = finger.storeModel(id_to_enroll);
	if (p != FINGERPRINT_OK)
	{
		events.send("{\"status\":\"error\", \"message\":\"Error storing fingerprint. Please try again.\"}", "enroll_status", millis());
		goto cleanup_enroll;
	}
	Serial.println("Fingerprint stored!");
	events.send("{\"status\":\"success\", \"message\":\"Enrollment successful! Redirecting...\"}", "enroll_status", millis());
	vTaskDelay(2000);

cleanup_enroll:
	xSemaphoreTake(state_mutex, portMAX_DELAY);
	systemState = LOCKED;
	xSemaphoreGive(state_mutex);

	vTaskResume(fingerprintTaskHandle);
	Serial.println("Enrollment task finished.");
	vTaskDelete(NULL);
}

void onFingerprintTask(void *pvParameters)
{
	while (true)
	{
		int finger_id = getFingerprintIDez();
		if (Serial.available() > 0)
		{
			if (Serial.readString() == "RESTART")
			{
				Serial.println("Restart ESP");
				ESP.restart();
			}
		}
		if (finger_id > 0)
		{
			Serial.printf("Fingerprint match found. User ID: %d\n", finger_id);
			authenticatedUserId = finger_id;

			xSemaphoreTake(state_mutex, portMAX_DELAY);
			systemState = UNLOCKED_SCANNING;
			data = "Fingerprint OK. Scan QR Code...";
			xSemaphoreGive(state_mutex);

			String json = "{\"state\":\"SCANNING\", \"payload\":\"" + data + "\"}";
			events.send(json.c_str(), "status", millis());

			Serial.println("Resuming Streaming and QR Code tasks...");
			vTaskResume(streamingTaskHandle);
			vTaskResume(qrCodeTaskHandle);

			vTaskSuspend(NULL);
		}
		vTaskDelay(100 / portTICK_PERIOD_MS);
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
		if (frame2jpg(fb, 40, &jpg_buf, &jpg_len))
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

void updateLaptopUser(int laptop_id, int user_id)
{
	// Add this line to see the available memory right before the HTTPS call.
	Serial.printf("!!! Before HTTPS call, Free Heap: %d bytes\n", esp_get_free_heap_size());

	if (WiFi.status() != WL_CONNECTED)
	{
		Serial.println("WiFi not connected, skipping Supabase update.");
		return;
	}

	// --- MODIFICATION START ---
	// Create a WiFiClientSecure object
	WiFiClientSecure client;

	// Disable certificate validation (INSECURE!)
	client.setInsecure();
	// --- MODIFICATION END ---

	HTTPClient http;

	String url = "https://jlumxgihbhviwvdhklrw.supabase.co/rest/v1/laptop_acc?id=eq." + String(laptop_id);

	// Pass the insecure client to http.begin()
	http.begin(client, url);

	http.addHeader("apikey", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImpsdW14Z2loYmh2aXd2ZGhrbHJ3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTc4NjY2NjQsImV4cCI6MjA3MzQ0MjY2NH0.BTK5cIsLr2NSL1HUVJXCTrkOGcPseh26DbzLgvc7OiY");
	http.addHeader("Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImpsdW14Z2loYmh2aXd2ZGhrbHJ3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTc4NjY2NjQsImV4cCI6MjA3MzQ0MjY2NH0.BTK5cIsLr2NSL1HUVJXCTrkOGcPseh26DbzLgvc7OiY");
	http.addHeader("Content-Type", "application/json");
	http.addHeader("Prefer", "return=representation");

	int httpResponseCode = http.GET();
	if (httpResponseCode == 200)
	{
		String response = http.getString();
		int userIdPos = response.indexOf("\"user_id\":");
		bool isNull = false;
		if (userIdPos != -1)
		{
			int valueStart = userIdPos + 10;
			while (valueStart < response.length() && (response[valueStart] == ' ' || response[valueStart] == '\n'))
				valueStart++;
			if (response.substring(valueStart, valueStart + 4) == "null")
				isNull = true;
		}
		http.end();

		// Pass the same insecure client to the second http.begin() call
		http.begin(client, url);

		http.addHeader("apikey", "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImpsdW14Z2loYmh2aXd2ZGhrbHJ3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTc4NjY2NjQsImV4cCI6MjA3MzQ0MjY2NH0.BTK5cIsLr2NSL1HUVJXCTrkOGcPseh26DbzLgvc7OiY");
		http.addHeader("Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImpsdW14Z2loYmh2aXd2ZGhrbHJ3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTc4NjY2NjQsImV4cCI6MjA3MzQ0MjY2NH0.BTK5cIsLr2NSL1HUVJXCTrkOGcPseh26DbzLgvc7OiY");
		http.addHeader("Content-Type", "application/json");
		http.addHeader("Prefer", "return=representation");
		char payload[64];
		if (isNull)
			snprintf(payload, sizeof(payload), "{\"user_id\": %d}", user_id);
		else
			snprintf(payload, sizeof(payload), "{\"user_id\": null}");
		Serial.printf("Sending PATCH to URL: %s\n", url.c_str());
		Serial.printf("With payload: %s\n", payload);
		int patchCode = http.PATCH(payload);
		if (patchCode >= 200 && patchCode < 300)
		{
			Serial.println("Supabase update successful.");
			Serial.println("Response: " + http.getString());
		}
		else
		{
			Serial.printf("Error on sending PATCH request. HTTP Code: %d\n", patchCode);
			Serial.println("Response: " + http.getString());
		}
		http.end();
	}
	else
	{
		Serial.printf("Error on GET request. HTTP Code: %d\n", httpResponseCode);
		Serial.println("Response: " + http.getString());
		http.end();
	}
}

void processQrPayload(const String &qrPayload, int userId)
{
	Serial.printf("Processing QR Payload: %s for User ID: %d\n", qrPayload.c_str(), userId);
	int semiPos = qrPayload.indexOf(';');
	if (semiPos == -1)
	{
		Serial.println("Invalid QR format: missing semicolon ';'");
		return;
	}
	String payload = qrPayload.substring(semiPos + 1);
	int dashPos = payload.indexOf('-');
	if (dashPos == -1)
	{
		Serial.println("Invalid QR format: missing hyphen '-' in payload");
		return;
	}
	String laptopIdStr = payload.substring(0, dashPos);
	String laptopName = payload.substring(dashPos + 1);
	int laptopId = laptopIdStr.toInt();
	if (laptopId == 0)
	{
		Serial.println("Failed to parse a valid laptop ID.");
		return;
	}
	Serial.printf("Parsed Record -> Laptop ID: %d, Name: %s\n", laptopId, laptopName.c_str());
	if (userId > 0)
	{
		updateLaptopUser(laptopId, userId);
	}
	else
	{
		Serial.println("Invalid User ID, skipping Supabase update.");
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
				String qrPayload = String((const char *)qrCodeData.payload);
				xSemaphoreTake(state_mutex, portMAX_DELAY);
				data = qrPayload;
				systemState = SUCCESS;
				xSemaphoreGive(state_mutex);
				Serial.print("QR Payload Received: ");
				Serial.println(data);
				int semiPos = qrPayload.indexOf(';');
				String displayData = (semiPos != -1) ? qrPayload.substring(semiPos + 1) : qrPayload;
				String json = "{\"state\":\"SUCCESS\", \"payload\":\"" + displayData + "\"}";
				events.send(json.c_str(), "status", millis());
				vTaskSuspend(streamingTaskHandle);
				processQrPayload(data, authenticatedUserId);
				Serial.println("SUCCESS! Displaying payload for 30 seconds, then resetting.");
				vTaskDelay(30000 / portTICK_PERIOD_MS);
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
	sserial.begin(57600, SWSERIAL_8N1, 33, 32);

	delay(100);
	finger.begin(57600);
	if (!finger.verifyPassword())
	{
		Serial.println("Did not find fingerprint sensor :(");
		while (1)
		{
			delay(1);
		}
	}
	Serial.println("Found fingerprint sensor!");
	frame_mutex = xSemaphoreCreateMutex();
	state_mutex = xSemaphoreCreateMutex();
	reader.setup();
	Serial.println("Setup QRCode Reader");
	reader.beginOnCore(1);
	Serial.println("Begin on Core 1");
	WiFi.begin("QRent", "QRENTQRENT");
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		Serial.print(".");
	}
	Serial.println("\nWiFi connected");
	startCameraServers();
	Serial.print("Web Server Ready! Use 'http://");
	Serial.print(WiFi.localIP());
	Serial.println("' to connect");
	Serial.println("System is LOCKED. Waiting for fingerprint...");
	xTaskCreatePinnedToCore(onFingerprintTask, "Fingerprint", 4 * 1024, NULL, 1, &fingerprintTaskHandle, 1);
	xTaskCreatePinnedToCore(onQrCodeTask, "QRCode", 8 * 1024, NULL, 1, &qrCodeTaskHandle, 1);
	xTaskCreate(onStreamingTask, "Streaming", 8 * 1024, NULL, 1, &streamingTaskHandle);
	vTaskSuspend(streamingTaskHandle);
	vTaskSuspend(qrCodeTaskHandle);
}

void loop()
{
	vTaskDelay(1000 / portTICK_PERIOD_MS);
}