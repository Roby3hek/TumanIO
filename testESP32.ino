/*
 * TumanIO Pro - Упрощенная прошивка для ESP32 без SSL сертификата
 * Автор: Туманян Робэн Александрович
 * Версия: 1.1 (без SSL)
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <UniversalTelegramBot.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <Preferences.h>
#include <ESPmDNS.h>        // ← добавлено
#include <WiFiClientSecure.h> // ← для безопасности
#include <ArduinoOTA.h>

// ========================= КОНСТАНТЫ И НАСТРОЙКИ =========================
// WiFi настройки
const char* WIFI_SSID = "HUAWEI-1FR6UW";
const char* WIFI_PASS = "19072004";

// Telegram бот владельца
#define BOT_TOKEN ""
#define ADMIN_ID 

// Конфигурация пинов ESP32
// Реле (6 каналов)
#define RELAY_HEATER_1 2
#define RELAY_PUMP_1 4
#define RELAY_HEATER_2 5
#define RELAY_PUMP_2 32
#define RELAY_HEATER_3 33
#define RELAY_PUMP_3 27

// Датчики
#define PIN_DS18B20 35
#define PIN_LEVEL_1 36
#define PIN_LEVEL_2 39
#define PIN_LEVEL_3 34

// Кнопки
#define BTN_AROMA_1 0
#define BTN_AROMA_2 1
#define BTN_AROMA_3 3
#define BTN_START 8

// ========================= ПРОТОТИПЫ ФУНКЦИЙ =========================
void updateStateMachine();
void sendTelegramAlert(String message);
void saveStatistics();
void saveSettings();
void loadSettings();
void loadStatistics();
void connectToWiFi();
void setupOTA();
void updateLEDs();
void checkEmergencyConditions();

// ========================= ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ =========================
enum SystemState {
  STATE_WAITING, STATE_SELECTION, STATE_PAYMENT, STATE_HEATING,
  STATE_READY, STATE_WORKING, STATE_FINISH, STATE_EMERGENCY
};

SystemState currentState = STATE_WAITING;
int selectedAroma = 0;
float currentTemperature = 0.0;
bool paymentConfirmed = false;
bool heatingComplete = false;
bool systemError = false;
String errorMessage = "";

int liquidLevels[3] = {100, 100, 100};
bool lowLevel[3] = {false, false, false};

unsigned long heatingStartTime = 0;
unsigned long workingStartTime = 0;
unsigned long paymentStartTime = 0;

int totalSales = 0;
float totalRevenue = 0.0;
int aromaSales[3] = {0, 0, 0};
String aromaNames[3] = {"ЦИТРУС", "ТУТТИ-ФРУТТИ", "АНТИ-ТАБАК"};
float servicePrice = 100.0;

WiFiClient client;
UniversalTelegramBot bot(BOT_TOKEN, client);
Preferences preferences;
unsigned long lastBotUpdate = 0;

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

// ========================= ПРОСТАЯ РЕАЛИЗАЦИЯ LED МАТРИЦЫ =========================
void displayText(String text) {
  Serial.println("LED: " + text);
}

// ========================= УПРАВЛЕНИЕ РЕЛЕ =========================
void setupRelays() {
  pinMode(RELAY_HEATER_1, OUTPUT);
  pinMode(RELAY_PUMP_1, OUTPUT);
  pinMode(RELAY_HEATER_2, OUTPUT);
  pinMode(RELAY_PUMP_2, OUTPUT);
  pinMode(RELAY_HEATER_3, OUTPUT);
  pinMode(RELAY_PUMP_3, OUTPUT);
  
  digitalWrite(RELAY_HEATER_1, HIGH);
  digitalWrite(RELAY_PUMP_1, HIGH);
  digitalWrite(RELAY_HEATER_2, HIGH);
  digitalWrite(RELAY_PUMP_2, HIGH);
  digitalWrite(RELAY_HEATER_3, HIGH);
  digitalWrite(RELAY_PUMP_3, HIGH);
}

void turnOnHeater(int channel) {
  if(channel == 1) digitalWrite(RELAY_HEATER_1, LOW);
  else if(channel == 2) digitalWrite(RELAY_HEATER_2, LOW);
  else if(channel == 3) digitalWrite(RELAY_HEATER_3, LOW);
}

void turnOffHeater(int channel) {
  if(channel == 1) digitalWrite(RELAY_HEATER_1, HIGH);
  else if(channel == 2) digitalWrite(RELAY_HEATER_2, HIGH);
  else if(channel == 3) digitalWrite(RELAY_HEATER_3, HIGH);
}

void turnOnPump(int channel) {
  if(channel == 1) digitalWrite(RELAY_PUMP_1, LOW);
  else if(channel == 2) digitalWrite(RELAY_PUMP_2, LOW);
  else if(channel == 3) digitalWrite(RELAY_PUMP_3, LOW);
}

void turnOffPump(int channel) {
  if(channel == 1) digitalWrite(RELAY_PUMP_1, HIGH);
  else if(channel == 2) digitalWrite(RELAY_PUMP_2, HIGH);
  else if(channel == 3) digitalWrite(RELAY_PUMP_3, HIGH);
}

void turnOffAll() {
  digitalWrite(RELAY_HEATER_1, HIGH);
  digitalWrite(RELAY_PUMP_1, HIGH);
  digitalWrite(RELAY_HEATER_2, HIGH);
  digitalWrite(RELAY_PUMP_2, HIGH);
  digitalWrite(RELAY_HEATER_3, HIGH);
  digitalWrite(RELAY_PUMP_3, HIGH);
}

// ========================= ОБРАБОТКА КНОПОК =========================
bool checkButton(int pin) {
  static bool lastState[8] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
  static unsigned long lastDebounce[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  
  bool reading = digitalRead(pin);
  
  if(reading != lastState[pin]) {
    lastDebounce[pin] = millis();
  }
  
  if((millis() - lastDebounce[pin]) > 50) {
    if(reading == LOW && lastState[pin] == HIGH) {
      lastState[pin] = reading;
      return true;
    }
  }
  
  lastState[pin] = reading;
  return false;
}

// ========================= TELEGRAM БОТ (УПРОЩЕННЫЙ) =========================
void handleTelegramMessages() {
  if(millis() - lastBotUpdate > 1000) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    
    for(int i = 0; i < numNewMessages; i++) {
      String chat_id = bot.messages[i].chat_id;
      String text = bot.messages[i].text;
      
      // Проверка администратора
      if(String(chat_id) != String(ADMIN_ID)) {
        bot.sendMessage(chat_id, "⛔ Доступ запрещен!");
        continue;
      }
      
      // Обработка команд
      if(text == "/start" || text == "/menu") {
        String welcome = "🤖 *TumanIO Pro - Панель управления*\n\n";
        welcome += "Команды:\n";
        welcome += "/status - Статус системы\n";
        welcome += "/control - Управление\n";
        welcome += "/info - Информация\n";
        welcome += "/help - Помощь";
        bot.sendMessage(chat_id, welcome, "Markdown");
      }
      else if(text == "/status") {
        String status = "📊 *Статус системы*\n\n";
        status += "• Состояние: " + String(currentState) + "\n";
        status += "• Температура: " + String(currentTemperature, 1) + "°C\n";
        status += "• Продаж: " + String(totalSales) + "\n";
        status += "• Выручка: " + String(totalRevenue, 2) + " руб.";
        bot.sendMessage(chat_id, status, "Markdown");
      }
      else if(text == "/help") {
        String help = "📋 *Список команд:*\n\n";
        help += "/menu - Главное меню\n";
        help += "/status - Статус системы\n";
        help += "/start_now [1-3] - Запуск аромата\n";
        help += "/reboot - Перезагрузка\n";
        help += "/info - Информация об устройстве";
        bot.sendMessage(chat_id, help, "Markdown");
      }
      else if(text.startsWith("/start_now")) {
        if(text.length() > 11) {
          int aroma = text.substring(11).toInt();
          if(aroma >= 1 && aroma <= 3) {
            selectedAroma = aroma;
            paymentConfirmed = true;
            currentState = STATE_HEATING;
            bot.sendMessage(chat_id, "✅ Запуск аромата " + String(aroma) + " начат!");
          }
        }
      }
      else if(text == "/reboot") {
        bot.sendMessage(chat_id, "🔄 Перезагрузка...");
        delay(1000);
        ESP.restart();
      }
      else if(text == "/info") {
        String info = "ℹ️ *Информация об устройстве*\n\n";
        info += "• IP: " + WiFi.localIP().toString() + "\n";
        info += "• MAC: " + WiFi.macAddress() + "\n";
        info += "• Память: " + String(ESP.getFreeHeap()) + " байт\n";
        info += "• Версия: 1.1";
        bot.sendMessage(chat_id, info, "Markdown");
      }
      else {
        bot.sendMessage(chat_id, "Неизвестная команда. Используйте /help");
      }
    }
    
    lastBotUpdate = millis();
  }
}

void sendTelegramAlert(String message) {
  bot.sendMessage(String(ADMIN_ID), message);
}

// ========================= МАШИНА СОСТОЯНИЙ =========================
void updateStateMachine() {
  static unsigned long stateStartTime = 0;
  static bool stateChanged = true;
  
  if(stateChanged) {
    stateStartTime = millis();
    stateChanged = false;
    
    // Действия при входе в состояние
    switch(currentState) {
      case STATE_WAITING:
        displayText("ДОБРО ПОЖАЛОВАТЬ!");
        turnOffAll();
        break;
      case STATE_SELECTION:
        displayText("ВЫБЕРИТЕ АРОМАТ");
        turnOnHeater(selectedAroma);
        heatingStartTime = millis();
        break;
      case STATE_PAYMENT:
        displayText("ОЖИДАНИЕ ОПЛАТЫ");
        break;
      case STATE_HEATING:
        displayText("НАГРЕВ...");
        break;
      case STATE_READY:
        displayText("НАЖМИТЕ СТАРТ");
        break;
      case STATE_WORKING:
        displayText("РАБОТА");
        turnOnPump(selectedAroma);
        workingStartTime = millis();
        break;
      case STATE_FINISH:
        displayText("СПАСИБО!");
        break;
      case STATE_EMERGENCY:
        displayText("АВАРИЯ!");
        turnOffAll();
        break;
    }
  }
  
  // Обновление состояний
  switch(currentState) {
    case STATE_WAITING:
      // Проверка кнопок ароматов
      if(checkButton(BTN_AROMA_1)) selectedAroma = 1;
      else if(checkButton(BTN_AROMA_2)) selectedAroma = 2;
      else if(checkButton(BTN_AROMA_3)) selectedAroma = 3;
      
      if(selectedAroma > 0) {
        // Проверка уровня жидкости
        if(liquidLevels[selectedAroma-1] > 20) {
          currentState = STATE_SELECTION;
          stateChanged = true;
        } else {
          displayText("АРОМАТ " + String(selectedAroma) + " ЗАКОНЧИЛСЯ");
          selectedAroma = 0;
        }
      }
      break;
      
    case STATE_SELECTION:
      // Автоматический переход к оплате через 2 секунды
      if(millis() - stateStartTime > 2000) {
        currentState = STATE_PAYMENT;
        stateChanged = true;
      }
      break;
      
    case STATE_PAYMENT:
      // Эмуляция оплаты - через 3 секунды считаем оплаченным
      if(millis() - stateStartTime > 3000) {
        paymentConfirmed = true;
        currentState = STATE_HEATING;
        stateChanged = true;
      }
      break;
      
    case STATE_HEATING:
      {
        // Чтение температуры
        sensors.requestTemperatures();
        currentTemperature = sensors.getTempCByIndex(0);
        
        displayText("ТЕМП: " + String(currentTemperature, 1) + "C");
        
        // Проверка достижения температуры
        if(currentTemperature >= 85.0) {
          heatingComplete = true;
          currentState = STATE_READY;
          stateChanged = true;
        }
        
        // Проверка таймаута нагрева
        if(millis() - heatingStartTime > 180000) { // 3 минуты
          sendTelegramAlert("🚨 Таймаут нагрева!");
          currentState = STATE_EMERGENCY;
          stateChanged = true;
        }
      }
      break;
      
    case STATE_READY:
      // Ожидание кнопки СТАРТ
      if(checkButton(BTN_START)) {
        currentState = STATE_WORKING;
        stateChanged = true;
      }
      
      // Поддержание температуры
      if(currentTemperature < 85.0) {
        turnOnHeater(selectedAroma);
      } else if(currentTemperature > 95.0) {
        turnOffHeater(selectedAroma);
      }
      break;
      
    case STATE_WORKING:
      {
        int secondsLeft = 60 - (millis() - workingStartTime) / 1000;
        if(secondsLeft < 0) secondsLeft = 0;
        
        displayText("ОСТАЛОСЬ: " + String(secondsLeft) + "С");
        
        if(millis() - workingStartTime >= 60000) { // 60 секунд
          turnOffPump(selectedAroma);
          
          // Обновление статистики
          aromaSales[selectedAroma-1]++;
          totalSales++;
          totalRevenue += servicePrice;
          saveStatistics();
          
          currentState = STATE_FINISH;
          stateChanged = true;
        }
      }
      break;
      
    case STATE_FINISH:
      // Возврат в ожидание через 30 секунд
      if(millis() - stateStartTime > 30000) {
        selectedAroma = 0;
        paymentConfirmed = false;
        heatingComplete = false;
        currentState = STATE_WAITING;
        stateChanged = true;
      }
      break;
      
    case STATE_EMERGENCY:
      // Требуется сброс через Telegram
      break;
  }
}

// ========================= ОСНОВНЫЕ ФУНКЦИИ =========================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== TumanIO Pro - Запуск ===");
  
  // Инициализация EEPROM и Preferences
  EEPROM.begin(512);
  preferences.begin("tumanio", false);
  loadSettings();
  loadStatistics();
  
  // Настройка пинов
  setupRelays();
  
  pinMode(PIN_LEVEL_1, INPUT_PULLUP);
  pinMode(PIN_LEVEL_2, INPUT_PULLUP);
  pinMode(PIN_LEVEL_3, INPUT_PULLUP);
  
  pinMode(BTN_AROMA_1, INPUT_PULLUP);
  pinMode(BTN_AROMA_2, INPUT_PULLUP);
  pinMode(BTN_AROMA_3, INPUT_PULLUP);
  pinMode(BTN_START, INPUT_PULLUP);
  
  // Датчик температуры
  sensors.begin();
  
  // Подключение к WiFi
  connectToWiFi();
  
  // Настройка OTA
  setupOTA();
  
  Serial.println("Система готова к работе");
}

void loop() {
  // Обновление состояния системы
  updateStateMachine();
  
  // Чтение датчиков уровня
  liquidLevels[0] = digitalRead(PIN_LEVEL_1) == LOW ? 100 : 0;
  liquidLevels[1] = digitalRead(PIN_LEVEL_2) == LOW ? 100 : 0;
  liquidLevels[2] = digitalRead(PIN_LEVEL_3) == LOW ? 100 : 0;
  
  // Проверка аварийных ситуаций
  checkEmergencyConditions();
  
  // Обработка Telegram сообщений
  handleTelegramMessages();
  
  // Обслуживание OTA
  ArduinoOTA.handle();
  
  delay(10);
}

void connectToWiFi() {
  Serial.print("Подключение к WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi подключен!");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nОшибка подключения к WiFi!");
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("tumanio-pro");
  ArduinoOTA.begin();
}

void checkEmergencyConditions() {
  // Проверка перегрева
  if(currentTemperature > 100.0) {
    systemError = true;
    errorMessage = "Перегрев ТЭНа!";
    currentState = STATE_EMERGENCY;
    sendTelegramAlert("🔥 Перегрев: " + String(currentTemperature, 1) + "C");
  }
  
  // Проверка низкого уровня
  static bool lowLevelSent[3] = {false, false, false};
  for(int i = 0; i < 3; i++) {
    if(liquidLevels[i] == 0 && !lowLevelSent[i]) {
      sendTelegramAlert("🚨 Аромат " + String(i+1) + " закончился!");
      lowLevelSent[i] = true;
    } else if(liquidLevels[i] > 0) {
      lowLevelSent[i] = false;
    }
  }
}

void saveStatistics() {
  preferences.putInt("total_sales", totalSales);
  preferences.putFloat("total_revenue", totalRevenue);
  
  for(int i = 0; i < 3; i++) {
    String key = "sales_aroma" + String(i+1);
    preferences.putInt(key.c_str(), aromaSales[i]);
  }
  
  Serial.println("Статистика сохранена");
}

void loadStatistics() {
  totalSales = preferences.getInt("total_sales", 0);
  totalRevenue = preferences.getFloat("total_revenue", 0.0);
  
  for(int i = 0; i < 3; i++) {
    String key = "sales_aroma" + String(i+1);
    aromaSales[i] = preferences.getInt(key.c_str(), 0);
  }
  
  Serial.println("Статистика загружена");
}

void saveSettings() {
  preferences.putFloat("price", servicePrice);
  
  for(int i = 0; i < 3; i++) {
    String key = "aroma" + String(i+1);
    preferences.putString(key.c_str(), aromaNames[i].c_str());
  }
  
  Serial.println("Настройки сохранены");
}

void loadSettings() {
  servicePrice = preferences.getFloat("price", 100.0);
  
  for(int i = 0; i < 3; i++) {
    String key = "aroma" + String(i+1);
    String defaultName = aromaNames[i];
    aromaNames[i] = preferences.getString(key.c_str(), defaultName.c_str());
  }
  
  Serial.println("Настройки загружены");
}
