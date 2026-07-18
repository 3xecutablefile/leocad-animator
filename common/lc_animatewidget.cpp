#include "lc_global.h"
#include "lc_animatewidget.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_mainwindow.h"
#include "lc_view.h"
#include "project.h"
#include "object.h"
#include "piece.h"
#include "minifig.h"

static const int THUMBNAIL_WIDTH = 96;
static const int THUMBNAIL_HEIGHT = 72;

void lcPoseAnimateFrame(lcModel* Model, const lcAnimateFrame& Frame)
{
	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		lcPiece* PiecePtr = Piece.get();

		// A piece not present in this frame's snapshot didn't exist yet when the frame was
		// captured (it was added later) - hide it here instead of leaving it visible at whatever
		// position it happens to currently be in. mHidden is independent of LeoCAD's Step show/hide
		// range, which we don't use (see lcAnimateFrame comment), so this doesn't fight anything.
		const bool ExistsInFrame = Frame.Positions.contains(PiecePtr);

		Piece->SetHidden(!ExistsInFrame);

		if (ExistsInFrame)
		{
			Piece->SetPosition(Frame.Positions.value(PiecePtr), 1, false);
			Piece->SetRotation(Frame.Rotations.value(PiecePtr), 1, false);
		}

		Piece->UpdatePosition(1);
	}
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
	mSocketModeCheck->setToolTip(tr("On: limbs can only rotate, so arms/legs stay attached to their sockets while posing. Off (Free Move): pieces can be dragged and detached, e.g. to pull an arm out of its socket for one frame."));
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
	mCaptureButton->setToolTip(tr("Snapshot every piece's position and rotation into a new frame, like pressing the shutter on a stop-motion camera"));
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
	connect(mPlayButton, &QPushButton::clicked, this, &lcAnimateWidget::PlayPauseClicked);
	connect(mOnionSkinCheck, &QCheckBox::toggled, this, &lcAnimateWidget::OnionSkinToggled);
	connect(mExportButton, &QPushButton::clicked, this, &lcAnimateWidget::ExportClicked);
	connect(mFilmstrip, &QListWidget::currentRowChanged, this, &lcAnimateWidget::FilmstripItemChanged);
	connect(mSocketModeCheck, &QCheckBox::toggled, this, &lcAnimateWidget::SocketModeToggled);

	SocketModeToggled(mSocketModeCheck->isChecked());
}

void lcAnimateWidget::EnsureInitialized(lcModel* Model)
{
	// ponytail: one animation history per dock, not per document - switching to a different open
	// model tab resets it. Fine for the single-document workflow this is built for; multi-document
	// support would need frames stored on the model itself, not here.
	if (mInitialized && mLastModel == Model)
		return;

	mFrames.clear();
	mFrames.push_back(SnapshotFrame(Model));
	mCurrentFrameIndex = 0;
	mThumbnailCache.clear();
	mLastModel = Model;
	mInitialized = true;
}

lcAnimateFrame lcAnimateWidget::SnapshotFrame(lcModel* Model) const
{
	lcAnimateFrame Frame;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
	{
		// Skip pieces currently hidden by lcPoseAnimateFrame (i.e. not part of whatever frame is
		// actually posed right now). Without this, RefreshFilmstrip/RefreshOnionSkin's "remember
		// the live state, render some other frame's thumbnails, then restore it" dance would
		// snapshot every piece unconditionally and restore by un-hiding all of them - silently
		// undoing the hide that just made a later-added piece correctly disappear from earlier
		// frames.
		if (Piece->IsHidden())
			continue;

		Frame.Positions[Piece.get()] = Piece->GetPosition();
		Frame.Rotations[Piece.get()] = Piece->GetRotation();
	}

	return Frame;
}

void lcAnimateWidget::ApplyFrame(lcModel* Model, int FrameIndex)
{
	if (FrameIndex < 0 || FrameIndex >= static_cast<int>(mFrames.size()))
		return;

	lcPoseAnimateFrame(Model, mFrames[FrameIndex]);
	Model->SetCurrentStep(1); // redraws the live viewport and refreshes the rest of the UI
}

QIcon lcAnimateWidget::RenderFrameThumbnail(lcModel* Model, int FrameIndex, int Width, int Height)
{
	if (FrameIndex < 0 || FrameIndex >= static_cast<int>(mFrames.size()))
		return QIcon();

	// Poses pieces directly without going through SetCurrentStep, so this doesn't touch the live
	// viewport - the caller is responsible for restoring the real current frame afterward.
	lcPoseAnimateFrame(Model, mFrames[FrameIndex]);

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
	mIgnoreUpdates = true;
	mFilmstrip->clear();

	// Snapshot whatever is actually live on screen right now - which may be an uncaptured edit
	// that doesn't match mFrames[mCurrentFrameIndex] yet - before any temporary re-posing below,
	// so we restore the real live state and never silently discard an in-progress move.
	lcAnimateFrame LiveState;
	bool NeedsViewportRestore = false;

	for (int Index = 0; Index < static_cast<int>(mFrames.size()); Index++)
	{
		QIcon Icon = mThumbnailCache.value(Index);

		if (Icon.isNull())
		{
			if (!NeedsViewportRestore)
				LiveState = SnapshotFrame(Model);

			Icon = RenderFrameThumbnail(Model, Index, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
			mThumbnailCache.insert(Index, Icon);
			NeedsViewportRestore = true;
		}

		mFilmstrip->addItem(new QListWidgetItem(Icon, QString::number(Index + 1)));
	}

	// Rendering thumbnails above may have posed pieces as other frames - restore the real live
	// state once at the end instead of after every single thumbnail (avoids viewport flicker too).
	if (NeedsViewportRestore)
	{
		lcPoseAnimateFrame(Model, LiveState);
		Model->SetCurrentStep(1);
	}

	mFilmstrip->setCurrentRow(mCurrentFrameIndex);
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

	if (mCurrentFrameIndex <= 0)
	{
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("No previous frame"));
		return;
	}

	const int PreviousIndex = mCurrentFrameIndex - 1;
	QIcon Icon = mThumbnailCache.value(PreviousIndex);

	if (Icon.isNull())
	{
		// Same rule as RefreshFilmstrip: preserve whatever is actually live on screen (which may
		// be an uncaptured edit), don't snap back to the stored current-frame data.
		const lcAnimateFrame LiveState = SnapshotFrame(Model);

		Icon = RenderFrameThumbnail(Model, PreviousIndex, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
		mThumbnailCache.insert(PreviousIndex, Icon);

		lcPoseAnimateFrame(Model, LiveState);
		Model->SetCurrentStep(1);
	}

	mOnionSkinPreview->setText(QString());
	mOnionSkinPreview->setPixmap(Icon.pixmap(120, 90));
}

void lcAnimateWidget::Update()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	EnsureInitialized(Model);

	const int FrameCount = static_cast<int>(mFrames.size());

	mFrameLabel->setText(tr("Frame %1 / %2").arg(mCurrentFrameIndex + 1).arg(FrameCount));
	mDeleteButton->setEnabled(FrameCount > 1);

	if (mFilmstrip->count() != FrameCount)
		RefreshFilmstrip(Model);
	else
	{
		mIgnoreUpdates = true;
		mFilmstrip->setCurrentRow(mCurrentFrameIndex);
		mIgnoreUpdates = false;
	}

	if (!mPlayTimer->isActive())
		RefreshOnionSkin(Model);
}

void lcAnimateWidget::FilmstripItemChanged(int Row)
{
	if (mIgnoreUpdates || Row < 0)
		return;

	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	mCurrentFrameIndex = Row;
	ApplyFrame(Model, mCurrentFrameIndex);
	Update();
}

void lcAnimateWidget::CaptureClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	EnsureInitialized(Model);

	mFrames.insert(mFrames.begin() + mCurrentFrameIndex + 1, SnapshotFrame(Model));
	mCurrentFrameIndex++;

	mThumbnailCache.clear();
	Update();
}

void lcAnimateWidget::DuplicateClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	EnsureInitialized(Model);

	mFrames.insert(mFrames.begin() + mCurrentFrameIndex + 1, mFrames[mCurrentFrameIndex]);
	mCurrentFrameIndex++;

	mThumbnailCache.clear();
	ApplyFrame(Model, mCurrentFrameIndex);
	Update();
}

void lcAnimateWidget::DeleteClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model || mFrames.size() <= 1)
		return;

	mFrames.erase(mFrames.begin() + mCurrentFrameIndex);

	if (mCurrentFrameIndex >= static_cast<int>(mFrames.size()))
		mCurrentFrameIndex = static_cast<int>(mFrames.size()) - 1;

	mThumbnailCache.clear();
	ApplyFrame(Model, mCurrentFrameIndex);
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

	EnsureInitialized(Model);

	if (mCurrentFrameIndex >= static_cast<int>(mFrames.size()) - 1)
	{
		mCurrentFrameIndex = 0;
		ApplyFrame(Model, mCurrentFrameIndex);
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

	if (mCurrentFrameIndex >= static_cast<int>(mFrames.size()) - 1)
		mCurrentFrameIndex = 0;
	else
		mCurrentFrameIndex++;

	ApplyFrame(Model, mCurrentFrameIndex);
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
	// Socket Mode disables the Move tool so limbs can only be rotated (staying attached to their
	// shoulder/hip socket); Free Move re-enables it so a piece can be dragged out of its socket
	// entirely, e.g. for a "the arm falls off" frame.
	gMainWindow->mActions[LC_EDIT_ACTION_MOVE]->setEnabled(!Checked);

	if (Checked && gMainWindow->GetTool() == lcTool::Move)
		gMainWindow->SetTool(lcTool::Rotate);
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

	const lcMatrix44 HandWorldMatrix(HandPiece->GetRotation(), HandPiece->GetPosition());
	const lcMatrix44 AccessoryWorldMatrix = lcMul(AccessoryEntry->Offset, HandWorldMatrix);

	AccessoryPiece->SetPosition(AccessoryWorldMatrix.GetTranslation(), 1, false);
	AccessoryPiece->SetRotation(lcMatrix33(AccessoryWorldMatrix), 1, false);
	AccessoryPiece->UpdatePosition(1);

	if (lcGroup* HandGroup = HandPiece->GetGroup())
		AccessoryPiece->SetGroup(HandGroup);

	Model->SetCurrentStep(1);
}

void lcAnimateWidget::ExportClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	EnsureInitialized(Model);

	lcAnimateExportDialog Dialog(this, Model, mFpsSpinBox->value(), mFrames);
	Dialog.exec();

	// Exporting poses pieces as each exported frame; make sure the viewport reflects the frame
	// we're actually parked on once the dialog closes.
	ApplyFrame(Model, mCurrentFrameIndex);
}
