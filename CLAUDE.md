# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Estado actual: sin código todavía

Este repo está en fase de planificación (Fase 0 — Reconocimiento, ver `.claude/PLAN.md`). No existe
`CMakeLists.txt`, `src/`, ni ningún archivo de código fuente. Por lo tanto no hay comandos de build,
lint o test que documentar — no los inventes. Antes de escribir código, consulta `.claude/PLAN.md`
para saber en qué fase está el proyecto y qué falta antes de esa fase.

# Krampus

Frontend Qt nativo para KoboldCpp en KDE Plasma 6. "El LM Studio de Kobold, pero abierto."

## Qué es y qué no es

Krampus **no hace inferencia**. Lanza el binario de koboldcpp y habla HTTP con él.
Si un modelo corre en koboldcpp, corre en Krampus. Punto. No parseamos GGUF para
inferir, no linkeamos llama.cpp, no compilamos nada de CUDA.

No estamos compitiendo con SillyTavern. ST es una capa de roleplay enorme
(personas, lorebooks, extensiones) que se paga con setup de Node y una pestaña
del navegador. Krampus es lo contrario: instalas, abres, cargas modelo, chateas.
Cero ceremonia.

## Stack (decidido, no re-litigar)

- **C++17 + Qt 6**. No Python.
- **QML / Qt Quick** para la UI. NO QtWidgets — el chat con streaming y burbujas
  de altura variable se hace mal en QTextEdit y luego hay que reescribir todo.
- **CMake** como build system.
- KDE Frameworks 6 permitido pero **solo donde aporte de verdad** (KConfig,
  KNotifications, KAboutData). No meter Kirigami en v1 — añade una dependencia
  grande antes de saber si la necesitamos.
- Config en `~/.config/krampus/`. Texto plano editable a mano.

---

## ⚠️ CONVENCIONES DE CÓDIGO — LEER ANTES DE ESCRIBIR NADA

**El dueño de este proyecto sabe Python básico, y está aprendiendo C++ con
este proyecto.** El código no es solo para que funcione, es para que se entienda.
Estas reglas mandan sobre cualquier idea de "código idiomático":

### Simplicidad sobre elegancia
- **Nada de plantillas (templates)** salvo que no haya alternativa razonable.
- **Nada de metaprogramación**, SFINAE, `constexpr` avanzado, ni trucos de tipos.
- Preferir código explícito y largo sobre código corto y denso.
- Nada de `auto` cuando el tipo no sea obvio en la misma línea. `auto reply =
  manager->get(req)` está bien; `auto x = foo()` no.
- Un archivo, una responsabilidad. Clases chicas.

### Comentarios: explicar el *porqué*, no el *qué*
Cada vez que aparezca un concepto de C++ que no exista en Python, **explicarlo
en un comentario en el momento**. Ejemplos de lo que sí quiero ver:

```cpp
// `const QString&` = referencia constante. Pasamos por referencia para no copiar
// el string entero, y `const` promete que no lo vamos a modificar. En Python los
// objetos ya se pasan por referencia y no hay que pensarlo; en C++ hay que pedirlo.
void setModelPath(const QString& path);

// std::unique_ptr = puntero que se borra solo cuando sale de scope. Es lo más
// parecido al conteo de referencias de Python para un objeto individual: no hay
// que llamar delete a mano.
std::unique_ptr<Config> m_config;

// Este QProcess NO lleva unique_ptr porque le pasamos `this` como padre.
// Qt tiene su propio sistema de ownership: cuando el padre muere, borra a los
// hijos. Mezclar smart pointers con el árbol de Qt causa double-free.
m_process = new QProcess(this);
```

Conceptos que **siempre** llevan explicación la primera vez que aparecen en un
archivo: referencias vs punteros, `const` en sus distintas posiciones, ownership
(quién borra qué), header vs implementación, `explicit`, el macro `Q_OBJECT`,
signals/slots, y cualquier cosa de CMake que no sea evidente.

### No dar por sabido
Si una decisión tiene una alternativa razonable, decir en un comentario por qué
se eligió esta. "Usamos X porque Y falla cuando Z."

### Errores legibles
- Compilar siempre con `-Wall -Wextra`.
- Build de debug con **AddressSanitizer activado** (`-fsanitize=address`). Esto
  convierte la mitad de los segfaults en mensajes que se pueden leer.
- Nada de `catch(...)` silencioso. Los errores se propagan o se muestran.

### Al explicar un crash
Cuando algo truene, no solo arreglarlo: decir qué lo causó en términos de C++
(uso después de liberar, referencia colgante, etc.) y cómo se detecta la próxima
vez con gdb o ASan.

---

## Alcance del v1

SÍ:
- Detección del binario de koboldcpp + selección manual como fallback
- Escaneo de un directorio de modelos, listado de GGUFs
- Cargar modelo con controles: gpulayers, context size, puerto
- Chat con streaming token por token
- Controles de samplers (temp, top_p, top_k, rep_pen, min_p)
- Abort de generación
- Guardar/cargar presets como `.kcpps`
- Historial de conversación en disco

NO en v1 (esto es una lista dura, no una sugerencia):
- Descargador / buscador de HuggingFace
- Character cards, personas, lorebooks, world info
- RAG, embeddings, vector stores
- Multimodal (visión, image gen / stable-diffusion.cpp)
- Text-to-speech, whisper
- Applet de Plasma, integración con KRunner
- Soporte multi-usuario o remoto avanzado

**El proyecto se muere si el scope se infla intentando ser SillyTavern otra vez.**
Si aparece una idea buena que no está en la lista de SÍ, va a `IDEAS.md`, no al código.

## Arquitectura

Dos capas, bien separadas:

1. **`src/backend/ProcessManager.{h,cpp}`** — gestor del proceso. `QProcess`
   lanzando koboldcpp con flags. Parsea stdout para progreso de carga. Detecta
   readiness. Cleanup garantizado.
2. **`src/backend/KoboldClient.{h,cpp}`** — cliente HTTP. `QNetworkAccessManager`
   contra la API.

La UI (QML) nunca lanza procesos ni hace HTTP directo. Habla con estas dos clases
a través de propiedades y signals expuestos con `Q_PROPERTY` / `Q_INVOKABLE`.

### Endpoints de koboldcpp

- `POST /v1/chat/completions` con `"stream": true` — SSE estilo OpenAI, token
  por token (el que usamos). koboldcpp aplica la plantilla de chat del modelo
  cargado, así que Krampus manda mensajes con rol y nunca arma plantillas.
  Acepta también los samplers nativos (`min_p`, `rep_pen`, `top_k`...).
  Formato medido en `NOTES-backend.md`, sección "Endpoint de chat".
- `POST /api/v1/generate` — generación bloqueante
- `POST /api/extra/generate/stream` — SSE de texto crudo, sin plantilla de chat
  (lo usó la primera versión; el modelo solo continuaba el texto)
- `POST /api/extra/abort` — cancelar
- `POST /api/extra/tokencount` — contador de contexto
- `GET  /api/extra/perf` — tok/s, stats
- `GET  /api/v1/model` — modelo cargado / readiness check

Swagger completo en `http://localhost:5001/api` con el server corriendo.
Consultarlo ahí antes de asumir la forma de un payload.

Qt no tiene cliente SSE nativo. Hay que parsear el `readyRead()` de la respuesta
a mano, buffereando hasta tener eventos completos. Es la parte más delicada del
backend — comentarla bien.

### Modos de operación

- **Managed** (default): Krampus lanza el proceso y lo mata al cerrar.
- **Attach**: conectarse a un koboldcpp ya corriendo, dado un URL.

Ambos modos usan el mismo `KoboldClient`. Solo cambia si `ProcessManager` está activo.

## Reglas de integración con Plasma 6

- **Nunca hardcodear colores.** Usar la palette de Qt / roles del sistema.
  Si defines hex, Breeze deja de aplicar y la app se ve ajena.
- Tray con `QSystemTrayIcon` (Plasma habla SNI, funciona de una).
- Notificaciones vía `org.freedesktop.Notifications` (QDBus o KNotifications).
- Wayland nativo. **No meter QtWebEngine** bajo ninguna circunstancia.
- `.desktop` file + AppStream metainfo + icono para que se sienta instalada.

## Empaquetado

`depends` NO puede ser `koboldcpp`. El backend está fragmentado en el AUR
(`koboldcpp-cuda`, `koboldcpp-hipblas`, `koboldcpp-vulkan`, `koboldcpp-bin`) y
ninguno declara un `provides` común. Van todos como `optdepends`.

`depends=('qt6-base' 'qt6-declarative')` y lo que se agregue de KF6.

Detección en runtime: buscar nombres conocidos en `$PATH`, y si no hay nada,
diálogo para apuntar al binario a mano. Mucha gente tiene binarios fuera de
`/usr/bin` — nunca asumir la ruta.

Mismo criterio para el directorio de modelos: configurable, default sensato,
pero apuntable a donde el usuario ya los tenga.

## El contrato `.kcpps`

Los presets se guardan como archivos `.kcpps` reales, no formato propio. Esto es
lo que hace a Krampus "la cara bonita de koboldcpp" en vez de "otro frontend más":

- Un preset del launcher oficial abre en Krampus
- Un preset de Krampus corre con `koboldcpp --config preset.kcpps`
- Si la GUI truena, el usuario no perdió su config

## Método de trabajo

**Feo pero funcionando, en cada commit.** Nunca dejar una rama donde nada corre.
Rebanadas verticales completas antes que capas horizontales perfectas.

El diseño visual va al final, cuando ya se conocen todos los estados reales que
hay que representar (cargando / listo / generando / error / sin backend).

## Entorno de desarrollo

- Arch Linux, KDE Plasma 6, Wayland, nvidia-open
- GPU: GTX 1650 Mobile, **4GB VRAM** — probar siempre con modelos que quepan, y
  probar a propósito el caso de "no cabe" porque es un estado real de la UI
- Modelos en `/mnt/Field/Storage/Local-AI/`
- El binario instalado es `koboldcpp-cuda`
- Herramientas: `gdb` para crashes, `ASan` en debug builds
