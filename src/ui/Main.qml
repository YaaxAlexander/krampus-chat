// Main.qml — la ventana principal de Krampus. Por ahora solo un placeholder.
//
// QML es un lenguaje declarativo: aquí no se escriben pasos ("crea una ventana,
// luego ponle un texto"), se describe el árbol de objetos que debe existir y Qt
// lo construye. La lógica de verdad (procesos, HTTP) vivirá en C++ y se asomará
// aquí como propiedades y signals; este archivo solo describe lo que se ve.

// Los `import` de QML funcionan como los de Python: traen los tipos de un módulo.
// QtQuick trae lo básico (Item, anchors...) y QtQuick.Controls los controles ya
// hechos (ApplicationWindow, Label, botones).
import QtQuick
import QtQuick.Controls

// Usamos ApplicationWindow (de Controls) y no el `Window` básico de QtQuick
// porque ApplicationWindow pinta su fondo con el color de la paleta del sistema.
// `Window` a secas trae un fondo blanco fijo que ignora el tema: en Breeze Dark
// la app se vería como un rectángulo blanco ajeno al escritorio.
ApplicationWindow {
    // Tamaño inicial, no fijo: el usuario puede redimensionar la ventana.
    width: 900
    height: 600

    // Una ventana QML nace invisible. Sin esta línea el programa arranca, no
    // muestra nada y se queda corriendo sin que se note.
    visible: true

    title: "Krampus"

    // Label (de Controls) y no `Text` (de QtQuick) por la misma razón que arriba:
    // Label toma el color del texto de la paleta del sistema, `Text` usa negro
    // fijo, que sobre un tema oscuro no se lee. Regla del proyecto: ningún color
    // escrito a mano, para que Breeze aplique solo.
    Label {
        // `anchors` es el sistema de posicionamiento de QML: en vez de
        // coordenadas se dice "pegado a qué". `parent` es el objeto que contiene
        // a este (el área de contenido de la ventana), así que esto lo deja
        // centrado aunque se redimensione la ventana.
        anchors.centerIn: parent

        text: "Krampus — todavía no hay nada aquí"
    }
}
