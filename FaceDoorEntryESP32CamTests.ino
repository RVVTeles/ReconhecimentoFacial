#include <ArduinoWebsockets.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "Arduino.h"
#include "fd_forward.h"
#include "fr_forward.h"
#include "fr_flash.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
const String firebaseHost = "https://reconhecimentofacial-ecf9d-default-rtdb.firebaseio.com/";

const char* ssid = "brisa-2832722_EXT";
const char* password = "j4edjnii";

String nome_temp = "";
String cpf_temp = "";
String telefone_temp = "";

#define ENROLL_CONFIRM_TIMES 5
#define FACE_ID_SAVE_NUMBER 7

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

using namespace websockets;
WebsocketsServer socket_server;

camera_fb_t * fb = NULL;

long current_millis;
long last_detected_millis = 0;

#define relay_pin 2
unsigned long door_opened_millis = 0;
long interval = 5000;
bool face_recognised = false;

void app_facenet_main();
void app_httpserver_init();

typedef struct {
  uint8_t *image;
  box_array_t *net_boxes;
  dl_matrix3d_t *face_id;
} http_img_process_result;

static inline mtmn_config_t app_mtmn_config() {
  mtmn_config_t mtmn_config = {0};
  mtmn_config.type = FAST;
  mtmn_config.min_face = 80;
  mtmn_config.pyramid = 0.707;
  mtmn_config.pyramid_times = 4;
  mtmn_config.p_threshold.score = 0.6;
  mtmn_config.p_threshold.nms = 0.7;
  mtmn_config.p_threshold.candidate_number = 20;
  mtmn_config.r_threshold.score = 0.7;
  mtmn_config.r_threshold.nms = 0.7;
  mtmn_config.r_threshold.candidate_number = 10;
  mtmn_config.o_threshold.score = 0.7;
  mtmn_config.o_threshold.nms = 0.7;
  mtmn_config.o_threshold.candidate_number = 1;
  return mtmn_config;
}
mtmn_config_t mtmn_config = app_mtmn_config();

face_id_name_list st_face_list;
static dl_matrix3du_t *aligned_face = NULL;

httpd_handle_t camera_httpd = NULL;

typedef enum {
  START_STREAM,
  START_DETECT,
  SHOW_FACES,
  START_RECOGNITION,
  START_ENROLL,
  ENROLL_COMPLETE,
  DELETE_ALL,
} en_fsm_state;
en_fsm_state g_state;

typedef struct {
  char enroll_name[ENROLL_NAME_LEN];
} httpd_resp_value;

httpd_resp_value st_name;


String html_content =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Face Recognition Access Control</title>"
"<style>"
"@media only screen and (min-width: 850px) { body { display: flex; } #content-right { margin-left: 10px; } }"
"body { font-family: Arial, Helvetica, sans-serif; background: #181818; color: #EFEFEF; font-size: 16px; margin: 0; padding: 10px; }"
"#content-left, #content-right { max-width: 400px; flex: 1; }"
"#stream { width: 100%; max-width: 100%; }"
"#status-display { height: 25px; border: none; padding: 10px; font: 18px/22px sans-serif; margin-bottom: 10px; border-radius: 5px; background: green; text-align: center; }"
"#person, #cpf, #telefone { width: 100%; height: 25px; border: none; padding: 20px 10px; font: 18px/22px sans-serif; margin-bottom: 10px; border-radius: 5px; resize: none; box-sizing: border-box; }"
"button { display: block; margin: 5px 0; padding: 0 12px; border: 0; width: 48%; line-height: 28px; cursor: pointer; color: #fff; background: #ff3034; border-radius: 5px; font-size: 16px; outline: 0; }"
".buttons { height: 40px; }"
"button:hover { background: #ff494d; }"
"button:active { background: #f21c21; }"
"button:disabled { cursor: default; background: #a0a0a0; }"
".left { float: left; }"
".right { float: right; }"
".image-container { position: relative; }"
".stream { max-width: 400px; }"
"ul { list-style: none; padding: 5px; margin: 0; }"
"li { padding: 5px 0; }"
".delete { background: #ff3034; border-radius: 100px; color: #fff; text-align: center; line-height: 18px; cursor: pointer; padding: 0 5px; margin-left: 10px; }"
"h3 { margin-bottom: 3px; }"
"</style></head><body>"
"<div id='content-left'>"
"<div id='stream-container' class='image-container'><img id='stream' src=''></div>"
"</div>"
"<div id='content-right'>"
"<div id='status-display'><span id='current-status'></span></div>"
"<input id='person' type='text' placeholder='Nome'>"
"<input id='cpf' type='text' placeholder='CPF'>"
"<input id='telefone' type='text' placeholder='Telefone'>"
"<div class='buttons'>"
"<button id='button-stream' class='left'>STREAM CAMERA</button>"
"<button id='button-detect' class='right'>DETECT FACES</button>"
"</div>"
"<div class='buttons'>"
"<button id='button-capture' class='left'>ADD USER</button>"
"<button id='button-recognise' class='right'>ACCESS CONTROL</button>"
"</div>"
"<div class='buttons'>"
"<button id='delete_all'>DELETE ALL</button>"
"</div>"
"<ul id='face-list'></ul>"
"</div>"
"<script>"
"function main() {"
"var ws = new WebSocket('ws://' + location.hostname + ':82');"
"var nome = document.getElementById('person');"
"var cpf = document.getElementById('cpf');"
"var tel = document.getElementById('telefone');"
"var status = document.getElementById('current-status');"
"var stream = document.getElementById('stream');"
"ws.onmessage = function(evt) {"
"  if (typeof evt.data === 'string') {"
"    if (evt.data.startsWith('listface:')) addFaceToScreen(evt.data.substring(9));"
"    else if (evt.data === 'delete_faces') clearFaces();"
"    else if (evt.data === 'door_open') status.innerText = 'Acesso liberado';"
"    else status.innerText = evt.data;"
"  } else if (evt.data instanceof Blob) {"
"    stream.src = URL.createObjectURL(evt.data);"
"  }"
"};"
"document.getElementById('button-stream').onclick = function() { ws.send('stream'); };"
"document.getElementById('button-detect').onclick = function() { ws.send('detect'); };"
"document.getElementById('button-capture').onclick = function() {"
"  if (nome.value === '' || cpf.value === '' || tel.value === '') { alert('Preencha todos os campos'); return; }"
"  ws.send('capture:' + nome.value + '|' + cpf.value + '|' + tel.value);"
"};"
"document.getElementById('button-recognise').onclick = function() { ws.send('recognise'); };"
"document.getElementById('delete_all').onclick = function() { ws.send('delete_all'); };"
"function addFaceToScreen(name) {"
"  var li = document.createElement('li');"
"  li.innerHTML = '<strong>' + name + '</strong><span class=\"delete\" onclick=\"removeFace(\\'' + name + '\\')\">X</span>';"
"  document.getElementById('face-list').appendChild(li);"
"}"
"function removeFace(name) { ws.send('remove:' + name); }"
"function clearFaces() { document.getElementById('face-list').innerHTML = ''; }"
"}"
"main();"
"</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html_content.c_str(), html_content.length());
}

httpd_uri_t index_uri = {
  .uri       = "/",
  .method    = HTTP_GET,
  .handler   = index_handler,
  .user_ctx  = NULL
};

void app_httpserver_init() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  if (httpd_start(&camera_httpd, &config) == ESP_OK)
    Serial.println("httpd_start");
  {
    httpd_register_uri_handler(camera_httpd, &index_uri);
  }
}

void app_facenet_main() {
  face_id_name_init(&st_face_list, FACE_ID_SAVE_NUMBER, ENROLL_CONFIRM_TIMES);
  aligned_face = dl_matrix3du_alloc(1, FACE_WIDTH, FACE_HEIGHT, 3);
  read_face_id_from_flash_with_name(&st_face_list);
}

static inline int do_enrollment(face_id_name_list *face_list, dl_matrix3d_t *new_id) {
  ESP_LOGD(TAG, "START ENROLLING");
  int left_sample_face = enroll_face_id_to_flash_with_name(face_list, new_id, st_name.enroll_name);
  ESP_LOGD(TAG, "Face ID %s Enrollment: Sample %d",
           st_name.enroll_name,
           ENROLL_CONFIRM_TIMES - left_sample_face);
  return left_sample_face;
}

static esp_err_t send_face_list(WebsocketsClient &client) {
  client.send("delete_faces");
  face_id_node *head = st_face_list.head;
  char add_face[64];
  for (int i = 0; i < st_face_list.count; i++) {
    sprintf(add_face, "listface:%s", head->id_name);
    client.send(add_face);
    head = head->next;
  }
}

static esp_err_t delete_all_faces(WebsocketsClient &client) {
  delete_face_all_in_flash_with_name(&st_face_list);
  client.send("delete_faces");
}

void enviarNomeFirebase(String nome, String cpf, String telefone) {
  static int userCount = 1;

  String usuarioPath = "usuario" + String(userCount < 10 ? "0" : "") + String(userCount);
  String fullUrl = firebaseHost + "usuario/" + usuarioPath + ".json";

  DynamicJsonDocument doc(256);
  doc["nome"] = nome;
  doc["cpf"] = cpf;
  doc["telefone"] = telefone;

  String jsonData;
  serializeJson(doc, jsonData);

  HTTPClient http;
  http.begin(fullUrl);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.PUT(jsonData);
  String response = http.getString();
  http.end();

  Serial.printf("Enviado ao Firebase (%s): %s\n", usuarioPath.c_str(), response.c_str());
  userCount++;
}

void deletarUsuarioFirebase(String nome) {
  HTTPClient http;
  
  String searchUrl = firebaseHost + "usuario.json?orderBy=\"nome\"&equalTo=\"" + nome + "\"";
  http.begin(searchUrl);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, response);
    
    for (JsonPair kv : doc.as<JsonObject>()) {
      String key = kv.key().c_str();
      String deleteUrl = firebaseHost + "usuario/" + key + ".json";
      
      HTTPClient httpDelete;
      httpDelete.begin(deleteUrl);
      int deleteCode = httpDelete.sendRequest("DELETE");
      httpDelete.end();
      
      Serial.printf("Usuário %s deletado do Firebase (chave: %s)\n", nome.c_str(), key.c_str());
    }
  } else {
    Serial.printf("Falha ao buscar usuário no Firebase: %d\n", httpCode);
  }
  
  http.end();
}


void handle_message(WebsocketsClient &client, WebsocketsMessage msg) {
  if (msg.data() == "stream") {
    g_state = START_STREAM;
    client.send("STREAMING");
  }
  if (msg.data() == "detect") {
    g_state = START_DETECT;
    client.send("DETECTING");
  }
  if (msg.data().substring(0, 8) == "capture:") {
    String payload = msg.data().substring(8); 
    int sep1 = payload.indexOf("|");
    int sep2 = payload.indexOf("|", sep1 + 1);

    String nome = payload.substring(0, sep1);
    String cpf = payload.substring(sep1 + 1, sep2);
    String telefone = payload.substring(sep2 + 1);

    nome.toCharArray(st_name.enroll_name, sizeof(st_name.enroll_name));

    cpf_temp = cpf;
    telefone_temp = telefone;
    nome_temp = nome;

    g_state = START_ENROLL;
    client.send("CAPTURING");
  }
  if (msg.data() == "recognise") {
    g_state = START_RECOGNITION;
    client.send("RECOGNISING");
  }
  if (msg.data().substring(0, 7) == "remove:") {
    char person[ENROLL_NAME_LEN * FACE_ID_SAVE_NUMBER];
    String nome = msg.data().substring(7);
    nome.toCharArray(person, sizeof(person));
    
    delete_face_id_in_flash_with_name(&st_face_list, person);
    
    deletarUsuarioFirebase(nome);
    
    send_face_list(client);
  }
  if (msg.data() == "delete_all") {
    delete_all_faces(client);
  }
}

void open_door(WebsocketsClient &client) {
  if (digitalRead(relay_pin) == LOW) {
    digitalWrite(relay_pin, HIGH);
    Serial.println("Door Unlocked");
    client.send("door_open");
    door_opened_millis = millis();
  }
}

void setup() {  
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  digitalWrite(relay_pin, LOW);
  pinMode(relay_pin, OUTPUT);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  
  config.frame_size = FRAMESIZE_QVGA;  
  config.jpeg_quality = 12;            
  config.fb_count = 1;                 

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);

  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); 
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  app_httpserver_init();
  app_facenet_main();
  socket_server.listen(82);

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop() {
  auto client = socket_server.accept();
  client.onMessage(handle_message);
  dl_matrix3du_t *image_matrix = dl_matrix3du_alloc(1, 320, 240, 3);
  http_img_process_result out_res = {0};
  out_res.image = image_matrix->item;

  send_face_list(client);
  client.send("STREAMING");

  while (client.available()) {
    client.poll();

    if (millis() - interval > door_opened_millis) {
      digitalWrite(relay_pin, LOW);
    }

    fb = esp_camera_fb_get();

    if (g_state == START_DETECT || g_state == START_ENROLL || g_state == START_RECOGNITION) {
      out_res.net_boxes = NULL;
      out_res.face_id = NULL;

      fmt2rgb888(fb->buf, fb->len, fb->format, out_res.image);
      out_res.net_boxes = face_detect(image_matrix, &mtmn_config);

      if (out_res.net_boxes) {
        if (align_face(out_res.net_boxes, image_matrix, aligned_face) == ESP_OK) {
          out_res.face_id = get_face_id(aligned_face);
          last_detected_millis = millis();
          
          if (g_state == START_DETECT) {
            client.send("FACE DETECTED");
          }
          else if (g_state == START_ENROLL) {
            int left_sample_face = do_enrollment(&st_face_list, out_res.face_id);
            char enrolling_message[64];
            sprintf(enrolling_message, "SAMPLE NUMBER %d FOR %s", ENROLL_CONFIRM_TIMES - left_sample_face, st_name.enroll_name);
            client.send(enrolling_message);
            if (left_sample_face == 0) {
              g_state = START_STREAM;
              char captured_message[64];
              sprintf(captured_message, "FACE CAPTURED FOR %s", st_face_list.tail->id_name);
              client.send(captured_message);
              send_face_list(client);
              enviarNomeFirebase(nome_temp, cpf_temp, telefone_temp);
            }
          }
          else if (g_state == START_RECOGNITION && (st_face_list.count > 0)) {
            face_id_node *f = recognize_face_with_name(&st_face_list, out_res.face_id);
            if (f) {
              char recognised_message[64];
              sprintf(recognised_message, "DOOR OPEN FOR %s", f->id_name);
              open_door(client);
              client.send(recognised_message);
            } else {
              client.send("FACE NOT RECOGNISED");
            }
          }
          dl_matrix3d_free(out_res.face_id);
        }
      } else {
        if (g_state != START_DETECT) {
          client.send("NO FACE DETECTED");
        }
      }

      if (g_state == START_DETECT && millis() - last_detected_millis > 500) {
        client.send("DETECTING");
      }
    }

    client.sendBinary((const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    fb = NULL;
  }
}