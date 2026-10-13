// tst_processmanager.cpp — tests automáticos de ProcessManager.
//
// No usan el koboldcpp real (necesita GPU): usan tests/fake-koboldcpp.sh, un
// script que imprime los logs reales capturados en logs/. Ver ese archivo para
// la lista de "escenarios" que sabe imitar.
//
// Cómo correrlos, desde la raíz del repo y después de compilar:
//   ctest --test-dir build --output-on-failure
// o directamente el ejecutable, que da más detalle:
//   ./build/tst_processmanager
//
// Qt Test es el equivalente de `unittest` de Python: una clase con un método
// por test, y macros (QCOMPARE, QVERIFY) en el papel de los `assert`.

#include "ProcessManager.h"

#include <QSignalSpy>
#include <QTest>

// Headers de POSIX (el sistema operativo, no Qt ni C++). De aquí sale `kill()`,
// la función detrás del comando `kill` de la terminal, y `errno`, la variable
// donde el sistema deja el motivo del último error.
#include <cerrno>
#include <signal.h>

// Ruta al koboldcpp falso. KRAMPUS_SOURCE_DIR no está definido en ningún .h: lo
// define CMake al compilar este archivo (ver target_compile_definitions en
// CMakeLists.txt) con la ruta absoluta del repo. Así el test encuentra el
// script sin importar desde qué carpeta se ejecute.
const QString FAKE_KOBOLDCPP = QString(KRAMPUS_SOURCE_DIR) + "/tests/fake-koboldcpp.sh";

// Un puerto que NO es el 5001 por defecto, a propósito: el falso imprime el
// puerto que recibe, así que si ProcessManager tuviera el 5001 escrito a mano en
// su patrón de "listo", los tests no llegarían nunca a Ready.
const int TEST_PORT = 5999;

// El falso ignora la ruta del modelo; solo hace falta pasar alguna.
const QString TEST_MODEL = "modelo-de-prueba.gguf";

// Tiempo máximo que un test espera a que ocurra algo. Es un tope, no una pausa:
// las esperas terminan en cuanto la condición se cumple (el falso responde en
// milisegundos). Es holgado para que una máquina cargada no haga fallar un test
// que en realidad está bien.
const int WAIT_MS = 10000;

// Tope para los tests del plan B (SIGKILL): ProcessManager espera 5 s antes de
// matar, así que estos necesitan más margen que eso.
const int WAIT_FOR_KILL_MS = 15000;

// ¿Existe todavía un proceso con este PID?
//
// `kill(pid, 0)` con señal 0 no manda ninguna señal: solo le pregunta al sistema
// si podría mandarla. Si devuelve -1 y errno vale ESRCH ("no such process"), el
// proceso ya no existe. Los `::` sueltos por delante piden "la función kill
// global, la del sistema", para que no se confunda con ningún método de Qt.
bool processExists(qint64 pid)
{
    int result = ::kill(static_cast<pid_t>(pid), 0);
    if (result == -1 && errno == ESRCH) {
        return false;
    }
    return true;
}

// Prepara un ProcessManager para usar el falso con un escenario, y lo arranca.
//
// `ProcessManager&` (referencia SIN const): se recibe el mismo objeto del que
// llama, no una copia, y aquí sí se lo va a modificar.
void startFake(ProcessManager& manager, const char* scenario)
{
    // qputenv fija una variable de entorno de ESTE proceso (el test). El proceso
    // hijo hereda el entorno de quien lo lanza, y así le llega al script.
    qputenv("FAKE_KOBOLDCPP_ESCENARIO", scenario);
    manager.setBinaryPath(FAKE_KOBOLDCPP);
    manager.start(TEST_MODEL, TEST_PORT);
}

// La clase de tests. Hereda de QObject y lleva Q_OBJECT porque Qt Test descubre
// los tests buscando sus "private slots": cada uno es un test, y se ejecutan en
// el orden en que están escritos. (Aquí sí se usa la palabra `slots`, que en
// ProcessManager.h no hizo falta: es la marca que Qt Test busca.)
class TestProcessManager : public QObject
{
    Q_OBJECT

private slots:
    void reachesReady();
    void readyLineSplitAcrossReads();
    void vramFailureHasSpecificMessage();
    void loadFailureWithoutOutOfMemory();
    void exitBeforeReadyReportsExitCode();
    void missingBinaryFails();
    void restartFromInsideFailedSignal();
    void stopCancelsRestartFromInsideFailedSignal();
    void deathAfterReadyIsFailure();
    void stopIsNotFailure();
    void startWhileRunningIsIgnored();
    void destructorKillsProcess();
    void stopKillsProcessThatIgnoresSigterm();
    void destructorKillsProcessThatIgnoresSigterm();
};

// Sobre los macros que aparecen en todos los tests:
//   QCOMPARE(a, b) : falla el test si a != b, y muestra los dos valores.
//   QVERIFY(x)     : falla el test si x es falso.
//   QVERIFY2(x, m) : lo mismo, mostrando además el mensaje m.
//   QTRY_COMPARE_WITH_TIMEOUT(a, b, ms) : como QCOMPARE, pero si no se cumple
//       todavía, deja correr el bucle de eventos y reintenta hasta `ms`.
//       ProcessManager es asíncrono (start() regresa enseguida y el resultado
//       llega después por signals), y esas signals solo se reparten mientras
//       corre el bucle de eventos. Un QCOMPARE justo después de start() vería
//       siempre "Loading".
//
// En cada test `manager` es una variable local: se destruye sola al llegar a la
// `}` del test (o antes, si un QCOMPARE falla y sale de la función), y su
// destructor mata al proceso falso. Por eso ningún test limpia a mano.

// Carga normal con el log real de ~1000 líneas: debe terminar en Ready.
void TestProcessManager::reachesReady()
{
    ProcessManager manager;
    QCOMPARE(manager.state(), ProcessManager::State::NotRunning);

    // QSignalSpy se engancha a una signal y anota cada vez que se emite; es una
    // lista con una entrada por emisión. Sirve para comprobar que la signal de
    // verdad se emitió, que es de lo que depende QML para enterarse.
    QSignalSpy stateSpy(&manager, &ProcessManager::stateChanged);

    startFake(manager, "ok");
    // Inmediatamente después de start(), sin esperar nada.
    QCOMPARE(manager.state(), ProcessManager::State::Loading);

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);

    // Dos cambios: NotRunning -> Loading -> Ready.
    QCOMPARE(stateSpy.count(), 2);
    QVERIFY(manager.errorMessage().isEmpty());
    QVERIFY(processExists(manager.processId()));

    // La salida expuesta está acotada (el log tiene ~1000 líneas) y contiene el
    // final, que es donde está la línea de listo.
    QString output = manager.recentOutput();
    QVERIFY(output.count('\n') < 200);
    QVERIFY(output.contains(QString("Starting Kobold API on port %1").arg(TEST_PORT)));
}

// La línea de listo llega partida en dos lecturas: el buffer de líneas tiene
// que juntarla.
void TestProcessManager::readyLineSplitAcrossReads()
{
    ProcessManager manager;
    startFake(manager, "linea-partida");

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);

    // La línea quedó guardada entera, como UNA línea, no como dos mitades.
    QCOMPARE(manager.recentOutput(),
             QString("Starting Kobold API on port %1 at http://localhost:%1/api/").arg(TEST_PORT));
}

// Log real de la carga que no cupo en la VRAM.
void TestProcessManager::vramFailureHasSpecificMessage()
{
    ProcessManager manager;
    startFake(manager, "falla-vram");

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Failed, WAIT_MS);

    // El mensaje nombra la causa (VRAM) y lo que según las notas no cupo.
    // qPrintable convierte un QString al texto plano que QVERIFY2 espera.
    QVERIFY2(manager.errorMessage().contains("VRAM"), qPrintable(manager.errorMessage()));
    QVERIFY2(manager.errorMessage().contains("contexto"), qPrintable(manager.errorMessage()));
    // Tras un fallo ya no hay proceso.
    QCOMPARE(manager.processId(), qint64(0));
}

// El modelo no carga pero no por memoria: el mensaje NO debe culpar a la VRAM.
void TestProcessManager::loadFailureWithoutOutOfMemory()
{
    ProcessManager manager;
    startFake(manager, "falla-carga");

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Failed, WAIT_MS);

    QVERIFY2(manager.errorMessage().contains("no pudo cargar el modelo"),
             qPrintable(manager.errorMessage()));
    QVERIFY2(!manager.errorMessage().contains("VRAM"), qPrintable(manager.errorMessage()));
}

// El proceso termina antes de estar listo sin ninguna pista: el mensaje lleva
// el código de salida (el falso sale con 7).
void TestProcessManager::exitBeforeReadyReportsExitCode()
{
    ProcessManager manager;
    startFake(manager, "sale-temprano");

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Failed, WAIT_MS);

    QVERIFY2(manager.errorMessage().contains("antes de estar listo"),
             qPrintable(manager.errorMessage()));
    QVERIFY2(manager.errorMessage().contains("código de salida 7"),
             qPrintable(manager.errorMessage()));
}

// El binario no existe. Y después de ese fallo se puede volver a intentar.
void TestProcessManager::missingBinaryFails()
{
    ProcessManager manager;
    manager.setBinaryPath("/no/existe/koboldcpp");
    manager.start(TEST_MODEL, TEST_PORT);

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Failed, WAIT_MS);
    QVERIFY2(manager.errorMessage().contains("/no/existe/koboldcpp"),
             qPrintable(manager.errorMessage()));

    // Corregida la ruta, el mismo objeto arranca bien y el error viejo se borra.
    startFake(manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
    QVERIFY(manager.errorMessage().isEmpty());
}

// Reintentar DESDE DENTRO del aviso de fallo: el binario no existe y, en el
// mismo slot que recibe `stateChanged` con el estado Failed, se corrige la ruta
// y se llama a start() otra vez. Es lo que hará la Fase 2 al probar el
// siguiente nombre de binario. Debe llegar a Ready.
//
// Esto es "reentrancia": volver a entrar a un objeto mientras ese mismo objeto
// está a mitad de otra cosa. Las signals de Qt son llamadas a función normales:
// `emit` no regresa hasta que terminaron todos los slots. Así que cuando este
// slot corre, más abajo en la cadena de llamadas QProcess todavía no terminó de
// avisar que falló. En Python pasa lo mismo si un callback modifica la lista
// que alguien más arriba está recorriendo con un `for`.
//
// Antes del arreglo (ver start() y onErrorOccurred en ProcessManager.cpp) este
// test no fallaba: se COLGABA, con el programa bloqueado dentro de QProcess.
void TestProcessManager::restartFromInsideFailedSignal()
{
    ProcessManager manager;
    manager.setBinaryPath("/no/existe/koboldcpp");

    // Para reintentar una sola vez. Si el segundo arranque también fallara, no
    // queremos un ciclo infinito de reintentos.
    bool alreadyRetried = false;

    // Aquí el slot no es un método sino una "lambda": una función sin nombre
    // escrita en el lugar, como `lambda` de Python pero con cuerpo de varias
    // líneas. Los corchetes `[&manager, &alreadyRetried]` son la "captura":
    // la lista de variables de afuera que la lambda puede usar. El `&` las
    // captura por referencia (usa las mismas variables, no copias), que aquí
    // es seguro porque las dos viven hasta el final del test, igual que la
    // conexión.
    //
    // El tercer argumento (`&manager`) es el objeto "contexto": si se destruye,
    // Qt corta la conexión sola.
    connect(&manager, &ProcessManager::stateChanged, &manager, [&manager, &alreadyRetried]() {
        if (manager.state() == ProcessManager::State::Failed && !alreadyRetried) {
            alreadyRetried = true;
            // Sin esperar nada: todavía estamos dentro de la emisión.
            startFake(manager, "ok");
        }
    });

    manager.start(TEST_MODEL, TEST_PORT);

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
    QVERIFY(alreadyRetried);
    QVERIFY(manager.errorMessage().isEmpty());
    QVERIFY(processExists(manager.processId()));
}

// Mismo escenario, pero el slot pide arrancar y enseguida detener. El arranque
// pedido desde dentro del aviso no se lanza en el momento (queda anotado para
// la siguiente vuelta del bucle de eventos), así que el stop() tiene que
// cancelarlo: no debe aparecer ningún proceso después.
void TestProcessManager::stopCancelsRestartFromInsideFailedSignal()
{
    ProcessManager manager;
    manager.setBinaryPath("/no/existe/koboldcpp");

    bool alreadyRetried = false;
    connect(&manager, &ProcessManager::stateChanged, &manager, [&manager, &alreadyRetried]() {
        if (manager.state() == ProcessManager::State::Failed && !alreadyRetried) {
            alreadyRetried = true;
            startFake(manager, "ok");
            manager.stop();
        }
    });

    manager.start(TEST_MODEL, TEST_PORT);

    QTRY_VERIFY_WITH_TIMEOUT(alreadyRetried, WAIT_MS);
    // Dar tiempo de sobra a que el arranque anotado se hubiera ejecutado, si
    // no se hubiera cancelado.
    QTest::qWait(300);
    QCOMPARE(manager.state(), ProcessManager::State::NotRunning);
    QCOMPARE(manager.processId(), qint64(0));
}

// El proceso muere DESPUÉS de estar listo sin que lo pidamos (aquí lo matamos
// "desde fuera", como haría `kill -9` desde otra terminal): eso es un fallo.
void TestProcessManager::deathAfterReadyIsFailure()
{
    ProcessManager manager;
    startFake(manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);

    qint64 pid = manager.processId();
    // static_cast<pid_t>(...) convierte el entero de 64 bits de Qt al tipo que
    // la función del sistema espera (pid_t, de 32 bits). C++ haría esa
    // conversión solo y EN SILENCIO, sin ningún aviso con nuestros flags; el
    // cast deja escrito que achicar el número es a propósito y no un descuido.
    ::kill(static_cast<pid_t>(pid), SIGKILL);

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Failed, WAIT_MS);
    QVERIFY2(manager.errorMessage().contains("se cerró"), qPrintable(manager.errorMessage()));
}

// Detener a pedido: pasa por Stopping, termina en NotRunning, sin error.
void TestProcessManager::stopIsNotFailure()
{
    ProcessManager manager;
    startFake(manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
    qint64 pid = manager.processId();

    manager.stop();
    QCOMPARE(manager.state(), ProcessManager::State::Stopping);

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::NotRunning, WAIT_MS);
    QVERIFY(manager.errorMessage().isEmpty());
    QVERIFY(!processExists(pid));

    // Y se puede volver a cargar después de detener.
    startFake(manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
}

// Un segundo start() con un proceso vivo no lanza otro.
void TestProcessManager::startWhileRunningIsIgnored()
{
    ProcessManager manager;
    startFake(manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
    qint64 pid = manager.processId();

    // ProcessManager avisa con un qWarning cuando ignora el start(). Qt Test
    // deja declarar qué warning se espera; si no llegara, el test falla.
    QTest::ignoreMessage(QtWarningMsg,
                         "ProcessManager: start() ignorado, ya hay un proceso de koboldcpp vivo.");
    manager.start(TEST_MODEL, TEST_PORT);

    // Mismo proceso, mismo estado.
    QCOMPARE(manager.processId(), pid);
    QCOMPARE(manager.state(), ProcessManager::State::Ready);
}

// Destruir el ProcessManager con el proceso vivo (lo que pasa al cerrar
// Krampus) no deja a koboldcpp huérfano.
void TestProcessManager::destructorKillsProcess()
{
    // Aquí el objeto se crea con `new` y no como variable local, para poder
    // destruirlo en un punto exacto del test con `delete` y seguir comprobando
    // cosas después. Es el único lugar donde hace falta: con `new` sin padre,
    // el `delete` es responsabilidad nuestra.
    ProcessManager* manager = new ProcessManager();
    startFake(*manager, "ok");
    QTRY_COMPARE_WITH_TIMEOUT(manager->state(), ProcessManager::State::Ready, WAIT_MS);

    qint64 pid = manager->processId();
    QVERIFY(processExists(pid));

    // `delete` ejecuta el destructor y libera la memoria. El destructor espera
    // (bloqueando) a que el proceso muera, así que al regresar ya no existe.
    delete manager;

    QVERIFY(!processExists(pid));
}

// stop() sobre un proceso que ignora SIGTERM: a los 5 s se lo mata con SIGKILL,
// y sigue sin contar como fallo. Este test tarda esos 5 s a propósito.
void TestProcessManager::stopKillsProcessThatIgnoresSigterm()
{
    ProcessManager manager;
    startFake(manager, "ignora-sigterm");
    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::Ready, WAIT_MS);
    qint64 pid = manager.processId();

    QTest::ignoreMessage(QtWarningMsg,
                         "ProcessManager: koboldcpp no cerró en 5000 ms tras SIGTERM; se mata con SIGKILL.");
    manager.stop();

    // Un segundo después sigue vivo y en Stopping: de verdad ignoró la señal.
    // QTest::qWait espera dejando correr el bucle de eventos.
    QTest::qWait(1000);
    QCOMPARE(manager.state(), ProcessManager::State::Stopping);
    QVERIFY(processExists(pid));

    QTRY_COMPARE_WITH_TIMEOUT(manager.state(), ProcessManager::State::NotRunning, WAIT_FOR_KILL_MS);
    QVERIFY(manager.errorMessage().isEmpty());
    QVERIFY(!processExists(pid));
}

// Lo mismo desde el destructor: si el proceso ignora SIGTERM, el destructor no
// se va dejándolo vivo. También tarda 5 s.
void TestProcessManager::destructorKillsProcessThatIgnoresSigterm()
{
    ProcessManager* manager = new ProcessManager();
    startFake(*manager, "ignora-sigterm");
    QTRY_COMPARE_WITH_TIMEOUT(manager->state(), ProcessManager::State::Ready, WAIT_MS);
    qint64 pid = manager->processId();

    QTest::ignoreMessage(QtWarningMsg,
                         "ProcessManager: koboldcpp no cerró en 5000 ms tras SIGTERM; se mata con SIGKILL.");
    delete manager;

    QVERIFY(!processExists(pid));
}

// QTEST_GUILESS_MAIN escribe por nosotros el `main()` de este ejecutable: crea
// la aplicación Qt, corre todos los tests de la clase y devuelve cuántos
// fallaron. "GUILESS" = sin interfaz gráfica (usa QCoreApplication): estos tests
// no abren ventanas, así que corren igual sin escritorio.
QTEST_GUILESS_MAIN(TestProcessManager)

// La clase de arriba lleva Q_OBJECT pero está en un .cpp, no en un .h. `moc`
// genera su código en un archivo aparte, y como no hay header que lo arrastre,
// hay que incluirlo a mano aquí, al final. Si falta, el enlace falla con
// "undefined reference to vtable for TestProcessManager".
#include "tst_processmanager.moc"
