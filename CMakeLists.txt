# CMakeLists.txt — la "receta" de compilación de Krampus.
#
# En Python no existe este paso: escribes un .py y el intérprete lo ejecuta. En
# C++ hay que *compilar* cada .cpp a código máquina y luego *enlazar* (link) esos
# pedazos con las librerías (Qt) para producir un ejecutable. CMake no compila
# nada por sí mismo: lee este archivo y genera las instrucciones para una
# herramienta que sí lo hace (aquí Ninja, que a su vez llama a g++).
#
# Uso normal, desde la raíz del repo:
#   cmake -S . -B build -G Ninja     (configurar: lee este archivo, crea build/)
#   cmake --build build              (compilar y enlazar)
#   ./build/krampus                  (ejecutar)

# Versión mínima de CMake que aceptamos. No es burocracia: esta línea además fija
# el comportamiento ("policies") de CMake al de esa versión, para que el mismo
# archivo se comporte igual en versiones más nuevas. 3.25 es lo que exige el Qt 6
# instalado (ver QT_SUPPORTED_MIN_CMAKE_VERSION_FOR_USING_QT en
# /usr/lib/cmake/Qt6/Qt6ConfigExtras.cmake).
cmake_minimum_required(VERSION 3.25)

# Tipo de build por defecto: Debug.
#
# CMake distingue "Debug" (sin optimizar, con símbolos para gdb) de "Release"
# (optimizado). Si no se le pasa nada, CMAKE_BUILD_TYPE queda VACÍO, que no es
# ninguno de los dos: ni símbolos de debug ni optimización, y encima nuestro
# AddressSanitizer de más abajo no se activaría. Para un proyecto en desarrollo
# eso es una trampa, así que si el usuario no eligió, elegimos Debug.
# Para un build optimizado: cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
#
# Lo ponemos arriba por claridad; no es obligatorio que vaya antes de project().
# CMake consulta CMAKE_BUILD_TYPE al final, al generar los archivos de Ninja,
# para elegir entre los flags de Debug y los de Release.
#
# `CACHE STRING ... FORCE` guarda el valor en build/CMakeCache.txt, que es donde
# CMake recuerda las opciones entre una ejecución y la siguiente.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Tipo de build: Debug o Release" FORCE)
endif()

# Nombre del proyecto y lenguaje. `LANGUAGES CXX` ("CXX" = C++) evita que CMake
# pierda tiempo buscando también un compilador de C que no usamos.
project(krampus VERSION 0.1.0 LANGUAGES CXX)

# --- Estándar de C++ ---------------------------------------------------------
# C++ tiene "versiones" del lenguaje (C++11, 14, 17, 20...), parecido a Python
# 3.8 vs 3.12. El proyecto usa C++17 (decidido en CLAUDE.md).
set(CMAKE_CXX_STANDARD 17)
# REQUIRED ON: si el compilador no soporta C++17, falla aquí con un error claro.
# Sin esto CMake baja en silencio a un estándar más viejo y los errores aparecen
# después, lejos de la causa.
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# EXTENSIONS OFF: pide `-std=c++17` en vez de `-std=gnu++17`. La variante "gnu"
# permite extensiones propias de GCC que no son C++ estándar. Las apagamos para
# que lo que se aprende aquí sea C++ de verdad y no un dialecto de un compilador.
set(CMAKE_CXX_EXTENSIONS OFF)

# Genera build/compile_commands.json: la lista exacta de comandos con los que se
# compila cada archivo. No afecta al ejecutable; lo leen los editores (clangd en
# Kate y VS Code) para autocompletar y marcar errores sabiendo dónde está Qt.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Qt ----------------------------------------------------------------------
# Busca Qt 6 instalado en el sistema. REQUIRED = si falta, detenerse aquí con un
# mensaje legible en vez de fallar más adelante con errores raros.
#   Core    : lo básico (QString, QObject, el bucle de eventos).
#   Quick   : Qt Quick, el motor que dibuja la UI escrita en QML.
#   Network : cliente HTTP. TODAVÍA no se usa en el código; se pide ya porque el
#             plan de la Fase 1 lo necesita para KoboldClient
#             (QNetworkAccessManager), y así un Qt sin Network instalado se
#             detecta ahora, en el scaffold, y no a mitad de escribir el cliente.
find_package(Qt6 REQUIRED COMPONENTS Core Quick Network)

# Configuración estándar que Qt recomienda para todo proyecto. Lo que más nos
# importa es que activa AUTOMOC: Qt extiende C++ con cosas que el lenguaje no
# tiene (signals/slots, propiedades visibles desde QML) y para eso corre un
# generador de código llamado `moc` sobre las clases marcadas con Q_OBJECT.
# AUTOMOC hace que CMake lo corra solo; sin esto habría que invocarlo a mano.
#
# `REQUIRES 6.8` hace dos cosas: falla si el Qt instalado es más viejo que 6.8, y
# activa los comportamientos modernos de los módulos QML (por ejemplo, que los
# archivos .qml se empaqueten bajo el prefijo `/qt/qml/`, que es donde el motor
# los busca por defecto). Sin esto Qt usa el comportamiento antiguo y avisa con
# warnings. 6.8 es una versión LTS; Arch trae una más nueva.
qt_standard_project_setup(REQUIRES 6.8)

# --- El ejecutable -----------------------------------------------------------
# Declara el ejecutable `krampus` y los .cpp que lo forman. Usamos
# `qt_add_executable` en vez del `add_executable` normal de CMake porque el de Qt
# añade los pasos extra que una app Qt necesita (entre otros, registrar plugins).
#
# Solo se listan los .cpp: los headers (.h) no se compilan por separado, se
# "pegan" dentro de cada .cpp que hace #include de ellos.
qt_add_executable(krampus
    src/main.cpp
)

# --- El módulo QML -----------------------------------------------------------
# Empaqueta los archivos .qml DENTRO del ejecutable, como un módulo QML llamado
# `Krampus` (el URI es el nombre con el que se importa: `import Krampus`).
#
# Por qué dentro del ejecutable y no leyéndolos del disco en runtime: así el
# binario es autocontenido (no hay una ruta a los .qml que se rompa al
# instalarlo), y de paso Qt compila el QML por adelantado y lo revisa al
# compilar, con lo que varios errores salen en el build y no al abrir la ventana.
#
# Esta es la forma moderna (Qt 6.2+). La alternativa antigua era escribir a mano
# un archivo .qrc con la lista de recursos y cargar `qrc:/main.qml` por URL;
# funciona, pero es un archivo más que mantener y no da la revisión en build.
#
# Cada .qml nuevo hay que añadirlo a QML_FILES; no se detectan solos.
qt_add_qml_module(krampus
    URI Krampus
    VERSION 1.0
    QML_FILES
        src/ui/Main.qml
)

# --- Librerías ---------------------------------------------------------------
# Enlaza el ejecutable con las librerías de Qt. Los nombres `Qt6::Core` etc. son
# "targets importados": además de la librería en sí, traen consigo las rutas de
# sus headers, así que no hay que configurar rutas de #include a mano.
#
# PRIVATE = estas dependencias son solo para compilar `krampus`. Las otras
# opciones (PUBLIC / INTERFACE) solo importan cuando lo que se construye es una
# librería que otros van a usar, que no es nuestro caso.
target_link_libraries(krampus PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::Network
)

# --- Warnings ----------------------------------------------------------------
# -Wall -Wextra activan la mayoría de los avisos del compilador (variables sin
# usar, comparaciones sospechosas, etc.). Van SIEMPRE, también en Release: un
# warning en C++ suele ser un bug real que en Python habría sido una excepción
# en runtime. La meta es compilar con cero warnings.
target_compile_options(krampus PRIVATE -Wall -Wextra)

# --- AddressSanitizer (solo en Debug) ----------------------------------------
# En C++ la memoria se maneja a mano, y usarla mal (leer algo ya liberado,
# salirse de un arreglo) NO da una excepción como en Python: da un segfault sin
# explicación o, peor, datos corruptos en silencio. AddressSanitizer (ASan)
# instrumenta el programa para detectar esos errores en el momento en que
# ocurren y explicar qué pasó, en qué línea, y dónde se reservó y liberó esa
# memoria. Al salir también reporta fugas (memoria que nunca se liberó).
#
# Cuesta ~2x de velocidad y de memoria, por eso solo va en Debug.
#
#   -fsanitize=address       : activa ASan.
#   -fno-omit-frame-pointer  : conserva la información que permite reconstruir la
#                              cadena de llamadas, para que los reportes de ASan
#                              (y los `bt` de gdb) muestren stack traces completos.
#
# `$<$<CONFIG:Debug>:...>` es una "generator expression": CMake la sustituye por
# lo que va después de los dos puntos si el build es Debug, y por nada si no.
# Es el `if` de CMake para flags que dependen del tipo de build.
#
# El flag va DOS veces a propósito: al compilar (para instrumentar el código) y
# al enlazar (para añadir la librería de ASan, libasan). Si falta el segundo, el
# enlace falla con cientos de "undefined reference to __asan_...".
#
# Los símbolos de debug (-g, lo que deja a gdb mostrar nombres de funciones y
# números de línea) no se añaden aquí porque CMake ya los pone él solo en todo
# build Debug.
target_compile_options(krampus PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)
target_link_options(krampus PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address>
)
