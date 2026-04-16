#include <WiFi.h>
#include <map>
#include <vector>
#include <PubSubClient.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>  // Cài đặt thư viện ESP32Servo
#include <Preferences.h> // Thư viện để lưu vào ROM
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
Preferences prefs;
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
  String type;                  // Thêm trường để phân biệt LIGHT, SERVO, v.v.
  Servo* servoInstance = NULL;  // Thêm con trỏ cho Servo
  String roomId;
  bool status;
};
struct AutomationConfig {
  String id;
  String sensorId;
  String deviceId;
  String operation; // ">", "<", "=="
  float threshold;
  int cooldownMinutes;
  bool isExecuting = false;
  bool isEnabled;
  int value; // Giá trị điều khiển (ví dụ mức quạt 1-4)
  unsigned long lastActivated = 0; // Để tính toán cooldown
};

// Tài nguyên FreeRTOS
QueueHandle_t sensorQueue;         // Hàng đợi cho dữ liệu cảm biến
SemaphoreHandle_t deviceMutex;     // Mutex bảo vệ danh sách thiết bị
QueueHandle_t autoQueue;
EventGroupHandle_t networkEvents;  // Nhóm sự kiện kết nối
const int WIFI_BIT = BIT0;
const int MQTT_BIT = BIT1;

// Danh sách lưu trữ (Shared Resources)
std::vector<DeviceConfig> deviceList;
std::vector<SensorConfig> sensorList;
std::map<String, float> lastValues;  // Để so sánh 10%
std::vector<AutomationConfig> automationList;

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
void vTaskNetwork(void* pvParameters) {
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
          client.subscribe("automation_update");

          client.subscribe("devices/control");
          xEventGroupSetBits(networkEvents, MQTT_BIT);
        } else {
          Serial.printf("[NET] MQTT Failed, rc=%d. Retry in 5s\n", client.state());
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
      } else {
        client.loop();  // Phải có để giữ kết nối
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
void vTaskAutomationProcessor(void* pvParameters) {
  SensorMessage msg;
  Serial.println("[TASK] Automation Processor đã sẵn sàng.");

  for (;;) {
    // 1. Chờ dữ liệu từ Queue dành riêng cho Automation
    // portMAX_DELAY giúp Task ngủ yên khi không có dữ liệu, tiết kiệm CPU
    if (xQueueReceive(autoQueue, &msg, portMAX_DELAY) == pdPASS) {
      
      float currentVal = (msg.type == "DHT11") ? msg.temp : msg.analog;

      // Log để biết Task nhận được dữ liệu từ cảm biến nào
      Serial.printf("\n[AUTO-TASK] Nhận dữ liệu từ: %s | Giá trị: %.2f\n", 
                    msg.id.c_str(), currentVal);

      // 2. Kiểm tra tính hợp lệ của dữ liệu (tránh NaN gây crash logic)
      if (isnan(currentVal)) {
        Serial.printf("[AUTO-TASK] Bỏ qua kịch bản cho %s vì dữ liệu lỗi (NaN).\n", msg.id.c_str());
        continue;
      }

      // 3. Thực thi kịch bản
      // Hàm này sẽ duyệt qua automationList và so sánh với currentVal
      unsigned long startTime = millis();
      
      checkAndExecuteAutomations(msg.id, currentVal);
      
      unsigned long duration = millis() - startTime;

      // Log thời gian xử lý để kiểm tra hiệu năng (thường chỉ mất vài ms)
      Serial.printf("[AUTO-TASK] Hoàn tất kiểm tra logic cho %s (Xử lý trong %d ms).\n", 
                    msg.id.c_str(), (int)duration);
    }
  }
}
// TASK 3: Xử lý và gửi dữ liệu MQTT (Chạy trên Core 1)
void vTaskMQTTProcessor(void* pvParameters) {
  SensorMessage received;
  for (;;) {
    // Đợi cho đến khi có mạng và có dữ liệu trong Queue
    if (xQueueReceive(sensorQueue, &received, portMAX_DELAY) == pdPASS) {

      xQueueSend(autoQueue, &received, 0);
      Serial.printf("[MQTT] Processing sensor: %s\n", received.id.c_str());
      // Kiểm tra xem có mạng không trước khi xử lý logic nặng
      EventBits_t bits = xEventGroupGetBits(networkEvents);
      if ((bits & (WIFI_BIT | MQTT_BIT)) == (WIFI_BIT | MQTT_BIT)) {

        Serial.printf("[MQTT] Processing: %s\n", received.id.c_str());
        // ... (Giữ nguyên logic so sánh 10% của bạn) ...

        float currentVal = (received.type == "DHT11") ? received.temp : received.analog;
                  checkAndExecuteAutomations(received.id, currentVal);

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
          String cleanId = received.id;
          cleanId.trim();
          doc["sensorId"] = received.id.c_str();
          doc["roomId"] = received.roomId.c_str();
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
  String strTopic = String(topic);
  
  // A. Xử lý kịch bản (Automation)
  if (strTopic == "automation_update") {
      Serial.println("[MQTT] Hệ thống yêu cầu reload Automations...");
      getAndSaveAutomations();
      return;
  }
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
        if (d.type == "DOOR" && d.servoInstance != NULL) {
          // Điều khiển góc quay tùy theo status
          int angle = d.status ? 170 : 100;
          d.servoInstance->write(angle);
          Serial.printf("[DOOR] ID %s rotated to %d deg\n", d.id.c_str(), angle);
        }else if(d.type == "PUMP"){
              digitalWrite(d.gipo, d.status ? LOW : HIGH);
            }
        else {
          // Thiết bị ON/OFF thông thường
          digitalWrite(d.gipo, d.status ? HIGH : LOW);
        }

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
// 3. Hàm Get Automations cập nhật logic check thất bại
void getAndSaveAutomations() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AUTO] WiFi Offline -> Loading from ROM");
    loadAutomationsFromROM();
    return;
  }

  HTTPClient http;
  http.begin("http://192.168.1.122:5435/house/automation?houseId=home1");
  http.setTimeout(3000); // 3 giây không phản hồi thì coi như fail

  int httpCode = http.GET();
  if (httpCode == 200) {
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, http.getString());
    
    automationList.clear(); 
    Serial.println("\n--- [SERVER] Fetching New Automations ---");

    for (JsonObject item : doc.as<JsonArray>()) {
      AutomationConfig a;
      a.id = item["id"].as<String>();
      a.sensorId = item["condition"]["sensorId"].as<String>();
      a.deviceId = item["action"]["deviceId"].as<String>();
      a.operation = item["condition"]["operation"].as<String>();
      a.threshold = item["condition"]["threshold"].as<float>();
      a.cooldownMinutes = item["control"]["cooldownMinutes"] | 0;
      a.isEnabled = item["isEnabled"] | true;
      a.value = item["action"]["value"] | 1;
      
      automationList.push_back(a);
      Serial.printf("-> Loaded: %s\n", a.id.substring(0,10).c_str());
    }
    // Thành công thì ghi đè vào Flash
    saveAutomationsToROM();
  } 
  else {
    // KHI CALL THẤT BẠI (Server local sập)
    Serial.printf("[AUTO] Call failed (Code: %d) -> Switching to ROM\n", httpCode);
    loadAutomationsFromROM();
  }
  http.end();
}
void checkAndExecuteAutomations(String sId, float currentVal) {
  for (auto& autoItem : automationList) {
    if (autoItem.isEnabled && autoItem.sensorId == sId) {
        Serial.printf("[AUTO] Chẹck");
      // Kiểm tra Cooldown (tránh bật tắt lên tục)
      /*if (millis() - autoItem.lastActivated < (autoItem.cooldownMinutes * 60000)) {
        continue; 
      }*/

      bool triggered = false;
      if (autoItem.operation == ">" && currentVal > autoItem.threshold){
          triggered = true;
          Serial.printf("[AUTO] Triggered:> %s (Val: %.2f %s %.2f)\n", 
                      sId.c_str(), currentVal, autoItem.operation.c_str(), autoItem.threshold);
      } 
      else if (autoItem.operation == "<" && currentVal < autoItem.threshold) {triggered = true;
              Serial.printf("[AUTO] Triggered: < %s (Val: %.2f %s %.2f)\n", 
                      sId.c_str(), currentVal, autoItem.operation.c_str(), autoItem.threshold);
      }
      else if (autoItem.operation == "==" && currentVal == autoItem.threshold) triggered = true;

      if (triggered) {
        autoItem.lastActivated = millis();
        Serial.printf("[AUTO] Triggered: %s (Val: %.2f %s %.2f)\n", 
                      sId.c_str(), currentVal, autoItem.operation.c_str(), autoItem.threshold);
        
        // Tìm thiết bị trong deviceList để điều khiển
        if (xSemaphoreTake(deviceMutex, portMAX_DELAY) == pdTRUE) {
          for (auto& d : deviceList) {

            if (d.id == autoItem.deviceId) {
              autoItem.isExecuting = true;
              d.status = true; // Ví dụ: Auto thì luôn là Bật
              if (d.type == "DOOR") d.servoInstance->write(170);
              else if(d.type == "PUMP"){
              digitalWrite(d.gipo, d.status ? LOW : HIGH);
            }
              else digitalWrite(d.gipo, HIGH);
              
              Serial.printf("[AUTO] Device %s is now ON\n", d.id.c_str());
              break;
            }
          }
          xSemaphoreGive(deviceMutex);
        }
      }else{
        // Nếu điều kiện KHÔNG thỏa mãn, nhưng trước đó ĐANG thực thi -> Tắt đi
        if (autoItem.isExecuting) {
          if (xSemaphoreTake(deviceMutex, portMAX_DELAY) == pdTRUE) {
            for (auto& d : deviceList) {
              if (d.id == autoItem.deviceId) {
                autoItem.isExecuting = false; // Đánh dấu đã dừng thực thi
                d.status = false;

                if (d.type == "DOOR" && d.servoInstance != NULL) d.servoInstance->write(100);
                else if(d.type == "PUMP"){
              digitalWrite(d.gipo, d.status ? LOW : HIGH);
            }
                else digitalWrite(d.gipo, LOW);

                Serial.printf("[AUTO] Condition NOT met - Device %s is now OFF\n", d.id.c_str());
                break;
              }
            }
            xSemaphoreGive(deviceMutex);
          }
        }
      }
    }
  }
}
// Hàm lưu danh sách automation hiện tại trong RAM vào ROM
void saveAutomationsToROM() {
  prefs.begin("auto_data", false);
  prefs.clear(); // Xóa sạch dữ liệu cũ để tránh rác
  
  String idList = "";
  for (auto& a : automationList) {
    String keyBase = a.id.substring(0, 10);
    idList += keyBase + ",";

    prefs.putString((keyBase + "_s").c_str(), a.sensorId);
    prefs.putString((keyBase + "_d").c_str(), a.deviceId);
    prefs.putString((keyBase + "_o").c_str(), a.operation);
    prefs.putFloat((keyBase + "_t").c_str(), a.threshold);
    prefs.putInt((keyBase + "_c").c_str(), a.cooldownMinutes);
    prefs.putBool((keyBase + "_e").c_str(), a.isEnabled);
    prefs.putInt((keyBase + "_v").c_str(), a.value);
  }
  prefs.putString("id_list", idList);
  prefs.end();
  Serial.println("[ROM] Data backed up to Flash.");
}

// 2. Hàm đọc Automation từ Flash (Dùng khi mất mạng/Server local sập)
void loadAutomationsFromROM() {
  Serial.println("\n[ROM] Đang bắt đầu khôi phục Automations...");

  if (!prefs.begin("auto_data", true)) {
    Serial.println("[ROM] LỖI: Không thể mở namespace auto_data!");
    return;
  }

  String idList = prefs.getString("id_list", "");
  
  if (idList == "" || idList == "null") {
    Serial.println("[ROM] Thông báo: Danh sách Automation trong Flash trống.");
  } else {
    automationList.clear();
    Serial.printf("[ROM] Tìm thấy chuỗi ID: %s\n", idList.c_str());
    Serial.println("------------------------------------");

    int start = 0;
    int end = idList.indexOf(',');

    while (end != -1) {
      String fullId = idList.substring(start, end);
      if (fullId.length() > 0) {
        // Dùng 8 ký tự đầu làm KeyBase để đảm bảo tổng Key < 15 ký tự
        String key = fullId.substring(0, 10);

        AutomationConfig a;
        a.id = fullId;
        
        // Đọc dữ liệu và gán vào Struct
        a.sensorId = prefs.getString((key + "_s").c_str(), "N/A");
        a.deviceId = prefs.getString((key + "_d").c_str(), "N/A");
        a.operation = prefs.getString((key + "_o").c_str(), ">");
        a.threshold = prefs.getFloat((key + "_t").c_str(), 0.0);
        a.cooldownMinutes = prefs.getInt((key + "_c").c_str(), 0);
        a.isEnabled = prefs.getBool((key + "_e").c_str(), true);
        a.value = prefs.getInt((key + "_v").c_str(), 0);
        a.lastActivated = 0;
        Serial.printf("  + Sensor: %s\n", prefs.getString((key + "_s").c_str(), "N/A").c_str());

        // Print ra Serial để debug
        Serial.printf(" + Khôi phục ID: %s\n", a.id.c_str());
        Serial.printf("   - Sensor: %s | Device: %s\n", a.sensorId.c_str(), a.deviceId.c_str());
        Serial.printf("   - Logic: %s %.2f | Value: %d\n", a.operation.c_str(), a.threshold, a.value);
        Serial.printf("   - Trạng thái: %s\n", a.isEnabled ? "BẬT" : "TẮT");
        Serial.println("   [OK]");

        // Đẩy vào danh sách quản lý
        automationList.push_back(a);
      }
      start = end + 1;
      end = idList.indexOf(',', start);
    }
    Serial.printf("------------------------------------\n[ROM] Hoàn tất! Đã nạp %d kịch bản vào RAM.\n", automationList.size());
  }

  prefs.end();
}
void saveStaffToROM() {
  prefs.begin("staff_data", false);
  prefs.clear(); 
  
  String sensorIds = "";
  for (auto& s : sensorList) {
    String key = "s_" + s.id.substring(0, 10);
    sensorIds += s.id + ","; // Lưu Full ID để dùng lại
    prefs.putInt((key + "_g").c_str(), s.gipo);
    prefs.putString((key + "_t").c_str(), s.type);
    prefs.putString((key + "_r").c_str(), s.roomId);
  }
  prefs.putString("s_list", sensorIds);

  String deviceIds = "";
  for (auto& d : deviceList) {
    String key = "d_" + d.id.substring(0, 10);
    deviceIds += d.id + ",";
    prefs.putInt((key + "_g").c_str(), d.gipo);
    prefs.putString((key + "_t").c_str(), d.type);
    prefs.putString((key + "_r").c_str(), d.roomId);
    prefs.putBool((key + "_s").c_str(), d.status);
  }
  prefs.putString("d_list", deviceIds);
  
  prefs.end();
  Serial.println("[ROM] Staff (Sensors/Devices) backed up.");
}

void loadStaffFromROM() {
  prefs.begin("staff_data", true);
  
  // Khôi phục Sensors
  String sList = prefs.getString("s_list", "");
  int start = 0, end = sList.indexOf(',');
  while (end != -1) {
    String id = sList.substring(start, end);
    String key = "s_" + id.substring(0, 10);
    
    SensorConfig s;
    s.id = id;
    s.gipo = prefs.getInt((key + "_g").c_str(), 0);
    s.type = prefs.getString((key + "_t").c_str(), "");
    s.roomId = prefs.getString((key + "_r").c_str(), "");
    
    if (s.type == "DHT11") {
      s.dhtInstance = new DHT(s.gipo, DHT11);
      s.dhtInstance->begin();
    } else {
      pinMode(s.gipo, INPUT);
    }
    sensorList.push_back(s);
    start = end + 1; end = sList.indexOf(',', start);
  }

  // Khôi phục Devices
  String dList = prefs.getString("d_list", "");
  start = 0; end = dList.indexOf(',');
  while (end != -1) {
    String id = dList.substring(start, end);
    String key = "d_" + id.substring(0, 10);
    
    DeviceConfig d;
    d.id = id;
    d.gipo = prefs.getInt((key + "_g").c_str(), 0);
    d.type = prefs.getString((key + "_t").c_str(), "");
    d.status = prefs.getBool((key + "_s").c_str(), false);
    
    if (d.type == "DOOR") {
      d.servoInstance = new Servo();
      d.servoInstance->attach(d.gipo);
      d.servoInstance->write(d.status ? 170 : 100);
    }else if(d.type == "PUMP"){
              pinMode(d.gipo, OUTPUT);

              digitalWrite(d.gipo, d.status ? LOW : HIGH);
            }
    else {
      pinMode(d.gipo, OUTPUT);
      digitalWrite(d.gipo, d.status ? HIGH : LOW);
    }
    deviceList.push_back(d);
    start = end + 1; end = dList.indexOf(',', start);
  }
  prefs.end();
  Serial.printf("[ROM] Restored %d sensors & %d devices\n", sensorList.size(), deviceList.size());
}
void setup() {
  Serial.begin(115200);
  delay(1000); // Chờ Serial ổn định

  // 1. Khởi tạo tài nguyên OS
  sensorQueue = xQueueCreate(20, sizeof(SensorMessage));
  autoQueue = xQueueCreate(20, sizeof(SensorMessage));
  deviceMutex = xSemaphoreCreateMutex();
  networkEvents = xEventGroupCreate();
  
  // 2. Kết nối WiFi
  Serial.println("[SETUP] Connecting to WiFi...");
  WiFi.begin(ssid, password);
  
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) { // Thử trong 10s
    delay(500);
    Serial.print(".");
    retry++;
  }

  bool isServerOnline = false;
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SETUP] WiFi connected!");
    
    HTTPClient http;
    // Check Server bằng endpoint nhẹ
    http.begin("http://192.168.1.122:5435/home/hello");
    int httpCode = http.GET();

    if (httpCode == 200) {
      isServerOnline = true;
      Serial.println("[SETUP] Server Online. Fetching data...");
      
      // Phải kết thúc request cũ trước khi bắt đầu request lấy staff
      http.end(); 
      
      http.begin("http://192.168.1.122:5435/house/staff?houseId=home1");
      http.setTimeout(4000);
      
      if (http.GET() == 200) {
        sensorList.clear(); 
        deviceList.clear();
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
            sensorList.push_back(s);
          } else if (kind == "DEVICE") {
            DeviceConfig d;
            d.gipo = item["gipo"];
            d.id = item["id"].as<String>();
            d.status = item["status"];
            d.type = item["type"].as<String>();
            d.roomId = item["roomId"].as<String>();
            if (d.type == "DOOR") {
              d.servoInstance = new Servo();
              d.servoInstance->attach(d.gipo);
              d.servoInstance->write(d.status ? 170 : 100);
            }else if(d.type == "PUMP"){
              pinMode(d.gipo, OUTPUT);

              digitalWrite(d.gipo, d.status ? LOW : HIGH);
            } else {
              pinMode(d.gipo, OUTPUT);

              digitalWrite(d.gipo, d.status ? HIGH : LOW);
            }
            deviceList.push_back(d);
          }
        }
        saveStaffToROM(); 
      }
      http.end(); // Kết thúc lấy staff
      
      getAndSaveAutomations(); // Hàm này bên trong đã có http.begin/end riêng
    } else {
      Serial.printf("[SETUP] Server Offline (Code: %d). Using ROM.\n", httpCode);
      http.end();
    }
  } else {
    Serial.println("\n[SETUP] No WiFi. Using ROM.");
  }

  // 3. Nếu Server không Online thì load từ Flash
  if (!isServerOnline) {
    loadStaffFromROM();
    loadAutomationsFromROM();
  }

  // 4. Khởi tạo MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // 5. Tạo Task (Luôn tạo Task dù có mạng hay không để hệ thống chạy offline)
  xTaskCreatePinnedToCore(vTaskAutomationProcessor, "AutoProc", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(vTaskNetwork, "NetManager", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(vTaskSensorCollector, "SensColl", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(vTaskMQTTProcessor, "MQTTProc", 16384, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(vTaskMonitor, "SysMon", 2048, NULL, 0, NULL, 1);
}
void loop() {
  // Không làm gì cả, Task loop sẽ bị xóa để tiết kiệm tài nguyên
  vTaskDelete(NULL);
}