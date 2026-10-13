# Plan de desarrollo — Krampus

Cinco fases. Cada una termina con algo que corre. No se pasa a la siguiente sin
que la anterior funcione de verdad en la máquina.

---

## Fase 0 — Reconocimiento (antes de escribir la app)

El objetivo no es código bonito, es **contestar preguntas que ningún plan puede
contestar en abstracto**. Scripts sucios, tirar a la basura después.

- [x] Correr `koboldcpp-cuda` a mano y capturar **todo el stdout** de una carga
      completa a un archivo. ¿Hay porcentaje de progreso o solo ruido? ¿Qué línea
      exacta marca "servidor listo"? → ver `NOTES-backend.md`
- [x] Capturar el stdout de una carga que **falla por VRAM insuficiente**. ¿El
      proceso muere, se cuelga, o escupe error y sigue vivo?
- [x] `curl` crudo contra `/api/extra/generate/stream`. Guardar la respuesta raw.
      ¿Los eventos SSE llegan completos o hay que bufferear fragmentos?
- [x] Abrir `http://localhost:5001/api` y guardar el esquema de los payloads que
      vamos a usar.
- [x] Cronometrar cuánto tarda realmente una carga. ¿Spinner o barra de progreso?

**Entregable:** un `NOTES-backend.md` con las respuestas. Esto alimenta todo lo demás.

---

## Fase 1 — Rebanada vertical fea

Una ventana horrible que funciona de punta a punta. QML mínimo, cero estilo.

- [x] Scaffold del proyecto: estructura de directorios, `CMakeLists.txt` con Qt6
      (Core, Quick, Network), build de debug con `-Wall -Wextra -fsanitize=address`
- [x] Verificar que `gdb ./krampus` corre y que `bt` funciona — antes de necesitarlo
- [x] `ProcessManager`: lanzar koboldcpp con QProcess, signal `ready` cuando
      el server responde, signal `failed` cuando no
- [ ] `KoboldClient`: POST a `/api/extra/generate/stream`, parser SSE a mano
      sobre `readyRead()`, signal `tokenReceived`
- [ ] UI: campo de texto para ruta del modelo, botón "cargar", input de prompt,
      área de salida sin formato
- [ ] Que se vea el texto apareciendo token por token

**Criterio de terminado:** cargas un GGUF, escribes algo, ves la respuesta
streaming. Feo, pero real.

---

## Fase 2 — Robustez del proceso

Lo ingrato. Es lo que separa un script de una app instalable, y es carísimo de
meter después.

- [ ] Cleanup garantizado: koboldcpp muere cuando Krampus muere, incluso si
      Krampus crashea (signal handlers POSIX, process group)
- [ ] Detectar un koboldcpp ya corriendo al arrancar → ofrecer attach en vez de
      lanzar otro
- [ ] Modo attach explícito por URL
- [ ] Manejar el crash del backend sin llevarse la GUI: estado de error, botón de
      reintentar
- [ ] Detección del binario: escanear `$PATH` por nombres conocidos, diálogo de
      selección manual como fallback
- [ ] Timeout de carga con mensaje útil (no un spinner infinito)

**Criterio de terminado:** matas koboldcpp desde otra terminal y la app te lo
dice en vez de congelarse. Cierras la app y no queda nada comiendo VRAM.

---

## Fase 3 — Funcionalidad real

Aquí la app se vuelve usable a diario.

- [ ] Escaneo del directorio de modelos, listado de GGUFs con tamaño en disco
- [ ] Controles de carga: gpulayers, context size, puerto
- [ ] Estimación de si el modelo cabe en VRAM (leer VRAM disponible + tamaño del
      archivo). Esta es *la* feature con 4GB.
- [ ] Panel de samplers: temp, top_p, top_k, rep_pen, min_p
- [ ] Botón de abort funcionando
- [ ] Contador de contexto usado (`/api/extra/tokencount`)
- [ ] tok/s en vivo (`/api/extra/perf`)
- [ ] Guardar/cargar presets `.kcpps` — verificar que abren en el launcher oficial
- [ ] Historial de conversación persistente en disco

**Criterio de terminado:** ya prefieres usar Krampus que la terminal.

---

## Fase 4 — La interfaz bonita

Ahora sí. Para este punto ya se conocen todos los estados reales que hay que
representar, así que diseñar es fácil.

Estados a cubrir explícitamente:
`sin backend detectado` / `backend listo, sin modelo` / `cargando modelo` /
`modelo listo` / `generando` / `error de carga` / `backend caído`

- [ ] Pasada de diseño sobre el chat: burbujas, scroll, markdown en respuestas
- [ ] Estados vacíos que digan qué hacer, no solo estar vacíos
- [ ] Identidad Krampus: icono con cuernos, splash de carga
- [ ] Respetar Breeze completamente — probar en tema claro y oscuro
- [ ] Tray icon + notificaciones ("modelo cargado", "generación terminada")

---

## Fase 5 — Empaquetado

- [ ] `.desktop` file + AppStream metainfo + iconos
- [ ] PKGBUILD con `optdepends` para las variantes de koboldcpp
- [ ] Probar instalación limpia en una VM o chroot
- [ ] README que abra vendiendo el concepto
- [ ] Publicar en AUR
- [ ] Anunciar en r/LocalLLaMA y r/kde

---

## Fuera de alcance (a propósito)

Si alguna de estas se vuelve tentadora, va a `IDEAS.md` y se queda ahí hasta el v2:

descargador de HuggingFace · character cards · lorebooks · RAG · multimodal ·
TTS · applet de Plasma · KRunner · soporte multi-GPU avanzado · edición de
prompts a nivel de template
