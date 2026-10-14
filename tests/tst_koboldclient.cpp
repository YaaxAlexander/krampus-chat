// tst_koboldclient.cpp — tests automáticos de KoboldClient.
//
// No usan el koboldcpp real (necesita GPU y un modelo). En su lugar, este mismo
// archivo levanta un servidor HTTP de mentira en 127.0.0.1 (la clase
// FakeKoboldServer de más abajo) que responde con lo que koboldcpp respondió de
// verdad en el endpoint de chat: las cabeceras de logs/chat-sse-trace.txt y el
// cuerpo de logs/chat-sse-raw.txt. KoboldClient no nota la diferencia: se
// conecta por un socket TCP real.
//
// Cómo correrlos, desde la raíz del repo y después de compilar:
//   ctest --test-dir build --output-on-failure
// o directamente el ejecutable, que da más detalle:
//   ./build/tst_koboldclient
//
// Qt Test es el `unittest` de Qt: una clase con un método por test, y macros
// (QCOMPARE, QVERIFY) en el papel de los `assert`.

#include "KoboldClient.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// Ruta al cuerpo real de una respuesta. KRAMPUS_SOURCE_DIR lo define CMake al
// compilar este archivo (ver target_compile_definitions en CMakeLists.txt).
const QString CHAT_CAPTURE_PATH = QString(KRAMPUS_SOURCE_DIR) + "/logs/chat-sse-raw.txt";

// El texto que forman los `content` de la captura, y cuántos eventos lo traen.
// La captura tiene además un evento con finish_reason "stop" (y content vacío)
// y el `data: [DONE]` final.
const QString CHAT_CAPTURE_TEXT =
    "El invierno se caracteriza por temperaturas bajas y días cortos de luz.";
const int CHAT_CAPTURE_TOKENS = 16;

// Las cabeceras con las que respondió koboldcpp, copiadas de
// logs/chat-sse-trace.txt (menos `access-control-allow-headers`, que es
// larguísima y solo le importa a un navegador). Los detalles que importan: es
// HTTP/1.0, dice `connection: keep-alive` y NO trae `Content-Length`, así que
// el cuerpo termina cuando el servidor cierra la conexión.
//
// En HTTP las cabeceras terminan cada una en "\r\n", y una línea vacía las
// separa del cuerpo. (En C++, dos strings entre comillas seguidos se pegan en
// uno solo: es la forma de partir un string largo en varias líneas.)
const QByteArray REAL_OK_HEADERS =
    "HTTP/1.0 200 OK\r\n"
    "Server: KoboldCppServer 1\r\n"
    "Date: Wed, 14 Oct 2026 06:01:11 GMT\r\n"
    "X-Accel-Buffering: no\r\n"
    "cache-control: no-cache\r\n"
    "connection: keep-alive\r\n"
    "access-control-allow-origin: *\r\n"
    "access-control-allow-methods: *\r\n"
    "access-control-allow-private-network: true\r\n"
    "cache-control: no-store\r\n"
    "content-type: text/event-stream\r\n"
    "\r\n";

// El warning que deja KoboldClient cuando el stream termina con `[DONE]` sin
// un finish_reason antes. Es una expresión regular (un patrón de búsqueda, el
// módulo `re` de Python) y no el texto entero, para que estos tests no se
// rompan si se retoca la redacción. `\\[` es un '[' literal: en un patrón los
// corchetes solos tienen otro significado, y en un string de C++ la barra
// invertida se escribe doble.
const QRegularExpression DONE_WITHOUT_REASON_WARNING("\\[DONE\\] sin dar un finish_reason");

// Pausa entre un pedazo de respuesta y el siguiente. El koboldcpp real tarda
// unos 50 ms por token; aquí alcanza con que sea más que cero, para que cada
// pedazo viaje por separado y el cliente los reciba en varias lecturas.
const int CHUNK_PAUSE_MS = 2;

// Tiempo máximo que un test espera a que ocurra algo. Es un tope, no una pausa:
// las esperas terminan en cuanto la condición se cumple.
const int WAIT_MS = 10000;

// --- Funciones de ayuda ---

// Lee la captura real entera, como bytes.
QByteArray readChatCapture()
{
    QFile file(CHAT_CAPTURE_PATH);
    // qFatal corta el programa con ese mensaje: sin la captura no hay tests.
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("No se pudo abrir %s", qPrintable(CHAT_CAPTURE_PATH));
    }
    return file.readAll();
}

// Parte la captura en eventos enteros, uno por pedazo. Es exactamente el
// troceo de logs/chat-sse-trace.txt, donde cada lectura trajo un evento.
//
// `QList<QByteArray>` = lista de bytes (`list[bytes]`).
QList<QByteArray> chatTraceChunks()
{
    QByteArray capture = readChatCapture();
    QList<QByteArray> chunks;

    qsizetype eventStart = 0;
    while (eventStart < capture.size()) {
        // Cada evento termina en "\n\n".
        qsizetype separatorPosition = capture.indexOf("\n\n", eventStart);
        if (separatorPosition == -1) {
            break;
        }
        // mid(desde, cuántos) = un trozo. El +2 incluye los dos '\n'.
        chunks.append(capture.mid(eventStart, separatorPosition + 2 - eventStart));
        eventStart = separatorPosition + 2;
    }
    return chunks;
}

// Parte la captura en pedazos MÁS chicos que los de la traza: cada evento en
// dos, la línea `data: ...\n` por un lado y la línea vacía que lo cierra por el
// otro. Es el troceo que usan casi todos los tests: si el cliente esperara un
// evento entero por lectura (lo que la traza de chat mostró, pero nadie
// promete), aquí se notaría. Con el endpoint nativo la traza sí mostró eventos
// partidos así.
QList<QByteArray> chatCaptureChunks()
{
    QList<QByteArray> chunks;
    for (const QByteArray& event : chatTraceChunks()) {
        // chop(1) quita el último byte: el segundo '\n'.
        QByteArray dataLine = event;
        dataLine.chop(1);
        chunks.append(dataLine);
        chunks.append("\n");
    }
    return chunks;
}

// Un evento SSE del endpoint de chat, armado a mano. `choiceJson` es lo que va
// dentro de `"choices": [ ... ]`; el resto imita a koboldcpp. Sirve para los
// casos que la captura real no trae.
QByteArray chatEvent(const QByteArray& choiceJson)
{
    return "data: {\"id\": \"chatcmpl-1\", \"object\": \"chat.completion.chunk\", "
           "\"choices\": [" + choiceJson + "]}\n\n";
}

// Un evento con un pedazo de texto, como los de la captura.
QByteArray tokenEvent(const QByteArray& token)
{
    return chatEvent("{\"index\": 0, \"finish_reason\": null, "
                     "\"delta\": {\"role\": \"assistant\", \"content\": \"" + token + "\"}}");
}

// El evento que cierra, con content vacío, como el de la captura.
QByteArray finishEvent(const QByteArray& reason)
{
    return chatEvent("{\"index\": 0, \"finish_reason\": \"" + reason + "\", "
                     "\"delta\": {\"role\": \"assistant\", \"content\": \"\"}}");
}

const QByteArray DONE_EVENT = "data: [DONE]\n\n";

// Un mensaje de chat como los que manda QML: un "diccionario" con `role` y
// `content`. QVariantMap es el `dict` de Qt (claves de texto, valores en una
// caja QVariant); un objeto de JavaScript llega a C++ convertido en uno de
// estos.
QVariantMap chatMessage(const QString& role, const QString& content)
{
    QVariantMap message;
    message.insert("role", role);
    message.insert("content", content);
    return message;
}

// La conversación más simple: un solo mensaje del usuario. QVariantList es la
// lista de Qt cuyos elementos son cajas QVariant (cada una puede guardar
// cualquier tipo): lo que llega cuando QML pasa un arreglo.
QVariantList userSays(const QString& text)
{
    QVariantList messages;
    messages.append(chatMessage("user", text));
    return messages;
}

// Une los tokens que anotó un QSignalSpy enganchado a `tokenReceived`.
//
// QSignalSpy es una lista con una entrada por cada vez que se emitió la signal,
// y cada entrada es a su vez la lista de argumentos de esa emisión. Así que
// `spy.at(i).at(0)` es el primer argumento de la emisión número i. Viene como
// QVariant (una caja que guarda cualquier tipo) y toString() saca el texto.
QString joinTokens(const QSignalSpy& spy)
{
    QString text;
    for (qsizetype i = 0; i < spy.count(); i++) {
        text += spy.at(i).at(0).toString();
    }
    return text;
}

// --- El servidor de mentira ---
//
// Un servidor HTTP mínimo: acepta una conexión, lee la petición entera, y
// responde con las cabeceras y los pedazos de cuerpo que el test le configuró,
// un pedazo cada CHUNK_PAUSE_MS. Atiende una conexión a la vez, que es lo único
// que KoboldClient debería abrir.
//
// Va en este mismo archivo porque solo sirve para estos tests. Es un QObject
// porque necesita slots para enterarse de lo que pasa en la red.
class FakeKoboldServer : public QObject
{
    Q_OBJECT

public:
    // `explicit` impide conversiones automáticas hacia esta clase; `parent` es
    // el padre de Qt (ninguno por defecto).
    explicit FakeKoboldServer(QObject* parent = nullptr);

    // --- Qué responder. El test lo cambia antes de llamar a generate(). ---
    // Son variables públicas a propósito: para una clase de ayuda de un test,
    // escribirlas directo es más claro que un setter por cada una.

    // Línea de estado y cabeceras, terminadas en la línea vacía.
    QByteArray responseHeaders = REAL_OK_HEADERS;
    // El cuerpo, ya cortado en los pedazos que se mandan por separado.
    QList<QByteArray> bodyChunks;
    // Si es verdadero, tras el último pedazo el servidor cierra la conexión
    // (lo que hace koboldcpp). Si es falso, la deja abierta sin mandar más.
    bool closeWhenDone = true;

    // --- Qué pasó. El test lo lee después. ---
    // Cuántas conexiones TCP llegaron y cuántas peticiones completas.
    int connectionCount = 0;
    int requestCount = 0;
    // La última petición: la parte de cabeceras (texto) y el cuerpo (bytes).
    QByteArray lastRequestHead;
    QByteArray lastRequestBody;

    // Dirección para darle al cliente: "http://127.0.0.1:<puerto>".
    // El `const` del final promete que el método solo lee.
    QString baseUrl() const;

    // ¿Hay una conexión abierta en este momento?
    bool hasOpenConnection() const;

signals:
    // El cliente cerró la conexión (o se cayó) antes de que cerráramos nosotros.
    void clientDisconnected();

private:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void sendNextChunk();

    // El socket que escucha, y el de la conexión que se está atendiendo (o
    // nullptr). Los dos son hijos de este objeto: Qt los borra con él.
    QTcpServer* m_server = nullptr;
    QTcpSocket* m_socket = nullptr;
    QTimer* m_chunkTimer = nullptr;

    // Bytes de la petición recibidos hasta ahora.
    QByteArray m_requestBytes;
    // ¿Ya se leyó la petición entera y se empezó a responder?
    bool m_responding = false;
    // Índice del próximo pedazo de bodyChunks a mandar.
    qsizetype m_nextChunk = 0;
};

FakeKoboldServer::FakeKoboldServer(QObject* parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    m_chunkTimer = new QTimer(this);
    m_chunkTimer->setInterval(CHUNK_PAUSE_MS);

    // `connect`: "cuando este objeto emita esta signal, llama a este método".
    connect(m_server, &QTcpServer::newConnection, this, &FakeKoboldServer::onNewConnection);
    connect(m_chunkTimer, &QTimer::timeout, this, &FakeKoboldServer::sendNextChunk);

    // Escuchar solo en 127.0.0.1 (LocalHost): el servidor no es visible desde
    // fuera de esta máquina. Puerto 0 = "que el sistema elija uno libre"; así
    // el test no choca con un koboldcpp real en el 5001 ni con otro test.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        qFatal("FakeKoboldServer: no se pudo escuchar en 127.0.0.1: %s",
               qPrintable(m_server->errorString()));
    }
}

QString FakeKoboldServer::baseUrl() const
{
    // serverPort() es el puerto que eligió el sistema.
    return QString("http://127.0.0.1:%1").arg(m_server->serverPort());
}

bool FakeKoboldServer::hasOpenConnection() const
{
    return m_socket != nullptr;
}

void FakeKoboldServer::onNewConnection()
{
    // nextPendingConnection() entrega el socket de la conexión nueva. Lo crea
    // Qt como hijo de m_server.
    QTcpSocket* socket = m_server->nextPendingConnection();
    connectionCount++;

    // Ya hay una conexión en curso: esta sobra. Se cuenta (arriba) para que el
    // test pueda detectarla, y se cierra.
    if (m_socket != nullptr) {
        socket->disconnectFromHost();
        // Que se borre sola cuando termine de cerrarse. Los cuatro argumentos
        // son los de siempre: emisor, signal, receptor, slot. Aquí el receptor
        // es el propio socket y el slot es su deleteLater().
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        return;
    }

    m_socket = socket;
    m_requestBytes.clear();
    m_responding = false;
    m_nextChunk = 0;
    connect(m_socket, &QTcpSocket::readyRead, this, &FakeKoboldServer::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &FakeKoboldServer::onSocketDisconnected);
}

void FakeKoboldServer::onSocketReadyRead()
{
    m_requestBytes.append(m_socket->readAll());
    if (m_responding) {
        return;
    }

    // Una petición HTTP son cabeceras, una línea vacía ("\r\n\r\n") y el
    // cuerpo. Igual que con SSE, puede llegar en varios pedazos: se acumula
    // hasta tenerla entera.
    qsizetype headEnd = m_requestBytes.indexOf("\r\n\r\n");
    if (headEnd == -1) {
        return;
    }
    QByteArray head = m_requestBytes.left(headEnd);

    // Cuánto mide el cuerpo lo dice la cabecera Content-Length.
    qsizetype contentLength = 0;
    // split corta por el separador y devuelve la lista de líneas.
    QList<QByteArray> headerLines = head.split('\n');
    for (const QByteArray& line : headerLines) {
        // Los nombres de las cabeceras HTTP no distinguen mayúsculas.
        if (line.toLower().startsWith("content-length:")) {
            // mid(15) = lo que sigue a "content-length:" (15 caracteres).
            contentLength = line.mid(15).trimmed().toLongLong();
        }
    }

    qsizetype bodyStart = headEnd + 4;
    if (m_requestBytes.size() < bodyStart + contentLength) {
        // Todavía falta parte del cuerpo.
        return;
    }

    // Petición completa: se anota y se empieza a responder.
    lastRequestHead = head;
    lastRequestBody = m_requestBytes.mid(bodyStart, contentLength);
    requestCount++;
    m_responding = true;

    m_socket->write(responseHeaders);
    // El cuerpo sale de a un pedazo por cada disparo del temporizador.
    m_chunkTimer->start();
}

void FakeKoboldServer::sendNextChunk()
{
    if (m_socket == nullptr) {
        m_chunkTimer->stop();
        return;
    }

    if (m_nextChunk < bodyChunks.size()) {
        m_socket->write(bodyChunks.at(m_nextChunk));
        m_nextChunk++;
        return;
    }

    // No quedan pedazos.
    m_chunkTimer->stop();
    if (closeWhenDone) {
        // Cerramos nosotros: ya no interesa su aviso de desconexión.
        QTcpSocket* socket = m_socket;
        m_socket = nullptr;
        socket->disconnect(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        // disconnectFromHost() termina de mandar lo pendiente y después cierra.
        socket->disconnectFromHost();
    }
}

void FakeKoboldServer::onSocketDisconnected()
{
    // Se fue el cliente. deleteLater() y no `delete`: estamos dentro de un slot
    // que el propio socket llamó (ver releaseReply() en KoboldClient.cpp).
    m_chunkTimer->stop();
    m_socket->deleteLater();
    m_socket = nullptr;
    emit clientDisconnected();
}


// --- Los tests ---
//
// La clase hereda de QObject y lleva Q_OBJECT porque Qt Test descubre los tests
// buscando sus "private slots": cada uno es un test, en el orden escrito.
class TestKoboldClient : public QObject
{
    Q_OBJECT

private slots:
    void fullStream();
    void requestIsPostToChatCompletions();
    void doneAfterFinishReasonIsNotASecondEnd();
    void doneWithoutFinishReasonEndsWithoutReason();
    void deltaWithoutContentIsNotAnError();
    void emptyOrMissingChoicesAreIgnored();
    void invalidMessagesFailWithoutNetwork();
    void unreachableServer();
    void emptyBaseUrlFailsWithoutNetwork();
    void http503();
    void connectionCutWithoutEndMarker();
    void invalidJsonInEvent();
    void secondGenerateWhileGeneratingIsIgnored();
    void generateAgainFromInsideFinishedSignal();
    void destroyClientMidStream();
};

// Sobre los macros:
//   QCOMPARE(a, b) / QVERIFY(x) : fallan el test si a != b / si x es falso.
//   QVERIFY2(x, m) : como QVERIFY, mostrando además el mensaje m.
//   QTRY_COMPARE_WITH_TIMEOUT(a, b, ms) : como QCOMPARE, pero si todavía no se
//       cumple deja correr el bucle de eventos y reintenta hasta `ms`.
//       KoboldClient es asíncrono: generate() regresa enseguida y todo lo demás
//       llega por signals, que solo se reparten mientras corre el bucle de
//       eventos. Casi todas las esperas de aquí son "hasta que el spy haya
//       anotado la signal de fin": terminan apenas llega, sin pausas fijas.
//
// En cada test, `server` y `client` son variables locales: se destruyen solas
// al llegar a la `}` (en orden inverso al de creación: primero el cliente).

// El camino feliz: la respuesta real, entera.
void TestKoboldClient::fullStream()
{
    FakeKoboldServer server;
    server.bodyChunks = chatCaptureChunks();

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());

    // Un QSignalSpy por cada signal que interesa. `&client` es la dirección del
    // objeto (el spy quiere un puntero) y `&KoboldClient::tokenReceived` nombra
    // la signal.
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);
    QSignalSpy generatingSpy(&client, &KoboldClient::generatingChanged);

    QVERIFY(!client.generating());
    client.generate(userSays("Escribe una frase corta sobre el invierno."));
    // Inmediatamente después de generate(), sin esperar nada.
    QVERIFY(client.generating());

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    // Los tokens, en orden y completos (incluida la 'í' que viaja escapada).
    QCOMPARE(tokenSpy.count(), CHAT_CAPTURE_TOKENS);
    QCOMPARE(tokenSpy.at(0).at(0).toString(), QString("El"));
    QCOMPARE(tokenSpy.at(1).at(0).toString(), QString(" inv"));
    QCOMPARE(joinTokens(tokenSpy), CHAT_CAPTURE_TEXT);

    // Terminó con el motivo que mandó el servidor.
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QString("stop"));

    // `generating` volvió a falso, con dos cambios: a verdadero y a falso.
    QVERIFY(!client.generating());
    QCOMPARE(generatingSpy.count(), 2);

    // Y no hubo ningún error.
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(client.errorMessage().isEmpty());
    QCOMPARE(server.requestCount, 1);
}

// Lo que el servidor recibe: un POST a /v1/chat/completions con un JSON que
// trae la conversación intacta, `stream: true` y un máximo de tokens.
void TestKoboldClient::requestIsPostToChatCompletions()
{
    FakeKoboldServer server;
    server.bodyChunks = chatCaptureChunks();

    KoboldClient client;
    // Con una barra de más al final, a propósito: no debe salir "//v1/...".
    client.setBaseUrl(server.baseUrl() + "/");
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);

    // Tres mensajes con todo lo que podría romperse al armar el JSON: comillas,
    // saltos de línea, tabulador, acentos, ñ, otros alfabetos y emoji (que en
    // UTF-16, el formato interno de QString, ocupan DOS unidades: un error al
    // copiarlos los partiría).
    QVariantList messages;
    messages.append(chatMessage("user", "Di \"hola\" en español, ñandú:\nsegunda línea"));
    messages.append(chatMessage("assistant", "¡Hola! 👋 «comillas» \"dobles\"\n\ty un tab"));
    messages.append(chatMessage("user", "¿Y en japonés? 日本語 🎌"));

    // Una clave de más en un mensaje: el cliente no debe mandarla. Se le
    // agrega a una COPIA del mensaje 2 (toMap() devuelve una copia) y la copia
    // reemplaza al original en la lista.
    QVariantMap withExtraKey = messages.at(1).toMap();
    withExtraKey.insert("hora", "12:00");
    messages.replace(1, withExtraKey);

    client.generate(messages);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    // La primera línea de la petición: método, ruta y versión.
    QVERIFY2(server.lastRequestHead.startsWith("POST /v1/chat/completions HTTP/1.1\r\n"),
             server.lastRequestHead.constData());
    QVERIFY2(server.lastRequestHead.toLower().contains("content-type: application/json"),
             server.lastRequestHead.constData());

    // El cuerpo es un objeto JSON válido...
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(server.lastRequestBody, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    QJsonObject body = document.object();

    // ...que pide streaming (un booleano de verdad, no el texto "true")...
    QVERIFY(body.value("stream").isBool());
    QCOMPARE(body.value("stream").toBool(), true);
    // ...con un max_tokens numérico y positivo...
    QVERIFY(body.value("max_tokens").isDouble());
    QVERIFY(body.value("max_tokens").toInt() > 0);
    // ...y nada más que eso y los mensajes: los samplers son de la Fase 3.
    QCOMPARE(body.size(), 3);

    // Los mensajes: mismos, en el mismo orden, con roles y textos idénticos, y
    // cada uno con solo sus dos claves.
    QJsonArray sent = body.value("messages").toArray();
    QCOMPARE(sent.size(), messages.size());
    for (qsizetype i = 0; i < sent.size(); i++) {
        QJsonObject sentMessage = sent.at(i).toObject();
        QVariantMap original = messages.at(i).toMap();
        QCOMPARE(sentMessage.size(), 2);
        QCOMPARE(sentMessage.value("role").toString(), original.value("role").toString());
        QCOMPARE(sentMessage.value("content").toString(), original.value("content").toString());
    }
}

// El final llega dos veces: el evento con finish_reason y después `[DONE]`. Se
// avisa UNA sola vez, con el motivo, y sin errores ni warnings. Se prueba con
// los dos casos de lectura: todo el cuerpo en una (el `[DONE]` queda en la
// misma lista de eventos que el finish_reason) y con el troceo de la traza (el
// `[DONE]` llega en una lectura posterior).
//
// En los dos el servidor deja la conexión ABIERTA al final: así se comprueba
// que lo que cierra la generación es el finish_reason y no el cierre.
void TestKoboldClient::doneAfterFinishReasonIsNotASecondEnd()
{
    // Si apareciera el warning de "[DONE] sin finish_reason", el test falla.
    QTest::failOnWarning(DONE_WITHOUT_REASON_WARNING);

    // Los dos troceos. `QList<QList<QByteArray>>` = lista de listas de bytes.
    QList<QList<QByteArray>> bodies;
    QList<QByteArray> allInOneRead;
    allInOneRead.append(readChatCapture());
    bodies.append(allInOneRead);
    bodies.append(chatTraceChunks());

    for (const QList<QByteArray>& body : bodies) {
        FakeKoboldServer server;
        server.bodyChunks = body;
        server.closeWhenDone = false;

        KoboldClient client;
        client.setBaseUrl(server.baseUrl());
        QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
        QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
        QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);
        QSignalSpy generatingSpy(&client, &KoboldClient::generatingChanged);

        client.generate(userSays("hola"));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);
        QCOMPARE(finishedSpy.at(0).at(0).toString(), QString("stop"));

        // Al terminar, el cliente corta la conexión por su cuenta. Se espera a
        // que el servidor lo note: para entonces, si el `[DONE]` fuera a
        // producir algo, ya lo habría hecho.
        QTRY_VERIFY_WITH_TIMEOUT(!server.hasOpenConnection(), WAIT_MS);

        QCOMPARE(finishedSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(generatingSpy.count(), 2);
        QCOMPARE(joinTokens(tokenSpy), CHAT_CAPTURE_TEXT);
        QVERIFY(client.errorMessage().isEmpty());
    }
}

// `[DONE]` sin ningún finish_reason antes. El servidor dijo que terminó, así
// que es un final bueno, con el motivo vacío (y un warning en el log). La
// conexión queda abierta: lo que cierra la generación es el `[DONE]`.
void TestKoboldClient::doneWithoutFinishReasonEndsWithoutReason()
{
    FakeKoboldServer server;
    server.bodyChunks.append(tokenEvent("Hola"));
    server.bodyChunks.append(tokenEvent(" mundo"));
    server.bodyChunks.append(DONE_EVENT);
    server.closeWhenDone = false;

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    // Se espera el warning: si no llegara, el test falla.
    QTest::ignoreMessage(QtWarningMsg, DONE_WITHOUT_REASON_WARNING);
    client.generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    QCOMPARE(finishedSpy.at(0).at(0).toString(), QString(""));
    QCOMPARE(joinTokens(tokenSpy), QString("Hola mundo"));
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!client.generating());
    QVERIFY(client.errorMessage().isEmpty());
}

// Deltas sin texto: sin la clave `content`, con `content: null`, con el delta
// vacío o sin delta. Ninguno es un error ni produce un token; el texto que sí
// llega se entrega igual.
void TestKoboldClient::deltaWithoutContentIsNotAnError()
{
    FakeKoboldServer server;
    // Solo el rol: lo que mandan otros servidores de este estilo al empezar.
    server.bodyChunks.append(chatEvent("{\"index\": 0, \"finish_reason\": null, \"delta\": {\"role\": \"assistant\"}}"));
    server.bodyChunks.append(chatEvent("{\"index\": 0, \"finish_reason\": null, \"delta\": {\"content\": null}}"));
    server.bodyChunks.append(chatEvent("{\"index\": 0, \"finish_reason\": null, \"delta\": {}}"));
    server.bodyChunks.append(chatEvent("{\"index\": 0, \"finish_reason\": null}"));
    server.bodyChunks.append(tokenEvent("Hola"));
    // El final, también sin content.
    server.bodyChunks.append(chatEvent("{\"index\": 0, \"finish_reason\": \"length\", \"delta\": {}}"));
    server.bodyChunks.append(DONE_EVENT);

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    QCOMPARE(tokenSpy.count(), 1);
    QCOMPARE(joinTokens(tokenSpy), QString("Hola"));
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QString("length"));
    QCOMPARE(failedSpy.count(), 0);
}

// Eventos con `choices` vacío, sin `choices`, o con `choices` que no es una
// lista. No traen nada que hacer: se ignoran, sin error.
void TestKoboldClient::emptyOrMissingChoicesAreIgnored()
{
    FakeKoboldServer server;
    server.bodyChunks.append("data: {\"id\": \"chatcmpl-1\", \"choices\": []}\n\n");
    server.bodyChunks.append(tokenEvent("Hola"));
    server.bodyChunks.append("data: {\"id\": \"chatcmpl-1\"}\n\n");
    server.bodyChunks.append("data: {\"id\": \"chatcmpl-1\", \"choices\": null}\n\n");
    server.bodyChunks.append(tokenEvent(" mundo"));
    server.bodyChunks.append(finishEvent("stop"));
    server.bodyChunks.append(DONE_EVENT);

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    QCOMPARE(joinTokens(tokenSpy), QString("Hola mundo"));
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QString("stop"));
    QCOMPARE(failedSpy.count(), 0);
}

// Conversaciones mal formadas: fallan en el momento, con un mensaje que dice
// qué está mal, sin `generating` en ningún momento y sin abrir conexión.
void TestKoboldClient::invalidMessagesFailWithoutNetwork()
{
    FakeKoboldServer server;

    // Cada caso: la lista de mensajes y un pedazo del mensaje de error
    // esperado. `struct` agrupa los dos datos (como un @dataclass).
    struct InvalidCase {
        QVariantList messages;
        QString expectedError;
    };

    QVariantMap withoutRole;
    withoutRole.insert("content", "hola");
    QVariantMap emptyRole = chatMessage("", "hola");
    QVariantMap withoutContent;
    withoutContent.insert("role", "user");
    QVariantMap numericContent;
    numericContent.insert("role", "user");
    numericContent.insert("content", 42);

    // Un caso por línea. Las llaves `{ ... }` arman cada InvalidCase campo por
    // campo, en el orden en que están declarados.
    QList<InvalidCase> cases = {
        {QVariantList(), "vacía"},
        {QVariantList({QVariant("hola")}), "mensaje 1"},
        {QVariantList({withoutRole}), "rol"},
        {QVariantList({emptyRole}), "rol"},
        {QVariantList({withoutContent}), "texto"},
        {QVariantList({numericContent}), "texto"},
        // El primero está bien, el segundo no: el error dice cuál.
        {QVariantList({chatMessage("user", "hola"), withoutRole}), "mensaje 2"},
    };

    for (const InvalidCase& invalidCase : cases) {
        KoboldClient client;
        client.setBaseUrl(server.baseUrl());
        QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);
        QSignalSpy generatingSpy(&client, &KoboldClient::generatingChanged);

        client.generate(invalidCase.messages);

        // Sin esperar: el fallo es inmediato.
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(generatingSpy.count(), 0);
        QVERIFY(!client.generating());
        QVERIFY2(client.errorMessage().contains(invalidCase.expectedError),
                 qPrintable(client.errorMessage()));
    }

    // Una pausa fija, la única de estos tests: aquí se comprueba que algo NO
    // pasa, y para eso no hay condición que esperar. Si alguno de los casos
    // hubiera abierto una conexión, en 200 ms en 127.0.0.1 ya habría llegado.
    QTest::qWait(200);
    QCOMPARE(server.connectionCount, 0);
}

// Nadie escucha en ese puerto: es lo que pasa si koboldcpp no está corriendo.
void TestKoboldClient::unreachableServer()
{
    // Para conseguir un puerto donde seguro no hay nadie: se levanta un
    // servidor (el sistema le da un puerto libre), se anota su dirección y se
    // lo destruye. Las llaves sueltas `{ }` crean un bloque: `server` deja de
    // existir en la `}`, y con él se cierra el puerto.
    QString deadUrl;
    {
        FakeKoboldServer server;
        deadUrl = server.baseUrl();
    }

    KoboldClient client;
    client.setBaseUrl(deadUrl);
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));
    QVERIFY(client.generating());

    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, WAIT_MS);

    QVERIFY(!client.generating());
    QVERIFY2(client.errorMessage().contains("conexión rechazada"), qPrintable(client.errorMessage()));
    // El mensaje nombra la dirección, para saber a dónde se intentó.
    QVERIFY2(client.errorMessage().contains(deadUrl), qPrintable(client.errorMessage()));
    // La signal lleva el mismo texto que la propiedad.
    QCOMPARE(failedSpy.at(0).at(0).toString(), client.errorMessage());
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(tokenSpy.count(), 0);
}

// Sin dirección configurada: falla en el momento, sin tocar la red.
void TestKoboldClient::emptyBaseUrlFailsWithoutNetwork()
{
    KoboldClient client;
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));

    // Sin esperar: el fallo es inmediato.
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!client.generating());
    QVERIFY2(client.errorMessage().contains("no es válida"), qPrintable(client.errorMessage()));
}

// El servidor responde, pero con un error HTTP. koboldcpp da 503 cuando está
// ocupado. Después, una generación que sale bien borra el error.
void TestKoboldClient::http503()
{
    FakeKoboldServer server;
    server.responseHeaders =
        "HTTP/1.0 503 Service Unavailable\r\n"
        "content-type: application/json\r\n"
        "\r\n";
    server.bodyChunks.append(
        "{\"detail\": {\"msg\": \"Server is busy; please try again later.\", \"type\": \"service_unavailable\"}}");

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, WAIT_MS);

    QVERIFY(!client.generating());
    // El mensaje trae el código y el motivo que dio el servidor en el cuerpo.
    QVERIFY2(client.errorMessage().contains("503"), qPrintable(client.errorMessage()));
    QVERIFY2(client.errorMessage().contains("Server is busy"), qPrintable(client.errorMessage()));
    QCOMPARE(finishedSpy.count(), 0);
    // El cuerpo del error no se confundió con tokens.
    QCOMPARE(tokenSpy.count(), 0);

    // Segundo intento, ahora con el servidor respondiendo bien.
    server.responseHeaders = REAL_OK_HEADERS;
    server.bodyChunks = chatCaptureChunks();

    client.generate(userSays("hola"));
    // El error viejo se borra al empezar, no al terminar.
    QVERIFY(client.errorMessage().isEmpty());

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);
    QCOMPARE(joinTokens(tokenSpy), CHAT_CAPTURE_TEXT);
    QCOMPARE(failedSpy.count(), 1);
}

// El servidor manda tokens y cierra la conexión sin haber mandado ni el
// finish_reason ni el `[DONE]` (koboldcpp se murió a mitad de la respuesta).
// Eso es un error, no un final: los tokens que llegaron se entregaron, pero se
// avisa que el texto quedó cortado. Dos variantes: cortado a mitad de un
// evento, y cortado justo después del último token (el texto está entero,
// pero el cliente no tiene cómo saberlo).
void TestKoboldClient::connectionCutWithoutEndMarker()
{
    QList<QByteArray> allChunks = chatCaptureChunks();

    // Cada caso: cuántos pedazos manda el servidor antes de cerrar, y el texto
    // que debe haber llegado. Los pedazos van de a dos por evento (ver
    // chatCaptureChunks).
    struct CutCase {
        qsizetype chunkCount;
        QString expectedText;
    };
    QList<CutCase> cases = {
        // 5 eventos enteros y la línea `data:` del sexto, sin la línea vacía
        // que lo cierra: ese sexto nunca se entrega.
        {11, "El invierno se caracteriza"},
        // Los 16 eventos con texto, y ni el finish_reason ni el `[DONE]`.
        {CHAT_CAPTURE_TOKENS * 2, CHAT_CAPTURE_TEXT},
    };

    for (const CutCase& cutCase : cases) {
        FakeKoboldServer server;
        // mid(0, n) sobre una lista = sus primeros n elementos.
        server.bodyChunks = allChunks.mid(0, cutCase.chunkCount);

        KoboldClient client;
        client.setBaseUrl(server.baseUrl());
        QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
        QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
        QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

        client.generate(userSays("hola"));
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, WAIT_MS);

        QCOMPARE(joinTokens(tokenSpy), cutCase.expectedText);
        QCOMPARE(finishedSpy.count(), 0);
        QVERIFY(!client.generating());
        QVERIFY2(client.errorMessage().contains("se cortó"), qPrintable(client.errorMessage()));
    }
}

// Un evento cuyo `data:` no es JSON (y tampoco es `[DONE]`). Lo de antes se
// entrega, la generación falla ahí, y lo que venga después se ignora.
void TestKoboldClient::invalidJsonInEvent()
{
    FakeKoboldServer server;
    server.bodyChunks.append(tokenEvent("bien"));
    server.bodyChunks.append("data: {esto no es json\n\n");
    server.bodyChunks.append(tokenEvent("tarde"));
    server.bodyChunks.append(finishEvent("stop"));
    server.bodyChunks.append(DONE_EVENT);

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    client.generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, WAIT_MS);

    QVERIFY(!client.generating());
    QVERIFY2(client.errorMessage().contains("JSON"), qPrintable(client.errorMessage()));
    // El mensaje muestra lo que llegó, para poder entender qué pasó.
    QVERIFY2(client.errorMessage().contains("{esto no es json"), qPrintable(client.errorMessage()));

    // Tras el fallo el cliente corta la conexión por su cuenta. Se espera a que
    // del lado del servidor la conexión termine (porque notó el corte, o porque
    // mandó todo y cerró él: lo que pase primero), para comprobar que lo que
    // venía detrás, si llegó a salir, ya no produjo nada.
    QTRY_VERIFY_WITH_TIMEOUT(!server.hasOpenConnection(), WAIT_MS);
    QCOMPARE(joinTokens(tokenSpy), QString("bien"));
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 1);
}

// Llamar a generate() con una generación en curso no manda una segunda petición.
void TestKoboldClient::secondGenerateWhileGeneratingIsIgnored()
{
    FakeKoboldServer server;
    server.bodyChunks = chatCaptureChunks();

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);
    QSignalSpy generatingSpy(&client, &KoboldClient::generatingChanged);

    client.generate(userSays("primero"));

    // KoboldClient avisa con un qWarning cuando ignora la llamada. Qt Test deja
    // declarar qué warning se espera; si no llegara, el test falla.
    QTest::ignoreMessage(QtWarningMsg,
                         "KoboldClient: generate() ignorado, ya hay una generación en curso.");
    client.generate(userSays("segundo"));
    QVERIFY(client.generating());

    // Otra vez a mitad del stream, cuando ya llegaron algunos tokens.
    QTRY_VERIFY_WITH_TIMEOUT(tokenSpy.count() >= 3, WAIT_MS);
    QTest::ignoreMessage(QtWarningMsg,
                         "KoboldClient: generate() ignorado, ya hay una generación en curso.");
    client.generate(userSays("tercero"));

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, WAIT_MS);

    // El servidor vio una sola conexión y una sola petición, la primera. Una
    // segunda habría llegado durante el stream, que dura decenas de pedazos.
    QCOMPARE(server.connectionCount, 1);
    QCOMPARE(server.requestCount, 1);
    QVERIFY(server.lastRequestBody.contains("primero"));
    // Y la respuesta no se mezcló ni se duplicó.
    QCOMPARE(joinTokens(tokenSpy), CHAT_CAPTURE_TEXT);
    QCOMPARE(generatingSpy.count(), 2);
    QCOMPARE(failedSpy.count(), 0);
}

// Volver a generar DESDE DENTRO del aviso de fin. Es "reentrancia" (ver el test
// restartFromInsideFailedSignal de tst_processmanager.cpp): el slot corre
// cuando KoboldClient todavía está a mitad de procesar el evento que cerró la
// primera respuesta. Debe funcionar, y las dos respuestas no deben mezclarse.
//
// Es lo que hará la interfaz de chat: al terminar una respuesta, el usuario ya
// puede mandar la siguiente. Se prueba con el cuerpo entero en una sola lectura,
// el caso más exigente: el `[DONE]` de la primera respuesta queda en la misma
// lista de eventos, detrás del finish_reason, cuando ya hay en curso una
// generación nueva. No debe tomarse como el final de la segunda.
void TestKoboldClient::generateAgainFromInsideFinishedSignal()
{
    // Por qué no debería aparecer: ver doneAfterFinishReasonIsNotASecondEnd.
    QTest::failOnWarning(DONE_WITHOUT_REASON_WARNING);

    FakeKoboldServer server;
    server.bodyChunks.append(readChatCapture());

    KoboldClient client;
    client.setBaseUrl(server.baseUrl());
    QSignalSpy tokenSpy(&client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(&client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(&client, &KoboldClient::generationFailed);

    bool alreadyRepeated = false;

    // El slot es una "lambda": una función sin nombre escrita en el lugar. Los
    // corchetes son la "captura": las variables de afuera que puede usar. El
    // `&` las captura por referencia (las mismas variables, no copias), que es
    // seguro porque viven hasta el final del test, igual que la conexión. El
    // tercer argumento (`&client`) es el objeto "contexto": si se destruye, Qt
    // corta la conexión sola.
    connect(&client, &KoboldClient::generationFinished, &client, [&client, &alreadyRepeated]() {
        if (!alreadyRepeated) {
            alreadyRepeated = true;
            // Al avisar que terminó, el cliente ya tiene que estar libre.
            QVERIFY(!client.generating());
            client.generate(userSays("segunda vuelta"));
            QVERIFY(client.generating());
        }
    });

    client.generate(userSays("primera vuelta"));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, WAIT_MS);

    QVERIFY(alreadyRepeated);
    QVERIFY(!client.generating());
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(server.requestCount, 2);
    QVERIFY(server.lastRequestBody.contains("segunda vuelta"));
    // Dos respuestas completas, una detrás de la otra, las dos con su motivo.
    QCOMPARE(tokenSpy.count(), CHAT_CAPTURE_TOKENS * 2);
    QCOMPARE(joinTokens(tokenSpy), CHAT_CAPTURE_TEXT + CHAT_CAPTURE_TEXT);
    QCOMPARE(finishedSpy.at(0).at(0).toString(), QString("stop"));
    QCOMPARE(finishedSpy.at(1).at(0).toString(), QString("stop"));
}

// Destruir el cliente con una respuesta a medio llegar: lo que pasa si se
// cierra Krampus mientras el modelo genera. Se comprueba que el destructor no
// emite ninguna signal (quien escucha es una interfaz QML a medio destruir) y
// que la conexión con el servidor se cierra. Además corre bajo
// AddressSanitizer, que aborta el programa con un reporte si NUESTRO código usa
// memoria ya liberada. (ASan no ve lo que pasa dentro de la librería de Qt: ver
// la nota sobre deleteLater() en releaseReply(), en KoboldClient.cpp.)
void TestKoboldClient::destroyClientMidStream()
{
    FakeKoboldServer server;
    QList<QByteArray> allChunks = chatCaptureChunks();
    // Unos pocos eventos (4, de a dos pedazos), y después el servidor se queda
    // callado con la conexión abierta: el cliente queda esperando en mitad del
    // stream.
    server.bodyChunks = allChunks.mid(0, 8);
    server.closeWhenDone = false;
    QSignalSpy disconnectedSpy(&server, &FakeKoboldServer::clientDisconnected);

    // Con `new` y no como variable local, para poder destruirlo con `delete`
    // en un punto exacto del test y seguir comprobando cosas después.
    KoboldClient* client = new KoboldClient();
    client->setBaseUrl(server.baseUrl());

    // `client` ya es un puntero, así que aquí va sin `&`. El spy sigue siendo
    // válido después de que el cliente se destruya: solo deja de anotar.
    QSignalSpy tokenSpy(client, &KoboldClient::tokenReceived);
    QSignalSpy finishedSpy(client, &KoboldClient::generationFinished);
    QSignalSpy failedSpy(client, &KoboldClient::generationFailed);
    QSignalSpy generatingSpy(client, &KoboldClient::generatingChanged);
    QSignalSpy errorSpy(client, &KoboldClient::errorMessageChanged);

    client->generate(userSays("hola"));
    QTRY_COMPARE_WITH_TIMEOUT(tokenSpy.count(), 4, WAIT_MS);
    QVERIFY(client->generating());
    // Hasta aquí, un solo cambio de `generating`: a verdadero.
    QCOMPARE(generatingSpy.count(), 1);

    // `delete` ejecuta el destructor y libera la memoria. La respuesta HTTP en
    // curso se cancela ahí adentro (ver ~KoboldClient).
    delete client;

    // El destructor canceló la respuesta sin avisar de nada a nadie.
    QCOMPARE(generatingSpy.count(), 1);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);

    // El servidor nota que el cliente cortó. Esta espera además hace correr el
    // bucle de eventos: si hubiera quedado algún aviso pendiente apuntando al
    // cliente ya destruido, se dispararía ahora y ASan lo reportaría.
    QTRY_COMPARE_WITH_TIMEOUT(disconnectedSpy.count(), 1, WAIT_MS);
}

// QTEST_GUILESS_MAIN escribe el `main()` de este ejecutable: crea la aplicación
// Qt, corre todos los tests de la clase y devuelve cuántos fallaron. "GUILESS"
// = sin interfaz gráfica: no abre ventanas, corre igual sin escritorio.
QTEST_GUILESS_MAIN(TestKoboldClient)

// Las dos clases de arriba llevan Q_OBJECT pero están en un .cpp, no en un .h.
// `moc` genera su código en un archivo aparte, y como no hay header que lo
// arrastre, hay que incluirlo a mano aquí, al final.
#include "tst_koboldclient.moc"
