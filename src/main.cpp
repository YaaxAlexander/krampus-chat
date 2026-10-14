// main.cpp — punto de entrada de Krampus.
//
// Única responsabilidad de este archivo: crear la aplicación Qt, cargar la
// ventana QML y arrancar el bucle de eventos. Nada de lógica aquí; la lógica irá
// en las clases de src/backend/.

// `#include` es el "import" de C++, pero mucho más tosco: antes de compilar, el
// preprocesador copia y pega literalmente el contenido de ese archivo aquí. Lo
// que se incluye es un "header": un archivo que solo DECLARA qué clases y
// funciones existen (nombres y tipos), sin su código. El código ya compilado
// vive en las librerías de Qt, y se une al nuestro al enlazar (ver
// target_link_libraries en CMakeLists.txt).
//
// Se incluye solo lo que este archivo usa, un header por clase.
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

// Lista de fugas de memoria que LeakSanitizer debe callarse.
//
// El build de Debug lleva AddressSanitizer, que al salir del programa revisa
// qué memoria quedó sin liberar e imprime un reporte. En Wayland, en cuanto se
// usa el teclado, ese reporte se llena con unos 160 KB que NO son nuestros: la
// librería del mapa de teclado (libxkbcommon), llamada por el plugin Wayland de
// Qt, no libera sus datos al cerrar. Es inofensivo (el sistema recupera toda la
// memoria cuando el proceso termina), pero ese ruido taparía una fuga real de
// Krampus, que es justo lo que queremos ver.
//
// Cómo funciona: LeakSanitizer busca en el programa una función con este nombre
// exacto y, si existe, la llama para pedir la lista. Cada línea `leak:TEXTO`
// silencia las fugas cuyo rastro de llamadas pase por algo que contenga TEXTO.
// Krampus nunca llama a libxkbcommon directamente, así que esto no puede
// esconder un error nuestro. Verificado el 2026-10-12: los 19 bloques del
// reporte pasaban todos por esa librería.
//
// Alternativa descartada: un archivo de supresiones aparte. Hay que pasarlo con
// la variable de entorno LSAN_OPTIONS en cada ejecución, y es fácil olvidarla.
//
// `extern "C"` le dice al compilador que no "decore" el nombre de la función.
// C++ añade al nombre información de los tipos de los parámetros (para permitir
// varias funciones con el mismo nombre); con eso LeakSanitizer, que la busca
// por su nombre a secas, no la encontraría.
//
// `const char*` = puntero a un texto estilo C que prometemos no modificar. El
// texto entre comillas vive dentro del ejecutable durante todo el programa, así
// que devolver su dirección es seguro: nadie tiene que liberarlo.
//
// En un build Release (sin sanitizers) nadie llama a esta función; sobra pero
// no estorba, y así evitamos compilación condicional.
extern "C" const char* __lsan_default_suppressions()
{
    return "leak:libxkbcommon.so\n";
}

// En Python un script se ejecuta de arriba a abajo. En C++ no: el programa
// SIEMPRE empieza en una función llamada `main`, y el código suelto fuera de
// funciones no está permitido.
//
// `int main(...)`: en C++ toda función declara qué tipo devuelve. main devuelve
// un entero, que es el código de salida del proceso (lo que la shell ve en `$?`):
// 0 = todo bien, cualquier otra cosa = error.
//
// `argc` y `argv` son los argumentos de línea de comandos, el equivalente de
// `sys.argv`:
//   argc ("argument count")  = cuántos son.
//   argv ("argument vector") = los textos. argv[0] es el nombre del programa.
//
// `char* argv[]` se lee "arreglo de punteros a char". Un puntero es una variable
// que guarda la DIRECCIÓN de memoria donde está un dato, no el dato. C++ heredó
// de C el no tener un tipo string básico: un texto es un puntero al primer
// carácter y se lee hasta encontrar un byte 0. Por eso un arreglo en C no sabe
// su propio tamaño y hay que pasar `argc` aparte. En el resto del proyecto se
// usa QString, que sí sabe su largo y maneja Unicode; esta firma es la única
// que no se puede elegir, porque la fija el lenguaje.
int main(int argc, char* argv[])
{
    // El objeto "aplicación": inicializa Qt, se conecta al escritorio (Wayland)
    // y contiene el bucle de eventos. Tiene que existir antes que cualquier
    // otra cosa de Qt, y solo puede haber uno.
    //
    // Hay tres clases posibles y la elección importa:
    //   QCoreApplication : sin interfaz gráfica (herramientas de consola).
    //   QGuiApplication  : interfaz gráfica con Qt Quick / QML.   <- esta
    //   QApplication     : interfaz gráfica con QtWidgets.
    // Usamos QGuiApplication porque el proyecto es QML puro; QApplication
    // arrastraría toda la librería QtWidgets, que decidimos no usar.
    //
    // Sobre la sintaxis: `QGuiApplication app(argc, argv);` CREA el objeto aquí
    // mismo, llamando a su constructor (su `__init__`) con esos argumentos. En
    // Python sería `app = QGuiApplication(argc, argv)`.
    //
    // Y sobre quién lo borra (ownership): `app` es una variable local, vive "en
    // el stack". C++ la destruye sola al llegar a la `}` que cierra main,
    // siempre y en un momento exacto, sin recolector de basura. Por eso aquí no
    // hay ningún `new` ni `delete`.
    //
    // Se le pasan argc y argv porque Qt entiende opciones propias (por ejemplo
    // `-platform offscreen`) y las saca de la lista.
    QGuiApplication app(argc, argv);

    // Nombre de la aplicación y de la organización. No son decorativos: Qt los
    // usa para decidir DÓNDE guardar la configuración. Con ambos en "krampus",
    // QSettings (la clase de Qt para guardar preferencias) escribe en
    // ~/.config/krampus/krampus.conf, que es la carpeta que pide CLAUDE.md.
    // Se ponen ya, aunque todavía no se guarda nada, porque cambiarlos más
    // adelante mueve la ruta y deja huérfana la config que el usuario ya tenía.
    //
    // OJO para cuando toque escribir la config: QStandardPaths::AppConfigLocation
    // concatena organización + aplicación y daría ~/.config/krampus/krampus/
    // (un nivel de más). Habrá que decidir entonces cuál de los dos mecanismos
    // usar.
    //
    // `QCoreApplication::setApplicationName(...)`: los `::` significan "la
    // función que pertenece a esta clase". Es una función estática: se llama
    // sobre la clase, no sobre un objeto (como un @staticmethod). Se llama sobre
    // QCoreApplication, aunque nuestro objeto sea un QGuiApplication, porque es
    // ahí donde está definida; QGuiApplication la hereda.
    QCoreApplication::setApplicationName("krampus");
    QCoreApplication::setOrganizationName("krampus");

    // El motor de QML: lee los .qml, crea los objetos que describen y los mantiene
    // vivos. También es una variable local, así que se destruye sola al salir de
    // main. C++ destruye las variables locales en orden INVERSO al de creación:
    // primero `engine` (y con él la ventana), después `app`. Es justo el orden
    // correcto, porque la ventana necesita que la aplicación siga existiendo
    // mientras se cierra. Por eso `app` se declara primero.
    QQmlApplicationEngine engine;

    // Carga el componente `Main` (el archivo Main.qml) del módulo QML `Krampus`.
    // Ambos nombres salen de `qt_add_qml_module` en CMakeLists.txt. El .qml no
    // se lee del disco: va empaquetado dentro del ejecutable.
    //
    // Cargamos por nombre de módulo y no por URL (la forma antigua era
    // `engine.load(QUrl("qrc:/..."))`) porque así no hay una ruta escrita a mano
    // que se rompa en silencio si el archivo se mueve de carpeta.
    engine.loadFromModule("Krampus", "Main");

    // Si el QML tiene un error (de sintaxis, un import que no existe...),
    // loadFromModule NO lanza una excepción ni aborta: imprime el error y sigue
    // como si nada, sin haber creado ninguna ventana. Sin esta comprobación el
    // programa se quedaría corriendo para siempre sin mostrar nada.
    //
    // rootObjects() devuelve la lista de objetos raíz que el motor logró crear
    // (nuestra ventana). Lista vacía = no se creó nada = la carga falló.
    //
    // Alternativa que aparece en la documentación de Qt: conectarse a la signal
    // `objectCreationFailed` del motor. Hace lo mismo, pero necesita signals,
    // una lambda y un tipo de conexión especial; para una carga que ocurre una
    // sola vez, preguntar directamente después es más fácil de leer.
    if (engine.rootObjects().isEmpty()) {
        // qCritical() escribe en stderr, como `print(..., file=sys.stderr)`. El
        // detalle del error (archivo, línea, causa) ya lo imprimió el motor
        // justo antes; este mensaje dice qué significa para el programa.
        qCritical() << "Krampus: no se pudo cargar la interfaz (Main.qml del módulo"
                    << "Krampus). Revisa el error de QML impreso arriba.";

        // Código de salida distinto de cero = el programa falló. Así un script o
        // la shell pueden detectarlo en vez de creer que todo salió bien.
        return 1;
    }

    // Arranca el bucle de eventos: esta llamada NO regresa hasta que la
    // aplicación termina (al cerrar la última ventana). Mientras tanto Qt espera
    // eventos (clics, teclas, datos de red, temporizadores) y los reparte. Todo
    // lo que haga el programa de aquí en adelante ocurre como reacción a un
    // evento, nunca como código que corre "de corrido" como en un script.
    //
    // Devuelve el código de salida que la aplicación decidió, y lo pasamos tal
    // cual como código de salida del proceso.
    return app.exec();
}
