#include "lc_global.h"
#include "lc_animatewidget.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_mainwindow.h"
#include "project.h"
#include "object.h"

lcAnimateWidget::lcAnimateWidget(QWidget* Parent)
	: QWidget(Parent)
{
	QVBoxLayout* MainLayout = new QVBoxLayout(this);
	MainLayout->setContentsMargins(4, 4, 4, 4);

	QHBoxLayout* SliderLayout = new QHBoxLayout;
	mFrameSlider = new QSlider(Qt::Horizontal, this);
	mFrameSlider->setMinimum(1);
	mFrameSlider->setMaximum(1);
	mFrameLabel = new QLabel(tr("Frame 1 / 1"), this);
	mFrameLabel->setMinimumWidth(90);
	SliderLayout->addWidget(mFrameSlider);
	SliderLayout->addWidget(mFrameLabel);
	MainLayout->addLayout(SliderLayout);

	QHBoxLayout* ButtonLayout = new QHBoxLayout;

	mPlayButton = new QPushButton(tr("Play"), this);
	ButtonLayout->addWidget(mPlayButton);

	ButtonLayout->addWidget(new QLabel(tr("fps:"), this));
	mFpsSpinBox = new QSpinBox(this);
	mFpsSpinBox->setRange(1, 60);
	mFpsSpinBox->setValue(12);
	ButtonLayout->addWidget(mFpsSpinBox);

	mRecordButton = new QToolButton(this);
	mRecordButton->setText(tr("Record"));
	mRecordButton->setCheckable(true);
	mRecordButton->setToolTip(tr("When enabled, moving or rotating a piece adds a keyframe at the current frame instead of overwriting the previous one"));
	ButtonLayout->addWidget(mRecordButton);

	QPushButton* NewFrameButton = new QPushButton(tr("+ Frame"), this);
	NewFrameButton->setToolTip(tr("Insert a new blank frame after the current one"));
	ButtonLayout->addWidget(NewFrameButton);

	QPushButton* AddKeyframeButton = new QPushButton(tr("Add Keyframe"), this);
	AddKeyframeButton->setToolTip(tr("Key the position and rotation of the selected pieces at the current frame"));
	ButtonLayout->addWidget(AddKeyframeButton);

	ButtonLayout->addStretch();

	mExportButton = new QPushButton(tr("Export Animation..."), this);
	ButtonLayout->addWidget(mExportButton);

	MainLayout->addLayout(ButtonLayout);

	mPlayTimer = new QTimer(this);
	connect(mPlayTimer, &QTimer::timeout, this, &lcAnimateWidget::Timeout);

	connect(mFrameSlider, &QSlider::valueChanged, this, &lcAnimateWidget::SliderChanged);
	connect(mPlayButton, &QPushButton::clicked, this, &lcAnimateWidget::PlayPauseClicked);
	connect(mRecordButton, &QToolButton::toggled, this, &lcAnimateWidget::RecordToggled);
	connect(NewFrameButton, &QPushButton::clicked, this, &lcAnimateWidget::NewFrameClicked);
	connect(AddKeyframeButton, &QPushButton::clicked, this, &lcAnimateWidget::AddKeyframeClicked);
	connect(mExportButton, &QPushButton::clicked, this, &lcAnimateWidget::ExportClicked);
}

void lcAnimateWidget::Update()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	mIgnoreUpdates = true;

	const lcStep CurrentStep = Model->GetCurrentStep();
	const lcStep LastStep = qMax(Model->GetLastStep(), CurrentStep);

	mFrameSlider->setMaximum(static_cast<int>(LastStep));
	mFrameSlider->setValue(static_cast<int>(CurrentStep));
	mFrameLabel->setText(tr("Frame %1 / %2").arg(CurrentStep).arg(LastStep));
	mRecordButton->setChecked(gMainWindow->GetAddKeys());

	mIgnoreUpdates = false;
}

void lcAnimateWidget::SetSelection(const std::vector<lcObject*>& Selection)
{
	mSelection = Selection;
}

void lcAnimateWidget::SliderChanged(int Value)
{
	if (mIgnoreUpdates)
		return;

	lcModel* Model = lcGetActiveModel();

	if (Model)
		Model->SetCurrentStep(static_cast<lcStep>(Value));
}

void lcAnimateWidget::PlayPauseClicked()
{
	if (mPlayTimer->isActive())
	{
		mPlayTimer->stop();
		mPlayButton->setText(tr("Play"));
		return;
	}

	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	if (Model->GetCurrentStep() >= Model->GetLastStep())
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

	if (Model->GetCurrentStep() >= Model->GetLastStep())
		Model->SetCurrentStep(1);
	else
		Model->ShowNextStep();
}

void lcAnimateWidget::NewFrameClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	Model->InsertStepAction(Model->GetCurrentStep() + 1);
	Model->ShowNextStep();
}

void lcAnimateWidget::AddKeyframeClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model || mSelection.empty())
		return;

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
		Model->SetObjectsKeyFrame(mSelection, PropertyId, true);
}

void lcAnimateWidget::RecordToggled(bool Checked)
{
	if (mIgnoreUpdates)
		return;

	gMainWindow->SetAddKeys(Checked);
}

void lcAnimateWidget::ExportClicked()
{
	lcModel* Model = lcGetActiveModel();

	if (!Model)
		return;

	lcAnimateExportDialog Dialog(this, Model, mFpsSpinBox->value());
	Dialog.exec();
}
