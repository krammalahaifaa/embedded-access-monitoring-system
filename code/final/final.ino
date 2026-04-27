#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "DHT.h"  
#include "SPIFFS.h"


MFRC522DriverPinSimple ss_pin(5);
MFRC522DriverSPI driver{ss_pin};
MFRC522 mfrc522{driver};
LiquidCrystal_I2C lcd(0x3F, 16, 2);

#define LED_red 26
#define LED_orange 27
#define DOOR_BUTTON 4
#define ledPin 14
#define fanPin 33
#define FP_RX 13
#define FP_TX 15
#define DHTPIN 32
#define DHTTYPE DHT11


Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);

const int led_channel = 0;
const int fan_channel = 1;
const int PWM_freq = 5000;
const int PWM_res = 10; 

const char* ssid = "haifaa";
const char* password = "haifaa17";

DHT dht(DHTPIN, DHTTYPE);

WebServer server(80);

struct Employee {
  String rfidUID;
  String name;
  uint8_t fingerID;  
};

Employee employees[] = {
  {"B7 B0 B7 89", "haifaa", 2},
  {"A1 C5 13 1D", "keltoum", 1},
};

const int numEmployees = sizeof(employees) / sizeof(employees[0]);

String authorizedNames[] = {
  "haifaa"  
};

const int numAuthorized = sizeof(authorizedNames) / sizeof(authorizedNames[0]);

bool fingerOK = false;
String currentName = "";
uint8_t currentFingerID = 0;
unsigned long lastActivity = 0;
const unsigned long AUTH_TIMEOUT = 15000;

struct AccessLog {
  String timestamp;
  String uid;
  String name;
  String status;
};

const int MAX_LOGS = 100;
AccessLog accessLogs[MAX_LOGS];
int logIndex = 0;
int totalLogs = 0;
int totalEntered = 0;

void handleHome();
void handleAccessHistory();
void handleSensors();
void handleControl();
void handleData();
String getUID();
String findEmployee(String uid);
bool checkAccess(String name);
String getTime();
void logAccess(String time, String uid, String name, bool granted);
void logAccessStatus(String time, String uid, String name, String status);

struct UIDStatus {
  String uid;
  bool inside;  // true = la personne est à l'intérieur
};

UIDStatus uidStatus[50]; // 50 max, ou le nombre d'employés max

  float temperature = NAN;
  float humidity = NAN;
  unsigned long lastDHTRead = 0;
  const unsigned long DHT_INTERVAL = 2000;


void setup() {
  pinMode(LED_red, OUTPUT);
  pinMode(LED_orange, OUTPUT);
  pinMode(DOOR_BUTTON, INPUT);
  digitalWrite(LED_red, LOW);
  digitalWrite(LED_orange, LOW);
  
   ledcAttach(ledPin, PWM_freq, PWM_res);
   ledcAttach(fanPin, PWM_freq, PWM_res);
   ledcWrite(ledPin, 0);
   ledcWrite(fanPin, 0);
  
  Serial.begin(115200);
  Serial2.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  delay(100);

  if (!finger.verifyPassword()) {
    lcd.clear();
    lcd.print("Fingerprint Error");
    Serial.println("Fingerprint sensor not found!");
    digitalWrite(LED_red, HIGH);
    while(1);
}

  Serial.println("Fingerprint sensor ready");

  Wire.begin(21, 22);  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi");
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  lcd.clear();

  configTime(3600, 0, "pool.ntp.org");
  Serial.println("Time synchronized via NTP");

  dht.begin();
  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS failed");
    return;
  }
  
  server.on("/", handleHome);
  server.on("/access", handleAccessHistory);
  server.on("/sensors", handleSensors);
  server.on("/control", handleControl);
  server.on("/data", handleData);
  
  server.on("/brightness", []() {
  if (server.hasArg("value")) {
    int val = constrain(server.arg("value").toInt(), 0, 100);
    int pwmVal = map(val, 0, 100, 0, 1023);
    ledcWrite(ledPin, pwmVal);
  }
  server.send(200, "text/plain", "OK");
});

  server.on("/speed", []() {
  if (server.hasArg("value")) {
    int val = constrain(server.arg("value").toInt(), 0, 100);
    int pwmVal = map(val, 0, 100, 0, 1023);
    ledcWrite(fanPin, pwmVal);
  }
  server.send(200, "text/plain", "OK");
});

  server.on("/", HTTP_GET,handleHome) ;
  
  server.on("/logo.png", HTTP_GET, [](){
    File file = SPIFFS.open("/logo.png", "r");
    server.streamFile(file, "image/png");
    file.close();
  });


  server.begin();
  Serial.println("HTTP server started");
  
  mfrc522.PCD_Init();
  Serial.println(F("Scan PICC to see UID and check-in"));
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ready");
  lcd.setCursor(0, 1);
  lcd.print("Scan card");
}

void loop() {
  server.handleClient();
  if (millis() - lastDHTRead >= DHT_INTERVAL) {
  lastDHTRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity = h;
  }
}
  
   
  if (millis() - lastActivity > AUTH_TIMEOUT) {
    fingerOK = false;
    currentName = "";
  }

  if (!fingerOK) {
    lcd.clear();
    lcd.print("Place Finger");

    int getImageResult = finger.getImage();
    if (getImageResult != FINGERPRINT_OK) {
      if (getImageResult == FINGERPRINT_NOFINGER) {
        return;
      }
      lcd.clear();
      lcd.print("Image Error");
      digitalWrite(LED_red, HIGH);
      delay(1000);
      digitalWrite(LED_red, LOW);
      return;
    }

    int image2TzResult = finger.image2Tz();
    if (image2TzResult != FINGERPRINT_OK) {
      lcd.clear();
      lcd.print("Bad Quality");
      digitalWrite(LED_red, HIGH);
      delay(2000);
      digitalWrite(LED_red, LOW);
      return;
    }

    int fingerSearchResult = finger.fingerSearch();
    if (fingerSearchResult != FINGERPRINT_OK) {
  lcd.clear();
  lcd.print("No Match");

  String currentTime = getTime();
  logAccessStatus(
    currentTime,
    "N/A",
    "UNKNOWN FINGERPRINT",
    "UNKNOWN"
  );

  digitalWrite(LED_red, HIGH);
  delay(2000);
  digitalWrite(LED_red, LOW);
  return;
}


    int idx = -1;
    for (int i = 0; i < numEmployees; i++) {
      if (employees[i].fingerID == finger.fingerID) {
        idx = i;
        break;
      }
    }

    if (idx < 0) {
      lcd.clear();
      lcd.print("Not Registered");
      String currentTime = getTime();
      // LOG UNKNOWN FINGERPRINT to web
      logAccessStatus(currentTime, "Finger ID: " + String(finger.fingerID), "UNKNOWN FINGERPRINT", "UNKNOWN");
      digitalWrite(LED_red, HIGH);
      delay(2000);
      digitalWrite(LED_red, LOW);
      return;
    }

    currentName = employees[idx].name;
    currentFingerID = finger.fingerID;
    fingerOK = true;
    lastActivity = millis();

    // DON'T LOG successful fingerprint scan to web - only on LCD
    lcd.clear();
    lcd.print("Finger OK");
    lcd.setCursor(0, 1);
    lcd.print(currentName);

    digitalWrite(LED_orange, HIGH);
    delay(1000);
    digitalWrite(LED_orange, LOW);

    lcd.clear();
    lcd.print("Scan RFID Card");
  }

  if (fingerOK && mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uid = getUID();
    String checkInTime = getTime();
    String employeeName = findEmployee(uid);
    bool hasAccess = checkAccess(employeeName);

    Serial.println("====================");
    Serial.println("Card Detected!");
    Serial.println("UID: " + uid);
    Serial.println("Employee: " + employeeName);
    Serial.println("Access: " + String(hasAccess ? "GRANTED" : "DENIED"));
    Serial.println("Time: " + checkInTime);
    Serial.println("====================");

    // CASE 1: UNKNOWN RFID (not in database)
    if (employeeName == "Unknown") {
      logAccessStatus(checkInTime, uid, "UNKNOWN RFID", "DENIED");
      digitalWrite(LED_red, HIGH);
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Unknown Card");
      lcd.setCursor(0,1);
      lcd.print("Access Denied");
      delay(3000);
      digitalWrite(LED_red, LOW);
      fingerOK = false;
    }
    // CASE 2: DENIED - RFID doesn't match current fingerprint
    else if (employeeName != currentName) {
      logAccessStatus(checkInTime, uid, currentName + " (Wrong Card)", "DENIED");
      digitalWrite(LED_red, HIGH);
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Card Mismatch");
      lcd.setCursor(0,1);
      lcd.print("Access Denied");
      delay(3000);
      digitalWrite(LED_red, LOW);
      fingerOK = false;
    }
    // CASE 3: DENIED - RFID matches but no access permission
    else if (!hasAccess) {
      logAccessStatus(checkInTime, uid, employeeName , "DENIED");
      digitalWrite(LED_red, HIGH);
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print(employeeName);
      lcd.setCursor(0,1);
      lcd.print("No Permission");
      delay(3000);
      digitalWrite(LED_red, LOW);
      fingerOK = false;
    }
    
    // CASE 4: GRANTED - Everything matches and has permission
else {
    // Cherche si l'UID est déjà dans uidStatus
    bool isInside = false;
    int statusIndex = -1;
    for (int i = 0; i < 50; i++) {
        if (uidStatus[i].uid == uid) {
            isInside = uidStatus[i].inside;
            statusIndex = i;
            break;
        }
    }

    // Si UID pas encore trouvé, on l’ajoute
    if (statusIndex == -1) {
        for (int i = 0; i < 50; i++) {
            if (uidStatus[i].uid == "") {
                uidStatus[i].uid = uid;
                uidStatus[i].inside = false; // initialement dehors
                statusIndex = i;
                break;
            }
        }
    }

    // Alterne IN / OUT
    String accessType;
    if (!isInside) {
        uidStatus[statusIndex].inside = true; // maintenant à l'intérieur
        totalEntered++;
        accessType = "GRANTED IN";
    } else {
        uidStatus[statusIndex].inside = false; // maintenant dehors
        totalEntered--;
        accessType = "GRANTED OUT";
    }

    // Log et affichage LCD
    logAccessStatus(checkInTime, uid, employeeName, accessType);

    digitalWrite(LED_red, LOW);
    digitalWrite(LED_orange, HIGH);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(employeeName + " " + accessType);
    lcd.setCursor(0,1);
    lcd.print("Scan Complete");

    while (digitalRead(DOOR_BUTTON) == HIGH) { delay(10); }

    digitalWrite(LED_orange, LOW);
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Ready");
    lcd.setCursor(0,1);
    lcd.print("Scan card");

 fingerOK = false;

}

     
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Ready");
    lcd.setCursor(0,1);
    lcd.print("Scan card");
  }

void logAccess(String time, String uid, String name, bool granted) {
  logAccessStatus(time, uid, name, granted ? "GRANTED" : "DENIED");
}

void logAccessStatus(String time, String uid, String name, String status) {
  accessLogs[logIndex].timestamp = time;
  accessLogs[logIndex].uid = uid;
  accessLogs[logIndex].name = name;
  accessLogs[logIndex].status = status;
  
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (totalLogs < MAX_LOGS){
    totalLogs++;
  }
}


 // Home page
void handleHome() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
        <head>
            <meta charset='UTF-8'>
            <meta name='viewport' content='width=device-width, initial-scale=1.0'>
            <title>Access Control System</title>
            <style>
                *{
                margin:0;
                padding:0;
                box-sizing:border-box;
                }

                body{
                font-family:'Segoe UI',Arial,sans-serif;
                background:#f4f6f8;
                color:#333;
                padding:20px;
                }

                .top-bar{
                display:flex;
                justify-content:space-between;
                align-items:center;
                background:#2c3e50;
                padding:12px 16px;
                border-radius:8px;
                margin-bottom:20px;
                }

                .nav a{
                color:white;
                text-decoration:none;
                background:#34495e;
                padding:8px 14px;
                border-radius:4px;
                margin-right:10px;
                }

                .nav a:hover{
                background:#1abc9c;
                }

                .image-container img { 
                    max-width: 100%; 
                    width: 120px; 
                    height: auto; 
                }
                
                .content{
                background:white;
                padding:25px;
                border-radius:8px;
                box-shadow:0 4px 15px rgba(0,0,0,0.1);
                display:flex;
                justify-content:space-between;
                align-items:center;
                }

                h1,h2{
                color:#2c3e50;
                margin-bottom:10px;
                }

                .Welcome{
                    max-width: 65%;
                }

                .Welcome h1 {
                    font-weight: 600;
                    letter-spacing: 0.5px;
                }

                .features {
                    list-style: none;
                    margin-top: 10px;
                    padding-left: 0;
                }

                .features li {
                    padding-left: 22px;
                    margin-bottom: 8px;
                    position: relative;
                    font-size: 0.95em;
                }

                .features li::before {
                    content: "▸";
                    position: absolute;
                    left: 0;
                    color: #1abc9c;
                    font-weight: bold;
                }

                #clock {
                    font-size: clamp(1.6rem, 4vw, 3rem);
                    font-weight: 600;
                    color: #1abc9c;
                    padding: clamp(12px, 2vw, 22px) clamp(18px, 3vw, 30px);
                    border-radius: 10px;
                    box-shadow:15px 4px 15px rgba(0,0,0,0.1);
                    min-width: clamp(120px, 20vw, 180px);
                    text-align: center;
                }
                
                 @media (max-width: 600px) {
                body {
                    padding: 10px;
                }

                .top-bar {
                    flex-direction: column;
                     gap: 10px;
                    text-align: center;
                }

                .nav {
                    display: flex;
                    flex-wrap: wrap;
                    justify-content: center;
                }

                .nav a {
                    margin: 5px;
                }
                
                .content {
                    padding: 15px;
                }
            }
            

            </style>
            <script>
                function updateClock() {
                    const now = new Date(); // Get current time
                    const hours = String(now.getHours()).padStart(2, '0');
                    const minutes = String(now.getMinutes()).padStart(2, '0');
                    const seconds = String(now.getSeconds()).padStart(2, '0');
    
                    const timeString = `${hours}:${minutes}:${seconds}`;
                    document.getElementById('clock').textContent = timeString;
                }

                setInterval(updateClock, 1000);
                window.onload = updateClock;
            </script>

        </head>
        <body>
            <div class='top-bar'>
                <div class='nav'>
                    <a href='/access'>Access History</a>
                    <a href='/sensors'>Sensors</a>
                    <a href='/control'>Control</a>
                </div>
                <div class="image-container"> 
                    <img src="/logo.png" alt="mobilis"> 
                </div>
            </div>
           <div class='content'>
            <div class="Welcome">
               <h1>Room Monitoring System </h1>
               <p>This system manages and monitors authorized access to
                critical infrastructure rooms within the sites.
            </p>
               <p style="margin-top: 13px;">Functions include:</p>
               <ul class="features">
                    <li>Double-check access authentication</li>
                    <li>Real-time access logging</li>
                    <li>Environmental supervision</li>
                    <li>Remote control of room equipment</li>
               </ul>
               </div>
               <div><span id="clock">--:--:--</span></div>
           </div>
           


     </body>
    </html>
)rawliteral";
  server.send(200, "text/html", html);
}


  // Access History Page
void handleAccessHistory() {
  String html = R"rawliteral(
<!DOCTYPE html>
    <html>
        <head>
            <meta charset='UTF-8'>
            <meta name='viewport' content='width=device-width, initial-scale=1.0'>
            <title>Access History</title>
            <style>
                *{ 
                margin:0;
                padding:0;
                box-sizing:border-box;
                }

                body{
                font-family:'Segoe UI',Arial,sans-serif;
                background:#f4f6f8;
                color:#333;
                padding:20px;
                }

                .top-bar{
                display:flex;
                justify-content:space-between;
                align-items:center;
                background:#2c3e50;
                padding:12px 16px;
                border-radius:8px;
                margin-bottom:20px;
                }

                .nav a{
                color:white;
                text-decoration:none;
                background:#34495e;
                padding:8px 14px;
                border-radius:4px;
                margin-right:10px;
                }
          
                .nav a:hover{
                background:#1abc9c;
                }

                .image-container img { 
                    max-width: 100%; 
                    width: 120px; 
                    height: auto; 
                }

                .content{
                background:white;
                padding:25px;
                border-radius:8px;
                box-shadow:0 4px 15px rgba(0,0,0,0.1);
                }

                h1,h2{
                color:#2c3e50;
                margin-bottom:10px;
                }

                table{
                width:100%;
                border-collapse:collapse;
                margin-top:15px;
                }

                th,td{
                padding:12px;
                text-align:left;
                border-bottom:1px solid #eee;
                }

                thead{
                background:#34495e;
                color:white;
                }

                .status{
                padding:4px 10px;
                border-radius:20px;
                font-size:12px;
                font-weight:bold;
                display:inline-block;
                }

                .granted{
                background:#d4edda;
                color:#155724;
                }

                .denied{
                background:#f8d7da;
                color:#856404;
                }

                .unknown{
                background:#fff3cd;
                color:#721c24;
                }
                 @media (max-width: 600px) {
                body {
                    padding: 10px;
                }

                .top-bar {
                    flex-direction: column;
                     gap: 10px;
                    text-align: center;
                }

                .nav {
                    display: flex;
                    flex-wrap: wrap;
                    justify-content: center;
                }

                .nav a {
                    margin: 5px;
                }
                
                .content {
                    padding: 15px;
                }
            }
            
            </style>
            <script>
                function updateData() {
                    fetch('/data').then(r=>r.json()).then(data=>{
                        let tbody=document.querySelector('tbody');
                        tbody.innerHTML='';
                       data.logs.forEach(log=>{
                          let statusClass;
                          if (log.status.startsWith('GRANTED')) statusClass = 'granted';
                          else if (log.status === 'DENIED') statusClass = 'denied';
                          else statusClass = 'unknown';
    
                          tbody.innerHTML+=`<tr>
                          <td>${log.timestamp}</td>
                          <td>${log.name}</td>
                          <td>${log.uid}</td>
                          <td><span class='status ${statusClass}'>${log.status}</span></td>
                          </tr>`;
                        });

                    document.getElementById('peopleCount').textContent=data.peopleInside;
                    });
                }
                setInterval(updateData,2000);
                window.onload=updateData;
                </script>
        </head>
        <body>
            <div class='top-bar'>
                <div class='nav'>
                    <a href='/access'>Access History</a>
                    <a href='/sensors'>Sensors</a>
                    <a href='/control'>Control</a>
                </div>
                <div class="image-container"> 
                    <img src="/logo.png" alt="mobilis"> 
                </div>
            </div>
            <div class='content'>
                <h2>Access History</h2>
                <div style="margin-bottom:15px;"> inside :  <span id="peopleCount">0</span></div>
                <table>
                <thead><tr><th>Time</th><th>Name</th><th>UID</th><th>Status</th></tr></thead>
                <tbody></tbody>
                </table>
            </div>
        </body>
    </html>
)rawliteral";
  server.send(200, "text/html", html);
}

// Sensors Page
void handleSensors() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Sensors</title>

    <style>
        *{
            margin:0;
            padding:0;
            box-sizing:border-box;
        }

        body{
            font-family:'Segoe UI',Arial,sans-serif;
            background:#f4f6f8;
            color:#333;
            padding:20px;
        }

        /* ===== TOP BAR ===== */
        .top-bar{
            display:flex;
            justify-content:space-between;
            align-items:center;
            background:#2c3e50;
            padding:12px 16px;
            border-radius:8px;
            margin-bottom:20px;
        }

        .nav a{
            color:white;
            text-decoration:none;
            background:#34495e;
            padding:8px 14px;
            border-radius:4px;
            margin-right:10px;
            font-size:14px;
        }

        .nav a:hover{
            background:#1abc9c;
        }

        .image-container img { 
                    max-width: 100%; 
                    width: 120px; 
                    height: auto; 
                }
        
        }

        /* ===== CONTENT ===== */
        .content{
            background:white;
            padding:25px;
            border-radius:8px;
            box-shadow:0 4px 15px rgba(0,0,0,0.1);
        }

        h2{
            color:#2c3e50;
            margin-bottom:20px;
        }

        /* ===== SENSOR GRID ===== */
        .sensor-grid{
            display:grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap:20px;
        }

        .sensor-card{
            background:#ecf0f1;
            padding:20px;
            border-radius:10px;
            text-align:center;
            box-shadow:0 2px 8px rgba(0,0,0,0.1);
        }

        .sensor-title{
            font-size:16px;
            color:#7f8c8d;
            margin-bottom:10px;
        }

        .sensor-value{
            font-size:42px;
            font-weight:bold;
            color:#2c3e50;
        }

        .unit{
            font-size:18px;
            color:#7f8c8d;
        }

        .sensor-card.alert {
        background: #e74c3c;
        color: white;
        }

        .sensor-card.alert .sensor-title,
        .sensor-card.alert .unit {
            color: #fceae9;
        }


        /* ===== RESPONSIVE ===== */
        @media (max-width:600px){
            body{padding:10px;}
            .top-bar{
                flex-direction:column;
                gap:10px;
                text-align:center;
            }
            .nav{
                display:flex;
                flex-wrap:wrap;
                justify-content:center;
            }
            .nav a{
                margin:5px;
            }
        }
    </style>
</head>

<body>

<div class='top-bar'>
                <div class='nav'>
                    <a href='/access'>Access History</a>
                    <a href='/sensors'>Sensors</a>
                    <a href='/control'>Control</a>
                </div>
                <div class="image-container"> 
                    <img src="/logo.png" alt="mobilis"> 
                </div>
            </div>

<div class='content'>
    <h2>Room Sensors</h2>

    <div class="sensor-grid">
        <div class="sensor-card" id="tempCard">
            <div class="sensor-title">Temperature</div>
            <div class="sensor-value">
                <span id="temperature">--</span>
                <span class="unit">°C</span>
            </div>
        </div>

        <div class="sensor-card" id="humCard">
            <div class="sensor-title">Humidity</div>
            <div class="sensor-value">
                <span id="humidity">--</span>
                <span class="unit">%</span>
            </div>
        </div>
    </div>
</div>

<script>
const TEMP_LIMIT = 26;   // LOW on purpose (lab test)
const HUM_LIMIT  = 60;   // LOW on purpose

function updateSensors(){
    fetch('/data')
        .then(res => res.json())
        .then(data => {

            // ---- TEMPERATURE ----
            if (data.temperature !== null && data.temperature !== undefined) {
                document.getElementById('temperature').textContent = data.temperature;

                if (data.temperature > TEMP_LIMIT)
                    document.getElementById('tempCard').classList.add('alert');
                else
                    document.getElementById('tempCard').classList.remove('alert');
            }

            // ---- HUMIDITY ----
            if (data.humidity !== null && data.humidity !== undefined) {
                document.getElementById('humidity').textContent = data.humidity;

                if (data.humidity > HUM_LIMIT)
                    document.getElementById('humCard').classList.add('alert');
                else
                    document.getElementById('humCard').classList.remove('alert');
            }
        });
}

setInterval(updateSensors, 2000);
updateSensors();
</script>


</body>
</html>

)rawliteral";
  server.send(200, "text/html", html);
}


// Control Page
void handleControl() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Room Control</title>
    <style>
        *{margin:0;padding:0;box-sizing:border-box;}

         body{font-family:'Segoe UI',Arial,sans-serif;background:#f4f6f8;color:#333;padding:20px;}

         .top-bar{display:flex;justify-content:space-between;align-items:center;background:#2c3e50;padding:12px 16px;border-radius:8px;margin-bottom:20px;}


         .nav a{
            color:white;
            text-decoration:none;
            background:#34495e;
            padding:8px 14px;
            border-radius:4px;
            margin-right:10px;
        }

         .nav a:hover{
            background:#1abc9c;
        }

         .image-container img { 
                    max-width: 100%; 
                    width: 120px; 
                    height: auto; 
                }

         .content{
            background:white;
            padding:25px;
            border-radius:8px;
            box-shadow:0 4px 15px rgba(0,0,0,0.1);
        }

        h1,h2{
            color:#2c3e50;
            margin-bottom:10px;
        }

        button{
            padding:10px 20px;
            border:none;
            border-radius:6px;
            background: #2c3e50;
            color:white;
            margin:5px;
            cursor:pointer;
        }

        button:hover{
            background:#1abc9c;
        }
        
        .fan,.brightness {
            margin-top: 10px;
            background:white;
            padding:25px;
            border-radius:8px;
            box-shadow:0 4px 15px rgba(0,0,0,0.1);
            display: flex;
            flex-direction:column;
            align-items: center;
        }

        #pad1, #pad2 {
            position: relative;
            width: 100%;
            max-width: 400px;
            height: 24px;
            background: #eee;
            border-radius: 20px;
            margin-top: 15px;
            touch-action: none;
        }

        #indicator1,#indicator2 {
            position: absolute;
            width: 20px;
            height: 20px;
            background: #2c3e50;
            border-radius: 50%;
            top: 50%;
            transform: translateY(-50%);
            left: 0%;
        }

        #indicator1:hover{
            background:#1abc9c;
        }

        #indicator2:hover{
            background:#1abc9c;
        }

        #label1,#label2 {
            margin-top: 20px;
            font-size: 1.5em;
        }
        @media (max-width: 600px) {

    body {
        padding: 10px;
    }

    .top-bar {
        flex-direction: column;
        gap: 10px;
        text-align: center;
    }

    .nav {
        display: flex;
        flex-wrap: wrap;
        justify-content: center;
    }

    .nav a {
        margin: 5px;
    }

    .content {
        padding: 15px;
    }

    h2 {
        font-size: 1.2em;
        text-align: center;
    }

    #label1, #label2 {
        font-size: 1.2em;
        text-align: center;
    }
     #indicator1, #indicator2 {
        width: 26px;
        height: 26px;
    }
}

    </style>
</head>
<body>
    <div class='top-bar'>
                <div class='nav'>
                    <a href='/access'>Access History</a>
                    <a href='/sensors'>Sensors</a>
                    <a href='/control'>Control</a>
                </div>
                <div class="image-container"> 
                    <img src="/logo.png" alt="mobilis"> 
                </div>
            </div>
    <div class="brightness">
        <h2>Light brightness Control</h2>
        <div id="pad1">
            <div id="indicator1"></div>
        </div>
        <div id="label1">0%</div>
        </div>
        <div class="fan">
            <h2>Fan speed Control</h2>
            <div id="pad2">
                <div id="indicator2"></div>
            </div>
            <div id="label2">0%</div>
        </div>

<script>
  const pad1 = document.getElementById("pad1");
  const indicator1 = document.getElementById("indicator1");
  const label1 = document.getElementById("label1");
  let brightness = 0; 

    let lastBrightnessSend = 0;

function sendBrightness(value){
  const now = Date.now();
  if (now - lastBrightnessSend > 50) { // 50 ms
    fetch(`/brightness?value=${value}`);
    lastBrightnessSend = now;
  }
}


  function updateBrightnessFromPosition(x){
    const rect = pad1.getBoundingClientRect();
    let pos = x - rect.left;
    pos = Math.max(0, Math.min(rect.width, pos));

    
    brightness = Math.round((pos / rect.width) * 100);

    
    const indicator1Width = indicator1.offsetWidth;
    let indicator1Pos = pos - indicator1Width/2;
    indicator1Pos = Math.max(0, Math.min(rect.width - indicator1Width, indicator1Pos));
    indicator1.style.left = indicator1Pos + "px";

    
    label1.textContent = brightness + "%";

    
    sendBrightness(brightness);
  }

  
  pad1.addEventListener("touchmove", e => {
    e.preventDefault();
    updateBrightnessFromPosition(e.touches[0].clientX);
  });

  pad1.addEventListener("touchstart", e => {
    e.preventDefault();
    updateBrightnessFromPosition(e.touches[0].clientX);
  });

  // Mouse events
  pad1.addEventListener("mousedown", e => {
    updateBrightnessFromPosition(e.clientX);
    function move(e){ updateBrightnessFromPosition(e.clientX); }
    document.addEventListener("mousemove", move);
    document.addEventListener("mouseup", ()=>{document.removeEventListener("mousemove", move)});
  });
</script>

<script>
  const pad2 = document.getElementById("pad2");
const indicator2 = document.getElementById("indicator2");
const label2 = document.getElementById("label2");
let speed = 0; 

let lastspeedSend = 0;

function sendspeed(value){
  const now = Date.now();
  if (now - lastspeedSend > 50) { 
    fetch(`/speed?value=${value}`);
    lastspeedSend = now;
  }
}


function updatespeedFromPosition(x){
    const rect = pad2.getBoundingClientRect();
    let pos = x - rect.left;
    pos = Math.max(0, Math.min(rect.width, pos));

    speed = Math.round((pos / rect.width) * 100);

    const indicator2Width = indicator2.offsetWidth;
    let indicator2Pos = pos - indicator2Width/2;
    indicator2Pos = Math.max(0, Math.min(rect.width - indicator2Width, indicator2Pos));
    indicator2.style.left = indicator2Pos + "px";

    label2.textContent = speed + "%";

    sendspeed(speed);
}

pad2.addEventListener("touchmove", e => {
    e.preventDefault();
    updatespeedFromPosition(e.touches[0].clientX);
});

pad2.addEventListener("touchstart", e => {
    e.preventDefault();
    updatespeedFromPosition(e.touches[0].clientX);
});

pad2.addEventListener("mousedown", e => {
    updatespeedFromPosition(e.clientX);
    function move(e){ updatespeedFromPosition(e.clientX); }
    document.addEventListener("mousemove", move);
    document.addEventListener("mouseup", ()=>{document.removeEventListener("mousemove", move)});
});

</script>

</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}


void handleData() {
 String json = "{";
  json += "\"peopleInside\":" + String(totalEntered) + ",";
  json += "\"temperature\":" + (isnan(temperature) ? "null" : String(temperature,1)) + ",";
  json += "\"humidity\":" + (isnan(humidity) ? "null" : String(humidity,1)) + ",";
  json += "\"logs\":[";
  
  
  int start = (totalLogs < MAX_LOGS) ? 0 : logIndex;
  int count = totalLogs;
  
  for (int i = 0; i < count; i++) {
    int idx = (start + count - 1 - i) % MAX_LOGS;
    if (i > 0) json += ",";
    json += "{";
    json += "\"timestamp\":\"" + accessLogs[idx].timestamp + "\",";
    json += "\"uid\":\"" + accessLogs[idx].uid + "\",";
    json += "\"name\":\"" + accessLogs[idx].name + "\",";
    json += "\"status\":\"" + accessLogs[idx].status + "\"";
    json += "}";
  }
  
  json += "]}";
  server.send(200, "application/json", json);
}

  
int findEmployeeByFinger(uint8_t fid) {
    for (int i = 0; i < numEmployees; i++) {
        if (employees[i].fingerID == fid) return i;
    }
    return -1;
}

void checkFingerprint() {
    lcd.clear();
    lcd.print("Place Finger");

    if (finger.getImage() != FINGERPRINT_OK) return;
    if (finger.image2Tz() != FINGERPRINT_OK) return;
    if (finger.fingerSearch() != FINGERPRINT_OK) {
        lcd.clear();
        lcd.print("Finger Denied");
        digitalWrite(LED_red, HIGH);
        delay(2000);
        digitalWrite(LED_red, LOW);
        return;
    }

    int idx = findEmployeeByFinger(finger.fingerID);
    if (idx < 0) {
        lcd.clear();
        lcd.print("Unknown Finger");
        digitalWrite(LED_red, HIGH);
        delay(2000);
        digitalWrite(LED_red, LOW);
        return;
    }

    currentName = employees[idx].name;
    currentFingerID = finger.fingerID;

    fingerOK = true;
    lastActivity = millis();

    lcd.clear();
    lcd.print("Finger OK");
    lcd.setCursor(0, 1);
    lcd.print(currentName);

    digitalWrite(LED_orange, HIGH);
    delay(1000);
    digitalWrite(LED_orange, LOW);

    lcd.clear();
    lcd.print("Scan RFID Card");
}

String getUID() {
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
    if (i < mfrc522.uid.size - 1) uid += " ";
  }
  uid.toUpperCase();
  return uid;
}

String findEmployee(String uid) {
  for (int i = 0; i < numEmployees; i++) {
    if (employees[i].rfidUID == uid) {
      return employees[i].name;
    }
  }
  return "Unknown";
}


bool checkAccess(String name) {
  for (int i = 0; i < numAuthorized; i++) {
    if (authorizedNames[i] == name) {
      return true;
    }
  }
  return false;
}


// ---------- NTP REAL TIME ----------
String getTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "No Time";
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}
