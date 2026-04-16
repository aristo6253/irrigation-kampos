extern "C" uint64_t esp_rtc_get_time_us();
#include <esp_system.h>
// #include <FirebaseESP32.h>
#include <Firebase_ESP_Client.h>
#include <WiFi.h>         // ESP32 WiFi library
#include <Preferences.h>  // ESP32's NVS storage
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include "RTClib.h"
//Provide the token generation process info.
#include "addons/TokenHelper.h"
// //Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp32/ulp.h"
#include "secrets.h"

#define RTC_MAGIC 0xDEADBEEF
#define WAKE_GPIO GPIO_NUM_33  // External trigger pin (RTC capable)

// mux pins + outputs
const int A_PIN = 25;
const int B_PIN = 26;
const int C_PIN = 27;
const int muxINH = 2;
const int voltRead = 35;
const int mainVlv = 13;  // output No. 8 - reverse logic
const int USB = 19;      // USB control reverse logic
const int divider = 18;  // activate relay switch to measure battery voltage
const int flowsensor = 34;
const int fiveVltSwitch = 32;     // 5v DC high-side switched through PMOS, so that it disconnects during deepsleep and avoid energy waste due to pull-ups
const int WDT = 4;                // pin feeding an external watchdog timer, wired to trigger reset if not fed for longer than 60s
const int valveDelay = 6000;      // time for central ball valve to open or close - if no central ball valve is to be used, set to 10
const long updateIntrvl = 60000;  // frequency of sending update to FB during watering
const int connDelay = 40;         // Secs waiting router to wake up and connect to network

RTC_DS3231 rtc;

// Credentials are defined in secrets.h (gitignored) — copy secrets.h.example to get started

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* ntpServer = "pool.ntp.org";


// RTC Memory structure - Variables to be kept during deepsleep
RTC_DATA_ATTR struct RTCData {
  uint32_t magic;
  unsigned long epochTime;
  unsigned long lastSrvrEpoch;
  float VOLUME_THRESHOLD;  // [remotely modifiable from FB]
  int eveningPauseTime;    // relevant only in mode 1
  int morningResumeTime;   // [remotely modifiable from FB]
  int oldDay;
  float totalLitres;
  bool charged;
  bool includeCharging;  // [remotely modifiable from FB] - include a 1-hour charging seession around midday when solar energy is at the maximum (in case router is battery-operated)
  uint64_t sleepStartEpoch;
  unsigned long WATERING_PAUSE_SEC;
  int channel;
  unsigned long SLEEP_TIME_SEC;  // should be 0 to resume watering attempts
  bool mode;                     // 0 = pressure sensor wakeup, 1 = interval checks during daytime
  bool sub_mode_timed;           // relevant only in mode 0 - to be set to true if no flow sensor in use
  bool updateSettings;           //variable to by synched from FB daily and only if true to update the rest of the remotely modifiable settings
  bool chkAgainNeedToUpdt;       // to trigger synching the "updateSettings" from FB if that does not succeed on the first wakeup of the day
  bool watered;
  int NUM_OUTPUTS;        //max: 8 - irrigation volume or time to be equqlly devided between the number of outputs
  int waterIntervalDays;  // If 0 irrigation will be possible upon next morningResumeTime. If 1 pause to be extended by 24 hours, if 2 by 48 hours, and so on.
  bool wakePinAtSleep;
  uint64_t internalRtcTimeAtSleep;
  uint32_t us_remainder;
} rtcMem;


uint64_t elapsedSec = 0;
esp_sleep_wakeup_cause_t wkUpCause;
volatile uint32_t flow_frequency;  // Measures flow sensor pulses (Hall effect waterflow sensor)
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

const unsigned long oneHourSecs = 3600;
int adcValue = 0;
bool shift = 0;
uint64_t startClckUs;
String timeString;
String datestring;
String timestringfornode;
String timeOriginCode;
unsigned long cloopTime;
unsigned long voltcurrentTime;
int wra;
int lepto;
int deftero;
int mera;
int xronos;
float volt = 0;
unsigned long startTime;
float flowRate = 0.0;
unsigned long flowMilliLitres;
unsigned int totalMilliLitres;
float flowLitres;
bool coldBoot = false;
bool offlineMode = false;
bool snoozed = false;
bool justCharged = false;
unsigned long rstMillis;
uint64_t currentEpoch;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;



void IRAM_ATTR pulseCounter() {
  portENTER_CRITICAL_ISR(&mux);
  flow_frequency++;
  portEXIT_CRITICAL_ISR(&mux);
}

inline bool FB_GUARD() {
  return (WiFi.status() == WL_CONNECTED && WiFi.RSSI() > -75 && Firebase.ready());
}

void setup() {

  // Set timezone to Eastern European Time
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();
  pinMode(WDT, OUTPUT);
  digitalWrite(WDT, LOW);
  pinMode(muxINH, OUTPUT);
  digitalWrite(muxINH, HIGH);
  pinMode(USB, OUTPUT);
  digitalWrite(USB, HIGH);
  pinMode(mainVlv, OUTPUT);
  digitalWrite(mainVlv, HIGH);
  Serial.begin(9600);
  Wire.begin(21, 22);
  rtc.begin(&Wire);
  startClckUs = esp_timer_get_time();

  wkUpCause = esp_sleep_get_wakeup_cause();
  Serial.print("Wakeup cause: ");
  Serial.println(wkUpCause);
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("RESET REASON: ");
  Serial.println(reason);

  if (reason == ESP_RST_DEEPSLEEP) {
    coldBoot = false;
    // Calculate how long we slept using the modern ESP32 RTC timer
    uint64_t internalRtcNow = esp_rtc_get_time_us();
    uint64_t sleptMicros = internalRtcNow - rtcMem.internalRtcTimeAtSleep;
    sleptMicros += rtcMem.us_remainder;
    uint64_t sleptSeconds = sleptMicros / 1000000ULL;
    rtcMem.us_remainder = sleptMicros % 1000000ULL;

    // ONLY apply this if the external RTC is dead to avoid double-counting
    if (getEpochFromDS3231() == 0) {
      rtcMem.epochTime += sleptSeconds;
      rstMillis = millis() - (rtcMem.us_remainder / 1000);
    }
  } else {
    coldBoot = true;
    initializeRTCDefaults();
    rstMillis = millis();
    rtcMem.us_remainder = 0;
  }

  validateStructIntegrity();

  pinMode(A_PIN, OUTPUT);
  pinMode(B_PIN, OUTPUT);
  pinMode(C_PIN, OUTPUT);
  pinMode(fiveVltSwitch, OUTPUT);
  digitalWrite(fiveVltSwitch, LOW);  //switch 5v ON
  FBsignup();
  pinMode(flowsensor, INPUT);  // pinMode(flowsensor, INPUT_PULLUP);
  pinMode(divider, OUTPUT);
  digitalWrite(divider, LOW);
  rtcMem.totalLitres = 0.0;
  digitalWrite(WDT, !digitalRead(WDT));
  attachInterrupt(digitalPinToInterrupt(flowsensor), pulseCounter, FALLING);  // attachInterrupt(digitalPinToInterrupt(flowsensor), pulseCounter, RISING);
}

void loop() {
  int firstVlv = rtcMem.channel;
  connectWiFi();
  synchTimeExtRTC();

  if (rtcMem.epochTime > rtcMem.sleepStartEpoch) elapsedSec = rtcMem.epochTime - rtcMem.sleepStartEpoch;
  else elapsedSec = 0;
  Serial.print("elapsedSec: ");
  Serial.println(elapsedSec);

  if (!rtcMem.mode || (wra & 1)) recordVoltage();
  if (coldBoot) {
    getSettings();
    rtcMem.oldDay = mera;
  } else if (rtcMem.oldDay != mera) {
    rtcMem.charged = false;
    rtcMem.chkAgainNeedToUpdt = true;
    chkUpdtSettings();
    if (rtcMem.updateSettings) getSettings();
    rtcMem.oldDay = mera;
  }
  if (!coldBoot && rtcMem.oldDay == mera && rtcMem.chkAgainNeedToUpdt) {
    chkUpdtSettings();
    if (rtcMem.updateSettings) getSettings();
  }

  if (wkUpCause == ESP_SLEEP_WAKEUP_EXT0) {
    if (rtcMem.wakePinAtSleep) {

      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/WaterOff" + timestringfornode, timeString);
      }
      if (elapsedSec < rtcMem.WATERING_PAUSE_SEC) rtcMem.WATERING_PAUSE_SEC -= elapsedSec;
      setMaxSlTime();
      startDeepSleep();
    } else {
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/WaterOn" + timestringfornode, timeString);
      }
      if ((elapsedSec + oneHourSecs) < rtcMem.WATERING_PAUSE_SEC)
      // ❌ Too early → record wake, adjust remaining time to attempt irrigation and go back to sleep
      {
        snoozed = true;
        Serial.println("snoozing");
        if (elapsedSec < rtcMem.WATERING_PAUSE_SEC) rtcMem.WATERING_PAUSE_SEC -= elapsedSec;

        if (FB_GUARD()) {
          bool ok = Firebase.RTDB.setBool(&fbdo, datestring + "/Snoozed" + timestringfornode, 1);
        }
        setMaxSlTime();
        startDeepSleep();
      } else {
        rtcMem.WATERING_PAUSE_SEC = 0;
        rtcMem.watered = false;
        snoozed = false;
      }
    }
  }

  else if (!coldBoot)  //if (wkUpCause == ESP_SLEEP_WAKEUP_TIMER)
                       // If max sleep time elapsed send life signal and go back to sleep
  {
    if (!rtcMem.mode)  //mode 0
    {
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setBool(&fbdo, datestring + "/LifeSignal" + timestringfornode, 1);
      }
      if ((elapsedSec + oneHourSecs) < rtcMem.WATERING_PAUSE_SEC) {
        rtcMem.WATERING_PAUSE_SEC -= elapsedSec;
        // rtcMem.watered = false;
      } else rtcMem.WATERING_PAUSE_SEC = 0;
      setMaxSlTime();
      startDeepSleep();
    } else  //mode 1
    {
      if ((elapsedSec + oneHourSecs) < rtcMem.WATERING_PAUSE_SEC)
      // ❌ Too early → record wake, adjust remaining time to attempt irrigation and go back to sleep till next morning
      {
        snoozed = true;
        Serial.println("snoozing");
        if (elapsedSec < rtcMem.WATERING_PAUSE_SEC) rtcMem.WATERING_PAUSE_SEC -= elapsedSec;
        if (FB_GUARD()) {
          bool ok = Firebase.RTDB.setBool(&fbdo, datestring + "/LifeSignal" + timestringfornode, 1);
        }

        setMaxSlTime();
        startDeepSleep();
      } else {
        snoozed = false;
        rtcMem.watered = false;
      }
    }
  }
  //If reached this point in loop() without being put back into deep sleep, code proceeds below to open valve and if flow exists run a watering cycle.
  openvalve();
  digitalWrite(mainVlv, LOW);  // open main valve
  delay(valveDelay);           // needs around 6 sec waiting for main (ball) valve to open sufficiently before checking if there is flow

  if (!rtcMem.mode && rtcMem.sub_mode_timed && !coldBoot) timeIrrigation();  // in case system is set to irrigate based on time - If pressure is detected  in mode 0 and there is no irrigation pause active, the system will keep the valves open without checking the presence of flow though the flow sensor. When finished it will go to sleep and not proceed with the rest of loop().

  uint32_t local_frequency;
  portENTER_CRITICAL(&mux);
  local_frequency = flow_frequency;
  portEXIT_CRITICAL(&mux);
  if (local_frequency <= 20)  // if no flow with the next valve
  {
    progressChnl();  // try with the next valve
    openvalve();
    delay(1000);
    portENTER_CRITICAL(&mux);
    local_frequency = flow_frequency;
    portEXIT_CRITICAL(&mux);
    if (local_frequency <= 20)  // if there is still no flow, close the valve and record the finding in FB
    {
      shutVlvs();
      updtEpochOffln();
      setTimeString();
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setInt(&fbdo, datestring + "/Checked-NoFlow" + timestringfornode, rtcMem.channel);
      }
      progressChnl();
      setMaxSlTime();
      rtcMem.WATERING_PAUSE_SEC = 0;
      rtcMem.watered = false;
      startDeepSleep();
      return;// in case it gets back here due to a return hit in startDeepSleep(), return back to the beginning of loop()
    } else {
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setInt(&fbdo, datestring + "/FaultyValve" + timestringfornode, firstVlv);
      }
    }
  }
  float segmentVol = rtcMem.VOLUME_THRESHOLD / rtcMem.NUM_OUTPUTS;
  volCalc();
  openvalve();
  updtEpochOffln();
  setTimeString();
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/Irrigation/Started-Vlv-" + String(rtcMem.channel) + timestringfornode, roundf(rtcMem.totalLitres * 100.0f) / 100.0f);
  }
  int line = 0;
  int segment = 1;
  unsigned long currentTime;
  cloopTime = millis();
  unsigned long SegmentLoop = millis();
  while (!(rtcMem.totalLitres >= rtcMem.VOLUME_THRESHOLD)) {
    currentTime = millis();
    if (currentTime >= SegmentLoop + updateIntrvl) {
      updtEpochOffln();
      setTimeString();
      int elpsdMins = segment * updateIntrvl / 60000;
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/Irrigation/Elapsed-Mins-" + String(elpsdMins) + timestringfornode, roundf(rtcMem.totalLitres * 100.0f) / 100.0f);
      }
      SegmentLoop = millis();
      segment++;
    }
    if (currentTime >= (cloopTime + 1000))  // Every second, calculate and print litres/hour
    {
      portENTER_CRITICAL(&mux);
      local_frequency = flow_frequency;
      portEXIT_CRITICAL(&mux);
      if (local_frequency > 5) {
        volCalc();

        if (rtcMem.totalLitres >= (segmentVol * (line + 1)) && rtcMem.totalLitres < rtcMem.VOLUME_THRESHOLD) {
          line++;
          progressChnl();
          switchChannel(rtcMem.channel);

          Serial.println("Switched to next valve.");
          updtEpochOffln();
          setTimeString();
          if (FB_GUARD()) {
            bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/Irrigation/SwitchedToValve-" + String(rtcMem.channel) + timestringfornode, roundf(rtcMem.totalLitres * 100.0f) / 100.0f);
          }
        }

        if (rtcMem.totalLitres >= rtcMem.VOLUME_THRESHOLD) {
          shutVlvs();
          updtEpochOffln();
          setTimeString();
          if (FB_GUARD()) {
            bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/Irrigation/Complete" + timestringfornode, roundf(rtcMem.totalLitres * 100.0f) / 100.0f);
          }
          Serial.println("Volume threshold for the defined period reached. Flow closed.");
          rtcMem.totalLitres = 0.0;  // Reset the total volume
          rtcMem.watered = true;
          rtcMem.WATERING_PAUSE_SEC = (24 + rtcMem.morningResumeTime - wra) * oneHourSecs - lepto * 60 - deftero + rtcMem.waterIntervalDays * 86400;
          progressChnl();
          setMaxSlTime();
          startDeepSleep();
          return;// in case it gets back here due to a return hit in startDeepSleep(), return back to the beginning of loop()
        }
      } else {
        firstVlv = rtcMem.channel;
        progressChnl();  // try with the next valve
        openvalve();
        delay(1000);
        portENTER_CRITICAL(&mux);
        local_frequency = flow_frequency;
        portEXIT_CRITICAL(&mux);
        if (local_frequency <= 20)  // if there is still no flow, close the valve and record the finding in FB
        {
          Serial.println(" No more flow, closing valve");
          Serial.print(flow_frequency);
          shutVlvs();
          progressChnl();
          updtEpochOffln();
          setTimeString();
          if (FB_GUARD()) {
            bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/Irrigation/FlowStopped" + timestringfornode, roundf(rtcMem.totalLitres * 100.0f) / 100.0f);
          }
          setMaxSlTime();
          startDeepSleep();
          return;// in case it gets back here due to a return hit in startDeepSleep(), return back to the beginning of loop()
        } else {
          if (FB_GUARD()) {
            bool ok = Firebase.RTDB.setInt(&fbdo, datestring + "/FaultyValve" + timestringfornode, firstVlv);
          }
        }
      }
      cloopTime = millis();
    }
  }
}


void startDeepSleep() {

  if (rtcMem.includeCharging && wra >= 11 && wra < 15 && !rtcMem.charged) {
    charge();
    if (rtcMem.mode && !snoozed) {
      coldBoot = false;
      return;  // in mode 1, after the ~1 hour charge, go back to loop for the next hourly irrigation attempt.
    } else {
      updtEpochOffln();
      setMaxSlTime();
    }
  }

  if (coldBoot && !rtcMem.watered) {
    rtcMem.WATERING_PAUSE_SEC = 0;  // As the system has just been switched on, ensure pause time is zero, i.e. if there is external trigger, it runs a watering cycle
  }
  int pauseMins = rtcMem.WATERING_PAUSE_SEC / 60;
  int sleepMins = rtcMem.SLEEP_TIME_SEC / 60;
  int sleepSecs = rtcMem.SLEEP_TIME_SEC;
  int adjSleepSecs = sleepSecs * 1.025 - connDelay + 5;  //adjustment to manage hardware sleep duration inaccuracies and connection delays

  if (!rtcMem.mode) {
    rtc_gpio_init(WAKE_GPIO);
    rtc_gpio_set_direction(WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(WAKE_GPIO);
    rtc_gpio_pulldown_en(WAKE_GPIO);
    if (rtc_gpio_get_level(WAKE_GPIO) == 0) {  // if LOW when going to sleep
      rtcMem.wakePinAtSleep = false;
      esp_sleep_enable_ext0_wakeup(WAKE_GPIO, 1);  // Wake on HIGH
    } else {
      if (justCharged && !rtcMem.watered && !snoozed) {
        // In case water pressure went high during charging and remains high, don't go to deep sleep but return to attempt irrigation
        justCharged = false;
        return;
      }
      // Wake on LOW
      rtcMem.wakePinAtSleep = true;
      esp_sleep_enable_ext0_wakeup(WAKE_GPIO, 0);  // if HIGH when going to sleep, wake on LOW, just to log in when when water was shut off.
    }
  }

  esp_sleep_enable_timer_wakeup((adjSleepSecs)*1000000ULL);
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setInt(&fbdo, datestring + "/WentToSleep" + timestringfornode + "/wateringPauseForMins", pauseMins);
    bool ok1 = Firebase.RTDB.setInt(&fbdo, datestring + "/WentToSleep" + timestringfornode + "/toWakeUpInMins", sleepMins);
  }
  if (!rtcMem.mode && FB_GUARD()) {
    bool ok2 = Firebase.RTDB.setBool(&fbdo, datestring + "/WentToSleep" + timestringfornode + "/Pressure", rtcMem.wakePinAtSleep);
  }
  Serial.println(rtcMem.SLEEP_TIME_SEC);
  // digitalWrite(fiveVltSwitch, HIGH);//switch 5v OFF
  pinMode(fiveVltSwitch, INPUT);  // ensure pin is floating during deepsleep so that 5v is disconnected

  updtEpochOffln();
  unsigned long leftoverMillis = millis() - rstMillis;
  rtcMem.us_remainder = leftoverMillis * 1000;
  rtcMem.sleepStartEpoch = rtcMem.epochTime;
  disconnectWifi();
  digitalWrite(USB, HIGH);
  rtc_gpio_pulldown_dis(WAKE_GPIO);
  Serial.print("WAKE_GPIO level = ");
  Serial.println(rtc_gpio_get_level(WAKE_GPIO));
  digitalWrite(USB, HIGH);
  Serial.println("Going to deep sleep...");
  delay(200);
  rtc_gpio_pullup_dis(WAKE_GPIO);
  rtc_gpio_pulldown_en(WAKE_GPIO);
  rtcMem.internalRtcTimeAtSleep = esp_rtc_get_time_us();
  esp_deep_sleep_start();
}


uint64_t getEpochFromDS3231() {

  Wire.beginTransmission(0x68);
  if (Wire.endTransmission() != 0) {
    Serial.println("RTC not detected on I2C bus.");
    return 0;
  }

  if (rtc.lostPower()) {
    return 0;
    Serial.println("RTC lost power, time invalid.");
  }

  DateTime now = rtc.now();

  if (now.year() < 2026 || now.year() > 2035) {
    Serial.println("RTC returned garbage data.");
    return 0;
  }
  return static_cast<uint64_t>(now.unixtime());  // Seconds since 1970-01-01 00:00:00 UTC
}

void synchTimeExtRTC() {
  uint64_t epoch = getEpochFromDS3231();
  // If we have internet AND 7 days have passed, get server time
  if (FB_GUARD() && (rtcMem.epochTime >= rtcMem.lastSrvrEpoch + 604800 || epoch == 0)) getSrvrTime();
  // Otherwise, if the RTC is working, trust it completely
  else if (epoch > 0) {
    rtcMem.epochTime = epoch;
    timeOriginCode = "";
    rstMillis = millis();
    rtcMem.us_remainder = 0;
  }
  // Last resort (no RTC, no internet): rely on internal offline tracking
  else {
    updtEpochOffln();
    timeOriginCode = "---";
  }
  // Rely purely on the updated rtcMem.epochTime

  setTimeString();
}

void switchChannel(int chnl) {
  // Calculate the binary representation of the channel number
  digitalWrite(A_PIN, bitRead(chnl, 0));
  digitalWrite(B_PIN, bitRead(chnl, 1));
  digitalWrite(C_PIN, bitRead(chnl, 2));

  delay(1);  // Short delay to ensure the signal stabilizes
}

void chkUpdtSettings() {
  if (FB_GUARD()) {
    if (Firebase.RTDB.getBool(&fbdo, "/Settings/updateSettings")) rtcMem.chkAgainNeedToUpdt = false;
    else rtcMem.chkAgainNeedToUpdt = true;
  }

  if (fbdo.dataType() == "boolean") {
    rtcMem.updateSettings = fbdo.boolData();
  }
}

void getSettings() {
  if (FB_GUARD()) {
    if (Firebase.RTDB.getBool(&fbdo, "/Settings/mode")) {
      rtcMem.updateSettings = false;
      rtcMem.chkAgainNeedToUpdt = false;
      if (fbdo.dataType() == "boolean") {
        rtcMem.mode = fbdo.boolData();
      }
    }
    if (Firebase.RTDB.getInt(&fbdo, "/Settings/VOLUME_THRESHOLD")) {
      if (fbdo.dataType() == "int") {
        rtcMem.VOLUME_THRESHOLD = fbdo.intData();
      }
    }
    if (Firebase.RTDB.getInt(&fbdo, "/Settings/NUM_OUTPUTS")) {
      if (fbdo.dataType() == "int") {
        rtcMem.NUM_OUTPUTS = fbdo.intData();
      }
    }


    if (Firebase.RTDB.getInt(&fbdo, "/Settings/waterIntervalDays")) {
      if (fbdo.dataType() == "int") {
        rtcMem.waterIntervalDays = fbdo.intData();
      }
    }


    if (Firebase.RTDB.getInt(&fbdo, "/Settings/morningResumeTime")) {
      if (fbdo.dataType() == "int") {
        rtcMem.morningResumeTime = fbdo.intData();
      }
    }


    if (Firebase.RTDB.getInt(&fbdo, "/Settings/eveningPauseTime")) {

      if (fbdo.dataType() == "int") {
        rtcMem.eveningPauseTime = fbdo.intData();
      }
    }

    if (Firebase.RTDB.getBool(&fbdo, "/Settings/includeCharging")) {
      if (fbdo.dataType() == "boolean") {
        rtcMem.includeCharging = fbdo.boolData();
      }
    }

    if (Firebase.RTDB.getBool(&fbdo, "/Settings/sub_mode_timed")) {
      if (fbdo.dataType() == "boolean") {
        rtcMem.sub_mode_timed = fbdo.boolData();
      }
    }
  }

  if (FB_GUARD()) {
    bool ok00 = Firebase.RTDB.setBool(&fbdo, "/Settings/updateSettings", rtcMem.updateSettings);  //set the setting update trigger back to False
    bool ok10 = Firebase.RTDB.setInt(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/morningResumeTime", rtcMem.morningResumeTime);
    bool ok11 = Firebase.RTDB.setInt(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/VOLUME_THRESHOLD", rtcMem.VOLUME_THRESHOLD);
    bool ok12 = Firebase.RTDB.setBool(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/updateSettings", rtcMem.updateSettings);
    bool ok13 = Firebase.RTDB.setBool(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/sub_mode_timed", rtcMem.sub_mode_timed);
    bool ok14 = Firebase.RTDB.setInt(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/waterIntervalDays", rtcMem.waterIntervalDays);
    bool ok15 = Firebase.RTDB.setInt(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/NUM_OUTPUTS", rtcMem.NUM_OUTPUTS);
    bool ok16 = Firebase.RTDB.setBool(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/mode", rtcMem.mode);
    bool ok17 = Firebase.RTDB.setBool(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/includeCharging", rtcMem.includeCharging);
    bool ok18 = Firebase.RTDB.setInt(&fbdo, datestring + "/SettingUpdated" + timestringfornode + "/eveningPauseTime", rtcMem.eveningPauseTime);
  }
}

void charge() {
  int mins;
  unsigned long chstrt = millis();
  digitalWrite(USB, LOW);
  connectWiFi();
  updtEpochOffln();
  setTimeString();
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/Charging/ON", timeString);
  }
  disconnectWifi();
  mins = 0;
  while (mins < (oneHourSecs / 60) - 1) {
    delay(30000);
    digitalWrite(WDT, !digitalRead(WDT));  // toggle
    delay(30000);
    digitalWrite(WDT, !digitalRead(WDT));
    mins++;
    Serial.print(" ");
    Serial.print((oneHourSecs / 60) - mins);
    Serial.println(" minutes charging left.");
  }
  // esp_task_wdt_add(NULL);
  rtcMem.charged = true;
  justCharged = true;

  connectWiFi();
  FBsignup();
  delay(3000);
  updtEpochOffln();
  setTimeString();
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/Charging/off", timeString);

    if (!ok) {
      FBsignup();
      delay(2000);
      if (FB_GUARD()) {
        Firebase.RTDB.setString(&fbdo, datestring + "/Charging_OFF", timeString + "-");
      }
    }
  }
  digitalWrite(USB, HIGH);
  uint32_t chrgSecs = (millis() - chstrt) / 1000;
  rtcMem.WATERING_PAUSE_SEC = rtcMem.WATERING_PAUSE_SEC > chrgSecs ? rtcMem.WATERING_PAUSE_SEC - chrgSecs : 0;
}

void connectWiFi() {
  digitalWrite(WDT, !digitalRead(WDT));  // toggle
  if (!offlineMode) {
    digitalWrite(USB, LOW);
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(1);
      WiFi.mode(WIFI_STA);
      delay(0.6 * 1000 * connDelay);
      digitalWrite(WDT, !digitalRead(WDT));
      delay(0.4 * 1000 * connDelay);
      startTime = millis();
      WiFi.begin(ssid, password);
      Serial.print("Connecting to Wi-Fi");
      while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
        Serial.print(".");
        delay(300);
      }
      // WiFi.config(staticIP, gateway, subnet);
      Serial.println();
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Connected");
        delay(1000);
        Serial.print(" with IP: ");
        Serial.println(WiFi.localIP());
        Serial.println();
      } else {
        Serial.print("Not connected");
        offlineMode = true;
      }
    }
  }
}
void disconnectWifi() {
  if (WiFi.status() == WL_CONNECTED) {  // wifi connection is active
    // ditch the existing connection
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    // allow some time for the connection to be fully dropped, **important!**
    delay(100);
    // check if the connection was really dropped
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Connection is still alive...");
    } else {
      Serial.println("Connection successfully terminated.");
    }
  } else {  // there was no wifi connection to begin with, so nothing to disconnect
    Serial.println("No active connection was found to be terminated.");
  }
}
void recordVoltage() {
  connectWiFi();
  digitalWrite(divider, HIGH);
  delay(100);
  adcValue = analogRead(voltRead);  // Read the Analog Input value
  delay(100);
  digitalWrite(divider, LOW);
  float reading = adcValue * 15.672 / 4095;
  volt = roundf(reading * 100.0f) / 100.0f;
  Serial.print("Voltage: ");
  Serial.println(volt);
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setFloat(&fbdo, datestring + "/batteryvoltage/" + timestringfornode, volt);
  }
}

void volCalc() {
  digitalWrite(WDT, !digitalRead(WDT));  // toggle
  portENTER_CRITICAL(&mux);
  uint32_t pulses = flow_frequency;
  flow_frequency = 0;  // Reset Counter
  portEXIT_CRITICAL(&mux);
  flowRate = ((1000.0 / (millis() - cloopTime)) * pulses) / 6.8;  //
  flowLitres = (flowRate / 60);
  rtcMem.totalLitres += flowLitres;
  Serial.print(pulses);

  Serial.print(" Flow rate: ");
  Serial.print(float(flowRate));
  Serial.print("L/min");
  Serial.print("\t");
  Serial.print("Total volume: ");
  Serial.print(rtcMem.totalLitres);
  Serial.println(" L");
}

void getSrvrTime() {
  currentEpoch = 0;
  bool timeFBupdated;
  connectWiFi();
  if (!syncEpochNPT()) {
    FirebaseJson json;
    json.set(".sv", "timestamp");

    if (Firebase.RTDB.setJSON(&fbdo, "/epochTime", &json)) {}


    delay(1000);

    uint64_t currentEpochMillis = getEpochTimeFromFirebase();
    // if (fbdo.dataType() == "int")
    // {

    Serial.print("currentEpochMillis: ");
    Serial.println(currentEpochMillis);
    currentEpoch = currentEpochMillis / 1000;
    timeOriginCode = "--";
  } else {
    timeOriginCode = "-";
  }
  Serial.print("currentEpoch: ");
  Serial.println(currentEpoch);
  // }

  if (currentEpoch > rtcMem.lastSrvrEpoch) {

    rtcMem.epochTime = currentEpoch;
    rtcMem.lastSrvrEpoch = currentEpoch;
    timeFBupdated = true;
    rstMillis = millis();
    rtcMem.us_remainder = 0;

    Serial.print("LastSrvrEpoch766: ");
    Serial.println(rtcMem.lastSrvrEpoch);
  } else {
    updtEpochOffln();
    timeOriginCode = "---";
    timeFBupdated = false;
  }

  DateTime rtcNow = rtc.now();
  uint64_t rtcEpoch = rtcNow.unixtime();

  int64_t drift = rtcMem.epochTime - rtcEpoch;
  Serial.print("drift: ");
  Serial.println(drift);
  uint64_t absDrift = abs(drift);
  if (absDrift > 10 && timeFBupdated) {
    DateTime dt(rtcMem.epochTime);  // RTClib constructor from Unix time
    rtc.adjust(dt);
    timeFBupdated = !timeFBupdated;
  }
}

// Function to get large epoch time from Firebase
uint64_t getEpochTimeFromFirebase() {
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.getString(&fbdo, "/epochTime");
    if (!ok) return 0;  // Return 0 if the retrieval fails
  }


  // Use strtoull to safely convert to uint64_t
  return strtoull(fbdo.stringData().c_str(), NULL, 10);
}
void setTimeString() {
  time_t standardTime = rtcMem.epochTime;  // + 10800;
  Serial.print("standardTime: ");
  Serial.println(standardTime);
  // Convert time_t to tm structure
  struct tm* timeInfo;
  timeInfo = localtime(&standardTime);

  // Access individual components of the time
  int year = timeInfo->tm_year + 1900;  // Year
  int month = timeInfo->tm_mon + 1;     // Month (0-11)
  int day = timeInfo->tm_mday;          // Day of the month
  int hour = timeInfo->tm_hour;         // Hour (0-23)
  int minute = timeInfo->tm_min;        // Minute (0-59)
  int second = timeInfo->tm_sec;        // Second (0-59)}
  wra = hour;
  lepto = minute;
  deftero = second;
  mera = day;
  xronos = year;
  timeString = String(hour) + ":" + String(minute) + ":" + String(second);
  timestringfornode = "/" + String(hour) + "-" + String(minute) + "-" + String(second) + timeOriginCode;
  datestring = "/" + String(year) + "-" + String(month) + "-" + String(day);
  Serial.println(timeString);
  Serial.println(datestring);
}

void openvalve() {
  switchChannel(rtcMem.channel);  //open first solenoid vlv
  digitalWrite(muxINH, LOW);
}

void shutVlvs() {
  digitalWrite(mainVlv, HIGH);
  delay(valveDelay);  //wait ballvalve to shut before closing magnetic ones, to avoid damage
  digitalWrite(muxINH, HIGH);
}

void progressChnl() {
  rtcMem.channel = rtcMem.channel < (rtcMem.NUM_OUTPUTS - 1) ? rtcMem.channel + 1 : 0;
}

void setMaxSlTime() {
  rtcMem.SLEEP_TIME_SEC = (wra < rtcMem.morningResumeTime - 1) ? oneHourSecs * (rtcMem.morningResumeTime - wra) - lepto * 60 - deftero : oneHourSecs * (24 - wra + rtcMem.morningResumeTime) - lepto * 60 - deftero;

  if (rtcMem.mode && !rtcMem.watered && !snoozed) {
    if (wra >= rtcMem.morningResumeTime - 1 && wra < rtcMem.eveningPauseTime) {
      rtcMem.SLEEP_TIME_SEC = oneHourSecs;
    }
  } else adaptSleepForcharge();
}

void updtEpochOffln() {
  unsigned long currentMillis = millis();
  unsigned long elpsdMillis = currentMillis - rstMillis;
  rtcMem.epochTime += elpsdMillis / 1000;
  rstMillis = currentMillis - (elpsdMillis % 1000);
  setTimeString();
}

bool syncEpochNPT() {
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", ntpServer);
  //configTime(0, 0, ntpServer);

  time_t now = 0;
  uint32_t start = millis();

  while (millis() - start < 10000) {
    time(&now);
    if (now > 1768772000) {
      Serial.print("NTP success: ");
      Serial.println(now);
      currentEpoch = now;
      return true;
    }
    delay(100);
  }
  return false;
}

void adaptSleepForcharge() {
  if (rtcMem.includeCharging && !rtcMem.charged) {
    unsigned long distanceFromMidday = (wra < 12) ? oneHourSecs * (12 - wra) - lepto * 60 - deftero : oneHourSecs * (24 - wra + 12) - lepto * 60 - deftero;

    rtcMem.SLEEP_TIME_SEC = rtcMem.SLEEP_TIME_SEC > distanceFromMidday ? distanceFromMidday : rtcMem.SLEEP_TIME_SEC;
  }
}

void FBsignup() {
  digitalWrite(WDT, !digitalRead(WDT));

  WiFi.persistent(false);
  connectWiFi();
  startTime = millis();
  while ((!networkReady()) && millis() - startTime < 20000) {
    Serial.print(":");
    delay(500);
  }
  digitalWrite(WDT, !digitalRead(WDT));
  if (networkReady()) {// to avoid any attenmpt if there is no internet connection with as the system could then hang indefinitely
    Serial.println("Firebase signup...");
    /* Assign the api key */
    config.api_key = API_KEY;

    /* Assign the RTDB URL */
    config.database_url = DATABASE_URL;
    /* Sign up */
    if (Firebase.signUp(&config, &auth, "", "")) {
      Serial.println("Firebase Success");
    } else {
      Serial.printf("%s\n", config.signer.signupError.message.c_str());
      ESP.restart();// Restart system to hopefully avoid hanging in an endless loop in case FB signup fails
    }

    /* Assign the callback function for the long running token generation task */
    config.token_status_callback = tokenStatusCallback;  //see addons/TokenHelper.h
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
  } else offlineMode = true;
}

bool networkReady() {
  // First, check the physical/local WiFi link
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClient client;
  // Keep the timeout tight. 1.5 seconds per attempt means a maximum
  // delay of 3 seconds if the internet is completely dead.
  client.setTimeout(1500);

  // Attempt 1: Cloudflare
  if (client.connect("1.1.1.1", 80)) {
    client.stop();
    return true;
  }

  // Attempt 2: Google (Fallback)
  // If we reach here, Cloudflare failed. Let's try our backup.
  if (client.connect("8.8.8.8", 80)) {
    client.stop();
    return true;
  }

  // If we reach here, both TCP attempts failed.
  // It is highly safe to assume the WAN connection is actually dead.
  return false;
}

void validateStructIntegrity() {
  // On cold boot, initialize magic
  if (coldBoot) {
    rtcMem.magic = RTC_MAGIC;
    Serial.println("Cold boot: Magic initialized");
  }

  // On warm boot, check magic
  if (rtcMem.magic != RTC_MAGIC) {
    Serial.println("❌ RTC STRUCT CORRUPTED - Magic mismatch!");
    Serial.print("   Expected: 0x");
    Serial.println(RTC_MAGIC, HEX);
    Serial.print("   Got: 0x");
    Serial.println(rtcMem.magic, HEX);

    // Reset to defaults
    initializeRTCDefaults();
    rtcMem.magic = RTC_MAGIC;

    // Force Firebase settings download
    coldBoot = true;
    return;
  }

  Serial.println("✓ RTC struct magic OK");
}

void initializeRTCDefaults() {
  rtcMem.epochTime = 1768771235;
  rtcMem.lastSrvrEpoch = 0;
  rtcMem.VOLUME_THRESHOLD = 400;
  rtcMem.morningResumeTime = 6;
  rtcMem.eveningPauseTime = 18;
  rtcMem.oldDay = 0;
  rtcMem.totalLitres = 0;
  rtcMem.charged = false;
  rtcMem.includeCharging = true;
  rtcMem.sleepStartEpoch = 0;
  rtcMem.channel = 0;
  rtcMem.mode = false;
  rtcMem.updateSettings = true;
  rtcMem.NUM_OUTPUTS = 2;
  rtcMem.waterIntervalDays = 0;
  rtcMem.watered = false;
  rtcMem.chkAgainNeedToUpdt = true;
}

void timeIrrigation() {
  int mins;
  int line = 0;
  int segment = 1;
  unsigned long irrstrt = millis();
  int irrigationTotMins = round((float)rtcMem.VOLUME_THRESHOLD / 10.0);  // Assuming 10L/minute flow to estimate needed time based on volume threshold
  int segmentMins = round((float)irrigationTotMins / (float)rtcMem.NUM_OUTPUTS);

  connectWiFi();
  updtEpochOffln();
  setTimeString();
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/Irrigation/Timer_ON_valve-" + String(rtcMem.channel), timeString);
  }
  if (segmentMins > 5) disconnectWifi();
  mins = 0;
  while (mins < irrigationTotMins) {
    if (mins >= (segmentMins * (line + 1))) {
      line++;
      progressChnl();
      switchChannel(rtcMem.channel);

      Serial.println("Switched to next valve.");
      updtEpochOffln();
      setTimeString();
      connectWiFi();
      FBsignup();
      if (FB_GUARD()) {
        bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/Irrigation/switchedToValve-" + String(rtcMem.channel), timeString);
      }
      if (mins < irrigationTotMins - 5) disconnectWifi();
    }
    delay(30000);
    digitalWrite(WDT, !digitalRead(WDT));  // toggle
    delay(30000);
    digitalWrite(WDT, !digitalRead(WDT));
    mins++;
    Serial.print(" ");
    Serial.print(irrigationTotMins - mins);
    Serial.println(" minutes watering left.");
  }
  rtcMem.watered = true;
  shutVlvs();
  connectWiFi();
  FBsignup();
  delay(3000);
  updtEpochOffln();
  setTimeString();
  if (FB_GUARD()) {
    bool ok = Firebase.RTDB.setString(&fbdo, datestring + "/Irrigation/timeEnded", timeString);
  }
  rtcMem.watered = true;
  rtcMem.WATERING_PAUSE_SEC = (24 + rtcMem.morningResumeTime - wra) * oneHourSecs - lepto * 60 - deftero + rtcMem.waterIntervalDays * 86400;
  progressChnl();
  setMaxSlTime();
  startDeepSleep();
}

