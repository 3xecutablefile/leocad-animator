#pragma once

#include <QString>
#include <QStringList>

QStringList lcBuildFfmpegAnimationArguments(const QString& InputPattern, int Fps, int StartNumber, const QString& OutputFile, bool IsGif);
