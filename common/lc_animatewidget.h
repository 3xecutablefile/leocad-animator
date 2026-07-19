#pragma once

#include "camera.h"
#include "lc_math.h"

class lcAnimateExportDialog;
class lcModel;
class lcPiece;
class Project;
class lcKeyframeTimelineWidget;

enum class lcAnimateMode { StopMotion, ConstantKeyframe };

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

	// Wherever the active viewport's camera was looking when this frame was captured, so Play and
	// Export reproduce camera moves/angle changes exactly as they were shot, not just piece poses.
	lcVector3 CameraPosition = lcVector3(0.0f, 0.0f, 0.0f);
	lcVector3 CameraTarget = lcVector3(0.0f, 0.0f, 0.0f);
	lcVector3 CameraUpVector = lcVector3(0.0f, 1.0f, 0.0f);
	lcCameraProjection CameraProjection = lcCameraProjection::Perspective;
	bool HasCamera = false;
};

// AnimateForcedHidden tracks which pieces THIS system most recently hid because they're absent
// from a frame - as opposed to hidden by the user's own native Hide Selected feature, which shares
// the same underlying lcPiece::mHidden flag. Posing a frame only ever un-hides a piece that's in
// this set (i.e. one this animation system hid itself); a piece the user hid manually stays hidden
// across frame navigation instead of silently reappearing.
void lcPoseAnimateFrame(lcModel* Model, const lcAnimateFrame& Frame, QSet<lcPiece*>& AnimateForcedHidden);

enum class lcEasingType { Linear, EaseIn, EaseOut, EaseInOut };

inline float ApplyEasing(lcEasingType Type, float t)
{
	switch (Type)
	{
	case lcEasingType::Linear:
		return t;
	case lcEasingType::EaseIn:
		return t * t * t;
	case lcEasingType::EaseOut:
	{
		const float u = 1.0f - t;
		return 1.0f - u * u * u;
	}
	case lcEasingType::EaseInOut:
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
	}
	return t;
}

// A keyframe pose is the same data as lcAnimateFrame (snapshot of all piece transforms + camera)
// but without the per-piece-existence semantics — every piece present in the keyframe animation is
// assumed present throughout. Used as the value type in lcKeyframePoint's sparse timeline.
struct lcKeyframePose
{
	QMap<lcPiece*, lcVector3> Positions;
	QMap<lcPiece*, lcMatrix33> Rotations;

	lcVector3 CameraPosition = lcVector3(0.0f, 0.0f, 0.0f);
	lcVector3 CameraTarget = lcVector3(0.0f, 0.0f, 0.0f);
	lcVector3 CameraUpVector = lcVector3(0.0f, 1.0f, 0.0f);
	lcCameraProjection CameraProjection = lcCameraProjection::Perspective;
	bool HasCamera = false;
};

// A keyframe point on the Constant Keyframe timeline: a pose at a specific frame index.
// SegmentEasing controls how the interpolation eases FROM this point TO the next.
struct lcKeyframePoint
{
	int Time;
	lcKeyframePose Pose;
	lcEasingType SegmentEasing = lcEasingType::EaseInOut;
};

// lcGetActiveModel() is not a stable "the current document" pointer - it switches to point at a
// submodel while one is being edited in place (LC_PIECE_EDIT_SELECTED_SUBMODEL), then back. Frame
// history is kept per-model so that happening never destroys another model's animation.
struct lcAnimateDocumentState
{
	std::vector<lcAnimateFrame> Frames;
	int CurrentFrameIndex = 0;
	QMap<int, QIcon> ThumbnailCache;
	QSet<lcPiece*> AnimateForcedHidden;
	lcAnimateMode AnimateMode = lcAnimateMode::StopMotion;
	std::vector<lcKeyframePoint> Keyframes;
};

class lcAnimateWidget : public QWidget
{
	Q_OBJECT

public:
	lcAnimateWidget(QWidget* Parent);

	void Update();

	// Called from lcModel's destructor so a closed document's (or exited submodel's) animation
	// state doesn't linger forever, and so a later lcModel allocated at the same freed address
	// can never inherit a previous, unrelated document's frame data.
	void ForgetModel(lcModel* Model);

	// Animation data isn't part of LeoCAD's own LDraw/binary file format, so it's saved/loaded as a
	// companion "<file>.animate.json" next to the project file - called from Project::Save/Load.
	void SaveAnimationData(Project* CurrentProject, const QString& FileName);
	void LoadAnimationData(Project* CurrentProject, const QString& FileName);

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
	void MirrorPoseClicked();
	void WalkCycleClicked();
	void ModeChanged(int Index);
	void AddKeyframeClicked();
	void DeleteKeyframeClicked();
	void ClearKeyframeClicked();
	void EasingChanged(int Index);
	void TimelineKeyframeSelected(int Index);
	void TimelineTimeDragged(int Frame);
	void TimelineStep(int Delta);

protected:
	lcAnimateDocumentState& GetState(lcModel* Model);
	lcAnimateFrame SnapshotFrame(lcModel* Model) const;
	void ApplyFrame(lcModel* Model, int FrameIndex);
	QIcon RenderFrameThumbnail(lcModel* Model, int FrameIndex, int Width, int Height);
	void RefreshFilmstrip(lcModel* Model);
	void RefreshOnionSkin(lcModel* Model);
	void BakeKeyframes(lcModel* Model, lcAnimateDocumentState& State);

	QComboBox* mModeSelector;
	QListWidget* mFilmstrip;
	QWidget* mKeyframeControls;
	lcKeyframeTimelineWidget* mTimelineWidget;
	QPushButton* mStepBackButton;
	QPushButton* mStepForwardButton;
	QPushButton* mAddKeyframeButton;
	QPushButton* mDeleteKeyframeButton;
	QPushButton* mClearKeyframeButton;
	QComboBox* mEasingCombo;
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

	// Update() only rebuilds the filmstrip when the frame count changes, as a cheap way to avoid
	// re-rendering thumbnails on every step-change notification; tracking which model that decision
	// was last made for stops it from also skipping a rebuild when switching to a *different* model
	// that happens to have the same frame count (e.g. two freshly-touched models both at 1 frame).
	lcModel* mLastFilmstripModel = nullptr;

	bool mIgnoreUpdates = false;
	bool mUpdating = false;

	QCheckBox* mAutoKeyframeCheck;
	int mFpsLastValue = 12;
	bool mIsApplyingFrame = false;
	bool mSkipAutoKeyframe = false;
	bool mAutoKeyframeInitialized = false;
	lcAnimateFrame mLastAutoKeyframeDigest;
};
