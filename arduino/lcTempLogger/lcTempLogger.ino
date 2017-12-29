#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <DNSServer.h>
#include "OneWire.h"
#include "DallasTemperature.h"
#include "FS.h"

#define HOSTNAME            "ESP8622"
#define APDEFAULTNAME       "LC-ESP8266"    // mac will be added to the end
#define APDEFAULTPW         "12345678"
#define ONE_WIRE_BUS        2               // GPIO
#define INACIVITY_RESET     g_ulInactivTime = millis();Serial.println("reset inactivity");
/*
 * https://thingspeak.com/
 * https://github.com/nothans/ESP8266/blob/master/examples/A0_to_ThingSpeak.ino
 * 
 * https://io.adafruit.com/
 *
 * https://emoncms.org/
 */

/*
 * Prototypes
 */
void sendFile(String sFile = "");
void goToSleep();
void shutdown();
void factoryReset();
bool saveStringToFile(String sContent,String sName);
String readStringFromFile(String sName);
void getDefaultApName(char *sString);
void saveApiSettings();
void getApiSettings();
String saveWifiSettings();
void handleNotFound();
void getNetworkStatus();
void setupWebserver();
void checkForDeepSleep();

/*
 * Globals
 */
// IP-Addresses for accesspoint
IPAddress g_apIP(192, 168, 4, 1);
IPAddress g_netMsk(255, 255, 255, 0);

// Web-server
ESP8266WebServer server(80);

// DNS-server
const byte DNS_PORT = 53;
DNSServer dnsServer;

// 1wire sensor (DS18B20)
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// measure supplyvoltage
ADC_MODE(ADC_VCC);

// timeout for wifi-connect
unsigned long g_ulStartTime;
unsigned long g_ulInactivTime;
// 
int g_bTryConnect = false;
int g_bConnectCanel = false;
// don't send Message to client
int g_bDeepSleep = false;
void goToSleep(){
  if(!g_bDeepSleep){
    // don't write the file every time
    saveStringToFile("1","deepsleep");
    server.send(200,"text/json","see ya!");
  }
  
  Serial.println("good night!");
  
  String sSleepTime = readStringFromFile("api.interval");
  int iSleepTime = sSleepTime.toInt();
  // 1000000 == 1s 
  ESP.deepSleep(1000000*60*iSleepTime, WAKE_RF_DEFAULT);
  delay(100);
}

void shutdown(){
  Serial.println("shutdown");
  // "shutdown" just sleep for 27 years
  ESP.deepSleep(999999999*999999999U, WAKE_RF_DEFAULT);
  delay(100);
}


void factoryReset(){
  SPIFFS.remove("api.method");
  SPIFFS.remove("api.endpoint");
  SPIFFS.remove("api.headers");
  SPIFFS.remove("api.post");
  SPIFFS.remove("api.interval");
  
  SPIFFS.remove("wifi.name");
  SPIFFS.remove("wifi.pw");
  SPIFFS.remove("deepsleep");

   ESP.eraseConfig();
   ESP.restart();  
}

bool setApiRequest(){
  String sValue = "";

  if(WiFi.status() != WL_CONNECTED){
    if(!g_bDeepSleep)
      server.send(200,"text/plain","wifi not connected!");

    Serial.println("wifi not connected!");
    return false;
  }

  // 85°C == Failture
  while(sValue == "" || sValue.startsWith("85.0")){
    sensors.requestTemperatures();
    delay(10);
    sValue = sensors.getTempCByIndex(0);
  }
  float fVcc=ESP.getVcc()/1024.00f;
  String sVcc = String(fVcc);
  
  String sEndPoint = readStringFromFile("api.endpoint");

  String sMethod = readStringFromFile("api.method");

  sMethod.toLowerCase();
  
  sEndPoint.replace("%VALUE%",sValue);
  sEndPoint.replace("%BATTERY%",sVcc);
  Serial.println("endpoint: "+sEndPoint);
  
  
  HTTPClient http;
  http.begin(sEndPoint);

  // add headers
  File fHeaders = SPIFFS.open("api.headers","r");
  if(fHeaders){
    String sLine = "";
    while(sLine = fHeaders.readStringUntil('\n')){
      
      if(sLine.length() == 0)
        break;

      sLine.replace("%VALUE%",sValue);
      sLine.replace("%BATTERY%",sVcc);
      Serial.println("Read line from api.headers: "+sLine);

      for (int i = 0; i < sLine.length(); i++) {
        if (sLine.substring(i, i+1) == "=") {
          String sKey = sLine.substring(0, i);
          String sValue = sLine.substring(i+1);

          Serial.println("Found header: "+sKey+" - "+sValue);
          http.addHeader(sKey,sValue);
          i=sLine.length();
        }
      }
    }
  }else{
    Serial.println("no headers to add");
  }

  
  int iHttpCode = -1;
  if(sMethod == "post"){
    
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    char acPayload[200] = {};
    String sPayload = "";
    File fPostData = SPIFFS.open("api.post","r");
    if(!fPostData){
      Serial.println("failed to open api.method");
    }else{
          while (fPostData.available()){
            sPayload += char(fPostData.read());
          }
          fPostData.close();
          
          sPayload.replace("%VALUE%",sValue);
          sPayload.replace("%BATTERY%",sVcc);
          
  }
    
    Serial.println("POST");
//    iHttpCode = http.POST((uint8_t*)acPayload,sizeof(acPayload));
    iHttpCode = http.POST(sPayload);
  }else{
    Serial.println("GET");
    iHttpCode = http.GET();
  }
  
  Serial.println("http-code: ");
  Serial.println(iHttpCode);

  if(!g_bDeepSleep)
    server.send(200,"text/plain",http.getString());
  
  http.end();
  return true;
}

bool saveStringToFile(String sContent,String sName){
  File fName = SPIFFS.open(sName,"w");
  if(!fName){
    Serial.println("failed to open "+sName);
     return false;
  }
  fName.print(sContent);
  fName.close();
  return true;
}
String readStringFromFile(String sName){
  if(!SPIFFS.exists(sName)){
    return "";
  }
  
  File fFile = SPIFFS.open(sName,"r");
  if(!fFile){
    Serial.println("failed to open "+sName);
    return "";
  }

  String sRead = fFile.readStringUntil('\n');
  fFile.close();

  return sRead;
}
void getDefaultApName(char *sString){
  byte mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(sString,100,"%s-%02x%02x%02x%02x%02x%02x",APDEFAULTNAME,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

void saveApiSettings(){
  for (uint8_t i=0; i<server.args(); i++){
    if(server.argName(i) == "ApiMethod"){
      saveStringToFile(server.arg(i),"api.method");
    }
    if(server.argName(i) == "ApiEndpoint"){
      saveStringToFile(server.arg(i),"api.endpoint");
    }
    if(server.argName(i) == "ApiHeaders"){
      saveStringToFile(server.arg(i),"api.headers");
    }
    if(server.argName(i) == "ApiPost"){
      saveStringToFile(server.arg(i),"api.post");
    }
    if(server.argName(i) == "ApiInterval"){
      saveStringToFile(server.arg(i),"api.interval");
    }
  }
}

void getApiSettings(){
  String sMessage = "{\"method\":\"";
  sMessage += readStringFromFile("api.method");
  sMessage += "\",\"endpoint\":\"";
  sMessage += readStringFromFile("api.endpoint");
  sMessage += "\",\"headers\":\"";
  sMessage += readStringFromFile("api.headers");
  sMessage += "\",\"post\":\"";
  sMessage += readStringFromFile("api.post");
  sMessage += "\",\"interval\":\"";
  sMessage += readStringFromFile("api.interval");
  sMessage += "\"}";
  server.send(200,"text/json",sMessage);
}

void WifiTryConnect(){
  String sPasswd = "";
  char charBufsPasswd[50];
  String sWifiName = "";
  char charBufsWifiName[50];

  sWifiName = readStringFromFile("wifi.name");
  sPasswd = readStringFromFile("wifi.pw");
  
  Serial.println("got networkname: "+sWifiName+" and password: "+sPasswd);
  // convert "String" into char[]... i hate c++
  sPasswd.toCharArray(charBufsPasswd, 50);
  sWifiName.toCharArray(charBufsWifiName, 50);

  WiFi.begin(charBufsWifiName,charBufsPasswd);
  g_bConnectCanel = false;
}

String saveWifiSettings(){
  String sPasswd = "";
  String sName = "";
  
  for (uint8_t i=0; i<server.args(); i++){
    if(server.argName(i) == "WifiName"){
      sName = server.arg(i);
    }
   if(server.argName(i) == "WifiPass"){
      sPasswd = server.arg(i);
    }
  }

  if(sName.length() == 0){
    return "Wifiname can't be empty!";
  }

  // save settings to files
  if(!saveStringToFile(sName,"wifi.name")){
     Serial.println("failed to save wifi.name");
     return"failed to save wifi.name" ;
  }
  Serial.println("saved wifi.name");
  
  if(!saveStringToFile(sPasswd,"wifi.pw")){
     Serial.print("failed to save wifi.pw");
     return"failed to save wifi.pw" ;
  }
  Serial.println("saved wifi.pw");

  g_bTryConnect = 1;
  
  return "Saved";
}


void handleNotFound(){
  INACIVITY_RESET

  if(SPIFFS.exists(server.uri())){
    sendFile();
    return;
  }
  if(server.uri()=="/"){
    sendFile("/index.html");
    return;
  }
    
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

void sendFile(String sFile){
  String sFileToSend = sFile;
  if(sFile.length() == 0)
    sFileToSend = server.uri();
    
  Serial.println("trying to send file: "+sFileToSend);
  File f = SPIFFS.open(sFileToSend,"r");
  if(!f){
    Serial.println("failed to open "+sFileToSend);
    return;
  }

  if(sFileToSend.endsWith("html")){
    server.streamFile(f,"text/html");     
  }else if(sFileToSend.endsWith("css")){
    server.streamFile(f,"text/css");    
  }else if(sFileToSend.endsWith("js")){
    server.streamFile(f,"text/javascript");    
  }
  f.close();
}
void getNetworkStatus(){
  IPAddress ip = WiFi.localIP();
  String sMessage = "{\"state\":\"";

  switch(WiFi.status())
  {
    case WL_NO_SHIELD: sMessage+= "No Wifi shiled";  break;
    case WL_IDLE_STATUS: sMessage+= "Idle";  break;
    case WL_NO_SSID_AVAIL: sMessage+= "SSID not available";  break;
    case WL_SCAN_COMPLETED: sMessage+= "Scan done";  break;
    case WL_CONNECTED: sMessage+= "Connected";  break;
    case WL_CONNECT_FAILED: sMessage+= "Connection failed";  break;
    case WL_CONNECTION_LOST: sMessage+= "Connected lost";  break;
    case WL_DISCONNECTED: sMessage+= "Not connected";  break;
  }
  sMessage+="\",\"ip\":\"";
  for(int i=0;i<4;i++){
    sMessage+=ip[i];
    if(i<3)
      sMessage+="."; 
  }
  sMessage+="\"}";
  server.send(200,"text/json",sMessage);
}
void getSensor(){
  String sMessage = "{\"sensor\":\"DS18B20\",\"value\":\"";
  float fVcc=ESP.getVcc()/1024.00f;

  sensors.requestTemperatures(); // Send the command to get temperatures
  sMessage += sensors.getTempCByIndex(0);
  sMessage += "°C\",\"battery\":\"";
  sMessage += String(fVcc)+"V";
  sMessage += "\"}";
  server.send(200,"text/plain",sMessage);
}

void getNetworks(){
  Serial.println("scan start");
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  String sMessage = "";
  for(int i=0;i<n;i++){
    sMessage += WiFi.SSID(i) +"\n";
  }
  server.send(200,"text/plain",sMessage);
}
void setupWebserver(){
  
  server.on("/factoryReset", factoryReset);
  server.on("/goToSleep", goToSleep);
  server.on("/setApiRequest", setApiRequest);
  
  server.on("/getApiSettings", getApiSettings);
  server.on("/saveApiSettings", saveApiSettings);
  
  server.on("/saveWifiSettings", saveWifiSettings);
  server.on("/WifiTryConnect", WifiTryConnect);
  server.on("/getNetworkStatus", getNetworkStatus);
  server.on("/getNetworks", getNetworks);
  server.on("/getSensor", getSensor);
  
//  server.on("/index.html",sendFile);
  
  server.onNotFound(handleNotFound);
  server.begin();
}

void checkForDeepSleep(){
  String sDeepSleep = readStringFromFile("deepsleep");

  if(!sDeepSleep.startsWith("1"))
    return;

  Serial.println("deepSleep active");

  // wait 2seconds for a buttonpress to reset from deepsleep
  for(int i=0;i<200;i++){
    delay(10);
    if(digitalRead(0) == 0){
      Serial.println("Abroting deepsleep");
      SPIFFS.remove("deepsleep");
      return;
    }
  }
  
  g_bDeepSleep = true;
  
  WiFi.hostname(HOSTNAME);
  WiFi.mode(WIFI_AP);
  WifiTryConnect();
  int i=0;
  while (WiFi.status() != WL_CONNECTED) {
    i++;
    Serial.println(WiFi.status());
    delay(1000);
    if(i>= 30){ // abrot after ~30s
      Serial.println("Abroting connect");
      goToSleep();
    }
  }
  Serial.println("connected!");
  setApiRequest();
  goToSleep();
}

void setup(void){
  Serial.begin(115200);
//  Serial.setDebugOutput(true);
  g_ulStartTime = millis();
  SPIFFS.begin();
  checkForDeepSleep();
  
  INACIVITY_RESET
  setupWebserver();

  // setup accesspoint
  WiFi.hostname(HOSTNAME);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(g_apIP, g_apIP, g_netMsk);
  char sAPName[100] = "";
  getDefaultApName(sAPName);
  WiFi.softAP(sAPName, APDEFAULTPW, 11, false);

  // start DNS server
  dnsServer.start(DNS_PORT, "*", g_apIP);

  if (MDNS.begin(HOSTNAME)) {
    Serial.println("MDNS responder started");
  }
}


void loop(void){
  server.handleClient();
  dnsServer.processNextRequest();
  if(g_bTryConnect == 1){
      INACIVITY_RESET
      g_bTryConnect = 0;
      WifiTryConnect();
  }

  // disconnect wifi after 20s of trying to connect
  // trying to connect blocks the webserver...
  if(WiFi.status() != WL_CONNECTED && g_bConnectCanel != true){ 
    if((millis() - g_ulStartTime) >= 20000){
      Serial.println("abroting connect..");
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);
      g_bConnectCanel = true;
    }
  }else{
    g_ulStartTime = millis();
  }

  // "shutdown" after 5minutes of inactivity
  if((millis() - g_ulInactivTime) >= 5*60*1000){
    shutdown();
  }  
}
