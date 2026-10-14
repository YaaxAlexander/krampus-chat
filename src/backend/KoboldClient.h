// KoboldClient.h — declaración del cliente HTTP que habla con koboldcpp.
//
// Única responsabilidad: el HTTP. Manda una lista de mensajes de chat y va
// avisando de cada token que el servidor devuelve. No lanza ni vigila ningún
// proceso (eso es ProcessManager) y no sabe si el servidor lo arrancó Krampus o
// ya estaba corriendo: solo conoce una URL. Por eso el mismo cliente servirá
// para el modo "attach" de la Fase 2.
//
// Tampoco guarda la conversación: no recuerda qué se dijo antes. Quien lo usa
// le pasa el historial completo en cada llamada. Así el cliente no tiene estado
// que se pueda desincronizar con lo que se ve en pantalla, y guardar o cargar
// conversaciones (Fase 3) no tiene que tocar esta clase.
//
// Este archivo es el header: DECLARA la clase. El código de los métodos está en
// KoboldClient.cpp. (Por qué C++ parte las clases en dos archivos: ver la
// explicación larga en ProcessManager.h.)

// "No pegues este archivo dos veces en el mismo .cpp" (ver ProcessManager.h).
#pragma once

// Las comillas indican un header nuestro. Aquí hace falta el header COMPLETO de
// SseParser, y no una forward declaration, porque esta clase guarda un
// SseParser entero adentro (ver m_parser): el compilador necesita saber cuánto
// mide.
#include "SseParser.h"

#include <QObject>
#include <QString>
#include <QVariantList>
// De aquí sale el macro QML_ELEMENT (ver abajo).
#include <QtQmlIntegration/qqmlintegration.h>

// "Forward declarations": le decimos al compilador "existen estas clases" sin
// incluir sus headers. Alcanza porque en este archivo solo guardamos PUNTEROS a
// ellas (una dirección de memoria, que mide lo mismo sea cual sea la clase).
// Los headers completos se incluyen en el .cpp, donde de verdad se usan.
class QNetworkAccessManager;
class QNetworkReply;

// `: public QObject` = KoboldClient hereda de QObject (en Python:
// `class KoboldClient(QObject):`). Es lo que da signals, propiedades visibles
// desde QML y el árbol de padres e hijos de Qt.
class KoboldClient : public QObject
{
    // Q_OBJECT: macro obligatorio en toda clase con signals o propiedades. C++
    // no tiene esos conceptos; los añade un generador de código de Qt (`moc`)
    // que busca las clases marcadas así. Si falta, el enlace falla con
    // "undefined reference to vtable for KoboldClient".
    Q_OBJECT

    // QML_ELEMENT: registra la clase como un tipo de QML, para poder escribir
    // `KoboldClient { }` en Main.qml. El objeto lo crea y lo destruye el motor
    // de QML; nadie en C++ hace `new KoboldClient`. (La alternativa descartada,
    // setContextProperty, está explicada en ProcessManager.h.)
    QML_ELEMENT

    // Q_PROPERTY declara una propiedad que QML puede leer por nombre y usar en
    // bindings. READ = método que devuelve el valor; WRITE = método que lo
    // cambia (sin WRITE, QML solo puede leer); NOTIFY = signal que avisa del
    // cambio, que es lo que mantiene al día los bindings.

    // Dirección del servidor, sin ruta: "http://localhost:5001". La pone quien
    // usa el cliente; esta clase no asume ni la máquina ni el puerto.
    Q_PROPERTY(QString baseUrl READ baseUrl WRITE setBaseUrl NOTIFY baseUrlChanged)

    // Verdadero desde que se manda la conversación hasta que la respuesta termina,
    // bien o mal. La UI lo usa para no dejar mandar otro mientras tanto.
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)

    // Por qué falló la última generación, en palabras para el usuario. Vacío si
    // no falló. Se borra al empezar la siguiente.
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

// `public:` = se puede usar desde fuera de la clase.
public:
    // El constructor (el `__init__`).
    //
    // `QObject* parent` es un PUNTERO: la dirección de otro objeto, o `nullptr`
    // ("ninguno"). Si se le da un padre, el padre lo destruye al destruirse.
    // `explicit` impide que C++ use este constructor para convertir un
    // `QObject*` suelto en un KoboldClient sin que nadie lo pida.
    explicit KoboldClient(QObject* parent = nullptr);

    // El destructor: corre justo antes de que el objeto deje de existir. Aquí
    // importa por si se destruye con una respuesta a medio llegar (ver el .cpp).
    // `override` = le decimos al compilador que este destructor corresponde a
    // uno ya declarado en QObject, para que avise si la firma no coincide. Ojo:
    // los destructores no se sustituyen, se encadenan. Primero corre este y
    // después, solo, el de QObject.
    ~KoboldClient() override;

    // --- Lectura de las propiedades ---
    // El `const` del final promete que el método solo lee, no modifica al objeto.
    QString baseUrl() const;
    bool generating() const;
    QString errorMessage() const;

    // `const QString&` = referencia constante: recibe el mismo string sin
    // copiarlo y promete no modificarlo.
    void setBaseUrl(const QString& url);

    // Q_INVOKABLE hace que el método se pueda llamar desde QML.
    //
    // Manda la conversación a `<baseUrl>/v1/chat/completions` y pide la
    // respuesta en streaming. koboldcpp aplica la plantilla de chat del modelo
    // cargado (los `<|start_header_id|>` de Llama 3, por ejemplo): Krampus
    // nunca arma plantillas. No espera la respuesta: regresa enseguida, y lo
    // que venga llega por las signals de abajo. Si ya hay una generación en
    // curso no hace nada (solo avisa en el log).
    //
    // `messages` es la conversación entera, en orden. Cada elemento es un
    // "diccionario" con dos claves de texto: `role` ("user", "assistant" o
    // "system") y `content`. Si la lista está vacía o algún mensaje no tiene
    // esa forma, la generación falla en el momento, sin tocar la red.
    //
    // QVariantList = una lista de QVariant, y un QVariant es una "caja" que
    // puede guardar un valor de cualquier tipo. Es lo más cercano a una `list`
    // de Python, donde cada elemento puede ser de un tipo distinto. Es el tipo
    // en el que Qt convierte solo un arreglo de JavaScript al pasarlo desde
    // QML: `koboldClient.generate([{role: "user", content: "hola"}])` llega
    // aquí como una lista cuyo elemento es una caja con un QVariantMap (el
    // `dict` de Qt: claves de texto, valores QVariant).
    //
    // Alternativa descartada: QJsonArray, que es justo lo que se va a mandar.
    // QML también lo acepta, pero QVariantList es el tipo "de siempre" para
    // recibir arreglos de JS y no ata la interfaz con QML al formato de la red.
    Q_INVOKABLE void generate(const QVariantList& messages);

    // PENDIENTE (Fase 3): cancelar una generación (POST /api/extra/abort),
    // samplers (temp, top_p...), /api/extra/tokencount y /api/extra/perf.

// Las signals son avisos que el objeto emite sin saber quién escucha. Aquí solo
// se declaran; el cuerpo lo escribe `moc`. Se emiten con `emit nombre(...)`.
signals:
    // AVISO para quien conecte un slot a estas signals: no destruir este
    // objeto con `delete` desde dentro del slot; usar `deleteLater()`. Emitir
    // una signal es llamar a los slots en ese mismo momento, así que al volver
    // del slot el código de KoboldClient sigue ejecutándose y todavía usa sus
    // propios miembros. Con `delete` ya no existirían (uso después de liberar);
    // `deleteLater()` espera a la siguiente vuelta del bucle de eventos.
    //
    // Las tres NOTIFY de las propiedades.
    void baseUrlChanged();
    void generatingChanged();
    void errorMessageChanged();

    // Llegó un pedazo de texto. Se emite una vez por token, en orden. Los
    // nombres de los argumentos importan: son los que ve el código de QML.
    void tokenReceived(const QString& token);

    // La generación terminó bien. `finishReason` es el motivo que dio el
    // servidor: "stop" (el modelo terminó su turno) o "length" (se llegó al
    // máximo de tokens). Vacío si el servidor cerró el stream con `[DONE]` sin
    // haber dado un motivo (ver handleEvent en el .cpp).
    void generationFinished(const QString& finishReason);

    // La generación terminó mal. `message` es el mismo texto que queda en
    // `errorMessage`. Después de una generación se emite SIEMPRE una de estas
    // dos, finished o failed, y nunca las dos.
    void generationFailed(const QString& message);

// `private:` = solo el código de esta clase puede usar lo que sigue.
private:
    // "Slots": métodos a los que se conectan signals de la respuesta HTTP. Las
    // conexiones se hacen en generate() (ver `connect` en el .cpp).
    void onReadyRead();
    void onFinished();

    // Lee lo que haya llegado, lo pasa al parser SSE y atiende cada evento.
    void processAvailableData();
    // Interpreta un evento ya completo: un JSON con un pedazo de respuesta, o
    // la marca `[DONE]`.
    void handleEvent(const SseEvent& event);
    // Da la generación por terminada bien, con ese motivo.
    void finish(const QString& finishReason);

    // Suelta la respuesta HTTP en curso y baja `generating`. Todos los finales
    // de una generación, buenos o malos, pasan por aquí: es lo que garantiza
    // que `generating` no quede atascado en verdadero.
    void releaseReply();
    // Termina la generación como fallida, con ese mensaje.
    void fail(const QString& message);

    // Cambian el valor y emiten la signal solo si de verdad cambió.
    void setGenerating(bool generating);
    void setErrorMessage(const QString& message);

    // --- Variables miembro (prefijo `m_`, el `self.` de Python) ---

    QString m_baseUrl;
    QString m_errorMessage;
    // `= false`: un bool sin valor inicial contiene basura, no False.
    bool m_generating = false;

    // El objeto de Qt que hace las peticiones HTTP. Uno solo sirve para todas.
    // Se crea con `new` en el constructor con `this` como padre, así que lo
    // borra Qt cuando este KoboldClient se destruye: sin `delete` a mano y sin
    // smart pointer (los dos juntos lo borrarían dos veces).
    QNetworkAccessManager* m_networkManager = nullptr;

    // La respuesta de la generación en curso, o `nullptr` si no hay ninguna.
    // Su ownership es el caso delicado de esta clase: ver releaseReply() y el
    // destructor en el .cpp.
    QNetworkReply* m_reply = nullptr;

    // El parser SSE. NO es un puntero: es el objeto mismo, guardado dentro de
    // este. Nace y muere junto con el KoboldClient, sin `new` ni `delete`. Se
    // puede porque SseParser no es un QObject y no necesita el árbol de padres
    // de Qt; cuando se puede, esta es la forma más simple de tener un objeto.
    SseParser m_parser;
};
