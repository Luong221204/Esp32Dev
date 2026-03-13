#include <WiFi.h>
#include <map>
#include <vector>
#include <PubSubClient.h>
#include <DHT.h>
#include <HTTPClient.h>   // Sửa lỗi 'HTTPClient' was not declared
#include <ArduinoJson.h>  // Thư viện để xử lý JSON
const char* ssid = "Quan Quyen Luong";
const char* password = "1593572486";
const char* mqtt_server = "192.168.1.122";  // IP máy chạy NestJS
const int mqtt_port = 1883;
struct Property {
  const char* key;
  float value;
};

// Cấu trúc mở rộng để chứa con trỏ tới Object DHT
struct SensorConfig {
  int gipo;
  String id;
  String roomId;
  String type;
  float lastValue = -1.0;   // Để so sánh 10%
  DHT* dhtInstance = NULL;  // Con trỏ quản lý đối tượng DHT
};

struct DeviceConfig {
  int gipo;
  String roomId;
  String id;
  bool status;
  int value;
};

std::vector<DeviceConfig> deviceList;
std::vector<SensorConfig> sensorList;
std::map<String, std::vector<Property>> sensorMap;
WiFiClient espClient;
PubSubClient client(espClient);

bool isSignificantChange(float oldValue, float newValue) {
  if (oldValue == 0) return newValue != 0;  // Tránh chia cho 0
  float diff = fabs(newValue - oldValue);
  return (diff / fabs(oldValue)) >= 0.10;  // Trả về true nếu lệch >= 10%
}
void processSensorData(SensorConfig sensor, std::vector<Property> newData) {
  bool shouldSend = false;

  // 1. Kiểm tra xem sensor này đã từng có dữ liệu chưa
  if (sensorMap.count(sensor.id)) {
    std::vector<Property>& lastData = sensorMap[sensor.id];

    // Duyệt qua các thuộc tính mới nhận được
    for (auto& newProp : newData) {
      for (auto& oldProp : lastData) {
        if (newProp.key == oldProp.key) {
          if (isSignificantChange(oldProp.value, newProp.value)) {
            shouldSend = true;
            oldProp.value = newProp.value;  // Cập nhật giá trị cũ bằng giá trị mới
          }
        }
      }
    }
  } else {
    // Lần đầu tiên nhận dữ liệu từ Sensor này
    sensorMap[sensor.id] = newData;
    shouldSend = true;
  }

  // 2. Nếu có bất kỳ sự thay đổi nào > 10%, gửi MQTT
  if (shouldSend) {
    sendMqtt(sensor, sensorMap[sensor.id]);
  }
}
void sendMqtt(SensorConfig sensor, std::vector<Property> data) {
  StaticJsonDocument<256> doc;
  doc["sensorId"] = sensor.id;
  doc["roomId"] = sensor.roomId;

  JsonObject values = doc.createNestedObject("current");

  for (const auto& prop : data) {
    values[prop.key] = prop.value;
  }

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish("sensors/temp", buffer);
  Serial.print("MQTT Sent: ");
  Serial.println(buffer);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected!");
  HTTPClient http;
  http.begin("http://192.168.1.122:5435/house/staff?houseId=home1");

  if (http.GET() == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(8192);  // Tăng size để chứa mảng lớn
    deserializeJson(doc, payload);
    for (JsonObject item : doc.as<JsonArray>()) {

      String kind = item["kind"].as<String>();
      if (kind == "SENSOR") {
        SensorConfig s;
        s.gipo = item["gipo"].as<int>();
        s.id = item["id"].as<String>();
        s.type = item["type"].as<String>();
        s.roomId = item["roomId"].as<String>();
        s.type.trim();
        // LỖI 2: Sửa các biến trong printf cho đúng (dùng s.id, s.gipo thay vì id, gipo)
        Serial.println("---------------------------");
        Serial.printf("Found [%s]: ID=%s, Pin=%d, Type=%s, Room=%s",
                      kind.c_str(), s.id.c_str(), s.gipo, s.type.c_str(), s.roomId.c_str());
        if (s.type == "DHT11") {
          s.dhtInstance = new DHT(s.gipo, DHT11);
          s.dhtInstance->begin();
        } else {
          pinMode(s.gipo, INPUT);
        }
        sensorList.push_back(s);
      } else if (kind == "DEVICE") {
        DeviceConfig d;
        d.gipo = item["gipo"].as<int>();
        d.id = item["id"].as<String>();
        d.status = item["status"].as<bool>();
        d.value = item["value"].as<int>();
        d.roomId = item["roomId"].as<String>();
        // LỖI 2: Sửa các biến trong printf cho đúng (dùng s.id, s.gipo thay vì id, gipo)
        Serial.println("---------------------------");
        Serial.printf("Found [%s]: ID=%s, Pin=%d , R=%s",
                      kind.c_str(), d.id.c_str(), d.gipo,d.roomId.c_str());
        pinMode(d.gipo, OUTPUT);
        digitalWrite(d.gipo, d.status ? HIGH : LOW);
        deviceList.push_back(d);
      }
    }
  }
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  // 1. Chuyển payload thành String hoặc Json
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // 2. Parse JSON để lấy lệnh
  StaticJsonDocument<200> doc;
  deserializeJson(doc, message);

  bool status = doc["status"];
  const char* target = doc["id"];  // ví dụ: "relay1"
  bool found = false;
  for (auto& d : deviceList) {
    if (d.id == target) {
      // Cập nhật trạng thái trong struct
      d.status = status;

      // Thực thi điều khiển chân GPIO
      digitalWrite(d.gipo, d.status ? HIGH : LOW);

      Serial.printf("DONE: GPIO %d set to %s\n", d.gipo, d.status ? "HIGH" : "LOW");
      found = true;
      break;  // Tìm thấy rồi thì thoát vòng lặp cho nhanh
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    // client.connect("ID_DUY_NHAT")
    if (client.connect("Arduino_Cua_Tui")) {
      Serial.println("Đã kết nối Broker NestJS!");
      // Đăng ký nhận lệnh từ NestJS
      client.subscribe("devices");
    } else {
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  for (auto& s : sensorList) {
    // 1. Đọc dữ liệu dựa trên loại Sensor
    std::vector<Property> newData;
    if (s.type == "DHT11" && s.dhtInstance != NULL) {
      float temperature = s.dhtInstance->readTemperature();
      float humidity = s.dhtInstance->readHumidity();
      Property temp = {
        "temperature",
        temperature,
      };
      Property humid = {
        "humidity",
        humidity,
      };
      newData.push_back(temp);
      newData.push_back(humid);

    } else {
      // Ví dụ cảm biến lửa hoặc cảm biến khác trả về Analog
      float analog = analogRead(s.gipo);
      Property ana = {
        "analog",
        analog,
      };
      newData.push_back(ana);
    }
    processSensorData(s, newData);
  }
}