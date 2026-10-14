// KoboldClient.cpp — el código de los métodos declarados en KoboldClient.h.
//
// La forma de la petición y de la respuesta sale de mediciones reales contra
// koboldcpp: ver NOTES-backend.md ("Stream SSE") y logs/sse-trace.txt.

// Un .cpp siempre incluye su propio header.
#include "KoboldClient.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

// Constantes de este archivo. `const` = no se pueden modificar. Van aquí y no
// en el header porque nadie fuera de este .cpp las necesita.

// La ruta del endpoint de generación con streaming (ver CLAUDE.md, "Endpoints
// de koboldcpp"). Se le antepone la URL base.
const QString GENERATE_STREAM_PATH = "/api/extra/generate/stream";

// Máximo de tokens que se le pide generar al servidor por cada prompt. Fijo por
// ahora y definido solo aquí; los controles de generación son de la Fase 3.
// 200 tokens son unas 150 palabras: alcanza para ver el streaming un buen rato
// (unos 11 s a los ~18 tokens por segundo medidos) sin que una prueba se
// eternice, porque todavía no hay botón para cancelar.
const int MAX_LENGTH_TOKENS = 200;

// Cuántos bytes del cuerpo de una respuesta de error se copian al mensaje. El
// cuerpo suele explicar el problema (koboldcpp manda un JSON corto con el
// motivo), pero no se quiere volcar una página entera en la ventana.
const int MAX_ERROR_BODY_BYTES = 300;

// `KoboldClient::KoboldClient` = el constructor de la clase KoboldClient. Lo
// que va tras los dos puntos es la "lista de inicialización": `QObject(parent)`
// llama al constructor de la clase base, como `super().__init__(parent)`.
KoboldClient::KoboldClient(QObject* parent)
    : QObject(parent)
{
    // `new` crea el objeto en el "heap": memoria que no se libera sola al salir
    // de esta función. Lo borrará Qt, porque le pasamos `this` (este mismo
    // KoboldClient, el `self` de Python) como padre.
    m_networkManager = new QNetworkAccessManager(this);
}

// El destructor. Corre al cerrar Krampus (el motor de QML destruye la ventana y
// con ella este objeto), y puede haber una respuesta a medio llegar.
KoboldClient::~KoboldClient()
{
    // `nullptr` = no hay ninguna generación en curso: nada que hacer.
    if (m_reply == nullptr) {
        return;
    }

    // Hay una respuesta viva. Primero se cortan sus conexiones hacia este
    // objeto, y recién después se la cancela.
    //
    // Por qué en ese orden: abort() hace que la respuesta emita `finished` ahí
    // mismo. Si siguiera conectada, correría onFinished(), que emitiría
    // `generationFailed` y `generatingChanged` hacia una interfaz QML que está
    // a medio destruir, y esta intentaría re-evaluar sus bindings contra
    // objetos que quizá ya no existen. Ese tipo de bug es un "use after free"
    // (usar memoria ya liberada). Durante la destrucción ya no le interesa a
    // nadie cómo terminó la generación: este destructor no emite nada.
    //
    // ¿Y sin estas dos líneas? Hoy también funcionaría (está probado): el
    // destructor de QObject corta las conexiones antes de borrar a los hijos, y
    // al borrarse la respuesta la conexión de red se cierra igual. Se hace a
    // mano para que la limpieza esté escrita aquí, a la vista y en un orden
    // conocido, en vez de depender de ese detalle interno de Qt.
    //
    // `->` es el `.` para punteros: "el método disconnect del objeto al que
    // apunta m_reply".
    m_reply->disconnect(this);
    // abort() cierra la conexión con el servidor.
    m_reply->abort();

    // Aquí NO hay `delete m_reply`. La respuesta es hija de m_networkManager
    // (Qt la crea así), que a su vez es hijo de `this`: al terminar este
    // destructor, el de QObject borra al manager, y el manager a la respuesta.
    //
    // PENDIENTE (Fase 3): esto corta la conexión pero no le pide a koboldcpp
    // que deje de generar (eso es POST /api/extra/abort). En modo "managed" da
    // igual, porque al cerrar Krampus el proceso muere de todos modos.
}

QString KoboldClient::baseUrl() const
{
    return m_baseUrl;
}

bool KoboldClient::generating() const
{
    return m_generating;
}

QString KoboldClient::errorMessage() const
{
    return m_errorMessage;
}

void KoboldClient::setBaseUrl(const QString& url)
{
    // Si no cambió no se emite la signal: es la convención de Qt para los
    // setters, y evita que QML re-evalúe bindings para nada.
    if (m_baseUrl == url) {
        return;
    }
    // Cambiarla a mitad de una generación no afecta a la que está en curso:
    // vale para la siguiente.
    m_baseUrl = url;
    emit baseUrlChanged();
}

void KoboldClient::generate(const QString& prompt)
{
    // Una generación a la vez. koboldcpp atiende de a una, y con dos respuestas
    // llegando juntas no se sabría de cuál es cada token.
    if (m_reply != nullptr) {
        qWarning() << "KoboldClient: generate() ignorado, ya hay una generación en curso.";
        return;
    }

    // El error de la generación anterior ya no aplica.
    setErrorMessage(QString());

    // Armar la URL completa. Se quitan las barras del final de la base para que
    // "http://host:5001/" y "http://host:5001" den lo mismo y no quede "//".
    QString base = m_baseUrl;
    while (base.endsWith('/')) {
        base.chop(1);
    }
    // QUrl interpreta el texto y lo separa en esquema, máquina, puerto y ruta.
    QUrl url(base + GENERATE_STREAM_PATH);
    if (base.isEmpty() || !url.isValid() || url.host().isEmpty()) {
        fail(QString("La dirección del servidor no es válida: «%1». Debe tener la "
                     "forma http://máquina:puerto.")
                 .arg(m_baseUrl));
        return;
    }

    // El cuerpo de la petición: un objeto JSON. QJsonObject es el `dict` y
    // QJsonDocument hace el trabajo de `json.dumps()`. Compact = en una sola
    // línea, sin espacios de adorno. toJson() devuelve bytes en UTF-8.
    QJsonObject body;
    body.insert("prompt", prompt);
    body.insert("max_length", MAX_LENGTH_TOKENS);
    QByteArray bodyBytes = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkRequest request(url);
    // Le dice al servidor que el cuerpo es JSON. Sin esta cabecera Qt avisa con
    // un warning y manda un tipo por defecto que no corresponde.
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // No se pone ningún tiempo límite a la petición, a propósito: antes del
    // primer token el servidor procesa el prompt entero, y con un prompt largo
    // en una GPU chica eso puede tardar mucho sin que nada esté mal.

    // Un stream nuevo: que el parser no arrastre restos del anterior.
    m_parser.reset();

    // post() NO espera la respuesta: manda la petición y regresa enseguida con
    // un objeto QNetworkReply que representa "la respuesta que va a ir
    // llegando". Lo crea Qt con `new`, como hijo de m_networkManager.
    //
    // Aunque tenga padre, la documentación de Qt pide que lo borremos nosotros
    // al terminar de usarlo: si no, cada respuesta vieja quedaría en memoria
    // hasta que se destruya el manager, o sea hasta cerrar Krampus. Eso se hace
    // en releaseReply().
    m_reply = m_networkManager->post(request, bodyBytes);

    // `connect` une una signal con un slot: "cuando ESTE objeto emita ESTA
    // signal, llama a ESTE método de ESTE otro objeto". `&QNetworkReply::readyRead`
    // es un "puntero a método": nombra al método sin llamarlo, para que Qt lo
    // llame después.
    //
    // Nada de esto corre ahora: los slots se llaman más tarde, desde el bucle de
    // eventos (el `app.exec()` de main.cpp), cada vez que la red traiga algo.
    // Por eso la ventana no se congela mientras el modelo genera.

    // readyRead: llegaron bytes nuevos de la respuesta.
    connect(m_reply, &QNetworkReply::readyRead, this, &KoboldClient::onReadyRead);
    // finished: la respuesta terminó, bien o mal (incluye "no se pudo conectar").
    connect(m_reply, &QNetworkReply::finished, this, &KoboldClient::onFinished);

    setGenerating(true);
}

void KoboldClient::onReadyRead()
{
    // El código de estado HTTP (200, 404, 503...) de la respuesta. attribute()
    // devuelve un QVariant, una caja que puede guardar cualquier tipo; toInt()
    // saca el número.
    int httpStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Si no es 200, lo que está llegando no es un stream de tokens sino la
    // explicación de un error. No se le pasa al parser: se deja sin leer, y
    // onFinished() lo usa para armar el mensaje.
    if (httpStatus != 200) {
        return;
    }

    processAvailableData();
}

void KoboldClient::processAvailableData()
{
    // Copia del puntero a la respuesta que se está atendiendo. Sirve para
    // detectar, más abajo, que la generación terminó a mitad del recorrido.
    QNetworkReply* reply = m_reply;

    // readAll() devuelve todos los bytes disponibles y los saca de la cola de
    // la respuesta. Pueden ser medio evento, uno, o varios: el parser se
    // encarga (ver SseParser.cpp). `events` es una lista con los eventos que
    // quedaron completos, posiblemente vacía.
    QList<SseEvent> events = m_parser.feed(reply->readAll());

    // `for (const SseEvent& event : events)` es el `for event in events:` de
    // Python. El `const ... &` hace que `event` sea una referencia a cada
    // elemento de la lista, sin copiarlo y sin poder modificarlo.
    for (const SseEvent& event : events) {
        handleEvent(event);

        // handleEvent puede haber TERMINADO la generación: porque el evento
        // traía el motivo de fin, o porque su JSON era inválido. En ese caso
        // m_reply ya no es la respuesta con la que entramos (quedó en nullptr;
        // o apunta a una nueva, si quien escuchó el aviso de fin llamó a
        // generate() ahí mismo). Los eventos que quedaran en la lista son de
        // una generación que ya no existe: no se procesan.
        if (m_reply != reply) {
            return;
        }
    }

    if (m_parser.hasOverflowed()) {
        fail("El servidor mandó demasiados datos sin completar ningún evento. "
             "Lo que responde en esa dirección no parece ser koboldcpp.");
    }
}

void KoboldClient::handleEvent(const SseEvent& event)
{
    // koboldcpp solo manda eventos de tipo "message" (vacío significa lo mismo
    // según el estándar SSE). Uno de otro tipo no sabríamos interpretarlo, así
    // que se salta, dejando constancia en el log.
    if (!event.type.isEmpty() && event.type != "message") {
        qWarning() << "KoboldClient: evento SSE de tipo desconocido, se ignora:" << event.type;
        return;
    }

    // Interpretar el JSON: el `json.loads()` de Python. En vez de lanzar una
    // excepción si el texto está mal, fromJson() deja el problema anotado en
    // `parseError`. El `&` de `&parseError` saca la DIRECCIÓN de la variable:
    // la función recibe un puntero para poder escribir en ella.
    //
    // Aquí es donde los bytes pasan a ser texto: fromJson() decodifica el UTF-8
    // y también los escapes como "\u00ed". Es seguro porque el evento ya está
    // entero (ver "Por qué bytes y no texto" en SseParser.cpp).
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(event.data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // Se muestra el principio de lo que llegó: es lo que permite entender
        // qué hay del otro lado. left(n) = los primeros n bytes.
        fail(QString("El servidor mandó un evento que no es JSON válido (%1): «%2»")
                 .arg(parseError.errorString(),
                      QString::fromUtf8(event.data.left(MAX_ERROR_BODY_BYTES))));
        return;
    }
    QJsonObject object = document.object();

    // value("token") busca la clave; si no existe o no es un string, toString()
    // devuelve un string vacío, como `obj.get("token", "")`.
    QString token = object.value("token").toString();
    // El último evento del stream trae el token vacío (está medido): no hay
    // nada que avisar.
    if (!token.isEmpty()) {
        emit tokenReceived(token);
    }

    // `finish_reason` vale null en todos los eventos menos el último.
    // isUndefined() cubre el caso de que la clave directamente no esté.
    QJsonValue finishReason = object.value("finish_reason");
    if (finishReason.isNull() || finishReason.isUndefined()) {
        return;
    }

    // Fin normal de la generación. koboldcpp no manda ningún "[DONE]": la señal
    // de fin es este campo.
    //
    // Se da por terminada AQUÍ, sin esperar a que el servidor cierre la
    // conexión. Alternativa descartada: esperar al cierre. En la traza real el
    // cierre llega 150 ms después del último evento, tiempo en que la UI
    // seguiría bloqueada sin motivo; y el final quedaría dependiendo de que el
    // servidor cierre, cuando su propia respuesta dice `connection: keep-alive`.
    //
    // Primero se suelta la respuesta y después se avisa, en ese orden: así
    // quien reciba `generationFinished` ya ve `generating` en falso y puede
    // llamar a generate() de nuevo ahí mismo.
    QString reason = finishReason.toString();
    releaseReply();
    emit generationFinished(reason);
}

// La respuesta terminó: el servidor cerró la conexión, o hubo un error de red.
// Si la generación ya se dio por terminada al ver `finish_reason`, este slot ni
// siquiera corre, porque releaseReply() cortó la conexión de la signal. Así que
// llegar aquí significa una de dos cosas: un error, o un cierre antes de tiempo.
void KoboldClient::onFinished()
{
    QNetworkReply* reply = m_reply;

    // 0 si nunca llegó una respuesta HTTP (por ejemplo, no se pudo conectar).
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Caso 1: no hubo respuesta HTTP. El servidor no está, o la red falló antes
    // de recibir nada.
    if (httpStatus == 0) {
        // error() dice qué clase de fallo fue; errorString() lo describe (en
        // inglés: viene de Qt y del sistema operativo).
        if (reply->error() == QNetworkReply::ConnectionRefusedError) {
            // "Conexión rechazada" = la máquina existe pero nadie escucha en
            // ese puerto. Es el fallo típico: koboldcpp no está corriendo.
            fail(QString("No se pudo conectar con %1: conexión rechazada. No hay "
                         "nadie escuchando en esa dirección; ¿está corriendo koboldcpp?")
                     .arg(m_baseUrl));
        } else {
            fail(QString("No se pudo conectar con %1 (%2).")
                     .arg(m_baseUrl, reply->errorString()));
        }
        return;
    }

    // Caso 2: el servidor respondió, pero no con "200 OK". Por ejemplo 503
    // cuando está ocupado con otra generación.
    if (httpStatus != 200) {
        // El texto que acompaña al código ("Service Unavailable").
        QString reasonPhrase = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        // El cuerpo de la respuesta, que onReadyRead() dejó sin leer. read(n)
        // lee como mucho n bytes. simplified() junta los saltos de línea y
        // espacios repetidos en uno, para que quepa en una línea.
        QString bodyText = QString::fromUtf8(reply->read(MAX_ERROR_BODY_BYTES)).simplified();

        QString message = QString("El servidor respondió con un error: HTTP %1 %2.")
                              .arg(httpStatus)
                              .arg(reasonPhrase);
        if (!bodyText.isEmpty()) {
            message += QString(" Detalle: %1").arg(bodyText);
        }
        fail(message);
        return;
    }

    // Caso 3: era un stream normal (200) y se terminó. Puede quedar un último
    // pedazo sin leer; se procesa antes de decidir nada.
    processAvailableData();
    // Si ese último pedazo traía el final (o un error), la generación ya se
    // cerró ahí adentro y no queda nada que hacer.
    if (m_reply != reply) {
        return;
    }

    // La conexión se cerró sin que llegara nunca un `finish_reason`. NO es un
    // final normal: el texto quedó cortado. Pasa si koboldcpp se muere o lo
    // matan a mitad de una respuesta.
    //
    // Hay dos formas de enterarse y las dos caen aquí. Como la respuesta es
    // HTTP/1.0 sin `Content-Length` (está medido), "el servidor cerró" es
    // también la forma normal de terminar el cuerpo, así que un corte limpio
    // llega SIN error de red. Un corte brusco sí trae uno, y se agrega al
    // mensaje.
    if (reply->error() != QNetworkReply::NoError) {
        fail(QString("La conexión con el servidor se cortó a mitad de la respuesta (%1). "
                     "El texto quedó incompleto.")
                 .arg(reply->errorString()));
    } else {
        fail("La conexión con el servidor se cortó a mitad de la respuesta: se cerró "
             "sin avisar que había terminado. El texto quedó incompleto.");
    }
}

void KoboldClient::releaseReply()
{
    QNetworkReply* reply = m_reply;
    // Se suelta el puntero ANTES de hacer nada más: desde aquí, para el resto
    // de la clase ya no hay generación en curso.
    m_reply = nullptr;

    // Que no nos avise de nada más. Sin esto, el abort() de abajo emitiría
    // `finished` y volveríamos a entrar a onFinished() desde aquí adentro.
    reply->disconnect(this);

    // Cierra la conexión si seguía abierta. Si ya había terminado no hace nada.
    reply->abort();

    // Borrar la respuesta: con deleteLater(), NO con `delete reply`.
    //
    // Casi siempre se llega aquí desde un slot que la propia respuesta llamó
    // (onReadyRead u onFinished). Eso quiere decir que más abajo en la cadena
    // de llamadas hay código de QNetworkReply a medio ejecutar, esperando a
    // que nuestro slot regrese para seguir. Un `delete` liberaría su memoria
    // ahora mismo, y al regresar ese código seguiría usando un objeto que ya no
    // existe: "use after free" (uso después de liberar).
    //
    // OJO: este es de los que NO avisan. ASan solo vigila el código compilado
    // con él, que es el nuestro; el uso indebido ocurriría dentro de la
    // librería de Qt, que viene ya compilada sin ASan. Se probó poniendo aquí
    // `delete reply`: los tests pasaron y ASan no dijo nada. Que no truene no
    // quiere decir que esté bien: es memoria liberada que por suerte todavía
    // nadie reutilizó. La herramienta que sí lo vería es valgrind, que vigila
    // todo el proceso (`valgrind ./build/tst_koboldclient`). Por eso la regla
    // se sigue siempre, sin esperar a un crash: dentro de un slot, el objeto
    // que emitió la signal se borra con deleteLater().
    //
    // deleteLater() solo lo ANOTA para borrar: Qt lo hace cuando el control
    // vuelve al bucle de eventos, con toda esa cadena de llamadas ya terminada.
    // En Python esto no existe porque el objeto vive mientras alguien lo
    // referencie; en C++ `delete` destruye aunque haya quien lo esté usando.
    reply->deleteLater();

    setGenerating(false);
}

void KoboldClient::fail(const QString& message)
{
    // Primero el mensaje y después el estado: quien reaccione a
    // `generatingChanged` y lea `errorMessage` ya encuentra el texto nuevo.
    setErrorMessage(message);

    // Puede no haber respuesta: generate() falla sin llegar a mandar nada
    // cuando la URL es inválida.
    if (m_reply != nullptr) {
        releaseReply();
    }

    emit generationFailed(message);
}

void KoboldClient::setGenerating(bool generating)
{
    if (m_generating == generating) {
        return;
    }
    m_generating = generating;
    emit generatingChanged();
}

void KoboldClient::setErrorMessage(const QString& message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}
