#include "lc_global.h"
#include "lc_animateexportdialog.h"
#include "lc_model.h"
#include "lc_view.h"
#include "lc_profile.h"

lcAnimateExportDialog::lcAnimateExportDialog(QWidget* Parent, lcModel* Model, int DefaultFps, const std::vector<lcAnimateFrame>& Frames)
	: QDialog(Parent), mModel(Model), mFrames(Frames)
{
	setWindowTitle(tr("Export Animation"));

	QVBoxLayout* MainLayout = new QVBoxLayout(this);
	QFormLayout* Form = new QFormLayout;

	mFormatCombo = new QComboBox(this);
	mFormatCombo->addItem(tr("Animated GIF (.gif)"));
	mFormatCombo->addItem(tr("Video (.mp4)"));
	mFormatCombo->addItem(tr("PNG Sequence (folder)"));
	Form->addRow(tr("Format:"), mFormatCombo);

	mFpsSpinBox = new QSpinBox(this);
	mFpsSpinBox->setRange(1, 60);
	mFpsSpinBox->setValue(DefaultFps);
	Form->addRow(tr("Frames per second:"), mFpsSpinBox);

	const int FrameCount = static_cast<int>(mFrames.size());

	mStartSpinBox = new QSpinBox(this);
	mStartSpinBox->setRange(1, FrameCount);
	mStartSpinBox->setValue(1);
	Form->addRow(tr("Start frame:"), mStartSpinBox);

	mEndSpinBox = new QSpinBox(this);
	mEndSpinBox->setRange(1, FrameCount);
	mEndSpinBox->setValue(FrameCount);
	Form->addRow(tr("End frame:"), mEndSpinBox);

	QHBoxLayout* FileLayout = new QHBoxLayout;
	mFileEdit = new QLineEdit(this);
	QPushButton* BrowseButton = new QPushButton(tr("Browse..."), this);
	FileLayout->addWidget(mFileEdit);
	FileLayout->addWidget(BrowseButton);
	Form->addRow(tr("Output:"), FileLayout);

	MainLayout->addLayout(Form);

	QDialogButtonBox* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	MainLayout->addWidget(Buttons);

	connect(BrowseButton, &QPushButton::clicked, this, &lcAnimateExportDialog::Browse);
	connect(Buttons, &QDialogButtonBox::accepted, this, &lcAnimateExportDialog::Accept);
	connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void lcAnimateExportDialog::Browse()
{
	if (mFormatCombo->currentIndex() == 2)
	{
		const QString Dir = QFileDialog::getExistingDirectory(this, tr("Select Output Folder"), mFileEdit->text());

		if (!Dir.isEmpty())
			mFileEdit->setText(Dir);
	}
	else
	{
		const bool IsGif = mFormatCombo->currentIndex() == 0;
		const QString Filter = IsGif ? tr("Animated GIF (*.gif)") : tr("MP4 Video (*.mp4)");
		const QString FileName = QFileDialog::getSaveFileName(this, tr("Export Animation"), mFileEdit->text(), Filter);

		if (!FileName.isEmpty())
			mFileEdit->setText(FileName);
	}
}

void lcAnimateExportDialog::Accept()
{
	const QString OutputPath = mFileEdit->text().trimmed();

	if (OutputPath.isEmpty())
	{
		QMessageBox::warning(this, tr("Export Animation"), tr("Please choose an output file or folder."));
		return;
	}

	const int Start = mStartSpinBox->value();
	const int End = mEndSpinBox->value();

	if (Start > End)
	{
		QMessageBox::warning(this, tr("Export Animation"), tr("Start frame must not be after end frame."));
		return;
	}

	const int Format = mFormatCombo->currentIndex(); // 0 = GIF, 1 = MP4, 2 = PNG Sequence
	const int Fps = mFpsSpinBox->value();

	QTemporaryDir TempDir;

	if (Format != 2 && !TempDir.isValid())
	{
		QMessageBox::warning(this, tr("Export Animation"), tr("Could not create a temporary folder."));
		return;
	}

	const QString FrameDir = (Format == 2) ? OutputPath : TempDir.path();
	QDir().mkpath(FrameDir);

	lcView View(lcViewType::View, mModel);
	View.SetOffscreenContext();
	View.MakeCurrent();
	View.SetSize(lcGetProfileInt(LC_PROFILE_RENDER_WIDTH), lcGetProfileInt(LC_PROFILE_RENDER_HEIGHT));

	QProgressDialog Progress(tr("Rendering frames..."), tr("Cancel"), 0, End - Start + 1, this);
	Progress.setWindowModality(Qt::WindowModal);

	// Frames are exported as a sequential 1..N sequence regardless of which logical frame numbers
	// Start/End refer to - ffmpeg and the PNG sequence don't need to know the original numbering.
	int OutputNumber = 1;

	for (int FrameIndex = Start - 1; FrameIndex <= End - 1; FrameIndex++, OutputNumber++)
	{
		lcPoseAnimateFrame(mModel, mFrames[FrameIndex]);

		const std::vector<QImage> Images = View.GetStepImages(1, 1);

		if (!Images.empty())
		{
			const QString FileName = FrameDir + QString("/frame_%1.png").arg(OutputNumber, 2, 10, QLatin1Char('0'));
			QImageWriter Writer(FileName);

			if (Writer.format().isEmpty())
				Writer.setFormat("png");

			Writer.write(Images.front());
		}

		Progress.setValue(OutputNumber);
		QApplication::processEvents();

		if (Progress.wasCanceled())
			return;
	}

	if (Format == 2)
	{
		QMessageBox::information(this, tr("Export Animation"), tr("PNG sequence saved to:\n%1").arg(FrameDir));
		QDialog::accept();
		return;
	}

	const QString FfmpegPath = QStandardPaths::findExecutable(QLatin1String("ffmpeg"));

	if (FfmpegPath.isEmpty())
	{
		QMessageBox::warning(this, tr("Export Animation"), tr("ffmpeg was not found on your system PATH, so the animation can't be encoded to %1.\nThe rendered PNG frames are in:\n%2").arg(Format == 0 ? tr("GIF") : tr("MP4"), FrameDir));
		return;
	}

	const QString InputPattern = FrameDir + QLatin1String("/frame_%02d.png");
	const QStringList Arguments = lcBuildFfmpegAnimationArguments(InputPattern, Fps, OutputPath, Format == 0);

	QProcess Ffmpeg;
	Ffmpeg.start(FfmpegPath, Arguments);

	if (!Ffmpeg.waitForFinished(120000) || Ffmpeg.exitCode() != 0)
	{
		QMessageBox::warning(this, tr("Export Animation"), tr("ffmpeg failed to encode the animation:\n%1").arg(QString::fromLocal8Bit(Ffmpeg.readAllStandardError())));
		return;
	}

	QMessageBox::information(this, tr("Export Animation"), tr("Animation saved to:\n%1").arg(OutputPath));

	QDialog::accept();
}
