#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Ui::Studio {

class Moon final {
public:
	[[nodiscard]] static bool available() {
		static const auto exists = QFile::exists(TexturePath());
		return exists && !CachedTexture().failed;
	}

	void rotate(QPointF deltaDegrees) {
		if (!std::isfinite(deltaDegrees.x())
			|| !std::isfinite(deltaDegrees.y())) {
			return;
		}
		const auto longitude = std::remainder(
			_longitude + std::remainder(deltaDegrees.x(), 360.), 360.);
		const auto latitude = std::clamp(
			_latitude + deltaDegrees.y(), -90., 90.);
		if (_longitude != longitude || _latitude != latitude) {
			_longitude = longitude;
			_latitude = latitude;
			_dirty = true;
		}
	}

	void paint(QPainter &painter, const QRectF &bounds) {
		if (bounds.isEmpty() || !available()) {
			return;
		}
		constexpr auto kMaxFramePixels = 640.;
		const auto diameter = std::min(bounds.width(), bounds.height());
		const auto pixels = qRound(std::clamp(
			diameter * painter.device()->devicePixelRatioF(),
			1.,
			kMaxFramePixels));
		if (_dirty || _frame.width() != pixels) {
			_frame = render(pixels);
			_dirty = false;
		}
		auto target = QRectF(0., 0., diameter, diameter);
		target.moveCenter(bounds.center());
		painter.save();
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.drawImage(target, _frame);
		painter.restore();
	}

private:
	struct TextureCache {
		QImage image;
		bool failed = false;
	};

	[[nodiscard]] static QString TexturePath() {
		return QStringLiteral(":/gui/art/kukagram-moon-map.jpg");
	}

	[[nodiscard]] static TextureCache &CachedTexture() {
		static auto cache = TextureCache();
		return cache;
	}

	[[nodiscard]] static const QImage &Texture() {
		auto &cache = CachedTexture();
		if (cache.image.isNull() && !cache.failed) {
			cache.image = QImage(TexturePath()).convertToFormat(QImage::Format_RGB32);
			cache.failed = cache.image.isNull();
			if (!cache.failed) {
				constexpr auto kTextureReuseMs = 20'000;
				QTimer::singleShot(
					kTextureReuseMs,
					Qt::VeryCoarseTimer,
					QCoreApplication::instance(),
					[] { CachedTexture().image = QImage(); });
			}
		}
		return cache.image;
	}

	[[nodiscard]] static QRgb Sample(
			const QImage &image,
			float longitude,
			float latitude) {
		const auto width = image.width();
		const auto height = image.height();
		const auto x = (longitude - std::floor(longitude)) * width - .5f;
		const auto y = std::clamp(latitude * height - .5f, 0.f, float(height - 1));
		const auto left = int(std::floor(x));
		const auto top = int(y);
		const auto wrappedLeft = (left < 0) ? (left + width) : left;
		const auto wrappedRight = (wrappedLeft + 1 == width) ? 0 : (wrappedLeft + 1);
		const auto upper = reinterpret_cast<const QRgb*>(image.constScanLine(top));
		const auto lower = reinterpret_cast<const QRgb*>(
			image.constScanLine(std::min(top + 1, height - 1)));
		const auto fx = x - left;
		const auto fy = y - top;
		const auto a = upper[wrappedLeft];
		const auto b = upper[wrappedRight];
		const auto c = lower[wrappedLeft];
		const auto d = lower[wrappedRight];
		const auto upperLeftWeight = (1.f - fx) * (1.f - fy);
		const auto upperRightWeight = fx * (1.f - fy);
		const auto lowerLeftWeight = (1.f - fx) * fy;
		const auto lowerRightWeight = fx * fy;
		const auto channel = [&](int shift) {
			return qRound(
				float((a >> shift) & 255) * upperLeftWeight
				+ float((b >> shift) & 255) * upperRightWeight
				+ float((c >> shift) & 255) * lowerLeftWeight
				+ float((d >> shift) & 255) * lowerRightWeight);
		};
		return qRgb(channel(16), channel(8), channel(0));
	}

	[[nodiscard]] QImage render(int pixels) const {
		constexpr auto kPi = std::numbers::pi_v<float>;
		const auto &texture = Texture();
		if (texture.isNull()) {
			return {};
		}
		const auto yaw = float(_longitude / 360.);
		const auto pitch = float(_latitude) * kPi / 180.f;
		const auto sinPitch = std::sin(pitch);
		const auto cosPitch = std::cos(pitch);
		const auto step = 2.f / pixels;
		const auto inner = 1.f - step / 2.f;
		const auto outer = 1.f + step / 2.f;
		auto frame = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
		if (frame.isNull()) {
			return frame;
		}
		frame.fill(Qt::transparent);
		for (auto row = 0; row != pixels; ++row) {
			const auto up = 1.f - (row + .5f) * step;
			const auto scan = reinterpret_cast<QRgb*>(frame.scanLine(row));
			for (auto column = 0; column != pixels; ++column) {
				const auto right = (column + .5f) * step - 1.f;
				const auto distanceSquared = right * right + up * up;
				if (distanceSquared >= outer * outer) {
					continue;
				}
				const auto alpha = (distanceSquared <= inner * inner)
					? 255
					: qRound(255.f * ((1.f - std::sqrt(distanceSquared)) / step + .5f));
				const auto front = std::sqrt(std::max(0.f, 1.f - distanceSquared));
				const auto north = std::clamp(up * cosPitch + front * sinPitch, -1.f, 1.f);
				const auto depth = front * cosPitch - up * sinPitch;
				const auto longitude = .5f + yaw + std::atan2(right, depth) / (2.f * kPi);
				const auto latitude = .5f - std::asin(north) / kPi;
				const auto terrain = Sample(texture, longitude, latitude);
				const auto sunlight = -.42f * right + .34f * up + .84f * front;
				const auto shade = .36f + .60f * std::max(0.f, sunlight);
				const auto tint = [&](int color, float gain, float offset) {
					return std::clamp(qRound(color * shade * gain + offset), 0, 255);
				};
				scan[column] = qPremultiply(qRgba(
					tint(qRed(terrain), .94f, 3.f),
					tint(qGreen(terrain), .98f, 4.f),
					tint(qBlue(terrain), 1.05f, 8.f),
					alpha));
			}
		}
		return frame;
	}

	QImage _frame;
	qreal _longitude = 0.;
	qreal _latitude = 0.;
	bool _dirty = true;

};

} // namespace Ui::Studio
