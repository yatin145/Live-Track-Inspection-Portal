#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h> // 🕒 Time library added for IST

// =========================================================================
// 🟢 CONFIGURATIONS & PINS
// =========================================================================
const int PWR_PIN = 10;
const int RX_PIN = 18;
const int TX_PIN = 17;
HardwareSerial modem(2);

const char* wifi_ssid = "Net";
const char* wifi_password = "Yatindas";
const char* apn = "airtelgprs.com"; 

String storageBucket = "ultrasonic-final-demo.firebasestorage.app"; 

// =========================================================================
// 🟢 DATA STRUCTURES & OFFLINE QUEUE (SMART FALLBACK ENABLED)
// =========================================================================
struct __attribute__((packed)) DefectPayload {
    char operator_name[12]; 
    float chainage;         
    uint8_t direction;      
    char defect_type[4];    
    double latitude;    // 👈 UPGRADED TO DOUBLE FOR GPS ACCURACY     
    double longitude;   // 👈 UPGRADED TO DOUBLE FOR GPS ACCURACY      
    uint32_t timestamp;     
    uint32_t encoder_val;   
    char machine_id[12];    
}; 

// 🚨 Replaced constant with dynamic variable to allow fallback sizing
int maxQueueSize = 20000; 
DefectPayload* offlineQueue = nullptr; // Pointer for memory allocation
int queueCount = 0;

byte wifiArray[44]; 
String syncedNumbers[5]; 
int totalSyncedContacts = 0;

char currentOperator[12] = "";
float startChainage = 0.0;
uint8_t currentDirection = 0; 
String devices[] = {"DEVICE_1", "DEVICE_2"}; // Dual Device Target
String currentFolder = "F_001"; 

unsigned int loopCounter = 0;

// Variables to store real-time GPS data
double currentLat = 0.0; // 👈 UPGRADED TO DOUBLE
double currentLng = 0.0; // 👈 UPGRADED TO DOUBLE

// Helper to flush and wait for command responses
String sendAT(String command, const unsigned long timeout) {
    while(modem.available()) modem.read();
    String response = "";
    modem.println(command);
    unsigned long startTime = millis();
    while ((millis() - startTime) < timeout) {
        while (modem.available()) response += (char)modem.read();
    }
    return response;
}

// 🚨 LIVE HANDSHAKE SMS UTILITY
void sendSMS(String number, String text) {
    number.replace(" ", "");
    number.replace("-", "");
    if(number.length() < 5) return;
    
    Serial.println("\n[SMS ENGINE] Sending to: " + number);
    sendAT("AT+CMGF=1", 1500);
    
    while(modem.available()) modem.read();
    modem.print("AT+CMGS=\"" + number + "\"\r");
    
    unsigned long promptStart = millis();
    bool readyToType = false;
    while(millis() - promptStart < 4000) {
        if(modem.available()) {
            char c = modem.read();
            if(c == '>') { readyToType = true; break; }
        }
    }
    
    if(readyToType) {
        modem.print(text);
        delay(500);
        modem.write(26); 
        unsigned long deliveryStart = millis();
        while(millis() - deliveryStart < 8000) { while(modem.available()) modem.read(); }
        Serial.println("[SMS DELIVERED]");
    } else {
        Serial.println("[ERROR] Modem rejected SMS prompt.");
    }
}

// 🌐 FETCH SYNCED CONTACTS
void fetchContactsFromCloud() {
    Serial.println("[CLOUD] Syncing phonebook...");
    String url = "https://firebasestorage.googleapis.com/v0/b/" + storageBucket + "/o/DEVICE_1%2Fcontacts.json?alt=media";
    
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(url);
        if (http.GET() == 200) {
            String payload = http.getString();
            DynamicJsonDocument doc(1024);
            deserializeJson(doc, payload);
            JsonArray arr = doc.as<JsonArray>();
            totalSyncedContacts = 0;
            for(JsonObject obj : arr) {
                if(totalSyncedContacts < 5) {
                    syncedNumbers[totalSyncedContacts] = obj["phone"].as<String>();
                    totalSyncedContacts++;
                }
            }
        }
        http.end();
    }
}

// =========================================================================
// 🌍 HIGH-ACCURACY GPS FUNCTION
// =========================================================================
void fetchGPS(double &lat, double &lng) { // 👈 PARAMETERS UPGRADED TO DOUBLE
    String response = sendAT("AT+QGPSLOC=2", 2000); 
    
    int idx = response.indexOf("+QGPSLOC:");
    if (idx != -1) {
        int commas[11];
        int currentComma = 0;
        int startFind = idx;
        
        while (currentComma < 11) {
            int nextComma = response.indexOf(',', startFind);
            if (nextComma == -1) break;
            commas[currentComma++] = nextComma;
            startFind = nextComma + 1;
        }

        if (currentComma >= 3) {
            lat = response.substring(commas[0] + 1, commas[1]).toDouble();
            lng = response.substring(commas[1] + 1, commas[2]).toDouble();
            Serial.println("🌍 [GPS] Location Locked: " + String(lat, 7) + ", " + String(lng, 7));
        }
    } 
    else if (response.indexOf("516") >= 0) {
        Serial.println("⏳ [GPS] Searching satellites...");
    } 
    else {
        Serial.println("⏳ [GPS] Waiting for satellite fix...");
    }
}

// 📡 UPLOAD WAVEFORM TO BOTH DEVICES
bool generateAndUploadWaveform() {
    for(int i=0; i<44; i++) { wifiArray[i] = random(10, 250); }

    bool success = false;
    for(int i = 0; i < 2; i++) {
        String path = devices[i] + "%2F" + currentFolder + "%2Fwaveform.bin";
        String url = "https://firebasestorage.googleapis.com/v0/b/" + storageBucket + "/o?name=" + path;
        
        sendAT("AT+QHTTPURL=" + String(url.length()) + ",30", 2000);
        modem.print(url); delay(1000);
        sendAT("AT+QHTTPPOST=" + String(44) + ",60,60", 3000);
        delay(500); modem.write(wifiArray, 44);
        
        unsigned long waitS = millis();
        while(millis() - waitS < 4000) { while(modem.available()) modem.read(); }
        String resp = sendAT("AT+QHTTPSTOP", 1000);
        if(resp.indexOf("OK") != -1) success = true; 
        Serial.println("-> Waveform sent to " + devices[i]);
    }
    return success;
}

// 📡 UPLOAD STRUCTURE TO BOTH DEVICES
bool uploadStructToFirebase(DefectPayload payload) {
    bool success = false;
    String payloadString = "Op: " + String(payload.operator_name) + ", Ch: " + String(payload.chainage) + 
                           ", Enc: " + String(payload.encoder_val) + ", Lat: " + String(payload.latitude, 7) + 
                           ", Lng: " + String(payload.longitude, 7) + ", Time: " + String(payload.timestamp);
                           
    for(int i = 0; i < 2; i++) {
        String path = devices[i] + "%2F" + currentFolder + "%2Fstructuredata.txt";
        String url = "https://firebasestorage.googleapis.com/v0/b/" + storageBucket + "/o?name=" + path;
        
        sendAT("AT+QHTTPURL=" + String(url.length()) + ",30", 2000);
        modem.print(url); delay(1000);
        sendAT("AT+QHTTPPOST=" + String(payloadString.length()) + ",60,60", 3000);
        delay(500); modem.print(payloadString);
        
        unsigned long waitS = millis();
        while(millis() - waitS < 4000) { while(modem.available()) modem.read(); }
        String resp = sendAT("AT+QHTTPSTOP", 1000);
        if(resp.indexOf("OK") != -1) success = true;
        Serial.println("-> Structure sent to " + devices[i]);
    }
    return success;
}

// 📱 UPDATED SMS TRIGGER FUNCTION
void triggerDefectSMS(DefectPayload payload) {
    String mapsLink = "http://maps.google.com/?q=" + String(payload.latitude, 7) + "," + String(payload.longitude, 7);
    String dirStr = (payload.direction == 1) ? "UP" : "DOWN";
    
    time_t t = payload.timestamp;
    struct tm *timeinfo = localtime(&t);
    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%d/%m/%Y %H:%M:%S", timeinfo);

    String msg = "⚠️ DEFECT ALERT ⚠️\n"
                 "M/C: " + String(payload.machine_id) + "\n" // 👈 Yeh ab dynamic ho chuka hai!
                 "Op: " + String(payload.operator_name) + "\n"
                 "Type: " + String(payload.defect_type) + "\n"
                 "Ch: " + String(payload.chainage, 2) + " (" + dirStr + ")\n"
                 "Enc: " + String(payload.encoder_val) + "mm\n"
                 "Time: " + String(timeStr) + " IST\n"
                 "Loc: " + mapsLink;
                 
    Serial.println("\n[🚨 TRIGGERING SMS]:\n" + msg);
    for(int i = 0; i < totalSyncedContacts; i++) { sendSMS(syncedNumbers[i], msg); }
}

// 🌐 PROCESS OFFLINE QUEUE
void processOfflineQueue() {
    if(queueCount > 0 && WiFi.status() == WL_CONNECTED) {
        Serial.println("\n📶 [NETWORK RECONNECTED] Sending buffer data first...");
        for(int i = 0; i < queueCount; i++) {
            Serial.println("-> Uploading queued item " + String(i + 1) + " of " + String(queueCount) + "...");
            uploadStructToFirebase(offlineQueue[i]);
            if(String(offlineQueue[i].defect_type) != "N/A") {
                // Offline data ke liye bhi dono devices ke SMS trigger karega
                for(int d = 0; d < 2; d++) {
                    strncpy(offlineQueue[i].machine_id, devices[d].c_str(), sizeof(offlineQueue[i].machine_id) - 1);
                    triggerDefectSMS(offlineQueue[i]);
                }
            }
        }
        queueCount = 0; 
        Serial.println("✅ Queue got emptied, now sending current data...");
    }
}

void getOperatorInput() {
    Serial.println("\n=== TELEMETRY CONSOLE BOOT ===");
    Serial.println("Enter Operator Name:");
    while(!Serial.available()) { delay(5); }
    String opName = Serial.readStringUntil('\n'); opName.trim();
    strncpy(currentOperator, opName.c_str(), sizeof(currentOperator) - 1);
    Serial.println("=> Operator Registered: " + String(currentOperator));
    
    Serial.println("Enter Starting Chainage:");
    while(!Serial.available()) { delay(5); }
    startChainage = Serial.readStringUntil('\n').toFloat();
    Serial.println("=> Start Chainage: " + String(startChainage));
    
    Serial.println("Enter Direction (1=UP, 0=DOWN):");
    while(!Serial.available()) { delay(5); }
    currentDirection = (uint8_t)Serial.readStringUntil('\n').toInt();
    Serial.println("=> Direction Set: " + String(currentDirection == 1 ? "UP (Chainage will Increase)" : "DOWN (Chainage will Decrease)"));
}

void setup() {
    Serial.begin(115200);

    Serial.println("[SYSTEM] Attempting to allocate 20,000 slots in PSRAM...");
    offlineQueue = (DefectPayload*) ps_malloc(maxQueueSize * sizeof(DefectPayload));
    
    if (offlineQueue == nullptr) {
        Serial.println("⚠️ PSRAM Allocation Failed! Board might not have PSRAM or it's disabled.");
        Serial.println("🔄 Falling back to standard internal SRAM with a smaller queue...");
        
        maxQueueSize = 500; 
        offlineQueue = (DefectPayload*) malloc(maxQueueSize * sizeof(DefectPayload));
        
        if (offlineQueue == nullptr) {
            Serial.println("❌ CRITICAL ERROR: Standard RAM Allocation Failed too! Halting system.");
            while(true) { delay(100); } 
        }
        Serial.printf("✅ Fallback RAM Allocated! Queue size reduced to %d slots to prevent crash.\n", maxQueueSize);
    } else {
        Serial.println("✅ PSRAM Successfully Allocated (~880 KB reserved)!");
    }

    Serial.println("[HARDWARE] Toggling Modem...");
    pinMode(PWR_PIN, OUTPUT); digitalWrite(PWR_PIN, HIGH); delay(2000); digitalWrite(PWR_PIN, LOW); delay(5000);                 
    modem.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN); 
    
    getOperatorInput();

    WiFi.begin(wifi_ssid, wifi_password);
    while (WiFi.status() != WL_CONNECTED) { delay(200); }

    Serial.println("[SYSTEM] Syncing time with NTP (IST)...");
    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

    fetchContactsFromCloud();

    sendAT("AT", 1500); sendAT("AT+CPIN?", 1500); 
    sendAT("AT+QICSGP=1,1,\"" + String(apn) + "\",\"\",\"\",1", 2000); sendAT("AT+QIACT=1", 6000);  
    
    Serial.println("[GPS] Powering up GNSS Module in High Accuracy Mode...");
    sendAT("AT+QGPSCFG=\"gnssconfig\",5", 2000); 
    sendAT("AT+QGPSCFG=\"sbasenable\",1", 2000); 
    delay(1000);
    sendAT("AT+QGPS=1", 2000);
}

void loop() { 
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
    }

    processOfflineQueue(); 

    loopCounter++;
    Serial.println("\n==================================");
    Serial.println("🔄 STARTING CONTINUOUS CYCLE #" + String(loopCounter));
    
    fetchGPS(currentLat, currentLng);

    float distanceTravelled = loopCounter * 3.5;
    float activeChainage = (currentDirection == 1) ? (startChainage + distanceTravelled) : (startChainage - distanceTravelled);
    uint32_t activeEncoder = (uint32_t)(distanceTravelled * 1000); 
    
    time_t now;
    time(&now);

    DefectPayload payload;
    memset(&payload, 0, sizeof(DefectPayload));
    strncpy(payload.operator_name, currentOperator, sizeof(payload.operator_name) - 1);
    
    payload.chainage = activeChainage;
    payload.direction = currentDirection;
    payload.latitude = currentLat;  
    payload.longitude = currentLng; 
    payload.encoder_val = activeEncoder;
    payload.timestamp = now; 
    
    if(loopCounter % 3 == 0) { strncpy(payload.defect_type, "IMR", sizeof(payload.defect_type) - 1); } 
    else { strncpy(payload.defect_type, "N/A", sizeof(payload.defect_type) - 1); }

    if(WiFi.status() == WL_CONNECTED) {
        generateAndUploadWaveform();
        uploadStructToFirebase(payload);
        
        // 🚨 NAYA LOGIC: Har 3rd structure par dono devices ke liye alag alag SMS trigger hoga!
        if(loopCounter % 3 == 0) { 
            for(int i = 0; i < 2; i++) {
                // Payload ki machine_id ko dynamically change kar rahe hain loop ke hisab se
                strncpy(payload.machine_id, devices[i].c_str(), sizeof(payload.machine_id) - 1);
                triggerDefectSMS(payload); 
                delay(1000); // Do SMS ke beech thoda safe gap
            }
        }
    } else {
        Serial.println("❌ NETWORK DOWN! Net not available, storing it in queue...");
        if(queueCount < maxQueueSize) {
            // Offline queue ke liye hum safe side ke liye DEVICE_1 default rakh rahe hain, sync par handle ho jayega
            strncpy(payload.machine_id, devices[0].c_str(), sizeof(payload.machine_id) - 1);
            offlineQueue[queueCount] = payload;
            queueCount++;
            Serial.println("📦 Status: (" + String(queueCount) + "/" + String(maxQueueSize) + ") items stored in queue.");
        } else {
            Serial.println("⚠️ Queue Full! Maximum limit (" + String(maxQueueSize) + ") reached. Cannot store more.");
        }
    }
    
    Serial.println("⏳ Cycle complete. Waiting 12 seconds...");
    delay(12000);
}