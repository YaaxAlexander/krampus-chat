// tst_sseparser.cpp — tests automáticos de SseParser.
//
// SseParser no usa la red: recibe bytes y devuelve eventos. Así que aquí se le
// dan bytes a mano, cortados de todas las formas que la red podría cortarlos, y
// se comprueba que el resultado es siempre el mismo.
//
// El material de prueba son dos respuestas REALES de koboldcpp:
//   - logs/chat-sse-raw.txt: el endpoint de chat (/v1/chat/completions), el que
//     usa Krampus. Sus eventos NO traen línea `event:`.
//   - logs/sse-raw.txt: el endpoint nativo (/api/extra/generate/stream), que
//     Krampus ya no usa. Se sigue probando porque sus eventos SÍ traen
//     `event: message` y porque su traza mostró eventos partidos entre
//     lecturas: el parser tiene que aguantar las dos cosas.
//
// Cómo correrlos, desde la raíz del repo y después de compilar:
//   ctest --test-dir build --output-on-failure
// o directamente el ejecutable, que da más detalle:
//   ./build/tst_sseparser
//
// Qt Test es el `unittest` de Qt: una clase con un método por test, y macros
// (QCOMPARE, QVERIFY) en el papel de los `assert`.

#include "SseParser.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

// Ruta a la captura real. KRAMPUS_SOURCE_DIR no está en ningún .h: lo define
// CMake al compilar este archivo (ver target_compile_definitions en
// CMakeLists.txt) con la ruta absoluta del repo.
const QString REAL_CAPTURE_PATH = QString(KRAMPUS_SOURCE_DIR) + "/logs/sse-raw.txt";

// El texto que forman, unidos, los tokens de la captura. Es lo que el modelo
// respondió ese día, cortado a los 40 tokens que se le pidieron. Incluye la
// 'í' de "frío", que en la captura viene escapada como \u00ed.
const QString REAL_CAPTURE_TEXT =
    "El invierno llega con su frío y su nieve, y trae consigo un aire fresco y limpio."
    "  (La frase tiene 15 palabras y describe la sens";

// La captura trae 38 eventos con token y uno final con el motivo de fin.
const int REAL_CAPTURE_EVENTS = 39;

// La captura del endpoint de chat, el que usa Krampus.
const QString CHAT_CAPTURE_PATH = QString(KRAMPUS_SOURCE_DIR) + "/logs/chat-sse-raw.txt";

// El texto que forman, unidos, los `content` de la captura de chat. Aquí el
// modelo terminó solo (finish_reason "stop"). La 'í' de "días" viene escapada.
const QString CHAT_CAPTURE_TEXT =
    "El invierno se caracteriza por temperaturas bajas y días cortos de luz.";

// 16 eventos con texto, uno con finish_reason "stop" y el `data: [DONE]`.
const int CHAT_CAPTURE_EVENTS = 18;

// Los tamaños, en bytes, de cada lectura que hizo curl en
// logs/chat-sse-trace.txt (las líneas "Recv data"), en orden. Ahí cada lectura
// trajo justo un evento entero.
const QList<qsizetype> CHAT_TRACE_READ_SIZES = {
    242, 244, 245, 252, 243, 244, 247, 246, 242,
    244, 242, 250, 245, 242, 247, 241, 242, 14,
};

// --- Funciones de ayuda ---

// Lee un archivo de captura entero, como bytes.
QByteArray readCaptureFile(const QString& path)
{
    QFile file(path);
    // qFatal corta el programa con ese mensaje. Sin la captura ningún test
    // tiene sentido, y es mejor un error claro que treinta fallos confusos.
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("No se pudo abrir %s", qPrintable(path));
    }
    return file.readAll();
}

// Las dos capturas. Funciones de una línea para que los tests digan QUÉ leen
// sin repetir la ruta.
QByteArray readRealCapture()
{
    return readCaptureFile(REAL_CAPTURE_PATH);
}

QByteArray readChatCapture()
{
    return readCaptureFile(CHAT_CAPTURE_PATH);
}

// Le da `input` a un parser nuevo en pedazos de `chunkSize` bytes (el último
// puede ser más corto) y devuelve todos los eventos que salieron, en orden.
//
// `const QByteArray&` = referencia constante: se recibe el mismo objeto, sin
// copiarlo y sin poder modificarlo. `QList<SseEvent>` = lista de SseEvent.
QList<SseEvent> feedInChunks(const QByteArray& input, qsizetype chunkSize)
{
    SseParser parser;
    QList<SseEvent> allEvents;
    // `position += chunkSize` avanza de a un pedazo por vuelta.
    for (qsizetype position = 0; position < input.size(); position += chunkSize) {
        // mid(desde, cuántos): si quedan menos bytes que `cuántos`, devuelve
        // los que haya.
        QList<SseEvent> events = parser.feed(input.mid(position, chunkSize));
        // append de una lista a otra: el `extend` de Python.
        allEvents.append(events);
    }
    return allEvents;
}

// Interpreta el `data` de un evento como el JSON de koboldcpp.
QJsonObject jsonOf(const SseEvent& event)
{
    return QJsonDocument::fromJson(event.data).object();
}

// Une los tokens de todos los eventos: el texto que vería el usuario.
QString textOf(const QList<SseEvent>& events)
{
    QString text;
    // El `for x in lista` de Python.
    for (const SseEvent& event : events) {
        text += jsonOf(event).value("token").toString();
    }
    return text;
}

// Lo mismo para el formato del endpoint de chat, donde el texto está en
// choices[0].delta.content. El `[DONE]` del final no es JSON: fromJson()
// devuelve un documento vacío y de ahí sale un string vacío, que no suma nada.
QString chatTextOf(const QList<SseEvent>& events)
{
    QString text;
    for (const SseEvent& event : events) {
        QJsonObject choice = jsonOf(event).value("choices").toArray().at(0).toObject();
        text += choice.value("delta").toObject().value("content").toString();
    }
    return text;
}

// Comprueba lo que tiene que cumplir CUALQUIER troceo de la captura de chat:
// los 18 eventos, todos de tipo "message" aunque ninguno trae línea `event:`,
// el texto completo, y el final en su lugar. QVERIFY y QCOMPARE solo se pueden
// usar dentro del test mismo (hacen `return` al fallar), así que esta función
// devuelve un mensaje con el primer problema, o vacío si está todo bien.
QString checkChatEvents(const QList<SseEvent>& events)
{
    if (events.size() != CHAT_CAPTURE_EVENTS) {
        return QString("se esperaban %1 eventos y salieron %2").arg(CHAT_CAPTURE_EVENTS).arg(events.size());
    }
    for (const SseEvent& event : events) {
        if (event.type != "message") {
            return QString("evento de tipo «%1», se esperaba «message»").arg(QString::fromUtf8(event.type));
        }
    }
    if (chatTextOf(events) != CHAT_CAPTURE_TEXT) {
        return QString("texto distinto: «%1»").arg(chatTextOf(events));
    }
    // El penúltimo trae el motivo de fin; el último es la marca, sin JSON.
    QJsonObject finishChoice =
        jsonOf(events.at(events.size() - 2)).value("choices").toArray().at(0).toObject();
    if (finishChoice.value("finish_reason").toString() != "stop") {
        return "el penúltimo evento no trae finish_reason \"stop\"";
    }
    if (events.last().data != "[DONE]") {
        return QString("el último evento no es [DONE]: «%1»").arg(QString::fromUtf8(events.last().data));
    }
    return QString();
}

// La clase de tests. Hereda de QObject y lleva Q_OBJECT porque Qt Test descubre
// los tests buscando sus "private slots": cada uno es un test, y corren en el
// orden en que están escritos.
class TestSseParser : public QObject
{
    Q_OBJECT

private slots:
    void chatCaptureAllAtOnce();
    void chatCaptureByteByByte();
    void chatCaptureEveryChunkSize();
    void chatCaptureSplitLikeTheTrace();
    void realCaptureAllAtOnce();
    void realCaptureByteByByte();
    void realCaptureEveryChunkSize();
    void splitLikeTheRealTrace();
    void splitInsideLineAndInsideSeparator();
    void severalEventsInOneRead();
    void escapedNonAsciiStaysIntact();
    void rawUtf8SplitBetweenReads();
    void crlfLineEndings();
    void finalEventCarriesFinishReason();
    void toleratesWhatTheStandardAllows();
    void incompleteEventIsNotDelivered();
    void resetForgetsPartialEvent();
    void bufferHasALimit();
};

// Sobre los macros:
//   QCOMPARE(a, b) : falla el test si a != b, y muestra los dos valores.
//   QVERIFY(x)     : falla el test si x es falso.

// --- Endpoint de chat ---

// Toda la respuesta de chat en una sola llamada.
void TestSseParser::chatCaptureAllAtOnce()
{
    SseParser parser;
    QList<SseEvent> events = parser.feed(readChatCapture());

    QString problem = checkChatEvents(events);
    // isEmpty() = sin problemas. Si hay uno, el mensaje dice cuál.
    QVERIFY2(problem.isEmpty(), qPrintable(problem));

    // El primero, tal cual está en la captura: sin el prefijo "data: ".
    QVERIFY(events.first().data.startsWith("{\"id\": \"chatcmpl-A372044\""));
    QVERIFY(events.first().data.endsWith("\"content\": \"El\"}}]}"));
    QVERIFY(!parser.hasOverflowed());
}

// Un byte por llamada: cada línea y cada separador quedan partidos.
void TestSseParser::chatCaptureByteByByte()
{
    QString problem = checkChatEvents(feedInChunks(readChatCapture(), 1));
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

// Todos los tamaños de pedazo desde 2 hasta 300 bytes. Los eventos de chat
// miden entre 240 y 252 bytes, así que hay cortes en todas las posiciones
// respecto a sus límites, y también lecturas que traen más de un evento.
void TestSseParser::chatCaptureEveryChunkSize()
{
    QByteArray capture = readChatCapture();

    SseParser referenceParser;
    QList<SseEvent> reference = referenceParser.feed(capture);

    for (qsizetype chunkSize = 2; chunkSize <= 300; chunkSize++) {
        QList<SseEvent> events = feedInChunks(capture, chunkSize);

        QString context = QString("tamaño de pedazo: %1").arg(chunkSize);
        QString problem = checkChatEvents(events);
        QVERIFY2(problem.isEmpty(), qPrintable(context + ": " + problem));
        for (qsizetype i = 0; i < events.size(); i++) {
            QVERIFY2(events.at(i).type == reference.at(i).type, qPrintable(context));
            QVERIFY2(events.at(i).data == reference.at(i).data, qPrintable(context));
        }
    }
}

// Los cortes exactos de logs/chat-sse-trace.txt: un evento entero por lectura.
// Cada lectura debe entregar justo un evento, en el momento en que llega.
void TestSseParser::chatCaptureSplitLikeTheTrace()
{
    QByteArray capture = readChatCapture();

    // La traza y la captura son de la misma respuesta: los tamaños suman el
    // archivo entero. Si no, la tabla de arriba está mal copiada.
    qsizetype total = 0;
    for (qsizetype size : CHAT_TRACE_READ_SIZES) {
        total += size;
    }
    QCOMPARE(total, capture.size());

    SseParser parser;
    QList<SseEvent> allEvents;
    qsizetype position = 0;
    for (qsizetype size : CHAT_TRACE_READ_SIZES) {
        QList<SseEvent> events = parser.feed(capture.mid(position, size));
        QCOMPARE(events.size(), 1);
        allEvents.append(events);
        position += size;
    }

    QString problem = checkChatEvents(allEvents);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

// --- Endpoint nativo (ver el comentario del principio) ---

// El caso más simple: toda la respuesta en una sola llamada.
void TestSseParser::realCaptureAllAtOnce()
{
    SseParser parser;
    QList<SseEvent> events = parser.feed(readRealCapture());

    QCOMPARE(events.size(), REAL_CAPTURE_EVENTS);
    QCOMPARE(textOf(events), REAL_CAPTURE_TEXT);

    // Todos los eventos de koboldcpp son de tipo "message".
    for (const SseEvent& event : events) {
        QCOMPARE(event.type, QByteArray("message"));
    }
    // El primero, tal cual está en la captura: sin el prefijo "data: ".
    QCOMPARE(events.first().data, QByteArray("{\"token\": \"El\", \"finish_reason\": null}"));
    QVERIFY(!parser.hasOverflowed());
}

// El peor troceo posible: un byte por llamada. Cada línea, cada separador y
// cada carácter quedan partidos.
void TestSseParser::realCaptureByteByByte()
{
    QList<SseEvent> events = feedInChunks(readRealCapture(), 1);

    QCOMPARE(events.size(), REAL_CAPTURE_EVENTS);
    QCOMPARE(textOf(events), REAL_CAPTURE_TEXT);
}

// El mismo contenido con TODOS los tamaños de pedazo desde 2 hasta 200 bytes.
// Un evento mide entre 60 y 70, así que esto pone cortes en todas las
// posiciones imaginables respecto a los límites de evento. El resultado debe
// ser idéntico al de recibirlo todo junto.
void TestSseParser::realCaptureEveryChunkSize()
{
    QByteArray capture = readRealCapture();

    SseParser referenceParser;
    QList<SseEvent> reference = referenceParser.feed(capture);

    for (qsizetype chunkSize = 2; chunkSize <= 200; chunkSize++) {
        QList<SseEvent> events = feedInChunks(capture, chunkSize);

        // Si algo falla, este mensaje dice con qué tamaño fue. QVERIFY2 es
        // QVERIFY con un mensaje; qPrintable convierte el QString al texto
        // plano que el macro espera.
        QString context = QString("tamaño de pedazo: %1").arg(chunkSize);
        QVERIFY2(events.size() == reference.size(), qPrintable(context));
        for (qsizetype i = 0; i < events.size(); i++) {
            QVERIFY2(events.at(i).type == reference.at(i).type, qPrintable(context));
            QVERIFY2(events.at(i).data == reference.at(i).data, qPrintable(context));
        }
    }
}

// Los cortes que se ven en logs/sse-trace.txt: `event: message\n` llega solo
// (15 bytes) y la línea `data:` con su línea vacía llega en la lectura
// siguiente.
void TestSseParser::splitLikeTheRealTrace()
{
    SseParser parser;

    QList<SseEvent> afterFirstRead = parser.feed("event: message\n");
    // Con la mitad del evento todavía no hay nada que entregar.
    QCOMPARE(afterFirstRead.size(), 0);

    QList<SseEvent> afterSecondRead =
        parser.feed("data: {\"token\": \"El\", \"finish_reason\": null}\n\n");
    QCOMPARE(afterSecondRead.size(), 1);
    QCOMPARE(afterSecondRead.first().type, QByteArray("message"));
    QCOMPARE(textOf(afterSecondRead), QString("El"));
}

// Cortes que la traza no mostró pero que nada impide: en mitad de una línea, y
// justo entre los dos '\n' que cierran el evento.
void TestSseParser::splitInsideLineAndInsideSeparator()
{
    SseParser parser;

    QCOMPARE(parser.feed("event: mes").size(), 0);
    QCOMPARE(parser.feed("sage\ndata: {\"tok").size(), 0);
    QCOMPARE(parser.feed("en\": \" inv\", \"finish_reason\": null}").size(), 0);
    // Llega el primer '\n': la línea `data:` está completa, pero el evento no
    // termina hasta la línea vacía.
    QCOMPARE(parser.feed("\n").size(), 0);

    QList<SseEvent> events = parser.feed("\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().type, QByteArray("message"));
    QCOMPARE(textOf(events), QString(" inv"));
}

// Varios eventos en una sola lectura, más el principio del siguiente.
void TestSseParser::severalEventsInOneRead()
{
    SseParser parser;

    QList<SseEvent> events = parser.feed(
        "event: message\ndata: {\"token\": \"uno\", \"finish_reason\": null}\n\n"
        "event: message\ndata: {\"token\": \" dos\", \"finish_reason\": null}\n\n"
        "event: message\ndata: {\"token\": \" tres\", \"finish_reason\": null}\n\n"
        "event: message\ndata: {\"token\": \" cua");
    QCOMPARE(events.size(), 3);
    QCOMPARE(textOf(events), QString("uno dos tres"));

    // El cuarto quedó guardado a medias y se completa con la lectura siguiente.
    QList<SseEvent> rest = parser.feed("tro\", \"finish_reason\": null}\n\n");
    QCOMPARE(rest.size(), 1);
    QCOMPARE(textOf(rest), QString(" cuatro"));
}

// koboldcpp manda los caracteres no ASCII escapados dentro del JSON. El parser
// no debe tocarlos: entrega los bytes tal cual, y quien interpreta el JSON
// obtiene el carácter.
void TestSseParser::escapedNonAsciiStaysIntact()
{
    SseParser parser;
    // En un string de C++ la barra se escribe doble: `\\u00ed` son los seis
    // caracteres \ u 0 0 e d, que es lo que viaja por la red.
    QList<SseEvent> events =
        parser.feed("event: message\ndata: {\"token\": \"\\u00edo\", \"finish_reason\": null}\n\n");

    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().data, QByteArray("{\"token\": \"\\u00edo\", \"finish_reason\": null}"));
    QCOMPARE(textOf(events), QString("ío"));
}

// El caso para el que el parser trabaja con bytes y no con texto: UTF-8 crudo
// (sin escapar) y un corte de lectura en medio de un carácter. La 'í' son dos
// bytes, 0xC3 0xAD; el primero llega en una lectura y el segundo en la otra.
void TestSseParser::rawUtf8SplitBetweenReads()
{
    QByteArray rawEvent = "event: message\ndata: {\"token\": \"frío\", \"finish_reason\": null}\n\n";

    // Posición del primer byte de la 'í'. `\xC3` escribe un byte por su valor.
    qsizetype firstByteOfAccent = rawEvent.indexOf('\xC3');
    QVERIFY(firstByteOfAccent != -1);

    SseParser parser;
    // Primera lectura: hasta el primer byte de la 'í' INCLUSIVE.
    QCOMPARE(parser.feed(rawEvent.left(firstByteOfAccent + 1)).size(), 0);
    // Segunda lectura: empieza con el segundo byte de la 'í'.
    QList<SseEvent> events = parser.feed(rawEvent.mid(firstByteOfAccent + 1));

    QCOMPARE(events.size(), 1);
    QCOMPARE(textOf(events), QString("frío"));
    // U+FFFD es el carácter de reemplazo que aparece cuando se decodifica UTF-8
    // roto. Si el parser convirtiera cada pedazo a texto por separado, estaría.
    QVERIFY(!textOf(events).contains(QChar(0xFFFD)));
}

// Fin de línea "\r\n". koboldcpp usa "\n", pero el estándar SSE permite los dos.
void TestSseParser::crlfLineEndings()
{
    QByteArray capture = readRealCapture();
    // replace cambia todas las apariciones y devuelve el resultado.
    QByteArray captureWithCrlf = capture.replace("\n", "\r\n");

    // Entero, y byte a byte (que además separa cada '\r' de su '\n').
    QList<SseEvent> allAtOnce = feedInChunks(captureWithCrlf, captureWithCrlf.size());
    QList<SseEvent> byteByByte = feedInChunks(captureWithCrlf, 1);

    QCOMPARE(allAtOnce.size(), REAL_CAPTURE_EVENTS);
    QCOMPARE(textOf(allAtOnce), REAL_CAPTURE_TEXT);
    QCOMPARE(byteByByte.size(), REAL_CAPTURE_EVENTS);
    QCOMPARE(textOf(byteByByte), REAL_CAPTURE_TEXT);

    // El '\r' no debe quedar pegado a los valores.
    QCOMPARE(allAtOnce.first().type, QByteArray("message"));
    QCOMPARE(allAtOnce.first().data, QByteArray("{\"token\": \"El\", \"finish_reason\": null}"));
}

// El último evento del stream: token vacío y `finish_reason` con valor. Todos
// los anteriores lo traen en null. El parser no lo interpreta (eso lo hace
// KoboldClient); aquí se comprueba que el evento sale entero y que es el último.
void TestSseParser::finalEventCarriesFinishReason()
{
    SseParser parser;
    QList<SseEvent> events = parser.feed(readRealCapture());
    QCOMPARE(events.size(), REAL_CAPTURE_EVENTS);

    for (qsizetype i = 0; i < events.size() - 1; i++) {
        QVERIFY(jsonOf(events.at(i)).value("finish_reason").isNull());
    }

    QJsonObject last = jsonOf(events.last());
    QCOMPARE(last.value("finish_reason").toString(), QString("length"));
    QCOMPARE(last.value("token").toString(), QString(""));
}

// Cosas que koboldcpp no manda pero que el estándar SSE permite y un parser
// correcto no debe tomar por datos.
void TestSseParser::toleratesWhatTheStandardAllows()
{
    SseParser parser;
    QList<SseEvent> events = parser.feed(
        // Comentario (empieza con ':') y campos que no usamos.
        ": esto es un comentario\n"
        "id: 7\n"
        "retry: 1000\n"
        // Sin espacio después de los dos puntos.
        "data:sin espacio\n"
        "\n"
        // Líneas vacías de más: no deben producir eventos vacíos.
        "\n"
        "\n"
        // Solo se quita UN espacio; el segundo es parte del valor.
        "data:  dos espacios\n"
        "\n"
        // Dos líneas `data:` en un evento se unen con un salto de línea.
        "data: primera\n"
        "data: segunda\n"
        "\n");

    QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(0).data, QByteArray("sin espacio"));
    // Sin línea `event:` el tipo es "message", como manda el estándar.
    QCOMPARE(events.at(0).type, QByteArray("message"));
    QCOMPARE(events.at(1).data, QByteArray(" dos espacios"));
    QCOMPARE(events.at(2).data, QByteArray("primera\nsegunda"));
}

// Si la conexión se corta a mitad de un evento, ese evento nunca se entrega:
// un JSON a medias no sirve para nada.
void TestSseParser::incompleteEventIsNotDelivered()
{
    SseParser parser;
    QList<SseEvent> events = parser.feed(
        "event: message\ndata: {\"token\": \"uno\", \"finish_reason\": null}\n\n"
        "event: message\ndata: {\"token\": \" dos\", \"finish_reason\": null}\n");
    // El segundo tiene su línea `data:` completa, pero le falta la línea vacía.
    QCOMPARE(events.size(), 1);
    QCOMPARE(textOf(events), QString("uno"));
}

// reset() tira lo que estuviera a medias, para que un stream nuevo no herede
// restos del anterior.
void TestSseParser::resetForgetsPartialEvent()
{
    SseParser parser;
    QCOMPARE(parser.feed("event: viejo\ndata: resto del stream anter").size(), 0);

    parser.reset();

    QList<SseEvent> events = parser.feed("data: nuevo\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().data, QByteArray("nuevo"));
    // El `event: viejo` se olvidó: el evento nuevo tiene el tipo por defecto.
    QCOMPARE(events.first().type, QByteArray("message"));
}

// Si nunca llega un fin de evento, el parser no guarda bytes para siempre.
void TestSseParser::bufferHasALimit()
{
    SseParser parser;

    // 64 KiB de letras 'x' sin ningún salto de línea. El constructor
    // QByteArray(n, c) crea n copias del byte c.
    QByteArray junk(64 * 1024, 'x');

    // 32 pedazos = 2 MiB, más que el tope de 1 MiB.
    for (int i = 0; i < 32; i++) {
        QCOMPARE(parser.feed(junk).size(), 0);
    }
    QVERIFY(parser.hasOverflowed());

    // Desbordado, ignora lo que llegue, aunque sea un evento bien formado.
    QCOMPARE(parser.feed("data: tarde\n\n").size(), 0);

    // Lo mismo si lo que no termina nunca es un evento hecho de muchas líneas
    // `data:` (cada línea sí termina, pero la línea vacía no llega).
    parser.reset();
    QVERIFY(!parser.hasOverflowed());
    QByteArray dataLine = "data: " + QByteArray(64 * 1024, 'x') + "\n";
    for (int i = 0; i < 32; i++) {
        QCOMPARE(parser.feed(dataLine).size(), 0);
    }
    QVERIFY(parser.hasOverflowed());

    // Y tras reset() vuelve a funcionar.
    parser.reset();
    QVERIFY(!parser.hasOverflowed());
    QList<SseEvent> events = parser.feed("data: de nuevo\n\n");
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().data, QByteArray("de nuevo"));
}

// QTEST_GUILESS_MAIN escribe el `main()` de este ejecutable: crea la aplicación
// Qt, corre todos los tests de la clase y devuelve cuántos fallaron. "GUILESS"
// = sin interfaz gráfica: no abre ventanas, corre igual sin escritorio.
QTEST_GUILESS_MAIN(TestSseParser)

// La clase de arriba lleva Q_OBJECT pero está en un .cpp, no en un .h. `moc`
// genera su código en un archivo aparte, y como no hay header que lo arrastre,
// hay que incluirlo a mano aquí, al final.
#include "tst_sseparser.moc"
