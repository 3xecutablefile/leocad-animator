#pragma once

#include "lc_ffmpegargs.h"

class lcModel;

class lcAnimateExportDialog : public QDialog
{
	Q_OBJECT

public:
	lcAnimateExportDialog(QWidget* Parent, lcModel* Model, int DefaultFps);

protected slots:
	void Browse();
	void Accept();

protected:
	lcModel* mModel;
	QComboBox* mFormatCombo;
	QSpinBox* mFpsSpinBox;
	QSpinBox* mStartSpinBox;
	QSpinBox* mEndSpinBox;
	QLineEdit* mFileEdit;
};
