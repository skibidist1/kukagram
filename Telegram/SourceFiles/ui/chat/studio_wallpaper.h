#pragma once

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <algorithm>

namespace Ui::Studio {

[[nodiscard]] inline bool PaintLunarWallpaper(
		QPainter &painter,
		const QRectF &area,
		const QRect &clip) {
	const auto visible = area.intersected(QRectF(clip));
	if (visible.isEmpty()) {
		return true;
	}
	static const auto image = QImage(
		QStringLiteral(":/gui/art/kukagram-lunar.jpg"));
	if (image.isNull()) {
		return false;
	}

	const auto width = qreal(image.width());
	const auto height = qreal(image.height());
	const auto scale = std::max(area.width() / width, area.height() / height);
	const auto cropWidth = std::min(width, area.width() / scale);
	const auto cropHeight = std::min(height, area.height() / scale);
	// Preserve the framing of the original art while keeping the moon in
	// view. When the full disc cannot fit, crop around its center.
	const auto cropOffset = [](qreal extent, qreal crop, qreal near, qreal far) {
		const auto focus = (near + far) * .5;
		const auto minimum = std::max(0., far * extent - crop);
		const auto maximum = std::min(extent - crop, near * extent);
		if (minimum > maximum) {
			return std::clamp(focus * extent - crop * .5, 0., extent - crop);
		}
		const auto anchor = .5 + (focus - .5) * crop / extent;
		return std::clamp(focus * extent - crop * anchor, minimum, maximum);
	};
	const auto left = cropOffset(width, cropWidth, .50, .895);
	const auto top = cropOffset(height, cropHeight, .055, .745);

	painter.save();
	painter.setClipRect(visible, Qt::IntersectClip);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.drawImage(area, image, QRectF(left, top, cropWidth, cropHeight));
	auto shade = QLinearGradient(area.topLeft(), area.bottomLeft());
	shade.setColorAt(0., QColor(10, 12, 21, 4));
	shade.setColorAt(.55, QColor(10, 12, 21, 14));
	shade.setColorAt(1., QColor(10, 12, 21, 48));
	painter.fillRect(visible, shade);
	painter.restore();
	return true;
}

} // namespace Ui::Studio
