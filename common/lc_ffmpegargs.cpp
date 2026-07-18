#include "lc_global.h"
#include "lc_ffmpegargs.h"

QStringList lcBuildFfmpegAnimationArguments(const QString& InputPattern, int Fps, const QString& OutputFile, bool IsGif)
{
	QStringList Arguments;

	Arguments << QLatin1String("-y")
	          << QLatin1String("-framerate") << QString::number(Fps)
	          << QLatin1String("-i") << InputPattern;

	if (IsGif)
		Arguments << QLatin1String("-loop") << QLatin1String("0");
	else
		Arguments << QLatin1String("-c:v") << QLatin1String("libx264") << QLatin1String("-pix_fmt") << QLatin1String("yuv420p");

	Arguments << OutputFile;

	return Arguments;
}
