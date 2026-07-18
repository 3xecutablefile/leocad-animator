// ponytail: standalone self-check for lcBuildFfmpegAnimationArguments, no QApplication/build-system wiring needed.
// Run with (from repo root, after `brew install qt`, macOS example):
//   QT=$(brew --prefix qt)/lib
//   g++ -std=gnu++17 -Icommon -I$QT/QtCore.framework/Headers -I$QT/QtGui.framework/Headers \
//       -I$QT/QtWidgets.framework/Headers -I$QT/QtOpenGL.framework/Headers -I$QT/QtOpenGLWidgets.framework/Headers \
//       -I$QT/QtPrintSupport.framework/Headers -I$QT/QtConcurrent.framework/Headers -F$QT -framework QtCore \
//       common/lc_ffmpegargs.cpp common/test_animate_export.cpp -o /tmp/test_animate_export && /tmp/test_animate_export
// (The extra -I paths are only needed because lc_ffmpegargs.cpp includes the project-wide lc_global.h;
// lc_ffmpegargs itself only uses QString/QStringList.)
#include "lc_ffmpegargs.h"
#include <QString>
#include <QStringList>
#include <cassert>
#include <cstdio>

int main()
{
	const QStringList GifArgs = lcBuildFfmpegAnimationArguments(QStringLiteral("/tmp/frame_%02d.png"), 12, QStringLiteral("/tmp/out.gif"), true);
	assert(GifArgs.contains(QStringLiteral("-framerate")));
	assert(GifArgs[GifArgs.indexOf(QStringLiteral("-framerate")) + 1] == QStringLiteral("12"));
	assert(GifArgs.contains(QStringLiteral("-loop")));
	assert(GifArgs.last() == QStringLiteral("/tmp/out.gif"));
	assert(!GifArgs.contains(QStringLiteral("-c:v")));

	const QStringList Mp4Args = lcBuildFfmpegAnimationArguments(QStringLiteral("/tmp/frame_%02d.png"), 24, QStringLiteral("/tmp/out.mp4"), false);
	assert(Mp4Args.contains(QStringLiteral("-c:v")));
	assert(Mp4Args[Mp4Args.indexOf(QStringLiteral("-c:v")) + 1] == QStringLiteral("libx264"));
	assert(!Mp4Args.contains(QStringLiteral("-loop")));

	std::printf("test_animate_export: all checks passed\n");
	return 0;
}
