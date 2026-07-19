#include "lc_global.h"
#include "lc_animatewidget.h"
#include "lc_keyframetimelinewidget.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_mainwindow.h"
#include "lc_view.h"
#include "lc_viewwidget.h"
#include "camera.h"
#include "project.h"
#include "object.h"
#include "piece.h"
#include "minifig.h"
#include "group.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

static const int THUMBNAIL_WIDTH = 96;
static const int THUMBNAIL_HEIGHT = 72;

void lcPoseAnimateFrame(lcModel* Model, const lcAnimateFrame& Frame, QSet<lcPiece*>& AnimateForcedHidden)
{
	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcPiece* PiecePtr = Piece.get();

		// A piece not present in this frame's snapshot didn't exist yet when the frame was
		// captured (it was added later) - hide it here instead of leaving it visible at whatever
		// position it happens to currently be in. Only ever un-hide a piece THIS system hid
		// itself (tracked in AnimateForcedHidden) - never a piece the user hid manually via the
		// native Hide Selected feature, which shares the same underlying mHidden flag.
		const bool ExistsInFrame = Frame.Positions.contains(PiecePtr);

		if (!ExistsInFrame)
		{
			if (!Piece->IsHidden())
				AnimateForcedHidden.insert(PiecePtr);

			Piece->SetHidden(true);
		}
		else
		{
			if (AnimateForcedHidden.remove(PiecePtr))
				Piece->SetHidden(false);

			Piece->SetPosition(Frame.Positions.value(PiecePtr), 1, false);
			Piece->SetRotation(Frame.Rotations.value(PiecePtr), 1, false);
		}

		Piece->UpdatePosition(1);
	}

	if (Frame.HasCamera)
	{
		if (lcView* View = gMainWindow->GetActiveView())
		{
			if (lcCamera* Camera = View->GetCamera())
			{
				Camera->SetPosition(Frame.CameraPosition, 1, false);
				Camera->SetTargetPosition(Frame.CameraTarget, 1, false);
				Camera->SetUpVector(Frame.CameraUpVector, 1, false);
				Camera->SetProjection(Frame.CameraProjection);
				Camera->UpdatePosition(1);
			}
		}
	}
}

static QJsonArray Vector3ToJson(const lcVector3& Vector)
{
	return QJsonArray{ Vector.x, Vector.y, Vector.z };
}

static lcVector3 Vector3FromJson(const QJsonArray& Array)
{
	if (Array.size() != 3)
		return lcVector3(0.0f, 0.0f, 0.0f);

	return lcVector3(static_cast<float>(Array[0].toDouble()), static_cast<float>(Array[1].toDouble()), static_cast<float>(Array[2].toDouble()));
}

static QJsonArray Matrix33ToJson(const lcMatrix33& Matrix)
{
	QJsonArray Array;
	const float* Floats = Matrix.GetFloats();

	for (int Index = 0; Index < 9; Index++)
		Array.append(Floats[Index]);

	return Array;
}

static lcMatrix33 Matrix33FromJson(const QJsonArray& Array)
{
	if (Array.size() != 9)
		return lcMatrix33Identity();

	float F[9];

	for (int Index = 0; Index < 9; Index++)
		F[Index] = static_cast<float>(Array[Index].toDouble());

	return lcMatrix33(lcVector3(F[0], F[1], F[2]), lcVector3(F[3], F[4], F[5]), lcVector3(F[6], F[7], F[8]));
}

// Shifting cached thumbnail indices on insert/delete (instead of clearing the whole cache) means
// only the frame that actually changed needs re-rendering, not every frame in the animation.
static void ThumbnailCacheOnInsert(QMap<int, QIcon>& Cache, int InsertIndex)
{
	QMap<int, QIcon> Shifted;

	for (auto It = Cache.constBegin(); It != Cache.constEnd(); ++It)
		Shifted.insert(It.key() >= InsertIndex ? It.key() + 1 : It.key(), It.value());

	Cache = Shifted;
}

static void ThumbnailCacheOnRemove(QMap<int, QIcon>& Cache, int RemoveIndex)
{
	QMap<int, QIcon> Shifted;

	for (auto It = Cache.constBegin(); It != Cache.constEnd(); ++It)
	{
		const int Index = It.key();

		if (Index < RemoveIndex)
			Shifted.insert(Index, It.value());
		else if (Index > RemoveIndex)
			Shifted.insert(Index - 1, It.value());
		// Index == RemoveIndex: that frame no longer exists, drop it.
	}

	Cache = Shifted;
}

lcAnimateWidget::lcAnimateWidget(QWidget* Parent)
	: QWidget(Parent)
{
	QVBoxLayout* MainLayout = new QVBoxLayout(this);
	MainLayout->setContentsMargins(4, 4, 4, 4);

	// Row 1: onion skin preview | big capture button | play/fps
	QHBoxLayout* ControlLayout = new QHBoxLayout;

	QVBoxLayout* OnionLayout = new QVBoxLayout;
	mSocketModeCheck = new QCheckBox(tr("Socket Mode"), this);
	mSocketModeCheck->setChecked(true);
	mSocketModeCheck->setToolTip(tr("On: minifig limb pieces can only rotate, staying attached to their sockets. Off (Free Move): they can be dragged and detached, e.g. to pull an arm out of its socket for one frame. Doesn't affect ordinary piece placement."));
	OnionLayout->addWidget(mSocketModeCheck);
	mOnionSkinCheck = new QCheckBox(tr("Onion Skin"), this);
	mOnionSkinCheck->setToolTip(tr("Show a small reference image of the previous frame so you can see how far to move things"));
	mOnionSkinPreview = new QLabel(tr("Onion skin off"), this);
	mOnionSkinPreview->setFixedSize(120, 90);
	mOnionSkinPreview->setAlignment(Qt::AlignCenter);
	mOnionSkinPreview->setStyleSheet(QLatin1String("QLabel { border: 1px solid palette(mid); }"));
	OnionLayout->addWidget(mOnionSkinCheck);
	OnionLayout->addWidget(mOnionSkinPreview);
	ControlLayout->addLayout(OnionLayout);

	ControlLayout->addStretch();

	mCaptureButton = new QPushButton(tr("●  Capture Frame"), this);
	mCaptureButton->setToolTip(tr("Snapshot every piece's position/rotation and the current camera view into a new frame, like pressing the shutter on a stop-motion camera"));
	QFont CaptureFont = mCaptureButton->font();
	CaptureFont.setPointSize(CaptureFont.pointSize() + 4);
	CaptureFont.setBold(true);
	mCaptureButton->setFont(CaptureFont);
	mCaptureButton->setMinimumSize(200, 64);
	mCaptureButton->setStyleSheet(QLatin1String(
		"QPushButton { border-radius: 32px; background-color: #d64545; color: white; }"
		"QPushButton:hover { background-color: #e05a5a; }"
		"QPushButton:pressed { background-color: #b83a3a; }"));
	ControlLayout->addWidget(mCaptureButton);

	ControlLayout->addStretch();

	QVBoxLayout* PlayLayout = new QVBoxLayout;
	QHBoxLayout* PlayRow = new QHBoxLayout;
	mPlayButton = new QPushButton(tr("Play"), this);
	PlayRow->addWidget(mPlayButton);
	PlayRow->addWidget(new QLabel(tr("fps:"), this));
	mFpsSpinBox = new QSpinBox(this);
	mFpsSpinBox->setRange(1, 60);
	mFpsSpinBox->setValue(12);
	PlayRow->addWidget(mFpsSpinBox);
	mAutoKeyframeCheck = new QCheckBox(tr("Auto Key"), this);
	mAutoKeyframeCheck->setChecked(true);
	mAutoKeyframeCheck->setToolTip(tr("Automatically add a keyframe when the scene changes in Constant Keyframe mode"));
	PlayRow->addWidget(mAutoKeyframeCheck);
	PlayLayout->addLayout(PlayRow);
	mFrameLabel = new QLabel(tr("Frame 1 / 1"), this);
	PlayLayout->addWidget(mFrameLabel);
	ControlLayout->addLayout(PlayLayout);

	MainLayout->addLayout(ControlLayout);

	// Row 2: duplicate/delete/export
	QHBoxLayout* ActionLayout = new QHBoxLayout;
	QPushButton* DuplicateButton = new QPushButton(tr("Duplicate Frame"), this);
	DuplicateButton->setToolTip(tr("Hold the current pose for one more frame, without capturing anything new"));
	mDeleteButton = new QPushButton(tr("Delete Frame"), this);
	ActionLayout->addWidget(DuplicateButton);
	ActionLayout->addWidget(mDeleteButton);

	QPushButton* AttachToHandButton = new QPushButton(tr("Attach to Hand"), this);
	AttachToHandButton->setToolTip(tr("Select a hand piece and a holdable accessory (tool, weapon, cup, etc.) to snap the accessory into the hand's grip"));
	ActionLayout->addWidget(AttachToHandButton);

	QPushButton* MirrorPoseButton = new QPushButton(tr("Mirror Pose"), this);
	MirrorPoseButton->setToolTip(tr("Select one leg of a Posable minifig to copy its current pose onto the opposite leg"));
	ActionLayout->addWidget(MirrorPoseButton);

	QPushButton* WalkCycleButton = new QPushButton(tr("Walk Cycle..."), this);
	WalkCycleButton->setToolTip(tr("Select a Posable minifig and auto-generate an alternating-leg walk cycle across several frames"));
	ActionLayout->addWidget(WalkCycleButton);

	QPushButton* RagdollButton = new QPushButton(tr("Ragdoll Death..."), this);
	RagdollButton->setToolTip(tr("Select a Posable minifig and generate a ragdoll death animation: knockback, fall, bounce, settle"));
	ActionLayout->addWidget(RagdollButton);

	ActionLayout->addStretch();
	mExportButton = new QPushButton(tr("Export Animation..."), this);
	ActionLayout->addWidget(mExportButton);
	MainLayout->addLayout(ActionLayout);

	// Row 3: mode selector (own row above filmstrip)
	QHBoxLayout* ModeLayout = new QHBoxLayout;
	mModeSelector = new QComboBox(this);
	mModeSelector->addItem(tr("Stop Motion"));
	mModeSelector->addItem(tr("Constant Keyframe"));
	mModeSelector->setCurrentIndex(0);
	ModeLayout->addWidget(new QLabel(tr("Mode:"), this));
	ModeLayout->addWidget(mModeSelector);
	ModeLayout->addStretch();
	MainLayout->addLayout(ModeLayout);

	// Row 4: keyframe timeline controls (hidden in Stop Motion mode)
	mKeyframeControls = new QWidget(this);
	mKeyframeControls->setVisible(false);
	QVBoxLayout* KFLayout = new QVBoxLayout(mKeyframeControls);
	KFLayout->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout* KFButtonRow = new QHBoxLayout;
	mStepBackButton = new QPushButton(tr("<<"), this);
	mStepBackButton->setToolTip(tr("Step back 10 frames"));
	mStepBackButton->setFixedWidth(32);
	mStepForwardButton = new QPushButton(tr(">>"), this);
	mStepForwardButton->setToolTip(tr("Step forward 10 frames"));
	mStepForwardButton->setFixedWidth(32);
	mAddKeyframeButton = new QPushButton(tr("Add Keyframe"), this);
	mAddKeyframeButton->setToolTip(tr("Capture the current pose as a new keyframe on the timeline"));
	mDeleteKeyframeButton = new QPushButton(tr("Delete Keyframe"), this);
	mDeleteKeyframeButton->setToolTip(tr("Remove the selected keyframe"));
	mClearKeyframeButton = new QPushButton(tr("Clear All"), this);
	mClearKeyframeButton->setToolTip(tr("Delete all keyframes and frames"));
	KFButtonRow->addWidget(mStepBackButton);
	KFButtonRow->addWidget(mStepForwardButton);
	KFButtonRow->addWidget(mAddKeyframeButton);
	KFButtonRow->addWidget(mDeleteKeyframeButton);
	KFButtonRow->addWidget(mClearKeyframeButton);
	KFButtonRow->addWidget(new QLabel(tr("Easing:"), this));
	mEasingCombo = new QComboBox(this);
	mEasingCombo->addItems({ tr("Linear"), tr("Ease In"), tr("Ease Out"), tr("Ease In/Out") });
	KFButtonRow->addWidget(mEasingCombo);
	KFButtonRow->addStretch();
	KFLayout->addLayout(KFButtonRow);

	mTimelineWidget = new lcKeyframeTimelineWidget(this);
	KFLayout->addWidget(mTimelineWidget);

	MainLayout->addWidget(mKeyframeControls);

	// Row 5: frame filmstrip
	mFilmstrip = new QListWidget(this);
	mFilmstrip->setViewMode(QListView::IconMode);
	mFilmstrip->setFlow(QListView::LeftToRight);
	mFilmstrip->setWrapping(false);
	mFilmstrip->setMovement(QListView::Static);
	mFilmstrip->setIconSize(QSize(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT));
	mFilmstrip->setFixedHeight(THUMBNAIL_HEIGHT + 40);
	mFilmstrip->setResizeMode(QListView::Adjust);
	mFilmstrip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mFilmstrip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	mFilmstrip->setSelectionMode(QAbstractItemView::SingleSelection);
	MainLayout->addWidget(mFilmstrip);

	mPlayTimer = new QTimer(this);
	connect(mPlayTimer, &QTimer::timeout, this, &lcAnimateWidget::Timeout);

	connect(mCaptureButton, &QPushButton::clicked, this, &lcAnimateWidget::CaptureClicked);
	connect(DuplicateButton, &QPushButton::clicked, this, &lcAnimateWidget::DuplicateClicked);
	connect(mDeleteButton, &QPushButton::clicked, this, &lcAnimateWidget::DeleteClicked);
	connect(AttachToHandButton, &QPushButton::clicked, this, &lcAnimateWidget::AttachToHandClicked);
	connect(MirrorPoseButton, &QPushButton::clicked, this, &lcAnimateWidget::MirrorPoseClicked);
	connect(WalkCycleButton, &QPushButton::clicked, this, &lcAnimateWidget::WalkCycleClicked);
	connect(RagdollButton, &QPushButton::clicked, this, &lcAnimateWidget::RagdollClicked);
	connect(mPlayButton, &QPushButton::clicked, this, &lcAnimateWidget::PlayPauseClicked);
	connect(mOnionSkinCheck, &QCheckBox::toggled, this, &lcAnimateWidget::OnionSkinToggled);
	connect(mExportButton, &QPushButton::clicked, this, &lcAnimateWidget::ExportClicked);
	connect(mFilmstrip, &QListWidget::currentRowChanged, this, &lcAnimateWidget::FilmstripItemChanged);
	connect(mSocketModeCheck, &QCheckBox::toggled, this, &lcAnimateWidget::SocketModeToggled);
	connect(mModeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &lcAnimateWidget::ModeChanged);
	connect(mAddKeyframeButton, &QPushButton::clicked, this, &lcAnimateWidget::AddKeyframeClicked);
	connect(mDeleteKeyframeButton, &QPushButton::clicked, this, &lcAnimateWidget::DeleteKeyframeClicked);
	connect(mClearKeyframeButton, &QPushButton::clicked, this, &lcAnimateWidget::ClearKeyframeClicked);
	connect(mEasingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &lcAnimateWidget::EasingChanged);
	connect(mTimelineWidget, &lcKeyframeTimelineWidget::KeyframeSelected, this, &lcAnimateWidget::TimelineKeyframeSelected);
	connect(mTimelineWidget, &lcKeyframeTimelineWidget::CurrentTimeDragged, this, &lcAnimateWidget::TimelineTimeDragged);
	connect(mStepBackButton, &QPushButton::clicked, this, [this]() { TimelineStep(-10); });
	connect(mStepForwardButton, &QPushButton::clicked, this, [this]() { TimelineStep(10); });
	connect(mFpsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int NewFps)
	{
		if (NewFps == mFpsLastValue)
			return;

		lcModel* Model = lcGetActiveModel();
		if (!Model)
		{
			mFpsLastValue = NewFps;
			return;
		}

		lcAnimateDocumentState& State = GetState(Model);
		if (State.AnimateMode != lcAnimateMode::ConstantKeyframe)
		{
			mFpsLastValue = NewFps;
			return;
		}

		const float Ratio = (float)NewFps / (float)mFpsLastValue;
		mFpsLastValue = NewFps;

		if (Ratio == 1.0f)
			return;

		for (lcKeyframePoint& Kf : State.Keyframes)
			Kf.Time = qRound(Kf.Time * Ratio);

		BakeKeyframes(Model, State);
		mTimelineWidget->SetKeyframes(&State.Keyframes);
		mTimelineWidget->SetFrameRange(0, std::max((int)State.Frames.size(), 10));
		mTimelineWidget->SetCurrentTime(State.CurrentFrameIndex);
		Update();
	});

	SocketModeToggled(mSocketModeCheck->isChecked());
}

lcAnimateDocumentState& lcAnimateWidget::GetState(lcModel* Model)
{
	const auto It = mDocumentStates.find(Model);

	if (It != mDocumentStates.end())
		return It.value();

	lcAnimateDocumentState& State = mDocumentStates[Model];
	State.Frames.push_back(SnapshotFrame(Model));
	State.CurrentFrameIndex = 0;

	return State;
}

void lcAnimateWidget::ForgetModel(lcModel* Model)
{
	mDocumentStates.remove(Model);

	if (mLastFilmstripModel == Model)
		mLastFilmstripModel = nullptr;
}

lcAnimateFrame lcAnimateWidget::SnapshotFrame(lcModel* Model) const
{
	lcAnimateFrame Frame;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		// Skip pieces currently hidden (native Hide Selected, or this system's own absent-from-
		// frame hiding) - capturing a frame should only include what's actually visible right now.
		if (Piece->IsHidden())
			continue;

		Frame.Positions[Piece.get()] = Piece->GetPosition();
		Frame.Rotations[Piece.get()] = Piece->GetRotation();
	}

	if (lcView* View = gMainWindow->GetActiveView())
	{
		if (lcCamera* Camera = View->GetCamera())
		{
			Frame.CameraPosition = Camera->GetPosition();
			Frame.CameraTarget = Camera->GetTargetPosition();
			Frame.CameraUpVector = Camera->GetUpVector();
			Frame.CameraProjection = Camera->GetProjection();
			Frame.HasCamera = true;
		}
	}

	return Frame;
}

void lcAnimateWidget::ApplyFrame(lcModel* Model, int FrameIndex)
{
	mIsApplyingFrame = true;

	lcAnimateDocumentState& State = GetState(Model);

	if (FrameIndex < 0 || FrameIndex >= static_cast<int>(State.Frames.size()))
	{
		mIsApplyingFrame = false;
		return;
	}

	lcPoseAnimateFrame(Model, State.Frames[FrameIndex], State.AnimateForcedHidden);
	Model->SetCurrentStep(1); // redraws the live viewport and refreshes the rest of the UI

	mIsApplyingFrame = false;
}

QIcon lcAnimateWidget::RenderFrameThumbnail(lcModel* Model, int FrameIndex, int Width, int Height)
{
	lcAnimateDocumentState& State = GetState(Model);

	if (FrameIndex < 0 || FrameIndex >= static_cast<int>(State.Frames.size()))
		return QIcon();

	// Poses pieces directly without going through SetCurrentStep, so this doesn't touch the live
	// viewport - the caller is responsible for restoring the real current frame afterward.
	lcPoseAnimateFrame(Model, State.Frames[FrameIndex], State.AnimateForcedHidden);

	lcView View(lcViewType::View, Model);
	View.SetOffscreenContext();
	View.MakeCurrent();
	View.SetSize(Width, Height);

	std::vector<QImage> Images = View.GetStepImages(1, 1);

	if (Images.empty())
		return QIcon();

	return QIcon(QPixmap::fromImage(Images.front()));
}

void lcAnimateWidget::RefreshFilmstrip(lcModel* Model)
{
	lcAnimateDocumentState& State = GetState(Model);

	mIgnoreUpdates = true;
	mFilmstrip->clear();

	// Snapshot ALL pieces (including hidden ones) before any temporary re-posing for thumbnails.
	// Using SnapshotFrame (which skips hidden pieces) would lose them - thumbnail rendering below
	// hides pieces that don't belong to a frame, and the restore step wouldn't know about pieces
	// that were already hidden by the animation system before the refresh.
	lcAnimateFrame LiveState;
	bool NeedsViewportRestore = false;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcPiece* Ptr = Piece.get();
		LiveState.Positions[Ptr] = Ptr->GetPosition();
		LiveState.Rotations[Ptr] = Ptr->GetRotation();
	}

	if (lcView* View = gMainWindow->GetActiveView())
	{
		if (lcCamera* Camera = View->GetCamera())
		{
			LiveState.CameraPosition = Camera->GetPosition();
			LiveState.CameraTarget = Camera->GetTargetPosition();
			LiveState.CameraUpVector = Camera->GetUpVector();
			LiveState.CameraProjection = Camera->GetProjection();
			LiveState.HasCamera = true;
		}
	}

	for (int Index = 0; Index < static_cast<int>(State.Frames.size()); Index++)
	{
		QIcon Icon = State.ThumbnailCache.value(Index);

		if (Icon.isNull())
		{
			Icon = RenderFrameThumbnail(Model, Index, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
			State.ThumbnailCache.insert(Index, Icon);
			NeedsViewportRestore = true;
		}

		mFilmstrip->addItem(new QListWidgetItem(Icon, QString::number(Index + 1)));
	}

	// Rendering thumbnails above may have posed pieces as other frames - restore the real live
	// state once at the end instead of after every single thumbnail (avoids viewport flicker too).
	if (NeedsViewportRestore)
	{
		lcPoseAnimateFrame(Model, LiveState, State.AnimateForcedHidden);
		Model->SetCurrentStep(1);
	}

	mFilmstrip->setCurrentRow(State.CurrentFrameIndex);
	mIgnoreUpdates = false;
}

void lcAnimateWidget::RefreshOnionSkin(lcModel* Model)
{
	lcView* ActiveView = gMainWindow->GetActiveView();
	lcViewWidget* ViewWidget = ActiveView ? ActiveView->GetWidget() : nullptr;

	if (!mOnionSkinCheck->isChecked())
	{
		if (ActiveView)
			ActiveView->ClearGhost();
		if (ViewWidget)
			ViewWidget->ClearGhostImage();
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("Onion skin off"));
		return;
	}

	lcAnimateDocumentState& State = GetState(Model);

	if (State.CurrentFrameIndex <= 0)
	{
		if (ActiveView)
			ActiveView->ClearGhost();
		if (ViewWidget)
			ViewWidget->ClearGhostImage();
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("No previous frame"));
		return;
	}

	const int PreviousIndex = State.CurrentFrameIndex - 1;

	// Set ghost frame for OpenGL pass
	if (ActiveView)
	{
		const lcAnimateFrame& PrevFrame = State.Frames[PreviousIndex];
		ActiveView->SetGhostFrame(PrevFrame.Positions, PrevFrame.Rotations, 0.5f);
	}

	// Render QImage overlay for viewport (works even when OpenGL ghost pass doesn't)
	if (ViewWidget)
	{
		const lcAnimateFrame LiveState = SnapshotFrame(Model);
		QIcon OverlayIcon = RenderFrameThumbnail(Model, PreviousIndex, 640, 480);
		ViewWidget->ShowGhostImage(OverlayIcon.pixmap(640, 480).toImage());
		lcPoseAnimateFrame(Model, LiveState, State.AnimateForcedHidden);
		Model->SetCurrentStep(1);
	}

	// Dock thumbnail
	QIcon Icon = State.ThumbnailCache.value(PreviousIndex);

	if (Icon.isNull())
	{
		const lcAnimateFrame LiveState = SnapshotFrame(Model);

		Icon = RenderFrameThumbnail(Model, PreviousIndex, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
		State.ThumbnailCache.insert(PreviousIndex, Icon);

		lcPoseAnimateFrame(Model, LiveState, State.AnimateForcedHidden);
		Model->SetCurrentStep(1);
	}

	mOnionSkinPreview->setText(QString());
	mOnionSkinPreview->setPixmap(Icon.pixmap(120, 90));
}

void lcAnimateWidget::Update()
{
	// Re-entrancy guard: ApplyFrame's Model->SetCurrentStep(1) synchronously calls back into this
	// same Update() (via lcModel::SetCurrentStep -> lcMainWindow::UpdateCurrentStep) before the
	// outer call below has finished. Without this, every frame change did the refresh work twice.
	if (mUpdating)
		return;

	mUpdating = true;

	lcModel* Model = lcGetActiveModel();

	if (Model)
	{
		lcAnimateDocumentState& State = GetState(Model);

		const int FrameCount = static_cast<int>(State.Frames.size());

		mFrameLabel->setText(tr("Frame %1 / %2").arg(State.CurrentFrameIndex + 1).arg(FrameCount));
		mDeleteButton->setEnabled(FrameCount > 1);

		// Compare model identity too, not just frame count - otherwise switching to a different
		// model (e.g. entering a submodel) that happens to have the same frame count as the one
		// previously displayed would leave its stale thumbnails on screen.
		if (mFilmstrip->count() != FrameCount || mLastFilmstripModel != Model)
		{
			RefreshFilmstrip(Model);
			mLastFilmstripModel = Model;
		}
		else
		{
			mIgnoreUpdates = true;
			mFilmstrip->setCurrentRow(State.CurrentFrameIndex);
			mIgnoreUpdates = false;
		}

		if (!mPlayTimer->isActive())
			RefreshOnionSkin(Model);

		// Auto-keyframe in Constant Keyframe mode
		if (State.AnimateMode == lcAnimateMode::ConstantKeyframe && mAutoKeyframeCheck->isChecked() && !mIsApplyingFrame && !mSkipAutoKeyframe)
		{
			// Capture ALL pieces (not just visible ones) — SnapshotFrame skips hidden pieces,
			// but pieces temporarily hidden by AnimateForcedHidden (absent from current frame)
			// are still part of the scene and must be preserved in auto-keyframes.
			lcAnimateFrame CurrentSnapshot;
			for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
			{
				lcPiece* Ptr = Piece.get();
				CurrentSnapshot.Positions[Ptr] = Ptr->GetPosition();
				CurrentSnapshot.Rotations[Ptr] = Ptr->GetRotation();
			}
			if (lcView* ActiveView = gMainWindow->GetActiveView())
			{
				if (lcCamera* Camera = ActiveView->GetCamera())
				{
					CurrentSnapshot.CameraPosition = Camera->GetPosition();
					CurrentSnapshot.CameraTarget = Camera->GetTargetPosition();
					CurrentSnapshot.CameraUpVector = Camera->GetUpVector();
					CurrentSnapshot.CameraProjection = Camera->GetProjection();
					CurrentSnapshot.HasCamera = true;
				}
			}

			if (!State.AutoKeyframeInitialized)
			{
				State.LastAutoKeyframeDigest = CurrentSnapshot;
				State.AutoKeyframeInitialized = true;
			}
			else if (CurrentSnapshot.Positions != State.LastAutoKeyframeDigest.Positions || CurrentSnapshot.Rotations != State.LastAutoKeyframeDigest.Rotations)
			{
				lcKeyframePoint Pt;
				Pt.Time = State.CurrentFrameIndex;
				Pt.Pose.Positions = CurrentSnapshot.Positions;
				Pt.Pose.Rotations = CurrentSnapshot.Rotations;
				Pt.Pose.CameraPosition = CurrentSnapshot.CameraPosition;
				Pt.Pose.CameraTarget = CurrentSnapshot.CameraTarget;
				Pt.Pose.CameraUpVector = CurrentSnapshot.CameraUpVector;
				Pt.Pose.CameraProjection = CurrentSnapshot.CameraProjection;
				Pt.Pose.HasCamera = CurrentSnapshot.HasCamera;

				State.Keyframes.push_back(Pt);
				mTimelineWidget->SetKeyframes(&State.Keyframes);
				BakeKeyframes(Model, State);
				RefreshFilmstrip(Model);

				State.LastAutoKeyframeDigest = CurrentSnapshot;
			}
		}
	}

	mUpdating = false;
}

void lcAnimateWidget::FilmstripItemChanged(int Row)
{
	if (mIgnoreUpdates || Row < 0)
		return;

	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	GetState(Model).CurrentFrameIndex = Row;
	ApplyFrame(Model, Row);
	Update();
}

void lcAnimateWidget::CaptureClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	// Note: this only mutates lcAnimateWidget's own frame list, never a piece, so there is
	// deliberately no BeginHistorySequence/EndHistorySequence wrapper here - LeoCAD's undo system
	// diffs piece/camera/light/group state, and would find nothing changed and silently drop the
	// entry. Capture/Duplicate/Delete Frame are not undoable via Ctrl+Z; that would need a
	// dedicated undo stack for the frame list itself, which doesn't exist yet.
	lcAnimateDocumentState& State = GetState(Model);

	const int InsertIndex = State.CurrentFrameIndex + 1;
	State.Frames.insert(State.Frames.begin() + InsertIndex, SnapshotFrame(Model));
	State.CurrentFrameIndex = InsertIndex;
	ThumbnailCacheOnInsert(State.ThumbnailCache, InsertIndex);

	Update();
}

void lcAnimateWidget::DuplicateClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	// See the comment in CaptureClicked: no history wrapper, this doesn't touch piece data.
	lcAnimateDocumentState& State = GetState(Model);

	// Frames can be empty here in Constant Keyframe mode (Clear/Delete Keyframe can drop it to 0
	// while this button stays visible) - without this guard, both indexing operations below are
	// undefined behavior on an empty vector.
	if (State.Frames.empty())
		return;

	const int InsertIndex = State.CurrentFrameIndex + 1;
	State.Frames.insert(State.Frames.begin() + InsertIndex, State.Frames[State.CurrentFrameIndex]);
	ThumbnailCacheOnInsert(State.ThumbnailCache, InsertIndex);

	// The duplicated frame is identical to the one it was copied from, so its thumbnail is too -
	// reuse it instead of leaving a cache miss that would force a render for a picture we already have.
	if (State.ThumbnailCache.contains(State.CurrentFrameIndex))
		State.ThumbnailCache.insert(InsertIndex, State.ThumbnailCache.value(State.CurrentFrameIndex));

	State.CurrentFrameIndex = InsertIndex;

	ApplyFrame(Model, State.CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::DeleteClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);

	if (State.Frames.size() <= 1)
		return;

	// See the comment in CaptureClicked: no history wrapper, this doesn't touch piece data.
	State.Frames.erase(State.Frames.begin() + State.CurrentFrameIndex);
	ThumbnailCacheOnRemove(State.ThumbnailCache, State.CurrentFrameIndex);

	if (State.CurrentFrameIndex >= static_cast<int>(State.Frames.size()))
		State.CurrentFrameIndex = static_cast<int>(State.Frames.size()) - 1;

	ApplyFrame(Model, State.CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::PlayPauseClicked()
{
	if (mPlayTimer->isActive())
	{
		mPlayTimer->stop();
		mPlayButton->setText(tr("Play"));

		lcModel* Model = lcGetActiveModel();

		if (Model)
			RefreshOnionSkin(Model);

		return;
	}

	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);

	if (State.CurrentFrameIndex >= static_cast<int>(State.Frames.size()) - 1)
	{
		State.CurrentFrameIndex = 0;
		ApplyFrame(Model, State.CurrentFrameIndex);
	}

	mPlayTimer->start(1000 / mFpsSpinBox->value());
	mPlayButton->setText(tr("Pause"));
}

void lcAnimateWidget::Timeout()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
	{
		mPlayTimer->stop();
		mPlayButton->setText(tr("Play"));
		return;
	}

	lcAnimateDocumentState& State = GetState(Model);

	if (State.CurrentFrameIndex >= static_cast<int>(State.Frames.size()) - 1)
		State.CurrentFrameIndex = 0;
	else
		State.CurrentFrameIndex++;

	ApplyFrame(Model, State.CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::OnionSkinToggled(bool /*Checked*/)
{
	lcModel* Model = lcGetActiveModel();

	if (Model)
		RefreshOnionSkin(Model);
}

void lcAnimateWidget::SocketModeToggled(bool Checked)
{
	// Enforced in lcView's mouse-manipulation code (see lc_view.cpp's UpdateTrackTool), scoped to
	// pieces in a Posable minifig limb group only - disabling the Move tool/action globally would
	// have blocked ordinary piece placement for anyone doing regular set building, not just posing.
	gMainWindow->SetSocketModeEnabled(Checked);
}

static const lcMinifigPieceInfo* FindMinifigPieceEntry(const std::vector<lcMinifigPieceInfo>& List, PieceInfo* Info)
{
	for (const lcMinifigPieceInfo& Entry : List)
		if (Entry.Info == Info)
			return &Entry;

	return nullptr;
}

void lcAnimateWidget::AttachToHandClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	int Flags = 0;
	std::vector<lcObject*> Selection;
	lcObject* Focus = nullptr;
	Model->GetSelectionInformation(&Flags, Selection, &Focus);

	std::vector<lcPiece*> Pieces;

	for (lcObject* Object : Selection)
		if (Object->IsPiece())
			Pieces.push_back((lcPiece*)Object);

	if (Pieces.size() != 2)
	{
		QMessageBox::information(this, tr("Attach to Hand"), tr("Select exactly one hand piece and one accessory piece."));
		return;
	}

	// Lazily created and kept alive for the app's lifetime just to reuse its parsed minifig.ini
	// hand/accessory offset tables (the same authoritative data the Minifig Wizard itself uses) -
	// avoids re-implementing that parsing here.
	static MinifigWizard* Wizard = new MinifigWizard();

	lcPiece* HandPiece = nullptr;
	lcPiece* AccessoryPiece = nullptr;
	const lcMinifigPieceInfo* AccessoryEntry = nullptr;

	for (int Index = 0; Index < 2 && !HandPiece; Index++)
	{
		lcPiece* Piece = Pieces[Index];
		lcPiece* Other = Pieces[1 - Index];

		if (FindMinifigPieceEntry(Wizard->mSettings[LC_MFW_RHAND], Piece->mPieceInfo))
		{
			HandPiece = Piece;
			AccessoryPiece = Other;
			// ponytail: always uses the right-hand offset table. Almost every holdable accessory
			// uses an identity offset that's the same for either hand; only a handful of
			// oddly-shaped items (animal figures, etc.) differ, and those would need a Left/Right
			// choice here to be exact.
			AccessoryEntry = FindMinifigPieceEntry(Wizard->mSettings[LC_MFW_RHANDA], Other->mPieceInfo);
		}
	}

	if (!HandPiece || !AccessoryEntry)
	{
		QMessageBox::information(this, tr("Attach to Hand"), tr("Couldn't recognize one selected piece as a hand and the other as something a hand can hold."));
		return;
	}

	mSkipAutoKeyframe = true;
	Model->RunInHistorySequence(tr("Attach to Hand"), [&]()
	{
		const lcMatrix44 HandWorldMatrix(HandPiece->GetRotation(), HandPiece->GetPosition());
		const lcMatrix44 AccessoryWorldMatrix = lcMul(AccessoryEntry->Offset, HandWorldMatrix);

		AccessoryPiece->SetPosition(AccessoryWorldMatrix.GetTranslation(), 1, false);
		AccessoryPiece->SetRotation(lcMatrix33(AccessoryWorldMatrix), 1, false);
		AccessoryPiece->UpdatePosition(1);

		if (lcGroup* HandGroup = HandPiece->GetGroup())
			AccessoryPiece->SetGroup(HandGroup);
	});

	Model->SetCurrentStep(1);
	mSkipAutoKeyframe = false;
}

void lcAnimateWidget::MirrorPoseClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	int Flags = 0;
	std::vector<lcObject*> Selection;
	lcObject* Focus = nullptr;
	Model->GetSelectionInformation(&Flags, Selection, &Focus);

	std::vector<lcPiece*> Pieces;

	for (lcObject* Object : Selection)
		if (Object->IsPiece())
			Pieces.push_back((lcPiece*)Object);

	if (Pieces.size() != 1)
	{
		QMessageBox::information(this, tr("Mirror Pose"), tr("Select exactly one Right Leg or Left Leg piece of a Posable minifig."));
		return;
	}

	lcPiece* SourcePiece = Pieces[0];
	lcGroup* SourceGroup = SourcePiece->GetGroup();
	lcGroup* Family = SourceGroup ? SourceGroup->mMinifigFamily : nullptr;

	if (!Family)
	{
		QMessageBox::information(this, tr("Mirror Pose"), tr("The selected piece isn't part of a Posable minifig (see the Posable checkbox in the Minifig Wizard)."));
		return;
	}

	// Scoped to legs: right/left leg reliably share the same LDraw part on a standard minifig, so
	// copying the rotation matrix directly (no angle decomposition) is exact and safe. Arms aren't
	// included since left/right arm are usually different mirrored parts.
	QString TargetName;

	if (SourceGroup->mName.contains(QLatin1String("Right Leg")))
		TargetName = QString(SourceGroup->mName).replace(QLatin1String("Right Leg"), QLatin1String("Left Leg"));
	else if (SourceGroup->mName.contains(QLatin1String("Left Leg")))
		TargetName = QString(SourceGroup->mName).replace(QLatin1String("Left Leg"), QLatin1String("Right Leg"));
	else
	{
		QMessageBox::information(this, tr("Mirror Pose"), tr("Mirror Pose only supports legs right now - select a Right Leg or Left Leg piece."));
		return;
	}

	lcGroup* TargetGroup = nullptr;

	for (const std::unique_ptr<lcGroup>& Candidate : Model->GetGroups())
	{
		if (Candidate->mMinifigFamily == Family && Candidate->mName == TargetName)
		{
			TargetGroup = Candidate.get();
			break;
		}
	}

	lcPiece* TargetPiece = nullptr;

	if (TargetGroup)
	{
		for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
		{
			if (Piece->GetGroup() == TargetGroup && Piece->mPieceInfo == SourcePiece->mPieceInfo)
			{
				TargetPiece = Piece.get();
				break;
			}
		}
	}

	if (!TargetPiece)
	{
		QMessageBox::information(this, tr("Mirror Pose"), tr("Couldn't find a matching piece on the opposite leg (different parts on each side aren't supported)."));
		return;
	}

	mSkipAutoKeyframe = true;
	Model->RunInHistorySequence(tr("Mirror Pose"), [&]()
	{
		TargetPiece->SetRotation(SourcePiece->GetRotation(), 1, false);
		TargetPiece->UpdatePosition(1);
	});
	mSkipAutoKeyframe = false;
}

// Shared by WalkCycleClicked/RagdollClicked: after procedurally regenerating State.Frames, mirror
// it into State.Keyframes (one keyframe per frame, Linear easing) so Constant Keyframe mode's later
// BakeKeyframes reproduces the full generated motion instead of a two-point start→end lerp. No-op
// outside Constant Keyframe mode.
static void RebuildKeyframesFromFrames(lcAnimateDocumentState& State)
{
	if (State.AnimateMode != lcAnimateMode::ConstantKeyframe)
		return;

	State.Keyframes.clear();

	for (int i = 0; i < (int)State.Frames.size(); i++)
	{
		lcKeyframePoint Kf;
		Kf.Time = i;
		Kf.Pose.Positions = State.Frames[i].Positions;
		Kf.Pose.Rotations = State.Frames[i].Rotations;
		Kf.Pose.CameraPosition = State.Frames[i].CameraPosition;
		Kf.Pose.CameraTarget = State.Frames[i].CameraTarget;
		Kf.Pose.CameraUpVector = State.Frames[i].CameraUpVector;
		Kf.Pose.CameraProjection = State.Frames[i].CameraProjection;
		Kf.Pose.HasCamera = State.Frames[i].HasCamera;
		Kf.SegmentEasing = lcEasingType::Linear;
		State.Keyframes.push_back(Kf);
	}
}

void lcAnimateWidget::ShowMinifigProjectionGhost(lcModel* Model, const QMap<lcPiece*, lcVector3>& GhostPositions, const QMap<lcPiece*, lcMatrix33>& GhostRotations)
{
	lcView* ActiveView = gMainWindow->GetActiveView();

	if (!ActiveView || !Model || GhostPositions.isEmpty())
		return;

	// OpenGL ghost pass (viewport overlay drawn by lcView::OnDraw).
	ActiveView->SetGhostFrame(GhostPositions, GhostRotations, 0.3f);

	// Render a QImage overlay at small size as a guaranteed-visibility fallback for configurations
	// where the OpenGL ghost pass doesn't reliably composite.
	QMap<lcPiece*, lcVector3> SavedPos;
	QMap<lcPiece*, lcMatrix33> SavedRot;
	QMap<lcPiece*, bool> SavedHidden;

	for (const std::unique_ptr<lcPiece>& P : Model->GetPieces())
	{
		lcPiece* Ptr = P.get();
		SavedPos[Ptr] = Ptr->GetPosition();
		SavedRot[Ptr] = Ptr->GetRotation();
		SavedHidden[Ptr] = P->IsHidden();
	}

	for (auto It = GhostPositions.constBegin(); It != GhostPositions.constEnd(); ++It)
	{
		It.key()->SetPosition(It.value(), 1, false);
		It.key()->SetRotation(GhostRotations.value(It.key()), 1, false);
		It.key()->SetHidden(false);
	}
	Model->SetCurrentStep(1);

	lcView TempView(lcViewType::View, Model);
	TempView.SetOffscreenContext();
	TempView.MakeCurrent();

	const int PW = 320, PH = 240;
	TempView.SetSize(PW, PH);

	lcCamera* Cam = ActiveView->GetCamera();
	if (Cam)
	{
		TempView.GetCamera()->CopyPosition(Cam);
		TempView.GetCamera()->SetProjection(Cam->GetProjection());
	}

	QImage RenderedGhost;
	std::vector<QImage> Images = TempView.GetStepImages(1, 1);
	if (!Images.empty())
		RenderedGhost = Images.front().scaled(PW, PH, Qt::KeepAspectRatio, Qt::SmoothTransformation);

	for (auto It = SavedPos.constBegin(); It != SavedPos.constEnd(); ++It)
	{
		It.key()->SetPosition(It.value(), 1, false);
		It.key()->SetRotation(SavedRot.value(It.key()), 1, false);
		It.key()->SetHidden(SavedHidden.value(It.key()));
	}
	Model->SetCurrentStep(1);

	// Restore+SetCurrentStep may have triggered Update()->RefreshOnionSkin(), which clears ghosts.
	// Re-apply immediately so the ghost stays visible during dialog interaction.
	ActiveView->SetGhostFrame(GhostPositions, GhostRotations, 0.3f);

	if (!RenderedGhost.isNull())
	{
		lcViewWidget* W = ActiveView->GetWidget();
		if (W)
			W->ShowGhostImage(RenderedGhost);
	}
}

void lcAnimateWidget::WalkCycleClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	int Flags = 0;
	std::vector<lcObject*> Selection;
	lcObject* Focus = nullptr;
	Model->GetSelectionInformation(&Flags, Selection, &Focus);

	lcGroup* Family = nullptr;

	for (lcObject* Object : Selection)
	{
		if (Object->IsPiece())
		{
			lcGroup* Group = ((lcPiece*)Object)->GetGroup();

			if (Group && Group->mMinifigFamily)
			{
				Family = Group->mMinifigFamily;
				break;
			}
		}
	}

	if (!Family)
	{
		QMessageBox::information(this, tr("Walk Cycle"), tr("Select (or Alt+click) any part of a Posable minifig first - pose it standing neutrally, since that pose is used as the walk cycle's reference."));
		return;
	}

	lcGroup* RightLegGroup = nullptr;
	lcGroup* LeftLegGroup = nullptr;
	lcGroup* RightArmGroup = nullptr;
	lcGroup* LeftArmGroup = nullptr;

	for (const std::unique_ptr<lcGroup>& Candidate : Model->GetGroups())
	{
		if (Candidate->mMinifigFamily != Family)
			continue;

		if (Candidate->mName.contains(QLatin1String("Right Leg")))
			RightLegGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Left Leg")))
			LeftLegGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Right Arm")))
			RightArmGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Left Arm")))
			LeftArmGroup = Candidate.get();
	}

	if (!RightLegGroup || !LeftLegGroup)
	{
		QMessageBox::information(this, tr("Walk Cycle"), tr("This minifig doesn't have both a Right Leg and a Left Leg group to alternate."));
		return;
	}

	std::vector<lcPiece*> RightLegPieces, LeftLegPieces, RightArmPieces, LeftArmPieces, OtherPieces;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcGroup* Group = Piece->GetGroup();

		if (!Group || Group->mMinifigFamily != Family)
			continue;

		if (Group == RightLegGroup)
			RightLegPieces.push_back(Piece.get());
		else if (Group == LeftLegGroup)
			LeftLegPieces.push_back(Piece.get());
		else if (RightArmGroup && Group == RightArmGroup)
			RightArmPieces.push_back(Piece.get());
		else if (LeftArmGroup && Group == LeftArmGroup)
			LeftArmPieces.push_back(Piece.get());
		else
			OtherPieces.push_back(Piece.get());
	}

	if (RightLegPieces.empty() || LeftLegPieces.empty())
	{
		QMessageBox::information(this, tr("Walk Cycle"), tr("This minifig's Right Leg or Left Leg group has no pieces left in it."));
		return;
	}

	// Build dialog
	QDialog Dialog(this);
	Dialog.setWindowTitle(tr("Walk Cycle"));
	QFormLayout* Form = new QFormLayout(&Dialog);

	QComboBox* GaitCombo = new QComboBox(&Dialog);
	GaitCombo->addItems({ tr("Walk"), tr("Jog"), tr("Run") });
	Form->addRow(tr("Gait:"), GaitCombo);

	QDoubleSpinBox* StrideSpin = new QDoubleSpinBox(&Dialog);
	StrideSpin->setRange(1.0, 60.0);
	StrideSpin->setSuffix(tr(" deg"));
	StrideSpin->setDecimals(0);
	Form->addRow(tr("Stride angle:"), StrideSpin);

	QDoubleSpinBox* ArmSwingSpin = new QDoubleSpinBox(&Dialog);
	ArmSwingSpin->setRange(0.0, 60.0);
	ArmSwingSpin->setValue(15.0);
	ArmSwingSpin->setSuffix(tr(" deg"));
	ArmSwingSpin->setDecimals(0);
	Form->addRow(tr("Arm swing:"), ArmSwingSpin);

	QDoubleSpinBox* DirSpin = new QDoubleSpinBox(&Dialog);
	DirSpin->setRange(0.0, 359.0);
	DirSpin->setSuffix(tr(" deg"));
	DirSpin->setDecimals(0);
	DirSpin->setToolTip(tr("0 = minifig's natural forward, 90 = right"));
	Form->addRow(tr("Direction:"), DirSpin);

	QDoubleSpinBox* DistSpin = new QDoubleSpinBox(&Dialog);
	DistSpin->setRange(0.5, 500.0);
	DistSpin->setDecimals(1);
	DistSpin->setSuffix(tr(" studs"));
	DistSpin->setSingleStep(0.5);
	Form->addRow(tr("Total travel:"), DistSpin);

	static MinifigWizard* TempWiz = new MinifigWizard();
	TempWiz->SetPieceInfo(LC_MFW_RLEG, RightLegPieces.front()->mPieceInfo);
	TempWiz->SetAngle(LC_MFW_RLEG, 0.0f);
	const float LegLen = TempWiz->mMinifig.Matrices[LC_MFW_RLEG].r[3].z + 44.0f;

	auto UpdateStrideFromDist = [&]()
	{
		const double distLdu = DistSpin->value() * 20.0;
		const double ratio = distLdu / (2.0 * LegLen);
		const double clamped = std::max(-1.0, std::min(1.0, ratio));
		const double angle = LC_RTOD * asin(clamped);
		StrideSpin->setValue(std::max(1.0, std::min(60.0, angle)));
	};

	auto UpdateDistFromStride = [&]()
	{
		const float Disp = LegLen * sinf(LC_DTOR * static_cast<float>(StrideSpin->value()));
		const double travel = 2.0 * Disp;
		DistSpin->setValue(travel / 20.0);
	};

	auto UpdateFromGait = [&]()
	{
		const int gait = GaitCombo->currentIndex();
		double stride;
		if (gait == 2) { stride = 45.0; }
		else if (gait == 1) { stride = 35.0; }
		else { stride = 25.0; }
		StrideSpin->setValue(stride);
	};

	auto UpdateWalkProjection = [&]()
	{
		const float DirRad = LC_DTOR * static_cast<float>(DirSpin->value());
		const lcVector3 ForwardAxis(-sinf(DirRad), -cosf(DirRad), 0.0f);
		const float Travel = 2.0f * LegLen * sinf(LC_DTOR * static_cast<float>(StrideSpin->value()));

		QMap<lcPiece*, lcVector3> GhostPos;
		QMap<lcPiece*, lcMatrix33> GhostRot;

		for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
		{
			lcGroup* G = Piece->GetGroup();

			if (G && G->mMinifigFamily == Family)
			{
				GhostPos[Piece.get()] = Piece->GetPosition() + ForwardAxis * Travel;
				GhostRot[Piece.get()] = Piece->GetRotation();
			}
		}

		ShowMinifigProjectionGhost(Model, GhostPos, GhostRot);
	};

	bool DistanceGuard = false;
	QObject::connect(GaitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [&](int) { UpdateFromGait(); UpdateWalkProjection(); });
	QObject::connect(StrideSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { if (!DistanceGuard) { DistanceGuard = true; UpdateDistFromStride(); DistanceGuard = false; } UpdateWalkProjection(); });
	QObject::connect(DistSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { if (!DistanceGuard) { DistanceGuard = true; UpdateStrideFromDist(); DistanceGuard = false; } UpdateWalkProjection(); });
	QObject::connect(DirSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { UpdateWalkProjection(); });
	QObject::connect(ArmSwingSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { UpdateWalkProjection(); });

	QDialogButtonBox* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
	Form->addRow(Buttons);
	QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
	QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);

	UpdateFromGait();
	UpdateDistFromStride();
	UpdateWalkProjection();
	if (!Dialog.exec())
	{
		if (lcView* V = gMainWindow->GetActiveView())
		{
			V->ClearGhost();
			if (lcViewWidget* W = V->GetWidget())
				W->ClearGhostImage();
		}
		return;
	}
	if (lcView* V = gMainWindow->GetActiveView())
	{
		V->ClearGhost();
		if (lcViewWidget* W = V->GetWidget())
			W->ClearGhostImage();
	}

	const int Steps = 24;
	const double StrideAngle = StrideSpin->value();
	const double ArmSwingAngle = ArmSwingSpin->value();
	const double DirectionDeg = DirSpin->value();
	const int GaitIndex = GaitCombo->currentIndex();

	// Reuses MinifigWizard's own angle-to-matrix math (the exact same code the Minifig Wizard's
	// angle sliders use) instead of re-deriving hip rotation math here - it's already correct for
	// every leg part in the catalog, including the non-obvious per-part pivot offsets.
	static MinifigWizard* Wizard = new MinifigWizard();

	// Helper: find the first piece in a group whose mPieceInfo matches a wizard slot's settings.
	auto FindPieceForSlot = [&](const std::vector<lcPiece*>& Pieces, int SlotType) -> lcPiece*
	{
		for (lcPiece* Piece : Pieces)
			for (const lcMinifigPieceInfo& Entry : Wizard->mSettings[SlotType])
				if (Entry.Info == Piece->mPieceInfo)
					return Piece;
		return nullptr;
	};

	lcPiece* RLegPiece = FindPieceForSlot(RightLegPieces, LC_MFW_RLEG);
	lcPiece* LLegPiece = FindPieceForSlot(LeftLegPieces, LC_MFW_LLEG);
	lcPiece* RArmPiece = FindPieceForSlot(RightArmPieces, LC_MFW_RARM);
	lcPiece* LArmPiece = FindPieceForSlot(LeftArmPieces, LC_MFW_LARM);
	lcPiece* RHandPiece = FindPieceForSlot(RightArmPieces, LC_MFW_RHAND);
	lcPiece* LHandPiece = FindPieceForSlot(LeftArmPieces, LC_MFW_LHAND);

	if (!RLegPiece) RLegPiece = RightLegPieces.front();
	if (!LLegPiece) LLegPiece = LeftLegPieces.front();

	Wizard->SetPieceInfo(LC_MFW_RLEG, RLegPiece->mPieceInfo);
	Wizard->SetAngle(LC_MFW_RLEG, 0.0f);
	const lcMatrix44 RLegNeutral = Wizard->mMinifig.Matrices[LC_MFW_RLEG];
	const lcMatrix44 RLegNeutralInv = lcMatrix44AffineInverse(RLegNeutral);

	Wizard->SetPieceInfo(LC_MFW_LLEG, LLegPiece->mPieceInfo);
	Wizard->SetAngle(LC_MFW_LLEG, 0.0f);
	const lcMatrix44 LLegNeutral = Wizard->mMinifig.Matrices[LC_MFW_LLEG];
	const lcMatrix44 LLegNeutralInv = lcMatrix44AffineInverse(LLegNeutral);

	lcMatrix44 RArmDelta = lcMatrix44Identity();
	lcMatrix44 LArmDelta = lcMatrix44Identity();
	lcMatrix44 RHandDelta = lcMatrix44Identity();
	lcMatrix44 LHandDelta = lcMatrix44Identity();
	lcMatrix44 RArmNeutralMat = lcMatrix44Identity();
	lcMatrix44 RArmNeutralInvMat = lcMatrix44Identity();
	lcMatrix44 LArmNeutralMat = lcMatrix44Identity();
	lcMatrix44 LArmNeutralInvMat = lcMatrix44Identity();
	lcMatrix44 RHandNeutralMat = lcMatrix44Identity();
	lcMatrix44 RHandNeutralInvMat = lcMatrix44Identity();
	lcMatrix44 LHandNeutralMat = lcMatrix44Identity();
	lcMatrix44 LHandNeutralInvMat = lcMatrix44Identity();

	if (RArmPiece)
	{
		Wizard->SetPieceInfo(LC_MFW_RARM, RArmPiece->mPieceInfo);
		if (RHandPiece)
			Wizard->SetPieceInfo(LC_MFW_RHAND, RHandPiece->mPieceInfo);
		Wizard->SetAngle(LC_MFW_RARM, 0.0f);
		RArmNeutralMat = Wizard->mMinifig.Matrices[LC_MFW_RARM];
		RArmNeutralInvMat = lcMatrix44AffineInverse(RArmNeutralMat);
		if (RHandPiece)
		{
			RHandNeutralMat = Wizard->mMinifig.Matrices[LC_MFW_RHAND];
			RHandNeutralInvMat = lcMatrix44AffineInverse(RHandNeutralMat);
		}
	}

	if (LArmPiece)
	{
		Wizard->SetPieceInfo(LC_MFW_LARM, LArmPiece->mPieceInfo);
		if (LHandPiece)
			Wizard->SetPieceInfo(LC_MFW_LHAND, LHandPiece->mPieceInfo);
		Wizard->SetAngle(LC_MFW_LARM, 0.0f);
		LArmNeutralMat = Wizard->mMinifig.Matrices[LC_MFW_LARM];
		LArmNeutralInvMat = lcMatrix44AffineInverse(LArmNeutralMat);
		if (LHandPiece)
		{
			LHandNeutralMat = Wizard->mMinifig.Matrices[LC_MFW_LHAND];
			LHandNeutralInvMat = lcMatrix44AffineInverse(LHandNeutralMat);
		}
	}

	// Build a quick lookup: which hand pieces (if any) live in each arm group.
	QHash<lcPiece*, bool> IsRHand, IsLHand;
	if (RHandPiece) IsRHand[RHandPiece] = true;
	if (LHandPiece) IsLHand[LHandPiece] = true;

	// Calculate step distance from the stride angle and the figure's leg geometry.
	const float HipOffset = 44.0f;
	const float TotalLegLength = RLegNeutral.r[3].z + HipOffset;
	const float ForwardDisplacement = TotalLegLength * sinf(LC_DTOR * static_cast<float>(StrideAngle));
	const float StepDistance = (2.0f * ForwardDisplacement) / static_cast<float>(Steps);

	struct lcStartPose { lcVector3 Position; lcMatrix33 Rotation; };
	QMap<lcPiece*, lcStartPose> StartPoses;

	for (lcPiece* Piece : RightLegPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : LeftLegPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : RightArmPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : LeftArmPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : OtherPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };

	// Forward direction based on user's angle: 0 = -Y (minifig's natural forward), rotates CCW as angle increases.
	const float DirRad = LC_DTOR * static_cast<float>(DirectionDeg);
	lcVector3 ForwardAxis(-sinf(DirRad), -cosf(DirRad), 0.0f);

	// Save the initial (neutral) state as a frame so deleting and re-generating always starts clean.
	const lcAnimateFrame NeutralFrame = SnapshotFrame(Model);

	// ponytail: build frames locally to avoid vector::insert inside RunInHistorySequence
	// (the insert was crashing because vector reallocation moves QMap-backed elements)
	std::vector<lcAnimateFrame> NewFrames;
	NewFrames.reserve(1 + Steps);

	NewFrames.push_back(NeutralFrame);

	for (int Step = 0; Step < Steps; Step++)
	{
		const float Phase = LC_2PI * static_cast<float>(Step) / static_cast<float>(Steps - 1);
		const float Wave = sinf(Phase);
		const float GaitWave = (GaitIndex == 2) ? sinf(Phase + 0.25f * Wave) :
			                   (GaitIndex == 1) ? 0.85f * Wave + 0.15f * sinf(Phase * 2.0f) :
			                   Wave;
		const float RightAngle = static_cast<float>(StrideAngle) * GaitWave;
		const float LeftAngle = -RightAngle;

		Wizard->SetAngle(LC_MFW_RLEG, RightAngle);
		const lcMatrix44 RDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_RLEG], RLegNeutralInv);

		Wizard->SetAngle(LC_MFW_LLEG, LeftAngle);
		const lcMatrix44 LDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_LLEG], LLegNeutralInv);

		if (RArmPiece)
		{
			Wizard->SetAngle(LC_MFW_RARM, -static_cast<float>(ArmSwingAngle) * GaitWave);
			RArmDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_RARM], RArmNeutralInvMat);
			if (RHandPiece)
				RHandDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_RHAND], RHandNeutralInvMat);
		}

		if (LArmPiece)
		{
			Wizard->SetAngle(LC_MFW_LARM, static_cast<float>(ArmSwingAngle) * GaitWave);
			LArmDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_LARM], LArmNeutralInvMat);
			if (LHandPiece)
				LHandDelta = lcMul(Wizard->mMinifig.Matrices[LC_MFW_LHAND], LHandNeutralInvMat);
		}

		const lcMatrix44 Forward = lcMatrix44Translation(ForwardAxis * static_cast<float>(StepDistance * (Step + 1)));

		for (lcPiece* Piece : RightLegPieces)
		{
			const lcStartPose& Start = StartPoses[Piece];
			const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
			const lcMatrix44 NewMatrix = lcMul(lcMul(RDelta, StartMatrix), Forward);
			Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
			Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
			Piece->UpdatePosition(1);
		}

		for (lcPiece* Piece : LeftLegPieces)
		{
			const lcStartPose& Start = StartPoses[Piece];
			const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
			const lcMatrix44 NewMatrix = lcMul(lcMul(LDelta, StartMatrix), Forward);
			Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
			Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
			Piece->UpdatePosition(1);
		}

		for (lcPiece* Piece : RightArmPieces)
		{
			const lcStartPose& Start = StartPoses[Piece];
			const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
			const lcMatrix44& Delta = IsRHand.contains(Piece) ? RHandDelta : RArmDelta;
			const lcMatrix44 NewMatrix = lcMul(lcMul(Delta, StartMatrix), Forward);
			Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
			Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
			Piece->UpdatePosition(1);
		}

		for (lcPiece* Piece : LeftArmPieces)
		{
			const lcStartPose& Start = StartPoses[Piece];
			const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
			const lcMatrix44& Delta = IsLHand.contains(Piece) ? LHandDelta : LArmDelta;
			const lcMatrix44 NewMatrix = lcMul(lcMul(Delta, StartMatrix), Forward);
			Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
			Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
			Piece->UpdatePosition(1);
		}

		for (lcPiece* Piece : OtherPieces)
		{
			const lcStartPose& Start = StartPoses[Piece];
			const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
			const lcMatrix44 NewMatrix = lcMul(StartMatrix, Forward);
			Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
			Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
			Piece->UpdatePosition(1);
		}

		NewFrames.push_back(SnapshotFrame(Model));
	}

	// No RunInHistorySequence wrapper here (unlike Attach to Hand / Mirror Pose): every piece this
	// loop moved gets set back to NeutralFrame by ApplyFrame(..., 0) below, so net piece state is
	// unchanged start-to-finish - there is nothing for LeoCAD's undo system to record. The generated
	// motion lives entirely in State.Frames/Keyframes, which (same as Capture/Duplicate/Delete
	// Frame) isn't undoable via Ctrl+Z yet.
	mSkipAutoKeyframe = true;

	lcAnimateDocumentState& State = GetState(Model);
	State.Frames = std::move(NewFrames);
	State.CurrentFrameIndex = 0;
	State.ThumbnailCache.clear();

	// In Constant Keyframe mode, create a keyframe at every frame so any later BakeKeyframes
	// preserves the full walk cycle (not just start→end lerp).
	RebuildKeyframesFromFrames(State);

	ApplyFrame(Model, GetState(Model).CurrentFrameIndex);
	mSkipAutoKeyframe = false;

	// Prevent auto-keyframe from detecting the just-applied walk cycle pose as "changed"
	// and calling BakeKeyframes (which would regen from keyframes and lose the motion).
	GetState(Model).LastAutoKeyframeDigest = SnapshotFrame(Model);

	mTimelineWidget->SetKeyframes(&GetState(Model).Keyframes);
	mTimelineWidget->SetFrameRange(0, std::max((int)GetState(Model).Frames.size(), 10));
	mTimelineWidget->SetCurrentTime(GetState(Model).CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::RagdollClicked()
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	int Flags = 0;
	std::vector<lcObject*> Selection;
	lcObject* Focus = nullptr;
	Model->GetSelectionInformation(&Flags, Selection, &Focus);

	lcGroup* Family = nullptr;

	for (lcObject* Object : Selection)
	{
		if (Object->IsPiece())
		{
			lcGroup* Group = ((lcPiece*)Object)->GetGroup();
			if (Group && Group->mMinifigFamily)
			{
				Family = Group->mMinifigFamily;
				break;
			}
		}
	}

	if (!Family)
	{
		QMessageBox::information(this, tr("Ragdoll Death"), tr("Select (or Alt+click) any part of a Posable minifig first."));
		return;
	}

	lcGroup* TorsoGroup = nullptr;
	lcGroup* HeadGroup = nullptr;
	lcGroup* RightLegGroup = nullptr;
	lcGroup* LeftLegGroup = nullptr;
	lcGroup* RightArmGroup = nullptr;
	lcGroup* LeftArmGroup = nullptr;

	for (const std::unique_ptr<lcGroup>& Candidate : Model->GetGroups())
	{
		if (Candidate->mMinifigFamily != Family)
			continue;

		if (Candidate->mName.contains(QLatin1String("Torso")))      TorsoGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Head")))  HeadGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Right Leg"))) RightLegGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Left Leg")))  LeftLegGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Right Arm"))) RightArmGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Left Arm")))  LeftArmGroup = Candidate.get();
	}

	std::vector<lcPiece*> TorsoPieces, HeadPieces, RightLegPieces, LeftLegPieces, RightArmPieces, LeftArmPieces, OtherPieces;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcGroup* Group = Piece->GetGroup();
		if (!Group || Group->mMinifigFamily != Family)
			continue;

		if (Group == TorsoGroup) TorsoPieces.push_back(Piece.get());
		else if (Group == HeadGroup) HeadPieces.push_back(Piece.get());
		else if (Group == RightLegGroup) RightLegPieces.push_back(Piece.get());
		else if (Group == LeftLegGroup) LeftLegPieces.push_back(Piece.get());
		else if (Group == RightArmGroup) RightArmPieces.push_back(Piece.get());
		else if (Group == LeftArmGroup) LeftArmPieces.push_back(Piece.get());
		else OtherPieces.push_back(Piece.get());
	}

	if (TorsoPieces.empty())
	{
		QMessageBox::information(this, tr("Ragdoll Death"), tr("This minifig doesn't have a Torso group."));
		return;
	}

	QDialog Dialog(this);
	Dialog.setWindowTitle(tr("Ragdoll Death"));
	QFormLayout* Form = new QFormLayout(&Dialog);

	QComboBox* PresetCombo = new QComboBox(&Dialog);
	PresetCombo->addItems({ tr("Punched"), tr("Got shot"), tr("Fell off cliff"), tr("Explosion") });
	Form->addRow(tr("Preset:"), PresetCombo);

	QDoubleSpinBox* DirSpin = new QDoubleSpinBox(&Dialog);
	DirSpin->setRange(0.0, 359.0);
	DirSpin->setValue(180.0);
	DirSpin->setSuffix(tr(" deg"));
	DirSpin->setToolTip(tr("Direction the minifig gets knocked back"));
	Form->addRow(tr("Direction:"), DirSpin);

	QDoubleSpinBox* KnockbackSpin = new QDoubleSpinBox(&Dialog);
	KnockbackSpin->setRange(0.0, 50.0);
	KnockbackSpin->setValue(2.0);
	KnockbackSpin->setSuffix(tr(" studs"));
	KnockbackSpin->setToolTip(tr("How hard the hit was - low values also calm down the limb flailing (see presets)"));
	Form->addRow(tr("Knockback:"), KnockbackSpin);

	QDoubleSpinBox* HeightSpin = new QDoubleSpinBox(&Dialog);
	HeightSpin->setRange(0.0, 30.0);
	HeightSpin->setValue(5.0);
	HeightSpin->setSuffix(tr(" studs"));
	Form->addRow(tr("Fall height:"), HeightSpin);

	QSpinBox* FramesSpin = new QSpinBox(&Dialog);
	FramesSpin->setRange(12, 48);
	FramesSpin->setValue(24);
	Form->addRow(tr("Frames:"), FramesSpin);

	auto UpdateRagdollProjection = [&]()
	{
		const float RDirRad = LC_DTOR * static_cast<float>(DirSpin->value());
		const lcVector3 RAxis(-sinf(RDirRad), -cosf(RDirRad), 0.0f);
		const float RKnockback = static_cast<float>(KnockbackSpin->value() * 20.0f);

		QMap<lcPiece*, lcVector3> GhostPos;
		QMap<lcPiece*, lcMatrix33> GhostRot;

		for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
		{
			lcGroup* G = Piece->GetGroup();
			if (G && G->mMinifigFamily == Family)
			{
				GhostPos[Piece.get()] = Piece->GetPosition() + RAxis * RKnockback;
				GhostRot[Piece.get()] = Piece->GetRotation();
			}
		}

		ShowMinifigProjectionGhost(Model, GhostPos, GhostRot);
	};

	auto ApplyPreset = [&]()
	{
		switch (PresetCombo->currentIndex())
		{
		case 0: DirSpin->setValue(180.0); KnockbackSpin->setValue(3.0);  HeightSpin->setValue(1.5);  FramesSpin->setValue(16); break; // Punched: stagger back a step, then fall
		case 1: DirSpin->setValue(180.0); KnockbackSpin->setValue(0.5); HeightSpin->setValue(1.0);  FramesSpin->setValue(18); break; // Got shot: legs buckle, mostly straight down
		case 2: DirSpin->setValue(180.0); KnockbackSpin->setValue(1.5); HeightSpin->setValue(20.0); FramesSpin->setValue(36); break; // Fell off cliff: height-dominant, little horizontal
		case 3: DirSpin->setValue(180.0); KnockbackSpin->setValue(20.0); HeightSpin->setValue(10.0); FramesSpin->setValue(30); break; // Explosion: full knockback + flailing
		}
		UpdateRagdollProjection();
	};
	QObject::connect(PresetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [&](int) { ApplyPreset(); });

	QDialogButtonBox* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog);
	Form->addRow(Buttons);
	QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
	QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);

	QObject::connect(DirSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { UpdateRagdollProjection(); });
	QObject::connect(KnockbackSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [&](double) { UpdateRagdollProjection(); });

	UpdateRagdollProjection();
	if (!Dialog.exec())
	{
		if (lcView* V = gMainWindow->GetActiveView())
			V->ClearGhost();
		return;
	}
	if (lcView* V = gMainWindow->GetActiveView())
		V->ClearGhost();

	const int TotalFrames = FramesSpin->value();
	const float DirRad = LC_DTOR * static_cast<float>(DirSpin->value());
	const float BaseKnockbackLDU = static_cast<float>(KnockbackSpin->value() * 20.0f);
	const float HeightLDU = static_cast<float>(HeightSpin->value() * 20.0f);
	const lcVector3 ForwardAxis(-sinf(DirRad), -cosf(DirRad), 0.0f);

	// How hard the hit was, derived from Knockback rather than a separate slider: a "Got shot, just
	// drops" death (small Knockback) gets a small, controlled buckle-and-collapse instead of the same
	// wild flailing as a big "Explosion" knockback. Floors are non-zero so even the calmest death
	// still has some natural settle instead of looking perfectly rigid.
	const float Intensity = qBound(0.2f, static_cast<float>(KnockbackSpin->value()) / 10.0f, 1.0f);
	const float TumbleScale = qBound(0.35f, Intensity, 1.0f);

	// per-run randomness so no two death anims are identical
	qsizetype Seed = static_cast<qsizetype>(QTime::currentTime().msecsSinceStartOfDay()) ^ reinterpret_cast<qsizetype>(Model);
	QRandomGenerator Rng(Seed);
	auto RF = [&](float lo, float hi) { return static_cast<float>(lo + Rng.generateDouble() * (hi - lo)); };

	const float DirJitter = RF(-10.0f, 10.0f);
	const lcVector3 JitterAxis(-sinf(DirRad + LC_DTOR * DirJitter), -cosf(DirRad + LC_DTOR * DirJitter), 0.0f);
	const float KnockbackScale = RF(0.75f, 1.25f);
	const float KnockbackLDU = BaseKnockbackLDU * KnockbackScale;
	const float HeightScale = RF(0.7f, 1.3f);
	const float ActualHeight = HeightLDU * HeightScale;

	struct lcStartPose { lcVector3 Position; lcMatrix33 Rotation; };
	QMap<lcPiece*, lcStartPose> StartPoses;

	auto SavePoses = [&](const std::vector<lcPiece*>& Pieces)
	{
		for (lcPiece* Piece : Pieces)
			StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	};
	SavePoses(TorsoPieces);  SavePoses(HeadPieces);
	SavePoses(RightLegPieces);  SavePoses(LeftLegPieces);
	SavePoses(RightArmPieces);  SavePoses(LeftArmPieces);
	SavePoses(OtherPieces);

	lcVector3 BodyCenter(0.0f, 0.0f, 0.0f);
	for (lcPiece* Piece : TorsoPieces)
		BodyCenter += StartPoses[Piece].Position;
	BodyCenter /= static_cast<float>(TorsoPieces.size());

	// Reuses MinifigWizard's own angle-to-matrix math for arm/leg/neck pivots (the same code Walk
	// Cycle and the Minifig Wizard itself use) so each limb swings from its actual joint instead of
	// spinning around the body's center of mass like a rigid flag - this is what makes a fall read
	// as an articulated ragdoll instead of one solid tumbling block with limbs pasted on.
	static MinifigWizard* RagdollWizard = new MinifigWizard();

	auto FindRagdollPieceForSlot = [&](const std::vector<lcPiece*>& Pieces, int SlotType) -> lcPiece*
	{
		for (lcPiece* Piece : Pieces)
			for (const lcMinifigPieceInfo& Entry : RagdollWizard->mSettings[SlotType])
				if (Entry.Info == Piece->mPieceInfo)
					return Piece;
		return Pieces.empty() ? nullptr : Pieces.front();
	};

	lcMatrix44 RArmNeutralInv = lcMatrix44Identity();
	lcMatrix44 LArmNeutralInv = lcMatrix44Identity();
	lcMatrix44 RLegNeutralInv = lcMatrix44Identity();
	lcMatrix44 LLegNeutralInv = lcMatrix44Identity();
	lcVector3 NeckPivot = BodyCenter;

	if (lcPiece* RArmPiece = FindRagdollPieceForSlot(RightArmPieces, LC_MFW_RARM))
	{
		RagdollWizard->SetPieceInfo(LC_MFW_RARM, RArmPiece->mPieceInfo);
		RagdollWizard->SetAngle(LC_MFW_RARM, 0.0f);
		RArmNeutralInv = lcMatrix44AffineInverse(RagdollWizard->mMinifig.Matrices[LC_MFW_RARM]);
	}
	if (lcPiece* LArmPiece = FindRagdollPieceForSlot(LeftArmPieces, LC_MFW_LARM))
	{
		RagdollWizard->SetPieceInfo(LC_MFW_LARM, LArmPiece->mPieceInfo);
		RagdollWizard->SetAngle(LC_MFW_LARM, 0.0f);
		LArmNeutralInv = lcMatrix44AffineInverse(RagdollWizard->mMinifig.Matrices[LC_MFW_LARM]);
	}
	if (lcPiece* RLegPiece = FindRagdollPieceForSlot(RightLegPieces, LC_MFW_RLEG))
	{
		RagdollWizard->SetPieceInfo(LC_MFW_RLEG, RLegPiece->mPieceInfo);
		RagdollWizard->SetAngle(LC_MFW_RLEG, 0.0f);
		RLegNeutralInv = lcMatrix44AffineInverse(RagdollWizard->mMinifig.Matrices[LC_MFW_RLEG]);
	}
	if (lcPiece* LLegPiece = FindRagdollPieceForSlot(LeftLegPieces, LC_MFW_LLEG))
	{
		RagdollWizard->SetPieceInfo(LC_MFW_LLEG, LLegPiece->mPieceInfo);
		RagdollWizard->SetAngle(LC_MFW_LLEG, 0.0f);
		LLegNeutralInv = lcMatrix44AffineInverse(RagdollWizard->mMinifig.Matrices[LC_MFW_LLEG]);
	}
	if (lcPiece* HeadPiece = FindRagdollPieceForSlot(HeadPieces, LC_MFW_HEAD))
	{
		RagdollWizard->SetPieceInfo(LC_MFW_HEAD, HeadPiece->mPieceInfo);
		RagdollWizard->SetAngle(LC_MFW_HEAD, 0.0f);
		NeckPivot = RagdollWizard->mMinifig.Matrices[LC_MFW_HEAD].GetTranslation();
	}

	// Per-piece jitter on top of the shared per-group scatter direction, so a multi-piece limb (e.g.
	// an arm group containing both the forearm and a hand piece) visibly separates piece-from-piece
	// at impact instead of moving as one perfectly rigid block - a cheap stand-in for true per-joint
	// unsocketing given groups only track minifig-family membership, not a joint graph.
	QMap<lcPiece*, lcVector3> PerPieceScatterJitter;
	auto AssignJitter = [&](const std::vector<lcPiece*>& Pieces)
	{
		for (lcPiece* Piece : Pieces)
			PerPieceScatterJitter[Piece] = lcVector3(RF(-1.0f, 1.0f), RF(-1.0f, 1.0f), RF(-1.0f, 1.0f)) * RF(2.0f, 8.0f);
	};
	AssignJitter(HeadPieces);
	AssignJitter(RightArmPieces);
	AssignJitter(LeftArmPieces);
	AssignJitter(RightLegPieces);
	AssignJitter(LeftLegPieces);

	// per-group scatter offsets triggered at impact (unsocketing)
	const lcVector3 RightArmScatter = lcVector3(1.0f, -0.3f, 0.3f) * RF(0.3f, 0.7f);
	const lcVector3 LeftArmScatter  = lcVector3(-1.0f, -0.3f, 0.3f) * RF(0.3f, 0.7f);
	const lcVector3 RightLegScatter = lcVector3(0.4f, 0.5f, -0.2f) * RF(0.2f, 0.5f);
	const lcVector3 LeftLegScatter  = lcVector3(-0.4f, 0.5f, -0.2f) * RF(0.2f, 0.5f);
	const lcVector3 HeadScatter     = lcVector3(RF(-0.2f, 0.2f), -0.5f, 0.5f) * RF(0.5f, 1.0f);
	const float ImpactFrame = static_cast<float>(0.55 + Rng.generateDouble() * 0.1); // impact timing varies

	const lcAnimateFrame NeutralFrame = SnapshotFrame(Model);
	std::vector<lcAnimateFrame> NewFrames;
	NewFrames.reserve(TotalFrames);
	NewFrames.push_back(NeutralFrame);

	for (int Frame = 0; Frame < TotalFrames; Frame++)
	{
		const float t = static_cast<float>(Frame) / static_cast<float>(TotalFrames - 1);

		// === Body position ===
		float horizT, vertT;
		if (t < 0.10f)                        { horizT = 0.0f;                         vertT = 0.0f; }
		else if (t < 0.40f)                   { horizT = (t - 0.10f) / 0.30f;          vertT = horizT * 0.5f; }
		else if (t < ImpactFrame)             { horizT = 1.0f;                         vertT = 0.5f + (t - 0.40f) / (ImpactFrame - 0.40f) * 0.5f; }
		else if (t < ImpactFrame + 0.05f)     { horizT = 1.0f;                         vertT = 1.0f - (t - ImpactFrame) / 0.05f * 0.3f; }
		else                                  { horizT = 1.0f;                         vertT = 0.7f - (t - ImpactFrame - 0.05f) / (1.0f - ImpactFrame - 0.05f) * 0.7f; }

		const float forwardProgress = (t < 0.40f) ? 1.0f - powf(1.0f - horizT, 3.0f) : horizT;
		const float heightLift = ActualHeight * sinf(vertT * LC_PI * 0.6f);

		// ground clamp: heightDrop floors at zero once the body reaches ground
		float heightDrop = 0.0f;
		if (t > ImpactFrame)
			heightDrop = (t - ImpactFrame) / (1.0f - ImpactFrame) * ActualHeight * 0.8f;
		const float GroundZ = std::max(0.0f, heightLift - heightDrop);

		lcVector3 BodyPos = JitterAxis * KnockbackLDU * forwardProgress;
		BodyPos.z += GroundZ;

		// bounce impulse on hit
		if (t > ImpactFrame && t < ImpactFrame + 0.08f)
		{
			const float bounceT = (t - ImpactFrame) / 0.08f;
			BodyPos.z += ActualHeight * 0.15f * sinf(bounceT * LC_PI);
		}

		// === Body rotation (tumble) ===
		const lcVector3 TumbleAxis(-ForwardAxis.y, ForwardAxis.x, 0.0f);
		float TumbleAngle;
		if (t < 0.10f)      TumbleAngle = RF(-5.0f, 5.0f);
		else if (t < 0.50f) TumbleAngle = (t - 0.10f) / 0.40f * 110.0f + RF(-10.0f, 10.0f);
		else if (t < ImpactFrame) TumbleAngle = 110.0f + (t - 0.50f) / (ImpactFrame - 0.50f) * 30.0f + RF(-10.0f, 10.0f);
		else if (t < ImpactFrame + 0.05f) TumbleAngle = 140.0f - (t - ImpactFrame) / 0.05f * 40.0f;
		else                TumbleAngle = 100.0f - (t - ImpactFrame - 0.05f) / (1.0f - ImpactFrame - 0.05f) * 10.0f;
		TumbleAngle *= TumbleScale;

		const lcMatrix33 BodyRot = lcQuaternionToMatrix33(lcQuaternionFromAxisAngle(lcVector4(TumbleAxis.x, TumbleAxis.y, TumbleAxis.z, LC_DTOR * TumbleAngle)));

		// === Per-group scatter offset (unsocketing) ===
		// Before impact, limbs follow body. At impact, they separate and fly on their own trajectory.
		float ScatterT = 0.0f;
		if (t > ImpactFrame)
			ScatterT = (t - ImpactFrame) / (1.0f - ImpactFrame);

		const float ScatterScale = ScatterT * ScatterT; // quadratic so limbs fling out fast then coast
		const lcVector3 RArmOffset = RightArmScatter * KnockbackLDU * ScatterScale;
		const lcVector3 LArmOffset  = LeftArmScatter * KnockbackLDU * ScatterScale;
		const lcVector3 RLegOffset  = RightLegScatter * KnockbackLDU * ScatterScale;
		const lcVector3 LLegOffset  = LeftLegScatter * KnockbackLDU * ScatterScale;
		const lcVector3 HeadOffset  = HeadScatter * KnockbackLDU * ScatterScale;

		// === Limb rotations ===
		// Peak angles and noise are scaled by Intensity so a calm "Got shot" death gets a controlled
		// buckle instead of the same full-windmill flailing as a big "Explosion" knockback; each limb
		// still gets per-run random noise (reduced frequency so it reads as a settle, not a vibration).
		const float armFlail = (t < 0.10f) ? 0.0f : Intensity * (
			(t < 0.50f) ? (t - 0.10f) / 0.40f * RF(80.0f, 100.0f) + sinf(t * LC_2PI * RF(0.8f, 1.5f) + RF(0.0f, LC_2PI)) * RF(5.0f, 15.0f) :
			(t < ImpactFrame + 0.05f) ? RF(90.0f, 110.0f) + sinf(t * LC_2PI * RF(1.0f, 2.0f) + RF(0.0f, LC_2PI)) * 8.0f :
			RF(70.0f, 100.0f) - ScatterT * RF(15.0f, 35.0f) + sinf(t * LC_2PI * RF(0.8f, 1.5f) + RF(0.0f, LC_2PI)) * 5.0f);

		const float legSplay = (t < 0.10f) ? 0.0f : Intensity * (
			(t < 0.40f) ? (t - 0.10f) / 0.30f * RF(25.0f, 45.0f) :
			(t < ImpactFrame) ? RF(30.0f, 50.0f) + (t - 0.40f) / (ImpactFrame - 0.40f) * RF(8.0f, 20.0f) + sinf(t * LC_2PI * RF(0.6f, 1.2f) + RF(0.0f, LC_2PI)) * 5.0f :
			RF(35.0f, 55.0f) + ScatterT * RF(12.0f, 25.0f));

		const float headLag = (t < 0.10f) ? 0.0f : Intensity * (
			(t < 0.50f) ? (t - 0.10f) / 0.40f * RF(35.0f, 55.0f) + sinf(t * LC_2PI * 2.0f + RF(0.0f, LC_2PI)) * 5.0f :
			(t < ImpactFrame + 0.05f) ? RF(40.0f, 60.0f) + (t - 0.50f) / (ImpactFrame + 0.05f - 0.50f) * RF(12.0f, 25.0f) :
			RF(45.0f, 65.0f) - ScatterT * RF(6.0f, 18.0f));

		// Joint deltas computed the same way Walk Cycle computes hip deltas: angle -> wizard matrix at
		// that angle -> composed against the neutral (angle 0) inverse. Applying this to a piece's
		// StartMatrix rotates/translates it about the joint's real pivot (shoulder/hip), not BodyCenter.
		RagdollWizard->SetAngle(LC_MFW_RARM, -armFlail);
		const lcMatrix44 RArmDelta = lcMul(RagdollWizard->mMinifig.Matrices[LC_MFW_RARM], RArmNeutralInv);
		RagdollWizard->SetAngle(LC_MFW_LARM, armFlail);
		const lcMatrix44 LArmDelta = lcMul(RagdollWizard->mMinifig.Matrices[LC_MFW_LARM], LArmNeutralInv);
		RagdollWizard->SetAngle(LC_MFW_RLEG, -legSplay);
		const lcMatrix44 RLegDelta = lcMul(RagdollWizard->mMinifig.Matrices[LC_MFW_RLEG], RLegNeutralInv);
		RagdollWizard->SetAngle(LC_MFW_LLEG, legSplay);
		const lcMatrix44 LLegDelta = lcMul(RagdollWizard->mMinifig.Matrices[LC_MFW_LLEG], LLegNeutralInv);

		// Head has no wizard-driven single-axis angle usable for a backward/sideways flop, so it
		// keeps a free axis-angle rotation - but pivots on NeckPivot (from the wizard) instead of
		// BodyCenter, so it hinges at the neck instead of swinging from the middle of the torso.
		const lcMatrix33 HeadFlop = lcQuaternionToMatrix33(lcQuaternionFromAxisAngle(lcVector4(TumbleAxis.x, TumbleAxis.y, TumbleAxis.z, LC_DTOR * -headLag)));
		const lcMatrix44 Identity44 = lcMatrix44Identity();

		// Composes a piece's already-articulated local pose (joint swing about its real pivot) with
		// the whole-body rigid transform (tumble about BodyCenter + BodyPos translation), then adds
		// the post-impact scatter offset (shared per-group direction plus a per-piece jitter so
		// multi-piece limbs visibly separate instead of drifting as one rigid block).
		auto ApplyToPieces = [&](const std::vector<lcPiece*>& Pieces, const lcMatrix44& JointDelta, const lcVector3& Offset)
		{
			for (lcPiece* Piece : Pieces)
			{
				const lcStartPose& Start = StartPoses[Piece];
				const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
				const lcMatrix44 Articulated = lcMul(JointDelta, StartMatrix);

				const lcVector3 RelPos = Articulated.GetTranslation() - BodyCenter;
				const lcVector3 PieceJitter = PerPieceScatterJitter.value(Piece) * ScatterScale * Intensity;
				const lcVector3 NewPos = BodyPos + Offset + PieceJitter + lcMul(RelPos, BodyRot) + BodyCenter;
				const lcMatrix33 NewRot = lcMul(lcMatrix33(Articulated), BodyRot);

				Piece->SetPosition(NewPos, 1, false);
				Piece->SetRotation(NewRot, 1, false);
				Piece->UpdatePosition(1);
			}
		};

		auto ApplyHeadPieces = [&]()
		{
			for (lcPiece* Piece : HeadPieces)
			{
				const lcStartPose& Start = StartPoses[Piece];
				const lcVector3 ArticulatedPos = NeckPivot + lcMul(Start.Position - NeckPivot, HeadFlop);
				const lcMatrix33 ArticulatedRot = lcMul(Start.Rotation, HeadFlop);

				const lcVector3 RelPos = ArticulatedPos - BodyCenter;
				const lcVector3 PieceJitter = PerPieceScatterJitter.value(Piece) * ScatterScale * Intensity;
				const lcVector3 NewPos = BodyPos + HeadOffset + PieceJitter + lcMul(RelPos, BodyRot) + BodyCenter;
				const lcMatrix33 NewRot = lcMul(ArticulatedRot, BodyRot);

				Piece->SetPosition(NewPos, 1, false);
				Piece->SetRotation(NewRot, 1, false);
				Piece->UpdatePosition(1);
			}
		};

		ApplyToPieces(TorsoPieces, Identity44, lcVector3(0.0f, 0.0f, 0.0f));
		ApplyToPieces(OtherPieces, Identity44, lcVector3(0.0f, 0.0f, 0.0f));
		ApplyHeadPieces();
		ApplyToPieces(RightArmPieces, RArmDelta, RArmOffset);
		ApplyToPieces(LeftArmPieces, LArmDelta, LArmOffset);
		ApplyToPieces(RightLegPieces, RLegDelta, RLegOffset);
		ApplyToPieces(LeftLegPieces, LLegDelta, LLegOffset);

		NewFrames.push_back(SnapshotFrame(Model));
	}

	// See the comment in WalkCycleClicked: pieces end up back at NeutralFrame (net piece state
	// unchanged), so there's nothing for LeoCAD's undo system to record - the generated motion lives
	// entirely in State.Frames/Keyframes, same not-undoable-via-Ctrl+Z category as Capture/
	// Duplicate/Delete Frame.
	mSkipAutoKeyframe = true;

	lcAnimateDocumentState& State = GetState(Model);
	State.Frames = std::move(NewFrames);
	State.CurrentFrameIndex = 0;
	State.ThumbnailCache.clear();

	RebuildKeyframesFromFrames(State);

	ApplyFrame(Model, GetState(Model).CurrentFrameIndex);
	mSkipAutoKeyframe = false;
	GetState(Model).LastAutoKeyframeDigest = SnapshotFrame(Model);

	mTimelineWidget->SetKeyframes(&GetState(Model).Keyframes);
	mTimelineWidget->SetFrameRange(0, std::max((int)GetState(Model).Frames.size(), 10));
	mTimelineWidget->SetCurrentTime(GetState(Model).CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::ModeChanged(int Index)
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	mSkipAutoKeyframe = true;

	lcAnimateDocumentState& State = GetState(Model);
	const lcAnimateMode NewMode = (Index == 0) ? lcAnimateMode::StopMotion : lcAnimateMode::ConstantKeyframe;

	if (NewMode == State.AnimateMode)
	{
		mSkipAutoKeyframe = false;
		return;
	}

	// Switching from Constant Keyframe → Stop Motion: bake sparse keyframes into full frame list.
	if (State.AnimateMode == lcAnimateMode::ConstantKeyframe && NewMode == lcAnimateMode::StopMotion)
	{
		BakeKeyframes(Model, State);
		RefreshFilmstrip(Model);
	}

	// Switching from Stop Motion → Constant Keyframe: if frames exist but no keyframes yet,
	// create keyframes at the first and last frame so the timeline has something to work with.
	if (State.AnimateMode == lcAnimateMode::StopMotion && NewMode == lcAnimateMode::ConstantKeyframe)
	{
		if (State.Keyframes.empty() && !State.Frames.empty())
		{
			lcKeyframePoint KfStart;
			KfStart.Time = 0;
			KfStart.Pose.Positions = State.Frames.front().Positions;
			KfStart.Pose.Rotations = State.Frames.front().Rotations;
			KfStart.Pose.CameraPosition = State.Frames.front().CameraPosition;
			KfStart.Pose.CameraTarget = State.Frames.front().CameraTarget;
			KfStart.Pose.CameraUpVector = State.Frames.front().CameraUpVector;
			KfStart.Pose.CameraProjection = State.Frames.front().CameraProjection;
			KfStart.Pose.HasCamera = State.Frames.front().HasCamera;
			State.Keyframes.push_back(KfStart);

			lcKeyframePoint KfEnd;
			KfEnd.Time = (int)State.Frames.size() - 1;
			KfEnd.Pose.Positions = State.Frames.back().Positions;
			KfEnd.Pose.Rotations = State.Frames.back().Rotations;
			KfEnd.Pose.CameraPosition = State.Frames.back().CameraPosition;
			KfEnd.Pose.CameraTarget = State.Frames.back().CameraTarget;
			KfEnd.Pose.CameraUpVector = State.Frames.back().CameraUpVector;
			KfEnd.Pose.CameraProjection = State.Frames.back().CameraProjection;
			KfEnd.Pose.HasCamera = State.Frames.back().HasCamera;
			State.Keyframes.push_back(KfEnd);

			BakeKeyframes(Model, State);
			RefreshFilmstrip(Model);
		}
	}

	State.AnimateMode = NewMode;

	// Capture button only makes sense in Stop Motion mode (Constant Keyframe uses the timeline).
	mCaptureButton->setVisible(NewMode == lcAnimateMode::StopMotion);
	mKeyframeControls->setVisible(NewMode == lcAnimateMode::ConstantKeyframe);

	// Clamp current frame index after any BakeKeyframes (which may have shrunk the frame range).
	if (State.CurrentFrameIndex >= (int)State.Frames.size())
		State.CurrentFrameIndex = std::max(0, (int)State.Frames.size() - 1);

	if (NewMode == lcAnimateMode::ConstantKeyframe)
	{
		mTimelineWidget->SetKeyframes(&State.Keyframes);
		mTimelineWidget->SetFrameRange(0, std::max((int)State.Frames.size(), 10));
		mTimelineWidget->SetCurrentTime(State.CurrentFrameIndex);
	}

	// Apply the current frame so the viewport is in sync after the mode switch.
	ApplyFrame(Model, State.CurrentFrameIndex);

	mSkipAutoKeyframe = false;
}

void lcAnimateWidget::BakeKeyframes(lcModel* Model, lcAnimateDocumentState& State)
{
	if (State.Keyframes.size() < 2)
	{
		State.Frames.clear();
		return;
	}

	std::sort(State.Keyframes.begin(), State.Keyframes.end(),
		[](const lcKeyframePoint& a, const lcKeyframePoint& b) { return a.Time < b.Time; });

	const int FirstTime = State.Keyframes.front().Time;
	const int LastTime = State.Keyframes.back().Time;

	State.Frames.resize(LastTime - FirstTime + 1);
	State.ThumbnailCache.clear();

	// Cursor into the (already time-sorted) Keyframes array. FrameTime only increases as i
	// increases, so the bracketing pair can only ever advance forward - re-scanning from the start
	// of Keyframes for every single output frame turned keyframe edits on long timelines into an
	// O(FrameCount * KeyframeCount) stall; this makes it O(FrameCount + KeyframeCount).
	size_t Cursor = 0;

	for (int i = 0; i < (int)State.Frames.size(); i++)
	{
		lcAnimateFrame& Frame = State.Frames[i];
		const int FrameTime = FirstTime + i;

		while (Cursor + 2 < State.Keyframes.size() && FrameTime > State.Keyframes[Cursor + 1].Time)
			Cursor++;

		// Find the two keyframes bracketing this frame
		const lcKeyframePoint* KfA = nullptr;
		const lcKeyframePoint* KfB = nullptr;

		if (FrameTime >= State.Keyframes[Cursor].Time && FrameTime <= State.Keyframes[Cursor + 1].Time)
		{
			KfA = &State.Keyframes[Cursor];
			KfB = &State.Keyframes[Cursor + 1];
		}

		if (!KfA || !KfB)
		{
			const lcKeyframePoint* Nearest = &State.Keyframes.front();
			for (const lcKeyframePoint& Kf : State.Keyframes)
				if (abs(Kf.Time - FrameTime) < abs(Nearest->Time - FrameTime))
					Nearest = &Kf;
			Frame.Positions = Nearest->Pose.Positions;
			Frame.Rotations = Nearest->Pose.Rotations;
			Frame.CameraPosition = Nearest->Pose.CameraPosition;
			Frame.CameraTarget = Nearest->Pose.CameraTarget;
			Frame.CameraUpVector = Nearest->Pose.CameraUpVector;
			Frame.HasCamera = Nearest->Pose.HasCamera;
			continue;
		}

		if (FrameTime == KfA->Time)
		{
			Frame.Positions = KfA->Pose.Positions;
			Frame.Rotations = KfA->Pose.Rotations;
			Frame.CameraPosition = KfA->Pose.CameraPosition;
			Frame.CameraTarget = KfA->Pose.CameraTarget;
			Frame.CameraUpVector = KfA->Pose.CameraUpVector;
			Frame.HasCamera = KfA->Pose.HasCamera;
			continue;
		}

		if (FrameTime == KfB->Time)
		{
			Frame.Positions = KfB->Pose.Positions;
			Frame.Rotations = KfB->Pose.Rotations;
			Frame.CameraPosition = KfB->Pose.CameraPosition;
			Frame.CameraTarget = KfB->Pose.CameraTarget;
			Frame.CameraUpVector = KfB->Pose.CameraUpVector;
			Frame.HasCamera = KfB->Pose.HasCamera;
			continue;
		}

		const float t = static_cast<float>(FrameTime - KfA->Time) / static_cast<float>(KfB->Time - KfA->Time);
		const float eased = ApplyEasing(KfA->SegmentEasing, t);

		// Interpolate positions
		Frame.Positions.clear();
		for (auto It = KfA->Pose.Positions.constBegin(); It != KfA->Pose.Positions.constEnd(); ++It)
		{
			if (KfB->Pose.Positions.contains(It.key()))
				Frame.Positions[It.key()] = lcLerp(It.value(), KfB->Pose.Positions.value(It.key()), eased);
			else
				Frame.Positions[It.key()] = It.value();
		}

		// Interpolate rotations via axis-angle decomposition
		// ponytail: axis-angle lerp is exact for single-axis rotations (handles all minifig poses)
		// and a reasonable approximation for multi-axis. Full quaternion SLERP if needed later.
		Frame.Rotations.clear();
		for (auto It = KfA->Pose.Rotations.constBegin(); It != KfA->Pose.Rotations.constEnd(); ++It)
		{
			if (!KfB->Pose.Rotations.contains(It.key()))
			{
				Frame.Rotations[It.key()] = It.value();
				continue;
			}

			// Delta = R_B * R_A^-1 = R_B * transpose(R_A)
			const lcMatrix33 RDelta = lcMul(KfB->Pose.Rotations.value(It.key()), lcMatrix33Transpose(It.value()));
			const lcVector4 QDelta = lcMatrix33ToQuaternion(RDelta);
			const lcVector4 AxisAngle = lcQuaternionToAxisAngle(QDelta);
			const float InterpAngle = AxisAngle[3] * eased;
			const lcVector4 InterpQ = lcQuaternionFromAxisAngle(lcVector4(AxisAngle[0], AxisAngle[1], AxisAngle[2], InterpAngle));
			const lcMatrix33 InterpRDelta = lcQuaternionToMatrix33(InterpQ);
			Frame.Rotations[It.key()] = lcMul(InterpRDelta, It.value());
		}

		// Interpolate camera
		if (KfA->Pose.HasCamera && KfB->Pose.HasCamera)
		{
			Frame.CameraPosition = lcLerp(KfA->Pose.CameraPosition, KfB->Pose.CameraPosition, eased);
			Frame.CameraTarget = lcLerp(KfA->Pose.CameraTarget, KfB->Pose.CameraTarget, eased);
			Frame.CameraUpVector = lcLerp(KfA->Pose.CameraUpVector, KfB->Pose.CameraUpVector, eased);
			Frame.CameraProjection = KfA->Pose.CameraProjection;
			Frame.HasCamera = true;
		}
		else if (KfA->Pose.HasCamera)
		{
			Frame.CameraPosition = KfA->Pose.CameraPosition;
			Frame.CameraTarget = KfA->Pose.CameraTarget;
			Frame.CameraUpVector = KfA->Pose.CameraUpVector;
			Frame.CameraProjection = KfA->Pose.CameraProjection;
			Frame.HasCamera = true;
		}
		else if (KfB->Pose.HasCamera)
		{
			Frame.CameraPosition = KfB->Pose.CameraPosition;
			Frame.CameraTarget = KfB->Pose.CameraTarget;
			Frame.CameraUpVector = KfB->Pose.CameraUpVector;
			Frame.CameraProjection = KfB->Pose.CameraProjection;
			Frame.HasCamera = true;
		}
	}

	// Keeps CurrentFrameIndex a valid index into the just-rebuilt (possibly shrunk) Frames vector
	// for every caller of BakeKeyframes, not just the ones that remember to clamp it themselves -
	// an out-of-range index here was reachable via Duplicate/Delete Frame indexing Frames[] directly.
	if (State.CurrentFrameIndex >= (int)State.Frames.size())
		State.CurrentFrameIndex = std::max(0, (int)State.Frames.size() - 1);
}

void lcAnimateWidget::AddKeyframeClicked()
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);

	lcKeyframePoint Pt;
	Pt.Time = mTimelineWidget->GetCurrentTime();
	Pt.Pose.Positions.clear();
	Pt.Pose.Rotations.clear();

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		Pt.Pose.Positions[Piece.get()] = Piece->GetPosition();
		Pt.Pose.Rotations[Piece.get()] = Piece->GetRotation();
	}

	if (lcView* View = gMainWindow->GetActiveView())
	{
		if (lcCamera* Camera = View->GetCamera())
		{
			Pt.Pose.CameraPosition = Camera->GetPosition();
			Pt.Pose.CameraTarget = Camera->GetTargetPosition();
			Pt.Pose.CameraUpVector = Camera->GetUpVector();
			Pt.Pose.CameraProjection = Camera->GetProjection();
			Pt.Pose.HasCamera = true;
		}
	}

	State.Keyframes.push_back(Pt);
	mTimelineWidget->SetKeyframes(&State.Keyframes);
	BakeKeyframes(Model, State);
	RefreshFilmstrip(Model);
	mTimelineWidget->SetFrameRange(0, std::max((int)State.Frames.size(), 10));
	mTimelineWidget->update();
	Update();
}

void lcAnimateWidget::DeleteKeyframeClicked()
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);
	const int Sel = mTimelineWidget->GetSelectedKeyframe();

	if (Sel < 0 || Sel >= (int)State.Keyframes.size())
		return;

	State.Keyframes.erase(State.Keyframes.begin() + Sel);
	mTimelineWidget->SetKeyframes(&State.Keyframes);

	if (State.Keyframes.size() < 2)
	{
		State.Frames.clear();
		State.ThumbnailCache.clear();
		State.CurrentFrameIndex = 0;
	}
	else
	{
		BakeKeyframes(Model, State);
	}

	RefreshFilmstrip(Model);
	mTimelineWidget->update();
	Update();
}

void lcAnimateWidget::ClearKeyframeClicked()
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);
	State.Keyframes.clear();
	State.Frames.clear();
	State.ThumbnailCache.clear();
	State.CurrentFrameIndex = 0;
	mTimelineWidget->SetKeyframes(&State.Keyframes);
	RefreshFilmstrip(Model);
	mTimelineWidget->update();
	Update();
}

void lcAnimateWidget::EasingChanged(int Index)
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);
	const int Seg = mTimelineWidget->GetSelectedSegment();

	if (Seg < 0 || Seg >= (int)State.Keyframes.size() - 1)
		return;

	State.Keyframes[Seg].SegmentEasing = static_cast<lcEasingType>(qBound(0, Index, 3));
	BakeKeyframes(Model, State);
	RefreshFilmstrip(Model);
	mTimelineWidget->update();
	Update();
}

void lcAnimateWidget::TimelineKeyframeSelected(int Index)
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);

	if (Index >= 0 && Index < (int)State.Keyframes.size())
	{
		State.CurrentFrameIndex = State.Keyframes[Index].Time;
		mEasingCombo->setCurrentIndex(static_cast<int>(State.Keyframes[Index].SegmentEasing));
		ApplyFrame(Model, State.CurrentFrameIndex);
		mTimelineWidget->SetCurrentTime(State.CurrentFrameIndex);
		Update();
	}
}

void lcAnimateWidget::TimelineTimeDragged(int Frame)
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);
	State.CurrentFrameIndex = qBound(0, Frame, std::max(0, (int)State.Frames.size() - 1));
	ApplyFrame(Model, State.CurrentFrameIndex);
	Update();
}

void lcAnimateWidget::TimelineStep(int Delta)
{
	lcModel* Model = lcGetActiveModel();
	if (!Model)
		return;

	lcAnimateDocumentState& State = GetState(Model);
	const int NewFrame = qBound(0, State.CurrentFrameIndex + Delta, std::max(0, (int)State.Frames.size() - 1));
	if (NewFrame != State.CurrentFrameIndex)
	{
		State.CurrentFrameIndex = NewFrame;
		ApplyFrame(Model, State.CurrentFrameIndex);
		mTimelineWidget->SetCurrentTime(State.CurrentFrameIndex);
		Update();
	}
}

void lcAnimateWidget::ExportClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	// Playback fighting the export loop for control of the same live model would both flicker the
	// viewport and leave the frame index in the wrong place once the export dialog closes.
	if (mPlayTimer->isActive())
		PlayPauseClicked();

	lcAnimateDocumentState& State = GetState(Model);

	// Constant Keyframe mode can have zero baked Frames (e.g. right after Clear/Delete Keyframe
	// drops below 2 keyframes). Without this guard, the export dialog's Start/End spin boxes
	// degenerate to an empty [1,0] range that Qt collapses to a bogus value, and the render loop
	// indexes the (empty) frame vector out of bounds.
	if (State.Frames.empty())
	{
		QMessageBox::information(this, tr("Export Animation"), tr("There's nothing to export yet - capture a frame (Stop Motion) or add at least two keyframes (Constant Keyframe) first."));
		return;
	}

	lcAnimateExportDialog Dialog(this, Model, mFpsSpinBox->value(), State.Frames);
	Dialog.exec();

	// Exporting poses pieces as each exported frame; make sure the viewport reflects the frame
	// we're actually parked on once the dialog closes.
	ApplyFrame(Model, State.CurrentFrameIndex);
}

void lcAnimateWidget::SaveAnimationData(Project* CurrentProject, const QString& FileName)
{
	if (FileName.isEmpty() || !CurrentProject)
		return;

	QJsonObject ModelsObject;
	bool AnyData = false;

	for (const std::unique_ptr<lcModel>& ModelPtr : CurrentProject->GetModels())
	{
		lcModel* Model = ModelPtr.get();
		const auto It = mDocumentStates.constFind(Model);

		// Nothing captured beyond the implicit initial frame - nothing worth saving for this model.
		if (It == mDocumentStates.constEnd() || It.value().Frames.size() <= 1)
			continue;

		// Piece identity doesn't survive a save/load round trip (pieces are recreated from
		// scratch), but the ORDER LDraw pieces are saved/loaded in is stable, so index-in-the-
		// piece-list is used as the on-disk identifier and re-resolved back to real lcPiece*
		// pointers on load.
		QMap<lcPiece*, int> PieceIndices;
		const std::vector<std::unique_ptr<lcPiece>>& Pieces = Model->GetPieces();

		for (size_t Index = 0; Index < Pieces.size(); Index++)
			PieceIndices[Pieces[Index].get()] = static_cast<int>(Index);

		QJsonArray FramesArray;

		for (const lcAnimateFrame& Frame : It.value().Frames)
		{
			QJsonArray PiecesArray;

			for (auto PosIt = Frame.Positions.constBegin(); PosIt != Frame.Positions.constEnd(); ++PosIt)
			{
				const auto IndexIt = PieceIndices.constFind(PosIt.key());

				if (IndexIt == PieceIndices.constEnd())
					continue; // piece no longer exists in the model (deleted since it was captured)

				QJsonObject PieceObject;
				PieceObject[QLatin1String("index")] = IndexIt.value();
				PieceObject[QLatin1String("position")] = Vector3ToJson(PosIt.value());
				PieceObject[QLatin1String("rotation")] = Matrix33ToJson(Frame.Rotations.value(PosIt.key()));

				PiecesArray.append(PieceObject);
			}

			QJsonObject FrameObject;
			FrameObject[QLatin1String("pieces")] = PiecesArray;

			if (Frame.HasCamera)
			{
				QJsonObject CameraObject;
				CameraObject[QLatin1String("position")] = Vector3ToJson(Frame.CameraPosition);
				CameraObject[QLatin1String("target")] = Vector3ToJson(Frame.CameraTarget);
				CameraObject[QLatin1String("up")] = Vector3ToJson(Frame.CameraUpVector);
				CameraObject[QLatin1String("projection")] = static_cast<int>(Frame.CameraProjection);
				FrameObject[QLatin1String("camera")] = CameraObject;
			}

			FramesArray.append(FrameObject);
		}

		QJsonObject ModelObject;
		ModelObject[QLatin1String("frames")] = FramesArray;
		ModelsObject[Model->GetProperties().mFileName] = ModelObject;
		AnyData = true;
	}

	const QString AnimateFileName = FileName + QLatin1String(".animate.json");

	if (!AnyData)
	{
		QFile::remove(AnimateFileName); // nothing to save - clean up a stale companion file if any
		return;
	}

	QJsonObject Root;
	Root[QLatin1String("models")] = ModelsObject;

	QFile File(AnimateFileName);

	if (File.open(QIODevice::WriteOnly))
		File.write(QJsonDocument(Root).toJson(QJsonDocument::Compact));
}

void lcAnimateWidget::LoadAnimationData(Project* CurrentProject, const QString& FileName)
{
	if (FileName.isEmpty() || !CurrentProject)
		return;

	QFile File(FileName + QLatin1String(".animate.json"));

	if (!File.open(QIODevice::ReadOnly))
		return;

	const QJsonDocument Document = QJsonDocument::fromJson(File.readAll());

	if (!Document.isObject())
		return;

	const QJsonObject ModelsObject = Document.object().value(QLatin1String("models")).toObject();

	for (const std::unique_ptr<lcModel>& ModelPtr : CurrentProject->GetModels())
	{
		lcModel* Model = ModelPtr.get();
		const QString Key = Model->GetProperties().mFileName;

		if (!ModelsObject.contains(Key))
			continue;

		const QJsonArray FramesArray = ModelsObject.value(Key).toObject().value(QLatin1String("frames")).toArray();

		if (FramesArray.isEmpty())
			continue;

		const std::vector<std::unique_ptr<lcPiece>>& Pieces = Model->GetPieces();
		lcAnimateDocumentState& State = GetState(Model); // ensures a default state exists first

		State.Frames.clear();
		State.ThumbnailCache.clear();
		State.AnimateForcedHidden.clear();
		State.CurrentFrameIndex = 0;

		for (const QJsonValue& FrameValue : FramesArray)
		{
			const QJsonObject FrameObject = FrameValue.toObject();
			lcAnimateFrame Frame;

			for (const QJsonValue& PieceValue : FrameObject.value(QLatin1String("pieces")).toArray())
			{
				const QJsonObject PieceObject = PieceValue.toObject();
				const int Index = PieceObject.value(QLatin1String("index")).toInt(-1);

				if (Index < 0 || static_cast<size_t>(Index) >= Pieces.size())
					continue; // piece no longer exists (model edited outside the saved animation)

				lcPiece* Piece = Pieces[Index].get();
				Frame.Positions[Piece] = Vector3FromJson(PieceObject.value(QLatin1String("position")).toArray());
				Frame.Rotations[Piece] = Matrix33FromJson(PieceObject.value(QLatin1String("rotation")).toArray());
			}

			if (FrameObject.contains(QLatin1String("camera")))
			{
				const QJsonObject CameraObject = FrameObject.value(QLatin1String("camera")).toObject();
				Frame.CameraPosition = Vector3FromJson(CameraObject.value(QLatin1String("position")).toArray());
				Frame.CameraTarget = Vector3FromJson(CameraObject.value(QLatin1String("target")).toArray());
				Frame.CameraUpVector = Vector3FromJson(CameraObject.value(QLatin1String("up")).toArray());
				Frame.HasCamera = true;
				if (CameraObject.contains(QLatin1String("projection")))
					Frame.CameraProjection = static_cast<lcCameraProjection>(CameraObject.value(QLatin1String("projection")).toInt());
			}

			State.Frames.push_back(Frame);
		}

		if (State.Frames.empty())
			State.Frames.push_back(SnapshotFrame(Model));
	}
}
