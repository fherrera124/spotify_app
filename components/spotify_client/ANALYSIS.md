# Análisis de `spotify_client` (2026-07-06)

Documento de trabajo con hallazgos del componente `spotify_client`. Se usa como
checklist para ir abordando los puntos en sesiones sucesivas. Marcar `[x]`
y agregar una línea `Resuelto: <commit/fecha/nota>` a medida que se resuelvan.

Referencias de línea válidas a fecha de este análisis; pueden desactualizarse.

## 1. Hallazgos críticos

- [ ] **1.15 (SIN CONFIRMAR) Después de un rato, el player deja de recibir eventos**
  Reportado por el usuario en hardware real (2026-07-07/08), después de la
  sesión que agregó control de volumen (1.14): el volumen funciona, pero en
  algún momento posterior la pantalla deja de reflejar cambios de track —
  "pareciera que deja de escuchar los eventos". No se pudo reproducir en el
  momento para sacar el log serie, así que **no está confirmada la causa
  ni si es una regresión de esta sesión o algo preexistente que recién se
  hizo visible**. El usuario sospecha de la conexión WebSocket.
  Hipótesis revisadas y descartadas (por ahora) leyendo el código:
  - `parse_device_volume()` (nuevo en 1.14, `parse_objects.c`): repasé el
    código fuente de `json_parser` (`json_obj_get_object`/`leave_object`/
    `json_obj_get_int`) — en caso de que falte la clave `"device"` no toca
    `jctx->cur` (falla silenciosa, sin efecto); y aunque `json_obj_leave_object`
    tiene un bug preexistente de la librería (deja `cur` a mitad de camino
    si el segundo chequeo de `parent` falla), `jctx` es una variable local
    de cada llamada a `parse_track()`, así que no puede filtrarse a la
    llamada siguiente. No debería explicar un cuelgue sostenido, pero queda
    anotado por si la causa real es más sutil de lo que parece en el código.
  - Bits de EventGroup nuevos (`DO_VOLUME_UP`/`DO_VOLUME_DOWN`,
    `1<<12`/`1<<13`): no colisionan con ningún bit existente ni con el rango
    reservado por FreeRTOS para event groups de 32 bits.
  - `player_cmd` con `VOLUME_UP`/`VOLUME_DOWN`: todas las ramas del `switch`
    llegan al `RELEASE_LOCK` final (no hay ningún `return` temprano dentro
    del case nuevo que se salte el unlock).
  **Nota 2026-07-08**: el mecanismo de `VOLUME_UP`/`VOLUME_DOWN` vía
  `player_task`/EventGroup descrito arriba **ya no existe** — se rediseñó
  el control de volumen (ver 1.14 actualizado) a una llamada síncrona
  directa (`spotify_set_volume()`) que no pasa por `player_task` en
  absoluto. Si 1.15 termina confirmándose relacionado con esos bits, el
  problema debería haber desaparecido solo; si sigue ocurriendo, esas dos
  hipótesis quedan definitivamente descartadas como causa.
  **Próximo paso**: reproducir con `idf.py monitor` corriendo y pegar el log
  de los segundos previos/durante el cuelgue — sin eso, seguir "adivinando"
  por lectura de código no es productivo. Candidatos a mirar con el log en
  mano: si el player realmente se cuelga en un `xQueueSend(..., portMAX_DELAY)`
  de `player_task` esperando que `player_screen_start()` vacíe
  `event_queue` (tamaño 1) — en ese caso el síntoma sería exactamente "deja
  de escuchar todo", no solo cambios de track —, o si el WebSocket se
  desconecta/reconecta de forma que `WS_DISCONNECT_EVENT`/`ENABLE_PLAYER`
  no vuelven a dispararse como se espera.

- [x] **1.1 Loop infinito en `player_cmd` con cuentas no-Premium**
  `spotify_client.c:887-902`. Si `PAUSE_UNPAUSE` recibe `403 Forbidden` en
  ambos endpoints (play/pause), el código alterna la URL con `goto retry`
  **sin límite de iteraciones** (a diferencia del path de error de conexión,
  acotado por `RETRIES_ERR_CONN`). Una cuenta Spotify Free (sin permiso de
  controlar reproducción vía API) devuelve 403 en ambos endpoints → loop
  infinito que cuelga `player_task` para siempre reteniendo `http_buf_lock`.
  Resuelto: 2026-07-06. Se agregó una bandera `toggled` local que acota el
  swap play/pause a un único intento; si el segundo intento también da 403,
  se retorna el error normalmente en vez de reintentar indefinidamente.

- [x] **1.2 Race condition sobre el buffer de tokens JSON compartido**
  `parse_objects.c:21`, `static json_tok_t tokens[MAX_TOKENS]` global sin
  mutex. `parse_track()` se llama desde `player_task` en `WS_DATA_EVENT`
  **sin tomar `http_buf_lock`** (`spotify_client.c:601-605`), mientras que
  `spotify_user_playlists`/`spotify_available_devices`/`get_access_token`
  sí toman ese lock pero usan el mismo `tokens[]` vía `parse_playlist`/
  `parse_available_devices`. Si se invoca una de estas funciones desde otra
  tarea mientras `player_task` procesa un evento WS, hay corrupción de datos
  concurrente. Mismo problema con los `static int output_len/in_items/
  brace_count` de `handler_callbacks.c`. El componente no es reentrante pese
  a exponer una API basada en `handle_t`.
  Resuelto: 2026-07-06. Se envolvieron las dos llamadas desprotegidas en
  `player_task` (`parse_connection_id` y `parse_track` en la rama
  `WS_DATA_EVENT`) con `ACQUIRE_LOCK/RELEASE_LOCK(http_buf_lock)`, el mismo
  mutex que ya protegía todos los demás call-sites de `parse_*`. Con esto
  `tokens[]` quedó protegido consistentemente en todo el componente ese
  mismo día. Más tarde, al aplicar 2.4, el buffer `tokens[]` (y los
  `static` de `handler_callbacks.c`) se movieron además al struct del
  cliente / a `evt_user_data_t`, eliminando el estado global compartido de
  raíz (ya no dependen únicamente de la disciplina de locking). El locking
  agregado acá se mantiene: sigue habiendo una única instancia de cliente
  compartida entre tareas, así que sigue siendo necesario.

- [x] **1.3 `ESP_ERROR_CHECK` sobre errores de red esperables (reboot)**
  `spotify_client.c:359, 407, 536, 541, 598`. `get_access_token`,
  `player_cmd(GET_STATE)` y el parseo de `conn_id` usan `ESP_ERROR_CHECK`,
  que reinicia el dispositivo ante cualquier fallo transitorio de red.
  Contradice la lógica de reintentos (`http_retries_available`,
  `RETRIES_ERR_CONN`) que ya existe en otras rutas.
  Resuelto: 2026-07-06. Los 5 sitios se cambiaron por chequeos explícitos de
  `esp_err_t` que degradan sin abortar el dispositivo:
  `spotify_user_playlists`/`spotify_available_devices` liberan la lista y
  devuelven `NULL`; en `player_task`, si falla `get_access_token`,
  `player_cmd(GET_STATE)` o `confirm_ws_session`, se loguea el error, se
  pone `enabled = 0` (y se cierra el websocket en el caso de
  `confirm_ws_session`) y se hace `continue`, quedando el player en estado
  deshabilitado a la espera de un nuevo `ENABLE_PLAYER_EVENT` en vez de
  reiniciar el equipo.

- [ ] **1.4 Esquema "episode" pedido pero no soportado (crash con podcasts)**
  `spotify_client.c:17` (`additional_types=episode`) pide episodios de
  podcast en el estado de reproducción, pero `parse_track()`
  (`parse_objects.c:170-201`) asume incondicionalmente el esquema de una
  canción (`artists`, `album.images`), inexistente en un episodio (usa
  `show`, sin `artists`). Como cada acceso está en `ERR_CHECK` (=
  `ESP_ERROR_CHECK`), reproducir un podcast reinicia el dispositivo.

- [x] **1.5 `assert()` sobre datos externos, no invariantes de programador**
  - `handler_callbacks.c:107`: `assert((data->payload_len)+1 <= buffer_size)`
    — mensaje WS más grande que `MAX_WS_BUFFER` (4096B) aborta el firmware.
    Ya marcado con `// TODO: don't use assert` en el código.
  - `parse_objects.c:44,46,255,179` y `spotify_client.c:596`: múltiples
    `assert()` sobre resultados de malloc/parsing dependientes de datos de
    red.
  Resuelto: 2026-07-06. Se reemplazaron todos los `assert()` de esta
  categoría por manejo explícito que descarta el dato problemático en vez de
  abortar el firmware:
  - `default_ws_event_cb` (`handler_callbacks.c`): si un mensaje WS excede el
    buffer, se loguea y se descarta; si era el último chunk del mensaje
    sobredimensionado, igual se libera `WS_READY_FOR_DATA` para no dejar
    trabado el pipeline del próximo mensaje.
  - `playlist_http_event_cb` (`handler_callbacks.c`): se agregó la bandera
    `item_overflow` para descartar un ítem de playlist que no entra en el
    buffer (en vez de abortar), y los `malloc`/`spotify_append_item_to_list`
    de esa misma función ahora, ante fallo, loguean y descartan el ítem
    liberando lo ya reservado, en vez de `assert`.
  - `parse_available_devices` y el loop de artistas de `parse_track`
    (`parse_objects.c`): ante fallo de `malloc`/`spotify_append_item_to_list`,
    se loguea y se corta el loop devolviendo una lista parcial (menos
    dispositivos/artistas) en vez de abortar.
  - `spotify_clone_track` (`spotify_client.c`): mismo tratamiento para el
    loop de clonado de artistas, con chequeo adicional de `strdup == NULL`.
  - `assert(conn_id)` en `player_task` (`spotify_client.c`): si
    `parse_connection_id` no puede extraer el id, se loguea, se deshabilita
    el player y se cierra el websocket en vez de abortar.

- [x] **1.6 Use-after-free potencial en `spotify_client_deinit`**
  `spotify_client.c:190,202`. `player_task` se crea con handle descartado
  (`xTaskCreate(..., NULL)`) y nunca se destruye en `deinit`. El propio
  código lo señala: `// TODO: make sure to set client to NULL` (línea 201).
  Si se llama `deinit` con la tarea activa, queda bloqueada sobre un
  `event_group`/`client` ya liberados → use-after-free al desbloquearse.
  Hoy no se dispara porque `main.c` usa un único cliente global que nunca
  se destruye, pero es una trampa para uso futuro.
  Resuelto: 2026-07-06. Se agregó `TaskHandle_t player_task_handle` al
  struct del cliente, capturado desde `xTaskCreate` en `spotify_client_init`.
  `spotify_client_deinit` ahora llama `vTaskDelete(client->player_task_handle)`
  como primer paso, antes de liberar cualquier recurso que la tarea use
  (`event_group`, buffers, mutex, etc.), evitando que la tarea se despierte
  o siga corriendo sobre memoria ya liberada.
  **Nota**: es un `vTaskDelete` abrupto, no una señal cooperativa de
  apagado; si la tarea está en medio de un `esp_http_client_perform` al
  momento de borrarla, esa conexión queda sin cerrar prolijamente (recurso
  del lado de ESP-IDF, no memoria nuestra). Una parada cooperativa (bit de
  "shutdown" + `vTaskDelete(NULL)` desde dentro de la propia tarea) sería
  más prolija pero es un cambio estructural mayor; queda anotada como
  posible mejora futura, no como bug abierto.

- [x] **1.7 Refresh de token inconsistente entre funciones públicas**
  Solo `player_task` (líneas 513-520) reintenta con token nuevo ante 401.
  `spotify_play_context_uri`, `spotify_user_playlists` y
  `spotify_available_devices` solo chequean `access_token_empty()` (cierto
  una única vez, al arrancar) y no detectan/reintentan ante 401. El campo
  `access_token.expiresIn` (`spotify_client.c:52`) está declarado pero
  **nunca se escribe ni se lee** — no hay refresco proactivo por expiración,
  solo el reactivo (y ni siquiera consistente entre funciones).
  Resuelto: 2026-07-06.
  - `parse_access_token` ahora también extrae `expires_in` (opcional, no
    aborta si Discord no lo manda) y `get_access_token_locked` calcula
    `access_token.expiresIn = time(NULL) + expires_in`. `access_token_empty()`
    se renombró a `access_token_needs_refresh()` y ahora también es `true`
    cuando el token está vencido, no solo cuando nunca se obtuvo.
  - Las 3 funciones públicas ahora reintentan una vez tras refrescar el
    token si la primera respuesta es 401, igual que `player_task`. Para
    evitar deadlock (no se puede volver a tomar `http_buf_lock` estando ya
    dentro de él) y evitar que otra tarea intercale un request y corrompa
    `ctx`/buffer entre el intento fallido y el reintento, se separó
    `get_access_token` en `get_access_token_locked` (sin lock propio, para
    usar dentro de una sección crítica ya abierta) + `get_access_token`
    (wrapper público que sí toma el lock). El reintento completo queda
    dentro de una única sección crítica.

- [~] **1.8 Autenticación vía endpoint interno de Discord con token de usuario**
  `spotify_client.c:14`. El access token de Spotify se obtiene golpeando
  `discord.com/api/v8/users/@me/connections/spotify/{uid}/access-token` con
  un token de **usuario** de Discord (`CONFIG_DISCORD_TOKEN`, no bot token).
  Riesgos: endpoint interno no documentado (puede cambiar sin aviso), uso
  de "self-token" contra ToS de Discord (riesgo de suspensión de cuenta), y
  el token almacenado tiene privilegios muy superiores a lo necesario (fuga
  = cuenta de Discord completa comprometida, no solo Spotify).
  **Decisión (2026-07-06): se descarta el reemplazo.** El token que entrega
  este endpoint de Discord habilita el acceso al WebSocket privado de
  `dealer.spotify.com` (eventos de reproducción en tiempo real: cambios de
  track, play/pause, progreso), algo que **no está disponible** para apps
  registradas vía el flujo OAuth2 público de Spotify (ese flujo da scopes
  para la Web API REST, no para el "dealer" interno que usan los clientes
  oficiales y la integración de Rich Presence de Discord). Migrar a OAuth2
  oficial implicaría perder los eventos push y pasar a polling del estado
  del player, degradando la experiencia (latencia, más tráfico, más
  consumo). Se prioriza la ventaja funcional sobre el riesgo, que queda
  aceptado y documentado explícitamente (ver advertencia en el código:
  `spotify_client.c` junto a `ACCESS_TOKEN_URL` y `Kconfig`).
  No se van a implementar cambios de código para este punto; se mantiene
  solo como advertencia permanente en el código y en este documento.

- [x] **1.9 Fuga de credenciales por logging en Debug**
  `main/main.c:51` habilita `esp_log_level_set("spotify_client", ESP_LOG_DEBUG)`
  en runtime. `get_access_token` (`spotify_client.c:795`) hace
  `ESP_LOGD(TAG, "Access Token obtained:\n%s", ...)`, imprimiendo el Bearer
  token completo por log/consola serie con el nivel que la app deja activo
  por defecto.
  Resuelto: 2026-07-06. Se quitó el token del mensaje; ahora loguea solo la
  longitud del token y el `expires_in` (útil para debug sin exponer el
  secreto). No se encontraron otros sitios logueando el token o el
  `CONFIG_DISCORD_TOKEN` completos.

- [x] **1.10 Menores**
  - `spotify_clone_track` (`spotify_client.c:459-479`): `strdup(src->album.url_cover)`
    sin chequear NULL (puede ser NULL si ninguna imagen tiene `height==300`,
    o en episodios) → `strdup(NULL)` es UB.
    Resuelto: 2026-07-06. Se agregó el helper `dup_or_null()` y se usa para
    `name`/`album.name`/`album.url_cover` en `spotify_clone_track`.
  - `spotify_play_context_uri` línea 319: `assert(str_len <= SPRINTF_BUF_SIZE)`
    se verifica **después** de que `sprintf` ya escribió. Usar `snprintf` y
    chequear el retorno antes.
    Resuelto: 2026-07-06. Se cambió a `snprintf` con el tamaño del buffer, y
    si el resultado indica truncamiento (`str_len < 0 || str_len >= SPRINTF_BUF_SIZE`)
    se aborta la operación devolviendo `ESP_ERR_INVALID_SIZE` en vez de
    proceder con un cuerpo JSON inválido/truncado.
  **Follow-up 2026-07-11**: usuario reportó en hardware real
  `E (...) spotify_client: No cover url` / `E (...) PLAYER_SCREEN: Failed to
  fetch album cover`. Se confirmó con `git diff` que el bloque de selección
  de portada (`parse_objects.c`, busca `height == 300` exacto en
  `album.images`) no fue tocado por los cambios de esta sesión (filtro de
  `"uri"`, colapso de `"payloads"`) — no es una regresión de eso, es
  exactamente este caso ya conocido: `url_cover` queda `NULL` cuando ninguna
  imagen del track trae `height == 300` (episodios de podcast, ítem 1.4
  abierto; o algún release sin esa variante de tamaño). El parseo del resto
  del track fue correcto (no hubo crash). El usuario pidió primero
  confirmar la causa antes de decidir un fix (se agregó un diagnóstico
  temporal para volcar el JSON crudo cuando pasara).
  **Fix implementado el mismo día**: en vez de exigir `height == 300`
  exacto, `parse_track()` ahora hace dos pasadas sobre `images` (los
  tokens de jsmn son indexables por `parent`, así que una segunda pasada
  por índice es segura): la primera busca el índice cuya `height` esté
  más cerca de `ALBUM_COVER_PREFERRED_SIZE` (nuevo `#define` en
  `spotify_client.h`, reemplaza el mágico `300` tanto acá como en
  `main/player_screen.c`), cortando temprano si hay un match exacto; la
  segunda vuelve a ese índice y duplica su `"url"`. Se logea con
  `ESP_LOGW` cuando el tamaño elegido no es el preferido (indicando
  cuántas imágenes había y cuál tamaño se usó en su lugar), y también
  cuando no hay ninguna imagen usable.
  Como el tamaño real ya no está garantizado a ser 300×300, `Album` ganó
  un campo **`cover_size`** (el lado real en px de `url_cover`, o `0` si
  no hay portada) — se copia en `spotify_clone_track()` y se resetea a
  `0` en `free_track()` junto con `url_cover`. Se descartó guardar
  `width`/`height` separados: las portadas de álbum de Spotify son
  siempre cuadradas, así que un solo valor alcanza sin validar un caso
  que la API no produce.
  **Segundo follow-up, mismo día**: en vez de saltear la portada cuando
  `cover_size` no es `ALBUM_COVER_PREFERRED_SIZE` (primera versión de
  este fix), ahora siempre se descarga y decodifica, y se hace zoom con
  LVGL para que se vea del mismo tamaño en pantalla sin importar la
  resolución real:
  - `pick_jpeg_scale()` (nueva, `player_screen.c`) elige el
    `JPEG_IMAGE_SCALE_*` (el decoder JPEG solo reduce en potencias de 2:
    `0`/`1_2`/`1_4`/`1_8`, nunca agranda) más detallado que aún así
    decodifique a `<= COVER_W_HALF` (150px) en cada eje — así nunca se
    excede el buffer fijo de `pixels` sin importar si la fuente es
    300/640/64px. Ej.: 640px → `1_8` (sale en 80×80); 300px → `1_2` (sale
    exacto en 150×150, como antes); 64px → `0` (sale tal cual, 64×64).
  - `decode_image()` (`main/decode_image.c`) ahora devuelve por
    out-params (`out_width`/`out_height`) la resolución real decodificada
    que reporta la librería (antes se descartaba). De paso se corrigió
    que el `esp_err_t` de `esp_jpeg_decode()` **se ignoraba por
    completo** (`ret` nunca se reasignaba) — ahora si el decode falla
    (p.ej. el propio chequeo de `outbuf_size` de la librería, que
    devuelve `ESP_ERR_NO_MEM` en vez de corromper memoria si alguna vez
    la estimación de tamaño no calzara exacto) se detecta y se propaga en
    vez de seguir con dimensiones de salida sin inicializar.
  - `player_screen.c` usa esa resolución real para `pic_img_dsc.header.w/h/stride/data_size`,
    y aplica `lv_image_set_scale(ui_CoverImage, 256 * COVER_W_HALF / decoded_w)`
    (256 = zoom 1:1 en LVGL) para estirar la imagen decodificada — sea cual
    sea su tamaño real — y llenar el mismo recuadro en pantalla. El
    fallback "sin portada" (`reset_cover_to_blank()`, nueva, deduplica lo
    que antes eran dos bloques idénticos) vuelve todo a la resolución fija
    de siempre con zoom 1:1.
  - Se agrandó el buffer de descarga del JPEG crudo (comprimido) de
    90.000 B (`COVER_W*COVER_H`, pensado solo para 300px) a un
    `ALBUM_COVER_JPEG_BUF_SIZE` fijo de 128 KB, con margen para la
    variante de 640px.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía.
  **Pendiente de decisión, no implementado**: el usuario preguntó si
  convendría aprovechar más pantalla y mostrar la portada más grande
  (hoy 150×150 en una pantalla de 480×320 landscape). El ancho sobra
  (150 de 480), pero el alto es el recurso escaso: hoy ya se usa casi
  todo (14px de margen arriba, ~8px libre hasta el título, 320 de alto
  total, ~8px libre abajo de los botones de transporte). Agrandar la
  portada requeriría ajustar espaciados/posiciones del resto de los
  elementos (título, artista, barra de progreso, botones), no es un
  cambio de una línea — queda como tarea de layout aparte si se decide
  encarar.
  **Tercer follow-up, mismo día**: se simplificaron los macros de
  `player_screen.c` — `COVER_W`/`COVER_H`/`COVER_W_HALF`/`COVER_H_HALF`
  colapsados a un solo `COVER_SIZE_HALF`, ya que la portada siempre es
  cuadrada (mismo razonamiento que llevó a no separar `width`/`height`
  en `Album.cover_size`). Verificado con `idf.py build`, sin errores ni
  warnings nuevos.

- [x] **1.11 Escáner de playlists sin conciencia de strings JSON**
  `playlist_http_event_cb` (`handler_callbacks.c`) cuenta llaves y decide qué
  espacios "comprimir" byte a byte, sin ningún tracking de si el byte actual
  está dentro de un string JSON. Nombres/descripciones de playlist son texto
  libre y pueden contener `{`/`}` (ej. `"lo-fi {study} beats"`) sin que estén
  balanceados dentro de ese string — en ese caso el contador de profundidad
  se desincroniza, mezclando o cortando mal los objetos de playlist
  siguientes. El fragmento resultante se pasa a `parse_playlist()`, que
  sigue envolviendo el parseo en `ERR_CHECK` (`ESP_ERROR_CHECK`), así que un
  JSON mal cortado puede abortar el dispositivo — un vector de crash más
  probable en uso real que 1.4 (podcasts), porque "ver mis playlists" es un
  flujo mucho más común. El heurístico de espacios tiene el mismo problema
  en menor escala (puede comprimir espacios que son contenido real de un
  string). Encontrado en sesión de análisis posterior a la primera pasada
  (no estaba en el checklist original).
  Resuelto: 2026-07-06. Se agregaron `in_string`/`escaped` a
  `evt_user_data_t` (`spotify_client_priv.h`) y el bucle byte a byte ahora
  actualiza ese estado (toggle de `in_string` en cada `"` no escapado,
  `escaped` en cada `\`) antes de decidir si un byte cuenta como llave
  estructural o como espacio comprimible — ambos chequeos ahora están
  gateados por `!in_string`. El copiado de bytes al buffer no cambia (sigue
  siendo incondicional mientras `brace_count > 0`), así que el contenido de
  los strings (incluyendo `{`/`}`/espacios literales) se preserva íntegro.
  **Se decidió explícitamente mantener la estrategia de streaming/extracción
  incremental** en vez de rediseñarla (ver alternativas evaluadas: paginar
  con `limit`/`offset` más chico y parsear cada página con el parser
  jsmn estándar, o reducir el payload con el parámetro `fields` de la Web
  API de Spotify — este último **no aplica** a `/me/playlists`, que solo
  soporta `limit`/`offset`, solo a `/playlists/{id}` y
  `/playlists/{id}/tracks`). Este es el fix mínimo sobre la estrategia
  actual, no una reescritura.
  **Nota relacionada, no resuelta**: `memcpy_trimmed` (usado por
  `json_http_event_cb` para compactar la respuesta completa de track/device/
  token) tiene el mismo heurístico de espacios sin conciencia de strings, y
  en teoría el mismo riesgo (nombres de track/artista/álbum también son
  texto libre). No se tocó porque no fue parte de lo pedido en esta sesión;
  queda como candidato a un fix análogo si se decide abordarlo.

- [x] **1.12 `parse_playlist` crasheaba el dispositivo (ERR_CHECK sobre dato externo)**
  Probado en hardware real: `spotify_user_playlists()` reventaba con
  `ESP_ERROR_CHECK failed` dentro de `parse_playlist`
  (`parse_objects.c:72`, `json_obj_dup_string(&jctx, "name", ...)`),
  reiniciando el dispositivo. Exactamente el mismo patrón que 1.4/1.5
  (`ERR_CHECK`/`ESP_ERROR_CHECK` sobre el parseo de datos controlados por la
  red) pero en una función que no había quedado cubierta por el fix de 1.5
  (ese fix cubrió los `assert()` de malloc/append alrededor de
  `playlist_http_event_cb`, no el parseo en sí dentro de `parse_playlist`).
  Repasé a fondo el escáner de 1.11 (brace-counting + conciencia de
  strings) buscando por qué el fragmento reconstruido no traía "name" en el
  nivel esperado, y no until ahora no encontré una falla lógica clara ahí
  para una respuesta compacta (confirmado en el log real del usuario:
  Spotify manda JSON sin espacios, así que el bloque de whitespace de 1.11
  ni se activa) — **la causa de fondo del fragmento malformado sigue sin
  identificarse con certeza**.
  Resuelto (el síntoma, no necesariamente la causa raíz): 2026-07-07.
  `parse_playlist` ya no usa `ERR_CHECK`: si `json_parse_start_static` o
  cualquiera de los `json_obj_dup_string` fallan, loguea el fragmento
  completo por `ESP_LOGE` (visible con el nivel de log default, a
  diferencia del `ESP_LOGD` de `playlist_http_event_cb` que solo se ve con
  "HANDLER_CALLBACKS" en DEBUG) y devuelve `ESP_FAIL` en vez de abortar.
  `playlist_http_event_cb` ahora descarta el ítem (`free(name)`/`free(uri)`,
  ambos NULL-safe) en vez de agregarlo a la lista cuando esto pasa. El
  efecto práctico: en vez de reiniciar el dispositivo, se pierde ese ítem
  puntual de la lista (la playlist no aparece) y queda logueado el JSON
  exacto que lo disparó — la próxima vez que ocurra, ese log da la pista
  definitiva para encontrar la causa raíz real.
  **Actualización 2026-07-08**: el usuario mandó el log completo con el JSON
  exacto (ver 1.13) — con eso se encontró y arregló la causa raíz real,
  descrita ahí.

- [x] **1.13 Causa raíz de 1.12: `brace_count` se iba a negativo al terminar el array `"items"`**
  Con el log real en mano (gracias al `ESP_LOGE` agregado en 1.12) se vio
  que en un segundo fetch de playlists (después de haber reproducido una
  playlist seleccionada), CADA sub-objeto anidado de cada playlist
  (`external_urls`, cada elemento de `images`, `owner`, `tracks`) se estaba
  extrayendo como si fuera un ítem completo — el patrón clásico de un
  contador de profundidad desalineado.
  Causa raíz: `playlist_http_event_cb` (`handler_callbacks.c`) no tiene
  forma de saber cuándo termina el array real `"items"` — solo cuenta
  `{`/`}`. Al terminar el ÚLTIMO playlist real (`brace_count` vuelve a 0
  correctamente), el escaneo sigue procesando el resto de la respuesta,
  incluyendo el `}` que cierra el objeto envolvente de Spotify
  (`{"href":...,"items":[...],"limit":50,...}`). Ese `}` extra decrementaba
  `brace_count` **sin ningún resguardo** (`user_data->brace_count--;`
  incondicional), dejándolo en un valor que ya no vuelve a alinearse solo.
  Aunque el reset de `HTTP_EVENT_ON_FINISH`/`DISCONNECTED` debería limpiar
  esto entre requests, el efecto observado (el segundo fetch arrancando ya
  "corrido" desde el primer playlist) indica que en la práctica no alcanza
  a corregirse a tiempo entre dos fetches consecutivos.
  Resuelto: 2026-07-08, con dos capas de defensa:
  1. `brace_count` ahora solo se decrementa si es `> 0`
     (`handler_callbacks.c`), así nunca se va a negativo por ese `}` extra
     del wrapper; de paso se agregó `current_size > 0` a la condición de
     "fin de playlist" para no reprocesar un ítem vacío si un `}` sobrante
     llega estando ya en profundidad 0.
  2. `spotify_user_playlists()` (`spotify_client.c`) ahora resetea
     explícitamente `in_items`/`brace_count`/`item_overflow`/`in_string`/
     `escaped`/`current_size` antes de cada fetch, como red de seguridad
     adicional independiente de que el reset de ON_FINISH/DISCONNECTED se
     haya disparado correctamente en el request anterior.
  **Nota de honestidad**: no pude reproducir esto sin hardware; el mecanismo
  exacto de por qué el reset entre requests no alcanzaba a limpiar el
  estado a tiempo no quedó 100% confirmado, pero ambas capas de defensa
  atacan directamente el bug demostrable (decremento sin resguardo) y
  deberían cubrir el síntoma real independientemente del detalle fino.

- [x] **1.14 `CHANGE_VOLUME` era un comando fantasma con una mina de NULL adentro**
  Encontrado en la re-revisión post-2.6/2.7 (2026-07-07), no estaba en el
  checklist original. `PlayerCommand_t` (`spotify_client_priv.h`) tenía un
  valor `CHANGE_VOLUME`, pero **nada en el código lo producía nunca**:
  `bits_to_player_cmd` (`player_commands.c`) no tenía ningún bit que mapeara a
  él, y `SendEvent_t`/`player_dispatch_event` no tenían ningún evento de
  volumen. Era inalcanzable. El problema real: si alguien lo conectaba sin
  mirar el `switch` de `player_cmd`, `case CHANGE_VOLUME: break;` no asignaba
  `url` (quedaba `NULL`), y el código seguía de largo hasta
  `perform_http_request(client, ..., url, method, &s_code)` →
  `esp_http_client_set_url(http_client, NULL)`.
  Resuelto: 2026-07-07, implementando la funcionalidad de verdad (decisión
  del usuario: quería poder subir/bajar volumen por touch en el player).
  - `Device.volume_percent` (`spotify_client.h`) pasó de `char[4]` (nunca
    poblado en la ruta real, solo en el `onDevicePlaying` muerto de 2.8, y
    con un bug de tratar un número JSON como string) a `int` (0-100, o `-1`
    = desconocido). `spotify_client_init` lo inicializa en `-1` (antes
    quedaba en `0` por el `calloc`, indistinguible de un volumen real de
    0%).
  - `parse_track()` (`parse_objects.c`) ahora completa el stub `// volume...`
    que quedaba: nuevo helper privado `parse_device_volume()` lee
    `device.volume_percent` (sibling de `item` al nivel del estado) en las
    dos ramas (SAME_TRACK y NEW_TRACK/estado inicial). Defensivo a
    propósito (no `ERR_CHECK`): si falta `device` o `volume_percent` en el
    payload, deja el valor anterior sin abortar (principio 3.2).
  **v1 (2026-07-07, ver más abajo v2)**: `PlayerCommand_t` cambió
  `CHANGE_VOLUME` por `VOLUME_UP`/`VOLUME_DOWN` reales, con dos botones
  +/- ruteados por `player_task`/EventGroup (misma ruta que prev/pause/next).
  Probado en hardware por el usuario: el volumen funcionaba, pero coincidió
  con el reporte de 1.15 (el player deja de recibir eventos) — no se
  confirmó relación causal, pero por las dudas **este diseño se abandonó y
  reemplazó por completo** (v2), así que si 1.15 estaba relacionado con
  meterle más carga a `player_task`, ya no aplica.

  **v2 (2026-07-08), diseño final**: control de volumen por **slider
  vertical con debounce**, decidido junto con el usuario tras evaluar dos
  alternativas para el lado servidor:
  (a) rutear por `player_task`/EventGroup como el resto de los comandos
  (consistente, pero un bit no lleva payload — habría que sumar un campo
  compartido tipo `pending_volume_percent`), vs.
  (b) una llamada síncrona directa, igual a `spotify_play_context_uri`.
  Se eligió **(b)** por dos razones: un slider debounced dispara una vez
  por "asentamiento" (no repetidamente como prev/pause/next), así que se
  parece más a una acción puntual (como seleccionar una playlist) que a un
  botón de transporte; y porque no se quiso sumar más superficie a
  `player_task` mientras 1.15 sigue sin confirmar.
  - `spotify_set_volume(client, volume_percent, status_code)` (nueva,
    `player_commands.c`, declarada en `spotify_client.h`): clampea
    `[0,100]`, seguí el mismo patrón que `spotify_play_context_uri`
    (chequeo/refresh de token, `ACQUIRE_LOCK`, arma la URL en
    `client->sprintf_buf` vía `PLAYERURL(VOLUME)` + el valor, PUT, reintento
    único ante 401). Como `PUT /me/player/volume` devuelve 204 sin cuerpo,
    no hay nada que parsear de vuelta: actualiza
    `track_info->device.volume_percent` de forma optimista solo si la
    petición fue 200/204.
  - Se eliminó por completo el mecanismo v1: `VOLUME_UP`/`VOLUME_DOWN`
    de `PlayerCommand_t`, `DO_VOLUME_UP`/`DO_VOLUME_DOWN` de
    `spotify_client_priv.h`, `DO_VOLUME_UP_EVENT`/`DO_VOLUME_DOWN_EVENT`
    de `SendEvent_t`, y todo el manejo asociado en `player_dispatch_event`/
    `player_task.c`/`bits_to_player_cmd`. También `VOLUME_STEP_PERCENT`/
    `VOLUME_UNKNOWN_BASELINE_PERCENT` (ya no hacen falta: el slider manda el
    valor absoluto directamente, no hay que calcular un delta desde el
    último conocido).
  - `spotify_clone_track` (`player_commands.c`) ahora sí copia
    `device.volume_percent` (antes ni siquiera estaba en la lista de campos
    clonados, quedaba en lo que tuviera el `track` local de
    `player_screen.c`) — necesario para que el slider arranque mostrando el
    volumen real en vez de un valor arbitrario.
  - UI (`main/ui/`): un solo `ui_VolumeSlider` (`lv_slider`, orientación
    vertical explícita vía `lv_slider_set_orientation`, no por
    auto-detección de ancho/alto) reemplaza los dos botones +/- de v1,
    misma esquina superior izquierda. El *debounce* vive en `ui_events.c`
    (`volumeSliderChangedFn` + `lv_timer` de `VOLUME_DEBOUNCE_MS = 400`ms,
    reseteado en cada `LV_EVENT_VALUE_CHANGED`; al vencer, vuelca el valor
    asentado a `volume_target_queue` vía `xQueueOverwrite` — cola de
    tamaño 1, así que solo importa el último valor). Un `volume_task`
    dedicado (`player_screen.c`, mismo patrón que `playlist_task` en
    `playlist_screen.c`) consume esa cola y hace la llamada bloqueante a
    `spotify_set_volume()` — así el HTTP no corre nunca en la tarea de
    `lvgl_port` (que congelaría el render/input mientras dura la request).
    `player_screen_start()` sincroniza el slider con el volumen real solo
    en `NEW_TRACK` (no en cada `SAME_TRACK`) para no pelearse con el dedo
    del usuario a mitad de un drag.
  Verificado con `idf.py build` completo (recompilación limpia de todos los
  archivos tocados en ambas iteraciones), sin errores ni warnings nuevos.

  **Probado en hardware (2026-07-08/09), primer bug encontrado y
  arreglado**: la barra al 100% superaba el límite superior de la
  pantalla. Causa: el tema default de LVGL le agrega al `LV_PART_KNOB` un
  padding (~6px por lado, `lv_theme_default.c`) que hace al knob más ancho
  que la barra; en el valor máximo el knob queda centrado justo en el
  borde superior del track, así que con solo 8px de margen (`y=8`) el
  knob sobresalía por encima de `y=0`. Arreglado subiendo el margen
  superior a `y=20` y fijando un padding de knob explícito y más chico
  (`lv_obj_set_style_pad_all(ui_VolumeSlider, 3, LV_PART_KNOB | LV_STATE_DEFAULT)`)
  en vez de depender del valor del tema por defecto.

  **Mejora propuesta por el usuario, implementada 2026-07-09**: reflejar
  también cambios de volumen hechos desde **otro** cliente de Spotify (el
  teléfono, otro dispositivo, etc.), no solo los que se originan desde
  este control. El usuario notó (correcto) que `parse_device_volume()` ya
  se llama tanto en el fetch de estado inicial como en cada evento
  `PLAYER_STATE_CHANGED` del WebSocket (rama `SAME_TRACK`), y propuso la
  solución: comparar el valor actual contra el que llega por WS, y solo
  actualizar el slider si difieren.
  Se implementó exactamente así, sin agregar ningún evento nuevo (el
  `VOLUME_CHANGED` de `Event_t` sigue muerto/sin usar, no hizo falta): el
  dato ya viaja en el payload del evento `SAME_TRACK` existente (es parte
  del mismo `TrackInfo*`), así que alcanzó con que el `case SAME_TRACK` de
  `player_screen_start()` (`main/player_screen.c`) compare
  `t_updated->device.volume_percent` contra `track.device.volume_percent`
  (el valor que la pantalla ya tenía) y solo si difieren (y el nuevo valor
  es válido, `>= 0`) actualice ambos.
  La "pelea con el dedo" se resuelve sola sin lógica extra: cuando el
  cambio lo originamos nosotros, `spotify_set_volume()`
  (`player_commands.c`) ya escribió el nuevo valor de forma optimista en
  `client->track_info->device.volume_percent` **antes** de que llegue el
  eco del WS confirmándolo — así que cuando `parse_device_volume()` compara
  contra ese mismo campo compartido como "valor viejo", ya coincide con el
  nuevo (no hay diferencia, no se toca el slider). Solo un cambio genuino
  desde otro cliente deja un valor "viejo" distinto del que trae el WS, y
  ahí sí se actualiza. Caso límite no cubierto (aceptado, no bloqueante):
  si el usuario re-arrastra el slider a un valor nuevo *durante* la ventana
  de ida y vuelta de un cambio anterior, puede haber un parpadeo momentáneo
  al valor viejo, auto-corregido con el próximo evento.
  Verificado con `idf.py build`, sin errores ni warnings nuevos. No
  probado en hardware todavía (falta confirmar con logs reales que el WS
  efectivamente entrega `volume_percent` en `SAME_TRACK` para otros
  clientes, y ver el comportamiento táctil real).

  **Logging de diagnóstico temporal agregado** (2026-07-08/09, a pedido
  del usuario, "para pasar crudo") — sirve tanto para la mejora de arriba
  como para 1.15 (el player que deja de escuchar eventos, todavía sin
  confirmar):
  - `parse_device_volume()` (`parse_objects.c`): `ESP_LOGI` con el
    `volume_percent` parseado (o su ausencia) cada vez que se llama.
  - `player_task()` (`player_task.c`): `ESP_LOGI` con `uxBits` en cada
    despertar de `xEventGroupWaitBits`, y con `spotify_evt.type` +
    `device.volume_percent` cada vez que `parse_track()` devuelve (tanto
    en el fetch inicial de `GET_STATE` como en cada `WS_DATA_EVENT`).
  - `player_screen_start()` (`player_screen.c`): `ESP_LOGI` con
    `event.type` cada vez que `spotify_wait_event()` devuelve un evento —
    la vista del consumidor externo de la misma cola, para poder comparar
    contra los logs de `player_task` si el freeze de 1.15 vuelve a
    ocurrir (si player_task sigue logueando pero esto no, el problema está
    más para el lado de `player_screen_start`/`event_queue`, no de
    `player_task`).
  Usan `ESP_LOGI` (no `ESP_LOGD`) a propósito: `main.c` solo eleva el tag
  `"spotify_client"` a `ESP_LOG_DEBUG`, no `"PARSE_OBJECT"` ni
  `"PLAYER_SCREEN"`, así que con `ESP_LOGD` estos logs no se verían sin
  cambiar la config. **Retirar este logging una vez que 1.15 se resuelva
  y la mejora de arriba esté decidida/implementada** — es ruido
  permanente si se deja para siempre.
  Verificado con `idf.py build`, sin errores ni warnings nuevos.

- [x] **1.16 Barra de progreso convertida en slider táctil (seek)**
  A pedido del usuario, 2026-07-09/10: la barra de progreso (`ui_ProgressBar`)
  era un `lv_bar` puramente decorativo (sin interacción táctil); se convirtió
  en un `lv_slider` que permite adelantar/retroceder tocando o arrastrando,
  con el mismo esquema arquitectónico ya usado para el volumen (1.14 v2):
  cola de tamaño 1 + tarea dedicada + llamada síncrona nueva.
  - `spotify_seek_to_position(client, position_ms, status_code)`
    (`player_commands.c`, declarada en `spotify_client.h`): mismo patrón que
    `spotify_set_volume` (clamp, chequeo/refresh de token, `ACQUIRE_LOCK`,
    URL armada en `client->sprintf_buf` vía `PLAYERURL(SEEK)` +
    `PUT /me/player/seek?position_ms=N`, reintento único ante 401,
    actualización optimista de `track_info->progress_ms` solo si la
    respuesta fue 200/204).
  - **Diferencia deliberada con el volumen**: en vez de debounce (esperar
    a que el usuario deje de tocar), el commit dispara en
    `LV_EVENT_RELEASED` (`ui_event_ProgressBar`, `ui.c`, reemplazando el
    boilerplate muerto de SquareLine `_ui_bar_set_property`/
    `LV_EVENT_VALUE_CHANGED` que nunca se disparaba realmente porque
    `lv_bar` no es interactivo y nada llamaba `lv_obj_send_event` sobre
    él). Un scrubber se comporta mejor así: la posición final al soltar
    el dedo *es* la intención del usuario, no hace falta esperar un
    timeout extra después de soltar.
  - `player_screen_start()` (`player_screen.c`): se eliminó la variable
    `percent` (y sus 3 sitios de cálculo `progreso*100/duración`) a favor
    de usar `progress_ms` directamente como valor/rango del slider — más
    simple y de paso más robusto (ya no hay una división que asuma
    `duration_ms != 0`, aunque esto último no era el objetivo, es un
    efecto colateral). El rango del slider se re-escala a
    `[0, track.duration_ms]` en cada `NEW_TRACK` (la duración cambia por
    track). La actualización periódica (cada `PROGRESS_TICK_MS`) que
    empuja `progress_ms_now` al slider ahora chequea
    `!lv_obj_has_state(ui_ProgressBar, LV_STATE_PRESSED)` antes de
    tocarlo, para no pelearle la posición al dedo del usuario mientras
    arrastra — mismo problema que se resolvió para el volumen, pero acá
    hacía falta explícitamente porque esta barra se actualiza sola cada
    segundo (no solo ocasionalmente en eventos de WS).
  - Nuevas piezas de plomería (mismo patrón que `volume_task`/
    `volume_target_queue`): `seek_target_queue`
    (`app_globals.h`/`main.c`) + `seek_task` (`player_screen.c`) +
    `seekSliderChangedFn` (`ui_events.c`, `xQueueOverwrite`).
  - Estilo del knob: mismo cuidado que el slider de volumen (pad
    explícito de 3px en `LV_PART_KNOB` en vez del default del tema,
    ~6px) aunque acá el margen horizontal (70px a cada lado, la barra
    mide 340 de 480 de ancho de pantalla) ya hacía improbable el
    problema de desborde que sí tuvo el slider de volumen en su esquina.
  **Caso límite aceptado, no resuelto** (mismo tipo de trade-off que el
  del volumen): justo después de soltar el dedo, el próximo tick
  periódico (hasta `PROGRESS_TICK_MS` = 1s después) puede mostrar
  brevemente la posición previa al seek hasta que un evento `SAME_TRACK`
  real confirme la nueva posición — un parpadeo acotado y auto-corregible,
  no se agregó sincronización cruzada entre tareas para eliminarlo por
  completo (se consideró y se decidió que no valía la complejidad extra).
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía.

- [x] **1.17 Fallos de `spotify_set_volume`/`spotify_seek_to_position` eran
  completamente silenciosos**
  Encontrado con el primer log real de hardware (2026-07-10) probando 1.14
  v2/1.16: varios PUT de volumen y de seek devolvieron **404** (uno de seek
  dio 502). La URL armada era correcta (coincide con lo documentado por la
  Web API de Spotify), así que no es un bug de construcción de request —
  es Spotify rechazando la operación, muy probablemente por no tener en ese
  momento un dispositivo "activo" para el endpoint de control (aun cuando
  el WS seguía empujando cambios de estado — llegó a coincidir con un
  evento `DEVICE_STATE_CHANGED` con `"devices":[]` justo antes del primer
  404). El 502 es transitorio del lado de Spotify, no nuestro.
  El problema real, no la causa del 404 en sí: `volume_task`/`seek_task`
  (`player_screen.c`) capturaban el `status_code` de estas dos funciones
  pero **nunca lo revisaban** — ni logueaban el fallo más allá del log
  genérico de `perform_http_request`, ni avisaban nada. Peor: el slider ya
  mostraba el valor que el usuario había tocado (LVGL lo mueve solo, al
  touch, sin importar si el PUT después tuvo éxito), así que ante un fallo
  la pantalla quedaba mintiendo — mostrando el valor pedido, no el
  realmente vigente en Spotify.
  Resuelto: 2026-07-10.
  - `HTTP_STATUS_NO_CONTENT` (204) se movió de `spotify_client_priv.h`
    (privado) a `spotify_client.h` (público): hacía falta que
    `player_screen.c` (fuera del componente) pudiera distinguir un éxito
    real (200/204) de un fallo HTTP usando el mismo `status_code` que ya
    devuelven `spotify_set_volume`/`spotify_seek_to_position`, sin
    necesitar otro out-param solo para eso.
  - `volume_task`/`seek_task` ahora chequean `err`/`status_code`; ante
    cualquier resultado que no sea 200/204 loguean con `ESP_LOGW` (target,
    `esp_err_to_name(err)`, `status_code`) y **resincronizan el slider** al
    último valor confirmado — en vez de dejar la UI mostrando un cambio
    que nunca se aplicó.
  - Para poder resincronizar sin tocar el struct interno (opaco) de
    `esp_spotify_client`, la variable `track` de `player_screen.c` (antes
    `static` local a `player_screen_start()`) pasó a ser `static` de
    archivo, visible también para `volume_task`/`seek_task` — evita
    exponer una función pública nueva solo para leer
    `device.volume_percent`/`progress_ms` desde fuera del componente.
  **No se investigó ni se intentó arreglar la causa raíz del 404** (el
  comportamiento de Spotify respecto a "dispositivo activo" para
  control), solo se dejó de ser silencioso — queda como posible ítem
  futuro si se vuelve muy frecuente en uso real.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía (el log que motivó esto no incluye una
  repetición del fallo con el fix ya puesto).

- [x] **1.18 Cosmético: la barra de seek volvía a la posición vieja ~1s
  después de soltar el dedo**
  Reportado por el usuario (2026-07-10), era exactamente el caso límite ya
  anotado como "aceptado" en 1.16: al soltar el slider, el tick periódico
  de `player_screen_start()` (cada `PROGRESS_TICK_MS`) sigue extrapolando
  desde `track.progress_ms`/`event_stamp`, que todavía no reflejan el seek
  recién hecho (solo se actualizan cuando llega un evento real
  `SAME_TRACK`/`NEW_TRACK`) — por eso la barra "rebota" a la posición vieja
  por hasta 1 segundo antes de corregirse.
  Se evaluaron dos estrategias con el usuario: (a) actualización optimista
  de `track.progress_ms`/`event_stamp` con rollback en `seek_task` si el
  PUT falla, o (b) congelar la barra/label un rato tras soltar, sin tocar
  nada del backend, hasta que llegue la confirmación real o venza un
  timeout. El usuario prefirió **(b)**, explícitamente por mantener esto
  como una preocupación de la UI y no cargar más al backend/`seek_task`.
  Resuelto: 2026-07-10, íntegramente en `player_screen.c` (sin tocar
  `ui_events.c`, `ui.c` ni el componente `spotify_client`):
  - `SEEK_GRACE_MS = 2000` (nueva constante).
  - `seek_grace_until` (variable local a `player_screen_start()`, no hace
    falta que sea de archivo): un deadline que se empuja hacia adelante
    mientras el slider está `LV_STATE_PRESSED`, así arranca a contar recién
    cuando se suelta.
  - `got_real_update` (bool, reseteado a `false` al principio de cada
    vuelta del `while(1)`, puesto en `true` dentro de los cases
    `NEW_TRACK`/`SAME_TRACK`): distingue "esta vuelta trajo un dato
    confirmado del servidor" de "esta vuelta es solo el recálculo
    periódico por timeout".
  - El bloque final (donde se pisa `ui_ProgressBar`/`ui_TrackElapsedLabel`)
    pasó a una cadena if/else-if/else: presionado → como antes + extiende
    el deadline; no presionado y sin dato real y todavía dentro de la
    ventana → no tocar nada (se queda mostrando lo que el usuario dejó);
    en cualquier otro caso (dato real llegó, o venció la ventana) →
    actualizar ambos normalmente.
  El resync-por-fallo de `seek_task` (1.17) queda intacto y complementa
  esto: si el PUT falla rápido, corrige antes de que termine la ventana;
  si no llega nada a tiempo, al vencer la ventana se retoma la
  extrapolación desde el `track.progress_ms` real (nunca modificado de
  forma optimista en este diseño), así que si el seek falló, lo que se
  termina mostrando es la posición verdadera, no una mentira.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía.

- [x] **1.19 El refresco reactivo de token (401) nunca se disparaba en la
  práctica — token vencido dejaba la app rota hasta reiniciar**
  Encontrado con un log real de hardware (2026-07-10): al vencer el access
  token (~26 minutos en esta sesión), **todo** pedido HTTP a la Web API de
  Spotify (seek, play, etc. — los eventos de WS seguían llegando bien)
  empezó a fallar así:
  ```
  E HTTP_CLIENT: This authentication method is not supported: Bearer realm="spotify", error="invalid_token", ...
  E spotify_client: HTTP request failed: ESP_ERR_NOT_SUPPORTED
  W spotify_client: Retrying 1/3...  (x3, todas fallan igual)
  ```
  Revisé el código fuente de `esp_http_client` (ESP-IDF) para entender la
  causa exacta: cuando la respuesta es 401, `esp_http_check_response()`
  llama a `esp_http_client_add_auth()`, que solo sabe negociar
  automáticamente `Basic`/`Digest`; Spotify manda
  `WWW-Authenticate: Bearer realm="spotify", error="invalid_token"...`, un
  esquema que no reconoce, así que devuelve **`ESP_ERR_NOT_SUPPORTED`**
  desde `esp_http_client_perform()` — no un 401 "limpio". Confirmado por
  grep: las únicas dos apariciones de `ESP_ERR_NOT_SUPPORTED` en todo
  `esp_http_client.c` están dentro de esa función, alcanzable únicamente
  desde el caso 401, así que la señal es inequívoca. Importante:
  `client->response->status_code` **ya queda en 401** en ese punto,
  `esp_http_client_get_status_code()` sigue devolviendo el valor correcto
  aunque `perform()` haya devuelto error.
  El bug real: `perform_http_request()` (`spotify_client.c`) solo llamaba
  a `esp_http_client_get_status_code()` cuando `err == ESP_OK`; con
  `ESP_ERR_NOT_SUPPORTED` caía derecho al loop de reintento por error de
  conexión (`RETRIES_ERR_CONN`, pensado para fallas de red transitorias),
  reintentando el mismo token vencido 3 veces (garantizado a fallar
  igual las 3), y devolvía `err != ESP_OK`/`s_code == 0` al llamador. Como
  **todos** los call sites (`spotify_play_context_uri`,
  `spotify_user_playlists`, `spotify_available_devices`,
  `spotify_set_volume`, `spotify_seek_to_position`, y el manejo de
  `player_cmd` en `player_task.c`) refrescan el token solo cuando
  `err == ESP_OK && s_code == HttpStatus_Unauthorized`, esa condición
  nunca se cumplía — el refresco reactivo (agregado en 1.7) estaba
  efectivamente muerto en este escenario real, y como el endpoint de
  Discord no manda `expires_in` (ver 1.7), el refresco proactivo por
  vencimiento tampoco puede funcionar nunca. Resultado: una vez vencido el
  token, la app queda rota para cualquier interacción HTTP hasta un
  reinicio (que vuelve a pasar por `ENABLE_PLAYER`, el único lugar que
  llama `get_access_token` incondicionalmente).
  Resuelto: 2026-07-10, en un solo lugar (`perform_http_request()`,
  `spotify_client.c`), sin tocar ninguno de los call sites individuales:
  si `esp_http_client_perform()` devuelve `ESP_ERR_NOT_SUPPORTED` **y**
  `esp_http_client_get_status_code()` confirma 401, se lo reporta como el
  401 limpio que en realidad es (`err = ESP_OK; s_code =
  HttpStatus_Unauthorized;`), dejando que la lógica de refresco-y-reintento
  ya existente en cada call site haga su trabajo sin modificaciones.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía (haría falta esperar a que el token
  vuelva a vencer para confirmarlo en vivo).

- [x] **1.20 Nueva funcionalidad: listar y transferir playback a otro
  dispositivo**
  Pedida por el usuario, 2026-07-10. Se evaluó un plan antes de
  implementar (ver hilo): reusar `spotify_available_devices()`
  (ya existía) + una función nueva de transferencia, y para la UI, un
  **modal** en vez de una pantalla nueva (decisión del usuario: lo pidió
  en la misma fila de transporte, con un botón que abra un modal, para no
  competir por espacio en las esquinas ya ocupadas por playlists/volumen).
  También decidió explícitamente **no forzar `play=true`** al transferir
  (si estaba pausado, sigue pausado en el nuevo dispositivo).
  - `DeviceItem_t` (`spotify_utils.h`) ganó `bool is_active` (parseado
    defensivamente, no `ERR_CHECK`, en `parse_available_devices` —
    `parse_objects.c`), para poder resaltar el dispositivo actual en la
    lista.
  - `spotify_transfer_playback(client, device_id, status_code)`
    (`player_commands.c` + `spotify_client.h`): `PUT /me/player` con body
    `{"device_ids":["<id>"]}` (sin campo `"play"`, a propósito). Mismo
    patrón que `spotify_play_context_uri` (cuerpo en `sprintf_buf`,
    reintento en 401 — que además ya se beneficia del fix de 1.19).
  - UI: `ui_DeviceModal` (`ui_PlayerScreen.c`) es un hijo de
    `ui_PlayerScreen`, no una screen SquareLine separada — abrir/cerrar es
    solo togglear `LV_OBJ_FLAG_HIDDEN`, sin `lv_disp_load_scr`. Backdrop
    semi-transparente (clickeable, para no dejar pasar toques al player de
    atrás) + panel centrado con título, label de estado y `lv_list`. Botón
    de entrada `ui_DeviceBtn` en la fila de transporte, `x=140`,
    continuando la progresión aritmética `-70/0/+70` de Prev/Pause/Next
    para que el espaciado quede consistente.
    **Cierre del modal, revisado 2026-07-10**: la primera versión tenía un
    botón X en el panel — el usuario no lo veía en la pantalla real (motivo
    exacto sin confirmar, pudo ser de tamaño/contraste) y pidió sacarlo del
    código directamente, reemplazándolo por "tocar cualquier lado de afuera
    del modal para cerrarlo". Se eliminó `ui_DeviceCloseBtn` por completo y
    se agregó un `LV_EVENT_CLICKED` directamente sobre `ui_DeviceModal` (el
    backdrop) llamando a la misma `closeDevicesFn` — funciona sin flags de
    bubble porque LVGL no propaga eventos de hijos (el panel/la lista) al
    padre por defecto, así que un toque solo llega al backdrop cuando es
    genuinamente *fuera* del panel.
  - `device_screen.c`/`.h` (nuevo, `main/`): espejo casi exacto de
    `playlist_screen.c` — `device_task` dedicada (espera
    `xTaskNotifyGive`, llama `spotify_available_devices()`, llena la
    lista marcando el dispositivo activo con `LV_SYMBOL_OK` vs
    `LV_SYMBOL_BLUETOOTH`, espera selección por `device_selection_queue`,
    llama `spotify_transfer_playback()`, oculta el modal en vez de
    navegar). A diferencia de `playlist_task` (que ignora el
    `status_code` de `spotify_play_context_uri`), acá si logueo el fallo
    con `ESP_LOGW` — mismo estándar que 1.17, no quería repetir ese
    silencio en código nuevo.
  - Nuevos globals en `app_globals.h`/`main.c`: `device_task_handle`,
    `device_selection_queue`. Nuevo `device_screen_init()` llamado desde
    `main.c` junto a `playlist_screen_init()`.
  Verificado con `idf.py build` completo (incluyó un `idf.py reconfigure`,
  ya que `device_screen.c` es un archivo nuevo y el glob `SRC_DIRS .` de
  CMake no lo recoge hasta reconfigurar — mismo gotcha ya anotado en la
  memoria de sesiones anteriores), sin errores ni warnings nuevos. El
  cambio de cierre (tocar afuera, sin botón X) también verificado con
  `idf.py build`, sin errores ni warnings nuevos. No probado en hardware
  todavía.

- [x] **1.21 `parse_access_token` crasheaba el dispositivo si Discord
  respondía sin `"access_token"`**
  Encontrado con un log real de hardware (2026-07-10), probando el modal
  de dispositivos (1.20). Secuencia completa: token vencido → `GET
  /me/player/devices` da el 401-disfrazado-de-`ESP_ERR_NOT_SUPPORTED` que
  arregla 1.19 → detectado correctamente, dispara
  `get_access_token_locked()` → el endpoint de Discord responde `200 OK`
  pero **sin el campo `"access_token"` en el cuerpo** (motivo exacto sin
  confirmar — no se logueaba el JSON crudo para saberlo; podría ser un
  rate-limit u otro error de Discord con status 200) → `parse_access_token`
  (`parse_objects.c:28`) hacía `ERR_CHECK(json_obj_get_string(&jctx,
  "access_token", ...))`, y como `ERR_CHECK` es `ESP_ERROR_CHECK`, la
  ausencia del campo **abortó el dispositivo entero** (reboot).
  Mismo tipo de bug que 1.5/1.12 (`ERR_CHECK`/`assert` sobre dato externo);
  a este punto puntual no lo cubrió la auditoría de 3.2. Importante: el
  bug no es específico de la pantalla de dispositivos — cualquier flujo
  que dispare un refresh de token (cualquier comando de player, seek,
  volumen, playlists, dispositivos, o el `ENABLE_PLAYER` inicial) estaba
  igual de expuesto; acá se disparó por casualidad vía el modal nuevo.
  Resuelto: 2026-07-10.
  - `parse_access_token` (`parse_objects.c`/`parse_objects.h`) cambió de
    `void` a `esp_err_t`: ya no usa `ERR_CHECK` en ningún punto (ni para
    `json_parse_start_static` ni para el campo `"access_token"`) — ambos
    casos ahora loguean el JSON crudo con `ESP_LOGE` y devuelven
    `ESP_FAIL` en vez de abortar. Confirmado leyendo `json_obj_get_string`
    (`json_parser.c`): si la clave no existe, no toca el buffer de
    salida, así que `access_token.value` queda intacto (el valor anterior,
    o vacío si nunca se obtuvo) — seguro de dejar así.
  - `get_access_token_locked` (`spotify_auth.c`) ahora revisa el
    `esp_err_t` que devuelve `parse_access_token`: si falla, no pisa
    `expiresIn` ni loguea el token como obtenido — deja que
    `access_token_needs_refresh()` pida otro intento la próxima vez, en
    vez de seguir con un token vencido/vacío silenciosamente.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía (haría falta que Discord repita esa
  respuesta rara para confirmarlo en vivo).
  **Hallazgo relacionado, resuelto en la misma sesión**:
  `parse_available_devices()` (el que alimenta el modal de 1.20) tenía el
  mismo patrón de `ERR_CHECK` sobre datos de red (`"devices"`, `"name"`,
  `"id"`) — el usuario pidió arreglarlo también. Cambió de `void` a
  `esp_err_t`, mismo criterio de granularidad que el resto: `ESP_FAIL`
  solo si la respuesta entera es imparseable o falta el array `"devices"`
  (nada rescatable); un dispositivo individual sin `"name"`/`"id"` ahora
  se salta con `continue` (no se aborta la lista completa) en vez de
  `break`, ya que un campo faltante en una sola entrada no implica que el
  resto de la lista esté mal. Se cambió `malloc` por `calloc` al reservar
  cada `DeviceItem_t`: si `json_obj_dup_string` falla para `"name"` o
  `"id"` sin haber tocado el puntero de salida (confirmado en
  `json_parser.c`, mismo comportamiento que ya se había confirmado para
  `parse_access_token`), liberar el campo no completado con `free()` sobre
  un puntero en cero es seguro; con `malloc` habría sido `free()` sobre
  memoria sin inicializar. `spotify_available_devices()`
  (`player_commands.c`) ahora revisa el `esp_err_t` devuelto: si falla,
  libera la lista y devuelve `NULL` (mismo camino que ya existía para un
  fallo de HTTP), en vez de devolver una lista silenciosamente vacía/a
  medio llenar. Verificado con `idf.py build` completo, sin errores ni
  warnings nuevos. No probado en hardware todavía.

- [x] **1.22 `parse_track` filtraba tipos de mensaje del WS por prueba y
  error en vez de mirar `"uri"`**
  El usuario preguntó por un log real (`ESP_LOGE ... "payloads" array
  first element is a string`, disparado por un push de tipo
  `hm://herodotus/uri/spotify:list:play-history:v1/resume-point-revision/...`
  — el servicio interno de Spotify que trackea "dónde quedaste" en una
  lista, sin relación con el estado de reproducción). No era un bug: el
  WS "dealer" multiplexa varios tipos de push no relacionados por la
  misma conexión (herodotus, actualizaciones de playlist, estado de
  broadcast social, etc.) además de los eventos de player/device que sí
  procesamos, y como sus `payloads` tienen formas completamente distintas
  (a veces un string en base64, no un objeto JSON), `parse_track` los
  hacía fallar en cascada por los chequeos de `payloads`/`events`
  (`ESP_LOGE` en cada paso) antes de finalmente descartarlos como
  `UNKNOW`.
  El usuario propuso filtrar por `"type"` al principio para evitar tanto
  chequeo de `payloads`. Al revisar los ejemplos reales de la sesión, el
  `"type"` de nivel superior siempre vale `"message"` en todos los casos
  (no discrimina nada) — el campo que sí distingue es **`"uri"`** de nivel
  superior: `"wss://event"` es el único valor observado que trae los
  eventos que entendemos; cualquier otro (`hm://herodotus/...`,
  `hm://playlist/...`, `social-connect/v2/broadcast_status_update`, etc.)
  es un push de otro tipo.
  Resuelto: 2026-07-10. Se agregó un chequeo de `"uri"` justo después del
  salto de `initial_state` (antes de tocar `"payloads"` en absoluto): si
  no es `"wss://event"`, se descarta como `UNKNOW` con un solo `ESP_LOGD`
  (nivel bajo a propósito, es tráfico esperado, no un error). Los
  `ESP_LOGE` existentes de `payloads`/`events` quedaron intactos en
  severidad, pero ahora solo se disparan ante una anomalía genuina (un
  mensaje que sí declaró `uri == "wss://event"` y aun así vino con una
  forma inesperada), en vez de en cada push rutinario que no era un
  evento. Esto también deja prácticamente sin uso el log puntual que el
  usuario había pedido bajar a warning ("`payloads` array first element is
  a string") — ya no se llega a él para herodotus/playlist, así que no
  hizo falta tocar su severidad por separado.
  **Follow-up mismo día**: el usuario pidió ir más lejos y sacar toda la
  lógica de diagnóstico fino sobre `"payloads"` — no veía utilidad en
  distinguir "falta el array" / "está vacío" / "tiene más de un elemento"
  / "el primer elemento no es objeto ni string" como 4 ramas separadas,
  ya que las 4 llevan al mismo resultado (`UNKNOW`). Se colapsó todo el
  bloque a un solo chequeo combinado
  (`json_obj_get_array(&jctx, "payloads", ...) != OS_SUCCESS ||
  json_arr_get_object(&jctx, 0) != OS_SUCCESS`) con un único `ESP_LOGE`
  (se mantiene en ese nivel, no `ESP_LOGD`, porque llegar hasta acá ya
  implica que `"uri"` dijo ser un evento real — que igual falle es una
  anomalía genuina, a diferencia del tráfico rutinario que el filtro de
  `"uri"` ya descarta antes). El bloque análogo para `"events"` (mismo
  patrón, un nivel más adentro) se dejó como estaba — no fue parte de lo
  pedido en este momento.
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  No probado en hardware todavía.

- [x] **1.23 Evaluación de uso de memoria: estática vs. dinámica (riesgo de
  fragmentación)**
  El usuario pidió una evaluación completa de memoria, con foco explícito
  en dinámica por el riesgo de fragmentación en un dispositivo de uptime
  largo.

  **Estática**: medida con `idf.py size` (DIRAM total 341.760 B, usado
  186.449 B / 54,56%, libre 155.311 B; Flash código 1.028.062 B, datos
  237.488 B) y `python -m esp_idf_size --files build/spotify_app.map`
  (desglose exacto por objeto). Ningún `.c` de `spotify_client` tiene
  bss/data — no hay variables globales/estáticas en el componente, todo
  vive en el struct heap-alocado `esp_spotify_client`. Lo único estático
  de esta sesión (~260 B en total) son los ~23 punteros `lv_obj_t*` de
  `ui.c` (92 B), el `track` file-scope y afines en `player_screen.c`
  (136 B), el timer de debounce en `ui_events.c` (4 B) y los globales de
  `main.c` (28 B). Irrelevante, no es un vector de riesgo.

  **Dinámica — allocaciones de arranque (riesgo ~nulo, ocurren una sola
  vez antes de que el heap pueda fragmentarse, y viven para siempre)**:
  - `struct esp_spotify_client` (1 solo `calloc` en `spotify_client_init`,
    `spotify_client.c`): ~20,6 KB, dominado por
    `json_tokens[MAX_TOKENS=1000]` (`jsmntok_t` pesa 20 B porque
    `json_parser.h` define `JSMN_PARENT_LINKS`: 4 `int` + `parent` =
    20.000 B) + `access_token.value[400]` + `sprintf_buf[100]`.
  - Buffer HTTP: `MAX_HTTP_BUFFER` = 8.192 B.
  - Buffer WS: `MAX_WS_BUFFER` = 4.096 B.
  - `track_info` (struct `TrackInfo`, chico).
  - `pixels` (portada, PSRAM): `heap_caps_calloc` de 45.000 B en
    `player_screen.c`, una sola vez al crear la pantalla.
  Total ~78 KB, todo pedido una vez cerca del boot y nunca vuelto a pedir
  ni liberar. Cero riesgo de fragmentación.

  **Dinámica — allocaciones recurrentes (el riesgo real: tamaños que
  varían, alloc/free intercalados indefinidamente durante el uptime)**:
  - Buffer de portada del álbum: `heap_caps_malloc(90.000 B, PSRAM)` en
    `player_screen.c` (pedido y liberado en cada `NEW_TRACK`, justo
    alrededor de `fetch_album_art`+`decode_image`). Es el bloque
    recurrente más grande de la app, pero al ser PSRAM y de tamaño fijo
    fragmenta menos de lo que su tamaño sugeriría.
  - JPEG work buffer (`decode_image.c`, `JPEG_WORK_BUF_SIZE=3100`,
    PSRAM): alloc+free en cada decodificación de portada, anidado en el
    punto anterior. Tamaño fijo, bajo riesgo. Se confirmó que se libera
    (`free(workbuf)` antes de retornar) — no hay leak.
  - Strings de metadata por track (`spotify_clone_track`,
    `player_commands.c`): `strdup`/`dup_or_null` de `name`,
    `album.name`, `album.url_cover`, más un nodo `malloc` + string
    `strdup`'d por cada artista en la lista `artists`. Tamaño
    **variable** según el título/artista/URL — este es el patrón clásico
    de fragmentación de heap (bloques chicos, tamaño variable, alloc/free
    intercalado para siempre). Se liberan correctamente vía
    `free_track()`.
  - `join_artist_names` (`player_screen.c`): `malloc(total_len+1)` para
    el string combinado de artistas, liberado inmediatamente después de
    usarlo.
  - Listas de playlists/dispositivos (`spotify_user_playlists`,
    `spotify_available_devices`): `calloc` de `List` + N nodos, bursty
    pero acotado (playlists hasta 50 ítems, devices 1-4), solo al abrir
    esas pantallas/modal. Se confirmó que `spotify_free_nodes()` libera
    correctamente los 3 tipos de nodo (`STRING_LIST`/`PLAYLIST_LIST`/
    `DEVICE_LIST`) sin leaks.
  - `http_utils_join_string` (interno de ESP-IDF, usado en
    `player_task.c` para la URL del WS y la de notificaciones): ambos
    call sites liberan el resultado correctamente (`free(uri)`/
    `free(url)`), y solo ocurre en (re)conexión del WS — infrecuente,
    bajo riesgo.

  **Conclusión**: no se encontraron leaks. El único patrón a vigilar a
  largo plazo es el de strings de tamaño variable por cambio de track
  (nombre/artista/álbum/URL de portada) — es el que clásicamente
  fragmenta un heap embebido corriendo semanas/meses seguidos. El buffer
  de portada (90 KB) es grande pero de tamaño fijo y en PSRAM, así que
  fragmenta menos de lo que parece a simple vista. No se hicieron
  cambios de código — es una evaluación, no un fix.

- [x] **1.24 Conectividad a internet: credenciales hardcodeadas en build
  time y crash del dispositivo si la conexión falla (fuera de
  `spotify_client`, en `main/main.c` y `components/protocol_examples_common`)**
  El usuario pidió verificar y documentar cómo la app accede a internet.
  Hallazgo:
  - `main/main.c` llama `ESP_ERROR_CHECK(example_connect())`.
    `example_connect()` (`components/protocol_examples_common/connect.c`,
    componente estándar de ejemplos de ESP-IDF, no específico de esta
    app) delega en `example_wifi_connect()`
    (`components/protocol_examples_common/wifi_connect.c`), que arma la
    config de la STA a partir de dos `#define` de Kconfig:
    `CONFIG_EXAMPLE_WIFI_SSID`/`CONFIG_EXAMPLE_WIFI_PASSWORD`. Estos
    valores se fijan en **build time** vía `idf.py menuconfig` (menú
    "Example Connection Configuration") y quedan grabados en el archivo
    `sdkconfig` del árbol de build — **no** en `sdkconfig.defaults`
    (que sí está versionado). `sdkconfig` está en `.gitignore`, así que
    no hay fuga de credenciales al repositorio, pero **cambiar de red
    requiere recompilar y reflashear** el firmware, no hay forma de
    hacerlo desde el dispositivo en uso.
  - `example_wifi_connect()` llama expresamente
    `esp_wifi_set_storage(WIFI_STORAGE_RAM)` (no `WIFI_STORAGE_FLASH`) —
    o sea que ni siquiera se apoya en la persistencia propia del driver
    Wi-Fi de ESP-IDF; cada boot vuelve a aplicar el SSID/password
    horneado en el firmware desde cero. No hay ningún uso de NVS para
    credenciales en el proyecto (`nvs_flash_init()` se llama en
    `main.c` pero nada lee/escribe claves ahí todavía).
  - **Bug real**: `example_handler_on_wifi_disconnect()`
    (`wifi_connect.c`) reintenta hasta `CONFIG_EXAMPLE_WIFI_CONN_MAX_RETRY`
    veces (default 6) y después desiste, liberando el semáforo que
    `example_wifi_sta_do_connect()` espera — en ese momento
    `example_connect()` devuelve `ESP_FAIL`. Como en `main.c` esa
    llamada está envuelta en `ESP_ERROR_CHECK`, una password mal escrita,
    un AP fuera de rango, o simplemente un router apagado en el momento
    del boot **abortan el dispositivo** — mismo patrón de bug que 1.19/
    1.21/1.5 (fallo externo/de red tratado como condición fatal en vez
    de recuperable), esta vez en el arranque más temprano de la app, antes
    de que exista ninguna UI de error. Con un router que tarda en
    levantar tras un corte de luz, esto puede producir un bucle de
    reinicios indefinido hasta que la red vuelva.
  - Dato de contexto para el diseño de una alternativa (ver 3.8): en
    `app_main()` (`main.c`), `bsp_display_start()`+`ui_init()` **ya
    corren antes** de `example_connect()` — la pantalla y todos los
    widgets (incluida cualquier pantalla nueva que se agregue) ya están
    disponibles en ese punto del arranque, así que no hay problema de
    orden de inicialización para mostrar una UI de configuración de red
    antes de tener conectividad.
  **Resuelto: 2026-07-09 (ver implementación completa en 3.8)** — el bug
  de `ESP_ERROR_CHECK(example_connect())` ya no existe: `main.c` no llama
  más a `example_connect()`/`protocol_examples_common`, así que un
  fallo de conexión ya no puede abortar el dispositivo.

- [x] **1.25 Caracteres especiales de español (ñ, tildes, ¿¡) no
  renderizaban en la UI**
  El usuario reportó que la ñ y las vocales acentuadas "rompen" en
  pantalla (nombres de tracks/artistas/playlists con acentos, textos de
  la propia UI).

  **Causa raíz**: las fuentes bitmap que trae LVGL (`lv_font_montserrat_14`
  — la fuente default del tema — y `lv_font_montserrat_20`, usada
  explícitamente para títulos/íconos) solo incluyen glifos ASCII
  (`0x20-0x7E`) más un puñado de símbolos extra (grado, viñeta, y los
  íconos `LV_SYMBOL_*`, que en realidad viven en el rango Unicode privado
  `0xF000+` de FontAwesome). **No incluyen para nada el bloque Latin-1
  Supplement (`0xA0-0xFF`)**, donde viven Ñ/ñ, Á/á, É/é, Í/í, Ó/ó, Ú/ú,
  Ü/ü, ¿, ¡ — confirmado leyendo directamente el `cmap` de
  `lv_font_montserrat_20.c` (`managed_components/lvgl__lvgl/src/font/`).
  No es un bug de decodificación UTF-8 (`CONFIG_LV_TXT_ENC_UTF8=y` está
  bien seteado, confirmado en `sdkconfig`) — el texto se decodifica bien,
  simplemente la fuente no tiene el glifo para esos codepoints.

  **Solución**: generar fuentes propias con el mismo Montserrat que usa
  LVGL pero con el rango extendido a `0x20-0x7F,0xA0-0xFF,0x2022`
  (ASCII + Latin-1 Supplement completo + viñeta), preservando los mismos
  íconos `LV_SYMBOL_*` que ya usa toda la UI. Sin `node`/`npm`
  instalados en el entorno y sin sudo passwordless disponible, se bajó
  un tarball portátil de Node.js a un directorio temporal (sin
  necesidad de privilegios de sistema) y se corrió `lv_font_conv`
  (exactamente la misma herramienta que
  `managed_components/lvgl__lvgl/scripts/built_in_font/built_in_font_gen.py`
  usa para generar las fuentes built-in de LVGL) contra las mismas
  fuentes fuente que LVGL: `Montserrat-Medium.ttf` +
  `FontAwesome5-Solid+Brands+Regular.woff` (ambas ya vendorizadas ahí
  mismo, con licencias abiertas — SIL OFL / Font Awesome Free — mismos
  archivos de licencia que ya trae el propio LVGL).
  - Nuevos: `main/ui/fonts/lv_font_es_14.c`, `lv_font_es_20.c`
    (`const lv_font_t lv_font_es_14`/`lv_font_es_20`), y
    `main/ui/fonts/lv_font_es.h` (declaraciones + el comando exacto de
    regeneración documentado en el propio header, por si hace falta
    ajustar el rango a futuro).
  - `main/ui/ui.h` incluye `fonts/lv_font_es.h`, así que está disponible
    en todos los archivos de pantalla sin tocar cada uno por separado.
  - `ui_init()` (`ui.c`): `lv_theme_default_init(..., &lv_font_es_14)` en
    vez de `LV_FONT_DEFAULT` — como el tema es la fuente por defecto que
    hereda cualquier label sin fuente explícita, este único cambio
    corrige todo el texto que no seteaba `&lv_font_montserrat_20` a mano
    (nombre de artista, tiempos, listas de playlists/devices/tracks,
    labels de estado, etc.) sin tocar cada sitio.
  - Las 8 referencias explícitas a `&lv_font_montserrat_20` (títulos,
    íconos de transporte/nav) reemplazadas por `&lv_font_es_20`.
  - Se corrigieron dos strings de la propia UI que se habían escrito sin
    tilde/ñ como workaround al momento de escribirlos ("Buscar
    cancion..." → "Buscar canción...", "Contrasena de red..." →
    "Contraseña de red...", `ui_SearchScreen.c`/`ui_WifiScreen.c`).
  **Nota/posible ajuste visual pendiente de confirmar en hardware**: las
  fuentes regeneradas tienen `line_height`/`base_line` levemente mayores
  que las built-in originales (14px: 16→18, base_line igual; 20px:
  22→25, base_line 4→5) — esperable, ya que los acentos de letras como
  á/é necesitan más alto por encima de la x-height que el ASCII puro.
  Los layouts de esta app usan bastante `LV_SIZE_CONTENT` y tienen algo
  de margen entre elementos (~8-16px, ver la discusión de layout en
  3.7), así que no debería notarse, pero no se verificó visualmente en
  pantalla real — a confirmar.
  Verificado con `idf.py reconfigure` (archivos nuevos) + `idf.py build`
  completo, sin errores ni warnings nuevos (el binario creció ~37KB de
  flash por los glifos extra, sin problema de espacio — 32% libre).
  No probado en hardware todavía.
  Fuera de alcance de este fix (típeo, no visualización): el teclado en
  pantalla (`lv_keyboard`, usado en `ui_SearchScreen.c`/`ui_WifiScreen.c`)
  sigue siendo QWERTY sin tecla de ñ/acentos — no se puede *escribir*
  una ñ desde el teclado en pantalla todavía, aunque ahora si se recibe
  una (p.ej. en el nombre de un track ya guardado) se muestra bien. No
  pedido, no se tocó.
  **Follow-up mismo día**: confirmado con el usuario que
  `lv_font_montserrat_20`/`_42` (Kconfig `LV_FONT_MONTSERRAT_20`/`_42`)
  ya no se usan en ningún lado de la app tras este fix (ni como símbolo
  explícito en código propio, ni como dependencia interna de LVGL — a
  diferencia de `lv_font_montserrat_14`, que **sí** debe seguir
  compilada: LVGL usa `LV_FONT_DEFAULT` como fallback en varios puntos
  internos de la librería —`lv_draw_label.c`, `lv_style.c`, `lv_font.c`,
  `lv_theme.c`, etc.— sin importar que nuestra app ya no lo referencie
  directamente, y `CONFIG_LV_FONT_DEFAULT_MONTSERRAT_14=y` fuerza esa
  dependencia vía `select` en el Kconfig). Se desactivaron
  `CONFIG_LV_FONT_MONTSERRAT_20`/`_42` en `sdkconfig` (editado
  directamente, no vía `menuconfig` interactivo — ninguna de las dos
  opciones tiene `default y` en el Kconfig de LVGL, así que un checkout
  nuevo desde `sdkconfig.defaults` ya las tendría apagadas; no hizo
  falta tocar ese archivo).
  Verificado con `idf.py reconfigure` + `idf.py build` completo, sin
  errores ni warnings nuevos, y con `xtensa-esp32s3-elf-nm` sobre el
  `.elf` resultante: `lv_font_montserrat_20`/`_42` ya no aparecen como
  símbolos, `lv_font_es_14`/`_20` sí. **Dato honesto**: el tamaño del
  binario no cambió (0x15b770 B, idéntico antes/después) — ESP-IDF
  linkea con `--gc-sections`, así que el linker ya venía descartando esos
  datos no referenciados del binario final desde que se dejó de usar
  `&lv_font_montserrat_20` en código (paso anterior de este mismo ítem);
  el ahorro real de flash ya había ocurrido ahí. Este cambio es
  prolijidad de build (menos objetos muertos compilados, menos confusión
  en el menuconfig), no una reducción adicional de tamaño.
  **Follow-up mismo día**: el usuario notó otro bloque de configuración
  en desuso en `sdkconfig` (líneas 981-992, `"Example Configuration"` /
  `CONFIG_EXAMPLE_DISPLAY_ROTATION_*`). A diferencia de los fonts, este
  no era un ajuste de Kconfig "sin efecto" sino uno directamente
  **huérfano**: definido en `main/Kconfig.projbuild` (todo el archivo, 15
  líneas, era exactamente este único menú) pero `EXAMPLE_DISPLAY_ROTATION`
  no aparece referenciado en ningún `.c`/`.h` de todo el árbol
  (`main/`, `components/`, `managed_components/`) — `main.c` ya hardcodea
  `bool landscape = true;` al llamar `bsp_display_start()`, sin leer este
  valor en absoluto. Es resabio de algún template/ejemplo con el que se
  bootstrapeó el proyecto originalmente (mismo patrón que
  `protocol_examples_common`, ver 1.24/3.8). Como `sdkconfig` es un
  archivo generado (la fuente real es el `Kconfig.projbuild`), borrar
  solo esas líneas de `sdkconfig` no alcanza — Kconfig las vuelve a
  materializar en cualquier `reconfigure` mientras la opción siga
  existiendo. Se borró `main/Kconfig.projbuild` entero (`git rm`).
  Verificado con `idf.py reconfigure` (confirmado que el bloque
  `"Example Configuration"`/`EXAMPLE_DISPLAY_ROTATION_*` ya no aparece en
  `sdkconfig` regenerado) + `idf.py build` completo, sin errores ni
  warnings nuevos, mismo tamaño de binario (no compilaba código real).

## 2. Sugerencias de refactoring

- [x] **2.1** Extraer el patrón repetido *lock → set callback → prepare_client
  → `retry:` → perform → chequeo status/reintentos → close → release lock*,
  duplicado ~7 veces (`spotify_play_context_uri`, `spotify_user_playlists`,
  `spotify_available_devices`, `confirm_ws_session`, `get_access_token`,
  `player_cmd`, `fetch_album_art`). Centralizarlo permite fijar el manejo de
  401/expiración (1.7) en un solo lugar.
  Resuelto: 2026-07-06. Se agregó `perform_http_request()` (nueva función
  privada) que concentra `prepare_client` + el bucle de reintentos + el
  `close`, y las 7 funciones la usan ahora en vez de duplicar el bloque.
  De paso se corrigieron dos bugs reales encontrados al deduplicar: (a)
  `spotify_play_context_uri` tenía un `HttpStatus_Code s_code` interno que
  *shadoweaba* al externo, por lo que el `status_code` de salida quedaba
  siempre en 0 aunque la petición fuera exitosa; (b) `confirm_ws_session`
  perdía (`leak`) `conn_id`/`url` si se agotaban los reintentos de conexión,
  ya que antes solo se liberaban en la rama de éxito.
- [x] **2.2** Reemplazar `goto retry` por bucle acotado (`for`/`do-while` con
  contador máximo) — resuelve de raíz 1.1.
  Resuelto: 2026-07-06, como parte de 2.1: `perform_http_request()` usa un
  `do { ... } while (http_retries_available(...))`. El caso especial de
  `player_cmd` (swap play/pause ante 403) ya no usa `goto` en absoluto: al
  eliminar el salto hacia atrás, la doble llamada explícita a
  `perform_http_request()` no puede volver a entrar en bucle, así que la
  bandera `toggled` que se había agregado para el fix de 1.1 se volvió
  innecesaria y se quitó.
- [x] **2.3** Desacoplar `PlayerCommand_t` de los bits del `EventGroup`
  (`spotify_client.c:34-40`, hoy `PAUSE = DO_PAUSE` etc.). Traducir bit→comando
  explícitamente en vez de depender de la coincidencia numérica.
  Resuelto: 2026-07-06. `PlayerCommand_t` ahora tiene valores propios (1..N,
  sin relación con `DO_*`), y se agregó `bits_to_player_cmd()` que traduce
  explícitamente el bit del EventGroup recibido en `player_task` al comando
  correspondiente (o rechaza el bit si no mapea a ningún comando).
- [x] **2.4** Mover los buffers `static` (tokens JSON, `output_len`, `in_items`,
  `brace_count`) al struct del cliente para que queden protegidos por el
  mutex existente (resuelve 1.2).
  Resuelto: 2026-07-06. El buffer `tokens[MAX_TOKENS]` de `parse_objects.c`
  pasó de `static` global a un campo `json_tokens` dentro de
  `struct esp_spotify_client`; las 5 funciones `parse_*` ahora reciben el
  buffer como parámetro (`parse_objects.h`/`.c`), y `evt_user_data_t` ganó
  un campo `tokens` (`void*`) para el único call site que no tiene acceso
  directo al `client` (`playlist_http_event_cb`). Los contadores
  `output_len`/`in_items`/`brace_count`/`item_overflow` de
  `handler_callbacks.c` se movieron de `static` de función a campos de
  `evt_user_data_t` por el mismo motivo. Con esto ya no queda ningún estado
  global mutable en el componente: dos instancias de `esp_spotify_client`
  (hoy no usado, pero ya no descartado por diseño) no se pisarían entre sí.
  El fix de locking de 1.2 (tomar `http_buf_lock` alrededor de
  `parse_connection_id`/`parse_track` en `player_task`) se mantiene sin
  cambios y sigue siendo necesario: sigue habiendo una única instancia de
  cliente compartida entre tareas, así que dos tareas accediendo al mismo
  `client->json_tokens` todavía necesitan serializarse.
- [x] **2.5** Unificar la política de errores: red/parsing → evento por la
  cola existente; `assert`/`ESP_ERROR_CHECK` solo para errores de programador.
  Resuelto: 2026-07-06. Se agregó `PLAYER_ERROR` a `Event_t`
  (`spotify_client.h`) y se resolvieron los 3 `// TODO: send error to queue`
  que quedaban en `player_task`, emitiendo ese evento en vez de no hacer
  nada. De paso se corrigió un bug real: la rama de error al pedir el
  estado del player durante `ENABLE_PLAYER` hacía `break;` dentro de un
  `while(1)` sin switch envolvente, lo cual terminaba la función
  `player_task` — es decir, **la tarea de FreeRTOS moría silenciosamente y
  nunca más procesaba eventos**, dejando el cliente completamente muerto
  hasta un reinicio del dispositivo. Ahora esa rama loguea, emite
  `PLAYER_ERROR`, deshabilita el player y hace `continue` para mantener la
  tarea viva. `main.c` no necesitó cambios: su `switch` sobre `event.type`
  ya tiene un `default:` que descarta eventos no manejados explícitamente.
  Nota: no se tocaron los `ERR_CHECK`/`assert` internos de `parse_track` en
  `parse_objects.c` (el parseo del JSON de Spotify en sí) — esos son el
  contenido de 1.4 (esquema de episodios no soportado), que sigue pendiente
  y es un cambio de mayor alcance.
- [x] **2.6** Dividir `spotify_client.c` (920 líneas) por responsabilidad:
  autenticación, comandos de player, bootstrap de sesión WebSocket.
  Resuelto: 2026-07-07. `spotify_client.c` quedó como núcleo (lifecycle
  `spotify_client_init`/`deinit`, `player_dispatch_event`/`spotify_wait_event`,
  y la infraestructura HTTP compartida: `perform_http_request`,
  `prepare_client`, `http_retries_available`, `debug_mem`,
  `http_event_cb_wrapper`), y se crearon tres archivos nuevos:
  - `spotify_auth.c`: `get_access_token`/`get_access_token_locked`/
    `access_token_needs_refresh`, `ACCESS_TOKEN_URL`/`TOKEN_URL`.
  - `player_commands.c`: `PlayerCommand_t`, `player_cmd`,
    `bits_to_player_cmd`, `spotify_play_context_uri`,
    `spotify_user_playlists`, `spotify_available_devices`,
    `fetch_album_art`, `spotify_clear_track`, `spotify_clone_track` y sus
    helpers privados `free_track`/`dup_or_null`.
  - `player_task.c`: el propio `player_task` (máquina de estados) y
    `confirm_ws_session` (bootstrap de la sesión WebSocket, solo se llama
    desde ahí).
  El struct `esp_spotify_client` y los macros/enums compartidos entre estos
  archivos (`ACQUIRE_LOCK`/`RELEASE_LOCK`, `MAX_HTTP_BUFFER`/`MAX_WS_BUFFER`/
  `SPRINTF_BUF_SIZE`, `PlayerCommand_t`) se movieron a
  `spotify_client_priv.h`, junto con los prototipos de las funciones que
  cruzan de un archivo a otro (antes todas `static` en un único archivo).
  CMake ya usa `SRC_DIRS .`, así que no hizo falta tocar `CMakeLists.txt`
  (aunque sí hace falta `idf.py reconfigure` una vez para que CMake vea los
  archivos nuevos). Verificado con una compilación completa
  (`idf.py build`) sin errores ni warnings nuevos en el componente.
- [x] **2.7** Nombrar los "magic numbers" (`400`, `30`, `100`, `8192`, `1000`,
  `4096`) con constantes documentadas.
  Resuelto: 2026-07-07.
  - `400` (tamaño de `access_token.value`) → `ACCESS_TOKEN_BUF_SIZE`
    (`spotify_client_priv.h`).
  - `30` del `ping_interval_sec` del websocket → `WS_PING_INTERVAL_SEC`; el
    otro `30` (tamaño de `TrackInfo.id`, `spotify_client.h`) →
    `SPOTIFY_ID_BUF_SIZE`, ahora también usado por `parse_track()`
    (`parse_objects.c`) al leer el campo `id`, así ambos no pueden
    desincronizarse.
  - `8192` tenía dos significados distintos que coincidían por casualidad:
    `MAX_HTTP_BUFFER` (ya nombrado) y el stack size de `player_task` en
    `xTaskCreate` → se le dio nombre propio, `PLAYER_TASK_STACK_SIZE`,
    para no atar sin querer una cosa a la otra si alguna cambia.
  - `1000` del `vTaskDelay` entre reintentos HTTP → `HTTP_RETRY_DELAY_MS`
    (el otro `1000`, `MAX_TOKENS`, ya estaba nombrado).
  - `100`/`4096` ya estaban nombrados (`SPRINTF_BUF_SIZE`/`MAX_WS_BUFFER`),
    sin cambios.
  - Extra, no estaba en la lista original pero es la misma categoría de
    problema: el `7` de `strlen("Bearer ")` estaba hardcodeado de forma
    independiente en 4 lugares distintos (`spotify_client.c`,
    `spotify_auth.c` x2, `player_task.c`) para saltear el prefijo
    `"Bearer "` de `access_token.value`. Se agregaron `BEARER_PREFIX`
    (`"Bearer "`) y `BEARER_PREFIX_LEN` (`sizeof(BEARER_PREFIX) - 1`,
    derivado del string en vez de un segundo número hardcodeado) y se
    reemplazaron los 4 usos — si el prefijo cambiara de longitud algún día,
    ahora es imposible que alguno de los 4 sitios quede desincronizado.
  - También, `s_code >= 400` (`player_task.c`) pasó a usar el enum ya
    existente de ESP-IDF `HttpStatus_BadRequest` (`esp_http_client.h`) en
    vez del literal `400`; y el `204` (sin dispositivo reproduciendo, mismo
    archivo) pasó a `HTTP_STATUS_NO_CONTENT` (ESP-IDF no define ese código
    en `HttpStatus_Code`).
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
- [ ] **2.8 Código muerto encontrado en la re-revisión post-2.6/2.7 (2026-07-07)**
  No estaba en el checklist original.
  - `TOKEN_URL` (`spotify_auth.c`, `"https://accounts.spotify.com/api/token"`)
    está definida pero nunca se usa — todo el flujo real pasa por
    `ACCESS_TOKEN_URL` (Discord, ver 1.8). Probable resto de un intento
    anterior de OAuth2 oficial que no se terminó de retirar.
  - `onDevicePlaying()` y `str_append()` (`parse_objects.c`, líneas ~251-310)
    son funciones enteras comentadas (`/* ... */`), ~55 líneas inertes. El
    docstring de `str_append` menciona explícitamente "u8g2 selection list
    menu", una librería de UI que no es la que usa esta app hoy (LVGL) — son
    restos de una versión anterior del proyecto.
  Ninguno de los tres afecta el comportamiento (código muerto, no se
  compila o no se llama), así que es solo limpieza de legibilidad: borrarlos
  o, si se prefiere conservar `onDevicePlaying`/`str_append` como referencia
  histórica, dejar una nota explícita de por qué siguen ahí.

- [x] **2.9 `Event_t` (`spotify_client.h`) recortado: 7 de 12 valores eran
  peso muerto**
  A pedido del usuario ("simplificá `Event_t`, decime si todavía tiene
  utilidad"), 2026-07-09. Grep de cada valor en todo el árbol (`components/`
  + `main/`, excluyendo `.md`) para separar lo realmente usado de lo que
  solo estaba declarado:
  - **Vivos** (se emiten *y* se consumen en algún lado): `SAME_TRACK`,
    `NEW_TRACK`, `NO_PLAYER_ACTIVE`, `PLAYER_ERROR`, `UNKNOW`. Se mantienen
    sin cambios.
  - **Completamente muertos** (cero referencias fuera de su propia
    declaración — nunca asignados a `spotify_evt.type`, nunca chequeados en
    ningún `switch`/`if`): `ACTIVE_DEVICES_FOUND`, `NO_ACTIVE_DEVICES`,
    `LAST_DEVICE_FAILED`, `VOLUME_CHANGED` (la duda que disparó esto, ver
    hilo de la mejora de volumen en 1.14), `TRANSFERRED_OK`,
    `TRANSFERRED_FAIL`. Parecen resabios de un diseño previo donde listado
    de dispositivos/transferencia de reproducción iban a pasar por este
    mismo enum de eventos; hoy `spotify_available_devices()` es una función
    síncrona aparte y no existe código de transferencia en absoluto.
    Eliminados.
  - **Caso intermedio**: `DEVICE_STATE_CHANGED` (ya tenía un
    `// <todo: delete later` en el propio código desde 2022). Sí se emite
    (`parse_track()`, cuando Spotify manda un WS de tipo
    `"DEVICE_STATE_CHANGED"`), pero nada lo consume — cae en el mismo
    `default:` que `UNKNOW` en todos los consumidores existentes. Como hoy
    es indistinguible de `UNKNOW` en la práctica (mismo efecto observable),
    se decidió también eliminarlo: `parse_track()` ahora le asigna
    `UNKNOW` en ese caso, conservando el `ESP_LOGW` que ya loguea el
    payload completo (esa es la única traza que aportaba el valor
    separado).
  Verificado con `idf.py build` completo, sin errores ni warnings nuevos.
  El archivo de ejemplo (`components/spotify_client/examples/main/
  spotify_client_example.c`, no compilado como parte del build normal) no
  referenciaba ninguno de los valores eliminados, así que no necesitó
  cambios.
  **Rename 2026-07-09 (mismo hilo)**: el usuario notó que el nombre
  `Event_t` no reflejaba que es específicamente el enum de eventos de
  *playback* (ya no de dispositivos/volumen/transferencia, tras el recorte
  de arriba), y sugirió renombrarlo para que el acoplamiento quedara
  explícito en el nombre. Renombrado a **`PlayerEvent_t`**, formando pareja
  simétrica con `PlayerCommand_t` (`spotify_client_priv.h`: comandos que
  *entran* al player) y consistente con el resto de la nomenclatura
  `player_*` del componente. Los valores del enum no cambiaron, solo el
  nombre del tipo (2 sitios en código: su declaración y el campo `type` de
  `SpotifyEvent_t`, ambos en `spotify_client.h`) y un comentario en
  `parse_objects.c` que lo mencionaba. Verificado con `idf.py build`, sin
  errores ni warnings nuevos.
  **Follow-up inmediato**: el usuario también pidió renombrar el campo
  `type` de `SpotifyEvent_t` a **`player_event`**, para que el nombre del
  campo combine con el del tipo (`spotify_evt.player_event`, no
  `spotify_evt.type`). Cambiado en los 2 sitios que quedaban en
  `spotify_client.h` (declaración del struct + el comentario de arriba que
  lo mencionaba) y en todos los call sites: 9 asignaciones + 2 usos en
  `ESP_LOGI` en `player_task.c`, 11 asignaciones + el inicializador de
  struct en `parse_objects.c`, 5 usos en `player_screen.c`, y 2 en el
  archivo de ejemplo (no compilado, actualizado solo por consistencia).
  Verificado con `idf.py build`, sin errores ni warnings nuevos.

## 3. Cuestiones estratégicas

- [~] **3.1** ~~Reemplazar la autenticación vía Discord por OAuth2 oficial de
  Spotify~~. **Descartado (2026-07-06)**: ver decisión en 1.8 — se prioriza
  el acceso al WebSocket de eventos en tiempo real (`dealer.spotify.com`),
  que el flujo OAuth2 público no otorga. Se mantiene como advertencia
  documentada, no como tarea pendiente.
- [x] **3.2** Adoptar "nunca crashear por errores de red/API" como principio
  de diseño (afecta 1.3 y 1.5).
  Resuelto: 2026-07-06. Tras los fixes de 1.3/1.5/1.6/2.5, se auditó todo el
  componente buscando `ESP_ERROR_CHECK`/`assert` restantes
  (`grep -n "ESP_ERROR_CHECK\|assert("`). Lo único que queda son: (a) el
  `assert(track && *track)` de `parse_track` — una precondición legítima de
  contrato interno (el llamador nunca debe pasar un puntero nulo), no un dato
  externo, así que se deja tal cual; (b) la cadena de `ERR_CHECK` dentro de
  `parse_track` que asume el esquema de "track" (sin `artists`/`album` en
  episodios) — eso es exactamente 1.4, deliberadamente fuera de este batch.
  Con esas dos excepciones documentadas, el principio ya se aplica de forma
  consistente en todo el resto del componente.
- [x] **3.3** Decidir soporte real de podcasts/episodios: implementar el
  esquema `episode` o retirar `additional_types=episode` hasta soportarlo (1.4).
  Resuelto (parcialmente, ver nota): 2026-07-06. Se optó por retirar
  `additional_types=episode` de `PLAYER_STATE` (`spotify_client.c`) en vez de
  implementar el esquema completo (eso sigue siendo 1.4, de mayor alcance,
  explícitamente no incluido en este batch). **Importante — alcance real de
  este fix**: `additional_types` solo afecta la respuesta del endpoint REST
  `/me/player` (usado por `GET_STATE`, el fetch de estado inicial al hacer
  `ENABLE_PLAYER`). El WebSocket `dealer.spotify.com` (eventos en tiempo
  real, ver 1.8) **no depende de este parámetro**: si el usuario reproduce un
  episodio en medio de una sesión, Spotify va a empujar igual un evento
  `PLAYER_STATE_CHANGED` con un `item` de esquema "episode", y `parse_track`
  va a crashear igual por esa vía. Este cambio solo elimina el disparador vía
  el fetch inicial, no el de la vía WebSocket (la más probable en uso real).
  El riesgo de fondo permanece abierto bajo 1.4.
- [x] **3.4** Documentar/mitigar el caso de cuentas Spotify Free (relacionado
  con 1.1): fallar rápido con mensaje claro en vez de colgarse.
  Resuelto: 2026-07-06. El "fallar rápido" ya estaba cubierto por el fix de
  1.1 (ya no se cuelga). Lo que faltaba era hacerlo visible: en
  `player_task`, la rama de comandos de player ahora trata `s_code >= 400`
  (no solo `err != ESP_OK`) como fallo — antes un 403 Forbidden persistente
  (cuenta Free intentando play/pause) devolvía `ESP_OK` igual (la petición
  HTTP en sí funcionó) y no se notificaba nada. Ahora se emite `PLAYER_ERROR`
  con `error_code = 403` en ese caso. Falta el paso final de mapear ese
  código a un mensaje de UI tipo "Se requiere Spotify Premium" — eso vive en
  `main.c`/`ui.c` y no se tocó (fuera del alcance de este componente).
- [x] **3.5** Mejorar observabilidad: agregar un evento genérico de error
  (código HTTP, subsistema) a la cola de eventos hacia la UI.
  Resuelto: 2026-07-06, ampliando el `PLAYER_ERROR` agregado en 2.5.
  `SpotifyEvent_t` (`spotify_client.h`) ganó un campo `error_code` (código
  HTTP si aplica, o 0 para fallos no-HTTP como no poder arrancar el
  WebSocket). Además de los 3 sitios de 2.5, se agregó emisión de
  `PLAYER_ERROR` a otros 3 puntos que deshabilitaban el player en silencio:
  fallo de `get_access_token` al habilitar, `parse_connection_id` sin
  resultado, y `confirm_ws_session` fallido — los 3 en `player_task`. Con
  esto, cualquier camino que deje al player deshabilitado ahora se reporta
  por la cola de eventos. `main.c` sigue sin cambios (su `default:` ya
  ignora eventos no manejados explícitamente); usar `error_code` para
  mostrar algo en la UI queda como trabajo futuro de la capa de UI.
- [ ] **3.6** Testing: extraer `parse_objects.c` detrás de una interfaz
  testeable en host, sin dependencia de hardware, y cubrir con tests
  unitarios los casos límite ya señalados por logs en el código (arrays
  vacíos, claves faltantes, episodios).

- [x] **3.7 Funcionalidad de búsqueda**
  Análisis de viabilidad pedido por el usuario 2026-07-11. **Viable, sin
  bloqueos arquitectónicos**, siguiendo los mismos patrones ya probados en
  el código:
  - Screen + task + queue dedicados (mismo molde que `playlist_screen.c`/
    `device_screen.c`).
  - `List`/`Node` ya soporta agregar un `NodeType_t` nuevo (ej.
    `TRACK_LIST`) sin tocar el resto de `spotify_utils.c`.
  - Parseo JSON: dos estrategias ya probadas en el código (bufferizada,
    `json_http_event_cb`, para devices; "streaming" objeto-por-objeto sin
    necesitar todo el payload en memoria, `playlist_http_event_cb`,
    endurecida en 1.11) — search puede apoyarse en la segunda si hace
    falta.
  - LVGL ya trae `lv_keyboard` + `lv_textarea` vendorizados, sin
    dependencias nuevas.
  **Piezas nuevas a construir** (nada existe hoy):
  - `spotify_search_tracks()` (`player_commands.c`), mismo esqueleto que
    `spotify_user_playlists()`, contra `GET /v1/search?q=...&type=track`.
  - `spotify_play_track_uri()`: `spotify_play_context_uri()`
    (`player_commands.c:36`) solo sirve para reproducir un *contexto*
    (`{"context_uri":"..."}`, playlists/álbumes) — reproducir una canción
    suelta de los resultados necesita `{"uris":["spotify:track:..."]}`,
    un endpoint/cuerpo distinto sobre el mismo `PUT /me/player/play`.
  - Helper de percent-encoding para la query del usuario: no existe hoy
    ni en `spotify_client` ni en el `http_utils` de ESP-IDF (confirmado
    revisando ambos) — el texto libre que tipee el usuario (espacios,
    tildes, `&`, etc.) necesita esto antes de ir en `?q=...`.
  - Parseo del esquema de resultados de `/v1/search`: más pesado que
    playlists (objetos "track" anidados con `artists[]`/`album{images[]}`,
    mismo nivel que `parse_track()` pero repetido por resultado).
  - Widget de teclado en pantalla: patrón de UI nuevo para esta app (el
    dispositivo es solo táctil, sin teclado físico en el BSP).
  **Riesgo real, con mitigación concreta**: `MAX_HTTP_BUFFER` (8192B) y
  el array de tokens jsmn compartido (1000, ~20KB, ver evaluación de
  memoria en 1.23) están dimensionados para playlists/devices, no para
  varios objetos "track" completos. Mitigación: pasar
  `market=from_token` en la query (Spotify omite el campo
  `available_markets` — un array de hasta ~180 códigos de país que si no
  se pide, infla cada track/artist/album en ~1-2KB cada uno — solo
  cuando se especifica `market`), restringir a `type=track` únicamente,
  `limit` chico (5-8, lo que entra cómodo en una lista táctil de todos
  modos), y usar un buffer + array de tokens **propio de la búsqueda**,
  alocado bajo demanda y liberado al salir de la pantalla (misma
  categoría de "ráfaga acotada" que ya son hoy playlists/devices, ver
  1.23) en vez de agrandar el buffer/tokens compartidos de 8KB/1000 que
  usan todos los demás endpoints.
  **UI**: el teclado necesita bastante alto (~200px de los 320
  disponibles) → pantalla propia (no un modal como devices). El botón de
  entrada sí tiene lugar fácil: la fila de transporte sigue el patrón
  `-70/0/+70/+140` (prev/pause/next/device); un quinto botón en `+210`
  todavía entra en los 480px de ancho sin chocar con `ui_PlaylistsBtn`.
  **Decisión de alcance (2026-07-11)**: primera versión **solo
  canciones** (`type=track`), no álbumes/artistas/playlists combinados
  — más simple (un solo esquema a parsear, reproducción directa de
  track), cubre el caso de uso más común. Multi-tipo queda descartado
  para la v1, no es un TODO abierto salvo que se pida después.

  **Implementado el mismo día, tal como se diseñó arriba**:
  - `spotify_utils.h`/`.c`: `NodeType_t` ganó `TRACK_LIST` y el struct
    `TrackSearchItem_t` (`name`/`uri`/`artists`, este último los nombres
    de `artists[]` ya unidos en un solo string `", "`-separado al
    parsear — una lista anidada por resultado hubiera sido complejidad
    sin uso, a diferencia de `TrackInfo.artists`). `spotify_free_nodes()`
    cubre el caso nuevo.
  - `parse_objects.c`/`.h`: `parse_search_results()`, mismo patrón
    "nunca abortar por dato externo" que `parse_available_devices()`
    (entradas malformadas se saltean, no abortan todo el resultado). A
    diferencia del resto de `parse_*`, toma `max_tokens` como parámetro
    explícito en vez de asumir el `MAX_TOKENS` del componente — el buffer
    de tokens que recibe es propio de la búsqueda, no el compartido.
  - `player_commands.c`: `url_encode()` (percent-encoding RFC 3986,
    no existía nada parecido en el proyecto ni en el `http_utils` de
    ESP-IDF), `spotify_search_tracks()` (mismo esqueleto que
    `spotify_user_playlists()`, con el swap de buffer HTTP usado ya por
    `fetch_album_art()` para apuntar temporalmente a un buffer/array de
    tokens propios y grandes — `SEARCH_HTTP_BUF_SIZE=40KB`,
    `SEARCH_MAX_TOKENS=4000`, `SEARCH_RESULT_LIMIT=6` — en vez de tocar
    los compartidos `MAX_HTTP_BUFFER`/1000 tokens; alocados con
    `heap_caps_malloc` y liberados al final de la llamada, ráfaga
    acotada igual que playlists/devices), y `spotify_play_track_uri()`
    (mismo esqueleto que `spotify_play_context_uri()`, cuerpo
    `{"uris":[...]}` en vez de `{"context_uri":...}`).
  - `spotify_client.h`: declaradas `spotify_search_tracks()` y
    `spotify_play_track_uri()`.
  - `main/ui/screens/ui_SearchScreen.c` (nueva, hand-written como
    `ui_PlaylistScreen.c`): back button, `lv_textarea` (una línea,
    placeholder), `lv_keyboard` (`lv_keyboard_set_textarea`, anclado
    abajo, 190px de alto) y `lv_list` de resultados — lista y teclado
    mutuamente excluyentes (nunca visibles a la vez), sin necesidad de
    una segunda pantalla ni de un flag de estado aparte: el flag
    `LV_OBJ_FLAG_HIDDEN` del teclado ya es la fuente de verdad de en qué
    fase está la pantalla (ver `searchBackFn` abajo).
  - `main/ui/ui.h`/`ui.c`: registro de la pantalla + los 3 dispatchers de
    evento (`ui_event_SearchBtn`/`ui_event_SearchBackBtn`/
    `ui_event_SearchKeyboard`, este último en `LV_EVENT_READY` — el
    "enter"/OK del teclado).
  - `main/ui/ui_events.h`/`.c`: `openSearchFn` (resetea la pantalla a
    "sin buscar todavía" y navega), `searchSubmitFn` (lee el texto,
    `strdup`, oculta teclado/muestra "Buscando...", encola en
    `search_query_queue`), `searchBackFn` (a diferencia de
    `playlistBackFn`/`closeDevicesFn`, `search_task` puede estar
    bloqueado en dos colas distintas según la fase — todavía escribiendo
    vs. resultados ya mostrados — así que decide a cuál mandar el
    sentinel `NULL` mirando si el teclado está oculto).
  - `main/search_screen.c`/`.h` (nueva, mismo molde que
    `playlist_screen.c`): `search_task` con dos puntos de espera
    (`search_query_queue` primero, `search_selection_queue` después,
    solo si de verdad se envió una query) — la única pantalla de las 3
    con esa forma, justamente por el paso intermedio de "escribiendo"
    que playlists/devices no tienen.
  - `main/ui/screens/ui_PlayerScreen.c`: `ui_SearchBtn` en la fila de
    transporte, continuando el espaciado `-70/0/+70/+140` a `+210`.
    Ícono: LVGL no trae una lupa en su fuente de símbolos (revisado
    `lv_symbol_def.h` completo) — se usó `LV_SYMBOL_KEYBOARD` en su
    lugar.
  - `main/app_globals.h`/`main.c`: `search_task_handle`,
    `search_query_queue`, `search_selection_queue`, llamada a
    `search_screen_init()`.
  Verificado con `idf.py reconfigure` (archivos nuevos, ver el gotcha de
  `SRC_DIRS .` ya documentado en este archivo) + `idf.py build` completo,
  sin errores ni warnings nuevos. No probado en hardware todavía —
  particularmente sin confirmar aún: tamaño real de una respuesta de
  `/v1/search` con 6 tracks (¿entra cómodo en 40KB/4000 tokens, o hace
  falta ajustar?), y si el teclado en pantalla responde bien al tacto en
  este panel.
  **Follow-up 2026-07-11, probado en hardware real, funciona.** Dos
  ajustes de UX pedidos por el usuario:
  - **Ícono**: `LV_SYMBOL_KEYBOARD` reemplazado por una lupa dibujada a
    mano (un `lv_obj` circular sin relleno, borde de 2px, + un `lv_line`
    corto en diagonal como mango) — LVGL no trae glifo de lupa en su
    fuente de símbolos (se revisó `lv_symbol_def.h` completo), y agregar
    una fuente de íconos nueva solo para un glifo no se justificaba. Es
    el workaround estándar de LVGL para este caso.
  - **Ubicación**: el botón se sacó de la fila de transporte (`+210`) y
    se agrupó junto a `ui_PlaylistsBtn` en la esquina superior derecha
    (apilado justo debajo, mismo tamaño 36×36 y alineación) — search es
    una acción de descubrimiento de contenido como "ver playlists", no
    una acción de reproducción como prev/pause/next, así que tiene más
    sentido agrupada con la navegación que estirando la fila de
    transporte.
  Verificado con `idf.py build`, sin errores ni warnings nuevos.

- [x] **3.8 Pantalla de configuración Wi-Fi en el dispositivo (reemplaza
  `protocol_examples_common`, ver 1.24)**
  Propuesta de diseño pedida junto con la verificación de 1.24,
  **implementada el mismo día** tras confirmación del usuario.

  **Backend nuevo, `main/wifi_manager.c`/`.h`** (infraestructura de la
  app, no del componente `spotify_client` — no tiene nada que ver con
  Spotify): reemplaza `example_connect()`/`protocol_examples_common` por
  esp_wifi manejado directamente:
  - `wifi_manager_init()`: `esp_netif_create_default_wifi_sta()`,
    `esp_wifi_init()`, registro de handlers de `WIFI_EVENT`/`IP_EVENT`,
    `esp_wifi_start()`. **Diferencia con el diseño propuesto**: se dejó
    `esp_wifi_set_storage(WIFI_STORAGE_RAM)` (no `FLASH`) — la
    persistencia la maneja explícitamente esta misma capa, vía NVS propio
    (namespace `wifi_cfg`, claves `ssid`/`pass`), en vez de apoyarse en el
    estado interno (menos explícito) que guarda el propio driver Wi-Fi con
    `WIFI_STORAGE_FLASH`. Un solo lugar responsable de qué se recuerda
    entre reinicios, más fácil de razonar/limpiar a futuro.
  - `wifi_manager_try_stored_credentials()`: lee `ssid`/`pass` de NVS: si
    no hay nada guardado o falla la lectura, devuelve el error de NVS tal
    cual (`ESP_ERR_NVS_NOT_FOUND` típicamente); si hay, intenta conectar
    (reintentos acotados + timeout de 15s). **Nunca envuelto en
    `ESP_ERROR_CHECK` en el caller** (`main.c`) — un fallo simplemente le
    indica que hay que mostrar la pantalla de configuración, en vez de
    abortar el dispositivo (esto es lo que resuelve 1.24).
  - `wifi_manager_scan(...)`: `esp_wifi_scan_start()` (bloqueante) +
    `esp_wifi_scan_get_ap_records()`, acotado a 20 resultados y ordenado
    por RSSI descendente (`qsort`), volcado a un array propio
    `wifi_ap_info_t` (ssid/rssi/secured) — no el `List` genérico de
    `spotify_client/spotify_utils.h`, deliberadamente: son conceptos no
    relacionados, mejor no acoplar la app-infra de red al tipo de datos
    de un componente que es específicamente sobre Spotify.
  - `wifi_manager_connect(ssid, password)`: setea la config, conecta,
    espera el evento de IP (mismo timeout/reintentos que arriba, factorado
    en un helper privado `connect_and_wait()` compartido con
    `try_stored_credentials()`), y si conecta guarda `ssid`/`password` en
    NVS. `password` puede ser `""` para redes abiertas.

  **UI nueva**, mismo molde que `ui_SearchScreen.c` (3.7) pero **sin task
  propia** (diferencia clave con el diseño original, ver más abajo):
  - `main/ui/screens/ui_WifiScreen.c`: lista de APs (`lv_list`, orden por
    señal), y al tocar una segura, el mismo par `lv_textarea`
    (`lv_textarea_set_password_mode`) + `lv_keyboard` que ya existía para
    buscar canciones — para redes abiertas se conecta directo, sin pedir
    nada. Label de estado único para "Buscando redes...", "Conectando...",
    "No se pudo conectar, reintentando...", etc. A diferencia de todas las
    demás pantallas de esta app, **no tiene botón de "volver" a
    `ui_PlayerScreen`**: la red es mandatoria para que la app haga
    cualquier cosa, así que esta pantalla o conecta o sigue reintentando
    (sí tiene un botón de "atrás" acotado a la sub-vista de contraseña,
    para cancelar y volver a la lista sin intentar conectar).
  - `main/wifi_screen.c`/`.h`: **no un task+cola como playlist/device/
    search** — `wifi_screen_run_until_connected()` corre bloqueante sobre
    el mismo task que la llama (`app_main()`, en el arranque), porque es
    un flujo de una sola vez al boot, no una pantalla que el usuario
    reabre repetidamente; no hace falta un task dedicado cuando quien
    llama ya no tiene nada más que hacer mientras tanto. Sí usa dos colas
    (`wifi_ap_selected_queue`, `wifi_password_submit_queue`,
    `app_globals.h`) para que los callbacks de LVGL (que corren en el
    task de `lvgl_port`) le entreguen la selección/contraseña.

  **Cambio de flujo de arranque (`main.c`)**, tal como se propuso:
  ```c
  ESP_ERROR_CHECK(wifi_manager_init());
  if (wifi_manager_try_stored_credentials() != ESP_OK)
  {
      if (wifi_screen_run_until_connected() != ESP_OK) { ...; return; }
      lv_disp_load_scr(ui_PlayerScreen); // ui_init() ya la había cargado por defecto
  }
  ```
  `wifi_manager_init()` sigue con `ESP_ERROR_CHECK` (fallos ahí son de
  setup del driver/netif, no de red — genuinamente fatales); conectar no.
  Resto del orden de arranque sin cambios.

  **Alcance explícitamente fuera** (sin cambios respecto a la propuesta):
  Ethernet/Thread/PPP, IPv6, aprovisionamiento BLE/SoftAP
  (`wifi_provisioning`) — una pantalla de escanear+tocar+tipear alcanza.

  Verificado con `idf.py reconfigure` (archivos nuevos) + `idf.py build`
  completo, sin errores ni warnings nuevos.
  **Probado en hardware real 2026-07-09: funciona.**
  **Follow-up mismo día**: se borró `components/protocol_examples_common`
  (`git rm -r`) — quedaba sin usar tras este cambio (confirmado con
  `grep` en todo el árbol antes de borrar), y el ejemplo standalone
  dentro de `components/spotify_client/examples/` no dependía de esta
  copia local: su `idf_component.yml` apunta a
  `${IDF_PATH}/examples/common_components/protocol_examples_common`, la
  copia vendorizada del propio SDK, así que no se rompió nada. Verificado
  con `idf.py reconfigure` + `idf.py build` completo (rebuild total,
  cambió la lista de componentes), sin errores ni warnings nuevos.

- [x] **3.9 Análisis de acoplamiento de `main/` + extracción de
  `wifi_manager` a componente propio**
  El usuario pidió un análisis "como desarrollador senior" de la
  viabilidad de aligerar `main/` y opciones de refactoring para evitar
  acoplamiento (o defensa del formato actual), incluyendo evaluar sacar
  cosas a componentes nuevos — puntualmente, si `wifi_manager` debería
  sumarse al managed component `bsp_jc3248w535` (mantenido por el
  usuario).

  **Hallazgos del análisis** (medido con `wc -l` sobre todo `main/`,
  ~3.2K líneas de código real sin contar los `.c` de fuentes generadas):
  el acoplamiento real no está en "un archivo gigante" — cada pantalla
  ya tiene su propio `.c`/`.h` acotado (99-453 líneas) — sino en dos
  archivos "pegamento" que crecen linealmente con cada feature nueva:
  - `app_globals.h`: namespace plano con las colas/handles de las 4
    features (playlist/device/search/wifi) + el handle de Spotify.
    Cualquier consumidor ve las colas de las otras features aunque no le
    importen.
  - `ui_events.c`: una función-pegamento por pantalla, todas en el mismo
    archivo, sin agruparlas por feature.
  Ninguno es grave hoy, pero es el patrón que va a doler con una 6ª
  pantalla. Refactors identificados (bajo riesgo, mecánicos, **no
  pedidos todavía**): partir `app_globals.h` por feature; mover cada
  función de `ui_events.c` a su propio `screen.c`. `ui.h` se dejó
  explícitamente sin tocar — partirlo no compensa, ya que las 4
  pantallas necesitan cross-referenciar `ui_PlayerScreen` para volver.

  **La pregunta de `wifi_manager` en `bsp_jc3248w535`**: recomendación en
  contra, y no por tecnicismo — es el propio README del BSP el que
  define su alcance ("encapsula el bring-up de [display+touch+batería+
  ampli]... para llevarlo tal cual a otro proyecto que use la misma
  placa"). Confirmado revisando su `CMakeLists.txt`: `REQUIRES driver
  esp_lcd esp_driver_i2s esp_adc`, todo genuinamente específico de esa
  placa. Wi-Fi es una capacidad del **SoC** (ESP32-S3), no de la placa —
  `wifi_manager.c` no toca un solo pin específico de la JC3248W535, así
  que meterlo en el BSP diluiría su promesa de reusabilidad documentada
  (alguien con el mismo panel en otra placa arrastraría Wi-Fi sin
  pedirlo; alguien que solo quiera Wi-Fi arrastraría display/audio/
  batería sin pedirlo).

  **Implementado, con confirmación del usuario**: se extrajo
  `main/wifi_manager.c`/`.h` a un componente local nuevo y separado,
  `components/wifi_manager/` (no al BSP) — ya tenía cero dependencia de
  LVGL/`spotify_client` (solo `esp_wifi`/`esp_netif`/`nvs_flash`/
  FreeRTOS), así que la extracción fue mecánica:
  - `components/wifi_manager/CMakeLists.txt`:
    `PRIV_REQUIRES esp_wifi esp_netif nvs_flash` (no `REQUIRES` — el
    header público no expone ningún tipo de esos tres, confirmado
    revisando `wifi_manager.h`: solo `esp_err_t`/`bool`/`stdint`).
  - `components/wifi_manager/README.md`: nuevo, documenta que es
    deliberadamente independiente de cualquier BSP/placa.
  - `main/wifi_screen.c`/`main.c` no necesitaron cambios de código — el
    `#include "wifi_manager.h"` sigue resolviendo igual, ESP-IDF
    descubre el componente nuevo automáticamente (mismo mecanismo por el
    que `main/CMakeLists.txt` nunca necesitó un `REQUIRES` explícito
    para `spotify_client.h`/`bsp_jc3248w535.h`).
  `wifi_screen.c`/`ui_WifiScreen.c` (la parte acoplada a LVGL/al flujo de
  pantallas de esta app puntual) se dejaron deliberadamente en `main/` —
  no son reusables tal cual en otro proyecto ni encajan en un BSP de
  hardware.
  Verificado con `idf.py reconfigure` (el componente nuevo aparece en la
  lista de componentes del build, `libwifi_manager.a` se linkea aparte)
  + `idf.py build` completo, sin errores ni warnings nuevos, mismo
  tamaño de binario (reorganización, no cambio de código).

## Estado

- 2026-07-06: resueltos 1.1, 1.2 (completado por 2.4, ver nota), 1.3, 1.5,
  1.6, 1.7, 1.9, 1.10, 1.11 (encontrado y resuelto en la misma sesión, no
  estaba en el checklist original), 2.1-2.5, y 3.2-3.5 (ver notas en cada
  ítem). 1.8/3.1 decididos como "no se hace" (ver notas ahí). 3.3 resuelto
  solo parcialmente en la práctica: mitiga el fetch inicial (`GET_STATE`)
  pero no el disparador más probable (evento de WebSocket con un episodio
  en medio de sesión) — el riesgo de fondo sigue abierto en 1.4.
- 2026-07-07: resueltos 2.6 (spotify_client.c dividido en spotify_client.c/
  spotify_auth.c/player_commands.c/player_task.c) y 2.7 (magic numbers
  nombrados, incluyendo el hallazgo extra del prefijo "Bearer " hardcodeado
  en 4 lugares), ver notas en cada ítem. Verificado con `idf.py build`
  completo, sin errores ni warnings nuevos. Re-revisando el componente
  entero con la nueva estructura de archivos se encontraron 2 puntos nuevos,
  no estaban en el checklist original: **1.14** (comando `CHANGE_VOLUME`
  inalcanzable con una mina de `url = NULL` si alguna vez se conecta) y
  **2.8** (código muerto: `TOKEN_URL` sin usar, y `onDevicePlaying`/
  `str_append` comentados en `parse_objects.c`, restos de una UI anterior).
  El usuario pidió implementar 1.14 de verdad (control de volumen táctil en
  el player, no solo eliminar el código muerto): resuelto ese mismo día,
  ver nota en el ítem. Afecta también `main/ui/` (nuevos botones +/- en
  `ui_PlayerScreen`). Verificado con `idf.py build`, no probado en hardware.
- 2026-07-08: usuario probó en hardware real. El volumen funciona, pero
  reportó que en algún momento posterior el player deja de recibir eventos
  de cambio de track ("pareciera que deja de escuchar los eventos"). No se
  pudo reproducir en el momento para conseguir el log serie — **sin
  confirmar si es una regresión de 1.14 o algo preexistente**. Ver **1.15**
  (nuevo, sin resolver) para el detalle de las hipótesis ya descartadas por
  lectura de código y qué mirar en el log cuando se pueda reproducir.
- 2026-07-09: arreglado el bug del slider (knob superaba el borde superior
  de la pantalla al 100%, ver nota en 1.14), implementada la sincronización
  de volumen desde otros clientes (propuesta y diseñada por el usuario, ver
  nota en 1.14), y resuelto **2.9** (recorte de `Event_t`: 7 de 12 valores
  eran peso muerto, ver nota ahí). Agregado logging de diagnóstico temporal
  (`ESP_LOGI`) para 1.15 y para confirmar el flujo de volumen por WS.
  Renombrado `Event_t` → `PlayerEvent_t` y el campo `SpotifyEvent_t.type` →
  `.player_event` (a pedido del usuario, mismo hilo que 2.9).
- 2026-07-09/10: resuelto **1.16** (barra de progreso convertida en slider
  táctil con seek real, ver nota ahí) — misma arquitectura que el volumen
  pero con commit en `LV_EVENT_RELEASED` en vez de debounce.
- 2026-07-10: usuario mandó el primer log real de hardware probando
  1.14/1.16. Confirma que la sincronización de volumen externo funciona
  (valores reales 0→29→58 detectados vía WS) y que el fallback `UNKNOW`
  aguanta bien tipos de mensaje WS no manejados (deviceBroadcastStatus,
  resume-point, etc.) sin crashear. **No reproduce 1.15** (sigue sin
  confirmar). Reveló y se resolvió **1.17**: fallos de volumen/seek (404
  de Spotify, probablemente "sin dispositivo activo") eran completamente
  silenciosos — ahora se loguean y el slider se resincroniza al valor
  real. Ver nota en 1.17 para el detalle completo.
- 2026-07-10: resuelto **1.18** (rebote cosmético de la barra de seek al
  soltar el dedo) — congelamiento temporal en `player_screen.c`, sin tocar
  el backend, decisión explícita del usuario de mantenerlo como
  preocupación de UI.
- 2026-07-10: mismo log de hardware reveló y se resolvió **1.19** — el
  refresco reactivo de token (401) nunca se disparaba en la práctica
  porque `esp_http_client` reporta el 401 de Spotify como
  `ESP_ERR_NOT_SUPPORTED` (WWW-Authenticate: Bearer no es Basic/Digest),
  no como un 401 limpio. Sin refresco proactivo posible (1.7, Discord no
  manda `expires_in`) ni reactivo funcionando, un token vencido dejaba la
  app rota para toda interacción HTTP hasta reiniciar. Arreglado en un
  solo lugar (`perform_http_request()`), sin tocar los call sites. Ver
  nota en 1.19 para el detalle completo — posiblemente explica parte de
  la sensación de "se cuelga" reportada informalmente antes de 1.15.
- 2026-07-10: resuelto **1.20** — nueva funcionalidad, listar dispositivos
  y transferir playback, como modal en `ui_PlayerScreen` (no una screen
  nueva, decisión del usuario) con botón en la fila de transporte. Ese
  mismo modal, al probarse en hardware, reveló y se resolvió **1.21**:
  `parse_access_token` crasheaba el dispositivo si Discord respondía sin
  `"access_token"` (mismo patrón que 1.5/1.12, se había escapado de 3.2).
  Hallazgo relacionado (mismo patrón de `ERR_CHECK` en
  `parse_available_devices`) también resuelto a pedido del usuario, mismo
  día — ver nota en 1.21.
- 2026-07-10: resuelto **1.22** — `parse_track` ahora filtra por `"uri"`
  (`"wss://event"`) antes de tocar `payloads`, en vez de descubrir por
  prueba y error que un push del WS (herodotus, playlist, social-connect)
  no es un evento de player/device. Idea del usuario, refinada (el
  `"type"` de nivel superior no discrimina nada, siempre es `"message"`).
- 2026-07-10/11: completado **1.23** — evaluación de memoria estática vs.
  dinámica a pedido del usuario, con foco en riesgo de fragmentación. Sin
  cambios de código (es una evaluación). Estática: irrelevante (~260 B
  propios de la app, componente `spotify_client` en 0 B de bss/data).
  Dinámica de arranque (~78 KB, incluye `json_tokens[1000]` a 20 B c/u por
  `JSMN_PARENT_LINKS`): una sola vez, riesgo nulo. Dinámica recurrente: sin
  leaks, patrón a vigilar es el de strings de tamaño variable por track
  (nombre/artista/álbum/portada); el buffer de portada (90 KB PSRAM) es
  grande pero de tamaño fijo, fragmenta menos de lo que aparenta. Ver nota
  completa en 1.23.
- 2026-07-11: implementado **3.7** — funcionalidad de búsqueda de
  canciones (solo `type=track`, decisión de alcance del mismo día).
  Backend nuevo en `spotify_client` (`TRACK_LIST`/`TrackSearchItem_t`,
  `parse_search_results()`, `url_encode()`, `spotify_search_tracks()`,
  `spotify_play_track_uri()`) + pantalla nueva en `main/`
  (`ui_SearchScreen.c`, `search_screen.c/.h`, botón en la fila de
  transporte a `+210`, wiring en `ui_events.c`/`ui.c`/`app_globals.h`/
  `main.c`). Ver nota completa en 3.7 para el detalle de cada pieza y las
  decisiones de diseño (buffer/tokens propios de la búsqueda en vez de
  agrandar los compartidos, teclado como pantalla propia no modal, cómo
  decide `searchBackFn` a qué cola mandar el sentinel). Verificado con
  `idf.py reconfigure` + `idf.py build` completo, sin errores ni warnings
  nuevos. No probado en hardware todavía.
- 2026-07-11: a pedido del usuario, se verificó y documentó cómo la app
  accede a internet hoy. Hallazgo: SSID/password hardcodeados en build
  time vía Kconfig (`protocol_examples_common`, componente de ejemplo de
  ESP-IDF, no propio de esta app), sin persistencia real
  (`WIFI_STORAGE_RAM`, no `FLASH`), y un bug de crash encontrado en el
  camino: `example_connect()` está envuelto en `ESP_ERROR_CHECK` en
  `main.c`, así que una password mal escrita o un AP inalcanzable en el
  boot abortan el dispositivo (mismo patrón que 1.5/1.19/1.21, ver
  **1.24**). Se propuso un diseño de pantalla de configuración Wi-Fi en
  el dispositivo (reutilizando el mismo patrón lista+textarea+teclado
  recién construido para 3.7) — ver **3.8**.
- 2026-07-09: implementados **1.24/3.8** tras confirmación del usuario.
  `main/wifi_manager.c/.h` (esp_wifi manejado directamente, persistencia
  propia vía NVS namespace `wifi_cfg`, nunca `ESP_ERROR_CHECK` sobre
  fallos de conexión) + `main/wifi_screen.c/.h`/`ui_WifiScreen.c` (a
  diferencia del diseño original, sin task+cola propia — corre
  bloqueante sobre el task que llama, porque es un flujo de una sola vez
  al boot, no una pantalla reabierta). `main.c` ya no depende de
  `protocol_examples_common`/`example_connect()`. Ver nota completa en
  3.8 para el detalle y las diferencias con la propuesta original.
  Verificado con `idf.py reconfigure` + `idf.py build` completo, sin
  errores ni warnings nuevos.
  **Probado en hardware real: funciona.** Se borró
  `components/protocol_examples_common` (`git rm -r`), quedaba sin usar.
- 2026-07-09: resuelto **1.25** — la ñ y las vocales acentuadas no
  renderizaban en la UI (fuentes bitmap de LVGL, ASCII-only, sin
  cobertura de Latin-1 Supplement). Se regeneraron fuentes propias
  (`main/ui/fonts/lv_font_es_14.c`/`_20.c`) con `lv_font_conv` a partir
  de las mismas fuentes que usa LVGL internamente, con el rango
  extendido. Un solo cambio en `ui_init()` (fuente del tema) corrige la
  mayoría del texto sin tocar cada label. Ver nota completa en 1.25.
  Verificado con `idf.py build`, sin errores ni warnings nuevos. No
  probado en hardware todavía.
- 2026-07-09: resuelto **3.9** — análisis de acoplamiento de `main/`
  ("como desarrollador senior") + extracción de `wifi_manager` a
  `components/wifi_manager/`, componente propio y separado del BSP
  `bsp_jc3248w535` (recomendación explícita en contra de meterlo ahí,
  justificada con el propio alcance documentado del BSP). Identificados
  pero **no implementados** dos refactors de bajo riesgo para cuando se
  agregue una pantalla más: partir `app_globals.h` por feature, mover
  las funciones de `ui_events.c` a cada `screen.c`. Ver nota completa en
  3.9. Verificado con `idf.py reconfigure` + `idf.py build` completo,
  sin errores ni warnings nuevos, mismo tamaño de binario.
  Pendientes: 1.4, 1.15, 2.8, 3.6.
