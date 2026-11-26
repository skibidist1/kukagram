/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/application.h"
#include "core/kukagram_account_groups.h"
#ifdef Q_OS_MAC
#include "core/kukagram_account_groups_dialog.h"
#include "core/kukagram_account_grid.h"
#endif // Q_OS_MAC
#include "core/kukagram_theme.h"
#include "core/kukagram_splash.h"
#include "core/kukagram_appearance.h"
#include "ui/chat/studio_paint.h"
#include "ui/text/format_values.h"

#include "data/data_abstract_structure.h"
#include "data/data_channel.h"
#include "data/data_forum.h"
#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "data/data_download_manager.h"
#include "base/battery_saving.h"
#include "base/event_filter.h"
#include "base/invoke_queued.h"
#include "base/concurrent_timer.h"
#include "base/options.h"
#include "base/qt_signal_producer.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/core_screenshot_protection.h"
#include "core/core_settings.h"
#include "core/update_checker.h"
#include "core/shortcuts.h"
#include "core/sandbox.h"
#include "core/local_url_handlers.h"
#include "core/launcher.h"
#include "core/proxy_rotation_manager.h"
#include "core/ui_integration.h"
#include "core/version.h"
#include "chat_helpers/emoji_keywords.h"
#include "chat_helpers/stickers_emoji_image_loader.h"
#include "base/platform/base_platform_global_shortcuts.h"
#include "base/platform/base_platform_url_scheme.h"
#include "base/platform/base_platform_last_input.h"
#include "base/platform/base_platform_info.h"
#include "base/platform/base_platform_process.h"
#include "platform/platform_specific.h"
#include "platform/platform_integration.h"
#include "history/history.h"
#include "apiwrap.h"
#include "api/api_updates.h"
#include "calls/calls_instance.h"
#include "countries/countries_manager.h"
#include "iv/iv_delegate_impl.h"
#include "iv/iv_instance.h"
#include "iv/iv_data.h"
#include "iv/editor/iv_editor_session.h"
#include "iv/editor/iv_editor_window.h"
#include "lang/lang_translator.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_hardcoded.h"
#include "lang/lang_instance.h"
#include "inline_bots/bot_attach_web_view.h"
#include "mainwidget.h"
#include "tray.h"
#include "core/click_handler_types.h" // ClickHandlerContext.
#include "core/crash_reports.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/view/media_view_overlay_widget.h"
#include "media/view/media_view_open_common.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/web_proxy/web_proxy_transport.h"
#include "media/audio/media_audio_track.h"
#include "media/player/media_player_instance.h"
#include "media/player/media_player_float.h"
#include "media/clip/media_clip_reader.h" // For Media::Clip::Finish().
#include "media/media_video_encode.h"
#include "media/system_media_controls_manager.h"
#include "window/notifications_manager.h"
#include "window/themes/window_theme.h"
#include "ui/widgets/tooltip.h"
#include "ui/gl/gl_detection.h"
#include "ui/text/text_options.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/cached_round_corners.h"
#include "ui/power_saving.h"
#include "ui/screen_reader_mode.h"
#include "storage/storage_domain.h"
#include "storage/storage_databases.h"
#include "storage/storage_folder_archive.h"
#include "storage/localstorage.h"
#include "payments/payments_checkout_process.h"
#include "tgw/tgw_runtime.h"
#include "export/export_manager.h"
#include "wallet/wallet_panel.h"
#include "webrtc/webrtc_environment.h"
#include "webview/webview_interface.h"
#include "window/window_saved_windows.h"
#include "window/window_separate_id.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "boxes/abstract_box.h"
#include "boxes/keys_legend.h"
#include "base/qthelp_regex.h"
#include "base/qthelp_url.h"
#include "ui/accessible/ui_accessible_factory.h"
#include "ui/boxes/confirm_box.h"
#include "core/cached_webview_availability.h"
#include "test/test_agent.h"

#include <QtCore/QStandardPaths>
#include <QtCore/QMimeDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <array>
#include <optional>

#include <ksandbox.h>

namespace Core {
namespace {

constexpr auto kQuitPreventTimeoutMs = crl::time(1500);
constexpr auto kAutoLockTimeoutLateMs = crl::time(3000);
constexpr auto kClearEmojiImageSourceTimeout = 10 * crl::time(1000);

#ifdef Q_OS_MAC
enum class KukagramAction : int {
	Next,
	Previous,
	Preferences,
	Search,
	Grid,
	Count,
};

struct KukagramShortcut {
	int key = 0;
	quint32 physicalKey = 0;
	Qt::KeyboardModifiers modifiers = Qt::NoModifier;
	QString label;
	bool physicalKeyKnown = false;
};

using KukagramShortcuts = std::array<KukagramShortcut, int(KukagramAction::Count)>;

const auto kKukagramShortcutKeys = {
	u"next"_q,
	u"previous"_q,
	u"preferences"_q,
	u"search"_q,
	u"grid"_q,
};

[[nodiscard]] bool HasKukagramPhysicalKey(const KukagramShortcut &binding) {
	return binding.physicalKeyKnown || binding.physicalKey;
}

[[nodiscard]] bool HasKukagramNativeKeyData(const QKeyEvent *event) {
	return event->nativeScanCode()
		|| event->nativeVirtualKey()
		|| event->nativeModifiers();
}

[[nodiscard]] bool ValidKukagramShortcut(const KukagramShortcut &binding) {
	const auto key = binding.key;
	const auto mods = binding.modifiers;
	const auto allowed = Qt::ControlModifier | Qt::AltModifier
		| Qt::MetaModifier | Qt::ShiftModifier;
	return key > 0
		&& key != Qt::Key_unknown
		&& key != Qt::Key_Control && key != Qt::Key_Meta
		&& key != Qt::Key_Alt && key != Qt::Key_Shift
		&& !(mods & ~allowed)
		&& ((mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
			|| (key >= Qt::Key_F1 && key <= Qt::Key_F12));
}

[[nodiscard]] QString KukagramShortcutFile() {
	return cWorkingDir() + u"keyboard-shortcuts.ini"_q;
}

[[nodiscard]] QString ShortcutLabel(const KukagramShortcut &binding) {
	return binding.label.isEmpty()
		? QKeySequence(int(binding.modifiers) | binding.key
			).toString(QKeySequence::NativeText)
		: binding.label;
}

[[nodiscard]] KukagramShortcuts ReadKukagramShortcuts() {
	auto result = KukagramShortcuts{ {
		{ Qt::Key_Right, 0, Qt::ControlModifier | Qt::AltModifier, {} },
		{ Qt::Key_Left, 0, Qt::ControlModifier | Qt::AltModifier, {} },
		{ Qt::Key_N, 0x2D, Qt::ControlModifier | Qt::ShiftModifier, {} },
		{ Qt::Key_K, 0x28, Qt::ControlModifier | Qt::AltModifier, {} },
		{ Qt::Key_4, 0x15, Qt::AltModifier, {} },
	} };
	const auto defaults = result;
	const auto file = KukagramShortcutFile();
	if (!QFileInfo::exists(file)) {
		return result;
	}
	const auto settings = QSettings(file, QSettings::IniFormat);
	auto i = 0;
	for (const auto &name : kKukagramShortcutKeys) {
		const auto prefix = name + u"/"_q;
		if (settings.contains(prefix + u"key"_q)) {
			auto &binding = result[i];
			auto keyValid = false;
			auto physicalValid = false;
			auto modifiersValid = false;
			binding.key = settings.value(prefix + u"key"_q).toInt(&keyValid);
			binding.physicalKey = settings.value(
				prefix + u"physical"_q).toUInt(&physicalValid);
			binding.modifiers = Qt::KeyboardModifiers::fromInt(
				settings.value(prefix + u"modifiers"_q).toInt(&modifiersValid));
			binding.label = settings.value(prefix + u"label"_q).toString();
			binding.physicalKeyKnown = settings.value(
				prefix + u"physicalKnown"_q,
				binding.physicalKey != 0 || !binding.label.isEmpty()).toBool();
			if (!keyValid || !physicalValid || !modifiersValid
				|| !ValidKukagramShortcut(binding)) {
				binding = defaults[i];
			}
		}
		++i;
	}
	return result;
}

KukagramShortcuts &CurrentKukagramShortcuts() {
	static auto bindings = ReadKukagramShortcuts();
	return bindings;
}

[[nodiscard]] bool SaveKukagramShortcuts(
		const KukagramShortcuts &bindings) {
	if (!ranges::all_of(bindings, ValidKukagramShortcut)) {
		return false;
	}
	const auto file = KukagramShortcutFile();
	if (!QDir().mkpath(QFileInfo(file).absolutePath())) {
		return false;
	}
	auto settings = QSettings(file, QSettings::IniFormat);
	auto i = 0;
	for (const auto &name : kKukagramShortcutKeys) {
		const auto prefix = name + u"/"_q;
		const auto &binding = bindings[i++];
		settings.setValue(prefix + u"key"_q, binding.key);
		settings.setValue(prefix + u"physical"_q, binding.physicalKey);
		settings.setValue(prefix + u"physicalKnown"_q, HasKukagramPhysicalKey(binding));
		settings.setValue(prefix + u"modifiers"_q, int(binding.modifiers));
		settings.setValue(prefix + u"label"_q, binding.label);
	}
	settings.sync();
	if (settings.status() != QSettings::NoError) {
		return false;
	}
	QFile::setPermissions(file, QFileDevice::ReadUser | QFileDevice::WriteUser);
	CurrentKukagramShortcuts() = bindings;
	return true;
}

[[nodiscard]] bool ShortcutMatches(
		const QKeyEvent *event,
		const KukagramShortcut &binding) {
	if (!binding.key || event->isAutoRepeat()
		|| (event->modifiers() & ~Qt::KeypadModifier)
			!= binding.modifiers) {
		return false;
	}
	const auto pressedPhysical = event->nativeVirtualKey();
	return (HasKukagramPhysicalKey(binding) && HasKukagramNativeKeyData(event))
		? (binding.physicalKey == pressedPhysical)
		: (event->key() == binding.key);
}

[[nodiscard]] std::optional<KukagramAction> MatchKukagramShortcut(
		const QKeyEvent *event) {
	const auto &bindings = CurrentKukagramShortcuts();
	for (auto i = 0; i != int(KukagramAction::Count); ++i) {
		if (ShortcutMatches(event, bindings[i])) {
			return KukagramAction(i);
		}
	}
	return std::nullopt;
}

class KukagramRecorder final : public QPushButton {
public:
	KukagramRecorder(KukagramShortcut *binding, QWidget *parent)
	: QPushButton(parent)
	, _binding(binding) {
		setFocusPolicy(Qt::StrongFocus);
		setText(ShortcutLabel(*_binding));
		QObject::connect(this, &QPushButton::clicked, this, [=] {
			_recording = true;
			setText(u"Нажми сочетание клавиш..."_q);
			setFocus(Qt::OtherFocusReason);
		});
	}

protected:
	void keyPressEvent(QKeyEvent *event) override {
		if (!_recording) {
			QPushButton::keyPressEvent(event);
			return;
		}
		const auto key = event->key();
		if (key == Qt::Key_Escape) {
			_recording = false;
			setText(ShortcutLabel(*_binding));
			event->accept();
			return;
		}
		if (key == Qt::Key_Control || key == Qt::Key_Meta
			|| key == Qt::Key_Alt || key == Qt::Key_Shift
			|| key == Qt::Key_unknown) {
			event->accept();
			return;
		}
		const auto mods = event->modifiers() & ~Qt::KeypadModifier;
		if (!(mods & (Qt::ControlModifier | Qt::AltModifier
				| Qt::MetaModifier))
			&& !(key >= Qt::Key_F1 && key <= Qt::Key_F12)) {
			setText(u"Нужен ⌘/⌥/Control + клавиша или F1–F12"_q);
			event->accept();
			return;
		}
		const auto binding = KukagramShortcut{
			.key = key,
			.physicalKey = event->nativeVirtualKey(),
			.modifiers = mods,
			.label = QKeySequence(int(mods) | key
				).toString(QKeySequence::NativeText),
			.physicalKeyKnown = HasKukagramNativeKeyData(event),
		};
		*_binding = binding;
		_recording = false;
		setText(ShortcutLabel(*_binding));
		event->accept();
	}

private:
	KukagramShortcut *_binding = nullptr;
	bool _recording = false;
};

class KukagramSettingsDialog final : public QDialog {
public:
	explicit KukagramSettingsDialog(QWidget *parent)
	: QDialog(parent) {
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_QuitOnClose, false);
		setWindowModality(Qt::ApplicationModal);
		QObject::connect(QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit, this, &QDialog::reject);
		QObject::connect(&Core::App(),
			&QObject::destroyed, this, &QDialog::reject);
		Core::App().passcodeLockChanges(
		) | rpl::filter([](bool locked) { return locked; }) | rpl::on_next([=] {
			reject();
		}, _lifetime);
	}

private:
	rpl::lifetime _lifetime;
};

void ShowKukagramWarning(not_null<QDialog*> parent, const QString &text) {
	const auto box = new QMessageBox(
		QMessageBox::Warning, u"kukagram"_q, text, QMessageBox::Ok, parent);
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->open();
}

void ShowKukagramNotificationAccountsDialog(not_null<Main::Domain*> domain) {
	const auto weakDomain = base::make_weak(domain);
	const auto dialog = new KukagramSettingsDialog(QApplication::activeWindow());
	dialog->setWindowTitle(u"kukagram — аккаунты и горячие клавиши"_q);
	auto layout = new QVBoxLayout(dialog);
	auto description = new QLabel(
		u"Отметь аккаунты, которым разрешены уведомления. "
		u"Неотмеченные останутся авторизованными. "
		u"После изменения списка новые аккаунты будут без уведомлений."_q,
		dialog);
	description->setWordWrap(true);
	layout->addWidget(description);
	auto search = new QLineEdit(dialog);
	search->setPlaceholderText(u"Поиск по имени или номеру"_q);
	layout->addWidget(search);
	auto list = new QListWidget(dialog);
	for (const auto &account : domain->storedAccounts()) {
		if (!account.uniqueId) {
			continue;
		}
		const auto name = account.name.isEmpty()
			? u"Аккаунт %1"_q.arg(account.index + 1)
			: account.name;
		const auto label = account.phone.isEmpty()
			? name
			: name + u"  ·  "_q + Ui::FormatPhone(account.phone);
		auto item = new QListWidgetItem(label, list);
		item->setData(Qt::UserRole, QVariant::fromValue(qulonglong(account.uniqueId)));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(domain->notificationsAllowed(account.uniqueId)
			? Qt::Checked : Qt::Unchecked);
	}
	layout->addWidget(list);
	const auto notificationPolicyEdited = std::make_shared<bool>(false);
	QObject::connect(list, &QListWidget::itemChanged, dialog, [=] {
		*notificationPolicyEdited = true;
	});
	QObject::connect(search, &QLineEdit::textChanged, dialog,
		[=](const QString &query) {
			for (auto i = 0; i != list->count(); ++i) {
				const auto item = list->item(i);
				item->setHidden(!KukagramAccountGroups::LabelMatches(
					item->text(),
					query));
			}
		});
	auto buttonsRow = new QHBoxLayout();
	auto onlyCurrent = new QPushButton(u"Только текущий"_q, dialog);
	auto none = new QPushButton(u"Выключить все"_q, dialog);
	buttonsRow->addWidget(onlyCurrent);
	buttonsRow->addWidget(none);
	layout->addLayout(buttonsRow);
	const auto active = domain->active().maybeSession();
	const auto currentId = active ? active->uniqueId() : 0;
	onlyCurrent->setEnabled(currentId != 0);
	QObject::connect(onlyCurrent, &QPushButton::clicked, dialog, [=] {
		*notificationPolicyEdited = true;
		for (auto i = 0; i != list->count(); ++i) {
			const auto item = list->item(i);
			const auto id = item->data(Qt::UserRole).toULongLong();
			item->setCheckState(id == currentId ? Qt::Checked : Qt::Unchecked);
		}
	});
	QObject::connect(none, &QPushButton::clicked, dialog, [=] {
		*notificationPolicyEdited = true;
		for (auto i = 0; i != list->count(); ++i) {
			list->item(i)->setCheckState(Qt::Unchecked);
		}
	});

	const auto bindings = std::make_shared<KukagramShortcuts>(CurrentKukagramShortcuts());
	auto keyGroup = new QGroupBox(u"Сочетания клавиш"_q, dialog);
	auto keyLayout = new QFormLayout(keyGroup);
	keyLayout->addRow(u"Следующий аккаунт:"_q,
		new KukagramRecorder(&(*bindings)[0], keyGroup));
	keyLayout->addRow(u"Предыдущий аккаунт:"_q,
		new KukagramRecorder(&(*bindings)[1], keyGroup));
	keyLayout->addRow(u"Открыть эти настройки:"_q,
		new KukagramRecorder(&(*bindings)[2], keyGroup));
	keyLayout->addRow(u"Поиск аккаунта:"_q,
		new KukagramRecorder(&(*bindings)[3], keyGroup));
	keyLayout->addRow(u"Сетка аккаунтов:"_q,
		new KukagramRecorder(&(*bindings)[4], keyGroup));
	layout->addWidget(keyGroup);
	auto hint = new QLabel(
		u"Нажми кнопку сочетания, затем новые клавиши. "
		u"На Mac запоминается физическая клавиша: русская и английская "
		u"раскладки работают одинаково. "
		u"Можно назначить любое сочетание, даже занятое macOS (например ⌘W): "
		u"в Kukagram оно будет переключать аккаунты. "
		u"Чтобы получать фоновые уведомления от отмеченных аккаунтов, "
		u"открой каждый из них один раз после запуска приложения."_q,
		dialog);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto actions = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel,
		Qt::Horizontal,
		dialog);
	layout->addWidget(actions);
	QObject::connect(actions, &QDialogButtonBox::rejected,
		dialog, &QDialog::reject);
	QObject::connect(actions, &QDialogButtonBox::accepted, dialog, [=] {
		const auto domain = weakDomain.get();
		if (Core::Quitting() || !domain || !domain->started()
			|| Core::App().passcodeLocked()) {
			dialog->reject();
			return;
		}
		for (auto i = 0; i != int(KukagramAction::Count); ++i) {
			for (auto j = i + 1; j != int(KukagramAction::Count); ++j) {
				if ((*bindings)[i].modifiers == (*bindings)[j].modifiers
					&& ((HasKukagramPhysicalKey((*bindings)[i])
						&& HasKukagramPhysicalKey((*bindings)[j]))
						? (*bindings)[i].physicalKey == (*bindings)[j].physicalKey
						: (*bindings)[i].key == (*bindings)[j].key)) {
					ShowKukagramWarning(dialog,
						u"Одно сочетание назначено двум действиям."_q);
					return;
				}
			}
		}
		auto allowed = std::vector<uint64>();
		for (auto i = 0; i != list->count(); ++i) {
			const auto item = list->item(i);
			if (item->checkState() == Qt::Checked) {
				allowed.push_back(item->data(Qt::UserRole).toULongLong());
			}
		}
		if (*notificationPolicyEdited && !domain->saveNotificationAllowlist(allowed)) {
			ShowKukagramWarning(dialog,
				u"Не удалось сохранить настройки уведомлений."_q);
			return;
		}
		if (!SaveKukagramShortcuts(*bindings)) {
			ShowKukagramWarning(dialog,
				*notificationPolicyEdited
					? u"Уведомления сохранены, но сочетания клавиш не записались."_q
					: u"Не удалось сохранить сочетания клавиш."_q);
			return;
		}
		dialog->accept();
	});
	dialog->show();
}

class KukagramStylePreview final : public QWidget {
public:
	explicit KukagramStylePreview(not_null<QWidget*> parent)
	: QWidget(parent) {
		setMinimumSize(320, 132);
		setAccessibleName(u"Предпросмотр оформления чатов"_q);
	}

	void setStudio(bool enabled) {
		_studio = enabled;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto painter = QPainter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const auto frame = QRectF(rect()).adjusted(1, 1, -1, -1);
		auto clip = QPainterPath();
		clip.addRoundedRect(frame, 13, 13);
		painter.setClipPath(clip);
		painter.setPen(Qt::NoPen);

		const auto content = frame;
		painter.fillRect(frame, QColor(27, 36, 49));
		if (_studio) {
			painter.fillRect(frame, Ui::Studio::Backdrop(frame));
		}

		const auto round = [&](QRectF area, const QBrush &brush, qreal radius) {
			painter.setBrush(brush);
			painter.drawRoundedRect(area, radius, radius);
		};
		const auto header = QRectF(
			content.x(), content.y(), content.width(), 28);
		painter.fillRect(header, QColor(39, 55, 71));
		if (_studio) {
			Ui::Studio::PaintHeader(painter, header, 2);
			painter.fillRect(QRectF(header.x(), header.y() + 7, 3, 14),
				Ui::Studio::Mint());
		}
		round(QRectF(header.x() + 12, header.y() + 11, 49, 5),
			QColor(225, 227, 245, 205), 2.5);
		const auto bubble = [&](const QRectF &area, bool outgoing, QColor base) {
			auto fill = QLinearGradient(area.topLeft(), area.topRight());
			const auto colors = Ui::Studio::BubbleColors(true, outgoing);
			fill.setColorAt(0., _studio ? colors[0] : base);
			fill.setColorAt(1., _studio ? colors[1] : base);
			round(area, fill, 9);
		};

		const auto incoming = QRectF(content.x() + 12, content.y() + 40,
			content.width() * 0.49, 30);
		bubble(incoming, false, QColor(38, 63, 82));
		round(incoming.adjusted(11, 13, -17, -12), QColor(229, 231, 247, 194), 2.5);
		const auto outgoingWidth = content.width() * 0.53;
		const auto outgoing = QRectF(content.right() - outgoingWidth - 12,
			content.y() + 79, outgoingWidth, 32);
		bubble(outgoing, true, QColor(48, 94, 135));
		round(outgoing.adjusted(13, 14, -19, -13), QColor(245, 237, 255, 218), 2.5);
	}

private:
	bool _studio = true;
};

void ShowKukagramAppearanceDialog() {
	const auto dialog = new KukagramSettingsDialog(QApplication::activeWindow());
	dialog->setWindowTitle(u"kukagram — внешний вид"_q);
	auto layout = new QVBoxLayout(dialog);
	const auto original = KukagramAppearance::Current();
	auto studio = new QCheckBox(u"Оформление Studio"_q, dialog);
	auto studioFont = studio->font();
	studioFont.setBold(true);
	studio->setFont(studioFont);
	studio->setChecked(original.studio);
	layout->addWidget(studio);
	auto preview = new KukagramStylePreview(dialog);
	preview->setStudio(original.studio);
	layout->addWidget(preview);
	QObject::connect(studio, &QCheckBox::toggled, preview, [=](bool enabled) {
		preview->setStudio(enabled);
	});

	auto legacy = new QGroupBox(u"Отдельные эффекты"_q, dialog);
	auto legacyLayout = new QVBoxLayout(legacy);
	auto colors = new QCheckBox(u"Фиолетово-мятные акценты интерфейса"_q, dialog);
	auto bubbles = new QCheckBox(u"Мягкие цвета и скругления текстовых сообщений"_q, dialog);
	auto animations = new QCheckBox(u"Лёгкие анимации при смене чата"_q, dialog);
	auto avatar = new QCheckBox(u"Аватарка аккаунта с мятным ободком"_q, dialog);
	colors->setChecked(original.colors);
	bubbles->setChecked(original.bubbles);
	animations->setChecked(original.animations);
	avatar->setChecked(original.avatar);
	for (auto checkbox : { colors, bubbles, animations, avatar }) {
		legacyLayout->addWidget(checkbox);
	}
	layout->addWidget(legacy);

	auto fresh = new QGroupBox(u"Новое оформление Kukagram"_q, dialog);
	auto freshLayout = new QVBoxLayout(fresh);
	auto theme = new QCheckBox(u"Фиолетовая тема: кнопки, выделение, счётчики"_q, dialog);
	auto liveBackground = new QCheckBox(u"Живой фон: Луна вращается, звёзды мерцают"_q, dialog);
	auto accountGrid = new QCheckBox(u"Сетка аккаунтов в меню ☰ (⌥4)"_q, dialog);
	auto switchToast = new QCheckBox(u"Подсказка при смене аккаунта"_q, dialog);
	auto splash = new QCheckBox(u"Экран загрузки Kukagram при запуске"_q, dialog);
	theme->setChecked(original.theme);
	liveBackground->setChecked(original.liveBackground);
	accountGrid->setChecked(original.accountGrid);
	switchToast->setChecked(original.switchToast);
	splash->setChecked(original.splash);
	for (auto checkbox : { theme, liveBackground, accountGrid, switchToast, splash }) {
		freshLayout->addWidget(checkbox);
	}
	auto freshOff = new QPushButton(u"Вернуть как было (выключить новое)"_q, dialog);
	freshLayout->addWidget(freshOff);
	QObject::connect(freshOff, &QPushButton::clicked, dialog, [=] {
		for (auto checkbox : { theme, liveBackground, accountGrid, switchToast, splash }) {
			checkbox->setChecked(false);
		}
	});
	layout->addWidget(fresh);
	auto row = new QHBoxLayout();
	auto allOff = new QPushButton(u"Выключить всё"_q, dialog);
	auto allOn = new QPushButton(u"Включить всё"_q, dialog);
	row->addWidget(allOff);
	row->addWidget(allOn);
	layout->addLayout(row);
	QObject::connect(allOff, &QPushButton::clicked, dialog, [=] {
		for (auto checkbox : { studio, colors, bubbles, animations, avatar,
				theme, liveBackground, accountGrid, switchToast, splash }) {
			checkbox->setChecked(false);
		}
	});
	QObject::connect(allOn, &QPushButton::clicked, dialog, [=] {
		for (auto checkbox : { studio, colors, bubbles, animations, avatar,
				theme, liveBackground, accountGrid, switchToast, splash }) {
			checkbox->setChecked(true);
		}
	});
	auto actions = new QDialogButtonBox(
		QDialogButtonBox::Save | QDialogButtonBox::Cancel,
		Qt::Horizontal, dialog);
	layout->addWidget(actions);
	QObject::connect(actions, &QDialogButtonBox::rejected,
		dialog, &QDialog::reject);
	QObject::connect(actions, &QDialogButtonBox::accepted, dialog, [=] {
		if (Core::Quitting() || Core::App().passcodeLocked()) {
			dialog->reject();
			return;
		}
		auto values = KukagramAppearance::Current();
		values.colors = colors->isChecked();
		values.bubbles = bubbles->isChecked();
		values.animations = animations->isChecked();
		values.avatar = avatar->isChecked();
		values.studio = studio->isChecked();
		values.theme = theme->isChecked();
		values.liveBackground = liveBackground->isChecked();
		values.accountGrid = accountGrid->isChecked();
		values.switchToast = switchToast->isChecked();
		values.splash = splash->isChecked();
		if (!KukagramAppearance::Save(values)) {
			ShowKukagramWarning(dialog,
				u"Не удалось сохранить внешний вид."_q);
			return;
		}
		KukagramTheme::Apply();
		dialog->accept();
	});
	dialog->show();
}

#endif // Q_OS_MAC

LaunchState GlobalLaunchState/* = LaunchState::Running*/;

void SetCrashAnnotationsGL() {
#ifdef DESKTOP_APP_USE_ANGLE
	CrashReports::SetAnnotation("OpenGL ANGLE", [] {
		if (Core::App().settings().disableOpenGL()) {
			return "Disabled";
		} else switch (Ui::GL::CurrentANGLE()) {
		case Ui::GL::ANGLE::Auto: return "Auto";
		case Ui::GL::ANGLE::D3D11: return "Direct3D 11";
		case Ui::GL::ANGLE::D3D9: return "Direct3D 9";
		case Ui::GL::ANGLE::D3D11on12: return "D3D11on12";
		//case Ui::GL::ANGLE::OpenGL: return "OpenGL";
		}
		Unexpected("Ui::GL::CurrentANGLE value in SetupANGLE.");
	}());
#else // DESKTOP_APP_USE_ANGLE
	CrashReports::SetAnnotation(
		"OpenGL",
		Core::App().settings().disableOpenGL() ? "Disabled" : "Enabled");
#endif // DESKTOP_APP_USE_ANGLE
}

base::options::toggle OptionSkipUrlSchemeRegister({
	.id = kOptionSkipUrlSchemeRegister,
	.name = "Skip URL scheme register",
	.description = "Don't re-register tg:// URL scheme on autoupdate.",
});

} // namespace

Application *Application::Instance = nullptr;

const char kOptionSkipUrlSchemeRegister[] = "skip-url-scheme-register";

struct Application::Private {
	base::Timer quitTimer;
	UiIntegration uiIntegration;
	Settings settings;
	std::unique_ptr<ProxyRotationManager> proxyRotation;
};

Application::Application()
: QObject()
, _private(std::make_unique<Private>())
, _platformIntegration(Platform::Integration::Create())
, _batterySaving(std::make_unique<base::BatterySaving>())
, _mediaDevices(std::make_unique<Webrtc::Environment>())
, _screenshotProtection(std::make_unique<ScreenshotProtection>())
, _databases(std::make_unique<Storage::Databases>())
, _animationsManager(std::make_unique<Ui::Animations::Manager>())
, _clearEmojiImageLoaderTimer([=] { clearEmojiSourceImages(); })
, _audio(std::make_unique<Media::Audio::Instance>())
, _fallbackProductionConfig(
	std::make_unique<MTP::Config>(MTP::Environment::Production))
, _downloadManager(std::make_unique<Data::DownloadManager>())
, _domain(std::make_unique<Main::Domain>(cDataFile()))
, _exportManager(std::make_unique<Export::Manager>())
, _calls(std::make_unique<Calls::Instance>())
, _iv(std::make_unique<Iv::Instance>(
	Ui::CreateChild<Iv::DelegateImpl>(this)))
, _langpack(std::make_unique<Lang::Instance>())
, _langCloudManager(std::make_unique<Lang::CloudManager>(langpack()))
, _emojiKeywords(std::make_unique<ChatHelpers::EmojiKeywords>())
, _tray(std::make_unique<Tray>())
, _setupEmailLock(false)
, _autoLockTimer([=] { checkAutoLock(); }) {
	Ui::Integration::Set(&_private->uiIntegration);
	_private->proxyRotation = std::make_unique<ProxyRotationManager>();

	_platformIntegration->init();

	passcodeLockChanges(
	) | rpl::on_next([=](bool locked) {
		_shouldLockAt = 0;
		if (locked) {
			closeAdditionalWindows();
		}
	}, _lifetime);

	passcodeLockChanges(
	) | rpl::on_next([=] {
		_notifications->updateAll();
		updateWindowTitles();
	}, _lifetime);

	settings().windowTitleContentChanges(
	) | rpl::on_next([=] {
		updateWindowTitles();
	}, _lifetime);

	_domain->activeSessionChanges(
	) | rpl::on_next([=](Main::Session *session) {
		if (session && !UpdaterDisabled()) { // #TODO multi someSessionValue
			UpdateChecker().setMtproto(session);
		}
	}, _lifetime);

	MTP::WebProxy::Transport::StateChanges(
	) | rpl::on_next([=](
			const MTP::WebProxy::Transport::StateChange &change) {
		using State = MTP::WebProxy::Transport::State;
		if (change.state != State::WaitingForBrowser) {
			if (_webProxyFallbackBox) {
				_webProxyFallbackBox->closeBox();
			}
			return;
		}
		const auto &proxy = settings().proxy();
		if (_webProxyFallbackBox
			|| !proxy.isEnabled()
			|| proxy.selected() != change.proxy) {
			return;
		}
		_webProxyFallbackBox = Ui::show(Ui::MakeConfirmBox({
			.text = tr::lng_proxy_web_fallback(tr::now),
			.confirmed = [=] {
				const auto &current = settings().proxy();
				if (current.isEnabled()
					&& current.selected() == change.proxy) {
					MTP::WebProxy::Transport::OpenBrowser(change.proxy);
				}
			},
			.confirmText = tr::lng_proxy_web_open(tr::now),
			.cancelText = tr::lng_cancel(tr::now),
		}), Ui::LayerOption::KeepOther);
	}, _lifetime);
}

void Application::closeAdditionalWindows() {
	Payments::CheckoutProcess::ClearAll();
	for (const auto &[index, account] : _domain->accounts()) {
		if (account->sessionExists()) {
			account->session().attachWebView().closeAll();
			Wallet::CloseWallet(&account->session());
		}
	}
	_iv->closeAll();
	Iv::Editor::CloseAllWindows();
}

Application::~Application() {
	if (_savedWindows) {
		_savedWindows->writeNow();
	}
	if (_saveSettingsTimer && _saveSettingsTimer->isActive()) {
		Local::writeSettings();
	}

	_windowStack.clear();
	setLastActiveWindow(nullptr);
	_windowInSettings = _lastActivePrimaryWindow = nullptr;
	_closingAsyncWindows.clear();
	_windows.clear();
	_mediaView = nullptr;
	_notifications->clearAllFast();

	// We must manually destroy all windows before going further.
	// DestroyWindow on Windows (at least with an active WebView) enters
	// event loop and invoke scheduled crl::on_main callbacks.
	//
	// For example Domain::removeRedundantAccounts() is called from
	// Domain::finish() and there is a violation on Ensures(started()).
	closeAdditionalWindows();

	_private->proxyRotation = nullptr;
	_domain->finish();
	MTP::WebProxy::Transport::Shutdown();

	Local::finish();

	Shortcuts::Finish();

	Ui::Emoji::Clear();
	Media::Clip::Finish();

	Ui::FinishCachedCorners();
	Data::clearGlobalStructures();

	Window::Theme::Uninitialize();

	_mediaControlsManager = nullptr;

	Media::Player::finish(_audio.get());
	style::StopManager();

	Instance = nullptr;
}

void Application::run() {
	// Depends on OpenSSL on macOS, so on ThirdParty::start().
	// Depends on notifications settings.
	_notifications = std::make_unique<Window::Notifications::System>();

	startLocalStorage();

	style::SetCustomFont(settings().customFontFamily());
	style::internal::StartFonts();

	Test::ApplyStartupOverrides();

	ValidateScale();

	refreshGlobalProxy(); // Depends on app settings being read.

	if (const auto old = Local::oldSettingsVersion(); old < AppVersion) {
		autoRegisterUrlScheme();
		Platform::NewVersionLaunched(old);
	}

	if (cAutoStart() && !Platform::AutostartSupported()) {
		cSetAutoStart(false);
	}

	if (cLaunchMode() == LaunchModeAutoStart && Platform::AutostartSkip()) {
		Platform::AutostartToggle(false);
		Quit();
		return;
	}

	_translator = std::make_unique<Lang::Translator>();
	QCoreApplication::instance()->installTranslator(_translator.get());

	style::StartManager(cScale());
	Ui::Accessible::Init();
	Ui::InitTextOptions();
	Ui::StartCachedCorners();
	Ui::Emoji::Init();
	Ui::PreloadTextSpoilerMask(_lifetime);
	startShortcuts();
	startEmojiImageLoader();
	startSystemDarkModeViewer();
	Media::Player::start(_audio.get());
	Media::Encode::ClearStaleTempFiles();
	Storage::ClearStaleArchiveFiles();
	Webview::ClearStaleTemporaryStorage();

	if (MediaControlsManager::Supported()) {
		_mediaControlsManager = std::make_unique<MediaControlsManager>();
	}

	rpl::combine(
		_batterySaving->value(),
		settings().ignoreBatterySavingValue()
	) | rpl::on_next([=](bool saving, bool ignore) {
		PowerSaving::SetForceAll(saving && !ignore);
	}, _lifetime);

	style::ShortAnimationPlaying(
	) | rpl::on_next([=](bool playing) {
		if (playing) {
			MTP::details::pause();
		} else {
			MTP::details::unpause();
		}
	}, _lifetime);

	DEBUG_LOG(("Application Info: inited..."));
	LOG(("Qt version: %1 (compiled with %2)").arg(
		QString::fromLatin1(qVersion()),
		QString::fromLatin1(QT_VERSION_STR)));

	DEBUG_LOG(("Application Info: starting app..."));

	// Create mime database, so it won't be slow later.
	QMimeDatabase().mimeTypeForName(u"text/plain"_q);

	// Check now to avoid re-entrance later.
	[[maybe_unused]] const auto &webviewAvailability
		= Core::CachedWebviewAvailability();

	_savedWindows = std::make_unique<Window::SavedWindows>(this);

	_windows.emplace(nullptr, std::make_unique<Window::Controller>());
	setLastActiveWindow(_windows.front().second.get());
	_windowInSettings = _lastActivePrimaryWindow = _lastActiveWindow;

	_domain->activeChanges(
	) | rpl::on_next([=](not_null<Main::Account*> account) {
		showAccount(account);
#ifdef Q_OS_MAC
		showKukagramSwitchToast(account);
#endif // Q_OS_MAC
	}, _lifetime);
	KukagramTheme::Start();

	QCoreApplication::instance()->installEventFilter(this);

	appDeactivatedValue(
	) | rpl::on_next([=](bool deactivated) {
		if (deactivated) {
			handleAppDeactivated();
		} else {
			handleAppActivated();
		}
	}, _lifetime);

	DEBUG_LOG(("Application Info: window created..."));

	startDomain();
	startTray();

	_lastActivePrimaryWindow->firstShow();

	startMediaView();

	DEBUG_LOG(("Application Info: showing."));
	_lastActivePrimaryWindow->finishFirstShow();
	ShowKukagramSplash(_lastActivePrimaryWindow->widget()->bodyWidget());

	if (!_lastActivePrimaryWindow->locked() && cStartToSettings()) {
		_lastActivePrimaryWindow->showSettings();
	}

	_lastActivePrimaryWindow->updateIsActiveFocus();

	for (const auto &error : Shortcuts::Errors()) {
		LOG(("Shortcuts Error: %1").arg(error));
	}

	SetCrashAnnotationsGL();
	if (Ui::GL::LastCrashCheckFailed()) {
		showOpenGLCrashNotification();
	}

	_openInMediaViewRequests.events(
	) | rpl::on_next([=](Media::View::OpenRequest &&request) {
		if (_mediaView) {
			_mediaView->show(std::move(request));
		}
	}, _lifetime);
	{
		const auto countries = std::make_shared<Countries::Manager>(
			_domain.get());
		countries->lifetime().add([=] {
			[[maybe_unused]] const auto countriesCopy = countries;
		});
	}

	processCreatedWindow(_lastActivePrimaryWindow);

	_savedWindows->startRestore();

	Test::Fire(u"launch_finished"_q);
	Test::Start();
}

void Application::autoRegisterUrlScheme() {
	if (!OptionSkipUrlSchemeRegister.value()) {
		InvokeQueued(this, [] { RegisterUrlScheme(); });
	}
}

void Application::showAccount(not_null<Main::Account*> account) {
	if (const auto separate = separateWindowFor(account)) {
		_lastActivePrimaryWindow = separate;
		separate->activate();
	} else if (const auto last = activePrimaryWindow()) {
		last->showAccount(account);
	}
}

void Application::checkWindowId(not_null<Window::Controller*> window) {
	const auto id = window->id();
	for (auto &[existingId, existing] : _windows) {
		if (existing.get() == window && existingId != id) {
			auto found = std::move(existing);
			_windows.remove(existingId);
			_windows.emplace(id, std::move(found));
			break;
		}
	}
	if (_savedWindows) {
		_savedWindows->scheduleSave();
	}
	if (!_lastActiveWindow) {
		setLastActiveWindow(window);
	}
}

void Application::showOpenGLCrashNotification() {
	const auto enable = [=] {
		Ui::GL::CrashCheckFinish();
		settings().setDisableOpenGL(false);
		Local::writeSettings();
		Restart();
	};
	const auto keepDisabled = [=](Fn<void()> close) {
		Ui::GL::CrashCheckFinish();
		settings().setDisableOpenGL(true);
		Local::writeSettings();
		close();
	};
	_lastActivePrimaryWindow->show(Ui::MakeConfirmBox({
		.text = ""
		"There may be a problem with your graphics drivers and OpenGL. "
		"Try updating your drivers.\n\n"
		"OpenGL has been disabled. You can try to enable it again "
		"or keep it disabled if crashes continue.",
		.confirmed = enable,
		.cancelled = keepDisabled,
		.confirmText = "Enable",
		.cancelText = "Keep Disabled",
	}));
}

void Application::startDomain() {
	const auto state = _domain->start(QByteArray());
	if (state != Storage::StartResult::IncorrectPasscodeLegacy) {
		// In case of non-legacy passcoded app all global settings are ready.
		startSettingsAndBackground();
	}
	if (state != Storage::StartResult::Success) {
		lockByPasscode();
		DEBUG_LOG(("Application Info: passcode needed..."));
	}
}

void Application::startSettingsAndBackground() {
	Local::rewriteSettingsIfNeeded();
	Window::Theme::Background()->start();
	checkSystemDarkMode();
	Ui::SetScreenReaderModeDisabled(
		settings().readPref<bool>(kScreenReaderModeDisabledKey));
}

void Application::checkSystemDarkMode() {
	const auto maybeDarkMode = settings().systemDarkMode();
	const auto darkModeEnabled = settings().systemDarkModeEnabled();
	const auto needToSwitch = darkModeEnabled
		&& maybeDarkMode
		&& (*maybeDarkMode != Window::Theme::IsNightMode());
	if (needToSwitch) {
		Window::Theme::ToggleNightMode();
		Window::Theme::KeepApplied();
	}
}

void Application::startSystemDarkModeViewer() {
	if (Window::Theme::Background()->editingTheme()) {
		settings().setSystemDarkModeEnabled(false);
	}
	rpl::merge(
		settings().systemDarkModeChanges() | rpl::to_empty,
		settings().systemDarkModeEnabledChanges() | rpl::to_empty
	) | rpl::on_next([=] {
		checkSystemDarkMode();
	}, _lifetime);
}

void Application::enumerateWindows(Fn<void(
		not_null<Window::Controller*>)> callback) const {
	for (const auto &window : ranges::views::values(_windows)) {
		callback(window.get());
	}
}

void Application::processCreatedWindow(
		not_null<Window::Controller*> window) {
	if (_savedWindows) {
		_savedWindows->attachToWindow(window);
	}
	window->openInMediaViewRequests(
	) | rpl::start_to_stream(_openInMediaViewRequests, window->lifetime());
}

void Application::startMediaView() {
#ifdef Q_OS_MAC
	// On macOS we create some windows async, otherwise they're
	// added to the Dock Menu as a visible window and are removed
	// only after first show and then hide.
	InvokeQueued(this, [=] {
		_mediaView = std::make_unique<Media::View::OverlayWidget>();
		_mediaView->setSystemMediaControls(_mediaControlsManager.get());
	});
#elif defined Q_OS_WIN // Q_OS_MAC || Q_OS_WIN
	// On Windows we needed such hack for the main window, otherwise
	// somewhere inside the media viewer creating code its geometry
	// was broken / lost to some invalid values.
	const auto current = _lastActivePrimaryWindow->widget()->geometry();
	_mediaView = std::make_unique<Media::View::OverlayWidget>();
	_mediaView->setSystemMediaControls(_mediaControlsManager.get());
	_lastActivePrimaryWindow->widget()->Ui::RpWidget::setGeometry(current);
#else
	_mediaView = std::make_unique<Media::View::OverlayWidget>();
	_mediaView->setSystemMediaControls(_mediaControlsManager.get());
#endif // Q_OS_MAC || Q_OS_WIN
}

void Application::startTray() {
#ifdef Q_OS_MAC
	// On macOS we create some windows async, otherwise they're
	// added to the Dock Menu as a visible window and are removed
	// only after first show and then hide, tray icon being "Item-0".
	InvokeQueued(this, [=] {
		createTray();
	});
#else // Q_OS_MAC
	createTray();
#endif // Q_OS_MAC
}

void Application::createTray() {
	using WindowRaw = not_null<Window::Controller*>;
	_tray->create();
	_tray->aboutToShowRequests(
	) | rpl::on_next([=] {
		enumerateWindows([&](WindowRaw w) { w->updateIsActive(); });
		_tray->updateMenuText();
	}, _lifetime);

	_tray->showFromTrayRequests(
	) | rpl::on_next([=] {
		activate();
	}, _lifetime);

	_tray->hideToTrayRequests(
	) | rpl::on_next([=] {
		enumerateWindows([&](WindowRaw w) {
			w->widget()->minimizeToTray();
		});
	}, _lifetime);
}

void Application::showKukagramPreferences() {
#ifdef Q_OS_MAC
	if (!_domain || !_domain->started() || passcodeLocked() || Core::Quitting()) {
		return;
	}
	if (const auto modal = QApplication::activeModalWidget()) {
		modal->raise();
		modal->activateWindow();
		return;
	}
	activate();
	ShowKukagramNotificationAccountsDialog(not_null(_domain.get()));
#endif // Q_OS_MAC
}


#ifdef Q_OS_MAC
void Application::showKukagramSwitchToast(not_null<Main::Account*> account) {
	static auto first = true;
	if (base::take(first)) {
		return;
	}
	_kukagramToastLifetime.destroy();
	account->sessionValue(
	) | rpl::filter([](Main::Session *session) {
		return session != nullptr;
	}) | rpl::take(1) | rpl::on_next([=](Main::Session *session) {
		if (!KukagramAppearance::Current().switchToast
			|| !_lastActivePrimaryWindow) {
			return;
		}
		const auto id = session->uniqueId();
		auto name = session->user()->name();
		const auto labels = QSettings(
			QDir(cWorkingDir()).filePath(u"account-labels.ini"_q),
			QSettings::IniFormat);
		const auto alias = labels.value(QString::number(id)).toString().trimmed();
		if (!alias.isEmpty()) {
			name = alias + u" · "_q + name;
		}
		auto ids = std::vector<quint64>();
		auto groupName = QString();
		const auto &menuGroup = KukagramAccountGroups::MenuGroup().current();
		if (menuGroup && ranges::contains(menuGroup->members, id)) {
			groupName = menuGroup->name;
			ids = menuGroup->members;
		} else {
			for (const auto &stored : _domain->storedAccounts()) {
				if (stored.uniqueId) {
					ids.push_back(stored.uniqueId);
				}
			}
		}
		const auto position = ranges::find(ids, id);
		auto details = QStringList();
		if (!groupName.isEmpty()) {
			details.push_back(u"группа «%1»"_q.arg(groupName));
		}
		if (position != end(ids)) {
			details.push_back(u"%1 из %2"_q
				.arg(int(position - begin(ids)) + 1)
				.arg(ids.size()));
		}
		auto text = TextWithEntities{ u"→ "_q + name };
		text.entities.push_back(
			{ EntityType::Bold, 0, int(text.text.size()) });
		if (!details.isEmpty()) {
			text.text += u"\n"_q + details.join(u" · "_q);
		}
		_lastActivePrimaryWindow->showToast(std::move(text), crl::time(1500));
	}, _kukagramToastLifetime);
}
#endif // Q_OS_MAC

void Application::showKukagramAppearance() {
#ifdef Q_OS_MAC
	if (!_domain || !_domain->started() || passcodeLocked() || Core::Quitting()) {
		return;
	}
	if (const auto modal = QApplication::activeModalWidget()) {
		modal->raise();
		modal->activateWindow();
		return;
	}
	activate();
	ShowKukagramAppearanceDialog();
#endif // Q_OS_MAC
}

void Application::activate() {
	for (const auto &window : _windowStack) {
		if (window == _lastActiveWindow) {
			break;
		}
		const auto widget = window->widget();
		const auto wasHidden = !widget->isVisible();
		const auto state = widget->windowState();
		if (state & Qt::WindowMinimized) {
			widget->setWindowState(state & ~Qt::WindowMinimized);
		}
		widget->setVisible(true);
		widget->activateWindow();
		if (wasHidden) {
			if (const auto session = window->sessionController()) {
				session->content()->windowShown();
			}
		}
	}
	if (_lastActiveWindow) {
		_lastActiveWindow->widget()->showFromTray();
	}
}

auto Application::prepareEmojiSourceImages()
-> std::shared_ptr<Ui::Emoji::UniversalImages> {
	const auto &images = Ui::Emoji::SourceImages();
	Ui::Emoji::ClearSourceImages(images);
	if (settings().largeEmoji()) {
		return images;
	}
	return std::make_shared<Ui::Emoji::UniversalImages>(images->id());
}

void Application::clearEmojiSourceImages() {
	_emojiImageLoader.with([](Stickers::EmojiImageLoader &loader) {
		crl::on_main([images = loader.releaseImages()]{
			Ui::Emoji::ClearSourceImages(images);
		});
	});
}

bool Application::isActiveForTrayMenu() const {
	return ranges::any_of(ranges::views::values(_windows), [=](
			const std::unique_ptr<Window::Controller> &controller) {
		return controller->widget()->isActiveForTrayMenu();
	});
}

bool Application::hideMediaView() {
	if (_mediaView
		&& _mediaView->isFullScreen()
		&& !_mediaView->isMinimized()
		&& !_mediaView->isHidden()) {
		_mediaView->close();
		return true;
	}
	return false;
}

bool Application::eventFilter(QObject *object, QEvent *e) {
	switch (e->type()) {
	case QEvent::KeyPress: {
		updateNonIdle();
		_inAppKeyPressed.fire({});
		const auto event = static_cast<QKeyEvent*>(e);
#ifdef Q_OS_MAC
		if (!QApplication::activeModalWidget()
			&& !QApplication::activePopupWidget()
			&& _domain->started() && !passcodeLocked()) {
			if (const auto action = MatchKukagramShortcut(event)) {
				if (*action == KukagramAction::Next
					|| *action == KukagramAction::Previous) {
					_domain->cycleStoredAccount(
						*action == KukagramAction::Next ? 1 : -1);
				} else {
					InvokeQueued(this, [=] {
						if (*action == KukagramAction::Search) {
							ShowKukagramAccountSearch();
							return;
						} else if (*action == KukagramAction::Grid) {
							if (KukagramAppearance::Current().accountGrid) {
								ShowKukagramAccountGrid();
							} else {
								ShowKukagramAccountSearch();
							}
							return;
						}
						showKukagramPreferences();
					});
				}
				return true;
			}
		}
#endif // Q_OS_MAC
		if (base::Platform::GlobalShortcuts::IsToggleFullScreenKey(event)
			&& toggleActiveWindowFullScreen()) {
			return true;
		} else if (Shortcuts::HandlePossibleChatSwitch(event)) {
			return true;
		} else if (HandleKeysLegendHotkey(this, object, event)) {
			return true;
		}
	} break;
	case QEvent::MouseButtonPress:
	case QEvent::TouchBegin:
	case QEvent::Wheel: {
		updateNonIdle();
	} break;

	case QEvent::KeyRelease: {
		const auto event = static_cast<QKeyEvent*>(e);
		if (Shortcuts::HandlePossibleChatSwitch(event)) {
			return true;
		}
	} break;

	case QEvent::ShortcutOverride: {
#ifdef Q_OS_MAC
		if (_domain->started() && !passcodeLocked()
			&& !QApplication::activeModalWidget()
			&& !QApplication::activePopupWidget()) {
			const auto event = static_cast<QKeyEvent*>(e);
			if (MatchKukagramShortcut(event)) {
				event->accept();
				return true;
			}
		}
#endif // Q_OS_MAC
		// Ctrl+Tab/Ctrl+Shift+Tab chat switch is a special shortcut case,
		// because it not only does an action on the shortcut activation,
		// but also keeps the UI visible until you release the Ctrl key.
		Shortcuts::HandlePossibleChatSwitch(static_cast<QKeyEvent*>(e));

		// Handle all the shortcut management manually.
		return true;
	} break;

	case QEvent::Shortcut: {
		const auto event = static_cast<QShortcutEvent*>(e);
		DEBUG_LOG(("Shortcut event caught: %1"
			).arg(event->key().toString()));
		if (Shortcuts::HandleEvent(object, event)) {
			return true;
		}
	} break;

	case QEvent::ApplicationActivate: {
		if (object == QCoreApplication::instance()) {
			updateNonIdle();
		}
	} break;

	case QEvent::FileOpen: {
		if (object == QCoreApplication::instance()) {
			if (_urlsToOpen.isEmpty()) {
				InvokeQueued(this, [=] {
					const auto activateRequired = ranges::any_of(
						ranges::views::all(
							_urlsToOpen
						 ) | ranges::views::transform([](const QUrl &url) {
							return url.toString();
						}),
						StartUrlRequiresActivate);
					cRefStartUrls() << base::take(_urlsToOpen);
					const auto activated = checkStartUrlsActivating();
					if (!activateRequired) {
						return;
					} else if (activated) {
						base::Platform::ActivateThisProcessWindow(
							activated->winId());
					} else if (_lastActivePrimaryWindow) {
						_lastActivePrimaryWindow->activate();
					}
				});
			}
			const auto event = static_cast<QFileOpenEvent*>(e);
			_urlsToOpen << event->url().toString(QUrl::FullyEncoded).mid(
				0,
				8192);
		}
	} break;

	case QEvent::ThemeChange: {
		if (Platform::IsLinux() && object == qApp) {
			refreshApplicationIcon();
			tray().updateIconCounters();
		}
	} break;
	}

	switch (e->type()) {
	case QEvent::TouchBegin:
		Ui::Integration::Instance().touchCounterIncrement();
		[[fallthrough]];
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd: {
		_lastTouchProcessed = object->isWidgetType();
	} break;

	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseMove: {
		const auto ev = static_cast<QMouseEvent*>(e);
		if (ev->source() == Qt::MouseEventSynthesizedBySystem) {
			const auto widget = static_cast<QWidget*>(object);
			if (_lastTouchProcessed
				|| (object->isWidgetType()
					&& widget->testAttribute(Qt::WA_AcceptTouchEvents))) {
				_lastMouseIgnored = true;
				return true;
			}
		}
		_lastMouseIgnored = false;
	} break;

	case QEvent::ContextMenu: {
		const auto ev = static_cast<QContextMenuEvent*>(e);
		return (ev->reason() == QContextMenuEvent::Mouse)
			&& _lastMouseIgnored;
	} break;
	}

	return QObject::eventFilter(object, e);
}

Settings &Application::settings() {
	return _private->settings;
}

const Settings &Application::settings() const {
	return _private->settings;
}

void Application::saveSettingsDelayed(crl::time delay) {
	if (_saveSettingsTimer) {
		_saveSettingsTimer->callOnce(delay);
	}
}

void Application::saveSettings() {
	Local::writeSettings();
}

bool Application::canReadDefaultDownloadPath() const {
	return KSandbox::isInside()
		? base::CanReadDirectory(
			QStandardPaths::writableLocation(
				QStandardPaths::DownloadLocation))
		: true;
}

bool Application::canSaveFileWithoutAskingForPath() const {
	return !settings().askDownloadPath();
}

MTP::Config &Application::fallbackProductionConfig() const {
	if (!_fallbackProductionConfig) {
		_fallbackProductionConfig = std::make_unique<MTP::Config>(
			MTP::Environment::Production);
	}
	return *_fallbackProductionConfig;
}

void Application::refreshFallbackProductionConfig(
		const MTP::Config &config) {
	if (config.environment() == MTP::Environment::Production) {
		_fallbackProductionConfig = std::make_unique<MTP::Config>(config);
	}
}

void Application::constructFallbackProductionConfig(
		const QByteArray &serialized) {
	if (auto config = MTP::Config::FromSerialized(serialized)) {
		if (config->environment() == MTP::Environment::Production) {
			_fallbackProductionConfig = std::move(config);
		}
	}
}

void Application::setCurrentProxy(
		const MTP::ProxyData &proxy,
		MTP::ProxyData::Settings settings) {
	auto &my = _private->settings.proxy();
	const auto current = [&] {
		return my.isEnabled() ? my.selected() : MTP::ProxyData();
	};
	const auto was = current();
	my.setSelected(proxy);
	my.setSettings(settings);
	const auto now = current();
	if (_badProxyConfiguration && *_badProxyConfiguration != now) {
		_badProxyConfiguration.reset();
		if (const auto box = base::take(_badProxyDisableBox)) {
			box->closeBox();
		}
	}
	refreshGlobalProxy();
	_proxyChanges.fire({ was, now });
	my.connectionTypeChangesNotify();
	proxyRotationSettingsChanged();
}

void Application::proxyRotationSettingsChanged() {
	_private->proxyRotation->settingsChanged();
}

void Application::checkProxyRotation(
		not_null<Main::Account*> account,
		int32 state) {
	_private->proxyRotation->handleConnectionStateChanged(account, state);
}

auto Application::proxyChanges() const -> rpl::producer<ProxyChange> {
	return _proxyChanges.events();
}

void Application::badMtprotoConfigurationError(const MTP::ProxyData &proxy) {
	const auto &current = settings().proxy();
	if (_badProxyDisableBox
		|| _badProxyConfiguration == proxy
		|| !current.isEnabled()
		|| current.selected() != proxy) {
		return;
	}
	_badProxyConfiguration = proxy;
	_badProxyDisableBox = Ui::show(Ui::MakeConfirmBox({
		.text = Lang::Hard::ProxyConfigError(),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto &current = settings().proxy();
			if (current.isEnabled() && current.selected() == proxy) {
				setCurrentProxy(
					current.selected(),
					MTP::ProxyData::Settings::System);
			}
		},
		.confirmText = tr::lng_proxy_disable(),
		.cancelText = tr::lng_cancel(),
	}), Ui::LayerOption::KeepOther);
	if (!_badProxyDisableBox) {
		_badProxyConfiguration.reset();
	}
}

void Application::startLocalStorage() {
	Ui::GL::DetectLastCheckCrash();
	Local::start();
	_saveSettingsTimer.emplace([=] { saveSettings(); });
	settings().saveDelayedRequests() | rpl::on_next([=] {
		saveSettingsDelayed();
	}, _lifetime);
}

void Application::startEmojiImageLoader() {
	_emojiImageLoader.with([
		source = prepareEmojiSourceImages()
	](Stickers::EmojiImageLoader &loader) mutable {
		loader.switchTo(std::move(source));
	});

	settings().largeEmojiChanges(
	) | rpl::on_next([=](bool large) {
		if (large) {
			_clearEmojiImageLoaderTimer.cancel();
		} else {
			_clearEmojiImageLoaderTimer.callOnce(
				kClearEmojiImageSourceTimeout);
		}
	}, _lifetime);

	Ui::Emoji::Updated(
	) | rpl::on_next([=] {
		_emojiImageLoader.with([
			source = prepareEmojiSourceImages()
		](Stickers::EmojiImageLoader &loader) mutable {
			loader.switchTo(std::move(source));
		});
	}, _lifetime);
}

void Application::setScreenIsLocked(bool locked) {
	_screenIsLocked = locked;
}

bool Application::screenIsLocked() const {
	return _screenIsLocked.current();
}

rpl::producer<bool> Application::screenIsLockedValue() const {
	return _screenIsLocked.value();
}

void Application::notifySystemSleep() {
	_systemSleep.fire({});
}

rpl::producer<> Application::systemSleepEvents() const {
	return _systemSleep.events();
}

void Application::floatPlayerToggleGifsPaused(bool paused) {
	_floatPlayerGifsPaused = paused;
	if (_lastActiveWindow) {
		if (const auto delegate = _lastActiveWindow->floatPlayerDelegate()) {
			delegate->floatPlayerToggleGifsPaused(paused);
		}
	}
}

rpl::producer<FullMsgId> Application::floatPlayerClosed() const {
	Expects(_floatPlayers != nullptr);

	return _floatPlayers->closeEvents();
}

void Application::logout(Main::Account *account) {
	if (account) {
		account->logOut();
	} else {
		_domain->resetWithForgottenPasscode();
	}
}

void Application::logoutWithChecks(Main::Account *account) {
	const auto weak = base::make_weak(account);
	const auto retry = [=] {
		if (const auto account = weak.get()) {
			logoutWithChecks(account);
		}
	};
	if (!account || !account->sessionExists()) {
		logout(account);
	} else if (_exportManager->inProgress(&account->session())) {
		_exportManager->stopWithConfirmation(retry);
	} else if (account->session().uploadsInProgress()) {
		account->session().uploadsStopWithConfirmation(retry);
	} else if (_downloadManager->loadingInProgress(&account->session())) {
		_downloadManager->loadingStopWithConfirmation(
			retry,
			&account->session());
	} else {
		logout(account);
	}
}

void Application::forceLogOut(
		not_null<Main::Account*> account,
		const TextWithEntities &explanation) {
	const auto box = Ui::show(Ui::MakeConfirmBox({
		.text = explanation,
		.confirmText = tr::lng_passcode_logout(tr::now),
		.inform = true,
	}));
	box->setCloseByEscape(false);
	box->setCloseByOutsideClick(false);
	const auto weak = base::make_weak(account);
	connect(box.get(), &QObject::destroyed, [=] {
		crl::on_main(weak, [=] {
			account->forcedLogOut();
		});
	});
}

void Application::checkLocalTime() {
	const auto adjusted = crl::adjust_time();
	if (adjusted) {
		base::Timer::Adjust();
		base::ConcurrentTimerEnvironment::Adjust();
		base::unixtime::http_invalidate();
	}
	if (const auto session = maybePrimarySession()) {
		session->updates().checkLastUpdate(adjusted);
	}
}

void Application::handleAppActivated() {
	checkLocalTime();
	if (_lastActiveWindow) {
		_lastActiveWindow->updateIsActiveFocus();
	}
}

void Application::handleAppDeactivated() {
	enumerateWindows([&](not_null<Window::Controller*> w) {
		w->updateIsActiveBlur();
	});
	const auto session = _lastActiveWindow
		? _lastActiveWindow->maybeSession()
		: nullptr;
	if (session) {
		session->updates().updateOnline();
	}
	Ui::Tooltip::Hide();
}

rpl::producer<bool> Application::appDeactivatedValue() const {
	const auto &app
		= static_cast<QGuiApplication*>(QCoreApplication::instance());
	return rpl::single(
		app->applicationState()
	) | rpl::then(
		base::qt_signal_producer(
			app,
			&QGuiApplication::applicationStateChanged
	)) | rpl::map([=](Qt::ApplicationState state) {
		return (state != Qt::ApplicationActive);
	});
}

void Application::materializeLocalDrafts() {
	_materializeLocalDraftsRequests.fire({});
}

rpl::producer<> Application::materializeLocalDraftsRequests() const {
	return _materializeLocalDraftsRequests.events();
}

void Application::switchDebugMode() {
	if (Logs::DebugEnabled()) {
		Logs::SetDebugEnabled(false);
		Launcher::Instance().writeDebugModeSetting();
		Restart();
	} else {
		Logs::SetDebugEnabled(true);
		Launcher::Instance().writeDebugModeSetting();
		DEBUG_LOG(("Debug logs started."));
		if (_lastActivePrimaryWindow) {
			_lastActivePrimaryWindow->hideLayer();
		}
	}
}

Main::Account &Application::activeAccount() const {
	return _domain->active();
}

Main::Session *Application::maybePrimarySession() const {
	return _domain->started() ? activeAccount().maybeSession() : nullptr;
}

bool Application::exportPreventsQuit(Fn<void()> retry) {
	if (_exportManager->inProgress()) {
		if (!retry) {
			retry = [] { Quit(); };
		}
		_exportManager->stopWithConfirmation(std::move(retry));
		return true;
	}
	return false;
}

bool Application::uploadPreventsQuit(Fn<void()> retry) {
	if (!_domain->started()) {
		return false;
	}
	for (const auto &[index, account] : _domain->accounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		if (account->session().uploadsInProgress()) {
			account->session().uploadsStopWithConfirmation([=] {
				for (const auto &[index, account] : _domain->accounts()) {
					if (account->sessionExists()) {
						account->session().uploadsStop();
					}
				}
				if (retry) {
					retry();
				} else {
					Quit();
				}
			});
			return true;
		}
	}
	return false;
}

bool Application::downloadPreventsQuit(Fn<void()> retry) {
	if (_downloadManager->loadingInProgress()) {
		if (!retry) {
			retry = [] { Quit(); };
		}
		_downloadManager->loadingStopWithConfirmation(std::move(retry));
		return true;
	}
	return false;
}

bool Application::preventsQuit(QuitReason reason, Fn<void()> retry) {
	if (exportPreventsQuit(retry)
		|| uploadPreventsQuit(retry)
		|| downloadPreventsQuit(std::move(retry))) {
		return true;
	} else if ((!_mediaView
		|| _mediaView->isHidden()
		|| !_mediaView->isFullScreen())
		&& Platform::PreventsQuit(reason)) {
		return true;
	}
	return false;
}

int Application::unreadBadge() const {
	return _domain->unreadBadge();
}

bool Application::unreadBadgeMuted() const {
	return _domain->unreadBadgeMuted();
}

rpl::producer<> Application::unreadBadgeChanges() const {
	return _domain->unreadBadgeChanges();
}

bool Application::offerLegacyLangPackSwitch() const {
	return (_domain->accounts().size() == 1)
		&& activeAccount().sessionExists();
}

bool Application::canApplyLangPackWithoutRestart() const {
	for (const auto &[index, account] : _domain->accounts()) {
		if (account->sessionExists()) {
			return false;
		}
	}
	return true;
}

void Application::checkStartUrls() {
	if (_setupEmailLock.current()) {
		return;
	}
	if (!Core::App().passcodeLocked()) {
		// WHY: tg://resolve?acc= switches the account, which shows the
		// new main widget and re-enters here while we still iterate,
		// so take the list out and merge whatever was added meanwhile.
		const auto urls = base::take(cRefStartUrls());
		auto left = ranges::views::all(
			urls
		) | ranges::views::filter([&](const QUrl &url) {
			if (url.scheme() == u"tonsite"_q) {
				iv().showTonSite(url.toString(), {});
				return false;
			} else if (url.scheme() == u"ton"_q) {
				const auto window = _lastActivePrimaryWindow;
				const auto controller = (window && !window->locked())
					? window->sessionController()
					: nullptr;
				if (!controller) {
					return true;
				}
				Wallet::OpenTransferLink(
					controller,
					url.toString(QUrl::FullyEncoded));
				return false;
			} else if (_lastActivePrimaryWindow) {
				const auto local = TryConvertUrlToLocal(url.toString());
				return IsGetPremiumLocalUrl(local)
					? !openInternalUrl(local, {})
					: !openLocalUrl(local, {});
			}
			return true;
		}) | ranges::to<QList<QUrl>>;
		left.append(base::take(cRefStartUrls()));
		cRefStartUrls() = std::move(left);
	}
	if (!cRefStartUrls().isEmpty()
		&& _lastActivePrimaryWindow
		&& !_lastActivePrimaryWindow->locked()) {
		auto paths = QStringList();
		cRefStartUrls() = ranges::views::all(
			cRefStartUrls()
		) | ranges::views::filter([&](const QUrl &url) {
			if (url.isLocalFile()) {
				paths.append(url.toLocalFile());
				return false;
			}
			return true;
		}) | ranges::to<QList<QUrl>>;
		if (!paths.isEmpty()) {
			_lastActivePrimaryWindow->widget()->handleStartFiles(
				std::move(paths));
		}
	}
}

QPointer<QWidget> Application::checkStartUrlsActivating() {
	if (_startUrlActivated) {
		checkStartUrls();
		return nullptr;
	}
	_startUrlActivated.emplace();
	checkStartUrls();
	return *base::take(_startUrlActivated);
}

void Application::registerWindowActivation(not_null<QWidget*> window) {
	if (_startUrlActivated) {
		*_startUrlActivated = window.get();
	}
}

bool Application::openLocalUrl(const QString &url, QVariant context) {
	const auto urlTrimmed = url.trimmed();
	const auto protocol = u"tg://"_q;
	if (urlTrimmed.startsWith(protocol, Qt::CaseInsensitive)
		&& !passcodeLocked()) {
		const auto command = urlTrimmed.mid(protocol.size());
		const auto my = context.value<ClickHandlerContext>();
		const auto controller = my.sessionWindow.get()
			? my.sessionWindow.get()
			: _lastActivePrimaryWindow
			? _lastActivePrimaryWindow->sessionController()
			: nullptr;
		if (TryRouterForLocalUrl(controller, command)) {
			return true;
		}
	}
	return openCustomUrl("tg://", LocalUrlHandlers(), url, context);
}

bool Application::openInternalUrl(const QString &url, QVariant context) {
	return openCustomUrl("internal:", InternalUrlHandlers(), url, context);
}

QString Application::changelogLink() const {
	return u"https://telegramdesktop.github.io/tdesktop/changelog/"_q;
}

bool Application::openCustomUrl(
		const QString &protocol,
		const std::vector<LocalUrlHandler> &handlers,
		const QString &url,
		const QVariant &context) {
	const auto urlTrimmed = url.trimmed();
	if (!urlTrimmed.startsWith(protocol, Qt::CaseInsensitive)
		|| passcodeLocked()) {
		return false;
	}
	auto skip = protocol.size();
	if (protocol == u"internal:"_q) {
		static const auto kTagExp = QRegularExpression(
			u"^\\~[a-zA-Z0-9_\\-]+\\~:"_q);
		const auto match = kTagExp.match(
			base::StringViewMid(urlTrimmed, skip));
		if (match.hasMatch()) {
			skip += match.capturedLength();
		}
	}
	const auto command = base::StringViewMid(urlTrimmed, skip, 8192);
	const auto my = context.value<ClickHandlerContext>();
	const auto controller = my.sessionWindow.get()
		? my.sessionWindow.get()
		: _lastActivePrimaryWindow
		? _lastActivePrimaryWindow->sessionController()
		: nullptr;

	using namespace qthelp;
	const auto options = RegExOption::CaseInsensitive;
	for (const auto &[expression, handler] : handlers) {
		const auto match = regex_match(expression, command, options);
		if (match) {
			return handler(controller, match, context);
		}
	}
	return false;
}

void Application::preventOrInvoke(Fn<void()> &&callback) {
	_lastActivePrimaryWindow->preventOrInvoke(std::move(callback));
}

void Application::updateWindowTitles() {
	enumerateWindows([](not_null<Window::Controller*> window) {
		window->widget()->updateTitle();
	});
}

void Application::lockByPasscode() {
	_passcodeLock = true;
	enumerateWindows([&](not_null<Window::Controller*> w) {
		w->setupPasscodeLock();
	});
	if (_mediaView) {
		_mediaView->close();
	}
	_calls->hidePanelLayers();
}

void Application::maybeLockByPasscode() {
	preventOrInvoke([=] {
		lockByPasscode();
	});
}

void Application::unlockPasscode() {
	clearPasscodeLock();
	enumerateWindows([&](not_null<Window::Controller*> w) {
		w->clearPasscodeLock();
	});
}

void Application::clearPasscodeLock() {
	cSetPasscodeBadTries(0);
	_passcodeLock = false;
}

bool Application::passcodeLocked() const {
	return _passcodeLock.current();
}

void Application::updateNonIdle() {
	_lastNonIdleTime = crl::now();
	if (const auto session = maybePrimarySession()) {
		session->updates().checkIdleFinish(_lastNonIdleTime);
	}
}

crl::time Application::lastNonIdleTime() const {
	return std::max(
		base::Platform::LastUserInputTime().value_or(0),
		_lastNonIdleTime);
}

rpl::producer<> Application::inAppKeyPressed() const {
	return _inAppKeyPressed.events();
}

rpl::producer<bool> Application::passcodeLockChanges() const {
	return _passcodeLock.changes();
}

rpl::producer<bool> Application::passcodeLockValue() const {
	return _passcodeLock.value();
}

void Application::lockBySetupEmail() {
	_setupEmailLock = true;
	closeAdditionalWindows();
	enumerateWindows([&](not_null<Window::Controller*> w) {
		w->setupSetupEmailLock();
	});
}

void Application::unlockSetupEmail() {
	_setupEmailLock = false;
	enumerateWindows([&](not_null<Window::Controller*> w) {
		w->clearSetupEmailLock();
	});
	checkStartUrls();
}

bool Application::someSessionExists() const {
	for (const auto &[index, account] : _domain->accounts()) {
		if (account->sessionExists()) {
			return true;
		}
	}
	return false;
}

void Application::checkAutoLock(crl::time lastNonIdleTime) {
	// Cached verification stops planted bytes from disarming idle locking.
	if (!_domain->local().hasLocalPasscode()
		|| passcodeLocked()
		|| !someSessionExists()) {
		_shouldLockAt = 0;
		_autoLockTimer.cancel();
		return;
	} else if (!lastNonIdleTime) {
		lastNonIdleTime = this->lastNonIdleTime();
	}

	checkLocalTime();
	const auto now = crl::now();
	const auto shouldLockInMs = settings().autoLock() * 1000LL;
	const auto checkTimeMs = now - lastNonIdleTime;
	if (checkTimeMs >= shouldLockInMs
		|| (_shouldLockAt > 0
			&& now > _shouldLockAt + kAutoLockTimeoutLateMs)) {
		_shouldLockAt = 0;
		_autoLockTimer.cancel();
		lockByPasscode();
	} else {
		_shouldLockAt = now + (shouldLockInMs - checkTimeMs);
		_autoLockTimer.callOnce(shouldLockInMs - checkTimeMs);
	}
}

void Application::checkAutoLockIn(crl::time time) {
	if (_autoLockTimer.isActive()) {
		auto remain = _autoLockTimer.remainingTime();
		if (remain > 0 && remain <= time) return;
	}
	_autoLockTimer.callOnce(time);
}

void Application::localPasscodeChanged() {
	_shouldLockAt = 0;
	_autoLockTimer.cancel();
	checkAutoLock(crl::now());
}

bool Application::savingPositionFor(
		not_null<Window::Controller*> window) const {
	return !_windowInSettings || (_windowInSettings == window);
}

bool Application::hasActiveWindow(not_null<Main::Session*> session) const {
	if (Quitting() || !_lastActiveWindow) {
		return false;
	} else if (_calls->hasActivePanel(session)) {
		return true;
	} else if (_iv->hasActiveWindow(session)) {
		return true;
	} else if (const auto window = _lastActiveWindow) {
		return (window->account().maybeSession() == session)
			&& window->widget()->isActive();
	}
	return false;
}

Window::Controller *Application::activePrimaryWindow() const {
	return _lastActivePrimaryWindow;
}

void Application::setActivePrimaryWindow(
		not_null<Window::Controller*> window) {
	if (window->isPrimary()) {
		_lastActivePrimaryWindow = window;
	}
}

Window::Controller *Application::separateWindowFor(
		Window::SeparateId id) const {
	for (const auto &[existingId, window] : _windows) {
		if (existingId == id) {
			return window.get();
		}
	}
	return nullptr;
}

not_null<Window::Controller*> Application::ensureSeparateWindowFor(
		Window::SeparateId id,
		MsgId showAtMsgId) {
	const auto activate = [&](not_null<Window::Controller*> window) {
		window->activate();
		return window;
	};
	if (const auto existing = separateWindowFor(id)) {
		if (id.thread
			&& id.type == Window::SeparateType::Chat
			&& !passcodeLocked()) {
			existing->sessionController()->showThread(
				id.thread,
				showAtMsgId,
				Window::SectionShow::Way::ClearStack);
		}
		return activate(existing);
	}

	Assert(Window::CanShowSeparateWindow(id));

	const auto result = _windows.emplace(
		id,
		std::make_unique<Window::Controller>(id, showAtMsgId)
	).first->second.get();
	processCreatedWindow(result);
	if (passcodeLocked()) {
		result->setupPasscodeLock();
	}
	result->firstShow();
	result->finishFirstShow();
	return activate(result);
}

Window::Controller *Application::windowFor(Window::SeparateId id) const {
	if (const auto separate = separateWindowFor(id)) {
		return separate;
	} else if (id && !id.primary()) {
		return windowFor(not_null(id.account));
	}
	return activePrimaryWindow();
}

Window::Controller *Application::windowForShowingHistory(
		not_null<PeerData*> peer) const {
	if (const auto separate = separateWindowFor(peer)) {
		return separate;
	}
	auto result = (Window::Controller*)nullptr;
	enumerateWindows([&](not_null<Window::Controller*> window) {
		if (const auto controller = window->sessionController()) {
			const auto current = controller->activeChatCurrent();
			if (const auto history = current.history()) {
				if (history->peer == peer) {
					result = window;
				}
			}
		}
	});
	return result;
}

Window::Controller *Application::windowForShowingForum(
		not_null<Data::Forum*> forum) const {
	const auto tabs = forum->bot() || forum->peer()->useSubsectionTabs();
	const auto id = Window::SeparateId(
		tabs ? Window::SeparateType::Chat : Window::SeparateType::Forum,
		forum->history());
	if (const auto separate = separateWindowFor(id)) {
		return separate;
	}
	auto result = (Window::Controller*)nullptr;
	enumerateWindows([&](not_null<Window::Controller*> window) {
		if (const auto controller = window->sessionController()) {
			if (tabs) {
				if (controller->windowId() == id) {
					result = window;
				}
			} else {
				const auto current = controller->shownForum().current();
				if (forum == current) {
					result = window;
				}
			}
		}
	});
	return result;
}

Window::Controller *Application::findWindow(
		not_null<QWidget*> widget) const {
	const auto window = widget->window();
	if (_lastActiveWindow && _lastActiveWindow->widget() == window) {
		return _lastActiveWindow;
	}
	for (const auto &[id, controller] : _windows) {
		if (controller->widget() == window) {
			return controller.get();
		}
	}
	return nullptr;
}

Window::Controller *Application::activeWindow() const {
	return _lastActiveWindow;
}

bool Application::closeNonLastAsync(not_null<Window::Controller*> window) {
	const auto hasOther = [&] {
		for (const auto &[id, controller] : _windows) {
			if (id.primary()
				&& !_closingAsyncWindows.contains(controller.get())
				&& controller.get() != window
				&& controller->maybeSession()) {
				return true;
			}
		}
		return false;
	}();
	if (!hasOther) {
		return false;
	}
	_closingAsyncWindows.emplace(window);
	crl::on_main(window, [=] { closeWindow(window); });
	return true;
}

void Application::setLastActiveWindow(Window::Controller *window) {
	_floatPlayerDelegateLifetime.destroy();

	if (_floatPlayerGifsPaused && _lastActiveWindow) {
		if (const auto delegate = _lastActiveWindow->floatPlayerDelegate()) {
			delegate->floatPlayerToggleGifsPaused(false);
		}
	}
	_lastActiveWindow = window;
	if (window) {
		const auto i = ranges::find(_windowStack, not_null(window));
		if (i == end(_windowStack)) {
			_windowStack.push_back(window);
		} else if (i + 1 != end(_windowStack)) {
			std::rotate(i, i + 1, end(_windowStack));
		}
	}
	if (!window) {
		_floatPlayers = nullptr;
		return;
	}
	window->floatPlayerDelegateValue(
	) | rpl::on_next([=](Media::Player::FloatDelegate *value) {
		if (!value) {
			_floatPlayers = nullptr;
		} else if (_floatPlayers) {
			_floatPlayers->replaceDelegate(value);
		} else if (value) {
			_floatPlayers = std::make_unique<Media::Player::FloatController>(
				value);
		}
		if (value && _floatPlayerGifsPaused) {
			value->floatPlayerToggleGifsPaused(true);
		}
	}, _floatPlayerDelegateLifetime);
}

void Application::closeWindow(not_null<Window::Controller*> window) {
	if (_savedWindows) {
		_savedWindows->windowClosed(window);
	}
	const auto stackIt = ranges::find(_windowStack, window);
	const auto nextFromStack = _windowStack.empty()
		? nullptr
		: (stackIt == end(_windowStack) || stackIt + 1 != end(_windowStack))
		? _windowStack.back().get()
		: (_windowStack.size() > 1)
		? (stackIt - 1)->get()
		: nullptr;
	const auto next = nextFromStack
		? nextFromStack
		: (_windows.front().second.get() != window)
		? _windows.front().second.get()
		: (_windows.back().second.get() != window)
		? _windows.back().second.get()
		: nullptr;
	Assert(next != window);

	if (_lastActivePrimaryWindow == window) {
		_lastActivePrimaryWindow = next;
	}
	if (_windowInSettings == window) {
		_windowInSettings = next;
	}
	if (stackIt != end(_windowStack)) {
		_windowStack.erase(stackIt);
	}
	if (_lastActiveWindow == window) {
		setLastActiveWindow(next);
		if (_lastActiveWindow) {
			_lastActiveWindow->activate();
			_lastActiveWindow->widget()->updateGlobalMenu();
		}
	}
	_closingAsyncWindows.remove(window);
	for (auto i = begin(_windows); i != end(_windows);) {
		if (i->second.get() == window) {
			Assert(_lastActiveWindow != window);
			Assert(_lastActivePrimaryWindow != window);
			Assert(_windowInSettings != window);
			i = _windows.erase(i);
		} else {
			++i;
		}
	}
	const auto account = domain().started()
		? &domain().active()
		: nullptr;
	if (account
		&& !_windows.contains(Window::SeparateId(account))
		&& _lastActiveWindow) {
		domain().activate(&_lastActiveWindow->account());
	}
	if (_savedWindows) {
		_savedWindows->scheduleSave();
	}
}

void Application::closeChatFromWindows(not_null<PeerData*> peer) {
	const auto closeOne = [&] {
		for (const auto &[id, window] : _windows) {
			if (id.thread && id.thread->peer() == peer) {
				closeWindow(window.get());
				return true;
			} else if (const auto controller = window->sessionController()) {
				if (controller->activeChatCurrent().peer() == peer) {
					controller->showByInitialId();
				}
				if (const auto forum = controller->shownForum().current()) {
					if (peer->forum() == forum) {
						controller->closeForum();
					}
				}
			}
		}
		return false;
	};

	while (closeOne()) {
	}
}

void Application::windowActivated(not_null<Window::Controller*> window) {
	const auto was = _lastActiveWindow;
	const auto now = window;

	setLastActiveWindow(window);

	if (_savedWindows) {
		_savedWindows->windowActivated();
	}

	if (window->isPrimary()) {
		_lastActivePrimaryWindow = window;
	}
	window->widget()->updateGlobalMenu();

	const auto wasSession = was ? was->maybeSession() : nullptr;
	const auto nowSession = now->maybeSession();
	if (wasSession != nowSession) {
		if (wasSession) {
			wasSession->updates().updateOnline();
		}
		if (nowSession) {
			nowSession->updates().updateOnline();
		}
	}
	if (_mediaView && _mediaView->takeFocusFrom(now->widget())) {
		_mediaView->activate();
	}
}

bool Application::closeOtherWindows() {
	const auto keep = _lastActivePrimaryWindow
		? _lastActivePrimaryWindow
		: activeWindow();
	auto toClose = std::vector<not_null<Window::Controller*>>();
	for (const auto &[id, window] : _windows) {
		if (window.get() != keep) {
			toClose.push_back(window.get());
		}
	}
	for (const auto &window : toClose) {
		window->close();
	}
	if (keep && !toClose.empty()) {
		keep->activate();
	}
	return !toClose.empty();
}

bool Application::closeActiveWindow() {
	if (_mediaView && _mediaView->isActive()) {
		_mediaView->close();
		return true;
	} else if (_iv->closeActive()
		|| Iv::Editor::CloseActiveWindow()
		|| calls().closeCurrentActiveCall()
		|| (_savedWindows && _savedWindows->closeActiveShell())
		|| Wallet::CloseActiveWindow()) {
		return true;
	} else if (const auto window = activeWindow()) {
		if (window->widget()->isActive()) {
			window->close();
			return true;
		}
	}
	return false;
}

bool Application::minimizeActiveWindow() {
	if (_mediaView && _mediaView->isActive()) {
		_mediaView->minimize();
		return true;
	} else if (_iv->minimizeActive()
		|| Iv::Editor::MinimizeActiveWindow()
		|| calls().minimizeCurrentActiveCall()
		|| Wallet::MinimizeActiveWindow()) {
		return true;
	} else if (const auto window = activeWindow()) {
		if (window->widget()->isActive()) {
			window->minimize();
			return true;
		}
	}
	return false;
}

bool Application::toggleActiveWindowFullScreen() {
	if (_mediaView && _mediaView->isActive()) {
		_mediaView->toggleFullScreen();
		return true;
	} else if (calls().toggleFullScreenCurrentActiveCall()) {
		return true;
	} else if (const auto window = activeWindow()) {
		if constexpr (Platform::IsMac()) {
			if (window->widget()->isFullScreen()) {
				window->widget()->showNormal();
			} else {
				window->widget()->showFullScreen();
			}
			return true;
		}
	}
	return false;
}

QWidget *Application::getFileDialogParent() {
	if (const auto view = _mediaView.get(); view && !view->isHidden()) {
		return view->widget();
	} else if (const auto active = activeWindow()) {
		return active->widget();
	}
	return nullptr;
}

void Application::notifyFileDialogShown(bool shown) {
	if (_mediaView) {
		_mediaView->notifyFileDialogShown(shown);
	}
}

QPoint Application::getPointForCallPanelCenter() const {
	if (const auto window = activeWindow()) {
		return window->getPointForCallPanelCenter();
	}
	// When the last monitor is removed QGuiApplication has no screens at
	// all, so primaryScreen() is nullptr.
	const auto primary = QGuiApplication::primaryScreen();
	return primary ? primary->geometry().center() : QPoint();
}

bool Application::isSharingScreen() const {
	return _calls->isSharingScreen();
}

// macOS Qt bug workaround, sometimes no leaveEvent() gets to the nested widgets.
void Application::registerLeaveSubscription(not_null<QWidget*> widget) {
#ifdef Q_OS_MAC
	if (const auto window = widget->window()) {
		auto i = _leaveFilters.find(window);
		if (i == end(_leaveFilters)) {
			const auto check = [=](not_null<QEvent*> e) {
				if (e->type() == QEvent::Leave) {
					if (const auto taken = _leaveFilters.take(window)) {
						for (const auto &weak : taken->registered) {
							if (const auto widget = weak.get()) {
								QEvent ev(QEvent::Leave);
								QCoreApplication::sendEvent(widget, &ev);
							}
						}
						delete taken->filter.data();
					}
				}
				return base::EventFilterResult::Continue;
			};
			const auto filter = base::install_event_filter(window, check);
			QObject::connect(filter, &QObject::destroyed, [=] {
				_leaveFilters.remove(window);
			});
			i = _leaveFilters.emplace(
				window,
				LeaveFilter{ .filter = filter.get() }).first;
		}
		i->second.registered.push_back(widget.get());
	}
#endif // Q_OS_MAC
}

void Application::unregisterLeaveSubscription(not_null<QWidget*> widget) {
#ifdef Q_OS_MAC
	if (const auto topLevel = widget->window()) {
		const auto i = _leaveFilters.find(topLevel);
		if (i != end(_leaveFilters)) {
			i->second.registered = std::move(
				i->second.registered
			) | ranges::actions::remove_if([&](QPointer<QWidget> weak) {
				const auto pointer = weak.data();
				return !pointer || (pointer == widget);
			});
		}
	}
#endif // Q_OS_MAC
}

void Application::postponeCall(FnMut<void()> &&callable) {
	Sandbox::Instance().postponeCall(std::move(callable));
}

void Application::refreshGlobalProxy() {
	const auto &proxySettings = settings().proxy();
	const auto proxy = proxySettings.isEnabled()
		? proxySettings.selected()
		: MTP::ProxyData();
	if (proxy.type == MTP::ProxyData::Type::Web && proxy.valid()) {
		MTP::WebProxy::Transport::Activate(proxy);
	} else {
		MTP::WebProxy::Transport::Deactivate();
	}
	Sandbox::Instance().refreshGlobalProxy();
}

void QuitAttempt() {
	const auto savingSession = Sandbox::Instance().isSavingSession();
	if (!IsAppLaunched()
		|| savingSession
		|| App().readyToQuit()) {
		Sandbox::QuitWhenStarted();
	}
}

bool Application::readyToQuit() {
	auto prevented = false;
	if (_calls->isQuitPrevent()) {
		prevented = true;
	}
	if (Tgw::Runtime::IsQuitPrevent()) {
		prevented = true;
	}
	if (_domain->started()) {
		for (const auto &[index, account] : _domain->accounts()) {
			if (const auto session = account->maybeSession()) {
				if (session->updates().isQuitPrevent()) {
					prevented = true;
				}
				if (session->api().isQuitPrevent()) {
					prevented = true;
				}
				if (session->data().stories().isQuitPrevent()) {
					prevented = true;
				}
				if (session->data().reactions().isQuitPrevent()) {
					prevented = true;
				}
			}
		}
	}
	if (prevented) {
		quitDelayed();
		return false;
	}
	return true;
}

void Application::quitPreventFinished() {
	if (Quitting()) {
		QuitAttempt();
	}
}

void Application::quitDelayed() {
	for (const auto &[id, controller] : _windows) {
		controller->widget()->hide();
	}
	if (!_private->quitTimer.isActive()) {
		_private->quitTimer.setCallback([] { Sandbox::QuitWhenStarted(); });
		_private->quitTimer.callOnce(kQuitPreventTimeoutMs);
	}
}

void Application::refreshApplicationIcon() {
	const auto session = (domain().started() && domain().active().sessionExists())
		? &domain().active().session()
		: nullptr;
	refreshApplicationIcon(session);
}

void Application::refreshApplicationIcon(Main::Session *session) {
	const auto support = session && session->supportMode();
	Shortcuts::ToggleSupportShortcuts(support);
	Platform::SetApplicationIcon(Window::CreateIcon(
		session,
		Platform::IsMac()));
}

void Application::startShortcuts() {
	Shortcuts::Start();

	_domain->activeSessionChanges(
	) | rpl::on_next([=](Main::Session *session) {
		refreshApplicationIcon(session);
	}, _lifetime);

	Shortcuts::Requests(
	) | rpl::on_next([=](not_null<Shortcuts::Request*> request) {
		using Command = Shortcuts::Command;
		request->check(Command::Quit) && request->handle([] {
			Quit();
			return true;
		});
		request->check(Command::Lock) && request->handle([=] {
			// Keep manual locking available for a verified app lock.
			if (!passcodeLocked() && _domain->local().hasLocalPasscode()) {
				maybeLockByPasscode();
				return true;
			}
			return false;
		});
		request->check(Command::Minimize) && request->handle([=] {
			return minimizeActiveWindow();
		});
		request->check(Command::Close) && request->handle([=] {
			return closeActiveWindow();
		});
		request->check(Command::ReopenClosedWindow) && request->handle([=] {
			return _savedWindows && _savedWindows->reopenLastClosed();
		});
		request->check(Command::CloseOtherWindows) && request->handle([=] {
			return closeOtherWindows();
		});
	}, _lifetime);
}

void Application::RegisterUrlScheme() {
	const auto arguments = Launcher::Instance().customWorkingDir()
		? u"-workdir \"%1\""_q.arg(cWorkingDir())
		: QString();

	base::Platform::RegisterUrlScheme(base::Platform::UrlSchemeDescriptor{
		.executable = Platform::ExecutablePathForShortcuts(),
		.arguments = arguments,
		.protocol = u"tg"_q,
		.protocolName = u"Telegram Link"_q,
		.shortAppName = u"tdesktop"_q,
		.longAppName = QCoreApplication::applicationName(),
		.displayAppName = AppName.utf16(),
		.displayAppDescription = AppName.utf16(),
	});

	base::Platform::RegisterUrlScheme(base::Platform::UrlSchemeDescriptor{
		.executable = Platform::ExecutablePathForShortcuts(),
		.arguments = arguments,
		.protocol = u"tonsite"_q,
		.protocolName = u"TonSite Link"_q,
		.shortAppName = u"tdesktop"_q,
		.longAppName = QCoreApplication::applicationName(),
		.displayAppName = AppName.utf16(),
		.displayAppDescription = AppName.utf16(),
	});

	base::Platform::RegisterUrlScheme(base::Platform::UrlSchemeDescriptor{
		.executable = Platform::ExecutablePathForShortcuts(),
		.arguments = arguments,
		.protocol = u"ton"_q,
		.protocolName = u"Ton Link"_q,
		.shortAppName = u"tdesktop"_q,
		.longAppName = QCoreApplication::applicationName(),
		.displayAppName = AppName.utf16(),
		.displayAppDescription = AppName.utf16(),
	});
}

bool IsAppLaunched() {
	return (Application::Instance != nullptr);
}

Application &App() {
	Expects(Application::Instance != nullptr);

	return *Application::Instance;
}

void Quit(QuitReason reason) {
   if (Quitting()) {
	   return;
   } else if (IsAppLaunched() && App().preventsQuit(reason)) {
	   return;
   }
   SetLaunchState(LaunchState::QuitRequested);

   QuitAttempt();
}

bool Quitting() {
   return GlobalLaunchState != LaunchState::Running;
}

LaunchState CurrentLaunchState() {
   return GlobalLaunchState;
}

void SetLaunchState(LaunchState state) {
   GlobalLaunchState = state;
}

void Restart(bool toSettings, Fn<bool()> prepare) {
	if (Quitting()) {
		return;
	}
	if (IsAppLaunched() && App().preventsQuit(QuitReason::Default, [=] {
			Restart(toSettings, prepare);
		})) {
		return;
	}
	if (prepare && !prepare()) {
		return;
	}
	const auto updateReady = !UpdaterDisabled()
		&& (UpdateChecker().state() == UpdateChecker::State::Ready);
	if (updateReady) {
		cSetRestartingUpdate(true);
	} else {
		cSetRestarting(true);
		cSetRestartingToSettings(toSettings);
	}
	SetLaunchState(LaunchState::QuitRequested);
	QuitAttempt();
}

} // namespace Core
