#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>

#include "wifi_params.h"

#include <OneWire.h>

ESP8266WebServer websrv (80);
WiFiUDP udp;
const int udp_port = 2222;

const int DS18B20_PIN = 14;

float temperature;

unsigned long int send_temperature_at = 1000;
unsigned long int read_temperature_at = 0;

OneWire ds(DS18B20_PIN);

static void handleRoot() {
	char temp[1024];
	int sec = millis() / 1000;
	int min = sec / 60;
	int hr = min / 60;

	snprintf ( temp, 1024,

"<html><head><title>Bedroom thermometer</title></head><body>\
    <h1>Hello from bedroom thermometer!</h1>\
    <p>Built on %s at %s</p><p>Uptime: %02d:%02d:%02d = %d ms</p>\
	<p>Current temperature is %f C\n</p> \
  </body></html>",

		__DATE__, __TIME__, hr, min % 60, sec % 60, (int)millis(),
		temperature
	);
	websrv.send ( 200, "text/html", temp );
}

void ota_onstart(void)
{
}

void ota_onprogress(unsigned int sz, unsigned int total)
{
	Serial.print("OTA: "); Serial.print(sz); Serial.print("/"); Serial.print(total);
	Serial.print("="); Serial.print(100*sz/total); Serial.println("%%");
}

void ota_onerror(ota_error_t err)
{
	Serial.print("OTA ERROR:"); Serial.println((int)err);
}

void setup ( void ) {
	Serial.begin(115200);

	Serial.println(ESP.getResetReason());
	WiFi.mode(WIFI_STA);
	WiFi.setOutputPower(10); // reduce power to 10dBm = 10mW

	WiFi.begin ( ssid, password );

	WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
	Serial.println ( "" );
	Serial.print("Connecting to ");
	Serial.print(ssid); Serial.print(" ");

	// Wait for connection
	while ( WiFi.status() != WL_CONNECTED ) {
		delay ( 500 );
		Serial.print ( "." );
	}

	Serial.println ( "" );
	Serial.print ( "Cnnectd to " );
	Serial.println ( ssid );
	Serial.print ( "IP " );
	Serial.println ( WiFi.localIP() );

	udp.begin(udp_port);
	websrv.on ( "/", handleRoot );

	websrv.begin();

	ArduinoOTA.onStart(ota_onstart);
	ArduinoOTA.onError(ota_onerror);
	ArduinoOTA.onProgress(ota_onprogress);
	ArduinoOTA.setHostname("esp8266-thermometer");
	ArduinoOTA.begin();
}

void udp_send(const char *str)
{
	udp.beginPacket(udp.remoteIP(), udp.remotePort());
	udp.write(str);
	udp.endPacket();
}

static void udp_send_status_report(void)
{
	char str[50];
	sprintf(str, "temperature %f", temperature);
	udp_send(str);
}

static void parse_cmd(const char *buf)
{
	if (!strncmp(buf, "STATUS", 6)) {
		udp_send_status_report();
	} 
}

void send_temperature_update()
{
	HTTPClient http;
	char URI[150];

	sprintf(URI, "http://192.168.1.6:5000/update/temperature/bed/%d.%d", (int)temperature, (int)((temperature-(int)temperature)*100.0));
	Serial.print("[HTTP] begin...\n");
	Serial.print(URI);

	http.begin(URI); 

	Serial.print("[HTTP] GET...\n");
	int httpCode = http.GET();

	// httpCode will be negative on error
	if(httpCode) {
		// HTTP header has been send and Server response header has been handled
		Serial.printf("[HTTP] GET... code: %d\n", httpCode);
	}

	http.end();
}

bool must_do_deep_sleep()
{
	HTTPClient http;
	char URI[150];

	sprintf(URI, "http://192.168.1.6:5000/deep_sleep_mode/");
	http.begin(URI); 

	int httpCode = http.GET();

	if (httpCode == HTTP_CODE_OK) {
		// HTTP header has been send and Server response header has been handled
		Serial.printf("[HTTP] GET... code: %d\n", httpCode);
		String payload = http.getString();
		Serial.println(payload);

		if (payload.startsWith("1")) {
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}

	http.end();
}

void loop ( void ) {
	ArduinoOTA.handle();
	websrv.handleClient();

	if (millis() > read_temperature_at) {
		uint8_t addr[8];
		uint8_t data[12];
		int i = 4;
		while(!ds.search(addr) && i--) {
			Serial.println("No more addresses.");
			ds.reset_search();
			delay(250);
		}
		if (!i) {
			return;
		}

		ds.reset();
		ds.select(addr);
		ds.write(0x44, 1);
		delay(800);
		uint8_t present = ds.reset();
		ds.select(addr);    
		ds.write(0xBE);

		for (int i = 0; i < 9; i++) {
			data[i] = ds.read();
		}

		// Convert the data to actual temperature
		// because the result is a 16 bit signed integer, it should
		// be stored to an "int16_t" type, which is always 16 bits
		// even when compiled on a 32 bit processor.
		int16_t raw = (data[1] << 8) | data[0];
		byte cfg = (data[4] & 0x60);
		// at lower res, the low bits are undefined, so let's zero them
		if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
		else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
		else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
		//// default is 12 bit resolution, 750 ms conversion time
		printf("Temperature is %d\n", (int)(100.0*(float)raw/16.0));
		temperature = raw/16.0;

		read_temperature_at = millis() + 60L * 1000L;
	}

	char packetBuffer[255];
	int packetSize = udp.parsePacket();
	if (packetSize) {
		// read the packet into packetBufffer
		int len = udp.read(packetBuffer, 255);
		if (len > 0) {
			packetBuffer[len] = 0;
		}

		parse_cmd(&packetBuffer[0]);
	}
	
	if (millis() > send_temperature_at) {
		send_temperature_at = millis() + 5L * 60L * 1000L;
		send_temperature_update();
	
		// Call my server to check if we need to deep sleep or not. This is used to enable OTAs from my desk. :)
		if (must_do_deep_sleep()) {
			Serial.println("deepsleep go");	
			ESP.deepSleep(5000*60000L); 
		}
	}
   
	delay(250);
}
