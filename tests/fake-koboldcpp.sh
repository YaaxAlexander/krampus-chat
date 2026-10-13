#!/usr/bin/env bash
# fake-koboldcpp.sh — un koboldcpp de mentira para los tests de ProcessManager.
#
# El koboldcpp real necesita una GPU, un modelo de 2 GB y varios segundos por
# carga; nada de eso sirve para un test automático. A ProcessManager lo único
# que le importa del proceso es QUÉ IMPRIME y CÓMO TERMINA, así que este script
# imita exactamente eso: recibe los mismos argumentos, reproduce un log real
# capturado en logs/ y termina (o no) como lo hizo el original.
#
# Uso (igual que el real):
#   fake-koboldcpp.sh --model <ruta> --port <puerto>
#
# Qué situación imitar se elige con la variable de entorno
# FAKE_KOBOLDCPP_ESCENARIO, y no con un argumento extra, para que la línea de
# comandos sea idéntica a la que recibe el koboldcpp de verdad. Los valores
# posibles son las ramas del `case` de más abajo.

# Si ProcessManager cambiara la forma de los argumentos sin querer, los tests
# deben notarlo: se termina con un código raro en vez de seguir como si nada.
if [ "$#" -ne 4 ] || [ "$1" != "--model" ] || [ "$3" != "--port" ]; then
    echo "fake-koboldcpp: argumentos inesperados: $*" >&2
    exit 64
fi
PUERTO="$4"

# Carpeta de los logs reales, ubicada a partir de la de este script, para que
# funcione sin importar desde qué carpeta se lance.
LOGS="$(dirname "$0")/../logs"

# Imprime un log capturado tal como lo imprimió koboldcpp:
#   * borra la línea final "exit code de koboldcpp: N", que añadió el script de
#     captura (scripts/capturar-carga.sh) y no koboldcpp;
#   * quita el prefijo de tiempo ("   1.784  ") del inicio de cada línea, que
#     también es del script de captura;
#   * cambia el 5001 de la captura por el puerto recibido, que es lo que haría
#     el koboldcpp real. Los tests usan otro puerto a propósito, para comprobar
#     que ProcessManager no depende de que sea 5001.
reproducir_log() {
    sed -E \
        -e '/^exit code de koboldcpp: /d' \
        -e 's/^ *[0-9]+\.[0-9]{3}  //' \
        -e "s/5001/$PUERTO/g" \
        "$LOGS/$1"
}

# Quedarse vivo hasta recibir una señal, como hace koboldcpp sirviendo su API.
# `exec` REEMPLAZA a bash por `sleep` en vez de lanzarlo como hijo: así el PID
# que ve ProcessManager es el de un único proceso. Sin `exec`, matar a bash
# dejaría al `sleep` hijo vivo y huérfano, y el test de "no quedan huérfanos"
# estaría midiendo a este script y no a ProcessManager.
quedarse_vivo() {
    exec sleep infinity
}

case "$FAKE_KOBOLDCPP_ESCENARIO" in
    ok)
        # Carga normal: el log real completo (~1000 líneas) y queda sirviendo.
        reproducir_log carga-ok.log
        quedarse_vivo
        ;;

    falla-vram)
        # Carga que no cabe en la VRAM: el log real, y termina con el mismo
        # código de salida que el original.
        reproducir_log carga-falla-vram.log
        exit 3
        ;;

    falla-carga)
        # El modelo no carga, pero SIN error de memoria (archivo corrupto, ruta
        # mala). No hay captura real de este caso; es la línea de fallo sola. Va
        # por stderr (`>&2`) para comprobar de paso que stderr también se lee.
        echo "Load Text Model OK: False" >&2
        exit 3
        ;;

    sale-temprano)
        # Termina antes de estar listo sin ninguna pista reconocible.
        echo "algo salió mal y no digo qué"
        exit 7
        ;;

    linea-partida)
        # La línea de "listo" llega en DOS escrituras separadas por una pausa,
        # que es justo lo que el buffer de líneas de ProcessManager debe
        # soportar. `printf` sin "\n" escribe sin terminar la línea.
        printf 'Starting Kobold API on po'
        sleep 0.3
        printf 'rt %s at http://localhost:%s/api/\n' "$PUERTO" "$PUERTO"
        quedarse_vivo
        ;;

    ignora-sigterm)
        # Un proceso que NO obedece al cierre amable: obliga a ProcessManager a
        # usar su plan B (SIGKILL). `trap '' TERM` hace que SIGTERM se ignore, y
        # esa decisión la hereda el `sleep` que reemplaza a bash. Va ANTES de
        # imprimir la línea de listo: si fuera después, el test podría mandar la
        # señal en el hueco entre las dos cosas.
        trap '' TERM
        echo "Starting Kobold API on port $PUERTO at http://localhost:$PUERTO/api/"
        quedarse_vivo
        ;;

    *)
        echo "fake-koboldcpp: FAKE_KOBOLDCPP_ESCENARIO desconocido: '$FAKE_KOBOLDCPP_ESCENARIO'" >&2
        exit 64
        ;;
esac
