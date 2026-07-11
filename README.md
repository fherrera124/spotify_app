# Spotify App (ESP32-S3)

Aplicación embebida que muestra en una pantalla táctil el estado de reproducción de Spotify en tiempo real: portada del álbum, título, artista, barra de progreso y controles (anterior / pausa-play / siguiente).

## Resumen de la aplicación

El firmware se conecta por WiFi, obtiene un access token de Spotify y se suscribe al canal de eventos en tiempo real (`wss://dealer.spotify.com`) para reflejar en pantalla lo que se está reproduciendo en la cuenta del usuario. Al recibir una pista nueva:

- Descarga la portada del álbum (JPEG) desde el CDN de Spotify y la decodifica en memoria.
- Actualiza título y artista(s) en la UI.
- Actualiza la barra de progreso de reproducción, interpolando el avance entre eventos.
- Permite controlar la reproducción (anterior / pausa-reanudar / siguiente) mediante botones táctiles en pantalla, que disparan comandos contra la API de Spotify.

La obtención del access token **no usa el flujo OAuth estándar de Spotify** (no requiere crear una app en el Developer Dashboard de Spotify). En su lugar, aprovecha la integración "Conectar cuenta de Spotify" del perfil de Discord: usando un token de sesión de Discord se pide, vía un endpoint interno de Discord, el access token de Spotify vinculado a esa cuenta. Ver sección de configuración más abajo.

## Stack tecnológico

- **ESP-IDF** v6.0.x
- **LVGL** 9.x como framework de UI, integrado vía `esp_lvgl_port` (maneja el ciclo de refresco, el lock/unlock de la UI y el framebuffer)
- **esp_lcd_axs15231b**: driver del controlador de panel AXS15231B (interfaz QSPI)
- **esp_lcd_touch**: driver de touch capacitivo I2C sobre el mismo panel
- **esp_jpeg**: decodificación de las portadas de álbum (JPEG → RGB565)
- **esp_http_client** / **esp-tls** (mbedTLS): llamadas REST a la API de Spotify y al endpoint de Discord
- **esp_websocket_client**: conexión en tiempo real al "dealer" de Spotify para eventos de reproducción
- **json_parser** / **jsmn**: parseo de las respuestas JSON de la API

## Hardware

- **Chipset**: ESP32-S3 (dual-core Xtensa LX7, WiFi + BLE), 16 MB de flash, 8 MB de PSRAM embebida en modo Octal.
- **Pantalla**: controlador AXS15231B por interfaz QSPI, resolución nativa 320×480, con panel táctil capacitivo por I2C.

## Configuración (menuconfig)

Antes de compilar hay que cargar dos credenciales propias en la configuración del proyecto:

```bash
idf.py menuconfig
```

Ir a **`Spotify Client Configuration`** y completar:

- **Discord Token**: el token de sesión de tu cuenta de Discord (no es un token de bot). Pasos para obtenerlo: [how to get discord token](https://www.reddit.com/r/Discord_selfbots/comments/1hhojww/how_to_get_discord_token/).
- **Spotify UID**: tu ID de usuario de Discord (snowflake numérico de 17-19 dígitos). Se obtiene activando el "Modo de desarrollador" en Discord (Ajustes → Avanzado → Modo desarrollador) y luego haciendo clic derecho sobre tu perfil → "Copiar ID de usuario".

Requisito: tu cuenta de Discord debe tener la cuenta de Spotify vinculada en Ajustes → Conexiones.

Guardá con `S` y salí con `Q`. Estos valores quedan en `sdkconfig` como `CONFIG_DISCORD_TOKEN` y `CONFIG_SPOTIFY_UID`.

## Actualización de certificados TLS

Los certificados de las autoridades intermedias (Discord y los distintos hosts de Spotify) están pineados en `components/spotify_client/certs.pem`. Como estas autoridades intermedias rotan periódicamente, si en algún momento las conexiones HTTPS empiezan a fallar por verificación de certificado, hay que refrescar ese archivo.

Para cada host, extraer el certificado intermedio (el segundo de la cadena que manda el servidor) con:

```bash
echo | openssl s_client -connect discord.com:443 -servername discord.com -showcerts 2>/dev/null \
  | awk 'BEGIN{c=0} /-----BEGIN CERTIFICATE-----/{c++} c==2{print} /-----END CERTIFICATE-----/{if(c==2) exit}'
```

Repetir para cada host usado por la app (API, eventos en tiempo real, login y CDN de portadas de Spotify, y el propio Discord):

```bash
for host in discord.com api.spotify.com dealer.spotify.com accounts.spotify.com i.scdn.co; do
  echo "=== $host ==="
  echo | openssl s_client -connect "$host:443" -servername "$host" -showcerts 2>/dev/null \
    | awk 'BEGIN{c=0} /-----BEGIN CERTIFICATE-----/{c++} c==2{print} /-----END CERTIFICATE-----/{if(c==2) exit}'
done
```

Pegar el bloque `-----BEGIN CERTIFICATE----- ... -----END CERTIFICATE-----` resultante de cada host en `components/spotify_client/certs.pem`, reemplazando el certificado correspondiente a ese host (no hace falta duplicar si dos hosts comparten la misma CA intermedia, como pasa hoy entre `api.spotify.com`, `dealer.spotify.com` y `accounts.spotify.com`).

Para verificar rápidamente el subject/issuer de un certificado ya guardado:

```bash
openssl x509 -in components/spotify_client/certs.pem -noout -subject -issuer
```

(si el archivo tiene varios certificados concatenados, usar el índice correspondiente extrayéndolo primero con `csplit` o revisando de a uno).

Después de actualizar `certs.pem`, recompilar y reflashear:

```bash
idf.py build flash monitor
```
