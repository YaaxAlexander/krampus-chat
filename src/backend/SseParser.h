// SseParser.h — junta los bytes de un stream SSE hasta tener eventos completos.
//
// Única responsabilidad: recibir bytes tal como llegan de la red, en pedazos
// cortados en cualquier lugar, y devolver los eventos que ya están ENTEROS. No
// sabe nada de red, ni de JSON, ni de koboldcpp: por eso se puede probar solo,
// dándole bytes a mano (ver tests/tst_sseparser.cpp). Quien lo usa es
// KoboldClient.
//
// --- Qué es SSE ---
// "Server-Sent Events" es un formato de texto para que un servidor HTTP mande
// una respuesta que no termina de golpe, sino que va llegando de a poco. Es lo
// que usa koboldcpp para mandar un token a la vez. Cada evento son unas líneas
// de la forma `campo: valor` y termina con una línea vacía. Lo que manda
// koboldcpp, medido (ver NOTES-backend.md, "Stream SSE", y logs/sse-raw.txt):
//
//     event: message
//     data: {"token": " inv", "finish_reason": null}
//     <línea vacía>
//
// Qt no trae un lector de SSE, así que se hace a mano aquí.
//
// --- Header (.h) vs implementación (.cpp) ---
// Este archivo DECLARA la clase (qué métodos y variables tiene); el código de
// los métodos está en SseParser.cpp. La explicación larga de por qué C++ parte
// las clases en dos archivos está en ProcessManager.h.

// "Si este archivo ya se pegó en el .cpp que se está compilando, no lo pegues
// otra vez" (ver ProcessManager.h).
#pragma once

#include <QByteArray>
#include <QList>

// Un evento SSE ya completo.
//
// `struct` es una clase pensada para solo agrupar datos, sin comportamiento: lo
// más parecido en Python es un `@dataclass`. (La única diferencia técnica con
// `class` es que en un struct todo es público si no se dice otra cosa.)
//
// Los dos campos son QByteArray = bytes crudos, el `bytes` de Python, y NO
// QString (texto). Por qué bytes: ver la explicación de feed() en el .cpp.
struct SseEvent {
    // El valor de la línea `event:`. koboldcpp siempre manda "message". Si el
    // evento no trae esa línea queda vacío (el estándar SSE dice que entonces
    // el tipo es "message").
    QByteArray type;

    // El valor de la línea `data:`, sin el prefijo. Para koboldcpp, un JSON.
    QByteArray data;
};

// Esta clase NO hereda de QObject ni lleva Q_OBJECT: no tiene signals, ni
// propiedades, ni la usa QML. Es una clase de C++ a secas, y eso la hace más
// fácil de crear y de probar. Regla práctica: QObject solo cuando hace falta
// algo de lo que QObject da.
class SseParser
{
// `public:` = lo que sigue se puede usar desde fuera de la clase.
public:
    // Recibe un pedazo de bytes recién leído de la red y devuelve los eventos
    // que con ese pedazo quedaron completos: ninguno, uno o varios. Lo que
    // sobra (un evento a medias) se guarda adentro para la próxima llamada.
    //
    // `const QByteArray&` = referencia constante: se recibe el mismo objeto del
    // que llama, sin copiarlo (`&`), y con la promesa de no modificarlo
    // (`const`). En Python los objetos siempre se pasan así; en C++ hay que
    // pedirlo.
    //
    // `QList<SseEvent>` = una lista cuyos elementos son todos SseEvent; en
    // Python se anotaría `list[SseEvent]`. Lo que va entre `< >` es el tipo de
    // los elementos: en C++ una lista no puede mezclar tipos, y hay que decir
    // de qué es. (QList es una "plantilla" que trae Qt; usarla así es normal,
    // lo que el proyecto evita es ESCRIBIR plantillas propias.)
    //
    // La lista se devuelve "por valor", sin `&`: el que llama recibe su propia
    // lista. No es caro: C++ la entrega sin copiar los elementos.
    //
    // Alternativa descartada: avisar de cada evento con una signal. Obligaría a
    // que esta clase fuera un QObject, y devolver una lista es más simple de
    // usar y de probar.
    QList<SseEvent> feed(const QByteArray& chunk);

    // Olvida todo lo acumulado. Se llama antes de empezar un stream nuevo, para
    // que no se mezclen restos del anterior.
    void reset();

    // Verdadero si se recibieron demasiados bytes sin que se completara un
    // evento (ver MAX_PENDING_BYTES en el .cpp). Lo que hay del otro lado no
    // está hablando SSE, o está roto. Desde ese momento feed() ignora todo
    // hasta que se llame a reset().
    //
    // El `const` DESPUÉS de los paréntesis promete que el método no modifica al
    // objeto: solo lee. El compilador lo verifica.
    bool hasOverflowed() const;

// `private:` = solo el código de esta misma clase puede usar lo que sigue.
private:
    // Procesa una línea ya completa y sin su salto de línea. Si la línea es la
    // vacía que cierra un evento, lo agrega a `events`.
    //
    // `QList<SseEvent>&` (referencia SIN const): se recibe la misma lista del
    // que llama y aquí sí se la modifica, agregándole elementos.
    void processLine(const QByteArray& line, QList<SseEvent>& events);

    // Bytes recibidos que todavía no forman una línea completa (les falta el
    // '\n'). El prefijo `m_` marca las variables miembro, como el `self.` de
    // Python.
    QByteArray m_buffer;

    // El evento que se está armando: sus líneas ya llegaron, pero todavía no
    // llegó la línea vacía que lo cierra.
    QByteArray m_currentType;
    QByteArray m_currentData;
    // ¿El evento en curso ya tiene alguna línea `data:`? Hace falta aparte de
    // m_currentData porque un `data:` vacío es válido y distinto de "no hubo".
    // `= false`: en C++ un bool sin valor inicial contiene basura, no False.
    bool m_currentHasData = false;

    bool m_overflowed = false;
};
