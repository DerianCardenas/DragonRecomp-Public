// DragonRecomp Studio — Qt 6 / QML UI Prototype (stub, not yet wired to C++ backend)
#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("DragonRecomp Studio");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule("DragonRecomp.Studio", "Main");
    return app.exec();
}
