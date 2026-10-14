// Main.qml — la ventana principal de Krampus. Fea a propósito: por ahora
// permite cargar un modelo, ver en qué estado está koboldcpp, detenerlo, y
// conversar con el modelo viendo cada respuesta aparecer token por token.
//
// QML es un lenguaje declarativo: aquí no se escriben pasos ("crea una ventana,
// luego ponle un texto"), se describe el árbol de objetos que debe existir y Qt
// lo construye. La lógica de verdad (procesos, HTTP) vive en C++ y se asoma aquí
// como propiedades y signals; este archivo solo describe lo que se ve.
//
// Regla de arquitectura: este archivo NUNCA lanza procesos ni hace HTTP. Solo
// habla con ProcessManager y KoboldClient: lee sus propiedades, llama a sus
// métodos y reacciona a sus signals.

// Los `import` de QML funcionan como los de Python: traen los tipos de un módulo.
// QtQuick trae lo básico (Item, anchors...), QtQuick.Controls los controles ya
// hechos (ApplicationWindow, Label, botones) y QtQuick.Layouts los contenedores
// que acomodan a sus hijos en fila o en columna.
//
// `ProcessManager` y `KoboldClient` no necesitan import: están registrados en el
// módulo `Krampus`, que es el mismo al que pertenece este archivo (ver
// CMakeLists.txt).
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

    // --- La conversación ---
    //
    // El historial de la sesión: un arreglo de JavaScript con un objeto por
    // mensaje, `{role: "user" | "assistant", content: "texto"}`, en orden. Es
    // exactamente lo que se le pasa entero a koboldClient.generate() en cada
    // envío: el cliente de C++ no recuerda nada entre llamadas, y el servidor
    // tampoco. Si el modelo "se acuerda" de lo anterior es porque se lo
    // volvemos a mandar todo cada vez.
    //
    // Vive aquí, en QML, porque por ahora es solo estado de la ventana. Se
    // pierde al cerrar Krampus: guardarlo en disco es de la Fase 3. Tampoco hay
    // mensaje de sistema todavía.
    //
    // `property var` = una propiedad que guarda cualquier valor de JavaScript.
    // Ojo: modificar el arreglo por dentro (push, pop) NO avisa a nadie, solo
    // asignarle uno nuevo lo hace. Por eso ningún binding depende de él: lo
    // que se ve en pantalla se actualiza a mano (ver renderConversation).
    property var conversation: []

    // El texto de la respuesta que está llegando, token a token. Al terminar
    // bien se guarda en `conversation` como mensaje del asistente. Hace falta
    // aparte del área de texto porque esa mezcla la conversación entera con
    // los "Tú:" / "Modelo:" de adorno.
    property string pendingReply: ""

    // El objeto de C++ que habla por HTTP con koboldcpp (ver
    // src/backend/KoboldClient.h). Mismo ownership que el de arriba.
    KoboldClient {
        id: koboldClient

        // La dirección se arma con el puerto de arriba, para que el número
        // siga escrito en un solo lugar. Es la misma máquina porque en este
        // modo a koboldcpp lo lanza Krampus; el cliente en sí acepta cualquier
        // URL (hará falta para el modo "attach" de la Fase 2).
        baseUrl: "http://localhost:" + window.koboldPort

        // Signal handler: corre cada vez que el cliente emite `tokenReceived`.
        // La forma `function(token) { }` le pone nombre al argumento de la
        // signal para poder usarlo adentro.
        onTokenReceived: function(token) {
            window.pendingReply += token
            // insert(posición, texto) con la posición del final AGREGA el
            // token a lo que ya había. La alternativa obvia,
            // `outputArea.text += token`, arma el texto completo de nuevo por
            // cada token y obliga a redibujarlo entero: con conversaciones
            // largas el trabajo crece con el cuadrado del largo.
            outputArea.insert(outputArea.length, token)
            // Mover el cursor al final hace que la vista siga al texto nuevo.
            outputArea.cursorPosition = outputArea.length
        }

        // La respuesta terminó bien: pasa al historial. Así el próximo envío
        // la incluye y la conversación sigue alternando usuario / asistente.
        // En pantalla no hay nada que hacer: ya se fue mostrando token a token.
        //
        // No se mira el motivo (`finishReason`): aunque sea "length" (cortada
        // por el máximo de tokens), lo que llegó es lo que el modelo dijo, y
        // es lo que tiene que ver en el siguiente turno.
        onGenerationFinished: {
            window.conversation.push({ role: "assistant", content: window.pendingReply })
            window.pendingReply = ""
        }

        // La generación falló (servidor caído, error HTTP, corte a mitad...).
        //
        // Decisión: el turno fallido se DESHACE entero. El mensaje del usuario
        // sale del historial y vuelve al campo de texto, listo para reenviarlo
        // con Enter; lo que hubiera llegado de la respuesta se descarta.
        //
        // Por qué: si el mensaje del usuario se quedara en el historial sin
        // respuesta, el siguiente envío mandaría dos mensajes de usuario
        // seguidos, y muchas plantillas de chat esperan que alternen. La
        // alternativa de guardar la respuesta a medias como si fuera del
        // asistente es peor: el modelo vería en el historial un texto cortado
        // como si lo hubiera dicho así. El motivo del fallo queda a la vista en
        // la etiqueta de error, debajo del campo de texto.
        //
        // También corre si generate() falla en el momento (por ejemplo, con
        // la dirección mal escrita), antes de mandar nada: el caso es el mismo.
        onGenerationFailed: {
            // El último mensaje siempre es el del usuario que se acaba de
            // mandar (sendMessage lo agrega justo antes de llamar a generate).
            // Se comprueba igual, para no borrar nunca una respuesta del
            // asistente por error. `length - 1` = el último índice.
            const lastIndex = window.conversation.length - 1
            if (lastIndex >= 0 && window.conversation[lastIndex].role === "user") {
                // pop() quita el último elemento y lo devuelve.
                const failedMessage = window.conversation.pop()
                promptField.text = failedMessage.content
            }
            window.pendingReply = ""
            window.renderConversation()
        }
    }

    // El nombre con el que se muestra cada rol en la conversación.
    function speakerLabel(role) {
        if (role === "user") {
            return "Tú: "
        }
        if (role === "assistant") {
            return "Modelo: "
        }
        // No debería pasar: el historial solo tiene esos dos roles.
        return role + ": "
    }

    // Vuelve a escribir el área de texto entera a partir del historial. Se usa
    // solo cuando el historial cambia de golpe (un turno fallido que se
    // deshace, o una conversación nueva). Mientras llega una respuesta NO se
    // usa: ahí se agrega token a token (ver onTokenReceived).
    function renderConversation() {
        // `let` declara una variable que solo existe dentro de este bloque,
        // como una variable local de Python. (La palabra vieja de JavaScript,
        // `var`, tiene reglas de alcance más raras: no se usa.)
        let text = ""
        for (let i = 0; i < window.conversation.length; i++) {
            if (i > 0) {
                text += "\n\n"
            }
            const message = window.conversation[i]
            text += window.speakerLabel(message.role) + message.content
        }
        outputArea.text = text
        outputArea.cursorPosition = outputArea.length
    }

    // Manda el mensaje escrito, junto con todo el historial. Es una función de
    // JavaScript definida dentro de la ventana; existe porque hay dos formas
    // de enviar (el botón y la tecla Enter) y las dos tienen que hacer
    // exactamente lo mismo.
    function sendMessage() {
        // Enter llega aquí aunque el botón esté deshabilitado, así que se
        // comprueba lo mismo que el botón.
        if (!sendButton.enabled) {
            return
        }
        // `const` = una variable que no se vuelve a asignar.
        const text = promptField.text
        window.conversation.push({ role: "user", content: text })
        // Se vacía ANTES de llamar a generate(): si la generación falla en el
        // momento, onGenerationFailed devuelve el texto al campo, y ese
        // retorno no debe pisarse después.
        promptField.clear()

        // En pantalla: el mensaje del usuario y el comienzo de la respuesta,
        // que irá creciendo con cada token.
        if (outputArea.length > 0) {
            outputArea.insert(outputArea.length, "\n\n")
        }
        outputArea.insert(outputArea.length,
                          window.speakerLabel("user") + text + "\n\n" + window.speakerLabel("assistant"))
        outputArea.cursorPosition = outputArea.length

        window.pendingReply = ""
        // El arreglo de JavaScript llega a C++ como una QVariantList (ver
        // KoboldClient.h).
        koboldClient.generate(window.conversation)
    }

    // Empieza de cero: olvida el historial y limpia la pantalla. El modelo
    // cargado sigue cargado; solo se olvida lo conversado.
    function newConversation() {
        // Aquí sí se asigna un arreglo nuevo, en vez de vaciar el viejo.
        window.conversation = []
        window.pendingReply = ""
        outputArea.clear()
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

        // Fila del mensaje: texto + botones.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: promptField
                Layout.fillWidth: true
                placeholderText: "Escribe un mensaje"
                // Mientras se genera no se edita. Además, si la generación
                // falla, el mensaje vuelve a este campo (ver
                // onGenerationFailed), y no debe encontrarse con otro texto
                // escrito mientras tanto.
                enabled: !koboldClient.generating
                // `accepted` se emite al apretar Enter dentro del campo.
                onAccepted: window.sendMessage()
            }

            Button {
                id: sendButton
                text: koboldClient.generating ? "Generando…" : "Enviar"
                // Solo se puede enviar con el servidor listo, sin otra
                // generación en curso y con algo escrito.
                enabled: processManager.state === ProcessManager.Ready
                         && !koboldClient.generating
                         && promptField.text.length > 0
                onClicked: window.sendMessage()
            }

            Button {
                text: "Nueva conversación"
                // Deshabilitado mientras genera: borrar el historial con una
                // respuesta en camino dejaría esa respuesta llegando a una
                // conversación que ya no existe.
                enabled: !koboldClient.generating
                onClicked: window.newConversation()
            }
        }

        // El error del cliente HTTP, si la última generación falló. En negrita
        // y no en rojo por la misma razón que el error de arriba.
        Label {
            Layout.fillWidth: true
            visible: koboldClient.errorMessage.length > 0
            text: koboldClient.errorMessage
            font.bold: true
            wrapMode: Text.Wrap
        }

        Label {
            text: "Conversación:"
        }

        // La conversación entera, en texto plano: "Tú: ..." / "Modelo: ...".
        // Los tokens de la respuesta en curso se agregan al final desde el
        // handler `onTokenReceived` de más arriba. Burbujas y markdown son de
        // la Fase 4.
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Hay dos áreas que quieren todo el alto sobrante: esta y la del
            // log. El "stretch factor" dice cómo repartirlo: con 2 aquí y 1
            // allá, esta se queda con dos tercios. Sin esto el reparto depende
            // de cuánto texto tenga cada una y las áreas cambian de tamaño
            // mientras llegan los tokens.
            Layout.verticalStretchFactor: 2

            TextArea {
                id: outputArea
                readOnly: true
                wrapMode: TextEdit.Wrap
                // Sin esto el área interpretaría como formato un texto que
                // parezca HTML; los mensajes se muestran literales.
                textFormat: TextEdit.PlainText
            }
        }

        Label {
            text: "Salida de koboldcpp (últimas líneas):"
        }

        // Las últimas líneas que imprimió koboldcpp, para depurar. ScrollView
        // le pone barras de desplazamiento a lo que contiene.
        ScrollView {
            Layout.fillWidth: true
            // fillHeight: este elemento se reparte el alto que sobre en la
            // columna con el área de la respuesta (ver el stretch factor allá).
            Layout.fillHeight: true
            Layout.verticalStretchFactor: 1

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
