#pragma once

#include "lc_math.h"

class lcAnimateExportDialog;
class lcModel;
class lcPiece;

// A frame is just a snapshot of every piece's position/rotation, keyed by the piece object itself
// (NOT lcPiece::GetID() - that's the LDraw part filename, shared by every instance of the same
// part, e.g. a left hand and right hand are literally the same part and collide on that key).
// This dock owns frames directly instead of using LeoCAD's per-Step keyframe/building-instructions
// machinery - that machinery tracks "the step a piece first appears," not "captured animation
// frames," and kept causing mismatches (frame counts collapsing, deletes silently no-oping, etc).
struct lcAnimateFrame
{
	QMap<lcPiece*, lcVector3> Positions;
	QMap<lcPiece*, lcMatrix33> Rotations;
};

void lcPoseAnimateFrame(lcModel* Model, const lcAnimateFrame& Frame);

// lcGetActiveModel() is not a stable "the current document" pointer - it switches to point at a
// submodel while one is being edited in place (LC_PIECE_EDIT_SELECTED_SUBMODEL), then back. Frame
// history is kept per-model so that happening never destroys another model's animation.
struct lcAnimateDocumentState
{
	std::vector<lcAnimateFrame> Frames;
	int CurrentFrameIndex = 0;
	QMap<int, QIcon> ThumbnailCache;
};

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
	void SocketModeToggled(bool Checked);
	void AttachToHandClicked();

protected:
	lcAnimateDocumentState& GetState(lcModel* Model);
	lcAnimateFrame SnapshotFrame(lcModel* Model) const;
	void ApplyFrame(lcModel* Model, int FrameIndex);
	QIcon RenderFrameThumbnail(lcModel* Model, int FrameIndex, int Width, int Height);
	void RefreshFilmstrip(lcModel* Model);
	void RefreshOnionSkin(lcModel* Model);

	QListWidget* mFilmstrip;
	QCheckBox* mSocketModeCheck;
	QCheckBox* mOnionSkinCheck;
	QLabel* mOnionSkinPreview;
	QPushButton* mCaptureButton;
	QPushButton* mPlayButton;
	QSpinBox* mFpsSpinBox;
	QLabel* mFrameLabel;
	QPushButton* mDeleteButton;
	QPushButton* mExportButton;
	QTimer* mPlayTimer;

	QMap<lcModel*, lcAnimateDocumentState> mDocumentStates;

	bool mIgnoreUpdates = false;
};
