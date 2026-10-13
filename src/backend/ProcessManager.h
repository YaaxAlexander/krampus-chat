// ProcessManager.h — declaración de la clase que lanza y vigila a koboldcpp.
//
// Única responsabilidad: el PROCESO. Lanzarlo, leer lo que imprime para saber si
// ya está listo o si falló, y garantizar que muere cuando Krampus cierra. No
// sabe nada de HTTP ni de chat: eso será KoboldClient.
//
// --- Header (.h) vs implementación (.cpp) ---
// En Python una clase vive entera en un archivo. En C++ se parte en dos:
//   * el header (.h, este archivo) DECLARA la clase: qué métodos y variables
//     tiene y de qué tipo, sin el código de los métodos. Es el "índice".
//   * el .cpp tiene el código de cada método.
// La razón es histórica y práctica: cada .cpp se compila por separado y sin ver
// a los demás. Para que main.cpp (u otro archivo) pueda usar ProcessManager, el
// compilador necesita saber qué forma tiene la clase; eso lo obtiene haciendo
// `#include "ProcessManager.h"`, que copia y pega este archivo ahí.

// `#pragma once` = "si este archivo ya se pegó en el .cpp que se está
// compilando, no lo pegues otra vez". Sin esto, si dos headers incluyen a este,
// la clase quedaría declarada dos veces y el compilador daría error. La forma
// antigua de lograr lo mismo son los "include guards" (#ifndef X / #define X /
// #endif); `#pragma once` hace lo mismo en una línea y lo soportan todos los
// compiladores que nos importan.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
// De aquí sale el macro QML_ELEMENT (ver abajo). Es un header diminuto, hecho a
// propósito para poder marcar clases para QML sin depender de todo el motor QML.
#include <QtQmlIntegration/qqmlintegration.h>

// "Forward declaration": le decimos al compilador "existe una clase llamada
// QTimer" sin incluir su header. Alcanza porque en este archivo solo guardamos
// un PUNTERO a QTimer (una dirección de memoria, que mide lo mismo sea cual sea
// la clase). El header completo se incluye en el .cpp, que es donde de verdad se
// usa. Menos includes en un header = se compila más rápido todo lo que lo incluye.
class QTimer;

// `class ProcessManager : public QObject` = ProcessManager HEREDA de QObject (en
// Python: `class ProcessManager(QObject):`). El `public` dice que la herencia es
// visible desde afuera: cualquiera puede tratar un ProcessManager como un
// QObject. En C++ hay que escribirlo; si se omite, la herencia es privada y Qt
// no puede trabajar con la clase.
//
// Heredar de QObject es lo que da signals/slots, propiedades visibles desde QML
// y el árbol de padres e hijos de Qt (ver m_process más abajo).
class ProcessManager : public QObject
{
    // Q_OBJECT: macro obligatorio en toda clase que declare signals, slots o
    // propiedades. C++ no tiene esos conceptos; Qt los añade con un generador
    // de código llamado `moc`, que lee este header, busca las clases marcadas
    // con Q_OBJECT y escribe un .cpp extra con la maquinaria (CMake lo corre
    // solo, ver AUTOMOC en CMakeLists.txt). Si se olvida, el error típico es
    // "undefined reference to vtable for ProcessManager" al enlazar.
    Q_OBJECT

    // QML_ELEMENT: registra esta clase como un TIPO de QML con su mismo nombre,
    // dentro del módulo `Krampus`. Con eso, en Main.qml se puede escribir
    // `ProcessManager { id: ... }` igual que se escribe `Label { }`.
    //
    // Alternativa descartada: crear el objeto en main.cpp y pasárselo al motor
    // con `engine.rootContext()->setContextProperty("processManager", &pm)`.
    // Funciona, pero (1) el nombre es un texto suelto que las herramientas de
    // QML no pueden revisar, (2) QML seguiría sin conocer el enum `State`, y
    // habría que registrarlo aparte de todos modos, y (3) son dos mecanismos
    // donde aquí hay uno.
    //
    // Qué implica para el ownership: el objeto lo crea el motor de QML cuando
    // construye la ventana, y lo destruye cuando destruye la ventana. Nadie en
    // C++ hace `new ProcessManager` ni `delete`. Ver el destructor en el .cpp
    // para lo que pasa con koboldcpp en ese momento.
    QML_ELEMENT

    // Q_PROPERTY declara una propiedad que QML puede leer (y a veces escribir)
    // por nombre, y sobre la que puede hacer "bindings": si en QML se escribe
    // `text: processManager.errorMessage`, ese texto se actualiza solo cada vez
    // que la propiedad cambia. Para eso cada propiedad dice:
    //   READ   : el método de C++ que devuelve el valor.
    //   WRITE  : el método que lo cambia (si no hay WRITE, QML solo puede leer).
    //   NOTIFY : la signal que se emite cuando el valor cambia. Es lo que
    //            despierta a los bindings; sin NOTIFY, QML lee el valor una vez
    //            y nunca se entera de que cambió.

    // En qué situación está el proceso. Solo lectura: lo decide esta clase.
    Q_PROPERTY(State state READ state NOTIFY stateChanged)

    // Explicación para el usuario de por qué falló. Vacío si no hay fallo.
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)

    // Nombre o ruta del ejecutable de koboldcpp. Si es un nombre sin carpetas
    // ("koboldcpp"), se busca en $PATH, igual que al escribirlo en la terminal.
    Q_PROPERTY(QString binaryPath READ binaryPath WRITE setBinaryPath NOTIFY binaryPathChanged)

    // Las últimas líneas que imprimió koboldcpp, unidas en un solo texto. Para
    // depurar: es lo que el usuario mira cuando el mensaje de error no alcanza.
    Q_PROPERTY(QString recentOutput READ recentOutput NOTIFY recentOutputChanged)

// `public:` = todo lo que sigue se puede usar desde fuera de la clase. En Python
// todo es público y lo privado se marca por convención con `_`; en C++ el
// compilador lo hace cumplir.
public:
    // Los estados posibles del proceso.
    //
    // `enum class` = un tipo cuyos únicos valores posibles son estos nombres (lo
    // más parecido en Python es `enum.Enum`). Por dentro son enteros 0, 1, 2...
    // El `class` hace dos cosas respecto al `enum` a secas, heredado de C: hay
    // que escribir `State::Loading` (los nombres no se "derraman" al resto del
    // código) y no se convierte a int por accidente.
    //
    // Por qué un enum y no un QString ("loading") o varios bool (isLoading,
    // isReady...): con un string, un error de tipeo en QML no avisa, solo no
    // funciona; y con varios bool se pueden representar combinaciones
    // imposibles (cargando Y listo). Con un enum hay exactamente un estado a la
    // vez y un nombre mal escrito en C++ es error de compilación.
    //
    // En QML los valores se escriben `ProcessManager.Loading`. QML exige que
    // empiecen con mayúscula.
    enum class State {
        // No hay proceso. Estado inicial, y al que se vuelve tras `stop()`.
        NotRunning,
        // Se pidió arrancar y koboldcpp todavía no imprimió la línea de "listo".
        // Casi siempre el proceso ya está corriendo; la excepción es un arranque
        // anotado para la siguiente vuelta del bucle de eventos (ver `start()`).
        Loading,
        // koboldcpp ya escucha en su puerto: se le puede hablar por HTTP.
        Ready,
        // Algo salió mal y ya NO hay proceso. `errorMessage` dice qué.
        Failed,
        // Se pidió detener y el proceso todavía no terminó de cerrarse (tarda
        // hasta unos segundos). Existe para que la UI no ofrezca "Cargar"
        // mientras el proceso viejo sigue vivo ocupando la VRAM.
        Stopping
    };
    // Q_ENUM le cuenta el enum al sistema de Qt: es lo que permite usarlo como
    // tipo de una Q_PROPERTY, leerlo desde QML, y que al imprimirlo en un log o
    // en un test fallido salga "Loading" en vez de "1".
    Q_ENUM(State)

    // El constructor (el `__init__`).
    //
    // `QObject* parent` es un PUNTERO a QObject: la dirección de memoria de otro
    // objeto, o `nullptr` ("ninguno", el None de los punteros). Todo QObject
    // puede tener un padre; cuando el padre se destruye, destruye a sus hijos.
    // `= nullptr` es un valor por defecto, como en Python.
    //
    // `explicit` prohíbe que C++ use este constructor para convertir tipos sin
    // que se lo pidan. Sin `explicit`, cualquier función que espere un
    // ProcessManager aceptaría un `QObject*` suelto y C++ fabricaría un
    // ProcessManager temporal en silencio. Regla práctica: todo constructor que
    // se pueda llamar con un solo argumento lleva `explicit`.
    explicit ProcessManager(QObject* parent = nullptr);

    // El destructor: se ejecuta solo, justo antes de que el objeto deje de
    // existir. Python tiene `__del__`, pero no garantiza cuándo corre; en C++ el
    // momento es exacto, y por eso es el lugar correcto para la limpieza
    // obligatoria: aquí se asegura que koboldcpp muera.
    //
    // `override` = "este método reemplaza a uno de la clase base (QObject)". No
    // cambia el comportamiento; hace que el compilador avise si en realidad no
    // se está reemplazando nada (por un error de tipeo en la firma, por ejemplo).
    ~ProcessManager() override;

    // --- Lectura de las propiedades (los READ de arriba) ---
    //
    // El `const` DESPUÉS de los paréntesis promete que el método no modifica al
    // objeto: solo lee. El compilador lo verifica. No existe en Python.
    State state() const;
    QString errorMessage() const;
    QString binaryPath() const;
    QString recentOutput() const;

    // `const QString&` = referencia constante. Una referencia (`&`) es otro
    // nombre para un objeto que ya existe: se pasa "el mismo string", no una
    // copia. El `const` de ADELANTE promete que no lo vamos a modificar. En
    // Python los objetos siempre se pasan así y no hay que pensarlo; en C++, sin
    // el `&`, se copiaría el string entero en cada llamada.
    //
    // Diferencia con un puntero (`QString*`): una referencia nunca es "ninguno"
    // y se usa con la sintaxis normal (`path.isEmpty()`, no `path->isEmpty()`).
    void setBinaryPath(const QString& path);

    // PID del proceso de koboldcpp, o 0 si no hay ninguno. No es una propiedad
    // de QML: lo usan los tests para comprobar que el proceso murió de verdad.
    // `qint64` = entero de 64 bits; es el tipo en que QProcess entrega el PID.
    qint64 processId() const;

    // Q_INVOKABLE hace que un método normal de C++ se pueda LLAMAR desde QML
    // (`processManager.start(...)`). Sin ese macro QML no lo ve.

    // Lanza `<binaryPath> --model <modelPath> --port <port>`. No espera a que
    // cargue: regresa enseguida y el resultado llega después por `state`.
    // Si ya hay un proceso vivo no hace nada (solo avisa en el log).
    Q_INVOKABLE void start(const QString& modelPath, int port);

    // Pide a koboldcpp que cierre. Tampoco espera: el estado pasa a Stopping y,
    // cuando el proceso termina, a NotRunning. Detener a pedido NO es un fallo.
    Q_INVOKABLE void stop();

// Las signals son avisos que el objeto emite ("me pasó esto") sin saber quién
// escucha. Quien quiera enterarse se conecta. Aquí solo se DECLARAN, como si
// fueran métodos; el cuerpo lo escribe `moc`. Se emiten con `emit nombre();`.
// Las de aquí son todas NOTIFY de las propiedades, así que quien escucha es QML.
signals:
    void stateChanged();
    void errorMessageChanged();
    void binaryPathChanged();
    void recentOutputChanged();

// `private:` = solo el código de esta misma clase puede usar lo que sigue.
private:
    // Un "slot" es un método al que se conecta una signal: cuando la signal se
    // emite, Qt llama al método. Estos tres reciben los avisos del QProcess. Las
    // conexiones se hacen en el constructor (ver `connect` en el .cpp).
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onErrorOccurred(QProcess::ProcessError error);

    // Se dispara si, tras pedir el cierre, el proceso no terminó a tiempo.
    void onStopTimeout();
    // Ejecuta un start() que quedó anotado para después (ver m_startPending).
    void runPendingStart();

    // Lee lo que haya disponible y procesa las líneas COMPLETAS (ver el .cpp).
    void readAvailableOutput();
    // Mira una línea ya completa: ¿es la de "listo"? ¿una pista de fallo?
    void processLine(const QString& line);

    // Cambian el estado / el mensaje y emiten la signal solo si de verdad cambió.
    void setState(State newState);
    void setErrorMessage(const QString& message);
    // Atajo: pone el mensaje y pasa a Failed.
    void fail(const QString& message);

    // --- Variables miembro ---
    // El prefijo `m_` ("member") es una convención de Qt para distinguirlas de
    // las variables locales; cumple el papel del `self.` de Python.

    // `= State::NotRunning` es el valor inicial, como asignarlo en `__init__`.
    // En C++ una variable de tipo básico SIN valor inicial contiene basura (lo
    // que hubiera en esa memoria), no None ni 0, así que siempre se inicializa.
    State m_state = State::NotRunning;
    QString m_errorMessage;
    QString m_binaryPath = "koboldcpp";

    // El proceso hijo. Es un puntero porque el objeto se crea con `new` en el
    // constructor, pasándole `this` como padre.
    // NO lleva std::unique_ptr ni se le hace `delete` a mano: Qt tiene su propio
    // sistema de ownership, y cuando este ProcessManager se destruye, borra a
    // sus hijos. Si además lo borrara un smart pointer, se borraría dos veces
    // ("double free"), que es un crash.
    // `= nullptr` hasta que el constructor lo cree, por la regla de arriba.
    QProcess* m_process = nullptr;

    // Temporizador para el plan B de `stop()`. Mismo ownership: hijo de `this`.
    QTimer* m_stopTimer = nullptr;

    // Bytes ya leídos del proceso que todavía NO forman una línea completa.
    // QByteArray = bytes crudos (el `bytes` de Python), no texto. La explicación
    // larga está en readAvailableOutput().
    QByteArray m_outputBuffer;

    // Las últimas líneas impresas por koboldcpp, con tope de cantidad.
    // QStringList = lista de strings (`list[str]`).
    QStringList m_recentLines;

    // Pistas vistas en la salida de ESTE arranque. Se guardan como banderas y se
    // interpretan al final, cuando el proceso termina, para no depender del
    // orden en que llegaron las líneas (ver onFinished en el .cpp).
    bool m_sawLoadFailed = false;
    bool m_sawOutOfMemory = false;

    // Un start() pedido en un momento en que no se podía lanzar (desde dentro
    // del aviso de "el binario no arrancó"). Se anota aquí y se ejecuta apenas
    // el bucle de eventos queda libre. La explicación está en start(), en el .cpp.
    //   m_insideErrorSignal : estamos avisando de ese fallo ahora mismo.
    //   m_startPending      : hay un pedido anotado esperando su turno.
    bool m_insideErrorSignal = false;
    bool m_startPending = false;
    QString m_pendingModelPath;
    int m_pendingPort = 0;

    // El patrón de la línea de "listo", compilado una sola vez (compilar una
    // expresión regular es caro; usarla es barato). Equivale a `re.compile()`.
    QRegularExpression m_readyPattern;
};
