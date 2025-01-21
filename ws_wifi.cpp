#include "ws_wifi.h"

// The name and password of the WiFi access point
const char *ssid = APSSID;                
const char *password = APPSK;               
IPAddress apIP(10, 10, 10, 1);    // Set the IP address of the AP

char ipStr[16];
WebServer server(80);                               

void handleRoot() {
String myhtmlPage =
    String("") +
    "<!DOCTYPE html>\n" +
    "<html>\n" +
    "<head>\n" +
    "    <meta charset=\"utf-8\">\n" +
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n" +
    "    <title>UNANGEBRACHT</title>\n" +
    "    <style>\n" +
    "        /* Existing styles */\n" +
    "        body {\n" +
    "            font-family: Arial, sans-serif;\n" +
    "            background-color: #f0f0f0;\n" +
    "            margin: 0;\n" +
    "            padding: 0;\n" +
    "        }\n" +
    "        .header {\n" +
    "            text-align: center;\n" +
    "            padding: 20px 0;\n" +
    "            background-color: #333;\n" +
    "            color: #fff;\n" +
    "            margin-bottom: 20px;\n" +
    "        }\n" +
    "        .container {\n" +
    "            max-width: 800px; /* Increased width to accommodate new sections */\n" +
    "            margin: 0 auto;\n" +
    "            padding: 20px;\n" +
    "            background-color: #fff;\n" +
    "            border-radius: 5px;\n" +
    "            box-shadow: 0 0 5px rgba(0, 0, 0, 0.3);\n" +
    "        }\n" +
    "        .input-container, .sent-strings-container, .pre-made-container {\n" +
    "            display: flex;\n" +
    "            flex-direction: column;\n" +
    "            align-items: center;\n" +
    "            margin-bottom: 20px;\n" +
    "        }\n" +
    "        .input-container label {\n" +
    "            width: 100%;\n" +
    "            text-align: center;\n" +
    "            margin-bottom: 10px;\n" +
    "        }\n" +
    "        .input-container input[type=\"text\"] {\n" +
    "            flex: 1;\n" +
    "            padding: 5px;\n" +
    "            border: 1px solid #ccc;\n" +
    "            border-radius: 3px;\n" +
    "            margin-bottom: 10px;\n" +
    "            width: 80%;\n" +
    "        }\n" +
    "        .radio-buttons, .pre-made-buttons {\n" +
    "            display: flex;\n" +
    "            justify-content: center;\n" +
    "            flex-wrap: wrap;\n" +
    "            gap: 8px;\n" +
    "            margin-bottom: 10px;\n" +
    "        }\n" +
    "        .radio-buttons button, .pre-made-buttons button {\n" +
    "            padding: 5px 10px;\n" +
    "            background-color: #333;\n" +
    "            color: #fff;\n" +
    "            font-size: 12px;\n" +
    "            font-weight: bold;\n" +
    "            border: none;\n" +
    "            border-radius: 2px;\n" +
    "            text-transform: uppercase;\n" +
    "            cursor: pointer;\n" +
    "        }\n" +
    "        .radio-buttons button:hover, .pre-made-buttons button:hover {\n" +
    "            background-color: #555;\n" +
    "        }\n" +
    "        .input-container button, .button-container button {\n" +
    "            padding: 5px 10px;\n" +
    "            background-color: #333;\n" +
    "            color: #fff;\n" +
    "            font-size: 14px;\n" +
    "            font-weight: bold;\n" +
    "            border: none;\n" +
    "            border-radius: 3px;\n" +
    "            text-transform: uppercase;\n" +
    "            cursor: pointer;\n" +
    "            margin-right: 5px;\n" +
    "        }\n" +
    "        .sent-strings-list {\n" +
    "            width: 80%;\n" +
    "            border: 1px solid #ccc;\n" +
    "            border-radius: 3px;\n" +
    "            padding: 10px;\n" +
    "            max-height: 150px;\n" +
    "            overflow-y: auto;\n" +
    "        }\n" +
    "    </style>\n" +
    "</head>\n" +
    "<body>\n" +
    "    <script defer=\"defer\">\n" +
    "        var selectedColor = '';\n" +
    "        var sentStrings = [];\n" +
    "        var preMadeStrings = [];\n" +
    "\n" +
    "        function setColor(color) {\n" +
    "            selectedColor = color;\n" +
    "            sendData();\n" +
    "        }\n" +
    "\n" +
    "        function sendData() {\n" +
    "            var textData = document.getElementById('Text').value;\n" +
    "            textData = selectedColor + textData;\n" +
    "            var xhr = new XMLHttpRequest();\n" +
    "            xhr.open('GET', '/SendData?data=' + encodeURIComponent(textData), true);\n" +
    "            xhr.send();\n" +
    "            fetchSentStrings(); // Refresh the sent strings list\n" +
    "        }\n" +
    "\n" +
    "        function clearText() {\n" +
    "            document.getElementById('Text').value = '';\n" +
    "            selectedColor = '';\n" +
    "            sendData();\n" +
    "        }\n" +
    "\n" +
    "        function fetchSentStrings() {\n" +
    "            var xhr = new XMLHttpRequest();\n" +
    "            xhr.open('GET', '/getData', true);\n" +
    "            xhr.onreadystatechange = function() {\n" +
    "                if (xhr.readyState === 4 && xhr.status === 200) {\n" +
    "                    sentStrings = JSON.parse(xhr.responseText);\n" +
    "                    displaySentStrings();\n" +
    "                }\n" +
    "            };\n" +
    "            xhr.send();\n" +
    "        }\n" +
    "\n" +
    "        function fetchPreMadeStrings() {\n" +
    "            var xhr = new XMLHttpRequest();\n" +
    "            xhr.open('GET', '/getPreMade', true);\n" +
    "            xhr.onreadystatechange = function() {\n" +
    "                if (xhr.readyState === 4 && xhr.status === 200) {\n" +
    "                    preMadeStrings = JSON.parse(xhr.responseText);\n" +
    "                    displayPreMadeStrings();\n" +
    "                }\n" +
    "            };\n" +
    "            xhr.send();\n" +
    "        }\n" +
    "\n" +
    "        function displaySentStrings() {\n" +
    "            var list = document.getElementById('sentStringsList');\n" +
    "            list.innerHTML = '';\n" +
    "            sentStrings.forEach(function(str, index) {\n" +
    "                var option = document.createElement('div');\n" +
    "                option.style.marginBottom = '5px';\n" +
    "                option.innerHTML = `<input type=\"checkbox\" id=\"sent${index}\" value=\"${str}\">\n" +
    "                                    <label for=\"sent${index}\">${str}</label>`;\n" +
    "                list.appendChild(option);\n" +
    "            });\n\n" +
    "        }\n" +
    "\n" +
    "        function displayPreMadeStrings() {\n" +
    "            var container = document.getElementById('preMadeList');\n" +
    "            container.innerHTML = '';\n" +
    "            preMadeStrings.forEach(function(str) {\n" +
    "                var button = document.createElement('button');\n" +
    "                button.textContent = str;\n" +
    "                button.onclick = function() {\n" +
    "                    document.getElementById('Text').value = str;\n" +
    "                };\n" +
    "                container.appendChild(button);\n" +
    "            });\n" +
    "        }\n" +
    "\n" +
    "        window.onload = function() {\n" +
    "            fetchSentStrings();\n" +
    "            fetchPreMadeStrings();\n" +
    "        };\n" +
    "    </script>\n" +
    "    <div class=\"header\">\n" +
    "        <h1>UNANGEBRACHT</h1>\n" +
    "    </div>\n" +
    "    <div class=\"container\">\n" +
    "        <div class=\"input-container\">\n" +
    "            <label for=\"input1\">Text</label>\n" +
    "            <input type=\"text\" id=\"Text\" />\n" +
    "            <div class=\"radio-buttons\">\n" +
    "                <button onclick=\"setColor('@red')\" style=\"background-color: red;\">Red</button>\n" +
    "                <button onclick=\"setColor('@green')\" style=\"background-color: green;\">Green</button>\n" +
    "                <button onclick=\"setColor('@blue')\" style=\"background-color: blue;\">Blue</button>\n" +
    "                <button onclick=\"setColor('@pink')\" style=\"background-color: pink; color: #333;\">Pink</button>\n" +
    "                <button onclick=\"setColor('@yellow')\" style=\"background-color: yellow; color: #333;\">Yellow</button>\n" +
    "                <button onclick=\"setColor('@cyan')\" style=\"background-color: cyan; color: #333;\">Cyan</button>\n" +
    "            </div>\n" +
    "            <button value=\"SendData\" id=\"btn1\" onclick=\"sendData()\">Anzeigen</button>\n" +
    "            <button value=\"ClearText\" id=\"btnClear\" onclick=\"clearText()\">Clear</button>\n" +
    "        </div>\n" +
    "        <div class=\"sent-strings-container\">\n" +
    "            <h3>Last Sent Strings:</h3>\n" +
    "            <div id=\"sentStringsList\" class=\"sent-strings-list\">\n" +
    "                <!-- Sent strings will be populated here -->\n" +
    "            </div>\n" +
    "        </div>\n" +
    "        <div class=\"pre-made-container\">\n" +
    "            <h3>Pre-Made Strings:</h3>\n" +
    "            <div id=\"preMadeList\" class=\"pre-made-buttons\">\n" +
    "                <!-- Pre-made strings buttons will be populated here -->\n" +
    "            </div>\n" +
    "        </div>\n" +
    "        <div class=\"button-container\">\n" +
    "            <button value=\"RGBOn\" id=\"btn2\" onclick=\"ledSwitch(2)\">RGB On</button>\n" +
    "            <button value=\"RGBOff\" id=\"btn3\" onclick=\"ledSwitch(3)\">RGB Off</button>\n" +
    "        </div>\n" +
    "    </div>\n" +
    "</body>\n" +
    "</html>\n" +
    "";


  server.send(200, "text/html", myhtmlPage); 
  printf("Home page visited\r\n");
  
}

void handleGetData() {
  String json = "[";
  for (int i = 0; i < sentCount; i++) {
    int index = (sentIndex - sentCount + i + MAX_SENT_STRINGS) % MAX_SENT_STRINGS;
    json += "\"" + sentStrings[index] + "\"";
    if (i < sentCount - 1) {
      json += ",";
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSwitch(uint8_t ledNumber) {
  switch(ledNumber){
    case 1:
      if (server.hasArg("data")) {
        String newData = server.arg("data");
        newData.toCharArray(Text, sizeof(Text));

        // Store the newData in the sentStrings buffer
        sentStrings[sentIndex] = newData;
        sentIndex = (sentIndex + 1) % MAX_SENT_STRINGS;
        if (sentCount < MAX_SENT_STRINGS) {
          sentCount++;
        }
      }
      Flow_Flag = true;
      printf("Text=%s.\r\n",Text);
      break;
    case 2:
      colorWipe(Matrix.Color(0, 255, 0), 0);
      printf("RGB On.\r\n");
      Flow_Flag = false;
      break;
    case 3:
      colorWipe(Matrix.Color(0, 0, 0), 0);
      printf("RGB Off.\r\n");
      Flow_Flag = true;
      break;
  }
  server.send(200, "text/plain", "OK");
}
void handleSendData()  { handleSwitch(1); }
void handleRGBOn()     { handleSwitch(2); }
void handleRGBOff()    { handleSwitch(3); }


void WIFI_Init()
{

  WiFi.mode(WIFI_AP); 
  while(!WiFi.softAP(ssid, password)) {
    printf("Soft AP creation failed.\r\n");
    printf("Try setting up the WIFI again.\r\n");
  } 
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0)); // Set the IP address and gateway of the AP
  
  IPAddress myIP = WiFi.softAPIP();
  uint32_t ipAddress = WiFi.softAPIP();
  printf("AP IP address: ");
  sprintf(ipStr, "%d.%d.%d.%d", myIP[0], myIP[1], myIP[2], myIP[3]);
  printf("%s\r\n", ipStr);

  server.on("/", handleRoot);
  server.on("/getData"  , handleGetData);
  server.on("/SendData" , handleSendData);
  server.on("/register", handleRegister);
  server.on("/RGBOn"       , handleRGBOn);
  server.on("/RGBOff"      , handleRGBOff);

  // Endpoint to retrieve pre-made strings
  server.on("/getPreMade", HTTP_GET, [](){
      String json = "[";
      for (int i = 0; i < PREMADE_COUNT; i++) {
          json += "\"" + String(preMadeStrings[i]) + "\"";
          if (i < PREMADE_COUNT - 1) {
              json += ",";
          }
      }
      json += "]";
      server.send(200, "application/json", json);
  });

  server.begin(); 
  printf("Web server started\r\n");
}

void WIFI_Loop()
{
  server.handleClient(); // Processing requests from clients
}
















