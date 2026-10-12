# NOTES-backend.md

Notas de reconocimiento (Fase 0) sobre el comportamiento real de koboldcpp,
para alimentar el diseño de `ProcessManager` y `KoboldClient`.

## Carga completa — `koboldcpp` con Llama-3.2-3B-Instruct-Q4_K_M, GTX 1650 4GB

Comando: koboldcpp con autodetección CUDA, autofit de capas (`-c 8320 -ngl 23`).
Log completo capturado el 2026-09-02, ver transcripción de la sesión.

### ¿Hay porcentaje de progreso o solo ruido?

**Solo ruido — no hay porcentaje numérico en ningún momento.** La carga pasa por:

1. Bloque `Namespace(...)` con todos los flags parseados (una sola línea larga).
2. Metadata del modelo vía `print_info:` (arquitectura, capas, vocab, etc.) — texto
   fijo, no progreso.
3. Una fila de puntos (`...........`) durante `load_all_data: using async uploads`
   — es la única señal visual de que algo se está subiendo a VRAM, pero **no trae
   número ni fracción**, solo aparece de una vez.
4. Más líneas de inicialización (`llama_context:`, `llama_kv_cache:`, `sched_reserve:`, etc.)

**Conclusión para la UI:** no se puede armar una barra de progreso real parseando
stdout. La carga se ve como "todo o nada" — hay que mostrar un estado indeterminado
(spinner) entre el lanzamiento del proceso y la línea de listo, no una barra con %.

### ¿Qué línea exacta marca "servidor listo"?

La línea que confirma que el servidor HTTP está escuchando y listo para requests:

```
Starting Kobold API on port 5001 at http://localhost:5001/api/
```

**Importante para el parser:** el puerto es el que se pasó por `--port` (5001 en
esta corrida porque es el default), así que `ProcessManager` debe matchear con un
patrón tipo `Starting Kobold API on port \d+` en vez de hardcodear `5001`.

Antes de esa línea, hay una señal intermedia útil que confirma que el modelo
específicamente cargó bien (separado de que la API ya esté arriba):

```
Load Text Model OK: True
```

Orden real de eventos relevantes:
1. `Load Text Model OK: True` → el modelo terminó de cargar en memoria/VRAM.
2. (unas líneas de módulos activos/inactivos, APIs habilitadas)
3. `Starting Kobold API on port 5001 at http://localhost:5001/api/` → **este es
   el signal de "listo" real** — recién acá se puede pegarle al `KoboldClient`.

### Otras notas sueltas de esta corrida

- `Detected Available GPU Memory: 4096 MB` y luego `VRAM: 3723 MiB` (real, según
  CUDA) — hay una diferencia entre lo que koboldcpp "detecta" al inicio y lo que
  reporta CUDA después. Para la feature de "¿cabe en VRAM?" (Fase 3), conviene
  usar el número de `ggml_cuda_init` / `free` (`3661 MiB free` en este caso), no
  el `Detected Available GPU Memory` inicial.
- `Autofit Success: 1, Autofit Result: -c 8320 -ngl 23` — con autofit activado,
  koboldcpp decide `ngl` y `contextsize` solo y lo imprime en una línea propia.
  Si Krampus deja el autofit prendido por default, este es el lugar para leer
  qué valores terminó usando.
- Hay ruido de entorno no relacionado a koboldcpp en sí (`No such schema
  ?org.gnome.mutter?`, warning de locale `C` → `C.UTF-8`, iconos KDE no
  encontrados) — hay que ignorarlo al parsear, no tratarlo como error.
- El caso de carga que falla por VRAM insuficiente se capturó el 2026-10-12,
  ver más abajo.

## Segunda ronda de capturas — 2026-10-12

Todo capturado con `scripts/capturar-carga.sh` (antepone a cada línea los segundos
desde el arranque). Los archivos crudos están en `logs/`.

### Tiempo de carga — `logs/carga-ok.log`

| Segundo | Evento |
|---|---|
| 1.78 | `Loading Text Model: ...` |
| 4.10 | `Autofit Success: 1, Autofit Result: -c 16512 -ngl 17` |
| 4.73 | `load_tensors: offloaded 17/29 layers to GPU` |
| 5.08 | fila de puntos (subida a VRAM terminada) |
| 6.04 | `Load Text Model OK: True` |
| 6.04 | `Starting Kobold API on port 5001 ...` |

**~6 s de lanzamiento a listo**, con el archivo ya en la caché de disco (se había
leído un minuto antes). Falta medir una carga en frío, que será más lenta.

**Conclusión:** spinner. Con 6 s y sin porcentaje en stdout, una barra no aporta.
El timeout de carga de la Fase 2 debe ser holgado (decenas de segundos) para
cubrir modelos más grandes y disco frío.

Otros datos de esta corrida:

- El resultado del autofit cambió respecto a septiembre (`-c 8320 -ngl 23` →
  `-c 16512 -ngl 17`) porque el `contextsize` por defecto ahora es 16384. No hay
  que asumir valores: siempre leer la línea `Autofit Result`.
- El log de una carga normal tiene **1031 líneas**, y 858 son ruido de
  `llama_graph_n_input_tensors:`. El parser debe descartar barato lo que no
  reconoce.
- **El orden de las líneas cambia al leer por pipe.** `This model does not use
  SWA` y `Starting model warm up` aparecieron 11 s después de la línea de listo,
  justo al llegar la primera petición. Son `printf` de C, que con stdout
  conectado a un pipe se guardan en un buffer en vez de salir línea por línea
  (en una terminal sí salen al momento). Las líneas que nos importan
  (`Load Text Model OK`, `Starting Kobold API`) las imprime Python y llegaron a
  tiempo. `QProcess` también lee por pipe, así que verá este mismo desorden: no
  depender del orden relativo entre líneas de C y de Python.

### Carga que falla por VRAM — `logs/carga-falla-vram.log`

Forzada con `--gpulayers 99 --contextsize 65536` (desactiva el autofit).

**El proceso muere solo, rápido y con código de salida distinto de cero.** No se
cuelga ni queda vivo sirviendo la API.

```
2.667  ggml_backend_cuda_buffer_type_alloc_buffer: allocating 7196.00 MiB on device 0: cudaMalloc failed: out of memory
2.668  alloc_tensor_range: failed to allocate CUDA0 buffer of size 7545552896
2.671  llama_init_from_model: failed to initialize the context: failed to allocate buffer for kv cache
2.671  gpttype_load_model: error: failed to load model '...'
3.671  Load Text Model OK: False
3.671  Could not load text model: ...
       exit code: 3
```

Para `ProcessManager`:

- Señal de fallo fiable: `Load Text Model OK: False`, y además el proceso termina
  con **exit code 3** (unos 2 s después de esa línea).
- Para el mensaje al usuario, buscar `cudaMalloc failed: out of memory` en lo ya
  leído: distingue "no cabe en VRAM" de otros fallos (archivo corrupto, ruta mala).
- Los pesos sí cupieron (1918 MiB); lo que no cupo fue el **KV cache del
  contexto** (7196 MiB). Para la estimación "¿cabe?" de la Fase 3 no basta el
  tamaño del archivo: el contexto pesa, y a 65536 pesa más que el modelo.

### Stream SSE — `logs/sse-raw.txt` y `logs/sse-trace.txt`

Formato de cada evento (saltos de línea `\n`, no `\r\n`):

```
event: message
data: {"token": " inv", "finish_reason": null}
<línea vacía>
```

- **Los eventos SÍ llegan fragmentados.** En la traza, varias veces
  `event: message\n` llega en una lectura (15 bytes) y la línea `data: ...\n\n` en
  la siguiente; otras veces llegan juntas. El parser tiene que acumular en un
  buffer y solo procesar cuando encuentre el separador `\n\n`.
- Un token por evento. El JSON de `data:` trae `token` y `finish_reason`.
- **Fin del stream:** un último evento con `"token": ""` y `finish_reason`
  distinto de `null` (`"length"` al agotar `max_length`). No hay `[DONE]`.
- Los caracteres no ASCII vienen escapados dentro del JSON (`"ío"` = "ío"),
  así que un corte de lectura nunca parte un carácter UTF-8 por la mitad.
- La respuesta es `HTTP/1.0` con `content-type: text/event-stream`, sin
  `Content-Length`.
- Latencia: ~0.5 s hasta el primer token, luego ~50 ms por token (≈ 18 tok/s
  con 17 capas en GPU).

### Esquema de la API — `logs/api-docs.html`

`GET /api` devuelve la página Swagger con la especificación OpenAPI 3.0.3
embebida (1.7 MB). Los seis endpoints que usa Krampus existen con esos nombres
exactos. Dos que no teníamos en la lista y pueden servir:

- `POST /api/extra/generate/check` — texto generado hasta el momento.
- `GET /api/extra/true_max_context_length` — contexto real con el que cargó.

### Apagado

Con Ctrl-C (SIGINT) el proceso termina en menos de 5 s. El código de salida no
es consistente (0 en una corrida, 120 en otra), así que tras pedirle que cierre
no hay que interpretar el código como error.

## Pendiente de Fase 0

- [x] Captura de carga fallida por VRAM insuficiente
- [x] `curl` crudo contra `/api/extra/generate/stream` — hay que bufferear
- [x] Guardar el esquema de `http://localhost:5001/api`
- [x] Cronometrar cuánto tarda una carga real — spinner
- [ ] (opcional) Cronometrar una carga en frío, recién arrancado el equipo
