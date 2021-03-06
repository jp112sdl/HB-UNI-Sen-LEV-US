//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2018-04-16 jp112sdl Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2020-03-12 Wolfram Winter Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//- -----------------------------------------------------------------------------------------------------------------------
// ci-test=yes board=328p aes=no

// define this to read the device id, serial and device type from bootloader section
// #define USE_OTA_BOOTLOADER

#define USE_TEMPERATURE_COMPENSATION // connect DS18B20 Sensor to Pin 9
#define SENSOR_ONLY

#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>

#include <Register.h>
#include <MultiChannelDevice.h>
#ifdef USE_TEMPERATURE_COMPENSATION
#include <OneWire.h>
#include <sensors/Ds18b20.h>
#endif

// Arduino Pro mini 8 Mhz
// Arduino pin for the config button
#define CONFIG_BUTTON_PIN  8
#define LED_PIN            4

#define SENSOR_EN_PIN      5 //VCC Pin des Sensors
#define SENSOR_ECHO_PIN    6
#define SENSOR_TRIG_PIN    14 //A0
#define BATT_EN_PIN        15
#define BATT_SENS_PIN      17
#define DS18B20_PIN        9

#ifdef USE_TEMPERATURE_COMPENSATION
//DS18B20 Sensors connected to pin
OneWire oneWire(DS18B20_PIN);
#endif

// number of available peers per channel
#define PEERS_PER_CHANNEL 2

// all library classes are placed in the namespace 'as'
using namespace as;

//Korrekturfaktor der Clock-Ungenauigkeit, wenn keine RTC verwendet wird
#define SYSCLOCK_FACTOR    0.88
#define MAX_MEASURE_COUNT  5

enum UltrasonicSensorTypes {
  JSN_SR04T_US100,
  MAXSONAR
};

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
  {0xF9, 0xD2, 0x01},          // Device ID
  "JPLEV00001",                // Device Serial
  {0xF9, 0xD2},                // Device Model
#ifdef USE_TEMPERATURE_COMPENSATION
  0x12,                        // Firmware Version
#else
  0x11,
#endif
  0x53,                        // Device Type
  {0x01, 0x01}                 // Info Bytes
};

/**
   Configure the used hardware
*/
typedef AskSin<StatusLed<LED_PIN>, BatterySensorUni<BATT_SENS_PIN, BATT_EN_PIN, 0>, Radio<AvrSPI<10, 11, 12, 13>, 2>> BaseHal;
class Hal : public BaseHal {
  public:
    void init (const HMID& id) {
      BaseHal::init(id);
      battery.init(seconds2ticks(60UL * 60) * SYSCLOCK_FACTOR, sysclock); //battery measure once an hour
      battery.low(22);
      battery.critical(20);
    }

    bool runready () {
      return sysclock.runready() || BaseHal::runready();
    }
} hal;


DEFREGISTER(UReg0, MASTERID_REGS, DREG_LOWBATLIMIT, 0x20, 0x21)
class UList0 : public RegList0<UReg0> {
  public:
    UList0 (uint16_t addr) : RegList0<UReg0>(addr) {}

    bool Sendeintervall (uint16_t value) const {
      return this->writeRegister(0x20, (value >> 8) & 0xff) && this->writeRegister(0x21, value & 0xff);
    }
    uint16_t Sendeintervall () const {
      return (this->readRegister(0x20, 0) << 8) + this->readRegister(0x21, 0);
    }

    void defaults () {
      clear();
      lowBatLimit(22);
      Sendeintervall(180);
    }
};

DEFREGISTER(UReg1, CREG_CASE_HIGH, CREG_CASE_WIDTH, CREG_CASE_DESIGN, CREG_CASE_LENGTH, 0x01, 0x02, 0x03)
class UList1 : public RegList1<UReg1> {
  public:
    UList1 (uint16_t addr) : RegList1<UReg1>(addr) {}

    bool distanceOffset (uint16_t value) const {
      return this->writeRegister(0x01, (value >> 8) & 0xff) && this->writeRegister(0x02, value & 0xff);
    }
    uint16_t distanceOffset () const {
      return (this->readRegister(0x01, 0) << 8) + this->readRegister(0x02, 0);
    }

    bool sensorType (uint16_t value) const {
      return this->writeRegister(0x03, value & 0xff);
    }
    uint16_t sensorType () const {
      return this->readRegister(0x03, 0);
    }

    void defaults () {
      clear();
      caseHigh(100);
      caseWidth(100);
      caseLength(100);
      caseDesign(0);
      distanceOffset(0);
      sensorType(0);
    }
};

class MeasureEventMsg : public Message {
  public:
    void init(uint8_t msgcnt, uint8_t percent, uint32_t liter, uint16_t height, uint8_t volt, int16_t temp) {
      // Message Length (first byte param.): 11 + payload = 17 = 0x11
      uint8_t msg_len = 0x11;
#ifdef USE_TEMPERATURE_COMPENSATION
      msg_len += 0x03; // three extra bytes for the temperature channel
#endif
      Message::init(msg_len, msgcnt, 0x53, BIDI | WKMEUP, percent & 0xff, volt & 0xff);
      pload[0] = (liter >>  24) & 0xff;
      pload[1] = (liter >>  16) & 0xff;
      pload[2] = (liter >>  8) & 0xff;
      pload[3] = liter & 0xff;
      pload[4] = (height >> 8) & 0xff;
      pload[5] = height & 0xff;
#ifdef USE_TEMPERATURE_COMPENSATION
      pload[6] = 0x42;
      pload[7] = (temp >> 8) & 0xff;
      pload[8] = temp & 0xff;
#endif
    }
};

class MeasureChannel : public Channel<Hal, UList1, EmptyList, List4, PEERS_PER_CHANNEL, UList0>, public Alarm {
    MeasureEventMsg msg;
    uint8_t  fillingPercent;
    uint32_t fillingLiter;
    uint16_t fillingHeight;
    uint16_t distance;
    int16_t temperature;
    uint8_t sensorCount;

    uint8_t last_flags = 0xff;

#ifdef USE_TEMPERATURE_COMPENSATION
    Ds18b20 tempSensors[1];
#endif

  public:
    MeasureChannel () : Channel(), Alarm(0), fillingPercent(0), fillingLiter(0), fillingHeight(0), distance(0), temperature(-600), sensorCount(0) {}
    virtual ~MeasureChannel () {}

    void measure() {
      uint32_t caseHeight = this->getList1().caseHigh();
      uint32_t caseWidth = this->getList1().caseWidth();
      uint32_t caseLength = this->getList1().caseLength();
      uint32_t caseDesign  = this->getList1().caseDesign();
      uint32_t distanceOffset = this->getList1().distanceOffset();

      uint32_t m_value = 0;
      uint32_t duration = 0;
      uint8_t validcnt = 0;


      pinMode(SENSOR_ECHO_PIN, INPUT_PULLUP);
      _delay_ms(300);
      digitalWrite(SENSOR_EN_PIN, HIGH);
      _delay_ms(300);
      switch (this->getList1().sensorType()) {
        case JSN_SR04T_US100:
        {
          digitalWrite(SENSOR_TRIG_PIN, LOW);
          delayMicroseconds(2);
          digitalWrite(SENSOR_TRIG_PIN, HIGH);
          delayMicroseconds(10);
          digitalWrite(SENSOR_TRIG_PIN, LOW);

          duration = pulseIn(SENSOR_ECHO_PIN, HIGH);

          validcnt++;
        }
          break;
        case MAXSONAR:
        {
          pulseIn(SENSOR_ECHO_PIN, HIGH);
          _delay_ms(100);
          for (uint8_t i = 0; i < MAX_MEASURE_COUNT; i++) {
            uint16_t p =  pulseIn(SENSOR_ECHO_PIN, HIGH);
            if (p < 35750) {
              duration += p;
              validcnt++;
            } else {
              DPRINTLN("Invalid range detected!");
            }
            _delay_ms(100);
          }

        }
          break;
        default:
          DPRINTLN(F("Invalid Sensor Type selected"));
          break;
      }

      digitalWrite(SENSOR_EN_PIN, LOW);
      pinMode(SENSOR_ECHO_PIN, INPUT);

      duration = (validcnt > 0) ? duration / validcnt : 0;

      m_value = (duration * 1000L / 57874L);

      DPRINT("DIST UNCOMP = "); DDECLN(m_value);
#ifdef USE_TEMPERATURE_COMPENSATION
      if (sensorCount > 0) {
        Ds18b20::measure(tempSensors, sensorCount);
        temperature = tempSensors[0].temperature();
        DPRINT("TEMPERATURE = ");DDECLN(temperature);
        uint32_t speedOfSound = 3313L + (606L * temperature) / 1000L;
        uint32_t compensatedDistance = ((duration * 100000L / 20000) * speedOfSound) / 1000000L;
        DPRINT("DIST   COMP = "); DDECLN(compensatedDistance);
        m_value = compensatedDistance;
      }
#endif
      DPRINTLN("");
      DPRINT(F("Abstand gemessen         : ")); DDECLN(m_value);
      distance = (distanceOffset > m_value) ? 0 :  m_value - distanceOffset;
      DPRINT(F("Abstand abzgl. OFFSET    : ")); DDECLN(distance);

      DPRINT(F("Behaelterhoehe           : ")); DDECLN(caseHeight);
      fillingHeight = (distance > caseHeight) ? 0 : caseHeight - distance;
      DPRINT(F("Fuellhoehe               : ")); DDECLN(fillingHeight);

      uint32_t caseVolume = 0; float r = 0;
      switch (caseDesign) {
        case 0:
          caseVolume = (PI * pow((caseWidth >> 1), 2) * caseHeight) / 1000L;
          fillingLiter = (PI * pow((caseWidth >> 1), 2) * fillingHeight) / 1000L;
          break;
        case 1:
          caseVolume = (PI * pow((caseHeight >> 1), 2) * caseWidth) / 1000L;
          //fillingLiter = pow(caseHeight >> 1, 2) * caseWidth * (acos((caseHeight >> 1 - fillingHeight) / caseHeight >> 1) - (caseHeight >> 1 - fillingHeight) * (sqrt((2 * caseHeight >> 1 * fillingHeight) - pow(fillingHeight, 2)) / pow(caseHeight >> 1, 2)))  ;
          r = caseHeight  / 2;
          fillingLiter = (r * r * 2 * acos(1 - fillingHeight / r) / 2 - 2 * sqrt(caseHeight * fillingHeight - fillingHeight * fillingHeight) * (r - fillingHeight) / 2) * caseWidth / 1000L;
          break;
        case 2:
          caseVolume = (caseHeight * caseWidth * caseLength) / 1000L;
          fillingLiter = (fillingHeight * caseWidth * caseLength) / 1000L;
          break;
        default:
          DPRINTLN(F("Invalid caseDesign")); DDECLN(caseDesign);
          break;
      }

      fillingPercent = (fillingLiter * 100) / caseVolume;
      DPRINT(F("Behaeltervolumen (gesamt): ")); DDECLN(caseVolume);
      DPRINT(F("Inhalt                   : ")); DDEC(fillingLiter); DPRINT(F("L (")); DDEC(fillingPercent); DPRINTLN(F("%)"));
      DPRINTLN("");
    }
    virtual void trigger (__attribute__ ((unused)) AlarmClock & clock) {
      uint8_t msgcnt = device().nextcount();
      if (last_flags != flags()) {
        this->changed(true);
        last_flags = flags();
      }
      measure();
      tick = delay();
      msg.init(msgcnt, fillingPercent, fillingLiter, fillingHeight, device().battery().current(), temperature);
      if (msgcnt % 20 == 1) device().sendPeerEvent(msg, *this); else device().broadcastEvent(msg);
      sysclock.add(*this);
    }

    uint32_t delay () {
      uint16_t _txMindelay = 20;
      _txMindelay = device().getList0().Sendeintervall();
      if (_txMindelay == 0) _txMindelay = 20;
      return seconds2ticks(_txMindelay  * SYSCLOCK_FACTOR);
    }

    void configChanged() {
      DPRINTLN(F("Config changed List1"));
      DPRINT(F("*CASE_HIGH:       "));
      DDECLN(this->getList1().caseHigh());
      DPRINT(F("*CASE_WIDTH:      "));
      DDECLN(this->getList1().caseWidth());
      DPRINT(F("*CASE_LENGTH:     "));
      DDECLN(this->getList1().caseLength());
      DPRINT(F("*CASE_DESIGN:     "));
      DDECLN(this->getList1().caseDesign());
      DPRINT(F("*DISTANCE_OFFSET: "));
      DDECLN(this->getList1().distanceOffset());
      DPRINT(F("*SENSOR_TYPE:     "));
      DDECLN(this->getList1().sensorType());
    }

    void setup(Device<Hal, UList0>* dev, uint8_t number, uint16_t addr) {
      Channel::setup(dev, number, addr);
      // pinMode(SENSOR_ECHO_PIN, INPUT_PULLUP);
      pinMode(SENSOR_ECHO_PIN, INPUT);
      pinMode(SENSOR_TRIG_PIN, OUTPUT);
      pinMode(SENSOR_EN_PIN, OUTPUT);

#ifdef USE_TEMPERATURE_COMPENSATION
      sensorCount = Ds18b20::init(oneWire, tempSensors, 1);
      DPRINT("Found "); DDEC(sensorCount); DPRINTLN(" DS18B20 Sensor(s)");
#endif
      sysclock.add(*this);
    }

    uint8_t status () const {
      return 0;
    }

    uint8_t flags () const {
      uint8_t flags = 0x00;

#ifdef USE_TEMPERATURE_COMPENSATION
      if (sensorCount == 0) flags = 0x01 << 1;
#endif

      flags |= this->device().battery().low() ? 0x80 : 0x00;
      return flags;
    }
};

class UType : public MultiChannelDevice<Hal, MeasureChannel, 1, UList0> {
  public:
    typedef MultiChannelDevice<Hal, MeasureChannel, 1, UList0> TSDevice;
    UType(const DeviceInfo& info, uint16_t addr) : TSDevice(info, addr) {}
    virtual ~UType () {}

    virtual void configChanged () {
      TSDevice::configChanged();
      DPRINT(F("*LOW BAT Limit: "));
      DDECLN(this->getList0().lowBatLimit());
      this->battery().low(this->getList0().lowBatLimit());
      DPRINT(F("*Sendeintervall: ")); DDECLN(this->getList0().Sendeintervall());
    }
};

UType sdev(devinfo, 0x20);
ConfigButton<UType> cfgBtn(sdev);

void setup () {
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
  DDEVINFO(sdev);
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if ( worked == false && poll == false ) {
    if ( hal.battery.critical() ) {
      hal.activity.sleepForever(hal);
    }
    hal.activity.savePower<Sleep<>>(hal);
  }
}
