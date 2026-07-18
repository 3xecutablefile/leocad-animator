#include "lc_global.h"
#include "lc_animatewidget.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_mainwindow.h"
#include "lc_view.h"
#include "project.h"
#include "object.h"
#include "piece.h"

static const int THUMBNAIL_WIDTH = 96;
static const int THUMBNAIL_HEIGHT = 72;

lcAnimateWidget::lcAnimateWidget(QWidget* Parent)
	: QWidget(Parent)
{
	QVBoxLayout* MainLayout = new QVBoxLayout(this);
	MainLayout->setContentsMargins(4, 4, 4, 4);

	// Row 1: onion skin preview | big capture button | play/fps
	QHBoxLayout* ControlLayout = new QHBoxLayout;

	QVBoxLayout* OnionLayout = new QVBoxLayout;
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
	mCaptureButton->setToolTip(tr("Insert a new frame after the current one and snapshot every piece's position and rotation, like pressing the shutter on a stop-motion camera"));
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
	connect(mPlayButton, &QPushButton::clicked, this, &lcAnimateWidget::PlayPauseClicked);
	connect(mOnionSkinCheck, &QCheckBox::toggled, this, &lcAnimateWidget::OnionSkinToggled);
	connect(mExportButton, &QPushButton::clicked, this, &lcAnimateWidget::ExportClicked);
	connect(mFilmstrip, &QListWidget::currentRowChanged, this, &lcAnimateWidget::FilmstripItemChanged);
}

QIcon lcAnimateWidget::RenderStepThumbnail(lcModel* Model, quint32 Step, int Width, int Height)
{
	lcView View(lcViewType::View, Model);
	View.SetOffscreenContext();
	View.MakeCurrent();
	View.SetSize(Width, Height);

	std::vector<QImage> Images = View.GetStepImages(Step, Step);

	if (Images.empty())
		return QIcon();

	return QIcon(QPixmap::fromImage(Images.front()));
}

void lcAnimateWidget::RefreshFilmstrip(lcModel* Model)
{
	const lcStep CurrentStep = Model->GetCurrentStep();
	const lcStep LastStep = mFrameCount;

	mIgnoreUpdates = true;
	mFilmstrip->clear();

	for (lcStep Step = 1; Step <= LastStep; Step++)
	{
		QIcon Icon = mThumbnailCache.value(static_cast<int>(Step));

		if (Icon.isNull())
		{
			Icon = RenderStepThumbnail(Model, Step, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
			mThumbnailCache.insert(static_cast<int>(Step), Icon);
		}

		mFilmstrip->addItem(new QListWidgetItem(Icon, QString::number(Step)));
	}

	mFilmstrip->setCurrentRow(static_cast<int>(CurrentStep) - 1);
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

	const lcStep CurrentStep = Model->GetCurrentStep();

	if (CurrentStep <= 1)
	{
		mOnionSkinPreview->setPixmap(QPixmap());
		mOnionSkinPreview->setText(tr("No previous frame"));
		return;
	}

	// Reuse the filmstrip's cached thumbnail instead of spinning up another offscreen GL render -
	// one less place for context churn to go wrong, and it's already sitting there.
	QIcon Icon = mThumbnailCache.value(static_cast<int>(CurrentStep - 1));

	if (Icon.isNull())
		Icon = RenderStepThumbnail(Model, CurrentStep - 1, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);

	mOnionSkinPreview->setText(QString());
	mOnionSkinPreview->setPixmap(Icon.pixmap(120, 90));
}

void lcAnimateWidget::Update()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	const lcStep CurrentStep = Model->GetCurrentStep();

	// Self-heal upward only: if we're sitting on a step further out than what we think the frame
	// count is (e.g. a freshly loaded document), adopt it. Never shrink just because the user
	// navigated to an earlier frame - that's not the same as the animation getting shorter.
	mFrameCount = qMax(mFrameCount, CurrentStep);
	const lcStep LastStep = mFrameCount;

	mFrameLabel->setText(tr("Frame %1 / %2").arg(CurrentStep).arg(LastStep));
	mDeleteButton->setEnabled(LastStep > 1);

	// ponytail: only re-render filmstrip thumbnails (offscreen GL) when the frame count actually
	// changed. Doing this on every step change was stealing the GL context out from under the live
	// viewport on every single Play tick, so the 3D view never visibly updated during playback.
	if (mFilmstrip->count() != static_cast<int>(LastStep))
		RefreshFilmstrip(Model);
	else
	{
		mIgnoreUpdates = true;
		mFilmstrip->setCurrentRow(static_cast<int>(CurrentStep) - 1);
		mIgnoreUpdates = false;
	}

	// Onion skin is a posing aid, not a playback aid, and rendering it is the same GL-context
	// hazard, so skip it while actively playing.
	if (!mPlayTimer->isActive())
		RefreshOnionSkin(Model);
}

void lcAnimateWidget::FilmstripItemChanged(int Row)
{
	if (mIgnoreUpdates || Row < 0)
		return;

	lcModel* Model = lcGetActiveModel();

	if (Model)
		Model->SetCurrentStep(static_cast<lcStep>(Row + 1));
}

void lcAnimateWidget::CaptureClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	Model->InsertStepAction(Model->GetCurrentStep() + 1);
	Model->ShowNextStep();
	mFrameCount++;

	std::vector<lcObject*> Pieces;

	for (const std::unique_ptr<lcPiece>& Piece : Model->GetPieces())
		Pieces.push_back(Piece.get());

	if (!Pieces.empty())
	{
		static const lcObjectPropertyId Properties[] =
		{
			lcObjectPropertyId::ObjectPositionX,
			lcObjectPropertyId::ObjectPositionY,
			lcObjectPropertyId::ObjectPositionZ,
			lcObjectPropertyId::ObjectRotationX,
			lcObjectPropertyId::ObjectRotationY,
			lcObjectPropertyId::ObjectRotationZ
		};

		for (lcObjectPropertyId PropertyId : Properties)
			Model->SetObjectsKeyFrame(Pieces, PropertyId, true);
	}

	mThumbnailCache.clear();
	Update();
}

void lcAnimateWidget::DuplicateClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	Model->InsertStepAction(Model->GetCurrentStep() + 1);
	Model->ShowNextStep();
	mFrameCount++;

	mThumbnailCache.clear();
	Update();
}

void lcAnimateWidget::DeleteClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model || mFrameCount <= 1)
		return;

	Model->RemoveStepAction(Model->GetCurrentStep());
	mFrameCount--;

	// RemoveStepAction doesn't clamp CurrentStep, so deleting the last frame while sitting on it
	// leaves CurrentStep pointing past the new end - which then tricks Update()'s "never shrink
	// the frame count below CurrentStep" self-heal into silently undoing this decrement.
	if (Model->GetCurrentStep() > mFrameCount)
		Model->SetCurrentStep(mFrameCount);

	mThumbnailCache.clear();
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

	if (Model->GetCurrentStep() >= mFrameCount)
		Model->SetCurrentStep(1);

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

	if (Model->GetCurrentStep() >= mFrameCount)
		Model->SetCurrentStep(1);
	else
		Model->ShowNextStep();
}

void lcAnimateWidget::OnionSkinToggled(bool)
{
	lcModel* Model = lcGetActiveModel();

	if (Model)
		RefreshOnionSkin(Model);
}

void lcAnimateWidget::ExportClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	lcAnimateExportDialog Dialog(this, Model, mFpsSpinBox->value(), static_cast<int>(mFrameCount));
	Dialog.exec();
}
