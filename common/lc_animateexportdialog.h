#pragma once

#include "lc_ffmpegargs.h"
#include "lc_animatewidget.h"

class lcModel;

class lcAnimateExportDialog : public QDialog
{
	Q_OBJECT

public:
	lcAnimateExportDialog(QWidget* Parent, lcModel* Model, int DefaultFps, const std::vector<lcAnimateFrame>& Frames);

protected slots:
	void Browse();
	void Accept();

protected:
	lcModel* mModel;
	const std::vector<lcAnimateFrame>& mFrames;
	QComboBox* mFormatCombo;
	QSpinBox* mFpsSpinBox;
	QSpinBox* mStartSpinBox;
	QSpinBox* mEndSpinBox;
	QLineEdit* mFileEdit;
};
