// ProcessManager.cpp — el código de los métodos declarados en ProcessManager.h.
//
// Qué se busca en la salida de koboldcpp, y por qué, sale de mediciones reales:
// ver NOTES-backend.md y los logs de logs/.

// Las comillas (en vez de < >) indican un header NUESTRO, que se busca primero
// junto a este archivo. Un .cpp siempre incluye su propio header: ahí está la
// declaración de la clase cuyos métodos se escriben aquí.
#include "ProcessManager.h"

#include <QDebug>
#include <QTimer>

// Constantes de este archivo. `const` = no se puede modificar después. Van aquí
// y no en el header porque nadie fuera de este .cpp las necesita. (Una variable
// `const` declarada fuera de toda función es visible solo dentro de su .cpp.)

// Cuántas líneas de salida se conservan para mostrar. Una carga normal imprime
// unas 1000 líneas (858 son ruido repetido) y koboldcpp sigue imprimiendo
// mientras genera, así que guardarlo todo crecería sin límite. Se guarda solo
// el final porque es donde está lo útil para depurar: cuando algo falla, la
// causa está en las últimas líneas. 200 es holgado para eso (el log de la carga
// fallida por VRAM mide 142 líneas en total) y sigue siendo poco texto.
const int MAX_RECENT_LINES = 200;

// Tope para una "línea" que nunca termina. Mientras genera, koboldcpp reescribe
// su contador de tokens sin imprimir salto de línea, así que el buffer de línea
// incompleta puede crecer mucho. Pasado este tamaño se procesa como si fuera una
// línea, para que la memoria tenga un techo. 64 KiB: la línea real más larga
// medida (el volcado `Namespace(...)`) tiene unos 3 KiB.
const int MAX_PARTIAL_LINE_BYTES = 64 * 1024;

// Cuánto se espera a que koboldcpp cierre por las buenas antes de matarlo.
// QProcess::terminate() manda SIGTERM, y está medido contra el koboldcpp real
// (NOTES-backend.md, "Apagado"): ya listo, muere en unos 260 ms y libera la
// VRAM. 5 s es margen de sobra. El plan B de kill() queda como red de seguridad
// para un proceso colgado que no atienda la señal; no es el camino normal.
const int STOP_TIMEOUT_MS = 5000;

// Cuánto se espera después de kill(). SIGKILL no se puede ignorar, así que esto
// es solo el tiempo que tarda el sistema en confirmar la muerte.
const int KILL_WAIT_MS = 2000;

// `ProcessManager::ProcessManager` se lee "el método ProcessManager (el
// constructor) de la clase ProcessManager". Como el código está fuera de la
// clase, cada método lleva el nombre de la clase y `::` por delante.
//
// Lo que va después de los dos puntos es la "lista de inicialización": da valor
// a la clase base y a las variables miembro ANTES de que empiece el cuerpo `{}`.
//   QObject(parent)   : llama al constructor de la clase base (el
//                       `super().__init__(parent)` de Python).
//   m_readyPattern(...) : construye la expresión regular ya con su patrón.
//
// El patrón NO lleva el puerto escrito: koboldcpp imprime el que se le pasó con
// --port. `\\d+` = uno o más dígitos; la barra va doble porque en un string de
// C++ `\` es carácter de escape (en Python se evitaría con r"...").
ProcessManager::ProcessManager(QObject* parent)
    : QObject(parent)
    , m_readyPattern("Starting Kobold API on port \\d+")
{
    // `new` crea el objeto en el "heap": memoria que NO se libera sola al salir
    // de esta función (a diferencia de una variable local). Alguien tiene que
    // borrarlo; aquí ese alguien es Qt, porque le pasamos `this` (un puntero a
    // este mismo ProcessManager, el `self` de Python) como padre.
    m_process = new QProcess(this);

    // koboldcpp reparte sus mensajes entre stdout y stderr (la parte en Python
    // usa uno, la parte en C usa otro). MergedChannels los junta en un solo
    // flujo, como `2>&1` en la shell, y así hay un solo lugar donde leer.
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    m_stopTimer = new QTimer(this);
    // SingleShot: cada vez que se arma dispara una sola vez, no en repetición.
    m_stopTimer->setSingleShot(true);
    m_stopTimer->setInterval(STOP_TIMEOUT_MS);

    // `connect` une una signal con un slot: "cuando ESTE objeto emita ESTA
    // signal, llama a ESTE método de ESTE otro objeto". Los cuatro argumentos
    // son: emisor, signal, receptor, slot.
    //
    // `&QProcess::readyRead` es un "puntero a método": no llama al método, lo
    // nombra, para que Qt lo guarde y lo llame después. El compilador verifica
    // que la signal y el slot existan y que sus argumentos sean compatibles.
    //
    // Nada de esto se ejecuta ahora. Los slots se llaman más tarde, desde el
    // bucle de eventos (el `app.exec()` de main.cpp), cuando el proceso tenga
    // algo que decir. Por eso start() puede regresar enseguida sin congelar la
    // ventana mientras el modelo carga.

    // readyRead: el proceso imprimió algo nuevo.
    connect(m_process, &QProcess::readyRead, this, &ProcessManager::onReadyRead);
    // finished: el proceso terminó (bien, mal o porque lo mataron).
    connect(m_process, &QProcess::finished, this, &ProcessManager::onFinished);
    // errorOccurred: QProcess no pudo hacer algo; nos importa "no arrancó".
    connect(m_process, &QProcess::errorOccurred, this, &ProcessManager::onErrorOccurred);

    connect(m_stopTimer, &QTimer::timeout, this, &ProcessManager::onStopTimeout);
}

// El destructor. Corre cuando Krampus cierra normalmente (el motor de QML
// destruye la ventana y, con ella, este objeto).
//
// FALTA (Fase 2): esto NO cubre el caso de que Krampus crashee o lo maten con
// `kill -9`. Ahí ningún destructor corre y koboldcpp queda huérfano, ocupando
// la VRAM. Resolverlo necesita process groups y signal handlers de POSIX.
ProcessManager::~ProcessManager()
{
    // Primero se cortan TODAS las conexiones del proceso hacia este objeto.
    //
    // Por qué: las esperas de más abajo (waitForFinished) hacen que QProcess
    // emita `readyRead` y `finished` ahí mismo. Sin desconectar, nuestros slots
    // correrían y emitirían `stateChanged` hacia una interfaz QML que está a
    // medio destruir, que intentaría re-evaluar sus bindings contra objetos que
    // quizá ya no existen. Ese tipo de bug es un "use after free" (usar memoria
    // ya liberada): a veces crashea y a veces no, y ASan es quien lo delata.
    // Durante la destrucción ya no le interesa a nadie en qué estado quedamos.
    //
    // `->` es el `.` para punteros: "el método disconnect del objeto al que
    // apunta m_process".
    m_process->disconnect(this);

    if (m_process->state() == QProcess::NotRunning) {
        return;
    }

    // Pedir el cierre por las buenas (SIGTERM) y esperar un tiempo acotado.
    //
    // waitForFinished BLOQUEA: no regresa hasta que el proceso termina o pasa
    // el tiempo. En cualquier otro lugar de una app gráfica eso está prohibido
    // (congela la ventana); aquí es lo correcto, porque la ventana ya se está
    // cerrando y no podemos irnos dejando al proceso vivo.
    // Devuelve false si se agotó el tiempo.
    m_process->terminate();
    bool finishedInTime = m_process->waitForFinished(STOP_TIMEOUT_MS);

    if (!finishedInTime) {
        // Plan B: SIGKILL. El sistema operativo mata al proceso sin
        // preguntarle; no puede ignorarlo ni limpiar nada.
        qWarning() << "ProcessManager: koboldcpp no cerró en" << STOP_TIMEOUT_MS
                   << "ms tras SIGTERM; se mata con SIGKILL.";
        m_process->kill();
        m_process->waitForFinished(KILL_WAIT_MS);
    }

    // Aquí NO hay `delete m_process`: al terminar este destructor corre el de
    // QObject, que borra a los hijos (m_process y m_stopTimer).
}

ProcessManager::State ProcessManager::state() const
{
    return m_state;
}

QString ProcessManager::errorMessage() const
{
    return m_errorMessage;
}

QString ProcessManager::binaryPath() const
{
    return m_binaryPath;
}

QString ProcessManager::recentOutput() const
{
    // Une las líneas con saltos de línea: el "\n".join(lista) de Python.
    // El texto se arma aquí, cuando alguien lo pide, en vez de mantenerlo
    // armado: así agregar una línea es barato y el costo de unir se paga una
    // sola vez por lectura.
    return m_recentLines.join('\n');
}

void ProcessManager::setBinaryPath(const QString& path)
{
    // Si no cambió, no se emite la signal. Es la convención de Qt para todos los
    // setters: evita que QML re-evalúe bindings para nada, y corta los ciclos
    // infinitos cuando dos propiedades están enlazadas entre sí.
    if (m_binaryPath == path) {
        return;
    }
    m_binaryPath = path;
    emit binaryPathChanged();
}

qint64 ProcessManager::processId() const
{
    return m_process->processId();
}

void ProcessManager::start(const QString& modelPath, int port)
{
    // Se pregunta al QProcess y no a m_state porque lo que importa es si hay un
    // proceso VIVO, sea cual sea nuestro estado (también lo hay en Stopping).
    if (m_process->state() != QProcess::NotRunning) {
        qWarning() << "ProcessManager: start() ignorado, ya hay un proceso de koboldcpp vivo.";
        return;
    }

    // Protección contra la REENTRANCIA: que nos llamen mientras este mismo
    // objeto está a mitad de otra cosa.
    //
    // Una signal de Qt es una llamada a función normal: `emit` no regresa hasta
    // que corrieron todos los slots conectados. Cuando el binario no existe,
    // QProcess emite `errorOccurred`, eso llama a onErrorOccurred(), que emite
    // `stateChanged`; si quien escucha reacciona llamando a start() (reintentar
    // con otro binario), estamos aquí con QProcess todavía DENTRO de su propia
    // emisión, sin haber terminado su limpieza interna del arranque fallido.
    // Lanzar el mismo QProcess en ese momento deja a Qt (6.12) bloqueado en un
    // read() mientras viva el proceso nuevo: la ventana se congela. No da
    // ningún error; se ve con gdb (`bt` muestra QSocketNotifier -> read).
    //
    // Arreglo: en ese caso no se lanza ahora. Se ANOTA el pedido y se ejecuta
    // en la siguiente vuelta del bucle de eventos, cuando QProcess ya terminó.
    //
    // Alternativas descartadas:
    //   * Conectar `errorOccurred` en modo "en cola" (Qt::QueuedConnection): un
    //     aviso de fallo viejo que siguiera en la cola podría tumbar a un
    //     arranque nuevo que no tiene nada que ver.
    //   * Retrasar el propio paso a Failed: mismo problema al revés, porque en
    //     ese hueco puede llegar un start() válido y el Failed atrasado lo pisa.
    if (m_insideErrorSignal) {
        m_pendingModelPath = modelPath;
        m_pendingPort = port;
        m_startPending = true;
        // Para quien mira el estado, el pedido ya está aceptado.
        setState(State::Loading);
        // singleShot con 0 ms no significa "ya": significa "en cuanto el bucle
        // de eventos quede libre", o sea después de que regrese toda la cadena
        // de llamadas actual. Pasar `this` hace que Qt lo cancele solo si este
        // objeto se destruye antes.
        QTimer::singleShot(0, this, &ProcessManager::runPendingStart);
        return;
    }
    // Un start() normal reemplaza a uno anotado que todavía no se ejecutó: vale
    // el pedido más reciente.
    m_startPending = false;

    // Borrar todo lo que quedó del arranque anterior.
    m_outputBuffer.clear();
    m_sawLoadFailed = false;
    m_sawOutOfMemory = false;
    m_recentLines.clear();
    emit recentOutputChanged();
    setErrorMessage(QString());

    // Los argumentos van como LISTA, un elemento por argumento, igual que
    // `subprocess.run([...])` en Python. No pasan por una shell, así que una
    // ruta con espacios o comillas llega intacta y no hay que escaparla.
    //
    // Solo --model y --port: sin --gpulayers ni --contextsize, koboldcpp decide
    // esos valores solo ("autofit"). Los controles para fijarlos son de la Fase 3.
    QStringList arguments;
    arguments << "--model" << modelPath;
    arguments << "--port" << QString::number(port);

    // El estado se cambia ANTES de lanzar para que quien escuche vea primero
    // "cargando" y después lo que venga.
    setState(State::Loading);

    // start() NO espera a que el programa arranque: regresa enseguida. Si el
    // ejecutable no existe, nos enteramos después por `errorOccurred`.
    // Una ruta de modelo relativa se resuelve contra la carpeta desde la que se
    // lanzó Krampus, porque el proceso hijo hereda nuestra carpeta de trabajo.
    m_process->start(m_binaryPath, arguments);
}

// Ejecuta el start() que quedó anotado (ver la protección contra reentrancia
// en start()). Ya corre desde el bucle de eventos, fuera de cualquier signal.
void ProcessManager::runPendingStart()
{
    // Puede haberse cancelado en el camino: un stop(), u otro start() más nuevo.
    if (!m_startPending) {
        return;
    }
    m_startPending = false;
    start(m_pendingModelPath, m_pendingPort);
}

void ProcessManager::stop()
{
    // Si hay un arranque anotado que todavía no se lanzó, detener es cancelarlo.
    if (m_startPending) {
        m_startPending = false;
        // El arranque anotado nació de un fallo anterior, y su mensaje sigue
        // puesto. Lo borramos: en NotRunning no debe quedar ningún error visible.
        setErrorMessage(QString());
        setState(State::NotRunning);
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    if (m_state == State::Stopping) {
        // Ya se pidió; el temporizador del plan B sigue corriendo.
        return;
    }

    // Desde este momento, que el proceso termine es lo esperado y no un fallo.
    // onFinished distingue los dos casos mirando este estado.
    setState(State::Stopping);

    // terminate() manda SIGTERM: "por favor cierra". No espera. A diferencia del
    // destructor, aquí NO se puede bloquear con waitForFinished (se congelaría
    // la ventana hasta 5 s), así que se arma un temporizador: si se dispara y el
    // proceso sigue vivo, onStopTimeout lo mata.
    m_process->terminate();
    m_stopTimer->start();
}

void ProcessManager::onStopTimeout()
{
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    qWarning() << "ProcessManager: koboldcpp no cerró en" << STOP_TIMEOUT_MS
               << "ms tras SIGTERM; se mata con SIGKILL.";
    // Cuando muera llegará `finished`, y onFinished pasará a NotRunning.
    m_process->kill();
}

void ProcessManager::onReadyRead()
{
    readAvailableOutput();
}

// La lectura de la salida, línea por línea.
//
// El problema: `readyRead` avisa "llegaron bytes", NO "llegó una línea". El
// sistema operativo entrega la salida del proceso en pedazos de tamaño
// arbitrario, cortados donde sea. En una lectura puede llegar:
//
//     "Load Text Model OK: True\nStarting Kobold API on po"
//
// y en la siguiente:
//
//     "rt 5001 at http://localhost:5001/api/\n"
//
// Si se buscara el patrón de "listo" en cada pedazo por separado, no se
// encontraría nunca: está partido entre los dos.
//
// La solución es un buffer propio (m_outputBuffer) que sobrevive entre una
// llamada y la siguiente:
//   1. Lo recién leído se AGREGA al final del buffer.
//   2. Mientras el buffer contenga un '\n', se corta la primera línea completa,
//      se procesa y se quita del buffer.
//   3. Lo que sobra (una línea a medias, sin su '\n') SE QUEDA en el buffer
//      esperando a que la próxima lectura traiga el resto.
//
// Es el mismo patrón que usa SseParser para el streaming de tokens: allá
// también se corta por líneas, y una línea vacía marca el fin de un evento.
void ProcessManager::readAvailableOutput()
{
    // readAllStandardOutput() devuelve todo lo disponible y lo saca de la cola
    // interna de QProcess. Con MergedChannels esto incluye también stderr.
    m_outputBuffer.append(m_process->readAllStandardOutput());

    bool anyNewLine = false;

    // `while (true)` con `break` adentro: se repite hasta que no queden líneas
    // completas en el buffer.
    while (true) {
        // Posición del primer '\n', o -1 si no hay ninguno. `qsizetype` es el
        // tipo entero que Qt usa para tamaños y posiciones.
        qsizetype newlinePosition = m_outputBuffer.indexOf('\n');
        if (newlinePosition == -1) {
            break;
        }

        // left(n) = los primeros n bytes: la línea SIN su '\n'.
        QByteArray lineBytes = m_outputBuffer.left(newlinePosition);
        // Si la línea venía terminada en "\r\n" (fin de línea estilo Windows),
        // sobra un '\r' al final. chop(1) quita el último byte.
        if (lineBytes.endsWith('\r')) {
            lineBytes.chop(1);
        }
        // remove(0, n) borra los primeros n bytes: la línea Y su '\n'.
        m_outputBuffer.remove(0, newlinePosition + 1);

        // Recién aquí se convierten los bytes a texto, y no antes, a propósito.
        // En UTF-8 un carácter como 'ñ' ocupa varios bytes, y un pedazo de
        // lectura puede cortarse justo en medio de uno; convertir un pedazo
        // suelto daría un carácter roto. Una línea completa nunca tiene ese
        // problema, porque el byte de '\n' jamás aparece dentro de otro carácter.
        processLine(QString::fromUtf8(lineBytes));
        anyNewLine = true;
    }

    // Válvula de seguridad: una "línea" que crece sin terminar nunca (ver
    // MAX_PARTIAL_LINE_BYTES arriba). Se procesa tal cual y se vacía el buffer.
    if (m_outputBuffer.size() > MAX_PARTIAL_LINE_BYTES) {
        processLine(QString::fromUtf8(m_outputBuffer));
        m_outputBuffer.clear();
        anyNewLine = true;
    }

    // Se avisa UNA vez por tanda de lectura y no una vez por línea: cada aviso
    // hace que QML vuelva a pedir el texto entero y lo redibuje, y durante la
    // carga llegan cientos de líneas juntas en una misma tanda.
    if (anyNewLine) {
        emit recentOutputChanged();
    }
}

void ProcessManager::processLine(const QString& line)
{
    // Guardar la línea para mostrarla, tirando la más vieja si se pasó el tope.
    m_recentLines.append(line);
    if (m_recentLines.size() > MAX_RECENT_LINES) {
        m_recentLines.removeFirst();
    }

    // De aquí en adelante: ¿la línea dice algo que nos importe? Casi ninguna lo
    // hace, y las que no, simplemente no casan con nada y se descartan. Son tres
    // búsquedas de texto por línea; para ~1000 líneas es trabajo despreciable.
    //
    // Se usa contains() ("aparece en algún lugar de la línea") y no una
    // comparación exacta para no depender de espacios o prefijos alrededor.

    // Pista de fallo 1: no hubo VRAM para algo. Sale de la parte en C/CUDA.
    if (line.contains("cudaMalloc failed: out of memory")) {
        m_sawOutOfMemory = true;
    }

    // Pista de fallo 2: koboldcpp declara que el modelo no cargó. Tras esta
    // línea el proceso termina solo, un par de segundos después.
    if (line.contains("Load Text Model OK: False")) {
        m_sawLoadFailed = true;
    }

    // La señal de "listo": el servidor HTTP ya escucha. (`Load Text Model OK:
    // True` sale antes, pero solo dice que el modelo cargó, no que ya se le
    // pueda hablar.)
    //
    // Solo cuenta si estábamos cargando: en Stopping o tras un fallo, una línea
    // atrasada no debe "resucitar" el estado.
    //
    // match() devuelve un objeto con el resultado; hasMatch() dice si casó.
    if (m_state == State::Loading && m_readyPattern.match(line).hasMatch()) {
        setState(State::Ready);
    }
}

void ProcessManager::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_stopTimer->stop();

    // Antes de decidir nada, leer TODO lo que el proceso alcanzó a imprimir.
    // Puede quedar salida sin leer, y además puede quedar en el buffer una
    // última línea sin '\n' final, que ya no va a completarse nunca.
    readAvailableOutput();
    if (!m_outputBuffer.isEmpty()) {
        processLine(QString::fromUtf8(m_outputBuffer));
        m_outputBuffer.clear();
        emit recentOutputChanged();
    }

    // Caso 1: lo pedimos nosotros. No es un fallo, y el código de salida NO se
    // interpreta: está medido que varía según cómo se cierre (143 con el SIGTERM
    // de terminate(); 0 o 120 con Ctrl-C), y si hubo que usar kill() figura
    // como "crash". Todo eso es normal.
    if (m_state == State::Stopping) {
        setState(State::NotRunning);
        return;
    }

    // Texto que describe cómo terminó, para los mensajes de abajo.
    // ExitStatus distingue "el programa terminó por su cuenta" (NormalExit, con
    // un código de salida) de "lo mató una señal o crasheó" (CrashExit, donde el
    // código de salida no significa nada).
    QString howItEnded;
    if (exitStatus == QProcess::CrashExit) {
        howItEnded = "terminó de forma abrupta: crasheó o lo mató una señal";
    } else {
        howItEnded = QString("código de salida %1").arg(exitCode);
    }

    // Caso 2: ya estaba listo y se murió sin que nadie lo pidiera (un crash, o
    // alguien lo mató desde otra terminal).
    if (m_state == State::Ready) {
        fail(QString("El backend se cerró inesperadamente (%1).").arg(howItEnded));
        return;
    }

    // Caso 3: se murió mientras cargaba. Recién AHORA se interpretan las pistas
    // juntadas en processLine().
    //
    // Por qué aquí y no en el momento de ver `Load Text Model OK: False`:
    //   * El orden de las líneas no es fiable. Leyendo por pipe, las líneas que
    //     imprime la parte en C pueden llegar mucho después que las de Python
    //     (ver "El orden de las líneas cambia al leer por pipe" en
    //     NOTES-backend.md). Al terminar el proceso ya se leyó todo, así que da
    //     igual si la línea de cudaMalloc llegó antes o después de la otra.
    //   * Así "Failed" siempre significa "ya no hay proceso", y se puede volver
    //     a cargar de inmediato. Si se declarara el fallo al ver la línea, el
    //     proceso seguiría vivo ~2 s y un start() en ese rato se ignoraría.
    // El costo: el fallo se muestra esos ~2 s más tarde.
    if (m_sawLoadFailed && m_sawOutOfMemory) {
        // Según lo medido, los pesos del modelo sí cupieron; lo que no cupo fue
        // el KV cache, cuyo tamaño depende del contexto. Por eso el consejo.
        fail("El modelo no cupo en la memoria de la GPU (VRAM). Lo que suele no "
             "caber no son los pesos del modelo sino el contexto: hace falta un "
             "tamaño de contexto más chico o menos capas en la GPU.");
        return;
    }
    if (m_sawLoadFailed) {
        fail("koboldcpp no pudo cargar el modelo. Revisa que la ruta sea correcta "
             "y que el archivo sea un GGUF válido; el detalle está en la salida "
             "del proceso.");
        return;
    }
    // Sin ninguna pista reconocida: se dice lo único que se sabe.
    fail(QString("koboldcpp terminó antes de estar listo (%1). El detalle está en "
                 "la salida del proceso.")
             .arg(howItEnded));
}

void ProcessManager::onErrorOccurred(QProcess::ProcessError error)
{
    // FailedToStart: el programa ni siquiera llegó a ejecutarse (no existe, no
    // está en $PATH, o no tiene permiso de ejecución). En este caso QProcess NO
    // emite `finished`, así que este es el único lugar donde enterarse.
    if (error == QProcess::FailedToStart) {
        m_stopTimer->stop();
        // errorString() es la descripción del sistema ("No such file...").
        // fail() emite signals, y quien las reciba puede llamar a start() ahí
        // mismo. La bandera le avisa a start() que NO lance todavía, porque
        // seguimos dentro de la emisión de `errorOccurred` de QProcess (ver la
        // explicación en start()). Se baja al terminar.
        m_insideErrorSignal = true;
        fail(QString("No se pudo ejecutar «%1». ¿Está instalado koboldcpp y está "
                     "en el PATH? (%2)")
                 .arg(m_binaryPath, m_process->errorString()));
        m_insideErrorSignal = false;
        return;
    }

    // Crashed: el proceso murió por una señal. Después de este aviso QProcess
    // emite también `finished`, y es onFinished quien decide qué significa.
    if (error == QProcess::Crashed) {
        return;
    }

    // El resto (errores de lectura/escritura del pipe) no deberían ocurrir. No
    // se convierten en un fallo visible porque el proceso puede seguir sano,
    // pero tampoco se tragan en silencio: quedan en el log.
    qWarning() << "ProcessManager: error de QProcess:" << error << m_process->errorString();
}

void ProcessManager::setState(State newState)
{
    if (m_state == newState) {
        return;
    }
    m_state = newState;
    emit stateChanged();
}

void ProcessManager::setErrorMessage(const QString& message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

void ProcessManager::fail(const QString& message)
{
    // Primero el mensaje y después el estado: quien reaccione a `stateChanged`
    // y lea `errorMessage` ya encuentra el texto nuevo.
    setErrorMessage(message);
    setState(State::Failed);
}
