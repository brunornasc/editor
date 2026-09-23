#include "core/ThemeManager.hpp"
#include "ui/MainWindow.hpp"

#include <QApplication>
#include <QStyleFactory>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setApplicationName("vs2005ide");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("editor");

    app.setStyle(QStyleFactory::create("Fusion"));

    core::ThemeManager::apply(app);

    ui::MainWindow window;
    window.show();

    // Suporte a `editor meuarquivo.cpp` (abre direto no editor) e
    // `editor meuprojeto.json|.vproj` (carrega projeto).
    QStringList fileArgs;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        if (a.endsWith(QLatin1String(".json")) ||
            a.endsWith(QLatin1String(".vproj")))
            window.openProjectFile(a);
        else
            fileArgs << a;
    }
    window.openFilesFromCommandLine(fileArgs);

    return app.exec();
}