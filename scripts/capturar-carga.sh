#!/usr/bin/env bash
# Script desechable de Fase 0: lanza koboldcpp y guarda todo su stdout+stderr
# en logs/, con el tiempo transcurrido al inicio de cada línea.
#
# Uso: scripts/capturar-carga.sh <nombre-del-log> [flags extra de koboldcpp...]
#   scripts/capturar-carga.sh carga-ok
#   scripts/capturar-carga.sh carga-falla-vram --gpulayers 99 --contextsize 65536
#
# Se detiene con Ctrl-C (koboldcpp no termina solo: se queda sirviendo la API).

MODELO="models/llama3/Llama-3.2-3B-Instruct-Q4_K_M.gguf"
NOMBRE="${1:?falta el nombre del log}"
shift

mkdir -p logs
# EPOCHREALTIME es una variable de bash con la hora actual en segundos y seis
# decimales ("1791842139.723456"). Quitándole el separador queda un entero en
# microsegundos, que bash sí sabe restar (no tiene aritmética con decimales).
# El separador puede ser punto o coma según el locale, por eso `[.,]`.
INICIO=${EPOCHREALTIME/[.,]/}

# `2>&1` junta stderr con stdout: koboldcpp reparte sus mensajes entre los dos
# y ProcessManager va a tener que leer ambos, así que los capturamos mezclados.
koboldcpp --model "$MODELO" --port 5001 "$@" 2>&1 | while IFS= read -r linea; do
    # Milisegundos desde el arranque. No usamos `ts` (moreutils) ni `bc` porque
    # no están instalados.
    ms=$(( (${EPOCHREALTIME/[.,]/} - INICIO) / 1000 ))
    printf '%4d.%03d  %s\n' $((ms / 1000)) $((ms % 1000)) "$linea"
done | tee "logs/$NOMBRE.log"

# PIPESTATUS[0] = código de salida de koboldcpp, no el del `tee` final.
echo "exit code de koboldcpp: ${PIPESTATUS[0]}" | tee -a "logs/$NOMBRE.log"
