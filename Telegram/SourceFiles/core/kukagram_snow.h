#pragma once

#include "base/random.h"
#include "base/timer.h"
#include "core/kukagram_appearance.h"

#include <QtCore/QEvent>
#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QRegion>
#include <QtWidgets/QWidget>

#include <cmath>
#include <vector>

namespace Core {

class KukagramSnow final : public QWidget {
public:
	explicit KukagramSnow(not_null<QWidget*> window)
	: QWidget(
		window,
		Qt::Tool
			| Qt::FramelessWindowHint
			| Qt::WindowTransparentForInput
			| Qt::WindowDoesNotAcceptFocus
			| Qt::NoDropShadowWindowHint)
	, _window(window.get())
	, _timer([=] { tick(); }) {
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_NoSystemBackground);
		setFocusPolicy(Qt::NoFocus);
		window->installEventFilter(this);
		connect(
			qGuiApp,
			&QGuiApplication::applicationStateChanged,
			this,
			[=] { refresh(); });
		refresh();
	}

	void refresh() {
		const auto window = _window.data();
		const auto enabled = window
			&& KukagramAppearance::Current().snow
			&& window->isVisible()
			&& !window->isMinimized()
			&& (QGuiApplication::applicationState() == Qt::ApplicationActive);
		if (!enabled) {
			_timer.cancel();
			_last = 0;
			if (isVisible()) {
				hide();
			}
			return;
		}
		if (geometry() != window->geometry()) {
			setGeometry(window->geometry());
			_flakes.clear();
		}
		if (!isVisible()) {
			show();
		}
		if (!_timer.isActive()) {
			_last = 0;
			_timer.callEach(kFrame);
		}
	}

protected:
	bool eventFilter(QObject *object, QEvent *e) override {
		if (object == _window.data()) {
			switch (e->type()) {
			case QEvent::Move:
			case QEvent::Resize:
			case QEvent::Show:
			case QEvent::Hide:
			case QEvent::WindowStateChange:
				refresh();
				break;
			default:
				break;
			}
		}
		return QWidget::eventFilter(object, e);
	}

	bool event(QEvent *e) override {
		if (int(e->type()) == KukagramAppearance::ChangeEventType()) {
			refresh();
		}
		return QWidget::event(e);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		for (const auto &flake : _flakes) {
			if (!e->rect().intersects(flakeRect(flake))) {
				continue;
			}
			p.setBrush(QColor(255, 255, 255, flake.alpha));
			p.drawEllipse(flake.position, flake.radius, flake.radius);
		}
	}

private:
	static constexpr auto kFrame = crl::time(55);

	struct Flake {
		QPointF position;
		qreal radius = 0.;
		qreal speed = 0.;
		qreal sway = 0.;
		qreal phase = 0.;
		int alpha = 0;
	};

	[[nodiscard]] static qreal Random() {
		return (base::RandomValue<uint32>() % 10000) / 10000.;
	}

	[[nodiscard]] QRect flakeRect(const Flake &flake) const {
		const auto extent = flake.radius + 1.5;
		return QRectF(
			flake.position - QPointF(extent, extent),
			QSizeF(extent * 2., extent * 2.)).toAlignedRect();
	}

	void spawn(Flake &flake, bool anywhere) {
		flake.radius = 1. + Random() * 2.;
		flake.speed = 18. + Random() * 38. + flake.radius * 6.;
		flake.sway = 6. + Random() * 14.;
		flake.phase = Random() * 6.2832;
		flake.alpha = 110 + int(Random() * 120);
		flake.position = QPointF(
			Random() * width(),
			anywhere ? (Random() * height()) : -flake.radius * 2.);
	}

	void tick() {
		const auto now = crl::now();
		const auto elapsed = _last ? std::min(now - _last, crl::time(200)) : kFrame;
		_last = now;
		const auto count = std::clamp(width() * height() / 20000, 16, 70);
		if (int(_flakes.size()) != count) {
			_flakes.resize(count);
			for (auto &flake : _flakes) {
				spawn(flake, true);
			}
			update();
			return;
		}
		auto region = QRegion();
		const auto seconds = now / 1000.;
		const auto dt = elapsed / 1000.;
		for (auto &flake : _flakes) {
			region += flakeRect(flake);
			flake.position.ry() += flake.speed * dt;
			flake.position.rx() += std::cos(seconds * 1.3 + flake.phase)
				* flake.sway * dt;
			if (flake.position.y() > height() + flake.radius * 2.) {
				spawn(flake, false);
			}
			region += flakeRect(flake);
		}
		update(region);
	}

	QPointer<QWidget> _window;
	base::Timer _timer;
	std::vector<Flake> _flakes;
	crl::time _last = 0;

};

inline void StartKukagramSnow(QWidget *window) {
	if (window) {
		new KukagramSnow(window);
	}
}

} // namespace Core
