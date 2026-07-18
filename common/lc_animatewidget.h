#pragma once

class lcAnimateExportDialog;

class lcAnimateWidget : public QWidget
{
	Q_OBJECT

public:
	lcAnimateWidget(QWidget* Parent);

	void Update();
	void SetSelection(const std::vector<lcObject*>& Selection);

public slots:
	void SliderChanged(int Value);
	void PlayPauseClicked();
	void Timeout();
	void NewFrameClicked();
	void AddKeyframeClicked();
	void RecordToggled(bool Checked);
	void ExportClicked();

protected:
	QSlider* mFrameSlider;
	QLabel* mFrameLabel;
	QPushButton* mPlayButton;
	QSpinBox* mFpsSpinBox;
	QToolButton* mRecordButton;
	QPushButton* mExportButton;
	QTimer* mPlayTimer;

	std::vector<lcObject*> mSelection;
	bool mIgnoreUpdates = false;
};
