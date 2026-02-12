// rcx-mcp-stdio: Bridges stdin/stdout to QLocalSocket for MCP transport.
// Claude Desktop spawns this process; it connects to the rcx-mcp named pipe
// inside the running Reclass application.
//
// stdin  (from Claude) → QLocalSocket → McpBridge (in Reclass)
// stdout (to Claude)   ← QLocalSocket ← McpBridge (in Reclass)

#include <QCoreApplication>
#include <QLocalSocket>
#include <QTimer>
#include <QTextStream>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

#ifdef _WIN32
    // Ensure stdin/stdout are in binary mode on Windows
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    auto* socket = new QLocalSocket(&app);
    QByteArray readBuf;

    // Socket → stdout: forward lines from Reclass to Claude Desktop
    QObject::connect(socket, &QLocalSocket::readyRead, [&]() {
        readBuf.append(socket->readAll());
        while (true) {
            int idx = readBuf.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = readBuf.left(idx + 1); // include newline
            readBuf.remove(0, idx + 1);
            fwrite(line.constData(), 1, line.size(), stdout);
            fflush(stdout);
        }
    });

    QObject::connect(socket, &QLocalSocket::disconnected, [&]() {
        fprintf(stderr, "[rcx-mcp-stdio] Disconnected from server\n");
        app.quit();
    });

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QObject::connect(socket, &QLocalSocket::errorOccurred, [&](QLocalSocket::LocalSocketError err) {
#else
    QObject::connect(socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error), [&](QLocalSocket::LocalSocketError err) {
#endif
        fprintf(stderr, "[rcx-mcp-stdio] Socket error %d: %s\n",
                (int)err, socket->errorString().toUtf8().constData());
        app.quit();
    });

    // Connect to the named pipe
    socket->connectToServer("rcx-mcp");
    if (!socket->waitForConnected(5000)) {
        fprintf(stderr, "[rcx-mcp-stdio] Failed to connect to rcx-mcp pipe: %s\n",
                socket->errorString().toUtf8().constData());
        return 1;
    }
    fprintf(stderr, "[rcx-mcp-stdio] Connected to rcx-mcp\n");

    // Stdin → socket: poll stdin with a timer (stdin isn't a socket on Windows)
    QByteArray stdinBuf;
    auto* stdinTimer = new QTimer(&app);
    stdinTimer->setInterval(10);

    QObject::connect(stdinTimer, &QTimer::timeout, [&]() {
#ifdef _WIN32
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD avail = 0;
        if (!PeekNamedPipe(hStdin, nullptr, 0, nullptr, &avail, nullptr)) {
            // stdin closed (pipe broken)
            app.quit();
            return;
        }
        if (avail == 0) return;

        char buf[4096];
        DWORD bytesRead = 0;
        DWORD toRead = qMin(avail, (DWORD)sizeof(buf));
        if (!ReadFile(hStdin, buf, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            app.quit();
            return;
        }
        stdinBuf.append(buf, (int)bytesRead);
#else
        // On Unix, we could use QSocketNotifier, but timer works fine too
        char buf[4096];
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) return;
        ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            app.quit();
            return;
        }
        stdinBuf.append(buf, (int)n);
#endif
        // Forward complete lines to socket
        while (true) {
            int idx = stdinBuf.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = stdinBuf.left(idx + 1);
            stdinBuf.remove(0, idx + 1);
            socket->write(line);
            socket->flush();
        }
    });

    stdinTimer->start();
    return app.exec();
}
