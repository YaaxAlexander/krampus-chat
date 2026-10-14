// SseParser.cpp — el código de los métodos declarados en SseParser.h.
//
// El formato que se espera sale de mediciones reales contra koboldcpp: ver
// NOTES-backend.md ("Stream SSE"), logs/sse-raw.txt y logs/sse-trace.txt.

// Un .cpp siempre incluye su propio header: ahí está la declaración de la clase
// cuyos métodos se escriben aquí. Las comillas indican un header nuestro.
#include "SseParser.h"

// Tope de bytes que se guardan sin haber completado un evento. `const` = no se
// puede modificar; va aquí y no en el header porque nadie más lo necesita.
//
// Por qué hace falta: el parser guarda todo lo que llega hasta ver el final del
// evento. Si del otro lado hay algo que nunca manda ese final (un servidor que
// no es koboldcpp y responde con un archivo enorme, por ejemplo), la memoria
// crecería sin límite. 1 MiB es muchísimo más que cualquier evento real: los
// medidos tienen entre 60 y 70 bytes, un token cada uno.
const int MAX_PENDING_BYTES = 1024 * 1024;

// `SseParser::feed` se lee "el método feed de la clase SseParser". Como el
// código está fuera de la clase, cada método lleva el nombre de la clase y `::`
// por delante.
//
// --- El problema que resuelve ---
// La red avisa "llegaron bytes", NO "llegó un evento". Los bytes llegan en
// pedazos cortados donde sea. En la traza real (logs/sse-trace.txt) se ve de
// todo: un evento entero en una lectura,
//
//     "event: message\ndata: {\"token\": \" inv\", \"finish_reason\": null}\n\n"
//
// o partido en dos, primero 15 bytes y después el resto:
//
//     "event: message\n"
//     "data: {\"token\": \"El\", \"finish_reason\": null}\n\n"
//
// Y nada impide que un corte caiga en mitad de una línea ("data: {\"tok") o
// que una sola lectura traiga tres eventos juntos. Si se interpretara cada
// pedazo por separado, la mitad de las veces se leería basura.
//
// --- La solución ---
// Es el mismo patrón de ProcessManager::readAvailableOutput(), en dos niveles:
//   1. Lo recién llegado se AGREGA a un buffer que sobrevive entre llamadas.
//   2. Mientras el buffer tenga un '\n', hay una línea completa: se procesa.
//      Las líneas `event:` y `data:` se van anotando en "el evento en curso".
//   3. Una línea VACÍA significa "el evento en curso terminó": recién ahí se
//      entrega.
//   4. Lo que sobra sin '\n' (una línea a medias) se queda en el buffer.
//
// Alternativa descartada: buscar directamente el separador "\n\n" y cortar el
// evento entero de una vez. Parece más corto, pero el estándar SSE también
// permite terminar las líneas con "\r\n", y entonces el separador pasa a ser
// "\r\n\r\n": habría que buscar los dos. Yendo línea por línea, quitar un '\r'
// del final de cada línea cubre los dos casos sin código aparte.
//
// --- Por qué bytes y no texto ---
// En UTF-8 los caracteres no ASCII ocupan varios bytes: 'í' son dos (0xC3
// 0xAD). Un corte de lectura puede caer justo entre esos dos bytes. Si cada
// pedazo se convirtiera a QString (texto) al llegar, ese carácter saldría roto
// en los dos pedazos y ya no se podría arreglar. Por eso aquí TODO se maneja
// como QByteArray, y la conversión a texto la hace quien recibe el evento ya
// completo (KoboldClient, al interpretar el JSON).
//
// Cortar por '\n' sí es seguro a nivel de bytes: UTF-8 está diseñado para que
// el byte de '\n' (0x0A) nunca aparezca como parte de otro carácter.
//
// Con el koboldcpp medido esto no llega a pasar, porque manda los caracteres no
// ASCII escapados dentro del JSON ("\u00edo" en vez de "ío"), que es ASCII
// puro. Pero es un detalle de cómo arma el JSON esa versión, no una garantía
// del formato, y hacerlo bien no cuesta nada.
QList<SseEvent> SseParser::feed(const QByteArray& chunk)
{
    // La lista que se va a devolver. Empieza vacía.
    QList<SseEvent> events;

    // Tras un desborde se ignora todo hasta reset(): el stream ya no es fiable.
    if (m_overflowed) {
        return events;
    }

    m_buffer.append(chunk);

    // Posición del buffer a partir de la cual todavía no se procesó nada.
    // `qsizetype` es el tipo entero que Qt usa para tamaños y posiciones.
    //
    // Se avanza una posición en vez de ir borrando cada línea del buffer a
    // medida que se procesa, porque borrar del principio obliga a correr todos
    // los bytes que quedan: con muchas líneas en una sola lectura, se copiaría
    // el buffer una vez por línea. Así se borra una sola vez, al final.
    qsizetype lineStart = 0;

    // Se repite hasta que no queden líneas completas.
    while (true) {
        // Posición del primer '\n' desde lineStart, o -1 si no hay ninguno.
        qsizetype newlinePosition = m_buffer.indexOf('\n', lineStart);
        if (newlinePosition == -1) {
            break;
        }

        // mid(desde, cuántos) = un trozo del buffer: la línea SIN su '\n'.
        QByteArray line = m_buffer.mid(lineStart, newlinePosition - lineStart);
        // Si la línea terminaba en "\r\n", sobra un '\r'. chop(1) quita el
        // último byte.
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        lineStart = newlinePosition + 1;

        processLine(line, events);
    }

    // Quitar del buffer todo lo ya procesado. Queda solo la línea a medias (o
    // nada). remove(0, n) borra los primeros n bytes.
    m_buffer.remove(0, lineStart);

    // Válvula de seguridad (ver MAX_PENDING_BYTES arriba). Se cuenta lo que
    // está pendiente en los dos lugares: la línea a medias y el evento en curso.
    if (m_buffer.size() + m_currentData.size() > MAX_PENDING_BYTES) {
        m_overflowed = true;
        // clear() además devuelve la memoria, que es el punto de todo esto.
        m_buffer.clear();
        m_currentType.clear();
        m_currentData.clear();
        m_currentHasData = false;
    }

    return events;
}

void SseParser::reset()
{
    m_buffer.clear();
    m_currentType.clear();
    m_currentData.clear();
    m_currentHasData = false;
    m_overflowed = false;
}

bool SseParser::hasOverflowed() const
{
    return m_overflowed;
}

void SseParser::processLine(const QByteArray& line, QList<SseEvent>& events)
{
    // Línea vacía = fin del evento en curso.
    if (line.isEmpty()) {
        // Solo se entrega si tuvo alguna línea `data:`. El estándar SSE dice
        // que un evento sin datos se descarta; pasa, por ejemplo, con dos
        // líneas vacías seguidas.
        if (m_currentHasData) {
            SseEvent event;
            event.type = m_currentType;
            event.data = m_currentData;
            events.append(event);
        }
        // Con o sin entrega, el evento siguiente empieza de cero.
        m_currentType.clear();
        m_currentData.clear();
        m_currentHasData = false;
        return;
    }

    // Una línea que empieza con ':' es un comentario. Algunos servidores los
    // mandan cada tanto para que la conexión no se cierre por inactividad.
    // koboldcpp no lo hace, pero es parte del estándar y cuesta una línea.
    if (line.startsWith(':')) {
        return;
    }

    // El resto de las líneas son `campo: valor`. Se separan en el primer ':'.
    // (Dentro del valor puede haber más ':': el JSON está lleno.)
    qsizetype colonPosition = line.indexOf(':');
    QByteArray field;
    QByteArray value;
    if (colonPosition == -1) {
        // Sin ':' la línea entera es el nombre del campo, con valor vacío.
        field = line;
    } else {
        // left(n) = los primeros n bytes; mid(desde) = desde ahí hasta el final.
        field = line.left(colonPosition);
        value = line.mid(colonPosition + 1);
        // El estándar dice: si después del ':' hay un espacio, se quita UNO
        // (solo uno: si hubiera más, son parte del valor).
        if (value.startsWith(' ')) {
            value.remove(0, 1);
        }
    }

    if (field == "event") {
        m_currentType = value;
    } else if (field == "data") {
        // Un evento puede traer varias líneas `data:`; el estándar manda unirlas
        // con un salto de línea. koboldcpp manda siempre una sola.
        if (m_currentHasData) {
            m_currentData.append('\n');
        }
        m_currentData.append(value);
        m_currentHasData = true;
    }
    // Cualquier otro campo (`id:`, `retry:`, o uno desconocido) se ignora, que
    // es lo que el estándar pide para los campos que no se entienden. No es un
    // error tragado: no hay nada que hacer con ellos.
}
