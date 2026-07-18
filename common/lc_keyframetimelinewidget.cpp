#include "lc_keyframetimelinewidget.h"
#include "lc_animatewidget.h"
#include <QPainter>
#include <QMouseEvent>

lcKeyframeTimelineWidget::lcKeyframeTimelineWidget(QWidget* Parent)
	: QWidget(Parent)
{
	setFixedHeight(50);
	setMouseTracking(true);
}

void lcKeyframeTimelineWidget::SetKeyframes(const std::vector<lcKeyframePoint>* Keyframes)
{
	mKeyframes = Keyframes;
	mSelectedIndex = -1;
	mSelectedSegment = -1;
	update();
}

void lcKeyframeTimelineWidget::SetCurrentTime(int Frame)
{
	mCurrentTime = Frame;
	update();
}

void lcKeyframeTimelineWidget::SetFrameRange(int Start, int End)
{
	mFrameStart = Start;
	mFrameEnd = End;
	update();
}

int lcKeyframeTimelineWidget::TimeToX(int Time) const
{
	const int w = width() - 20;
	if (mFrameEnd <= mFrameStart)
		return 10;
	return 10 + (Time - mFrameStart) * w / (mFrameEnd - mFrameStart);
}

int lcKeyframeTimelineWidget::XToTime(int X) const
{
	const int w = width() - 20;
	if (w <= 0)
		return mFrameStart;
	return mFrameStart + (X - 10) * (mFrameEnd - mFrameStart) / w;
}

void lcKeyframeTimelineWidget::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);

	const int h = height();
	const int w = width();

	// Background
	p.fillRect(rect(), QColor(45, 45, 45));

	// Track line
	const int trackY = h / 2;
	p.setPen(QColor(80, 80, 80));
	p.drawLine(10, trackY, w - 10, trackY);

	if (!mKeyframes || mKeyframes->empty())
	{
		p.setPen(QColor(140, 140, 140));
		p.drawText(rect(), Qt::AlignCenter, tr("No keyframes — click Add Keyframe"));
		return;
	}

	// Segment fills and frame ticks
	for (size_t i = 0; i < mKeyframes->size() - 1; i++)
	{
		const lcKeyframePoint& kf = (*mKeyframes)[i];
		const lcKeyframePoint& next = (*mKeyframes)[i + 1];
		const int x1 = TimeToX(kf.Time);
		const int x2 = TimeToX(next.Time);

		// Easing label centered on segment
		const char* easingNames[] = { "Lin", "In", "Out", "InOut" };
		QColor segmentColors[] = { QColor(100, 100, 100), QColor(60, 120, 200), QColor(200, 120, 60), QColor(100, 180, 100) };
		const int ei = static_cast<int>(kf.SegmentEasing);
		if (ei >= 0 && ei < 4)
		{
			p.setPen(segmentColors[ei]);
			p.drawText((x1 + x2) / 2 - 12, 8, 24, 14, Qt::AlignCenter, easingNames[ei]);
		}

		// Frame ticks along segment
		for (int f = kf.Time + 1; f < next.Time; f++)
		{
			const int x = TimeToX(f);
			p.setPen(QColor(70, 70, 70));
			p.drawLine(x, trackY - 3, x, trackY + 3);
		}
	}

	// Keyframe diamonds
	for (size_t i = 0; i < mKeyframes->size(); i++)
	{
		const int x = TimeToX((*mKeyframes)[i].Time);
		QPolygon diamond;
		diamond << QPoint(x, trackY - 6) << QPoint(x + 5, trackY) << QPoint(x, trackY + 6) << QPoint(x - 5, trackY);

		if ((int)i == mSelectedIndex)
		{
			p.setBrush(QColor(255, 200, 50));
			p.setPen(QPen(QColor(255, 200, 50), 2));
		}
		else
		{
			p.setBrush(QColor(200, 200, 200));
			p.setPen(QPen(QColor(200, 200, 200), 1));
		}
		p.drawPolygon(diamond);

		// Frame number below
		p.setPen(QColor(180, 180, 180));
		QFont f = p.font();
		f.setPointSize(7);
		p.setFont(f);
		p.drawText(x - 15, trackY + 10, 30, 14, Qt::AlignCenter, QString::number((*mKeyframes)[i].Time));
	}

	// Current time cursor
	{
		const int x = TimeToX(mCurrentTime);
		p.setPen(QPen(QColor(255, 100, 100), 2));
		p.drawLine(x, 4, x, h - 4);
	}
}

void lcKeyframeTimelineWidget::mousePressEvent(QMouseEvent* Event)
{
	if (!mKeyframes || mKeyframes->empty())
		return;

	const int mx = Event->x();

	// Check if clicking on a keyframe diamond
	for (size_t i = 0; i < mKeyframes->size(); i++)
	{
		const int x = TimeToX((*mKeyframes)[i].Time);
		if (abs(mx - x) <= 6)
		{
			mSelectedIndex = (int)i;
			mSelectedSegment = -1;
			emit KeyframeSelected((int)i);
			update();
			return;
		}
	}

	// Click on a segment — select the segment and seek
	for (size_t i = 0; i < mKeyframes->size() - 1; i++)
	{
		const int x1 = TimeToX((*mKeyframes)[i].Time);
		const int x2 = TimeToX((*mKeyframes)[i + 1].Time);
		if (mx >= x1 && mx <= x2)
		{
			mSelectedIndex = -1;
			mSelectedSegment = (int)i;
			mCurrentTime = qBound(mFrameStart, XToTime(mx), mFrameEnd);
			emit CurrentTimeDragged(mCurrentTime);
			update();
			return;
		}
	}

	// Click elsewhere — set current time
	mSelectedIndex = -1;
	mSelectedSegment = -1;
	mCurrentTime = qBound(mFrameStart, XToTime(mx), mFrameEnd);
	update();
}

void lcKeyframeTimelineWidget::mouseMoveEvent(QMouseEvent* Event)
{
	if (Event->buttons() & Qt::LeftButton)
	{
		const int newTime = qBound(mFrameStart, XToTime(Event->x()), mFrameEnd);
		if (newTime != mCurrentTime)
		{
			mCurrentTime = newTime;
			emit CurrentTimeDragged(mCurrentTime);
			update();
		}
	}
}
