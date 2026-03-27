#include <WiFi.h>
#include <map>
#include <vector>
#include <PubSubClient.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ==========================================
// 1. ĐỊNH NGHĨA CẤU TRÚC VÀ TÀI NGUYÊN
// ==========================================

// Cấu trúc dữ liệu để truyền qua Queue (Hàng đợi)
struct SensorMessage {
  String id;
  String roomId;
  float temp;
  float humid;
  float analog;
  String type;
};

struct SensorConfig {
  int gipo;
  String id;
  String roomId;
  String type;
  DHT* dhtInstance = NULL;
};

struct DeviceConfig {
  int gipo;
  String id;
  String roomId;
  bool status;
};

// Tài nguyên FreeRTOS
QueueHandle_t sensorQueue;         // Hàng đợi cho dữ liệu cảm biến
SemaphoreHandle_t deviceMutex;     // Mutex bảo vệ danh sách thiết bị
EventGroupHandle_t networkEvents;  // Nhóm sự kiện kết nối
const int WIFI_BIT = BIT0;
const int MQTT_BIT = BIT1;

// Danh sách lưu trữ (Shared Resources)
std::vector<DeviceConfig> deviceList;
std::vector<SensorConfig> sensorList;
std::map<String, float> lastValues;  // Để so sánh 10%

// Cấu hình mạng
const char* ssid = "Quan Quyen Luong";
const char* password = "1593572486";
const char* mqtt_server = "192.168.1.122";

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// 2. CÁC TASK CHỨC NĂNG
// ==========================================

// TASK 1: Quản lý kết nối (Chạy trên Core 0 - Ưu tiên cao)
void vTaskNetwork(void *pvParameters) {
  for (;;) {
    // 1. Kiểm tra WiFi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[NET] WiFi Lost. Reconnecting...");
      xEventGroupClearBits(networkEvents, WIFI_BIT);
      WiFi.begin(ssid, password);
      
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        retry++;
      }
    } else {
      xEventGroupSetBits(networkEvents, WIFI_BIT);
    }

    // 2. Kiểm tra MQTT (Chỉ khi WiFi đã OK)
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        Serial.println("[NET] MQTT Connecting...");
        xEventGroupClearBits(networkEvents, MQTT_BIT);
        
        // Thêm ID duy nhất để tránh bị Broker đá ra
        String clientId = "ESP32_HVKTMM_" + String(random(0, 9999));
        if (client.connect(clientId.c_str())) {
          Serial.println("[NET] MQTT Connected!");
          client.subscribe("devices/control");
          xEventGroupSetBits(networkEvents, MQTT_BIT);
        } else {
          Serial.printf("[NET] MQTT Failed, rc=%d. Retry in 5s\n", client.state());
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
      } else {
        client.loop(); // Phải có để giữ kết nối
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS); 
  }
}

// TASK 2: Thu thập dữ liệu cảm biến (Chạy trên Core 1)
void vTaskSensorCollector(void* pvParameters) {
  for (;;) {
    for (auto& s : sensorList) {
      SensorMessage msg;
      msg.id = s.id;
      msg.roomId = s.roomId;
      msg.type = s.type;

      if (s.type == "DHT11" && s.dhtInstance != NULL) {
        msg.temp = s.dhtInstance->readTemperature();
        msg.humid = s.dhtInstance->readHumidity();
        Serial.printf("[SENSOR] %s -> Temp: %.2f | Humid: %.2f\n",
                      msg.id.c_str(), msg.temp, msg.humid);

      } else {
        msg.analog = (float)analogRead(s.gipo);
        Serial.printf("[SENSOR] %s -> Analog: %.2f\n",
                      msg.id.c_str(), msg.analog);
      }

      // Đẩy vào Queue, nếu hàng đợi đầy thì bỏ qua (non-blocking)
      xQueueSend(sensorQueue, &msg, 0);
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS);  // Đọc mỗi 10 giây
  }
}

// TASK 3: Xử lý và gửi dữ liệu MQTT (Chạy trên Core 1)
void vTaskMQTTProcessor(void* pvParameters) {
  SensorMessage received;
  for (;;) {
    // Đợi cho đến khi có mạng và có dữ liệu trong Queue
    if (xQueueReceive(sensorQueue, &received, portMAX_DELAY) == pdPASS) {
      Serial.printf("[MQTT] Processing sensor: %s\n", received.id.c_str());
      // Kiểm tra xem có mạng không trước khi xử lý logic nặng
      EventBits_t bits = xEventGroupGetBits(networkEvents);
      if ((bits & (WIFI_BIT | MQTT_BIT)) == (WIFI_BIT | MQTT_BIT)) {

        Serial.printf("[MQTT] Processing: %s\n", received.id.c_str());
        // ... (Giữ nguyên logic so sánh 10% của bạn) ...

        float currentVal = (received.type == "DHT11") ? received.temp : received.analog;

        // Logic so sánh 10% (Chống spam Broker)
        bool changed = false;
        if (lastValues.find(received.id) == lastValues.end()) {
          changed = true;
        } else {
          float oldVal = lastValues[received.id];
          if (oldVal == 0 || (fabs(currentVal - oldVal) / fabs(oldVal)) >= 0.10) changed = true;
        }

        if (changed && !isnan(currentVal)) {
          lastValues[received.id] = currentVal;

          StaticJsonDocument<256> doc;
          doc["sensorId"] = received.id;
          doc["roomId"] = received.roomId;
          JsonObject cur = doc.createNestedObject("current");
          if (received.type == "DHT11") {
            cur["temperature"] = received.temp;
            cur["humidity"] = received.humid;
          } else {
            cur["analog"] = received.analog;
          }

          char buffer[256];
          serializeJson(doc, buffer);
          client.publish("sensors/temp", buffer);
          Serial.printf("[MQTT] Published %s\n", received.id.c_str());
        }
      } else {
        Serial.println("[MQTT] Skip publish - Network down");
      }
    }
  }
}

// TASK 4: Giám sát hệ thống (System Health)
void vTaskMonitor(void* pvParameters) {
  for (;;) {
    Serial.println("\n--- SYSTEM MONITOR ---");
    Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("Queue Messages: %u\n", uxQueueMessagesWaiting(sensorQueue));
    Serial.printf("Min Free Stack (MQTT): %u words\n", uxTaskGetStackHighWaterMark(NULL));
    Serial.println("----------------------");
    vTaskDelay(20000 / portTICK_PERIOD_MS);
  }
}

// ==========================================
// 3. CALLBACK & SETUP
// ==========================================

void callback(char* topic, byte* payload, unsigned int length) {
  // 1. Theo dõi dữ liệu thô nhận được từ Broker
  Serial.println("\n--- [MQTT CALLBACK] New Message ---");
  Serial.printf("Topic: %s\n", topic);
  
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("Payload: %s\n", message.c_str());

  // 2. Giải mã JSON
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.printf("[ERROR] JSON Deserialization failed: %s\n", error.c_str());
    return;
  }

  // Lấy dữ liệu từ JSON
  String targetId = doc["id"] | "Unknown";
  bool newStatus = doc["status"] | false;
  
  Serial.printf("[DEBUG] Parsed - ID: %s, Requested Status: %s\n", 
                targetId.c_str(), newStatus ? "ON" : "OFF");

  // 3. Quá trình xử lý thiết bị với Mutex
  if (xSemaphoreTake(deviceMutex, portMAX_DELAY) == pdTRUE) {
    bool deviceFound = false;
    
    for (auto& d : deviceList) {
      if (d.id == targetId) {
        d.status = newStatus;
        digitalWrite(d.gipo, d.status ? HIGH : LOW);
        
        Serial.printf("[SUCCESS] Switched Device %s (GPIO %d) to %s\n", 
                      d.id.c_str(), d.gipo, d.status ? "ON" : "OFF");
        deviceFound = true;
        break;
      }
    }

    if (!deviceFound) {
      Serial.printf("[WARN] Device ID '%s' not found in deviceList!\n", targetId.c_str());
    }

    xSemaphoreGive(deviceMutex);
  } else {
    Serial.println("[ERROR] Could not acquire Mutex to control device!");
  }
  
  Serial.println("-----------------------------------\n");
}

void setup() {
  Serial.begin(115200);

  // Khởi tạo tài nguyên OS
  sensorQueue = xQueueCreate(20, sizeof(SensorMessage));
  deviceMutex = xSemaphoreCreateMutex();
  networkEvents = xEventGroupCreate();

  // Kết nối WiFi lấy cấu hình (Chạy 1 lần trong setup)
  Serial.println("[SETUP] Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[SETUP] WiFi connected!");
  Serial.print("[SETUP] IP Address: ");
  Serial.println(WiFi.localIP());
  HTTPClient http;
  http.begin("http://192.168.1.122:5435/house/staff?houseId=home1");
  if (http.GET() == 200) {
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, http.getString());
    for (JsonObject item : doc.as<JsonArray>()) {
      String kind = item["kind"];
      if (kind == "SENSOR") {
        SensorConfig s;
        s.gipo = item["gipo"];
        s.id = item["id"].as<String>();
        s.type = item["type"].as<String>();
        s.roomId = item["roomId"].as<String>();
        if (s.type == "DHT11") {
          s.dhtInstance = new DHT(s.gipo, DHT11);
          s.dhtInstance->begin();
        } else {
          pinMode(s.gipo, INPUT);
        }
        Serial.printf("  -> ID: %s | GPIO: %d | Type: %s | Room: %s\n",
                      s.id.c_str(), s.gipo, s.type.c_str(), s.roomId.c_str());
        sensorList.push_back(s);
      } else if (kind == "DEVICE") {
        DeviceConfig d;
        d.gipo = item["gipo"];
        d.id = item["id"].as<String>();
        d.status = item["status"];
        d.roomId = item["roomId"].as<String>();
        Serial.printf("  -> ID: %s | GPIO: %d | Status: %s\n",
                      d.id.c_str(), d.gipo, d.status ? "ON" : "OFF");
        pinMode(d.gipo, OUTPUT);
        digitalWrite(d.gipo, d.status ? HIGH : LOW);
        deviceList.push_back(d);
      }
    }
  }
  http.end();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // Tạo Task và gán vào các nhân CPU
  // Core 0: Xử lý Network
  xTaskCreatePinnedToCore(vTaskNetwork, "NetManager", 4096, NULL, 3, NULL, 0);

  // Core 1: Xử lý Logic ứng dụng
  xTaskCreatePinnedToCore(vTaskSensorCollector, "SensColl", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(vTaskMQTTProcessor, "MQTTProc", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(vTaskMonitor, "SysMon", 2048, NULL, 0, NULL, 1);
}

void loop() {
  // Không làm gì cả, Task loop sẽ bị xóa để tiết kiệm tài nguyên
  vTaskDelete(NULL);
}