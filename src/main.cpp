#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>

#define BUZZER 26

WiFiManager wm;
WebServer server(80);

// ===== buffer ảnh =====
uint8_t *imgBuffer = nullptr;
size_t imgSize = 0;

String resultText = "Waiting image...";

// =========================
// HTML (KHÔNG NHẢY TRANG)
// =========================
const char html[] PROGMEM = R"rawliteral(
<h2>ESP32 Fire Detection</h2>

<input type="file" id="file">
<button onclick="uploadFile()">Upload</button>

<h3 id="r">Waiting...</h3>

<script>

function uploadFile() {
  let file = document.getElementById("file").files[0];
  let formData = new FormData();
  formData.append("image", file);

  fetch("/upload", {
    method: "POST",
    body: formData
  });
}

// realtime result
setInterval(()=>{
 fetch('/result')
 .then(r=>r.text())
 .then(t=>document.getElementById('r').innerHTML=t);
},500);

</script>
)rawliteral";

// =========================
// FAKE FIRE SCORE
// =========================
float fakeFireScore()
{
  long sum = 0;

  for (size_t i = 0; i < imgSize && i < 20000; i++)
  {
    sum += imgBuffer[i];
  }

  float avg = sum / (float)(imgSize + 1);

  return avg / 255.0;  // 0 → 1
}

// =========================
// UPLOAD HANDLER
// =========================
void handleUpload()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.println("UPLOAD START");

    if (imgBuffer) free(imgBuffer);

    imgBuffer = (uint8_t *)malloc(50000);
    imgSize = 0;
  }

  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    memcpy(imgBuffer + imgSize, upload.buf, upload.currentSize);
    imgSize += upload.currentSize;
  }

  else if (upload.status == UPLOAD_FILE_END)
  {
    Serial.println("UPLOAD DONE");

    float score = fakeFireScore() * 100.0;

    Serial.print("Fire %: ");
    Serial.println(score);

    if (score > 40.0)
    {
      digitalWrite(BUZZER, HIGH);   // kêu liên tục
      resultText = "🔥 FIRE DETECTED | " + String(score) + "%";
    }
    else
    {
      digitalWrite(BUZZER, LOW);
      resultText = "SAFE | " + String(score) + "%";
    }
  }
}

// =========================
void setup()
{
  Serial.begin(115200);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  wm.autoConnect("ESP32-SETUP");

  Serial.println(WiFi.localIP());

  // web
  server.on("/", []()
            { server.send(200, "text/html", html); });

  server.on("/upload", HTTP_POST,
            []()
            { server.send(200, "text/plain", "OK"); },
            handleUpload);

  server.on("/result", []()
            { server.send(200, "text/plain", resultText); });

  server.begin();

  Serial.println("SYSTEM READY");
}

// =========================
void loop()
{
  server.handleClient();
}