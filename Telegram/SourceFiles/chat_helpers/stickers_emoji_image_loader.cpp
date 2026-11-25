/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/stickers_emoji_image_loader.h"

#include "styles/style_chat.h"
#include "ui/emoji_source_cache.h"

#include <QtCore/QtMath>

namespace Stickers {
namespace {

// Application owns the only loader. All cache access runs on its queue.
auto SourceSprites = Ui::Emoji::SourceSpriteCache();

} // namespace

EmojiImageLoader::EmojiImageLoader(crl::weak_on_queue<EmojiImageLoader> weak)
: _weak(std::move(weak)) {
}

QImage EmojiImageLoader::prepare(EmojiPtr emoji) const {
	const auto factor = style::DevicePixelRatio();
	const auto rendered = SourceSprites.render(
		_images->id(),
		emoji,
		st::largeEmojiSize * factor);
	const auto side = st::largeEmojiSize + 2 * st::largeEmojiOutline;
	auto tinted = QImage(
		QSize(st::largeEmojiSize, st::largeEmojiSize) * factor,
		QImage::Format_ARGB32_Premultiplied);
	tinted.fill(Qt::white);
	if (!rendered.isNull()) {
		QPainter p(&tinted);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(0, 0, rendered);
	}
	auto result = QImage(
		QSize(side, side) * factor,
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	if (!rendered.isNull()) {
		QPainter p(&result);
		const auto delta = st::largeEmojiOutline * factor;
		const auto planar = std::array<QPoint, 4>{ {
			{ 0, -1 },
			{ -1, 0 },
			{ 1, 0 },
			{ 0, 1 },
		} };
		for (const auto &shift : planar) {
			for (auto i = 0; i != delta; ++i) {
				p.drawImage(QPoint(delta, delta) + shift * (i + 1), tinted);
			}
		}
		const auto diagonal = std::array<QPoint, 4>{ {
			{ -1, -1 },
			{ 1, -1 },
			{ -1, 1 },
			{ 1, 1 },
		} };
		const auto corrected = int(base::SafeRound(delta / M_SQRT2));
		for (const auto &shift : diagonal) {
			for (auto i = 0; i != corrected; ++i) {
				p.drawImage(QPoint(delta, delta) + shift * (i + 1), tinted);
			}
		}
		p.drawImage(delta, delta, rendered);
	}
	return result;
}

QImage EmojiImageLoader::prepareWithoutOutline(EmojiPtr emoji) const {
	return SourceSprites.render(
		_images->id(),
		emoji,
		st::largeEmojiSize * style::DevicePixelRatio());
}

void EmojiImageLoader::switchTo(std::shared_ptr<UniversalImages> images) {
	Expects(images != nullptr);
	SourceSprites = {};
	_images = std::move(images);
}

auto EmojiImageLoader::releaseImages() -> std::shared_ptr<UniversalImages> {
	SourceSprites = {};
	return std::exchange(
		_images,
		std::make_shared<UniversalImages>(_images->id()));
}

} // namespace Stickers
