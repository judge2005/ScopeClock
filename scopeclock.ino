#include "Arduino.h"

#include "freertos/task.h"			// For FreeRTOS task functions
#include "soc/sens_reg.h"			// For my own DAC code

// Standard ESP Arduino libraries
#include "ESPmDNS.h"				// Implementation of mDNS
#include "WiFi.h"
#include "WiFiUdp.h"

// Use these in preference to default ESP Arduino classes
#include "AsyncTCP.h"				// https://github.com/me-no-dev/ESPAsyncTCP
#include "ESPAsyncWebServer.h"		// https://github.com/me-no-dev/ESPAsyncWebServer

// Captive portal implementation
#include "ESPAsyncWiFiManager.h"	// https://github.com/judge2005/ESPAsyncWiFiManager

#define OTA	// Create a web page to allow uploading of new firmware
#ifdef OTA
// OTA updates
#include "Update.h"					// Standard ESP Arduino library
#include "ASyncOTAWebUpdate.h"		// https://github.com/judge2005/ASyncOTAWebUpdate
#endif

// Persistent configuration framework
#include "ConfigItem.h"				// https://github.com/judge2005/ESPConfig
#include "EEPROMConfig.h"			// 				Ditto

// NTP time synchronization
#include "EspSNTPTimeSync.h"		// https://github.com/judge2005/TimeSync

#include "GPIOButton.h"
#include "MovementSensor.h"

// Oscilloscope clock functions
#include "services.h"
#include "crt.h"
#include "bounce.h"
#include "pingpong.h"
#include "extents.h"
#include "parallel.h"
#include "grat.h"
#include "binaryclk.h"
#include "bcdclk.h"
#include "digital.h"
#include "barcode.h"
#include "klingon.h"
#include "ascope.h"
#include "flw.h"

//#define INTERNAL_SYNC
#define MY_DAC
#define MODELESS_PORTAL

#define PARK_X 15
#define PARK_Y 15

// Persistent Configuration
StringConfigItem hostName("hostname", 63, "ScopeClock");
StringConfigItem timeZone("timeZone", 63, "EST5EDT,M3.2.0,M11.1.0");	// POSIX timezone format
IntConfigItem modeConfig("mode", cmExtents);

// For captive portal and other stuff
AsyncWebServer server(80);
//AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws
DNSServer dns;

AsyncWiFiManager wifiManager(&server,&dns);
bool timeInitialized = false;
void asyncTimeSetCallback(String msg);
void asyncTimeErrorCallback(String msg);

// TimeSync library abstracts out how time synchronization is done.
// This implementation uses NTP
EspSNTPTimeSync timeSync(timeZone, asyncTimeSetCallback, asyncTimeErrorCallback);

TaskHandle_t crtTask;

#ifdef OTA
ASyncOTAWebUpdate otaUpdater(Update, "update", "secretsauce");
#endif

// Global configuration
BaseConfigItem *configSetGlobal[] = {
	&hostName,
	&timeZone,
	&modeConfig,
	0
};

CompositeConfigItem globalConfig("global", 0, configSetGlobal);

// All configurations
BaseConfigItem *configSetRoot[] = {
	&globalConfig,
	0
};

CompositeConfigItem rootConfig("root", 0, configSetRoot);

// Store the configurations in EEPROM
EEPROMConfig config(rootConfig);

// Generate a meaningful, but unique SSID
String getChipId(void)
{
	uint8_t macid[6];
    esp_efuse_mac_get_default(macid);
    String chipId = String((uint32_t)(macid[5] + (((uint32_t)(macid[4])) << 8) + (((uint32_t)(macid[3])) << 16)), HEX);
    chipId.toUpperCase();
    return chipId;
}

String chipId = getChipId();
String ssid = "ScopeClock-";

const uint8_t pinX = 26;
const uint8_t pinY = 25;
const uint8_t pinTX = 18;
const uint8_t pinRelay = 16;	// Turn CRT power on (high), off (low)
const uint8_t pinTrigger = 4;	// Mains frequency square wave
const uint8_t pinToggle = 17;	// Toggle button
const uint8_t pinSlowSet = 2;	// Slow set button
const uint8_t pinFastSet = 15;	// fast set button
const uint8_t pinMystery = 5;	// ??
const uint8_t pinMov = 13;


GPIOButton frontButton(pinToggle, true);	// open == low, closed == high
GPIOButton middleButton(pinSlowSet, true);	// open == low, closed == high
GPIOButton backButton(pinFastSet, true);	// open == low, closed == high

MovementSensor mov(pinMov);

// Code thanks to Grahame http://www.sgitheach.org.uk/scope1.html

//--- Clock Mode ------------------------------------------------------------------------------------------------------

void sweepRefresh()
{
    crtSetTextInfo(1,7);
    crtATextf(8,28,"Wait...");
}

//--- clock 1 hour tick -----------------------------------------------------------------------------------------------

// jiggle crt display as anti burn measure
// check if long term clock adjustment required

void clockHourChange()
{
struct tm dt;
	datetimeConvert(false, false, &dt, 0);	// Get current local time

// every hr move the screen a little as an antiburn measure
    crtJiggle(1 + (dt.tm_hour & 3));
}

//--- clock 1 minute tick -----------------------------------------------------------------------------------------------

void clockMinuteChange()
{
// reset bounce on the minute
    bounceInit();
}

//--- automatic sweep through available/shown faces -------------------------------------------------------------------

// this is an array of pointers into the config struct, each element points at the
// show variable.  the number of elements and order must correspond to cmXXXX enum
// after cmSweep
boolean * flash shown[] =
{
    0,
    0,
    &config_use.extents_show,
    &config_use.parallel_show,
    &config_use.grat_show,
    &config_use.bounce_show,
    &config_use.analogue[0].show,
    &config_use.analogue[1].show,
    &config_use.analogue[2].show,
    &config_use.analogue[3].show,
    &config_use.binary_show,
    &config_use.bcd_show,
    &config_use.digital[0].show,
    &config_use.digital[1].show,
    &config_use.digital[2].show,
    &config_use.digital[3].show,
    &config_use.barcode_show,
    &config_use.klingon_show,
    &config_use.pingpong_show,
    &config_use.ascope_show,
    &config_use.flw_show
};


//--- Periodic updates ------------------------------------------------------------------------------------------------

//--- displays that require updates every 1 S clock tick period

// define a pointer to a function taking no paramters not returning anything
typedef void(*TUpdate)();

flash TUpdate update[] =
{
// no display
    0, 0,

// test displays
    0, 0, 0, 0,

// analogue clocks (required one pointer per clock)
    clockAnalogueUpdate, clockAnalogueUpdate, clockAnalogueUpdate, clockAnalogueUpdate,

// binary and bcd clocks
    clockBinaryUpdate, clockBCDUpdate,

// digital clocks (required one pointer per clock)
    clockDigitalUpdate, clockDigitalUpdate, clockDigitalUpdate, clockDigitalUpdate,

// other clocks
    clockBarCodeUpdate, clockKlingonUpdate, clockPingPongUpdate, clockAScopeUpdate,

// four letter words
    flwUpdate
};
#if MAX_DIGITAL != 4 || MAX_ANALOGUE != 4
    #error  Changes in TUpdate array required!
#endif

void clockSecondChange()
{
// use sweep_mode if mode is sweep, else mode as nominated
    if (mode == cmSweep)
    {
        if (!sweep_counter)
        {
            sweep_counter = config_use.sweep_count;
            do
            {
                if (sweep_mode == cmTop)
                    sweep_mode = cmBlank + 2;
                else
                    ++sweep_mode;
            } while (!(*shown[sweep_mode]));
        } else
            --sweep_counter;

        display_mode = sweep_mode;
    } else
        display_mode = mode;

    if (update[display_mode])               // check not nil
        update[display_mode]();             // jump to update function
}

// define a pointer to a function taking no parameters not returning anything
typedef void(*TRefresh)();

// array of pointers to screen refresh functions
// NB order must be the same as the cmXXXX enum
flash TRefresh refresh[] =
{
// no display
    0, sweepRefresh,

// test displays
    extentsRefresh, parallelRefresh, gratRefresh, bounceRefresh,

// analogue clocks (required one pointer per clock)
    clockAnalogueRefresh, clockAnalogueRefresh, clockAnalogueRefresh, clockAnalogueRefresh,

// binary and bcd clocks
    clockBinaryRefresh, clockBCDRefresh,

// digital clocks (required one pointer per clock)
    clockDigitalRefresh, clockDigitalRefresh, clockDigitalRefresh, clockDigitalRefresh,

// other clocks
    clockBarCodeRefresh, clockKlingonRefresh, clockPingPongRefresh, clockAScopeRefresh,

// four letter words
    flwRefresh
};
#if MAX_DIGITAL != 4 || MAX_ANALOGUE != 4
    #error  Changes in TRefresh array required!
#endif

const unsigned long msPerSec = 1000L;
const unsigned long msPerMin = 60000L;
const unsigned long msPerHour = 3600000L;

unsigned long lastSecond = 0;
unsigned long lastMinute = 0;
unsigned long lastHour = 0;

void everyTrigger()
{
	unsigned long now = millis();

	if (now - lastSecond >= msPerSec) {
		lastSecond = (now / msPerSec) * msPerSec;
		clockSecondChange();
	}

	if (now - lastMinute >= msPerMin) {
		lastMinute = (now / msPerMin) * msPerMin;
		clockMinuteChange();
	}

	if (now - lastHour >= msPerHour) {
		lastHour = (now / msPerHour) * msPerHour;
		clockHourChange();
	}

	if (!timeInitialized) {
	    crtSetTextInfo(1,7);
	    crtATextf(8,28,"Waiting");
	    crtAText(8, 16, "for NTP");
	    // Park the beam off-screen, because we have no Z control!
#ifdef MY_DAC
	    dacWrite(PARK_X, PARK_Y);
#else
	    dacWrite(pinX, PARK_X);
	    dacWrite(pinY, PARK_Y);
#endif
	    return;
	}

// displays that require calculation updates every 20mS
    switch (display_mode)
    {
        case cmBounce:
            bounceUpdate20mS();
            break;

        case cmAnalogue0:
        case cmAnalogue1:
        case cmAnalogue2:
        case cmAnalogue3:
            clockAnalogueUpate20mS();
            break;

        case cmPingPong:
            clockPingPongUpdate20mS();
            break;
    }

// redraw CRT face
    if (refresh[display_mode])              // check not nil
        refresh[display_mode]();            // refresh the crt

    // Park the beam off-screen, because we have no Z control!
#ifdef MY_DAC
	    dacWrite(PARK_X, PARK_Y);
#else
	    dacWrite(pinX, PARK_X);
	    dacWrite(pinY, PARK_Y);
#endif
}

portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool flagTrigger;     // flag set every 20mS to start CRT refresh

void crtCheck()
{
//	portENTER_CRITICAL_ISR(&timerMux);

    if (flagTrigger)                           // wait for CRT interrupt
    {
        //    	portEXIT_CRITICAL_ISR(&timerMux);
        everyTrigger();
        flagTrigger = false;
    } else {
        //    	portEXIT_CRITICAL_ISR(&timerMux);
    }
}

// If we use a separate task to drive the clock display
void crtTaskFn(void *pArg) {
	while (true) {
		crtCheck();
		delay(1);	// Give lower priority tasks a chance to run
	}
}

void forceUpdateRefresh()
{
    clockSecondChange();            // start screen transition to new mode
    everyTrigger();
}

#ifdef MY_DAC
void dacInit() {
	pinMode(pinX, ANALOG);
	pinMode(pinY, ANALOG);
    //Disable Tone
    CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL1_REG, SENS_SW_TONE_EN);
	//Disable Channel 1 Tone
	CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN1_M);
	//Disable Channel 2 Tone
	CLEAR_PERI_REG_MASK(SENS_SAR_DAC_CTRL2_REG, SENS_DAC_CW_EN2_M);
	dacWrite(0, 0);
	//Channel 1 output enable
	SET_PERI_REG_MASK(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_XPD_DAC | RTC_IO_PDAC1_DAC_XPD_FORCE);
	//Channel 2 output enable
	SET_PERI_REG_MASK(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_XPD_DAC | RTC_IO_PDAC2_DAC_XPD_FORCE);
}

void IRAM_ATTR dacWrite(uint8_t x, uint8_t y)
{
	// Channel 1
	//Set the Dac value
	SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, y, RTC_IO_PDAC1_DAC_S);   //dac_output

	// Channel 2
	//Set the Dac value
	SET_PERI_REG_BITS(RTC_IO_PAD_DAC2_REG, RTC_IO_PDAC2_DAC, x, RTC_IO_PDAC2_DAC_S);   //dac_output
}
#endif

hw_timer_t * syncTimer = NULL;
hw_timer_t * drawTimer = NULL;

void IRAM_ATTR onTrigger() {
	static int count = 0;
//	portENTER_CRITICAL_ISR(&timerMux);
	count = (count + 1) % 2;
	flagTrigger = true;

//	portEXIT_CRITICAL_ISR(&timerMux);
}

uint8_t dacXScale = 1;
uint8_t dacYScale = 1;
uint8_t dacXOffset = 0;
uint8_t dacYOffset = 0;

void crtPlot(const int x, const int y, const int delay) {
	int _y = 255 - (y * dacYScale + dacYOffset);
	int _x = x * dacXScale + dacXOffset;
#ifdef MY_DAC
	dacWrite(_x, _y);
#else
    dacWrite(pinX, _x);
    dacWrite(pinY, _y);
#endif

	crtBeam(true);
	delayMicroseconds(delay);
	crtBeam(false);
}

// Called every time a NTP time is successfully retrieved
void asyncTimeSetCallback(String time) {
	timeInitialized = true;
}

// Called every time an attempt to retrieve a NTP time fails
void asyncTimeErrorCallback(String msg) {
}

// A callback from within the CRT functions to get the current local time
void clockGetLocalTime(struct tm* pTm, suseconds_t* uSec) {
    timeSync.getLocalTime(pTm, uSec);
}

void createSSID() {
	// Create a unique SSID that includes the hostname. Max SSID length is 32!
	ssid = (chipId + hostName).substring(0, 31);
}

#ifdef OTA
void sendUpdateForm(AsyncWebServerRequest *request) {
	request->send(200, "text/html",
		"<form method='POST' action='/update' enctype='multipart/form-data'>"
		"<input type='file' name='update' id='file' value=''>"
		"<input type='submit' data-mini='true' value='Update'>"
		"</form>"
	);
}

void sendUpdatingInfo(AsyncResponseStream *response, boolean hasError) {
    response->print("<html><head><meta http-equiv=\"refresh\" content=\"10; url=/\"></head><body>");

    hasError ?
    		response->print("Update failed: please wait while the device reboots") :
    		response->print("Update OK: please wait while the device reboots");

    response->print("</body></html>");
}
#endif

// Additional text boxes to draw in the captive portal.
// A stopgap until I implement actual web pages
AsyncWiFiManagerParameter *tzParam;
AsyncWiFiManagerParameter *hostnameParam;

// Called from ASyncWifiManager when it successfully attaches to WiFI
void SetupServer() {
	DEBUG("SetupServer()");
	hostName = String(hostnameParam->getValue());
	hostName.put();

	timeZone = String(tzParam->getValue());
	timeZone.put();

	// write configuration to EEPROM
	config.commit();
	DEBUG(hostName.value);
	DEBUG(timeZone.value);

	// Set up MDNS (resolves .local hostnames)
	MDNS.begin(hostName.value.c_str());
    MDNS.addService("http", "tcp", 80);
}

void initFromEEPROM() {
//	config.setDebugPrint(debugPrint);
	// Initialize EEPROM from defaults, if the set of defaults has changed from what is
	// already there.
	config.init();
//	rootConfig.debug(debugPrint);

	// Get all of the configuration values from EEPROM
	rootConfig.get();

	// Set up extra text fields for use in config portal.
	hostnameParam = new AsyncWiFiManagerParameter("Hostname", "Clock host name", hostName.value.c_str(), 63);
	tzParam = new AsyncWiFiManagerParameter("Timezone", "Time zone", timeZone.value.c_str(), 63);

	// Restore current mode from EEPROM
	mode = modeConfig;
	modeChanged();
}

//The setup function is called once at startup
void setup()
{
    Serial.begin(115200);

    // First things first
	EEPROM.begin(2048);
	initFromEEPROM();

#ifdef MY_DAC
	dacInit();
	dacWrite(0, 0);
#else
	dacWrite(pinX, 0);
	dacWrite(pinY, 0);
#endif

	pinMode(pinRelay, OUTPUT);
	pinMode(pinTrigger, INPUT_PULLUP);

	mov.setDelay(1);
	mov.setOnTime(millis());
//	mov.setCallback(&sendMovMsg);

#ifdef INTERNAL_SYNC
	syncTimer = timerBegin(1, 80, true);	// Specify time interval in uS.
	timerAttachInterrupt(syncTimer, &onTrigger, true);
	timerAlarmWrite(syncTimer, 16666, true);
	timerAlarmEnable(syncTimer);
#else
	attachInterrupt(pinTrigger, onTrigger, RISING);
#endif

	// Turn the CRT on
	digitalWrite(pinRelay, HIGH);

	// Default clock config - need to adapt use ConfigItems and provide web
	// pages to edit them.
	configDefault();

	// The ESP32 DACs are 8 bit. This is a reasonable scale factor to
	// convert from the CRT library coordinate system to the OSC7.0 hardware
	crtScaleX = 3;
	crtScaleY = 3;

	// Set the CRT plot point callback
	crt_plot = crtPlot;

	unsigned long now = millis();

	// Should do this every time the time is changed
	lastSecond = (now / msPerSec) * msPerSec;
	lastMinute = (now / msPerMin) * msPerMin;
	lastHour = (now / msPerHour) * msPerHour;

	forceUpdateRefresh();

	// Start captive portal
	createSSID();

//	wifiManager.setCustomOptionsElement("<br><form action='/t' name='time_form' method='post'><button name='time' onClick=\"{var now=new Date();this.value=now.getFullYear()+','+(now.getMonth()+1)+','+now.getDate()+','+now.getHours()+','+now.getMinutes()+','+now.getSeconds();} return true;\">Set Clock Time</button></form><br><form action=\"/app.html\" method=\"get\"><button>Configure Clock</button></form>");
	wifiManager.setConnectTimeout(10);
	wifiManager.addParameter(hostnameParam);
	wifiManager.addParameter(tzParam);
	wifiManager.setSaveConfigCallback(SetupServer);
    wifiManager.startConfigPortalModeless(ssid.c_str(), "secretsauce");

#ifdef OTA
	otaUpdater.init(server, "/update", sendUpdateForm, sendUpdatingInfo);
#endif

	// Start the web server
	server.begin();

	// Synchronize time with time source
	timeSync.init();

	// Set local time function CRT callback
	pGetLocalTimeF = clockGetLocalTime;

#ifdef notdef
    xTaskCreatePinnedToCore(
          crtTaskFn, /* Function to implement the task */
          "CRT refresh task", /* Name of the task */
          4096,  /* Stack size in words */
          1,  /* Task input parameter */
		  tskIDLE_PRIORITY + 6,  /* More than background tasks */
          &crtTask,  /* Task handle. */
		  0
		  );
#endif
}

// The loop function is called in an endless loop
void loop()
{
	// Occasionally scan for new SSIDs - this method throttles the calls!
	wifiManager.loop();

	// Do something?
	if (frontButton.clicked()) {
		do {
			modeConfig = (modeConfig + 1) % (cmTop + 1);
		} while (modeConfig == cmSweep || modeConfig == cmBlank);

		modeConfig.put();	// Make the change committable
		config.commit();	// Commit it
		mode = modeConfig;
	}
	if (middleButton.clicked()) {
		do {
			modeConfig = (modeConfig + cmTop) % (cmTop + 1);
		} while (modeConfig == cmSweep || modeConfig == cmBlank);

		modeConfig.put();	// Make the change committable
		config.commit();	// Commit it
		mode = modeConfig;
	}
	if (backButton.clicked()) {
		modeConfig = cmSweep;
		modeConfig.put();	// Make the change committable
		config.commit();	// Commit it
		mode = modeConfig;
	}

	// Check interrupt flag
	crtCheck();
}
