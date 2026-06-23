#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <TJpg_Decoder.h>
#include <Fire_Detection_ESP32-CAM_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "edge-impulse-sdk/porting/ei_classifier_porting.h"

// ===== OVERRIDE EDGE IMPULSE MALLOC =====
// Đảm bảo Edge Impulse sử dụng PSRAM cho tensor arena nếu có
void *ei_malloc(size_t size) {
    if (psramFound()) {
        void *ptr = ps_malloc(size);
        if (ptr) return ptr;
    }
    return malloc(size);
}

void *ei_calloc(size_t nitems, size_t size) {
    if (psramFound()) {
        void *ptr = ps_calloc(nitems, size);
        if (ptr) return ptr;
    }
    return calloc(nitems, size);
}

void ei_free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

#define BUZZER 26

WiFiManager wm;
WebServer server(80);

// ===== buffer ảnh =====
uint8_t *imgBuffer = nullptr;
size_t imgSize = 0;
size_t imgMax = 150000; // Sẽ được cập nhật tùy theo PSRAM có hay không


uint8_t *dec_buf = nullptr;
uint16_t dec_w = 0;
uint16_t dec_h = 0;

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!dec_buf) return false;
  for (int16_t j = 0; j < h; j++) {
    for (int16_t i = 0; i < w; i++) {
      int16_t dx = x + i;
      int16_t dy = y + j;
      if (dx < dec_w && dy < dec_h) {
        uint16_t color = bitmap[j * w + i];
        uint8_t r = (color & 0xF800) >> 8;
        uint8_t g = (color & 0x07E0) >> 3;
        uint8_t b = (color & 0x001F) << 3;
        int idx = (dy * dec_w + dx) * 3;
        dec_buf[idx] = r;
        dec_buf[idx+1] = g;
        dec_buf[idx+2] = b;
      }
    }
  }
  return true;
}

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
  let fileInput = document.getElementById("file");
  if (!fileInput.files || fileInput.files.length === 0) return;
  let file = fileInput.files[0];

  document.getElementById('r').innerHTML = "Processing image...";

  let reader = new FileReader();
  reader.onload = function(e) {
    let img = new Image();
    img.onload = function() {
      let canvas = document.createElement("canvas");
      let ctx = canvas.getContext("2d");
      
      // Giảm kích thước ảnh xuống đúng bằng input của AI để tiết kiệm tối đa RAM trên ESP32 (không có PSRAM)
      let max_size = 96;
      let w = img.width;
      let h = img.height;
      if (w > max_size || h > max_size) {
        if (w > h) {
          h = Math.round((h * max_size) / w);
          w = max_size;
        } else {
          w = Math.round((w * max_size) / h);
          h = max_size;
        }
      }
      
      canvas.width = w;
      canvas.height = h;
      ctx.drawImage(img, 0, 0, w, h);
      
      // Nén ảnh thành JPEG
      canvas.toBlob(function(blob) {
        let formData = new FormData();
        formData.append("image", blob, "image.jpg");
        fetch("/upload", {
          method: "POST",
          body: formData
        }).then(()=> {
            document.getElementById('r').innerHTML = "Uploaded, waiting for result...";
        });
      }, "image/jpeg", 0.7);
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
}

// realtime result
setInterval(()=>{
 fetch('/result')
 .then(r=>r.text())
 .then(t=> {
    if(t !== "Waiting image...") {
        document.getElementById('r').innerHTML=t;
    }
 });
},500);

</script>
)rawliteral";

// =========================
// AI inference is executed in handleUpload()
// =========================
// UPLOAD HANDLER
// =========================
void handleUpload()
{
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.println("UPLOAD START");

    if (imgBuffer) {
        free(imgBuffer);
        imgBuffer = nullptr;
    }

    if (psramFound()) {
      imgMax = 150000; // 150KB for PSRAM
      imgBuffer = (uint8_t *)ps_malloc(imgMax);
    } else {
      imgMax = 25000; // Rút xuống 25KB cho ảnh cực nhỏ, tiết kiệm RAM
      imgBuffer = (uint8_t *)malloc(imgMax);
    }
    imgSize = 0;
    
    if (!imgBuffer) {
        Serial.println("ERR: Out of memory for imgBuffer!");
        resultText = "System Error: Out of memory";
    }
  }

  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (!imgBuffer) return; // Prevent crash if malloc failed

    if (imgSize + upload.currentSize <= imgMax) {
      memcpy(imgBuffer + imgSize, upload.buf, upload.currentSize);
      imgSize += upload.currentSize;
    } else {
      Serial.println("Upload exceeds buffer size!");
    }
  }

  else if (upload.status == UPLOAD_FILE_END)
  {
    Serial.println("UPLOAD DONE");
    if (!imgBuffer) return; // Prevent crash if malloc failed

    uint16_t orig_w = 0, orig_h = 0;
    TJpgDec.getJpgSize(&orig_w, &orig_h, imgBuffer, imgSize);
    
    if (orig_w == 0 || orig_h == 0) {
        Serial.println("Invalid JPEG");
        resultText = "Invalid JPEG format";
        if (imgBuffer) { free(imgBuffer); imgBuffer = nullptr; }
        return;
    }

    int max_dim = orig_w > orig_h ? orig_w : orig_h;
    uint8_t scale = 1;
    if (max_dim >= 800) scale = 8;
    else if (max_dim >= 400) scale = 4;
    else if (max_dim >= 200) scale = 2;
    
    TJpgDec.setJpgScale(scale);
    dec_w = orig_w / scale;
    dec_h = orig_h / scale;
    
    if (psramFound()) {
        dec_buf = (uint8_t *)ps_malloc(dec_w * dec_h * 3);
    } else {
        dec_buf = (uint8_t *)malloc(dec_w * dec_h * 3);
    }
    if (!dec_buf) {
        Serial.println("Out of memory for decoding");
        resultText = "System Error: Out of memory";
        if (imgBuffer) { free(imgBuffer); imgBuffer = nullptr; }
        return;
    }

    TJpgDec.setCallback(tft_output);
    TJpgDec.drawJpg(0, 0, imgBuffer, imgSize);

    // Calculate the required size for ei_buf to prevent buffer overflow.
    // Edge Impulse SDK uses dstImage (ei_buf) as a workspace for the cropped image before scaling.
    size_t ei_buf_size = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3;
    size_t crop_workspace_size = dec_w * dec_h * 3;
    if (crop_workspace_size > ei_buf_size) {
        ei_buf_size = crop_workspace_size;
    }

    uint8_t *ei_buf = nullptr;
    if (psramFound()) {
        ei_buf = (uint8_t *)ps_malloc(ei_buf_size);
    } else {
        ei_buf = (uint8_t *)malloc(ei_buf_size);
    }
    if (!ei_buf) {
        free(dec_buf); dec_buf = nullptr;
        Serial.println("Out of memory for resizing");
        resultText = "System Error: Out of memory";
        if (imgBuffer) { free(imgBuffer); imgBuffer = nullptr; }
        return;
    }

    ei::image::processing::crop_and_interpolate_rgb888(
        dec_buf, dec_w, dec_h,
        ei_buf, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

    free(dec_buf); dec_buf = nullptr;

    float *features = nullptr;
    if (psramFound()) {
        features = (float *)ps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * sizeof(float));
    } else {
        features = (float *)malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * sizeof(float));
    }
    if (!features) {
        free(ei_buf);
        Serial.println("Out of memory for features");
        resultText = "System Error: Out of memory";
        if (imgBuffer) { free(imgBuffer); imgBuffer = nullptr; }
        return;
    }

    for (int i = 0; i < EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT; i++) {
        uint8_t r = ei_buf[i * 3];
        uint8_t g = ei_buf[i * 3 + 1];
        uint8_t b = ei_buf[i * 3 + 2];
        features[i] = (r << 16) | (g << 8) | b;
    }
    free(ei_buf);

    ei::signal_t signal;
    int err = numpy::signal_from_buffer(features, EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT, &signal);

    ei_impulse_result_t result = { 0 };
    err = run_classifier(&signal, &result, false);
    free(features);

    if (err != EI_IMPULSE_OK) {
        Serial.printf("ERR: Failed to run classifier (%d)\n", err);
        resultText = "Classifier Error";
        if (imgBuffer) { free(imgBuffer); imgBuffer = nullptr; }
        return;
    }

    float max_fire_score = 0.0;
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value > max_fire_score) {
            max_fire_score = bb.value;
        }
    }
#else
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (String(ei_classifier_inferencing_categories[i]) == "fire" || EI_CLASSIFIER_LABEL_COUNT == 1) {
             max_fire_score = result.classification[i].value;
        }
    }
#endif

    float score = max_fire_score * 100.0;
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

    // Giải phóng imgBuffer ngay sau khi xử lý xong để tiết kiệm RAM
    if (imgBuffer) {
        free(imgBuffer);
        imgBuffer = nullptr;
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
            {
              String htmlStr = String(html);
              int max_dim = EI_CLASSIFIER_INPUT_WIDTH > EI_CLASSIFIER_INPUT_HEIGHT ? EI_CLASSIFIER_INPUT_WIDTH : EI_CLASSIFIER_INPUT_HEIGHT;
              htmlStr.replace("let max_size = 96;", "let max_size = " + String(max_dim) + ";");
              server.send(200, "text/html", htmlStr);
            });

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