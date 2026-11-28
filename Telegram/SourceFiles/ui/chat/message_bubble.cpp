/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/chat/message_bubble.h"

#include "core/kukagram_appearance.h"
#include "ui/chat/studio_paint.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/torn_edge.h"
#include "ui/image/image_prepare.h"
#include "ui/cached_round_corners.h"
#include "styles/style_chat.h"
#include "styles/style_chat_style.h"

#include <QtGui/QPainterPath>

namespace Ui {
namespace {

using Corner = BubbleCornerRounding;

[[nodiscard]] bool UsePatternBubble(const SimpleBubble &args) {
	return !args.selected
		&& args.outbg
		&& args.pattern
		&& !args.patternViewport.isEmpty()
		&& !args.pattern->pixmap.size().isEmpty();
}

[[nodiscard]] bool UseRoundedText(const SimpleBubble &args) {
	return args.allowRoundedText
		&& Core::KukagramAppearance::Current().bubbles;
}

[[nodiscard]] const QPixmap &PatternPixmap(const SimpleBubble &args) {
	const auto pattern = args.pattern;
	if (!Core::KukagramAppearance::Current().bubbles) {
		return pattern->pixmap;
	}
	const auto key = pattern->pixmap.cacheKey();
	if (pattern->kukagramPixmapSourceKey != key
		|| pattern->kukagramPixmap.isNull()) {
		auto tinted = pattern->pixmap.copy();
		if (tinted.isNull()) {
			return pattern->pixmap;
		}
		auto p = QPainter(&tinted);
		if (!p.isActive()) {
			return pattern->pixmap;
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
		p.fillRect(
			QRect(QPoint(), pattern->pixmap.deviceIndependentSize().toSize()),
			QColor(155, 223, 191, 24));
		if (!p.end()) {
			return pattern->pixmap;
		}
		pattern->kukagramPixmap = std::move(tinted);
		pattern->kukagramPixmapSourceKey = key;
	}
	return pattern->kukagramPixmap;
}

[[nodiscard]] float64 PatternBubbleOpacity(
		const SimpleBubble &args,
		float64 wasOpacity) {
	return args.st->msgOutBg()->c.alphaF() * wasOpacity;
}

void PaintBubblePiece(
		QPainter &p,
		const SimpleBubble &args,
		QRect geometry,
		bool fromTop,
		bool tillBottom) {
	auto simple = args;
	simple.geometry = geometry;
	if (!fromTop) {
		simple.rounding.topLeft
			= simple.rounding.topRight
			= Corner::None;
	}
	if (!tillBottom) {
		simple.rounding.bottomLeft
			= simple.rounding.bottomRight
			= Corner::None;
		simple.shadowed = false;
	}
	PaintBubble(p, simple);
}

void PaintBubblePiece(
		QPainter &p,
		const SimpleBubble &args,
		QRect geometry,
		bool selected,
		bool fromTop,
		bool tillBottom) {
	auto simple = args;
	simple.selected = selected;
	PaintBubblePiece(p, simple, geometry, fromTop, tillBottom);
}

template <
	typename FillBg, // fillBg(QRect rect)
	typename FillSh, // fillSh(QRect rect)
	typename FillCorner, // fillCorner(int x, int y, int index, Corner size)
	typename PaintTail> // paintTail(QPoint bottomPosition) -> tailWidth
void PaintBubbleGeneric(
		const SimpleBubble &args,
		FillBg &&fillBg,
		FillSh &&fillSh,
		FillCorner &&fillCorner,
		PaintTail &&paintTail) {
	using namespace Images;

	const auto topLeft = args.rounding.topLeft;
	const auto topRight = args.rounding.topRight;
	const auto bottomWithTailLeft = args.rounding.bottomLeft;
	const auto bottomWithTailRight = args.rounding.bottomRight;
	if (topLeft == Corner::None
		&& topRight == Corner::None
		&& bottomWithTailLeft == Corner::None
		&& bottomWithTailRight == Corner::None) {
		fillBg(args.geometry);
		return;
	}
	const auto bottomLeft = (bottomWithTailLeft == Corner::Tail)
		? Corner::None
		: bottomWithTailLeft;
	const auto bottomRight = (bottomWithTailRight == Corner::Tail)
		? Corner::None
		: bottomWithTailRight;
	const auto rect = args.geometry;
	const auto small = BubbleRadiusSmall();
	const auto large = UseRoundedText(args)
		? KukagramTextBubbleRadiusLarge()
		: BubbleRadiusLarge();
	const auto cornerSize = [&](Corner corner) {
		return (corner == Corner::Large)
			? large
			: (corner == Corner::Small)
			? small
			: 0;
	};
	const auto verticalSkip = [&](Corner left, Corner right) {
		return std::max(cornerSize(left), cornerSize(right));
	};
	const auto top = verticalSkip(topLeft, topRight);
	const auto bottom = verticalSkip(bottomLeft, bottomRight);
	if (top) {
		const auto left = cornerSize(topLeft);
		const auto right = cornerSize(topRight);
		if (left) {
			fillCorner(rect.left(), rect.top(), kTopLeft, topLeft);
			if (const auto add = top - left) {
				fillBg({ rect.left(), rect.top() + left, left, add });
			}
		}
		if (const auto fill = rect.width() - left - right; fill > 0) {
			fillBg({ rect.left() + left, rect.top(), fill, top });
		}
		if (right) {
			fillCorner(
				rect.left() + rect.width() - right,
				rect.top(),
				kTopRight,
				topRight);
			if (const auto add = top - right) {
				fillBg({
					rect.left() + rect.width() - right,
					rect.top() + right,
					right,
					add,
				});
			}
		}
	}
	if (const auto fill = rect.height() - top - bottom; fill > 0) {
		fillBg({ rect.left(), rect.top() + top, rect.width(), fill });
	}
	if (bottom) {
		const auto left = cornerSize(bottomLeft);
		const auto right = cornerSize(bottomRight);
		if (left) {
			fillCorner(
				rect.left(),
				rect.top() + rect.height() - left,
				kBottomLeft,
				bottomLeft);
			if (const auto add = bottom - left) {
				fillBg({
					rect.left(),
					rect.top() + rect.height() - bottom,
					left,
					add,
				});
			}
		}
		if (const auto fill = rect.width() - left - right; fill > 0) {
			fillBg({
				rect.left() + left,
				rect.top() + rect.height() - bottom,
				fill,
				bottom,
			});
		}
		if (right) {
			fillCorner(
				rect.left() + rect.width() - right,
				rect.top() + rect.height() - right,
				kBottomRight,
				bottomRight);
			if (const auto add = bottom - right) {
				fillBg({
					rect.left() + rect.width() - right,
					rect.top() + rect.height() - bottom,
					right,
					add,
				});
			}
		}
	}
	const auto leftTail = (bottomWithTailLeft == Corner::Tail)
		? paintTail({ rect.x(), rect.y() + rect.height() })
		: 0;
	const auto rightTail = (bottomWithTailRight == Corner::Tail)
		? paintTail({ rect.x() + rect.width(), rect.y() + rect.height() })
		: 0;
	if (!args.shadowed) {
		return;
	}
	const auto shLeft = rect.x() + cornerSize(bottomLeft) - leftTail;
	const auto shWidth = rect.x()
		+ rect.width()
		- cornerSize(bottomRight)
		+ rightTail
		- shLeft;
	if (shWidth > 0) {
		fillSh({ shLeft, rect.y() + rect.height(), shWidth, st::msgShadow });
	}
}

void PaintPatternBubble(QPainter &p, const SimpleBubble &args) {
	const auto wasOpacity = p.opacity();
	const auto opacity = PatternBubbleOpacity(args, wasOpacity);
	const auto shadowOpacity = opacity * args.st->msgOutShadow()->c.alphaF();
	const auto pattern = args.pattern;
	const auto &tail = (args.rounding.bottomRight == Corner::Tail)
		? pattern->tailRight
		: pattern->tailLeft;
	const auto tailShift = (args.rounding.bottomRight == Corner::Tail
		? QPoint(0, tail.height())
		: QPoint(tail.width(), tail.height())) / int(tail.devicePixelRatio());
	const auto fillBg = [&](const QRect &rect) {
		const auto fill = rect.intersected(args.patternViewport);
		if (!fill.isEmpty()) {
			PaintPatternBubblePart(
				p,
				args.patternViewport,
				PatternPixmap(args),
				fill);
		}
	};
	const auto fillSh = [&](const QRect &rect) {
		p.setOpacity(shadowOpacity);
		fillBg(rect);
		p.setOpacity(opacity);
	};
	const auto fillPattern = [&](
			int x,
			int y,
			const QImage &mask,
			QImage &cache) {
		PaintPatternBubblePart(
			p,
			args.patternViewport,
			PatternPixmap(args),
			QRect(QPoint(x, y), mask.size() / int(mask.devicePixelRatio())),
			mask,
			cache);
	};
	const auto fillCorner = [&](int x, int y, int index, Corner size) {
		const auto text = (size == Corner::Large) && UseRoundedText(args);
		const auto &corner = text
			? pattern->cornersLargeText[index]
			: (size == Corner::Large)
			? pattern->cornersLarge[index]
			: pattern->cornersSmall[index];
		auto &cache = text
			? (index < 2
				? pattern->cornerTopTextCache
				: pattern->cornerBottomTextCache)
			: (size == Corner::Large)
			? (index < 2
				? pattern->cornerTopLargeCache
				: pattern->cornerBottomLargeCache)
			: (index < 2
				? pattern->cornerTopSmallCache
				: pattern->cornerBottomSmallCache);
		fillPattern(x, y, corner, cache);
	};
	const auto paintTail = [&](QPoint bottomPosition) {
		const auto position = bottomPosition - tailShift;
		fillPattern(position.x(), position.y(), tail, pattern->tailCache);
		return tail.width() / int(tail.devicePixelRatio());
	};

	p.setOpacity(opacity);
	PaintBubbleGeneric(args, fillBg, fillSh, fillCorner, paintTail);
	p.setOpacity(wasOpacity);
}

void PaintSolidBubble(QPainter &p, const SimpleBubble &args) {
	const auto &st = args.st->messageStyle(args.outbg, args.selected);
	const auto soft = Core::KukagramAppearance::Current().bubbles
		&& !args.selected;
	const auto roundedText = UseRoundedText(args);
	const auto &bg = soft ? st.kukagramBg->color() : st.msgBg;
	const auto &tail = (args.rounding.bottomRight == Corner::Tail)
		? st.tailRight
		: st.tailLeft;
	const auto tailShift = (args.rounding.bottomRight == Corner::Tail)
		? QPoint(0, tail.height())
		: QPoint(tail.width(), tail.height());

	PaintBubbleGeneric(args, [&](const QRect &rect) {
		p.fillRect(rect, bg);
	}, [&](const QRect &rect) {
		if (args.rounding.bottomRight != Corner::None) {
			p.fillRect(rect, st.msgShadow);
		}
	}, [&](int x, int y, int index, Corner size) {
		const auto &corners = st.solidBubbleCorners(
			size == Corner::Large, roundedText, soft);
		p.drawPixmap(x, y, corners.p[index]);
	}, [&](const QPoint &bottomPosition) {
		if (soft) {
			tail.paint(p, bottomPosition - tailShift, args.outerWidth, bg->c);
		} else {
			tail.paint(p, bottomPosition - tailShift, args.outerWidth);
		}
		return tail.width();
	});
}

[[nodiscard]] QPainterPath StudioBubblePath(
		const QRect &rect,
		int topLeft,
		int topRight,
		int bottomLeft,
		int bottomRight) {
	const auto x = rect.x();
	const auto y = rect.y();
	const auto right = x + rect.width();
	const auto bottom = y + rect.height();
	auto path = QPainterPath();
	path.moveTo(x + topLeft, y);
	path.lineTo(right - topRight, y);
	if (topRight) {
		path.arcTo(
			right - 2 * topRight, y, 2 * topRight, 2 * topRight, 90, -90);
	}
	path.lineTo(right, bottom - bottomRight);
	if (bottomRight) {
		path.arcTo(
			right - 2 * bottomRight,
			bottom - 2 * bottomRight,
			2 * bottomRight,
			2 * bottomRight,
			0,
			-90);
	}
	path.lineTo(x + bottomLeft, bottom);
	if (bottomLeft) {
		path.arcTo(
			x, bottom - 2 * bottomLeft, 2 * bottomLeft, 2 * bottomLeft, 270, -90);
	}
	path.lineTo(x, y + topLeft);
	if (topLeft) {
		path.arcTo(x, y, 2 * topLeft, 2 * topLeft, 180, -90);
	}
	path.closeSubpath();
	return path;
}

void PaintStudioEdge(
		QPainter &p,
		const SimpleBubble &args,
		int topLeft,
		int topRight) {
	const auto left = args.geometry.x() + topLeft + 2 * st::lineWidth;
	const auto right = args.geometry.x() + args.geometry.width()
		- topRight - 2 * st::lineWidth;
	if ((!topLeft && !topRight) || right <= left) {
		return;
	}
	auto edge = QLinearGradient(left, 0, right, 0);
	edge.setColorAt(0., Qt::transparent);
	edge.setColorAt(args.outbg ? 0.7 : 0.3,
		args.outbg ? Studio::Violet(182) : Studio::Mint(176));
	edge.setColorAt(1., Qt::transparent);
	p.fillRect(QRect(left, args.geometry.y(), right - left, st::lineWidth), edge);
}

[[nodiscard]] bool PaintStudioBubble(QPainter &p, const SimpleBubble &args) {
	if (args.selected
		|| !args.allowStudioText
		|| !Core::KukagramAppearance::Current().studio
		|| args.geometry.isEmpty()) {
		return false;
	}
	const auto &style = args.st->messageStyle(args.outbg, false);
	if (!style.studioBg) {
		return false;
	}
	const auto &[start, end] = *style.studioBg;
	const auto rect = args.geometry;
	const auto rounding = args.rounding;
	const auto rounded = UseRoundedText(args);
	const auto radius = [&](Corner corner) {
		const auto max = std::min(rect.width(), rect.height()) / 2;
		return (rounded && corner == Corner::Large)
			? std::min(max, KukagramTextBubbleRadiusLarge())
			: std::min(max, BubbleCornerRadius(corner));
	};
	const auto topLeft = radius(rounding.topLeft);
	const auto topRight = radius(rounding.topRight);
	const auto path = StudioBubblePath(
		rect,
		topLeft,
		topRight,
		radius(rounding.bottomLeft),
		radius(rounding.bottomRight));
	const auto left = rect.x();
	const auto right = left + rect.width();
	const auto bottom = rect.y() + rect.height();
	auto fill = QLinearGradient(left, 0, right, 0);
	fill.setColorAt(0., start);
	fill.setColorAt(1., end);
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	if (args.shadowed && rounding.bottomRight != Corner::None) {
		p.translate(0, st::msgShadow);
		p.fillPath(path, style.msgShadow->b);
		p.translate(0, -st::msgShadow);
	}
	const auto glow = Core::KukagramAppearance::Current().glowBubbles;
	if (glow) {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(args.outbg ? Studio::Violet(44) : Studio::Mint(24), 3.));
		p.drawPath(path);
	}
	p.fillPath(path, fill);
	if (glow) {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(args.outbg ? Studio::Violet(120) : Studio::Mint(64), 1.));
		p.drawPath(path);
	}
	PaintStudioEdge(p, args, topLeft, topRight);
	if (rounding.bottomLeft == Corner::Tail) {
		const auto &tail = style.tailLeft;
		tail.paint(
			p,
			QPoint(left - tail.width(), bottom - tail.height()),
			args.outerWidth,
			start);
	}
	if (rounding.bottomRight == Corner::Tail) {
		const auto &tail = style.tailRight;
		tail.paint(
			p,
			QPoint(right, bottom - tail.height()),
			args.outerWidth,
			end);
	}
	p.restore();
	return true;
}

} // namespace

std::unique_ptr<BubblePattern> PrepareBubblePattern(
		not_null<const style::palette*> st) {
	auto result = std::make_unique<Ui::BubblePattern>();
	result->cornersSmall = Images::CornersMask(BubbleRadiusSmall());
	result->cornersLarge = Images::CornersMask(BubbleRadiusLarge());
	result->cornersLargeText = Images::CornersMask(
		KukagramTextBubbleRadiusLarge());
	const auto addShadow = [&](QImage &bottomCorner) {
		auto result = QImage(
			bottomCorner.width(),
			(bottomCorner.height()
				+ st::msgShadow * int(bottomCorner.devicePixelRatio())),
			QImage::Format_ARGB32_Premultiplied);
		result.fill(Qt::transparent);
		result.setDevicePixelRatio(bottomCorner.devicePixelRatio());
		auto p = QPainter(&result);
		p.setOpacity(st->msgInShadow()->c.alphaF());
		p.drawImage(0, st::msgShadow, bottomCorner);
		p.setOpacity(1.);
		p.drawImage(0, 0, bottomCorner);
		p.end();

		bottomCorner = std::move(result);
	};
	addShadow(result->cornersSmall[2]);
	addShadow(result->cornersSmall[3]);
	result->cornerTopSmallCache = QImage(
		result->cornersSmall[0].size(),
		QImage::Format_ARGB32_Premultiplied);
	result->cornerTopLargeCache = QImage(
		result->cornersLarge[0].size(),
		QImage::Format_ARGB32_Premultiplied);
	result->cornerBottomSmallCache = QImage(
		result->cornersSmall[2].size(),
		QImage::Format_ARGB32_Premultiplied);
	result->cornerBottomLargeCache = QImage(
		result->cornersLarge[2].size(),
		QImage::Format_ARGB32_Premultiplied);
	return result;
}

void FinishBubblePatternOnMain(not_null<BubblePattern*> pattern) {
	pattern->tailLeft = st::historyBubbleTailOutLeft.instance(Qt::white);
	pattern->tailRight = st::historyBubbleTailOutRight.instance(Qt::white);
	pattern->tailCache = QImage(
		pattern->tailLeft.size(),
		QImage::Format_ARGB32_Premultiplied);
}

void PaintBubble(QPainter &p, const SimpleBubble &args) {
	if (UsePatternBubble(args)) {
		PaintPatternBubble(p, args);
		if (args.allowStudioText && Core::KukagramAppearance::Current().studio) {
			const auto rounded = UseRoundedText(args);
			const auto radius = [&](Corner corner) {
				return (rounded && corner == Corner::Large)
					? KukagramTextBubbleRadiusLarge()
					: BubbleCornerRadius(corner);
			};
			PaintStudioEdge(p, args,
				radius(args.rounding.topLeft),
				radius(args.rounding.topRight));
		}
	} else if (!PaintStudioBubble(p, args)) {
		PaintSolidBubble(p, args);
	}
}

void PaintBubble(QPainter &p, const ComplexBubble &args) {
	if (args.selection.empty()) {
		PaintBubble(p, args.simple);
		return;
	}
	const auto rect = args.simple.geometry;
	const auto left = rect.x();
	const auto width = rect.width();
	const auto top = rect.y();
	const auto bottom = top + rect.height();
	auto from = top;
	for (const auto &selected : args.selection) {
		if (selected.top > from) {
			PaintBubblePiece(
				p,
				args.simple,
				QRect(left, from, width, selected.top - from),
				false,
				(from <= top),
				false);
		}
		PaintBubblePiece(
			p,
			args.simple,
			QRect(left, selected.top, width, selected.height),
			true,
			(selected.top <= top),
			(selected.top + selected.height >= bottom));
		from = selected.top + selected.height;
	}
	if (from < bottom) {
		PaintBubblePiece(
			p,
			args.simple,
			QRect(left, from, width, bottom - from),
			false,
			false,
			true);
	}
}

void PaintBubble(QPainter &p, const BubbleWithGaps &args) {
	if (args.gaps.empty()) {
		PaintBubble(p, args.simple);
		return;
	}
	const auto rect = args.simple.geometry;
	const auto left = rect.x();
	const auto width = rect.width();
	const auto top = rect.y();
	const auto bottom = top + rect.height();
	const auto pattern = UsePatternBubble(args.simple);
	const auto paintStrip = [&](
			const QImage &mask,
			QImage &patternCache,
			QImage &solidCache,
			QColor &solidColor,
			int y) {
		const auto target = QRect(
			left,
			y,
			width,
			mask.height() / int(mask.devicePixelRatio()));
		if (pattern) {
			const auto wasOpacity = p.opacity();
			p.setOpacity(PatternBubbleOpacity(args.simple, wasOpacity));
			PaintPatternBubblePart(
				p,
				args.simple.patternViewport,
				PatternPixmap(args.simple),
				target,
				mask,
				patternCache);
			p.setOpacity(wasOpacity);
		} else {
			const auto &st = args.simple.st->messageStyle(
				args.simple.outbg,
				args.simple.selected);
			const auto color = (Core::KukagramAppearance::Current().bubbles
				&& !args.simple.selected)
				? st.kukagramBg->color()->c
				: st.msgBg->c;
			if (solidCache.size() != mask.size() || solidColor != color) {
				if (solidCache.size() != mask.size()) {
					solidCache = QImage(
						mask.size(),
						QImage::Format_ARGB32_Premultiplied);
					solidCache.setDevicePixelRatio(mask.devicePixelRatio());
				}
				style::colorizeImage(mask, color, &solidCache);
				solidColor = color;
			}
			p.drawImage(target, solidCache);
		}
	};
	const auto torn = args.torn;
	const auto stripHeight = torn->maskTop.height()
		/ int(torn->maskTop.devicePixelRatio());
	auto from = top;
	for (const auto &gap : args.gaps) {
		const auto gapTop = gap.top;
		const auto gapBottom = gap.top + gap.height;
		if (gapTop > from) {
			PaintBubblePiece(
				p,
				args.simple,
				QRect(left, from, width, gapTop - from),
				(from <= top),
				false);
			paintStrip(
				torn->maskBottom,
				torn->patternCacheBottom,
				torn->solidCacheBottom,
				torn->solidColorBottom,
				gapTop);
		}
		if (gapBottom < bottom) {
			paintStrip(
				torn->maskTop,
				torn->patternCacheTop,
				torn->solidCacheTop,
				torn->solidColorTop,
				gapBottom - stripHeight);
		}
		from = gapBottom;
	}
	if (from < bottom) {
		PaintBubblePiece(
			p,
			args.simple,
			QRect(left, from, width, bottom - from),
			false,
			true);
	}
}

void PaintPatternBubblePart(
		QPainter &p,
		const QRect &viewport,
		const QPixmap &pixmap,
		const QRect &target) {
	const auto factor = pixmap.devicePixelRatio();
	if (viewport.size() * factor == pixmap.size()) {
		const auto fill = target.intersected(viewport);
		if (fill.isEmpty()) {
			return;
		}
		p.drawPixmap(fill, pixmap, QRect(
			(fill.topLeft() - viewport.topLeft()) * factor,
			fill.size() * factor));
	} else {
		const auto to = viewport;
		const auto from = QRect(QPoint(), pixmap.size());
		const auto deviceRect = QRect(
			QPoint(),
			QSize(p.device()->width(), p.device()->height()));
		const auto clip = (target != deviceRect);
		if (clip) {
			p.setClipRect(target);
		}
		p.drawPixmap(to, pixmap, from);
		if (clip) {
			p.setClipping(false);
		}
	}
}

void PaintPatternBubblePart(
		QPainter &p,
		const QRect &viewport,
		const QPixmap &pixmap,
		const QRect &target,
		const QImage &mask,
		QImage &cache) {
	Expects(mask.bytesPerLine() == mask.width() * 4);
	Expects(mask.format() == QImage::Format_ARGB32_Premultiplied);

	if (cache.size() != mask.size()) {
		cache = QImage(
			mask.size(),
			QImage::Format_ARGB32_Premultiplied);
	}
	cache.setDevicePixelRatio(mask.devicePixelRatio());
	Assert(cache.bytesPerLine() == cache.width() * 4);
	memcpy(cache.bits(), mask.constBits(), mask.sizeInBytes());

	auto q = QPainter(&cache);
	q.setCompositionMode(QPainter::CompositionMode_SourceIn);
	PaintPatternBubblePart(
		q,
		viewport.translated(-target.topLeft()),
		pixmap,
		QRect(QPoint(), cache.size() / int(cache.devicePixelRatio())));
	q.end();

	p.drawImage(target, cache);
}

void PaintPatternBubblePart(
		QPainter &p,
		const QRect &viewport,
		const QPixmap &pixmap,
		const QRect &target,
		Fn<void(QPainter&)> paintContent,
		QImage &cache) {
	Expects(paintContent != nullptr);

	const auto targetOrigin = target.topLeft();
	const auto targetSize = target.size();
	if (cache.size() != targetSize * style::DevicePixelRatio()) {
		cache = QImage(
			target.size() * style::DevicePixelRatio(),
			QImage::Format_ARGB32_Premultiplied);
		cache.setDevicePixelRatio(style::DevicePixelRatio());
	}
	cache.fill(Qt::transparent);
	auto q = QPainter(&cache);
	q.translate(-targetOrigin);
	paintContent(q);
	q.translate(targetOrigin);
	q.setCompositionMode(QPainter::CompositionMode_SourceIn);
	PaintPatternBubblePart(
		q,
		viewport.translated(-targetOrigin),
		pixmap,
		QRect(QPoint(), targetSize));
	q.end();

	p.drawImage(target, cache);
}

} // namespace Ui
