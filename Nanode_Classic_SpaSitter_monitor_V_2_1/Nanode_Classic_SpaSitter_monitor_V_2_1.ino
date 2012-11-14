
#include <DallasTemperature.h>
#include <OneWire.h>

/*                          _                                                      _      
SpaSitter 2.0
 */
//--------------------------------------------------------------------------------------
// Uses RF error in code to send data to pachube.  Version 3.0 will use an RF node for wireless capabilities. 
// Uses mac address from 11AA02E48 on NanodeRF
// Decode reply from server to check for server 'ok' from emoncms and OK status from pachube
// Demonstrate using domain name or static IP destination and port
// Pachube on port 80
// uses POST and csv format to send to pachube
// custom callback functions used for all destination

// Code for SpaSitter adapted from emonBase and the OpenEnergyMonitor.org
//Documentation: http://openenergymonitor.org/emon/emonbase

// Authors: Trystan Lea, Glyn Hudson and Francois Hug
// Part of the: openenergymonitor.org project
// Licenced under GNU GPL V3
// http://openenergymonitor.org/emon/license

// EtherCard Library by Jean-Claude Wippler and Andrew Lindsay
// JeeLib Library by Jean-Claude Wippler
// NanodeMAC library by Rufus Cable
//--------------------------------------------------------------------------------------

#define DEBUG     //comment out to disable serial printing to increase long term stability 
#define UNO       //anti crash wachdog reset only works with Uno (optiboot) bootloader, comment out the line if using delianuova

#define HTTP_TIMEOUT 10000

//#define POST2EMONCMS	// uncomment to send to emoncms
//#define POST2LOCAL		// uncomment to send to local emoncms
#define POST2PACHUBE	// uncomment to send to pachube

#include <Wire.h>
//#include <RTClib.h>
//RTC_Millis RTC;

#include <JeeLib.h>	     // https://github.com/jcw/jeelib
#include <avr/wdt.h>


//#define MYNODE 16            // node ID 30 reserved for base station
//#define freq RF12_868MHZ     // frequency
//#define group 0xb3           // network group 

// The RF12 data payload - a neat way of packaging data when sending via RF - JeeLabs
// must be same structure as transmitted from emonTx
typedef struct
{
  byte boardStatus;
  int battery;
  int ct1; 
} 
Payload;
Payload emontx;   

float PHLevel;

#define ONE_WIRE_BUS 8

OneWire oneWire(ONE_WIRE_BUS);

DallasTemperature sensors(&oneWire);

DeviceAddress insideThermometer;

static unsigned long lastSampleTime = 0;


#define DS18S20_ID 0x10
#define DS18B20_ID 0x28
float temp;
boolean getTemperature();
byte i;
byte present = 0;
byte data[12];
byte addr[8];

float Ftemp;

//---------------------------------------------------------------------
// The PacketBuffer class is used to generate the json string that is send via ethernet - JeeLabs
//---------------------------------------------------------------------
class PacketBuffer : 
public Print {
public:
  PacketBuffer () : 
  fill (0) {
  }
  const char* buffer() { 
    return buf; 
  }
  byte length() { 
    return fill; 
  }
  void reset()
  { 
    memset(buf,NULL,sizeof(buf));
    fill = 0; 
  }
  virtual size_t write (uint8_t ch)
  { 
    if (fill < sizeof buf) buf[fill++] = ch; 
  }
  byte fill;
  char buf[150];		// Might need to be set higher if long strings need to be sent (Be carefull to stay within ram limits)
private:
};
PacketBuffer str;

//--------------------------------------------------------------------------
// Ethernet
//--------------------------------------------------------------------------
#include <EtherCard.h>		// https://github.com/jcw/ethercard 
#include <NanodeMAC.h>      // https://github.com/thiseldo/NanodeMAC

// ethernet interface mac address, must be unique on the LAN
static uint8_t mymac[6] = { 
  0,0,0,0,0,0 };		// Mac taken from 11AA02E48
NanodeMAC mac( mymac );

byte Ethernet::buffer[600];
static uint32_t timer;

//Domain name of remote webservers - leave blank if posting to IP address 
#ifdef POST2EMONCMS
char websiteemon[] PROGMEM = "vis.openenergymonitor.org";		// domain name of emoncms
static byte emonip[] = { 
  0,0,0,0 };
static uint16_t emonport = 80;
#define APIKEY_EMON "/emoncms3/api/post.json?apikey=xxxx&json="	// Set xxxx to your emoncms write API key
#endif

#ifdef POST2LOCAL
char websitelocal[] PROGMEM = "";
static byte hislocalip[] = { 
  192,168,1,105 };  // set it to local IP of emoncms
static uint16_t localport = 8888;
#define APIKEY_LOCAL "/emoncms3/api/post.json?apikey=yyyy&json="		// Set yyyy to your local emoncms write API key
#endif

#ifdef POST2PACHUBE   // Pachube is Cosm.com
char websitepac[] PROGMEM = "api.pachube.com";
static byte pachubeip[] = { 
  0,0,0,0 };
static uint16_t pachubeport = 80;
// Pachube change these settings to match your own setup
#define FEED_PAC  "/v2/feeds/YOUR_FEED_ID.csv?_method=put"		// YOUR_FEED_ID  from cosm.com
#define APIKEY_PAC "X-PachubeApiKey: YOUR_API_KEY"		// Set YOUR_API_KEY to your cosm api write key
#endif

//--------------------------------------------------------------------------
// Flow control varaiables
int dataReady=0;                                                  // is set to 1 when there is data ready to be sent
unsigned long lastRF;                                             // used to check for RF recieve failures
MilliTimer tReply;
static boolean httpHaveReply;									  // Set to 1 when answer received from webserver

//NanodeRF error indication LED variables 
const int redLED=6;                      // NanodeRF RED indicator LED
const int greenLED=5;                    // NanodeRF GREEN indicator LED
int error=0;                             // Ethernet (controller/DHCP/server timeout) error flag
int RFerror=0;                           // RF error flag - high when no data received 
int dhcp_status = 0;

#ifdef POST2EMONCMS
int dns_status_emon = 0; 
#endif

#ifdef POST2PACHUBE
int dns_status_pac = 0; 
#endif

int request_attempt = 0;
char line_buf[50];						// Buffer to hold a line from server reply (used in callbacks)

//-----------------------------------------------------------------------------------
// Ethernet callbacks
// receive reply and decode
//-----------------------------------------------------------------------------------
#ifdef POST2PACHUBE
static void callback_pac (byte status, word off, word len) {		// callback function for pachube

  get_header_line(1,off);      // Get the http status code
#ifdef DEBUG
  Serial.println(line_buf);    // Print out the http status code
#endif
  //-----------------------------------------------------------------------------
  if (strcmp(line_buf,"HTTP/1.1 200 OK")) {
#ifdef DEBUG
    Serial.println("ok received from pachube");
#endif
    httpHaveReply = 1;		// update flags (reply received, reset request attempt counter and network error
    request_attempt = 0;
    error=0;
  }
}



static void format_pac_json (void) {		// function to format data to send to pachube. Format is key_name,key_value - one line per pair
  str.reset();                            // Reset json string      
  str.println("RF,0");                    // RF recieved so no failure
  str.print("Sta,");    
  str.println(emontx.boardStatus); // send various fields
  str.print("ct1,");    
  str.println(emontx.ct1);
  str.print("Bat,");    
  str.println(emontx.battery);
  str.print("\0");
}
#endif

#ifdef POST2EMONCMS
static void callback_emon (byte status, word off, word len) {		// callback function for emoncms

  get_header_line(1,off);      // Get the http status code
#ifdef DEBUG
  Serial.println(line_buf);    // Print out the http status code
#endif
  //-----------------------------------------------------------------------------
  get_reply_data(off);
  //#ifdef DEBUG
  //Serial.println(line_buf);
  //#endif
  if (strcmp(line_buf,"ok")) {
#ifdef DEBUG
    Serial.println("ok received from emoncms");
#endif
    httpHaveReply = 1;
    request_attempt = 0;
    error=0;
  }
}
#endif

#if defined(POST2EMONCMS) || defined(POST2LOCAL)		// Function to format json string for emoncms
static void format_emon_json (void) {
  // JSON creation: JSON sent are of the format: {key1:value1,key2:value2} and so on
  str.reset();                                                 // Reset json string    
  str.print("{rf_fail1:0");                                     // RF recieved so no failure
  str.print(",status1:");    
  str.print(emontx.boardStatus);
  str.print(",ct1:");    
  str.print(emontx.ct1);              // Add CT 1 reading  - un-comment if needed
  str.print(",battery1:");    
  str.print(emontx.battery);
  str.print("}\0");
}
#endif

#ifdef POST2LOCAL
static void callback_local (byte status, word off, word len) {	// callback function for local emoncms (can use the same callback for local and remote emoncms)

  get_header_line(1,off);      // Get the http status code
#ifdef DEBUG
  Serial.println(line_buf);    // Print out the http status code
#endif
  get_header_line(2,off);      // Get the date and time from the header
#ifdef DEBUG
  Serial.println(line_buf);    // Print out the date and time
#endif

    // Decode date time string to get integers for hour, min, sec, day
  // We just search for the characters and hope they are in the right place
  /*
	char val[1];
   val[0] = line_buf[23]; val[1] = line_buf[24];
   int hour = atoi(val);
   val[0] = line_buf[26]; val[1] = line_buf[27];
   int mins = atoi(val);
   val[0] = line_buf[29]; val[1] = line_buf[30];
   int sec = atoi(val);
   val[0] = line_buf[11]; val[1] = line_buf[12];
   int day = atoi(val);
   
   // Set the RTC
   RTC.adjust(DateTime(2012, 2, day, hour, mins, sec));
   DateTime now = RTC.now();
   */
  //-----------------------------------------------------------------------------
  get_reply_data(off);
  //#ifdef DEBUG
  //Serial.println(line_buf);
  //#endif
  if (strcmp(line_buf,"ok")) {
#ifdef DEBUG
    Serial.println("ok received from local emon");
#endif
    httpHaveReply = 1;
    request_attempt = 0;
    error=0;
  }
}

static void format_local_json (void) {		// Function to format json string for local emoncms (Use same format than remote cms in this example)
  format_emon_json();
}
#endif

//**********************************************************************************************************************
// SETUP
//**********************************************************************************************************************
void setup () {

  //Nanode RF LED indictor  setup - green flashing means good - red on for a long time means bad! 
  //High means off since NanodeRF tri-state buffer inverts signal 
  pinMode(redLED, OUTPUT); 
  digitalWrite(redLED,LOW);            
  pinMode(greenLED, OUTPUT); 
  digitalWrite(greenLED,LOW);       
  delay(100); 
  digitalWrite(redLED,HIGH);                        //turn off redLED
#ifdef DEBUG
  Serial.begin(9600);
  Serial.println("\n[multi webClient]");
  //sensors.begin(); // IC Default 9 bit. If you have troubles consider upping it 12. Ups the delay giving the IC more time to process the temperature measurement
Serial.print("Locating devices...");
  sensors.begin();
  Serial.print("Found ");
  Serial.print(sensors.getDeviceCount(), DEC);
  Serial.println(" devices.");
  
  if (!sensors.getAddress(insideThermometer, 0)) Serial.println("Unable to find address for Device 0"); 
  
  // show the addresses we found on the bus
  Serial.print("Device 0 Address: ");
  printAddress(insideThermometer);
  Serial.println();

  // set the resolution to 9 bit (Each Dallas/Maxim device is capable of several different resolutions)
  sensors.setResolution(insideThermometer, 9);
 
  Serial.print("Device 0 Resolution: ");
  Serial.print(sensors.getResolution(insideThermometer), DEC); 
  Serial.println();
#endif
  error=0;

  if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {
#ifdef DEBUG
    Serial.println( "Failed to access Ethernet controller");
#endif
    error=1;  
  }
  dhcp_status = 0;
  httpHaveReply = 0;
#ifdef POST2EMONCMS
  int dns_status_emon = 0; 
#endif
#ifdef POST2PACHUBE
  int dns_status_pac = 0; 
#endif
  request_attempt = 0;
  //rf12_initialize(MYNODE, freq, group);
  lastRF = millis()-40000;  // setting lastRF back 40s is useful as it forces the ethernet code to run straight away
#ifdef BMP085
  lastBMP085 = millis()-40000;
  pressureSens.begin();
#endif
  digitalWrite(greenLED,HIGH);                                    //Green LED off - indicate that setup has finished 
#ifdef UNO
  wdt_enable(WDTO_8S); 
#endif
}

void printTemperature(DeviceAddress deviceAddress)
{
  // method 1 - slower
  //Serial.print("Temp C: ");
  //Serial.print(sensors.getTempC(deviceAddress));
  //Serial.print(" Temp F: ");
  //Serial.print(sensors.getTempF(deviceAddress)); // Makes a second call to getTempC and then converts to Fahrenheit

  // method 2 - faster
  float tempC = sensors.getTempC(deviceAddress);
  //Serial.print("Temp C: ");
  //Serial.print(tempC);
  //Serial.print(" Temp F: ");
  //Serial.println(DallasTemperature::toFahrenheit(tempC)); // Converts tempC to Fahrenheit
  Ftemp = DallasTemperature::toFahrenheit(tempC);
}
//**********************************************************************************************************************

//**********************************************************************************************************************
// LOOP
//**********************************************************************************************************************
void loop () {

#ifdef UNO
 wdt_reset();
#endif


  //-----------------------------------------------------------------------------------
  // Get DHCP address
  // Putting DHCP setup and DNS lookup in the main loop allows for: 
  // powering nanode before ethernet is connected
  //-----------------------------------------------------------------------------------
  if (ether.dhcpExpired()) dhcp_status = 0;    // if dhcp expired start request for new lease by changing status

  if (!dhcp_status){
#ifdef UNO
    wdt_disable();
#endif 
    dhcp_status = ether.dhcpSetup();           // DHCP setup
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
#ifdef DEBUG
    Serial.print("DHCP status: ");             // print
    Serial.println(dhcp_status);               // dhcp status
#endif
    if (dhcp_status){                          // on success print out ip's
      ether.printIp("IP:  ", ether.myip);
      ether.printIp("GW:  ", ether.gwip);  
      //static byte dnsip[] = {8,8,8,8};  		// Setup to DNS server IP (8.8.8.8 is google public dns server)
      static byte dnsip[] = {
        192,168,1,1      };
      ether.copyIp(ether.dnsip, dnsip);
      ether.printIp("DNS: ", ether.dnsip);          
    } 
    else { 
      error=1; 
    }  
  }
         

  //-----------------------------------------------------------------------------------
  // Get server addresses via DNS
  //-----------------------------------------------------------------------------------
#ifdef POST2EMONCMS			// get IP of remote emoncms, and store it
  if (dhcp_status && !dns_status_emon){
#ifdef UNO
    wdt_disable();
#endif 
    dns_status_emon = ether.dnsLookup(websiteemon);    // Attempt DNS lookup
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
#ifdef DEBUG
    Serial.print("DNS status emon: ");             // print
    Serial.println(dns_status_emon);               // dns status
#endif
    if (dns_status_emon){
      ether.copyIp(emonip, ether.hisip); 		// Store IP
#ifdef DEBUG
      ether.printIp("SRV emoncms: ", emonip);         // server ip
#endif
    } 
    else { 
      error=1; 
    }  
  }
#endif

#ifdef POST2PACHUBE			// get IP of pachube, and store it
  if (dhcp_status && !dns_status_pac){
#ifdef UNO
    wdt_disable();
#endif 
    dns_status_pac = ether.dnsLookup(websitepac);    // Attempt DNS lookup
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
#ifdef DEBUG
    Serial.print("DNS status pachube: ");             // print
    Serial.println(dns_status_pac);               // dns status
#endif
    if (dns_status_pac){
      ether.copyIp(pachubeip, ether.hisip); 
#ifdef DEBUG
      ether.printIp("SRV pachube: ", pachubeip);         // server ip
#endif
    } 
    else { 
      error=1; 
    }  
  }
#endif

  if (error==1 || RFerror==1 || request_attempt > 1) digitalWrite(redLED,LOW);      //turn on red LED if RF / DHCP or Etherent controllor error. Need way to notify of server error
  else digitalWrite(redLED,HIGH);



  //---------------------------------------------------------------------
  // On data receieved from rf12
  //---------------------------------------------------------------------
  if (rf12_recvDone() && rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0) 
  {
    digitalWrite(greenLED,LOW);                                   // turn green LED on to indicate RF recieve 
    emontx=*(Payload*) rf12_data;                                 // Get the payload
    byte emontx_nodeID=rf12_hdr & 0x1F;                           //extract node ID from received packet - only needed when multiple emonTx are posting on same network     
    RFerror=0;                                                    //reset RF  flag
    if (emontx_nodeID == 12)									  // check sender id (change or comment out to fit your setup)
    {
      dataReady = 1;                                              // Ok, data is ready in received packet
    }
    lastRF = millis();                                            // reset lastRF timer
#ifdef DEBUG 
    Serial.println("RF recieved");
#endif
    digitalWrite(greenLED,HIGH);                                  // Turn green LED on OFF
  }

  // If no data is recieved from rf12 module the server is updated every 30s with RFfail = 1 indicator for debugging
  if ((millis()-lastRF)>30000)
  {
    lastRF = millis();                                            // reset lastRF timer
    dataReady = 1;                                                // Ok, data is ready
    RFerror=1;
  }

  if (dataReady) {
    // Example of posting to emoncms v3 demo account goto http://vis.openenergymonitor.org/emoncms3 
    request_attempt ++;
    
#ifdef POST2EMONCMS											// Send to emoncms
    while (ether.packetLoop(ether.packetReceive()) != 0) {
    }	// wait for ethernet buffer to be free
    ether.copyIp(ether.hisip, emonip); 						// set destination IP address
    ether.hisport = emonport;									// set destination port
    httpHaveReply = 0;										// reset reply flag
    if(RFerror)												// format string to send
    {
      str.reset();
      str.print("{rf_fail1:1}");
    }
    else
    {
      format_emon_json();
    }
#ifdef DEBUG 
    Serial.println(str.buf); 
    Serial.println(request_attempt);   
#endif    // Print final json string to terminal
    ether.browseUrl(PSTR(APIKEY_EMON),str.buf, websiteemon, callback_emon);	// Use GET to send string to pachube      // Wait for reply
    tReply.set(HTTP_TIMEOUT);
#ifdef UNO
    wdt_disable();
#endif 
    while (!httpHaveReply) {
      ether.packetLoop(ether.packetReceive());
      if (tReply.poll()) {
        error=1;        // network timeout
        break;
      }
    }
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
    ether.packetLoop(ether.packetReceive());
#endif

#ifdef POST2LOCAL
    while (ether.packetLoop(ether.packetReceive()) != 0) {
    }
    ether.copyIp(ether.hisip, hislocalip); 
    ether.hisport = localport;
    httpHaveReply = 0;
    if(RFerror)
    {
      str.reset();
      str.print("{rf_fail1:1}");
    }
    else
    {
      format_local_json();
    }
#ifdef DEBUG 
    Serial.println(str.buf); 
    Serial.println(request_attempt);   
#endif    // Print final json string to terminal
    ether.browseUrl(PSTR(APIKEY_LOCAL),str.buf, websitelocal, callback_local);
    // Wait for reply
    tReply.set(HTTP_TIMEOUT);
#ifdef UNO
    wdt_disable();
#endif 
    while (!httpHaveReply) {
      ether.packetLoop(ether.packetReceive());
      if (tReply.poll()) {
        error=1;        // network timeout
        break;
      }
    }
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
    ether.packetLoop(ether.packetReceive());
#endif


#ifdef POST2PACHUBE
    
    analogRead(A5);     // pH Level equation
    //delay(2000);
    float PHprobe = analogRead(A5);     // pH Level equation
    PHLevel = (0.0178 * (PHprobe) - 1.889);  //PHprobe -26  //-14
    float HOT_pH = -1.9 - (((2.5 - PHprobe)/200) / (0.257179 + 0.000941468 * temp));  // - 1.9
    
    //pH = 7 - (2.5 - SensorValue / 200) / (0.257179 + 0.000941468 * Temperature)

    int mV;
    analogRead(A1);
    //delay(2000);
    float ORPprobe = analogRead(A1);
    mV = ((2.5 - ORPprobe / 200) / 1.037)* 1000;
    
    
    // call sensors.requestTemperatures() to issue a global temperature 
  // request to all devices on the bus
  //Serial.print("Requesting temperatures...");
  sensors.requestTemperatures(); // Send the command to get temperatures
  //Serial.println("DONE");
  
  // It responds almost immediately. Let's print out the data
   printTemperature(insideThermometer); // Use a simple function to print out the data
   
   
    if (millis() - lastSampleTime > 50000)  // Only after 10 seconds
         {
         lastSampleTime = millis();
  
         }


    while (ether.packetLoop(ether.packetReceive()) != 0) {
    }
    ether.copyIp(ether.hisip, pachubeip); 
    ether.hisport = pachubeport;
    httpHaveReply = 0;
    if(RFerror)
    {
      str.reset();      
      str.print("0,");
      str.println(HOT_pH);
      str.print("1,");
      str.println(mV);
      str.print("2,");
      str.println(Ftemp);
    }
    else
    {
      format_pac_json();
    }
#ifdef DEBUG 
    Serial.println(str.buf); 
    Serial.println(request_attempt);   
#endif    // Print final json string to terminal
    ether.httpPost(PSTR(FEED_PAC), websitepac, PSTR(APIKEY_PAC), str.buf, callback_pac);	// Use POST to send string to pachube
    // Wait for reply
    tReply.set(HTTP_TIMEOUT);
#ifdef UNO
    wdt_disable();
#endif 
    while (!httpHaveReply) {
      ether.packetLoop(ether.packetReceive());
      if (tReply.poll()) {
        error=1;        // network timeout
        break;
      }
    }
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
    ether.packetLoop(ether.packetReceive());
#endif
    dataReady =0;
  }

  if (request_attempt > 10) // Reset flags and ethernet chip if more than 10 request attempts have been tried without a reply
  {
    ether.begin(sizeof Ethernet::buffer, mymac);
    dhcp_status = 0;
#ifdef POST2EMONCMS
    dns_status_emon = 0;
#endif
#ifdef POST2PACHUBE
    dns_status_pac = 0; 
#endif
    httpHaveReply = 0;
    request_attempt = 0;
    error=0;
  }
}

void printAddress(DeviceAddress deviceAddress)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

//**********************************************************************************************************************


