#pragma once

#include "core/kukagram_appearance.h"
#include "styles/style_basic.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QImage>

namespace Core {

class KukagramSplash final : public Ui::RpWidget {
public:
	explicit KukagramSplash(not_null<Ui::RpWidget*> parent)
	: RpWidget(parent)
	, _logo(QImage(u":/gui/art/kukagram.png"_q)) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		parent->sizeValue(
		) | rpl::on_next([=](QSize size) {
			setGeometry(QRect(QPoint(), size));
		}, lifetime());
		show();
		raise();
		QTimer::singleShot(0, this, [=] { raise(); });
		const auto weak = QPointer<KukagramSplash>(this);
		QTimer::singleShot(kHoldMs, this, [=] {
			if (weak) {
				_fade.start([=] { fadeStep(); }, 1., 0., kFadeMs);
			}
		});
	}

private:
	static constexpr auto kHoldMs = 900;
	static constexpr auto kFadeMs = crl::time(320);

	void fadeStep() {
		raise();
		update();
		if (!_fade.animating()) {
			deleteLater();
		}
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto opacity = _fade.value(_fade.animating() ? 0. : 1.);
		if (opacity <= 0.) {
			return;
		}
		p.setOpacity(opacity);
		p.fillRect(e->rect(), st::windowBg);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = std::min(120, std::min(width(), height()) / 4);
		const auto center = rect().center();
		const auto logo = QRect(
			center.x() - size / 2,
			center.y() - size / 2 - size / 4,
			size,
			size);
		if (!_logo.isNull()) {
			p.drawImage(logo, _logo);
		}
		auto font = st::semiboldFont->f;
		font.setPixelSize(std::max(16, size / 5));
		p.setFont(font);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(0, logo.bottom() + size / 6, width(), size / 3),
			Qt::AlignHCenter | Qt::AlignTop,
			u"kukagram"_q);
	}

	QImage _logo;
	Ui::Animations::Simple _fade;

};

inline void ShowKukagramSplash(Ui::RpWidget *body) {
	if (body && KukagramAppearance::Current().splash) {
		Ui::CreateChild<KukagramSplash>(body);
	}
}

} // namespace Core
