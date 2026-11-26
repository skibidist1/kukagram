/*
Local kukagram visual preferences.
No session data, Telegram themes or upstream settings are modified.
*/
#pragma once

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QEvent>
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <atomic>

#include "settings.h"

namespace Core::KukagramAppearance {

struct Values {
	bool colors = false;
	bool bubbles = false;
	bool animations = false;
	bool avatar = false;
	bool studio = false;
	bool moonVisible = true;
	bool theme = true;
	bool liveBackground = true;
	bool accountGrid = true;
	bool switchToast = true;
	bool splash = true;
	bool operator==(const Values &) const = default;
};

inline QString SettingsFile() {
	return cWorkingDir() + u"appearance.ini"_q;
}

inline unsigned short Encode(const Values &values) {
	return (values.colors ? 1 : 0)
		| (values.bubbles ? 2 : 0)
		| (values.animations ? 4 : 0)
		| (values.avatar ? 8 : 0)
		| (values.studio ? 16 : 0)
		| (values.moonVisible ? 32 : 0)
		| (values.theme ? 64 : 0)
		| (values.liveBackground ? 128 : 0)
		| (values.accountGrid ? 256 : 0)
		| (values.switchToast ? 512 : 0)
		| (values.splash ? 1024 : 0);
}

inline std::atomic<unsigned short> &StoredFlags() {
	static auto flags = std::atomic<unsigned short>([] {
		const auto settings = QSettings(SettingsFile(), QSettings::IniFormat);
		return Encode(Values{
			settings.value(QStringLiteral("colors"), false).toBool(),
			settings.value(QStringLiteral("bubbles"), false).toBool(),
			settings.value(QStringLiteral("animations"), false).toBool(),
			settings.value(QStringLiteral("avatar"), false).toBool(),
			settings.value(QStringLiteral("studio"), true).toBool(),
			settings.value(QStringLiteral("moonVisible"), true).toBool(),
			settings.value(QStringLiteral("theme"), true).toBool(),
			settings.value(QStringLiteral("liveBackground"), true).toBool(),
			settings.value(QStringLiteral("accountGrid"), true).toBool(),
			settings.value(QStringLiteral("switchToast"), true).toBool(),
			settings.value(QStringLiteral("splash"), true).toBool(),
		});
	}());
	return flags;
}

inline Values Current() {
	const auto bits = StoredFlags().load(std::memory_order_acquire);
	return Values{
		bool(bits & 1), bool(bits & 2),
		bool(bits & 4), bool(bits & 8), bool(bits & 16),
		bool(bits & 32), bool(bits & 64), bool(bits & 128),
		bool(bits & 256), bool(bits & 512), bool(bits & 1024),
	};
}

[[nodiscard]] inline bool MoonVisible() {
	return Current().moonVisible;
}

// Register a private event type to avoid collisions with other user events.
inline int ChangeEventType() {
	static const auto type = QEvent::registerEventType();
	return type;
}

inline bool Save(const Values &values) {
	const auto application = QCoreApplication::instance();
	if (application && QThread::currentThread() != application->thread()) {
		return false;
	}
	if (values == Current()) {
		return true;
	}
	const auto filename = SettingsFile();
	if (!QDir().mkpath(QFileInfo(filename).absolutePath())) {
		return false;
	}
	auto settings = QSettings(filename, QSettings::IniFormat);
	settings.setValue(QStringLiteral("colors"), values.colors);
	settings.setValue(QStringLiteral("bubbles"), values.bubbles);
	settings.setValue(QStringLiteral("animations"), values.animations);
	settings.setValue(QStringLiteral("avatar"), values.avatar);
	settings.setValue(QStringLiteral("studio"), values.studio);
	settings.setValue(QStringLiteral("moonVisible"), values.moonVisible);
	settings.setValue(QStringLiteral("theme"), values.theme);
	settings.setValue(QStringLiteral("liveBackground"), values.liveBackground);
	settings.setValue(QStringLiteral("accountGrid"), values.accountGrid);
	settings.setValue(QStringLiteral("switchToast"), values.switchToast);
	settings.setValue(QStringLiteral("splash"), values.splash);
	settings.sync();
	if (settings.status() != QSettings::NoError) {
		return false;
	}
	QFile::setPermissions(
		filename, QFileDevice::ReadUser | QFileDevice::WriteUser);
	StoredFlags().store(Encode(values), std::memory_order_release);

	// Repaint the UI once, without replacing Telegram's current theme.
	if (qobject_cast<QApplication*>(application)) {
		const auto type = ChangeEventType();
		for (auto widget : QApplication::allWidgets()) {
			QCoreApplication::removePostedEvents(widget, type);
			QCoreApplication::postEvent(
				widget, new QEvent(static_cast<QEvent::Type>(type)));
			widget->update();
		}
	}
	return true;
}

[[nodiscard]] inline bool SetMoonVisible(bool visible) {
	auto values = Current();
	values.moonVisible = visible;
	return Save(values);
}

} // namespace Core::KukagramAppearance
