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
- Pendiente (no cubierto en esta corrida): capturar el caso de carga que falla
  por VRAM insuficiente — sigue en la lista de Fase 0.

## Pendiente de Fase 0

- [ ] Captura de carga fallida por VRAM insuficiente
- [ ] `curl` crudo contra `/api/extra/generate/stream` — ¿eventos SSE completos o
      hay que bufferear fragmentos?
- [ ] Guardar el esquema de `http://localhost:5001/api`
- [ ] Cronometrar cuánto tarda una carga real (¿spinner o barra?)
