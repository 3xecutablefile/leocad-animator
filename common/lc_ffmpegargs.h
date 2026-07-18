#pragma once

#include <QString>
#include <QStringList>

QStringList lcBuildFfmpegAnimationArguments(const QString& InputPattern, int Fps, const QString& OutputFile, bool IsGif);
