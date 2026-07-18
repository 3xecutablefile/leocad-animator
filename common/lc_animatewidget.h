#pragma once

class lcAnimateExportDialog;
class lcModel;

class lcAnimateWidget : public QWidget
{
	Q_OBJECT

public:
	lcAnimateWidget(QWidget* Parent);

	void Update();

public slots:
	void FilmstripItemChanged(int Row);
	void CaptureClicked();
	void DuplicateClicked();
	void DeleteClicked();
	void PlayPauseClicked();
	void Timeout();
	void OnionSkinToggled(bool Checked);
	void ExportClicked();

protected:
	QIcon RenderStepThumbnail(lcModel* Model, quint32 Step, int Width, int Height);
	void RefreshFilmstrip(lcModel* Model);
	void RefreshOnionSkin(lcModel* Model);

	QListWidget* mFilmstrip;
	QCheckBox* mOnionSkinCheck;
	QLabel* mOnionSkinPreview;
	QPushButton* mCaptureButton;
	QPushButton* mPlayButton;
	QSpinBox* mFpsSpinBox;
	QLabel* mFrameLabel;
	QPushButton* mDeleteButton;
	QPushButton* mExportButton;
	QTimer* mPlayTimer;

	QMap<int, QIcon> mThumbnailCache;
	bool mIgnoreUpdates = false;

	// lcModel::GetLastStep() tracks "step a piece first appears" (for building instructions), not
	// "highest frame with a captured pose" - it never grows in a stop-motion workflow where every
	// piece is placed once and just moved. Track the real frame count ourselves instead.
	quint32 mFrameCount = 1;
};
