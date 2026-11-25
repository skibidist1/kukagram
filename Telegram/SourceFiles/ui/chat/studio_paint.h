#pragma once

#include "core/kukagram_appearance.h"

#include <QtGui/QPainter>
#include <QtWidgets/QWidget>
#include <array>

namespace Ui::Studio {

[[nodiscard]] inline QColor Violet(int alpha = 255) {
	return QColor(148, 105, 247, alpha);
}

[[nodiscard]] inline QColor Mint(int alpha = 255) {
	return QColor(107, 234, 198, alpha);
}

[[nodiscard]] inline std::array<QColor, 2> BubbleColors(
		bool dark,
		bool outgoing) {
	return dark
		? (outgoing
			? std::array{ QColor(84, 47, 142), QColor(62, 38, 105) }
			: std::array{ QColor(51, 45, 70), QColor(35, 32, 48) })
		: (outgoing
			? std::array{ QColor(220, 201, 251), QColor(239, 227, 255) }
			: std::array{ QColor(237, 231, 247), QColor(248, 245, 251) });
}

[[nodiscard]] inline QLinearGradient Backdrop(const QRectF &area) {
	auto wash = QLinearGradient(area.topLeft(), area.bottomRight());
	wash.setColorAt(0., Violet(72));
	wash.setColorAt(0.6, QColor(77, 57, 102, 40));
	wash.setColorAt(1., Mint(38));
	return wash;
}

inline void PaintBackdrop(QWidget *widget, const QRect &clip) {
	if (!clip.isEmpty() && Core::KukagramAppearance::Current().studio) {
		auto painter = QPainter(widget);
		painter.fillRect(clip, Backdrop(widget->rect()));
	}
}

inline void PaintHeader(QPainter &painter, const QRectF &bar, qreal line) {
	auto wash = QLinearGradient(bar.topLeft(), bar.topRight());
	wash.setColorAt(0., Violet(85));
	wash.setColorAt(0.65, Violet(25));
	wash.setColorAt(1., Mint(46));
	painter.fillRect(bar, wash);
	auto rail = QLinearGradient(bar.topLeft(), bar.topRight());
	rail.setColorAt(0., Violet());
	rail.setColorAt(1., Mint());
	painter.fillRect(QRectF(bar.x(), bar.bottom() - line, bar.width(), line), rail);
}

} // namespace Ui::Studio
