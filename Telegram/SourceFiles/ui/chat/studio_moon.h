#pragma once

#include "base/algorithm.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

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
			render(pixels);
			_dirty = false;
		}
		const auto ratio = painter.device()->devicePixelRatioF();
		const auto side = pixels / ratio;
		const auto snap = [&](qreal value) {
			return base::SafeRound(value * ratio) / ratio;
		};
		const auto target = QRectF(
			snap(bounds.center().x() - side / 2.),
			snap(bounds.center().y() - side / 2.),
			side,
			side);
		painter.save();
		painter.setRenderHint(QPainter::SmoothPixmapTransform);
		painter.drawImage(target, _frame);
		painter.restore();
	}

private:
	struct TextureCache {
		QImage image;
		qint64 used = 0;
		bool failed = false;
	};

	static constexpr auto kTextureReuseMs = 20'000;

	static void ScheduleTextureRelease() {
		QTimer::singleShot(
			kTextureReuseMs,
			Qt::VeryCoarseTimer,
			QCoreApplication::instance(),
			[] {
				auto &cache = CachedTexture();
				const auto idle = QDateTime::currentMSecsSinceEpoch() - cache.used;
				if (idle >= kTextureReuseMs) {
					cache.image = QImage();
				} else if (!cache.image.isNull()) {
					ScheduleTextureRelease();
				}
			});
	}

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
				ScheduleTextureRelease();
			}
		}
		cache.used = QDateTime::currentMSecsSinceEpoch();
		return cache.image;
	}

	struct Texel {
		quint32 target = 0;
		quint32 top = 0;
		quint32 bottom = 0;
		quint32 x = 0;
		std::array<quint16, 3> shade = {};
		quint8 fy = 0;
		quint8 alpha = 0;
	};

	struct Geometry {
		int pixels = 0;
		qreal latitude = 0.;
		QSize texture;
		std::vector<Texel> texels;
	};

	void prepareGeometry(int pixels, const QImage &texture) {
		if (_geometry.pixels == pixels
			&& _geometry.latitude == _latitude
			&& _geometry.texture == texture.size()) {
			return;
		}
		constexpr auto kPi = std::numbers::pi_v<float>;
		const auto width = texture.width();
		const auto height = texture.height();
		const auto textureStride = quint32(texture.bytesPerLine() / 4);
		const auto pitch = float(_latitude) * kPi / 180.f;
		const auto sinPitch = std::sin(pitch);
		const auto cosPitch = std::cos(pitch);
		const auto step = 2.f / pixels;
		const auto inner = 1.f - step / 2.f;
		const auto outer = 1.f + step / 2.f;
		const auto fixed = [](float value) {
			return quint32(std::max(0., base::SafeRound(value * 65536.)));
		};
		const auto weight = [](float value) {
			return quint16(std::clamp(base::SafeRound(value * 256.), 0., 65535.));
		};
		_geometry.pixels = pixels;
		_geometry.latitude = _latitude;
		_geometry.texture = texture.size();
		_geometry.texels.clear();
		_geometry.texels.reserve(std::size_t(pixels) * pixels * 4 / 5);
		for (auto row = 0; row != pixels; ++row) {
			const auto up = 1.f - (row + .5f) * step;
			for (auto column = 0; column != pixels; ++column) {
				const auto right = (column + .5f) * step - 1.f;
				const auto distanceSquared = right * right + up * up;
				if (distanceSquared >= outer * outer) {
					continue;
				}
				const auto front = std::sqrt(std::max(0.f, 1.f - distanceSquared));
				const auto north = std::clamp(up * cosPitch + front * sinPitch, -1.f, 1.f);
				const auto depth = front * cosPitch - up * sinPitch;
				const auto longitude = .5f + std::atan2(right, depth) / (2.f * kPi);
				auto x = (longitude - std::floor(longitude)) * width - .5f;
				if (x < 0.f) {
					x += width;
				}
				const auto y = std::clamp(
					(.5f - std::asin(north) / kPi) * height - .5f,
					0.f,
					float(height - 1));
				const auto top = int(y);
				const auto sunlight = -.42f * right + .34f * up + .84f * front;
				const auto shade = .36f + .60f * std::max(0.f, sunlight);
				_geometry.texels.push_back({
					.target = quint32(row * pixels + column),
					.top = quint32(top) * textureStride,
					.bottom = quint32(std::min(top + 1, height - 1)) * textureStride,
					.x = std::min(fixed(x), (quint32(width) << 16) - 1),
					.shade = {
						weight(shade * .94f),
						weight(shade * .98f),
						weight(shade * 1.05f),
					},
					.fy = quint8(std::clamp(
						int(base::SafeRound((y - top) * 256.)),
						0,
						255)),
					.alpha = quint8((distanceSquared <= inner * inner)
						? 255
						: std::clamp(int(base::SafeRound(255.f
							* ((1.f - std::sqrt(distanceSquared)) / step + .5f))), 1, 255)),
				});
			}
		}
	}

	[[nodiscard]] static quint32 Mix(quint32 a, quint32 b, quint32 f) {
		const auto g = 256 - f;
		return ((((a & 0xFF00FF) * g + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF)
			| ((((a & 0x00FF00) * g + (b & 0x00FF00) * f) >> 8) & 0x00FF00);
	}

	void render(int pixels) {
		const auto &texture = Texture();
		if (texture.isNull()) {
			return;
		}
		prepareGeometry(pixels, texture);
		if (_frame.width() != pixels
			|| _frame.format() != QImage::Format_ARGB32_Premultiplied) {
			_frame = QImage(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
			if (_frame.isNull()) {
				return;
			}
			_frame.fill(Qt::transparent);
		}
		const auto width = quint32(texture.width());
		const auto wrap = width << 16;
		auto turn = _longitude / 360.;
		turn -= std::floor(turn);
		const auto yaw = std::min(
			quint32(std::max(0., base::SafeRound(turn * wrap))),
			wrap - 1);
		const auto source = reinterpret_cast<const quint32*>(texture.constBits());
		const auto target = reinterpret_cast<QRgb*>(_frame.bits());
		const auto clamp = [](quint32 value, quint32 shade, quint32 offset) {
			return std::min((value * shade >> 8) + offset, quint32(255));
		};
		for (const auto &texel : _geometry.texels) {
			auto x = texel.x + yaw;
			if (x >= wrap) {
				x -= wrap;
			}
			const auto left = x >> 16;
			const auto right = (left + 1 == width) ? 0 : (left + 1);
			const auto fx = (x >> 8) & 0xFF;
			const auto upper = source + texel.top;
			const auto lower = source + texel.bottom;
			const auto color = Mix(
				Mix(upper[left], upper[right], fx),
				Mix(lower[left], lower[right], fx),
				texel.fy);
			const auto r = clamp((color >> 16) & 0xFF, texel.shade[0], 3);
			const auto g = clamp((color >> 8) & 0xFF, texel.shade[1], 4);
			const auto b = clamp(color & 0xFF, texel.shade[2], 8);
			target[texel.target] = (texel.alpha == 255)
				? (0xFF000000u | (r << 16) | (g << 8) | b)
				: qPremultiply(qRgba(int(r), int(g), int(b), texel.alpha));
		}
	}

	Geometry _geometry;
	QImage _frame;
	qreal _longitude = 0.;
	qreal _latitude = 0.;
	bool _dirty = true;

};

} // namespace Ui::Studio
