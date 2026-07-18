#include "lc_global.h"
#include "lc_animatewidget.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_mainwindow.h"
#include "lc_view.h"
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

	ActionLayout->addStretch();
	mExportButton = new QPushButton(tr("Export Animation..."), this);
	ActionLayout->addWidget(mExportButton);
	MainLayout->addLayout(ActionLayout);

	// Row 3: frame filmstrip
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
	connect(mPlayButton, &QPushButton::clicked, this, &lcAnimateWidget::PlayPauseClicked);
	connect(mOnionSkinCheck, &QCheckBox::toggled, this, &lcAnimateWidget::OnionSkinToggled);
	connect(mExportButton, &QPushButton::clicked, this, &lcAnimateWidget::ExportClicked);
	connect(mFilmstrip, &QListWidget::currentRowChanged, this, &lcAnimateWidget::FilmstripItemChanged);
	connect(mSocketModeCheck, &QCheckBox::toggled, this, &lcAnimateWidget::SocketModeToggled);

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
			Frame.HasCamera = true;
		}
	}

	return Frame;
}

void lcAnimateWidget::ApplyFrame(lcModel* Model, int FrameIndex)
{
	lcAnimateDocumentState& State = GetState(Model);

	if (FrameIndex < 0 || FrameIndex >= static_cast<int>(State.Frames.size()))
		return;

	lcPoseAnimateFrame(Model, State.Frames[FrameIndex], State.AnimateForcedHidden);
	Model->SetCurrentStep(1); // redraws the live viewport and refreshes the rest of the UI
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

	// Snapshot whatever is actually live on screen right now - which may be an uncaptured edit
	// that doesn't match State.Frames[State.CurrentFrameIndex] yet - before any temporary
	// re-posing below, so we restore the real live state and never silently discard an
	// in-progress move.
	lcAnimateFrame LiveState;
	bool NeedsViewportRestore = false;

	for (int Index = 0; Index < static_cast<int>(State.Frames.size()); Index++)
	{
		QIcon Icon = State.ThumbnailCache.value(Index);

		if (Icon.isNull())
		{
			if (!NeedsViewportRestore)
				LiveState = SnapshotFrame(Model);

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
	if (!mOnionSkinCheck->isChecked())
	{
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("Onion skin off"));
		return;
	}

	lcAnimateDocumentState& State = GetState(Model);

	if (State.CurrentFrameIndex <= 0)
	{
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("No previous frame"));
		return;
	}

	const int PreviousIndex = State.CurrentFrameIndex - 1;
	QIcon Icon = State.ThumbnailCache.value(PreviousIndex);

	if (Icon.isNull())
	{
		// Same rule as RefreshFilmstrip: preserve whatever is actually live on screen (which may
		// be an uncaptured edit), don't snap back to the stored current-frame data.
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

void lcAnimateWidget::OnionSkinToggled(bool)
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

	Model->RunInHistorySequence(tr("Mirror Pose"), [&]()
	{
		TargetPiece->SetRotation(SourcePiece->GetRotation(), 1, false);
		TargetPiece->UpdatePosition(1);
	});
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

	for (const std::unique_ptr<lcGroup>& Candidate : Model->GetGroups())
	{
		if (Candidate->mMinifigFamily != Family)
			continue;

		if (Candidate->mName.contains(QLatin1String("Right Leg")))
			RightLegGroup = Candidate.get();
		else if (Candidate->mName.contains(QLatin1String("Left Leg")))
			LeftLegGroup = Candidate.get();
	}

	if (!RightLegGroup || !LeftLegGroup)
	{
		QMessageBox::information(this, tr("Walk Cycle"), tr("This minifig doesn't have both a Right Leg and a Left Leg group to alternate."));
		return;
	}

	std::vector<lcPiece*> RightLegPieces, LeftLegPieces, OtherPieces;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcGroup* Group = Piece->GetGroup();

		if (!Group || Group->mMinifigFamily != Family)
			continue;

		if (Group == RightLegGroup)
			RightLegPieces.push_back(Piece.get());
		else if (Group == LeftLegGroup)
			LeftLegPieces.push_back(Piece.get());
		else
			OtherPieces.push_back(Piece.get());
	}

	bool Ok = false;
	const int Steps = QInputDialog::getInt(this, tr("Walk Cycle"), tr("Number of steps (each step is one captured frame):"), 8, 2, 64, 1, &Ok);
	if (!Ok)
		return;

	const double StrideAngle = QInputDialog::getDouble(this, tr("Walk Cycle"), tr("Stride angle (degrees each leg swings forward/back from the current pose):"), 25.0, 1.0, 60.0, 1, &Ok);
	if (!Ok)
		return;

	const double StepDistance = QInputDialog::getDouble(this, tr("Walk Cycle"), tr("Distance to move forward per step (LDraw units - use a negative number if the figure ends up walking backward):"), 20.0, -500.0, 500.0, 1, &Ok);
	if (!Ok)
		return;

	// Reuses MinifigWizard's own angle-to-matrix math (the exact same code the Minifig Wizard's
	// angle sliders use) instead of re-deriving hip rotation math here - it's already correct for
	// every leg part in the catalog, including the non-obvious per-part pivot offsets.
	static MinifigWizard* Wizard = new MinifigWizard();

	Wizard->SetPieceInfo(LC_MFW_RLEG, RightLegPieces.front()->mPieceInfo);
	Wizard->SetAngle(LC_MFW_RLEG, 0.0f);
	const lcMatrix44 RLegNeutral = Wizard->mMinifig.Matrices[LC_MFW_RLEG];
	Wizard->SetAngle(LC_MFW_RLEG, static_cast<float>(StrideAngle));
	const lcMatrix44 RLegForward = Wizard->mMinifig.Matrices[LC_MFW_RLEG];
	Wizard->SetAngle(LC_MFW_RLEG, static_cast<float>(-StrideAngle));
	const lcMatrix44 RLegBack = Wizard->mMinifig.Matrices[LC_MFW_RLEG];

	Wizard->SetPieceInfo(LC_MFW_LLEG, LeftLegPieces.front()->mPieceInfo);
	Wizard->SetAngle(LC_MFW_LLEG, 0.0f);
	const lcMatrix44 LLegNeutral = Wizard->mMinifig.Matrices[LC_MFW_LLEG];
	Wizard->SetAngle(LC_MFW_LLEG, static_cast<float>(StrideAngle));
	const lcMatrix44 LLegForward = Wizard->mMinifig.Matrices[LC_MFW_LLEG];
	Wizard->SetAngle(LC_MFW_LLEG, static_cast<float>(-StrideAngle));
	const lcMatrix44 LLegBack = Wizard->mMinifig.Matrices[LC_MFW_LLEG];

	// A pure "how did the leg move" delta, independent of where in the scene the minifig actually
	// is (the wizard always computes matrices relative to its own fixed origin) - applying this
	// delta on top of each piece's CURRENT (start-of-cycle) world matrix reproduces the same swing
	// wherever the figure has actually been placed/moved to.
	const lcMatrix44 RLegNeutralInv = lcMatrix44AffineInverse(RLegNeutral);
	const lcMatrix44 LLegNeutralInv = lcMatrix44AffineInverse(LLegNeutral);
	const lcMatrix44 RLegForwardDelta = lcMul(RLegForward, RLegNeutralInv);
	const lcMatrix44 RLegBackDelta = lcMul(RLegBack, RLegNeutralInv);
	const lcMatrix44 LLegForwardDelta = lcMul(LLegForward, LLegNeutralInv);
	const lcMatrix44 LLegBackDelta = lcMul(LLegBack, LLegNeutralInv);

	struct lcStartPose { lcVector3 Position; lcMatrix33 Rotation; };
	QMap<lcPiece*, lcStartPose> StartPoses;

	for (lcPiece* Piece : RightLegPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : LeftLegPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };
	for (lcPiece* Piece : OtherPieces)
		StartPoses[Piece] = { Piece->GetPosition(), Piece->GetRotation() };

	// Walking forward is along the model's +Y axis, matching the leg hip swing axis convention used
	// throughout minifig.cpp (RotationX for the hip means the foot travels along Y) - if a figure
	// ends up walking backward, use a negative Step Distance above rather than changing this.
	const lcVector3 ForwardAxis(0.0f, 1.0f, 0.0f);

	Model->RunInHistorySequence(tr("Walk Cycle"), [&]()
	{
		lcAnimateDocumentState& State = GetState(Model);
		int InsertIndex = State.CurrentFrameIndex;

		for (int Step = 0; Step < Steps; Step++)
		{
			const bool RightForward = (Step % 2) == 0;
			const lcMatrix44& RDelta = RightForward ? RLegForwardDelta : RLegBackDelta;
			const lcMatrix44& LDelta = RightForward ? LLegBackDelta : LLegForwardDelta;
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

			for (lcPiece* Piece : OtherPieces)
			{
				const lcStartPose& Start = StartPoses[Piece];
				const lcMatrix44 StartMatrix(Start.Rotation, Start.Position);
				const lcMatrix44 NewMatrix = lcMul(StartMatrix, Forward);

				Piece->SetPosition(NewMatrix.GetTranslation(), 1, false);
				Piece->SetRotation(lcMatrix33(NewMatrix), 1, false);
				Piece->UpdatePosition(1);
			}

			InsertIndex++;
			State.Frames.insert(State.Frames.begin() + InsertIndex, SnapshotFrame(Model));
			ThumbnailCacheOnInsert(State.ThumbnailCache, InsertIndex);
			State.CurrentFrameIndex = InsertIndex;
		}
	});

	ApplyFrame(Model, GetState(Model).CurrentFrameIndex);
	Update();
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
			}

			State.Frames.push_back(Frame);
		}

		if (State.Frames.empty())
			State.Frames.push_back(SnapshotFrame(Model));
	}
}
