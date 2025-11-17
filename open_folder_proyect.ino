/*
  esp_captive_littlefs.ino
  Portal cautivo open + LittleFS (almacenamiento interno del ESP8266)
  - Usuarios anonimos NO pueden subir archivos
  - Admin (HTTP Basic Auth) puede subir y borrar sin límites
  - Admin puede editar encabezado (header) y pie de página (footer) que se muestran en la página pública
  - Meta en /meta.csv: storedName|displayName|ownerIP|size
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

const char* apSSID = "📂Open Folder Proyect";
const byte DNS_PORT = 53;
const IPAddress apIP(192,168,4,1);

// Admin credentials (CAMBIA ESTO antes de desplegar)
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "change_me";

// Cuota por cliente en bytes (ej. 2 MB) -- sigue aplicando a usuarios normales
const size_t QUOTA_PER_CLIENT = 2 * 1024 * 1024UL;
const size_t MAX_UPLOAD_SIZE = 1024 * 1024UL * 3UL; // límite para usuarios normales (3 MB)

DNSServer dnsServer;
ESP8266WebServer server(80);

struct MetaEntry {
  String storedName;   // sin slash inicial
  String displayName;
  String ownerIP;
  size_t size;
};

#include <vector>
std::vector<MetaEntry> metaEntries;

// extensiones permitidas (minúsculas)
const char* allowedExts[] = { ".txt", ".log", ".csv", ".json", ".html", ".htm", ".css",
                             ".js", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".pdf",
                             ".zip", ".mp3" };
const size_t ALLOWED_EXT_COUNT = sizeof(allowedExts)/sizeof(allowedExts[0]);

File uploadFile;
size_t uploadBytesWritten = 0;
String uploadStoredName;
String uploadOwnerIdentifier; // owner IP or "admin"
bool uploadAborted = false;

/* ---------------- BATERÍA: Ajustes y lectura ---------------- */
const float ADC_REF_VOLTAGE = 1.0f;
const int ADC_MAX_READING = 1023;
const float VOLTAGE_DIVIDER_RATIO = 4.0f; // AJUSTA según tu divisor
const float BATTERY_MIN_V = 3.3f;
const float BATTERY_MAX_V = 4.2f;

/* ---------------- utilities ---------------- */
String htmlHeader(const String& title = "ESP Captive LittleFS") {
  return String("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>")
         + "<title>" + title + "</title>"
         + "<style>body{font-family:Arial,Helvetica,sans-serif;margin:12px;}a{word-break:break-all}table{border-collapse:collapse;width:100%}th,td{padding:8px;border-bottom:1px solid #ddd;text-align:left}button{padding:6px 8px}#statusbar{display:flex;gap:12px;align-items:center;margin-bottom:12px} .bar{background:#eee;border-radius:6px;overflow:hidden;height:14px;width:140px} .bar > i{display:block;height:100%;background:linear-gradient(90deg,#4caf50,#8bc34a);width:0%}</style>"
         + "</head><body>";
}
String htmlFooter() {
  return String("<hr><small>ESP8266 Captive LittleFS</small></body></html>");
}

String contentType(const String& filename) {
  if (filename.endsWith(".htm") || filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css")) return "text/css";
  if (filename.endsWith(".js")) return "application/javascript";
  if (filename.endsWith(".png")) return "image/png";
  if (filename.endsWith(".gif")) return "image/gif";
  if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  if (filename.endsWith(".ico")) return "image/x-icon";
  if (filename.endsWith(".xml")) return "text/xml";
  if (filename.endsWith(".pdf")) return "application/pdf";
  if (filename.endsWith(".zip")) return "application/zip";
  if (filename.endsWith(".mp3")) return "audio/mpeg";
  return "application/octet-stream";
}

String sanitizeName(const String& n) {
  String s = n;
  s.replace("/", "_");
  s.replace("\\", "_");
  s.replace("|", "_");
  return s;
}

String getClientIP() {
  IPAddress rip = server.client().remoteIP();
  return rip.toString();
}

String readFileToString(const char* path) {
  if (!LittleFS.exists(path)) return String("");
  File f = LittleFS.open(path, "r");
  if (!f) return String("");
  String s;
  while (f.available()) s += char(f.read());
  f.close();
  return s;
}

bool writeStringToFile(const char* path, const String& data) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  f.print(data);
  f.close();
  return true;
}

/* ---------------- metadata handling ----------------
   meta file: /meta.csv with lines:
   storedName|displayName|ownerIP|size\n
*/
void loadMeta() {
  metaEntries.clear();
  if (!LittleFS.exists("/meta.csv")) return;
  File f = LittleFS.open("/meta.csv", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1+1);
    int p3 = line.indexOf('|', p2+1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    MetaEntry e;
    e.storedName = line.substring(0, p1);
    e.displayName = line.substring(p1+1, p2);
    e.ownerIP = line.substring(p2+1, p3);
    e.size = (size_t) line.substring(p3+1).toInt();
    metaEntries.push_back(e);
  }
  f.close();
}

void saveMeta() {
  File f = LittleFS.open("/meta.csv", "w");
  if (!f) return;
  for (auto &e : metaEntries) {
    String line = e.storedName + "|" + e.displayName + "|" + e.ownerIP + "|" + String(e.size) + "\n";
    f.print(line);
  }
  f.close();
}

void addMetaEntry(const String& stored, const String& disp, const String& owner, size_t size) {
  MetaEntry e;
  e.storedName = stored;
  e.displayName = disp;
  e.ownerIP = owner;
  e.size = size;
  metaEntries.push_back(e);
  // append to file for menor coste
  File f = LittleFS.open("/meta.csv", "a");
  if (f) {
    String line = e.storedName + "|" + e.displayName + "|" + e.ownerIP + "|" + String(e.size) + "\n";
    f.print(line);
    f.close();
  } else {
    saveMeta();
  }
}

bool removeMetaEntryByStored(const String& stored) {
  bool found = false;
  for (size_t i=0; i<metaEntries.size(); ++i) {
    if (metaEntries[i].storedName == stored) {
      metaEntries.erase(metaEntries.begin() + i);
      found = true;
      break;
    }
  }
  if (found) saveMeta();
  return found;
}

MetaEntry* findMetaByStored(const String& stored) {
  for (size_t i=0; i<metaEntries.size(); ++i) {
    if (metaEntries[i].storedName == stored) {
      return &metaEntries[i];
    }
  }
  return nullptr;
}

size_t getUsageForIP(const String& ip) {
  size_t total = 0;
  for (auto &e : metaEntries) {
    if (e.ownerIP == ip) total += e.size;
  }
  return total;
}

bool isExtAllowed(const String& filename) {
  String lower = filename;
  lower.toLowerCase();
  for (size_t i=0;i<ALLOWED_EXT_COUNT;i++) {
    if (lower.endsWith(allowedExts[i])) return true;
  }
  return false;
}

/* ---------------- DISCO: uso y capacidad usando LittleFS ---------------- */
uint64_t getUsedSpace() {
  uint64_t used = 0;
  // try LittleFS.usedBytes() if available
  #ifdef LittleFS
  // Many ESP8266 core versions expose usedBytes()
  used = LittleFS.usedBytes();
  #endif
  if (used > 0) return used;
  for (auto &e : metaEntries) used += e.size;
  return used;
}

uint64_t getTotalSpace() {
  uint64_t total = 0;
  #ifdef LittleFS
  total = LittleFS.totalBytes();
  #endif
  if (total == 0) total = 1024UL * 1024UL * 4UL; // default 4MB fallback
  return total;
}

/* ---------------- BATERÍA: lectura y conversión ---------------- */
float readBatteryVoltage() {
  int raw = analogRead(A0);
  float vadc = ( (float)raw / (float)ADC_MAX_READING ) * ADC_REF_VOLTAGE;
  float vbat = vadc * VOLTAGE_DIVIDER_RATIO;
  return vbat;
}

int voltageToPercent(float v) {
  if (v <= BATTERY_MIN_V) return 0;
  if (v >= BATTERY_MAX_V) return 100;
  float p = (v - BATTERY_MIN_V) / (BATTERY_MAX_V - BATTERY_MIN_V) * 100.0f;
  int pi = (int) p;
  if (pi < 0) pi = 0;
  if (pi > 100) pi = 100;
  return pi;
}

/* ---------------- handlers (web) ---------------- */

// Public root: NO permite subida por usuarios anonimos. Muestra header/footer guardados.
void handleRoot() {
  String header = readFileToString("/header.html");
  String footer = readFileToString("/footer.html");

  String html = htmlHeader("Portal cautivo - archivos en LittleFS");
  if (header.length() > 0) {
    html += header;
  } else {
    html += "<h2>ESP Captive LittleFS</h2>";
  }

  html += "<div id='statusbar'><div id='bat'>Batería: ...</div><div id='disk'>Disco: ...</div><div id='quota'>Cuota: ...</div></div>";
  html += "<div id='app'><p>Listando archivos...</p></div>";
  html += "<hr>";
  html += "<p>La subida de archivos está deshabilitada para usuarios anónimos.</p>";
  html += "<p><a href='/admin'>Panel admin</a> (requiere usuario/clave)</p>";
  if (footer.length() > 0) html += footer;
  html += "<script>\n"
          "const apiList = '/api/list';\n"
          "const apiQuota = '/api/quota';\n"
          "const apiStatus = '/api/status';\n"
          "async function refresh(){\n"
          "  try{\n"
          "    const [r1,r2,r3] = await Promise.all([fetch(apiList), fetch(apiQuota), fetch(apiStatus)]);\n"
          "    const list = await r1.json();\n"
          "    const quota = await r2.json();\n"
          "    const status = await r3.json();\n"
          "    document.getElementById('bat').innerHTML = 'Batería: ' + status.battery.voltage.toFixed(2) + ' V <div class=\"bar\"><i style=\"width:'+status.battery.percent+'%\"></i></div> ' + status.battery.percent + '%';\n"
          "    const kb = 1024;\n"
          "    document.getElementById('disk').innerHTML = 'Disco: ' + (status.disk.used/kb).toFixed(1) + ' KB / ' + (status.disk.capacity/kb).toFixed(1) + ' KB (' + Math.round((status.disk.used/status.disk.capacity)*100) + '%)';\n"
          "    document.getElementById('quota').innerHTML = 'Tu IP: '+quota.ip+' | Cuota: '+(quota.quota/1024).toFixed(1)+' KB | Usado: '+(quota.used/1024).toFixed(1)+' KB';\n"
          "    let out = '<table><tr><th>Archivo</th><th>Tamaño</th><th>Owner</th><th>Acciones</th></tr>';\n"
          "    for(const f of list){\n"
          "      out += '<tr>';\n"
          "      out += '<td>'+encodeHTML(f.displayName)+'</td>';\n"
          "      out += '<td>'+f.size+'</td>';\n"
          "      out += '<td>'+f.ownerIP+'</td>';\n"
          "      out += '<td><a href=\"/download?f='+encodeURIComponent(f.storedName)+'\">Descargar</a>';\n"
          "      if(f.ownerIP === quota.ip) out += ' | <button onclick=\"del(\\''+f.storedName+'\\')\">Borrar</button>';\n"
          "      out += '</td>';\n"
          "      out += '</tr>';\n"
          "    }\n"
          "    out += '</table>';\n"
          "    document.getElementById('app').innerHTML = out;\n"
          "  }catch(e){ document.getElementById('app').innerText = 'Error cargando listados'; }\n"
          "}\n"
          "function encodeHTML(s){ return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }\n"
          "async function del(stored){ if(!confirm('Borrar?')) return; let fd = new URLSearchParams(); fd.append('stored', stored); const r = await fetch('/api/delete',{method:'POST',body:fd}); if(r.ok) refresh(); else alert('Error'); }\n"
          "refresh();\n"
          "</script>\n";
  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleApiList() {
  String json = "[";
  bool first = true;
  for (auto &e : metaEntries) {
    if (!first) json += ",";
    json += "{\"storedName\":\"" + e.storedName + "\",\"displayName\":\"" + e.displayName + "\",\"ownerIP\":\"" + e.ownerIP + "\",\"size\":" + String(e.size) + "}";
    first = false;
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleApiQuota() {
  String ip = getClientIP();
  size_t used = getUsageForIP(ip);
  String json = "{\"ip\":\"" + ip + "\",\"quota\":" + String(QUOTA_PER_CLIENT) + ",\"used\":" + String(used) + "}";
  server.send(200, "application/json", json);
}

void handleApiStatus() {
  float v = readBatteryVoltage();
  int pct = voltageToPercent(v);
  uint64_t used = getUsedSpace();
  uint64_t capacity = getTotalSpace();
  uint64_t free = (capacity > used) ? (capacity - used) : 0;
  String json = "{";
  json += "\"battery\":{";
  json += "\"voltage\":" + String(v, 2) + ",";
  json += "\"percent\":" + String(pct);
  json += "},";
  json += "\"disk\":{";
  json += "\"capacity\":" + String((uint32_t)capacity) + ",";
  json += "\"used\":" + String((uint32_t)used) + ",";
  json += "\"free\":" + String((uint32_t)free);
  json += "}";
  json += "}";
  server.send(200, "application/json", json);
}

void handleDownload() {
  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Parametro missing");
    return;
  }
  String stored = server.arg("f");
  if (!stored.startsWith("/")) stored = "/" + stored;
  if (!LittleFS.exists(stored)) {
    server.send(404, "text/plain", "No encontrado");
    return;
  }
  File file = LittleFS.open(stored, "r");
  if (!file) {
    server.send(500, "text/plain", "Error abriendo archivo");
    return;
  }
  server.streamFile(file, contentType(stored));
  file.close();
}

void handleApiDelete() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Not allowed"); return; }
  String stored;
  if (server.hasArg("stored")) stored = server.arg("stored");
  else stored = server.arg(0);
  if (stored.length() == 0) { server.send(400, "text/plain", "stored required"); return; }
  if (!stored.startsWith("/")) stored = "/" + stored;
  MetaEntry* e = findMetaByStored(stored.substring(1));
  if (!e) { server.send(404, "text/plain", "Not found"); return; }
  String ip = getClientIP();
  if (e->ownerIP != ip) { server.send(403, "text/plain", "Only owner can delete"); return; }
  bool ok = LittleFS.remove(stored);
  if (ok) {
    removeMetaEntryByStored(stored.substring(1));
    server.send(200, "text/plain", "Deleted");
  } else {
    server.send(500, "text/plain", "Error deleting");
  }
}

/* upload handling for admin only:
   - Admin authenticated via Basic Auth can upload without quota/size checks
   - Ordinary users cannot upload (no public upload route)
*/
void handleAdminUpload() {
  // final responder after upload
  // if aborted, cleanup done in handler
  if (uploadAborted) {
    uploadAborted = false;
    if (uploadFile) { uploadFile.close(); LittleFS.remove("/" + uploadStoredName); }
    server.send(413, "text/plain", "Upload aborted (type/size/quota)");
    return;
  }
  if (uploadStoredName.length() > 0) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "No file");
  }
  uploadStoredName = "";
  uploadBytesWritten = 0;
  uploadOwnerIdentifier = "";
}

// upload handler (called during multipart streaming)
void handleAdminFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    // check admin credentials
    bool isAdmin = server.authenticate(ADMIN_USER, ADMIN_PASS);
    if (!isAdmin) {
      // deny anonymous/normal users
      Serial.println("Upload denied: not admin");
      uploadAborted = true;
      return;
    }
    String filename = upload.filename;
    if (!isExtAllowed(filename)) {
      Serial.println("Denied: extension not allowed");
      uploadAborted = true;
      return;
    }
    // admin is allowed: create stored name
    uploadOwnerIdentifier = "admin";
    String stored = uploadOwnerIdentifier + "_" + String(millis()) + "_" + sanitizeName(filename);
    uploadStoredName = stored;
    // admin bypasses quota/size limits (no checks)
    uploadFile = LittleFS.open("/" + stored, "w");
    uploadBytesWritten = 0;
    if (!uploadFile) {
      Serial.println("ERROR: cannot open file for write");
      uploadAborted = true;
      uploadStoredName = "";
    } else {
      Serial.printf("Admin upload start: %s\n", stored.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadFile) return;
    uploadFile.write(upload.buf, upload.currentSize);
    uploadBytesWritten += upload.currentSize;
    // admin: no quota enforcement
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!uploadAborted && uploadFile) {
      uploadFile.close();
      addMetaEntry(uploadStoredName, upload.filename, uploadOwnerIdentifier, uploadBytesWritten);
      Serial.printf("Admin upload complete: %s (%u bytes)\n", uploadStoredName.c_str(), uploadBytesWritten);
      uploadBytesWritten = 0;
      uploadStoredName = "";
      uploadOwnerIdentifier = "";
    } else {
      uploadAborted = false;
    }
  }
}

/* ---------------- Admin panel (protected) ---------------- */
// Basic Auth protection for admin routes
bool checkAdminAuth() {
  if (!server.authenticate(ADMIN_USER, ADMIN_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleAdmin() {
  if (!checkAdminAuth()) return;
  String header = readFileToString("/header.html");
  String footer = readFileToString("/footer.html");

  String html = htmlHeader("Admin - Gestión de archivos");
  html += "<p>Panel admin: ver, subir y borrar archivos; editar encabezado y pie de página.</p>";

  // form para header/footer
  html += "<h3>Editar encabezado y pie de página (HTML permitido)</h3>";
  html += "<form id='tplForm' method='POST' action='/admin/save_template'>";
  html += "<label>Encabezado (html):</label><br><textarea name='header' id='header' rows='6' style='width:100%'>" + header + "</textarea><br>";
  html += "<label>Pie de página (html):</label><br><textarea name='footer' id='footer' rows='6' style='width:100%'>" + footer + "</textarea><br>";
  html += "<input type='submit' value='Guardar encabezado/pie'></form><hr>";

  // upload form (admin-only)
  html += "<h3>Subir archivo (admin)</h3>";
  html += "<form id='uploadForm' method='POST' action='/admin/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='file' required> <input type='submit' value='Subir'></form>";
  html += "<div id='msg'></div><hr>";

  // list (admin) and JS
  html += "<div id='app'><p>Cargando...</p></div>";
  html += "<script>\n"
          "async function refresh(){\n"
          "  try{ const r = await fetch('/admin/list'); if(!r.ok){ document.getElementById('app').innerText='Error'; return; } const list = await r.json();\n"
          "    let out = '<table><tr><th>Archivo</th><th>Tamaño</th><th>Owner</th><th>Acciones</th></tr>';\n"
          "    for(const f of list){ out += '<tr>'; out += '<td>'+encodeHTML(f.displayName)+'</td>'; out += '<td>'+f.size+'</td>'; out += '<td>'+f.ownerIP+'</td>'; out += '<td><a href=\"/download?f='+encodeURIComponent(f.storedName)+'\">Descargar</a> | <button onclick=\"del(\\''+f.storedName+'\\')\">Borrar</button></td>'; out += '</tr>'; }\n"
          "    out += '</table>'; document.getElementById('app').innerHTML = out;\n"
          "  }catch(e){ document.getElementById('app').innerText='Error'; }\n"
          "}\n"
          "function encodeHTML(s){ return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }\n"
          "async function del(stored){ if(!confirm('Borrar este archivo (admin)?')) return; let fd = new URLSearchParams(); fd.append('stored', stored); const r = await fetch('/admin/delete',{method:'POST',body:fd}); if(r.ok) refresh(); else alert('Error ' + r.status); }\n"
          "document.getElementById('uploadForm').addEventListener('submit', function(e){\n"
          "  e.preventDefault(); const f = this.querySelector('input[type=file]').files[0]; if(!f){ alert('Selecciona archivo'); return; }\n"
          "  const fd = new FormData(); fd.append('file', f);\n"
          "  fetch('/admin/upload',{method:'POST',body:fd}).then(resp=>{ if(resp.ok){ document.getElementById('msg').innerText='Subida OK'; refresh(); } else { resp.text().then(t=>document.getElementById('msg').innerText='Error: '+t); }}).catch(()=>document.getElementById('msg').innerText='Error en upload');\n"
          "});\n"
          "refresh();\n"
          "</script>\n";

  html += htmlFooter();
  server.send(200, "text/html", html);
}

void handleAdminList() {
  if (!checkAdminAuth()) return;
  String json = "[";
  bool first = true;
  for (auto &e : metaEntries) {
    if (!first) json += ",";
    json += "{\"storedName\":\"" + e.storedName + "\",\"displayName\":\"" + e.displayName + "\",\"ownerIP\":\"" + e.ownerIP + "\",\"size\":" + String(e.size) + "}";
    first = false;
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleAdminDelete() {
  if (!checkAdminAuth()) return;
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Not allowed"); return; }
  String stored;
  if (server.hasArg("stored")) stored = server.arg("stored");
  else stored = server.arg(0);
  if (stored.length() == 0) { server.send(400, "text/plain", "stored required"); return; }
  if (!stored.startsWith("/")) stored = "/" + stored;
  bool ok = LittleFS.remove(stored);
  if (ok) {
    removeMetaEntryByStored(stored.substring(1));
    server.send(200, "text/plain", "Deleted");
  } else {
    server.send(500, "text/plain", "Error deleting");
  }
}

// Admin template endpoints: save and view
void handleAdminTemplateSave() {
  if (!checkAdminAuth()) return;
  // data may arrive as application/x-www-form-urlencoded
  String header = server.arg("header");
  String footer = server.arg("footer");
  bool okH = writeStringToFile("/header.html", header);
  bool okF = writeStringToFile("/footer.html", footer);
  if (okH && okF) {
    server.send(200, "text/plain", "Template saved");
  } else {
    server.send(500, "text/plain", "Error saving template");
  }
}

void handleAdminTemplateGet() {
  if (!checkAdminAuth()) return;
  String header = readFileToString("/header.html");
  String footer = readFileToString("/footer.html");
  String json = "{\"header\":\"" + header + "\",\"footer\":\"" + footer + "\"}";
  server.send(200, "application/json", json);
}

/* catch-all for captive behaviour */
void handleNotFound() {
  server.sendHeader("Location", String("http://") + apIP.toString() + "/");
  server.send(302, "text/html", "");
}

/* ---------------- setup & loop ---------------- */
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("\nESP Captive LittleFS - arrancando...");

  if (!LittleFS.begin()) {
    Serial.println("No se pudo montar LittleFS. Prueba a formatear o revisar memoria.");
  } else {
    Serial.println("LittleFS montado.");
  }

  loadMeta();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(apSSID);
  dnsServer.start(DNS_PORT, "*", apIP);
  Serial.printf("AP '%s' iniciado. IP: %s\n", apSSID, apIP.toString().c_str());

  // rutas públicas
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/list", HTTP_GET, handleApiList);
  server.on("/api/quota", HTTP_GET, handleApiQuota);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/api/delete", HTTP_POST, handleApiDelete); // delete by owner only; admin has /admin/delete

  // upload for admin only (multipart)
  server.on("/admin/upload", HTTP_POST, handleAdminUpload, handleAdminFileUpload);

  // admin routes
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/list", HTTP_GET, handleAdminList);
  server.on("/admin/delete", HTTP_POST, handleAdminDelete);
  server.on("/admin/save_template", HTTP_POST, handleAdminTemplateSave);
  server.on("/admin/template", HTTP_GET, handleAdminTemplateGet);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor web iniciado.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
}
