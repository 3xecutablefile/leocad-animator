#pragma once

#include "lc_math.h"

class lcAnimateExportDialog;
class lcModel;

// A frame is just a snapshot of every piece's position/rotation, keyed by piece ID. This dock owns
// frames directly instead of using LeoCAD's per-Step keyframe/building-instructions machinery -
// that machinery tracks "the step a piece first appears," not "captured animation frames," and
// kept causing mismatches (frame counts collapsing, deletes silently no-oping, etc).
struct lcAnimateFrame
{
	QMap<QString, lcVector3> Positions;
	QMap<QString, lcMatrix33> Rotations;
};

void lcPoseAnimateFrame(lcModel* Model, const lcAnimateFrame& Frame);

class lcAnimateWidget : public QWidget
{
	Q_OBJECT

public:
	lcAnimateWidget(QWidget* Parent);

	void Update();
	const std::vector<lcAnimateFrame>& GetFrames() const
	{
		return mFrames;
	}

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
	void EnsureInitialized(lcModel* Model);
	lcAnimateFrame SnapshotFrame(lcModel* Model) const;
	void ApplyFrame(lcModel* Model, int FrameIndex);
	QIcon RenderFrameThumbnail(lcModel* Model, int FrameIndex, int Width, int Height);
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

	std::vector<lcAnimateFrame> mFrames;
	int mCurrentFrameIndex = 0;
	lcModel* mLastModel = nullptr;
	bool mInitialized = false;

	QMap<int, QIcon> mThumbnailCache;
	bool mIgnoreUpdates = false;
};
