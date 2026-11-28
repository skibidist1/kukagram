#pragma once

#include "core/kukagram_appearance.h"
#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>

namespace Core {

class KukagramSwitchFade final : public Ui::RpWidget {
public:
	explicit KukagramSwitchFade(not_null<Ui::RpWidget*> body)
	: RpWidget(body)
	, _snapshot(Ui::GrabWidget(body)) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		body->sizeValue(
		) | rpl::on_next([=](QSize size) {
			setGeometry(QRect(QPoint(), size));
		}, lifetime());
		show();
		raise();
		QTimer::singleShot(kHoldMs, this, [=] {
			_fade.start([=] {
				raise();
				update();
				if (!_fade.animating()) {
					deleteLater();
				}
			}, 1., 0., kFadeMs);
		});
	}

private:
	static constexpr auto kHoldMs = 90;
	static constexpr auto kFadeMs = crl::time(260);

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setOpacity(_fade.value(_fade.animating() ? 0. : 1.));
		p.drawPixmap(0, 0, _snapshot);
	}

	QPixmap _snapshot;
	Ui::Animations::Simple _fade;

};

inline void ShowKukagramSwitchFade(Ui::RpWidget *body) {
	static auto current = QPointer<KukagramSwitchFade>();
	if (!body || !KukagramAppearance::Current().switchFade) {
		return;
	}
	if (current) {
		delete current.data();
	}
	current = Ui::CreateChild<KukagramSwitchFade>(body);
}

} // namespace Core
