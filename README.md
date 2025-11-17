# Open Folder Proyect - Portal cautivo con LittleFS (sobre un NODEMCU ESP8266) sin microSD

Este proyecto pone en marcha un portal cautivo abierto en un NodeMCU / ESP8266 usando LittleFS (almacenamiento interno). Permite listar y descargar archivos, restringe la subida a un administrador y ofrece opciones de personalización (encabezado y pie de página).

Características principales
- Access Point abierto (sin contraseña) actuando como portal cautivo (DNS captura todas las peticiones).
- Interfaz web ligera para listar y descargar archivos almacenados en LittleFS.
- Usuarios anónimos: solo pueden navegar y descargar; no pueden subir archivos.
- Administrador (HTTP Basic Auth): puede subir archivos (sin límites), borrar archivos y editar el encabezado y pie de página que se muestran en la portada.
- Validación de extensiones permitidas en servidor.
- Indicadores en la UI: nivel de batería (A0) y espacio en disco (estimado por LittleFS).
- Código pensado para consumo moderado de recursos en ESP8266.

Archivos incluidos
- `open_folder_proyect.ino` — sketch principal (LittleFS).
- `README.md` — este archivo.
- `LICENSE` — licencia MIT (plantilla).

Ajustes recomendados antes de usar
- ADMIN_USER / ADMIN_PASS: cambia las credenciales por defecto en el sketch antes de compilar.
- QUOTA_PER_CLIENT y MAX_UPLOAD_SIZE: ajusta la cuota para usuarios normales y el límite de subida si lo deseas.
- VOLTAGE_DIVIDER_RATIO, BATTERY_MIN_V, BATTERY_MAX_V: ajusta según tu divisor de tensión y la química de tu batería.
- Si usas NTP/RTC, las funciones de fecha podrán usarse en mejoras adicionales.

Wiring básico (si mides batería)
- Si la tensión de la batería > 1.0 V, alimenta un divisor resistivo hacia A0:
  - Vbat -> R1 -> A0 -> R2 -> GND
  - Ajusta `VOLTAGE_DIVIDER_RATIO = (R1+R2)/R2` en el sketch.
- No hay conexión de módulo SD en esta versión (usa LittleFS).

Uso rápido
1. Abre Arduino IDE (o PlatformIO) con soporte ESP8266 instalado.
2. Copia `open_folder_proyect.ino` en un nuevo sketch o en la carpeta del proyecto.
3. Configura las constantes (ADMIN_USER/ADMIN_PASS, SD_CS no aplica, etc.).
4. Selecciona la placa NodeMCU/Generic ESP8266 y sube el sketch.
5. Conéctate al AP `📂Open Folder Proyect` (sin contraseña).
6. Abre un navegador y dirígete a `http://192.168.4.1/` (o cualquier URL — el DNS captura y redirige al portal).
7. Para administración: visita `http://192.168.4.1/admin` y proporciona las credenciales.

Rutas principales
- GET `/` — portal público (lista de archivos, indicadores).
- GET `/download?f=storedName` — descargar archivo.
- POST `/api/delete` — borrar archivo (solo propietario).
- GET `/admin` — panel admin (Basic Auth).
- POST `/admin/upload` — subir archivo (admin).
- POST `/admin/delete` — borrar archivo (admin).
- POST `/admin/save_template` — guardar header/footer (admin).
- GET `/api/list`, `/api/status`, `/api/quota` — endpoints usados por la UI.

Notas sobre LittleFS y duración de la flash
- LittleFS usa la flash interna del ESP8266. Evita escrituras continuas o muy frecuentes para no desgastar la memoria.
- Si necesitas limpiar el sistema de archivos (solo en pruebas), puedes usar `LittleFS.format()` en un sketch de mantenimiento (ten cuidado: borra todo).

Seguridad y privacidad
- El AP es abierto y sin cifrado — cualquiera que se conecte puede ver y descargar archivos públicos.
- Protege el panel admin con una contraseña fuerte. En un repositorio público no incluyas credenciales reales.
- Si necesitas auditoría o restricciones más estrictas, considera migrar a ESP32 o añadir autenticación adicional.

Solución de problemas rápida
- LittleFS.begin() falla: revisa la versión del core ESP8266 o considera formateo (solo para pruebas).
- Subidas fallan: asegúrate de cargar el panel admin en el navegador (Basic Auth) antes de usar la UI de subida o usar el formulario tradicional (sin AJAX).
- Reinicios durante transferencias: probablemente falta de corriente en la fuente USB; usa una fuente más estable.

Licencia
- MIT (archivo LICENSE incluido).
```
