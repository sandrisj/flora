/**
   A BLE client for the Xiaomi Mi Plant Sensor, pushing measurements to an MQTT server.

   See https://github.com/nkolban/esp32-snippets/blob/master/Documentation/BLE%20C%2B%2B%20Guide.pdf
   on how bluetooth low energy and the library used are working.

   See https://github.com/ChrisScheffler/miflora/wiki/The-Basics for details on how the
   protocol is working.

   MIT License

   Copyright (c) 2017 Sven Henkel
   Multiple units reading by Grega Lebar 2018

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

#include "BLEDevice.h"
#include <WiFi.h>
#include <PubSubClient.h>

#include "config.h"

// boot count used to check if battery status should be read
RTC_DATA_ATTR int bootCount = 0;

// Root service for Flora Devices
static BLEUUID rootServiceDataUUID((uint16_t) 0xfe95);
  
// the remote service we wish to connect to
static BLEUUID serviceUUID("00001204-0000-1000-8000-00805f9b34fb");

// the characteristic of the remote service we are interested in
static BLEUUID uuid_version_battery("00001a02-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_sensor_data("00001a01-0000-1000-8000-00805f9b34fb");
static BLEUUID uuid_write_mode("00001a00-0000-1000-8000-00805f9b34fb");

TaskHandle_t hibernateTaskHandle = NULL;

WiFiClient espClient;
PubSubClient client(espClient);

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("");
}

void disconnectWifi() {
  WiFi.disconnect(true);
  Serial.println("WiFi disonnected");
}

void connectMqtt() {
  Serial.println("Connecting to MQTT...");
  client.setServer(MQTT_HOST, MQTT_PORT);

  while (!client.connected()) {
    if (!client.connect(MQTT_CLIENTID, MQTT_USERNAME, MQTT_PASSWORD)) {
      Serial.print("MQTT connection failed:");
      Serial.print(client.state());
      Serial.println("Retrying...");
      delay(MQTT_RETRY_WAIT);
    }
  }

  Serial.println("MQTT connected");
  Serial.println("");
}

void disconnectMqtt() {
  client.disconnect();
  Serial.println("MQTT disconnected");
}

BLEClient* getFloraClient(BLEAddress floraAddress) {
  BLEClient* floraClient = BLEDevice::createClient();

  if (!floraClient->connect(floraAddress)) {
    Serial.print("- Connection failed, skipping ");
    Serial.println(floraAddress.toString().c_str());
    return nullptr;
  }

  Serial.println("- Connection successful");
  return floraClient;
}

BLERemoteService* getFloraService(BLEClient* floraClient) {
  BLERemoteService* floraService = nullptr;

  try {
    floraService = floraClient->getService(serviceUUID);
  }
  catch (...) {
    // something went wrong
  }
  if (floraService == nullptr) {
    Serial.println("- Failed to find data service");
  }
  else {
    Serial.println("- Found data service");
  }

  return floraService;
}

bool forceFloraServiceDataMode(BLERemoteService* floraService) {
  BLERemoteCharacteristic* floraCharacteristic;

  // get device mode characteristic, needs to be changed to read data
  Serial.println("- Force device in data mode");
  floraCharacteristic = nullptr;
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_write_mode);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // write the magic data
  uint8_t buf[2] = {0xA0, 0x1F};
  floraCharacteristic->writeValue(buf, 2, true);

  delay(500);
  return true;
}

bool readFloraDataCharacteristic(BLERemoteService* floraService, String baseTopic) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the main device data characteristic
  Serial.println("- Access characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_sensor_data);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping device");
    return false;
  }

  // read characteristic value
  Serial.println("- Read value from characteristic");
  std::string value;
  try {
    value = floraCharacteristic->readValue();
  }
  catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping device");
    return false;
  }
  const char *val = value.c_str();

  Serial.print("Hex: ");
  for (int i = 0; i < 16; i++) {
    Serial.print((int)val[i], HEX);
    Serial.print(" ");
  }
  Serial.println(" ");

  int16_t* temp_raw = (int16_t*)val;
  float temperature = (*temp_raw) / ((float)10.0);
  Serial.print("-- Temperature: ");
  Serial.println(temperature);

  int moisture = val[7];
  Serial.print("-- Moisture: ");
  Serial.println(moisture);

  int light = val[3] + val[4] * 256;
  Serial.print("-- Light: ");
  Serial.println(light);

  int conductivity = val[8] + val[9] * 256;
  Serial.print("-- Conductivity: ");
  Serial.println(conductivity);

 if (temperature > 100 || temperature < -100) {
    Serial.println("-- Unreasonable values received, skip publish");
    return false;
  }

  char buffer[64];

  snprintf(buffer, 64, "%f", temperature);
  client.publish((baseTopic + "temperature").c_str(), buffer, true);
 char state[] = "{temperature:";
  strcat(state,buffer);
delay(100);
  
  snprintf(buffer, 64, "%d", moisture);
  client.publish((baseTopic + "moisture").c_str(), buffer, true);
   strcat(state,",moisture:");
  strcat(state,buffer);
delay(100);
  
  snprintf(buffer, 64, "%d", light);
  client.publish((baseTopic + "light").c_str(), buffer, true);
   strcat(state,",light:");
  strcat(state,buffer);
  delay(100);
  
  snprintf(buffer, 64, "%d", conductivity);
  client.publish((baseTopic + "conductivity").c_str(), buffer, true);
  strcat(state,",conductivity:");
  strcat(state,buffer);
  strcat(state,"}");
  delay(100);
  client.publish((baseTopic + "state").c_str(), state);

  
  Serial.println("MQTT pub for topic: " + baseTopic);

  return true;
}

bool readFloraBatteryCharacteristic(BLERemoteService* floraService, String baseTopic) {
  BLERemoteCharacteristic* floraCharacteristic = nullptr;

  // get the device battery characteristic
  Serial.println("- Access battery characteristic from device");
  try {
    floraCharacteristic = floraService->getCharacteristic(uuid_version_battery);
  }
  catch (...) {
    // something went wrong
  }
  if (floraCharacteristic == nullptr) {
    Serial.println("-- Failed, skipping battery level");
    return false;
  }

  // read characteristic value
  Serial.println("- Read value from characteristic");
  std::string value;
  try {
    value = floraCharacteristic->readValue();
  }
  catch (...) {
    // something went wrong
    Serial.println("-- Failed, skipping battery level");
    return false;
  }
  const char *val2 = value.c_str();
  int battery = val2[0];

  char buffer[64];

  Serial.print("-- Battery: ");
  Serial.println(battery);
  snprintf(buffer, 64, "%d", battery);
  client.publish((baseTopic + "battery").c_str(), buffer, true);

  return true;
}

bool processFloraService(BLERemoteService* floraService, const char* deviceMacAddress, bool readBattery) {
  // set device in data mode
  if (!forceFloraServiceDataMode(floraService)) {
    return false;
  }

  String baseTopic = MQTT_BASE_TOPIC + "/" + deviceMacAddress + "/";
  bool dataSuccess = readFloraDataCharacteristic(floraService, baseTopic);

  bool batterySuccess = true;
  if (readBattery) {
    batterySuccess = readFloraBatteryCharacteristic(floraService, baseTopic);
  }

  return dataSuccess && batterySuccess;
}

bool processFloraDevice(BLEAddress floraAddress, bool getBattery, int tryCount) {
  Serial.print("Processing Flora device at ");
  Serial.print(floraAddress.toString().c_str());
  Serial.print(" (try ");
  Serial.print(tryCount);
  Serial.println(")");

  // connect to flora ble server
  BLEClient* floraClient = getFloraClient(floraAddress);
  if (floraClient == nullptr) {
    return false;
  }

  // connect data service
  BLERemoteService* floraService = getFloraService(floraClient);
  if (floraService == nullptr) {
    floraClient->disconnect();
    return false;
  }

  // process devices data
  bool success = processFloraService(floraService, floraAddress.toString().c_str(), getBattery);

  // disconnect from device
  floraClient->disconnect();

  return success;
}

void hibernate() {
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION * 1000000ll);
  Serial.println("Going to sleep now.");
  delay(100);
  esp_deep_sleep_start();
}

void delayedHibernate(void *parameter) {
  delay(EMERGENCY_HIBERNATE * 1000); // delay for five minutes
  Serial.println("Something got stuck, entering emergency hibernate...");
  hibernate();
}

// before setup()
static void my_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
  ESP_LOGW(LOG_TAG, "custom gattc event handler, event: %d", (uint8_t)event);
  if (event == ESP_GATTC_DISCONNECT_EVT) {
    Serial.print("Disconnect reason: ");
    Serial.println((int)param->disconnect.reason);
  }
}


class FloraDevicesScanner {
  public:
    // Scan BLE and return true if flora devices are found
    bool scan();

    int getDeviceCount() const {
      return _deviceCount;
    }

    std::string getDeviceAddress(int i) const {
      if (i < _deviceCount)
        return _devices[i];
      else
        return std::string();
    }

  private:
    std::string _devices[MAX_DEVICES];
    int         _deviceCount = 0;

    void registerDevice(BLEAdvertisedDevice& advertisedDevice) {
      std::string deviceAddress(advertisedDevice.getAddress().toString());
      Serial.print("Flora device found at address ");
      Serial.println(deviceAddress.c_str());
      
      if (_deviceCount < MAX_DEVICES)
        _devices[_deviceCount++] = deviceAddress;
      else
        Serial.println("can't register device, no remaining slot");
    }

};

bool FloraDevicesScanner::scan() {
  Serial.println("Scan BLE, looking for Flora Devices");

  // detect and register Flora devices during BLE scan
  class FloraDevicesBLEDetector: public BLEAdvertisedDeviceCallbacks {
    public:
      FloraDevicesBLEDetector(FloraDevicesScanner &floraScanner) : _floraScanner(floraScanner) { }
      
      void onResult(BLEAdvertisedDevice advertisedDevice)
      {
        if (advertisedDevice.haveServiceUUID()) {
          BLEUUID service = advertisedDevice.getServiceUUID();
          if (service.equals(rootServiceDataUUID))
            _floraScanner.registerDevice(advertisedDevice);
        }
      }
      
    private:
      FloraDevicesScanner& _floraScanner;
  };
  
  BLEScan* scan = BLEDevice::getScan();
  FloraDevicesBLEDetector floraDetector(*this);
  scan->setAdvertisedDeviceCallbacks(&floraDetector);
  scan->start(BLE_SCAN_DURATION);
  
  Serial.print("Number of Flora devices detected: ");
  Serial.println(_deviceCount);
  return (_deviceCount > 0);
}


void setup() {
  // all action is done when device is woken up
  Serial.begin(115200);
  delay(1000);

  // increase boot count
  bootCount++;

  // create a hibernate task in case something gets stuck
  xTaskCreate(delayedHibernate, "hibernate", 4096, NULL, 1, &hibernateTaskHandle);

  Serial.println("Initialize BLE client...");
  // BLEDevice::setCustomGattcHandler(my_gattc_event_handler);  // before BLEDevice::init();
  BLEDevice::init("");
  BLEDevice::setPower(ESP_PWR_LVL_P7);

  FloraDevicesScanner floraScanner;
  if (floraScanner.scan()) {

    // connecting wifi and mqtt server
    connectWifi();
    connectMqtt();

    // check if battery status should be read - based on boot count
    bool readBattery = ((bootCount % BATTERY_INTERVAL) == 0);
    if (readBattery) Serial.println("Battery will be read during this run");

    // process devices
    for (int i = 0; i < floraScanner.getDeviceCount(); i++) {
      int tryCount = 0;
      BLEAddress floraAddress(floraScanner.getDeviceAddress(i));

      while (tryCount < RETRY) {
        tryCount++;
        if (processFloraDevice(floraAddress, readBattery, tryCount)) {
          break;
        }
        delay(1000);
      }
      delay(1500);
    }

    // disconnect wifi and mqtt
    disconnectWifi();
    disconnectMqtt();
  }

  // delete emergency hibernate task
  vTaskDelete(hibernateTaskHandle);

  // go to sleep now
  hibernate();
}

void loop() {
  /// we're not doing anything in the loop, only on device wakeup
  delay(10000);
}
