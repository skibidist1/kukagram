#pragma once

#include "base/algorithm.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace Ui::Studio {
namespace PlanetTextures {

[[nodiscard]] inline float Hash(int x, int y, int seed) {
	auto h = quint32(x) * 374761393u
		+ quint32(y) * 668265263u
		+ quint32(seed) * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return float(h & 0xFFFFFF) / float(0x1000000);
}

[[nodiscard]] inline float Smooth(float t) {
	return t * t * (3.f - 2.f * t);
}

[[nodiscard]] inline float ValueNoise(float x, float y, int period, int seed) {
	const auto xi = int(std::floor(x));
	const auto yi = int(std::floor(y));
	const auto fx = Smooth(x - xi);
	const auto fy = Smooth(y - yi);
	const auto wrap = [&](int value) {
		const auto result = value % period;
		return (result < 0) ? (result + period) : result;
	};
	const auto x0 = wrap(xi);
	const auto x1 = wrap(xi + 1);
	const auto a = Hash(x0, yi, seed);
	const auto b = Hash(x1, yi, seed);
	const auto c = Hash(x0, yi + 1, seed);
	const auto d = Hash(x1, yi + 1, seed);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

[[nodiscard]] inline float Fbm(
		float u,
		float v,
		int frequencyX,
		int frequencyY,
		int octaves,
		int seed) {
	auto result = 0.f;
	auto amplitude = .5f;
	auto total = 0.f;
	for (auto octave = 0; octave != octaves; ++octave) {
		const auto scale = 1 << octave;
		result += amplitude * ValueNoise(
			u * frequencyX * scale,
			v * frequencyY * scale,
			frequencyX * scale,
			seed + octave * 101);
		total += amplitude;
		amplitude *= .5f;
	}
	return result / total;
}

[[nodiscard]] inline QRgb Mix(QRgb a, QRgb b, float t) {
	t = std::clamp(t, 0.f, 1.f);
	const auto channel = [&](int x, int y) {
		return std::clamp(int(base::SafeRound(x + (y - x) * t)), 0, 255);
	};
	return qRgb(
		channel(qRed(a), qRed(b)),
		channel(qGreen(a), qGreen(b)),
		channel(qBlue(a), qBlue(b)));
}

template <typename Callback>
[[nodiscard]] QImage Generate(Callback &&pixel) {
	constexpr auto kWidth = 1024;
	constexpr auto kHeight = 512;
	auto result = QImage(kWidth, kHeight, QImage::Format_RGB32);
	for (auto y = 0; y != kHeight; ++y) {
		const auto v = (y + .5f) / kHeight;
		const auto polar = std::abs(v - .5f) * 2.f;
		const auto line = reinterpret_cast<QRgb*>(result.scanLine(y));
		for (auto x = 0; x != kWidth; ++x) {
			line[x] = pixel((x + .5f) / kWidth, v, polar);
		}
	}
	return result;
}

[[nodiscard]] inline QImage Earth() {
	return Generate([](float u, float v, float polar) {
		constexpr auto kSea = .53f;
		const auto continent = Fbm(u, v, 6, 3, 6, 11);
		const auto detail = Fbm(u, v, 24, 12, 3, 23);
		const auto cloud = Fbm(u, v, 10, 5, 5, 37);
		auto color = QRgb();
		if (continent > kSea) {
			const auto height = (continent - kSea) / (1.f - kSea);
			const auto dry = Fbm(u, v, 4, 2, 3, 51);
			color = Mix(qRgb(46, 104, 46), qRgb(122, 124, 64), (dry - .4f) * 3.f);
			color = Mix(color, qRgb(132, 106, 74), height * 2.2f - .4f);
			color = Mix(color, qRgb(24, 70, 30), (detail - .5f) * .8f);
			if (polar > .82f - detail * .08f) {
				color = qRgb(236, 240, 246);
			}
		} else {
			const auto depth = (kSea - continent) / kSea;
			color = Mix(qRgb(34, 98, 176), qRgb(8, 30, 92), depth * 3.f);
			if (polar > .9f - detail * .05f) {
				color = qRgb(226, 234, 244);
			}
		}
		return Mix(color, qRgb(250, 251, 255), (cloud - .55f) * 2.6f);
	});
}

[[nodiscard]] inline QImage Mars() {
	return Generate([](float u, float v, float polar) {
		const auto base = Fbm(u, v, 6, 3, 6, 71);
		const auto dark = Fbm(u, v, 4, 2, 4, 83);
		const auto detail = Fbm(u, v, 32, 16, 3, 89);
		auto color = Mix(qRgb(204, 116, 72), qRgb(146, 78, 50), base * 1.6f - .3f);
		color = Mix(color, qRgb(92, 52, 38), (dark - .58f) * 4.f);
		color = Mix(color, qRgb(118, 66, 44), (detail - .5f) * .7f);
		if (polar > .9f - detail * .06f) {
			color = Mix(color, qRgb(236, 228, 220), .9f);
		}
		return color;
	});
}

[[nodiscard]] inline QImage Saturn() {
	return Generate([](float u, float v, float polar) {
		constexpr auto kPi = std::numbers::pi_v<float>;
		const auto swirl = Fbm(u, v, 8, 16, 4, 97);
		const auto fine = std::sin((v + (swirl - .5f) * .03f) * kPi * 28.f);
		const auto broad = std::sin(v * kPi * 7.f + swirl * 1.5f);
		auto color = Mix(qRgb(226, 206, 160), qRgb(200, 168, 118), broad * .5f + .5f);
		color = Mix(color, qRgb(172, 140, 100), fine * .35f + .1f);
		return Mix(color, qRgb(150, 142, 124), (polar - .82f) * 5.f);
	});
}

} // namespace PlanetTextures

class Moon final {
public:
	enum class Planet {
		Moon,
		Earth,
		Mars,
		Saturn,
	};

	void setPlanet(int planet) {
		const auto value = Planet(std::clamp(planet, 0, 3));
		if (_planet != value) {
			_planet = value;
			_geometry = Geometry();
			_dirty = true;
		}
	}

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

	void paint(QPainter &painter, const QRectF &full) {
		if (full.isEmpty() || !available()) {
			return;
		}
		const auto saturn = (_planet == Planet::Saturn);
		auto bounds = full;
		if (saturn) {
			const auto side = full.width() * .42;
			bounds = QRectF(0., 0., side, side);
			bounds.moveCenter(full.center());
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
		painter.setRenderHint(QPainter::Antialiasing);
		if (saturn) {
			paintRings(painter, target, true);
		} else if (_planet == Planet::Earth || _planet == Planet::Mars) {
			paintHalo(painter, target);
		}
		painter.drawImage(target, _frame);
		if (saturn) {
			paintRings(painter, target, false);
		}
		painter.restore();
	}

private:
	void paintHalo(QPainter &painter, const QRectF &sphere) const {
		const auto radius = sphere.width() / 2.;
		const auto outer = radius * 1.1;
		auto color = (_planet == Planet::Earth)
			? QColor(96, 168, 255)
			: QColor(240, 150, 100);
		auto gradient = QRadialGradient(sphere.center(), outer);
		color.setAlpha(0);
		gradient.setColorAt(.86, color);
		color.setAlpha((_planet == Planet::Earth) ? 120 : 60);
		gradient.setColorAt(.9, color);
		color.setAlpha(0);
		gradient.setColorAt(1., color);
		painter.setPen(Qt::NoPen);
		painter.setBrush(gradient);
		painter.drawEllipse(sphere.center(), outer, outer);
	}

	void paintRings(
			QPainter &painter,
			const QRectF &sphere,
			bool back) const {
		struct Band {
			qreal inner = 0.;
			qreal outer = 0.;
			QColor color;
		};
		const auto bands = std::array<Band, 4>{ {
			{ 1.22, 1.48, QColor(170, 150, 120, 70) },
			{ 1.5, 1.94, QColor(222, 204, 166, 200) },
			{ 2.0, 2.22, QColor(200, 182, 148, 160) },
			{ 2.25, 2.3, QColor(190, 176, 150, 70) },
		} };
		constexpr auto kTilt = .3;
		const auto radius = sphere.width() / 2.;
		painter.save();
		painter.translate(sphere.center());
		painter.rotate(-16.);
		const auto limit = radius * 2.4;
		painter.setClipRect(back
			? QRectF(-limit, -limit, limit * 2., limit)
			: QRectF(-limit, 0., limit * 2., limit));
		painter.setPen(Qt::NoPen);
		for (const auto &band : bands) {
			auto path = QPainterPath();
			path.setFillRule(Qt::OddEvenFill);
			path.addEllipse(
				QPointF(),
				radius * band.outer,
				radius * band.outer * kTilt);
			path.addEllipse(
				QPointF(),
				radius * band.inner,
				radius * band.inner * kTilt);
			painter.setBrush(band.color);
			painter.drawPath(path);
		}
		painter.restore();
	}

	[[nodiscard]] std::array<float, 6> tint() const {
		switch (_planet) {
		case Planet::Earth: return { 1.f, 1.f, 1.03f, 2.f, 3.f, 6.f };
		case Planet::Mars: return { 1.03f, .98f, .95f, 3.f, 2.f, 2.f };
		case Planet::Saturn: return { 1.f, .99f, .96f, 3.f, 3.f, 2.f };
		case Planet::Moon: break;
		}
		return { .94f, .98f, 1.05f, 3.f, 4.f, 8.f };
	}

	[[nodiscard]] const QImage &texture() const {
		if (_planet == Planet::Moon) {
			return Texture();
		}
		static auto generated = std::array<QImage, 3>();
		const auto index = int(_planet) - 1;
		if (generated[index].isNull()) {
			generated[index] = (_planet == Planet::Earth)
				? PlanetTextures::Earth()
				: (_planet == Planet::Mars)
				? PlanetTextures::Mars()
				: PlanetTextures::Saturn();
		}
		return generated[index];
	}

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
		const auto gains = tint();
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
						weight(shade * gains[0]),
						weight(shade * gains[1]),
						weight(shade * gains[2]),
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
		const auto &texture = this->texture();
		if (texture.isNull()) {
			return;
		}
		prepareGeometry(pixels, texture);
		const auto shades = tint();
		const auto offsetR = quint32(shades[3]);
		const auto offsetG = quint32(shades[4]);
		const auto offsetB = quint32(shades[5]);
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
			const auto r = clamp((color >> 16) & 0xFF, texel.shade[0], offsetR);
			const auto g = clamp((color >> 8) & 0xFF, texel.shade[1], offsetG);
			const auto b = clamp(color & 0xFF, texel.shade[2], offsetB);
			target[texel.target] = (texel.alpha == 255)
				? (0xFF000000u | (r << 16) | (g << 8) | b)
				: qPremultiply(qRgba(int(r), int(g), int(b), texel.alpha));
		}
	}

	Geometry _geometry;
	QImage _frame;
	Planet _planet = Planet::Moon;
	qreal _longitude = 0.;
	qreal _latitude = 0.;
	bool _dirty = true;

};

} // namespace Ui::Studio
