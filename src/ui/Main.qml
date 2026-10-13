// Main.qml — la ventana principal de Krampus. Fea a propósito: por ahora solo
// permite cargar un modelo, ver en qué estado está koboldcpp y detenerlo.
//
// QML es un lenguaje declarativo: aquí no se escriben pasos ("crea una ventana,
// luego ponle un texto"), se describe el árbol de objetos que debe existir y Qt
// lo construye. La lógica de verdad (procesos, HTTP) vive en C++ y se asoma aquí
// como propiedades y signals; este archivo solo describe lo que se ve.
//
// Regla de arquitectura: este archivo NUNCA lanza procesos. Solo habla con
// ProcessManager: lee sus propiedades y llama a start() / stop().

// Los `import` de QML funcionan como los de Python: traen los tipos de un módulo.
// QtQuick trae lo básico (Item, anchors...), QtQuick.Controls los controles ya
// hechos (ApplicationWindow, Label, botones) y QtQuick.Layouts los contenedores
// que acomodan a sus hijos en fila o en columna.
//
// `ProcessManager` no necesita import: está registrado en el módulo `Krampus`,
// que es el mismo al que pertenece este archivo (ver CMakeLists.txt).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Usamos ApplicationWindow (de Controls) y no el `Window` básico de QtQuick
// porque ApplicationWindow pinta su fondo con el color de la paleta del sistema.
// `Window` a secas trae un fondo blanco fijo que ignora el tema: en Breeze Dark
// la app se vería como un rectángulo blanco ajeno al escritorio.
ApplicationWindow {
    // `id` es el nombre con el que el resto del archivo se refiere a este
    // objeto. No es una propiedad normal: solo existe dentro de este .qml.
    id: window

    // Tamaño inicial, no fijo: el usuario puede redimensionar la ventana.
    width: 900
    height: 600

    // Una ventana QML nace invisible. Sin esta línea el programa arranca, no
    // muestra nada y se queda corriendo sin que se note.
    visible: true

    title: "Krampus"

    // El puerto de koboldcpp, definido en este único lugar. Fijo por ahora; el
    // control para cambiarlo es de la Fase 3. `readonly property` = una
    // constante con nombre; 5001 es el puerto por defecto de koboldcpp.
    readonly property int koboldPort: 5001

    // El objeto de C++ que maneja el proceso de koboldcpp (ver
    // src/backend/ProcessManager.h). Escribirlo aquí CREA uno, igual que
    // escribir `Label { }` crea una etiqueta.
    //
    // Ownership: lo crea el motor de QML como hijo de la ventana y lo destruye
    // cuando la ventana se destruye, al cerrar Krampus. Su destructor en C++ es
    // el que se encarga de que koboldcpp muera en ese momento.
    ProcessManager {
        id: processManager
    }

    // ColumnLayout apila a sus hijos de arriba hacia abajo. La alternativa,
    // posicionar cada cosa con `anchors` respecto a la anterior, se vuelve
    // difícil de leer en cuanto hay más de dos o tres elementos.
    ColumnLayout {
        // `anchors` es el sistema de posicionamiento de QML: en vez de
        // coordenadas se dice "pegado a qué". `parent` es el objeto que contiene
        // a este (el área de contenido de la ventana). fill = ocuparlo entero,
        // también cuando se redimensiona la ventana.
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // Fila de arriba: ruta del modelo + botón.
        RowLayout {
            // Dentro de un Layout, el tamaño de cada hijo se pide con las
            // propiedades `Layout.*`. fillWidth = "estírate a todo el ancho
            // disponible".
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: modelPathField
                Layout.fillWidth: true

                // Ruta inicial para no tener que escribirla en cada prueba. Es
                // relativa: se resuelve contra la carpeta desde la que se lanzó
                // Krampus, así que solo sirve lanzándolo desde la raíz del repo.
                // El selector de archivos es de la Fase 3.
                text: "models/llama3/Llama-3.2-3B-Instruct-Q4_K_M.gguf"
                placeholderText: "Ruta del modelo .gguf"

                // Esto es un "binding": no una asignación que se hace una vez,
                // sino una fórmula que QML mantiene al día. Cada vez que
                // `processManager.state` cambia (C++ emite stateChanged), QML
                // vuelve a evaluar la expresión sola.
                //
                // Solo se puede editar la ruta cuando no hay nada cargado ni
                // cargando. `ProcessManager.NotRunning` (con mayúscula: el
                // tipo) es un valor del enum State; `processManager.state` (con
                // minúscula: nuestro objeto) es el estado actual.
                enabled: processManager.state === ProcessManager.NotRunning
                         || processManager.state === ProcessManager.Failed
            }

            // Un solo botón que cambia de papel según el estado, en vez de dos
            // botones que se muestran y se ocultan: así nunca hay un momento en
            // que se vean los dos o ninguno.
            Button {
                // Un binding puede ser un bloque `{ }` con varias líneas: el
                // valor de la propiedad es lo que devuelva el `return`. Dentro
                // del bloque el lenguaje es JavaScript.
                text: {
                    if (processManager.state === ProcessManager.Loading
                            || processManager.state === ProcessManager.Ready) {
                        return "Detener"
                    }
                    if (processManager.state === ProcessManager.Stopping) {
                        return "Deteniendo…"
                    }
                    return "Cargar"
                }

                // Deshabilitado mientras el proceso se está cerrando (no hay
                // nada que hacer más que esperar) y cuando no hay ruta escrita.
                enabled: {
                    if (processManager.state === ProcessManager.Stopping) {
                        return false
                    }
                    if (processManager.state === ProcessManager.Loading
                            || processManager.state === ProcessManager.Ready) {
                        return true
                    }
                    return modelPathField.text.length > 0
                }

                // onClicked es un "signal handler": código que corre cuando el
                // botón emite su signal `clicked`. Es el equivalente en QML del
                // `connect` de C++.
                onClicked: {
                    if (processManager.state === ProcessManager.Loading
                            || processManager.state === ProcessManager.Ready) {
                        processManager.stop()
                    } else {
                        processManager.start(modelPathField.text, window.koboldPort)
                    }
                }
            }
        }

        // Fila de estado: indicador de actividad + texto.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // Indicador INDETERMINADO (da vueltas, sin porcentaje) y no una
            // barra de progreso: koboldcpp no imprime ningún porcentaje mientras
            // carga, así que una barra tendría que inventárselo (ver
            // NOTES-backend.md).
            BusyIndicator {
                // `running` lo hace girar; `visible` hace que además no ocupe
                // lugar en la fila cuando no hay nada en curso.
                running: processManager.state === ProcessManager.Loading
                         || processManager.state === ProcessManager.Stopping
                visible: running
                // Por defecto es bastante grande; lo dejamos del alto del texto.
                Layout.preferredHeight: statusLabel.implicitHeight * 2
                Layout.preferredWidth: Layout.preferredHeight
            }

            // Label (de Controls) y no `Text` (de QtQuick): Label toma el color
            // del texto de la paleta del sistema, `Text` usa negro fijo, que
            // sobre un tema oscuro no se lee. Regla del proyecto: ningún color
            // escrito a mano, para que Breeze aplique solo.
            Label {
                id: statusLabel
                Layout.fillWidth: true

                text: {
                    // `switch` compara un valor contra varios casos; como una
                    // cadena de if/elif sobre la misma variable.
                    switch (processManager.state) {
                    case ProcessManager.NotRunning:
                        return "Sin modelo cargado."
                    case ProcessManager.Loading:
                        return "Cargando modelo…"
                    case ProcessManager.Ready:
                        return "Listo. koboldcpp escucha en el puerto " + window.koboldPort + "."
                    case ProcessManager.Failed:
                        return "Falló."
                    case ProcessManager.Stopping:
                        return "Deteniendo koboldcpp…"
                    }
                    return ""
                }
            }
        }

        // El mensaje de error. Solo existe visualmente cuando hay un fallo.
        // Va en negrita y no en rojo porque la paleta de Qt no tiene un color
        // de "error", y escribir un rojo a mano rompería la regla de no
        // hardcodear colores.
        Label {
            Layout.fillWidth: true
            visible: processManager.state === ProcessManager.Failed
            text: processManager.errorMessage
            font.bold: true
            // Sin esto un mensaje largo se sale de la ventana en vez de partirse
            // en varias líneas.
            wrapMode: Text.Wrap
        }

        Label {
            text: "Salida de koboldcpp (últimas líneas):"
        }

        // Las últimas líneas que imprimió koboldcpp, para depurar. ScrollView
        // le pone barras de desplazamiento a lo que contiene.
        ScrollView {
            Layout.fillWidth: true
            // fillHeight: este es el elemento que se queda con todo el alto que
            // sobre en la columna.
            Layout.fillHeight: true

            // TextArea de solo lectura y no un Label: así el texto se puede
            // seleccionar y copiar, que es lo que uno quiere hacer con un error.
            TextArea {
                readOnly: true
                text: processManager.recentOutput
                // Fuente de ancho fijo de la que tenga configurada el sistema:
                // la salida de koboldcpp está alineada en columnas.
                font.family: "monospace"
                // El log tiene líneas de miles de caracteres; se parten en vez
                // de obligar a desplazarse hacia la derecha.
                wrapMode: TextEdit.Wrap

                // Cada vez que llega salida nueva, mover el cursor al final para
                // que la vista siga a las últimas líneas, que son las que
                // importan. onTextChanged corre cuando cambia la propiedad `text`.
                onTextChanged: cursorPosition = length
            }
        }
    }
}
