/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/mac/tray_mac.h"

#include "base/platform/mac/base_utilities_mac.h"
#include "core/application.h"
#include "core/sandbox.h"
#include "core/version.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "ui/painter.h"
#include "ui/dynamic_image.h"
#include "styles/style_window.h"

#include <QtWidgets/QMenu>
#include <QtGui/QIcon>
#include <QtGui/QPainterPath>
#include <QtCore/QTimer>

#import <AppKit/NSMenu.h>
#import <AppKit/NSStatusItem.h>

@interface CommonDelegate : NSObject<NSMenuDelegate> {
}

- (void) menuDidClose:(NSMenu *)menu;
- (void) menuWillOpen:(NSMenu *)menu;
- (void) observeValueForKeyPath:(NSString *)keyPath
	ofObject:(id)object
	change:(NSDictionary<NSKeyValueChangeKey, id> *)change
	context:(void *)context;

- (rpl::producer<>) closes;
- (rpl::producer<>) aboutToShowRequests;
- (rpl::producer<>) appearanceChanges;

@end // @interface CommonDelegate

@implementation CommonDelegate {
	rpl::event_stream<> _closes;
	rpl::event_stream<> _aboutToShowRequests;
	rpl::event_stream<> _appearanceChanges;
}

- (void) menuDidClose:(NSMenu *)menu {
	Core::Sandbox::Instance().customEnterFromEventLoop([&] {
		_closes.fire({});
	});
}

- (void) menuWillOpen:(NSMenu *)menu {
	Core::Sandbox::Instance().customEnterFromEventLoop([&] {
		_aboutToShowRequests.fire({});
	});
}

// Thanks https://stackoverflow.com/a/64525038
- (void) observeValueForKeyPath:(NSString *)keyPath
		ofObject:(id)object
		change:(NSDictionary<NSKeyValueChangeKey, id> *)change
		context:(void *)context {
	if ([keyPath isEqualToString:@"button.effectiveAppearance"]) {
		_appearanceChanges.fire({});
	}
}

- (rpl::producer<>) closes {
	return _closes.events();
}

- (rpl::producer<>) aboutToShowRequests {
	return _aboutToShowRequests.events();
}

- (rpl::producer<>) appearanceChanges {
	return _appearanceChanges.events();
}

@end // @implementation MenuDelegate

namespace Platform {

namespace {

[[nodiscard]] bool IsAnyActiveForTrayMenu() {
	for (const NSWindow *w in [[NSApplication sharedApplication] windows]) {
		if (w.isKeyWindow) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QImage TrayIconBack(bool darkMode) {
	constexpr auto kSide = 22;
	constexpr auto kStroke = 3.;
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		QSize(kSide * ratio, kSide * ratio),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	result.setDevicePixelRatio(ratio);
	{
		Painter p(&result);
		PainterHighQualityEnabler hq(p);
		p.setPen(QPen(
			darkMode ? QColor(255, 255, 255) : QColor(0, 0, 0, 180),
			kStroke,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		auto letter = QPainterPath();
		letter.moveTo(6., 3.5);
		letter.lineTo(6., 18.5);
		letter.moveTo(16., 3.5);
		letter.lineTo(6., 12.);
		letter.moveTo(10., 9.);
		letter.lineTo(16.5, 18.5);
		p.drawPath(letter);
	}
	return result;
}

void PlaceCounter(
		QImage &img,
		int size,
		int count,
		style::color bg,
		style::color color) {
	if (!count) {
		return;
	}
	const auto savedRatio = img.devicePixelRatio();
	img.setDevicePixelRatio(1.);

	{
		Painter p(&img);
		PainterHighQualityEnabler hq(p);

		const auto cnt = (count < 100)
			? QString("%1").arg(count)
			: QString("..%1").arg(count % 100, 2, 10, QChar('0'));
		const auto cntSize = cnt.size();

		p.setBrush(bg);
		p.setPen(Qt::NoPen);
		int32 fontSize, skip;
		if (size == 22) {
			skip = 1;
			fontSize = 8;
		} else {
			skip = 2;
			fontSize = 16;
		}
		style::font f(fontSize, 0, 0);
		int32 w = f->width(cnt), d, r;
		if (size == 22) {
			d = (cntSize < 2) ? 3 : 2;
			r = (cntSize < 2) ? 6 : 5;
		} else {
			d = (cntSize < 2) ? 6 : 5;
			r = (cntSize < 2) ? 9 : 11;
		}
		p.drawRoundedRect(
			QRect(
				size - w - d * 2 - skip,
				size - f->height - skip,
				w + d * 2,
				f->height),
			r,
			r);

		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.setFont(f);
		p.setPen(color);
		p.drawText(
			size - w - d - skip,
			size - f->height + f->ascent - skip,
			cnt);
	}
	img.setDevicePixelRatio(savedRatio);
}

void UpdateIcon(const NSStatusItem *status) {
	if (!status) {
		return;
	}

	const auto appearance = status.button.effectiveAppearance;
	const auto darkMode = [[appearance.name lowercaseString]
		containsString:@"dark"];

	// The recommended maximum title bar icon height is 18 points
	// (device independent pixels). The menu height on past and
	// current OS X versions is 22 points. Provide some future-proofing
	// by deriving the icon height from the menu height.
	const int padding = 0;
	const int menuHeight = NSStatusBar.systemStatusBar.thickness;
	// [[status.button window] backingScaleFactor];
	const int maxImageHeight = (menuHeight - padding)
		* style::DevicePixelRatio();

	// Select pixmap based on the device pixel height. Ideally we would use
	// the devicePixelRatio of the target screen, but that value is not
	// known until draw time. Use qApp->devicePixelRatio, which returns the
	// devicePixelRatio for the "best" screen on the system.

	const auto side = 22 * style::DevicePixelRatio();
	const auto selectedSize = QSize(side, side);

	auto result = TrayIconBack(darkMode);
	auto resultActive = result;
	resultActive.detach();

	const auto counter = 0; // kukagram: keep the menu bar K free of unread badges.
	const auto muted = Core::App().unreadBadgeMuted();

	const auto &bg = (muted ? st::trayCounterBgMute : st::trayCounterBg);
	const auto &fg = st::trayCounterFg;
	const auto &fgInvert = st::trayCounterFgMacInvert;
	const auto &bgInvert = st::trayCounterBgMacInvert;

	const auto &resultFg = !darkMode ? fg : muted ? fgInvert : fg;
	PlaceCounter(result, side, counter, bg, resultFg);
	PlaceCounter(resultActive, side, counter, bgInvert, fgInvert);

	// Scale large pixmaps to fit the available menu bar area.
	if (result.height() > maxImageHeight) {
		result = result.scaledToHeight(
			maxImageHeight,
			Qt::SmoothTransformation);
	}
	if (resultActive.height() > maxImageHeight) {
		resultActive = resultActive.scaledToHeight(
			maxImageHeight,
			Qt::SmoothTransformation);
	}

	status.button.image = Q2NSImage(result);
	status.button.alternateImage = Q2NSImage(resultActive);
	status.button.imageScaling = NSImageScaleProportionallyDown;
}

} // namespace

class NativeIcon final {
public:
	NativeIcon();
	~NativeIcon();

	void updateIcon();
	void setMenuProvider(Fn<QMenu*(bool rightButton)> provider);
	void dismissMenu();
	void deactivateButton();

	[[nodiscard]] rpl::producer<> activateRequests() const;
	[[nodiscard]] rpl::producer<> aboutToShowRequests() const;
	[[nodiscard]] rpl::producer<> menuClosed() const;

private:
	void prepareMenuForClick(bool rightButton);

	CommonDelegate *_delegate;
	NSStatusItem *_status;
	id _clickMonitor = nil;
	Fn<QMenu*(bool rightButton)> _menuProvider;

	rpl::event_stream<> _activateRequests;
	rpl::event_stream<> _menuClosed;

	rpl::lifetime _lifetime;

};

NativeIcon::NativeIcon()
: _delegate([[CommonDelegate alloc] init])
, _status([
	[NSStatusBar.systemStatusBar
		statusItemWithLength:NSSquareStatusItemLength] retain]) {

	[_status
		addObserver:_delegate
		forKeyPath:@"button.effectiveAppearance"
		options:0
			| NSKeyValueObservingOptionNew
			| NSKeyValueObservingOptionInitial
		context:nil];

	[_delegate closes] | rpl::on_next([=] {
		_status.menu = nil;
		_menuClosed.fire({});
	}, _lifetime);

	[_delegate appearanceChanges] | rpl::on_next([=] {
		updateIcon();
	}, _lifetime);

	_clickMonitor = [NSEvent
		addLocalMonitorForEventsMatchingMask:(NSEventMaskLeftMouseDown
			| NSEventMaskRightMouseDown)
		handler:^ NSEvent *(NSEvent *event) {
			if (event.window == _status.button.window) {
				prepareMenuForClick(
					event.type == NSEventTypeRightMouseDown);
			}
			return event;
		}];

	const auto masks = NSEventMaskLeftMouseDown
		| NSEventMaskLeftMouseUp
		| NSEventMaskRightMouseDown
		| NSEventMaskRightMouseUp
		| NSEventMaskOtherMouseUp;
	[_status.button sendActionOn:masks];

	id buttonCallback = [^{
		if (_status.menu) {
			return;
		} else if (NSApp.currentEvent.type == NSEventTypeLeftMouseDown) {
			Core::Sandbox::Instance().customEnterFromEventLoop([=] {
				_activateRequests.fire({});
			});
		}
	} copy];

	_lifetime.add([=] {
		[buttonCallback release];
	});

	_status.button.target = buttonCallback;
	_status.button.action = @selector(invoke);
	_status.button.toolTip = Q2NSString(AppName.utf16());
}

NativeIcon::~NativeIcon() {
	[NSEvent removeMonitor:_clickMonitor];
	[_status
		removeObserver:_delegate
		forKeyPath:@"button.effectiveAppearance"];
	[NSStatusBar.systemStatusBar removeStatusItem:_status];

	[_status release];
	[_delegate release];
}

void NativeIcon::updateIcon() {
	UpdateIcon(_status);
}

void NativeIcon::setMenuProvider(Fn<QMenu*(bool rightButton)> provider) {
	_menuProvider = std::move(provider);
}

void NativeIcon::prepareMenuForClick(bool rightButton) {
	Core::Sandbox::Instance().customEnterFromEventLoop([&] {
		const auto menu = _menuProvider
			? _menuProvider(rightButton)
			: nullptr;
		if (menu) {
			_status.menu = menu->toNSMenu();
			_status.menu.delegate = _delegate;
		} else {
			_status.menu = nil;
		}
	});
}

void NativeIcon::deactivateButton() {
	[_status.button highlight:false];
}

void NativeIcon::dismissMenu() {
	[_status.menu cancelTrackingWithoutAnimation];
	_status.menu = nil;
}

rpl::producer<> NativeIcon::activateRequests() const {
	return _activateRequests.events();
}

rpl::producer<> NativeIcon::aboutToShowRequests() const {
	return [_delegate aboutToShowRequests];
}

rpl::producer<> NativeIcon::menuClosed() const {
	return _menuClosed.events();
}

struct Tray::MenuContents {
	base::unique_qptr<QMenu> widget = base::make_unique_q<QMenu>(nullptr);
	rpl::lifetime actions;
};

Tray::Tray() {
}

void Tray::createIcon() {
	if (!_nativeIcon) {
		_nativeIcon = std::make_unique<NativeIcon>();
		// On macOS we are activating the window on click
		// instead of showing the menu, when the window is not activated.
		_nativeIcon->setMenuProvider([=](bool rightButton) -> QMenu* {
			if (!_menuBuilder || (!rightButton && !IsAnyActiveForTrayMenu())) {
				return nullptr;
			}
			_menuBuilder();
			return _menu ? _menu->widget.get() : nullptr;
		});
		_nativeIcon->activateRequests(
		) | rpl::on_next([=] {
			_nativeIcon->deactivateButton();
			_showFromTrayRequests.fire({});
		}, _iconLifetime);
		_nativeIcon->aboutToShowRequests(
		) | rpl::start_to_stream(_aboutToShowRequests, _iconLifetime);
		_nativeIcon->menuClosed() | rpl::on_next([=] {
			destroyMenu();
		}, _iconLifetime);
	}
	updateIcon();
}

void Tray::destroyIcon() {
	_iconLifetime.destroy();
	dismissMenu();
	_nativeIcon = nullptr;
}

void Tray::updateIcon() {
	if (_nativeIcon) {
		_nativeIcon->updateIcon();
	}
}

void Tray::setMenuBuilder(Fn<void()> builder) {
	_menuBuilder = std::move(builder);
}

void Tray::dismissMenu() {
	if (_nativeIcon) {
		_nativeIcon->dismissMenu();
	}
	destroyMenu();
}

void Tray::createMenu() {
	if (_menu) {
		return;
	}
	_menu = std::make_unique<MenuContents>();
	if (Core::App().passcodeLocked()) {
		return;
	}
	const auto widget = _menu->widget.get();
	const auto action = widget->addAction(
		u"kukagram: аккаунты, уведомления и клавиши"_q,
		[] {
			QTimer::singleShot(0, &Core::App(), [] {
				Core::App().showKukagramPreferences();
			});
		});
	action->setObjectName(u"kukagram-preferences"_q);
	const auto appearance = widget->addAction(
		u"kukagram: внешний вид"_q,
		[] {
			QTimer::singleShot(0, &Core::App(), [] {
				Core::App().showKukagramAppearance();
			});
		});
	appearance->setObjectName(u"kukagram-appearance"_q);
	widget->addSeparator();
}

void Tray::destroyMenu() {
	if (!_menu) {
		return;
	}
	// AppKit may still dispatch the selected QAction after menuDidClose.
	QTimer::singleShot(0, QCoreApplication::instance(), [
		closed = std::move(_menu)
	]() mutable {
		closed.reset();
	});
}

void Tray::addAction(rpl::producer<QString> text, Fn<void()> &&callback) {
	addAction(std::move(text), std::move(callback), QIcon());
}

void Tray::addAction(
		rpl::producer<QString> text,
		Fn<void()> &&callback,
		const QIcon &icon) {
	if (!_menu) {
		return;
	}

	const auto action = _menu->widget->addAction(QString(), std::move(callback));
	action->setIcon(icon);
	std::move(
		text
	) | rpl::on_next([=](const QString &text) {
		action->setText(text);
	}, _menu->actions);
}

void Tray::addAction(
		rpl::producer<QString> text,
		Fn<void()> &&callback,
		std::shared_ptr<Ui::DynamicImage> icon,
		int size) {
	if (!_menu) {
		return;
	}

	const auto action = _menu->widget->addAction(QString(), std::move(callback));
	if (icon) {
		const auto updateIcon = crl::guard(action, [=] {
			action->setIcon(QIcon(QPixmap::fromImage(icon->image(size))));
		});
		icon->subscribeToUpdates([=] {
			Core::Sandbox::Instance().customEnterFromEventLoop([=] {
				updateIcon();
			});
		});
		updateIcon();
		_menu->actions.add([icon = std::move(icon)] {
			icon->subscribeToUpdates(nullptr);
		});
	}
	std::move(
		text
	) | rpl::on_next([=](const QString &text) {
		action->setText(text);
	}, _menu->actions);
}

void Tray::addSeparator() {
	if (_menu) {
		_menu->widget->addSeparator();
	}
}

void Tray::showTrayMessage() const {
}

bool Tray::hasTrayMessageSupport() const {
	return false;
}

rpl::producer<> Tray::aboutToShowRequests() const {
	return _aboutToShowRequests.events();
}

rpl::producer<> Tray::showFromTrayRequests() const {
	return _showFromTrayRequests.events();
}

rpl::producer<> Tray::hideToTrayRequests() const {
	return rpl::never<>();
}

rpl::producer<> Tray::iconClicks() const {
	return rpl::never<>();
}

bool Tray::hasIcon() const {
	return _nativeIcon != nullptr;
}

rpl::lifetime &Tray::lifetime() {
	return _lifetime;
}

Tray::~Tray() {
	destroyIcon();
}

} // namespace Platform
