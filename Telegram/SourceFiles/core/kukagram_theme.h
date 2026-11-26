#pragma once

#include "core/kukagram_appearance.h"
#include "styles/style_basic.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_palette.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLatin1String>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <initializer_list>
#include <vector>
#include <map>

namespace Core::KukagramTheme {

struct State {
	std::map<QByteArray, QColor> originals;
	bool applied = false;
	bool notifying = false;
	bool scheduled = false;
	bool started = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] inline State &GetState() {
	static auto state = State();
	return state;
}

struct Accent {
	const char *name;
	QColor light;
	QColor dark;
};

[[nodiscard]] inline const std::vector<Accent> &Accents() {
	static const auto result = [] {
		const auto violet = QColor(143, 99, 242);
		const auto over = QColor(157, 116, 245);
		const auto ripple = QColor(122, 79, 219);
		const auto text = QColor(122, 72, 230);
		const auto textDark = QColor(184, 152, 255);
		const auto activeDark = QColor(98, 66, 184);
		auto list = std::vector<Accent>();
		const auto add = [&](
				std::initializer_list<const char*> names,
				QColor light,
				QColor dark) {
			for (const auto name : names) {
				list.push_back({ name, light, dark });
			}
		};
		add({
			"windowBgActive",
			"activeButtonBg",
			"sliderBgActive",
			"checkboxFgActive",
			"dialogsUnreadBg",
			"dialogsVerifiedIconBg",
			"dialogsVerifiedIconBgOver",
			"msgFileInBg",
			"msgWaveformInActive",
			"historyReplyIconFg",
			"overviewCheckBgActive",
			"profileVerifiedCheckBg",
			"profileAdminStartFg",
			"notificationSampleUserpicFg",
			"mediaPlayerActiveFg",
			"sideBarBadgeBg",
			"sideBarBadgeBgActive",
		}, violet, violet);
		add({
			"activeButtonBgOver",
			"dialogsUnreadBgOver",
			"activeLineFg",
			"msgInReplyBarColor",
			"msgInReplyBarSelColor",
		}, over, over);
		add({
			"activeButtonBgRipple",
			"dialogsRippleBgActive",
			"callBarMuteRipple",
		}, ripple, ripple);
		add({
			"windowActiveTextFg",
			"lightButtonFg",
			"lightButtonFgOver",
			"msgFileThumbLinkInFg",
			"msgFileThumbLinkInFgSelected",
			"historyLinkInFg",
			"historyLinkInFgSelected",
			"historyLinkOutFg",
			"historyLinkOutFgSelected",
			"contactsStatusFgOnline",
			"dialogsTextFgService",
			"dialogsTextFgServiceOver",
			"msgInServiceFg",
			"msgInServiceFgSelected",
			"sideBarTextFgActive",
			"sideBarIconFgActive",
		}, text, textDark);
		add({
			"dialogsBgActive",
			"mainMenuCoverBg",
			"callBarBg",
			"notificationsBoxScreenBg",
			"dialogsVerifiedIconFgActive",
			"dialogsUnreadFgActive",
		}, violet, activeDark);
		add({
			"historySendIconFg",
			"historySendIconFgOver",
		}, violet, textDark);
		return list;
	}();
	return result;
}

[[nodiscard]] inline QColor CurrentColor(const char *name) {
	const auto index = style::internal::GetPaletteIndex(QLatin1String(name));
	if (index < 0) {
		return QColor();
	}
	const auto &palette = *style::main_palette::get();
	return palette.colorAtIndex(index)->c;
}

inline void SetColor(const char *name, const QColor &color) {
	style::main_palette::setColor(
		QLatin1String(name),
		uchar(color.red()),
		uchar(color.green()),
		uchar(color.blue()),
		uchar(color.alpha()));
}

inline void Notify() {
	auto &state = GetState();
	state.notifying = true;
	style::main_palette::load(style::main_palette::save());
	style::NotifyPaletteChanged();
	state.notifying = false;
	if (const auto app = qobject_cast<QApplication*>(
			QCoreApplication::instance())) {
		for (const auto widget : QApplication::topLevelWidgets()) {
			widget->update();
		}
	}
}

[[nodiscard]] inline bool LooksApplied() {
	const auto &first = Accents().front();
	const auto now = CurrentColor(first.name);
	return now == first.light || now == first.dark;
}

inline void Apply() {
	auto &state = GetState();
	const auto enabled = KukagramAppearance::Current().theme;
	if (!enabled) {
		if (!state.applied) {
			return;
		}
		for (const auto &[name, color] : state.originals) {
			SetColor(name.constData(), color);
		}
		state.originals.clear();
		state.applied = false;
		Notify();
		return;
	}
	if (!state.applied || !LooksApplied()) {
		state.originals.clear();
		for (const auto &accent : Accents()) {
			const auto color = CurrentColor(accent.name);
			if (color.isValid()) {
				state.originals.emplace(QByteArray(accent.name), color);
			}
		}
	}
	const auto dark = st::windowBg->c.lightness() < 128;
	for (const auto &accent : Accents()) {
		if (state.originals.contains(QByteArray(accent.name))) {
			SetColor(accent.name, dark ? accent.dark : accent.light);
		}
	}
	state.applied = true;
	Notify();
}

inline void Schedule() {
	auto &state = GetState();
	if (state.scheduled) {
		return;
	}
	state.scheduled = true;
	// Deferred so a theme being applied saves its own colors first.
	QTimer::singleShot(0, QCoreApplication::instance(), [] {
		GetState().scheduled = false;
		Apply();
	});
}

inline void Start() {
	auto &state = GetState();
	if (state.started) {
		return;
	}
	state.started = true;
	style::PaletteChanged(
	) | rpl::on_next([] {
		auto &state = GetState();
		if (state.notifying) {
			return;
		}
		if (state.applied || KukagramAppearance::Current().theme) {
			state.applied = state.applied && LooksApplied();
			Schedule();
		}
	}, state.lifetime);
	Schedule();
}

} // namespace Core::KukagramTheme
