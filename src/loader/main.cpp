#include <QLibrary>
#include <QApplication>
#include "pluginmanager.h"

int main(int argc, char *argv[])
{
	QApplication::setAttribute(Qt::AA_DontShowIconsInMenus,false);
	//xdg-desktop-portal refuses/aborts requests from applications without a
	//desktop id (needed for the screen sharing permission dialog).
	QApplication::setDesktopFileName(QStringLiteral("vacuum.desktop"));

	QApplication app(argc, argv);
	app.setQuitOnLastWindowClosed(false);
	app.addLibraryPath(app.applicationDirPath());

	QLibrary utils(app.applicationDirPath()+"/utils",&app);
	utils.load();

	PluginManager pm(&app);
	pm.restart();

	return app.exec();
}

