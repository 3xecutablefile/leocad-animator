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
};
