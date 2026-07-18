#pragma once

#include <QWidget>
#include <QComboBox>
#include <vector>

struct lcKeyframePoint;
enum class lcEasingType;

class lcKeyframeTimelineWidget : public QWidget
{
	Q_OBJECT

public:
	lcKeyframeTimelineWidget(QWidget* Parent = nullptr);

	void SetKeyframes(const std::vector<lcKeyframePoint>* Keyframes);
	void SetCurrentTime(int Frame);
	void SetFrameRange(int Start, int End);
	int GetCurrentTime() const { return mCurrentTime; }
	int GetSelectedKeyframe() const { return mSelectedIndex; }
	int GetSelectedSegment() const { return mSelectedSegment; }

signals:
	void KeyframeSelected(int Index);

protected:
	void paintEvent(QPaintEvent* Event) override;
	void mousePressEvent(QMouseEvent* Event) override;
	void mouseMoveEvent(QMouseEvent* Event) override;

	int TimeToX(int Time) const;
	int XToTime(int X) const;

	const std::vector<lcKeyframePoint>* mKeyframes = nullptr;
	int mCurrentTime = 0;
	int mFrameStart = 0;
	int mFrameEnd = 100;
	int mSelectedIndex = -1;
	int mSelectedSegment = -1;
};
