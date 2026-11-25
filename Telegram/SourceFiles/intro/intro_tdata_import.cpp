/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_tdata_import.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/ui_utility.h"
#include "window/window_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_intro.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <atomic>

namespace Intro::details {
namespace {

std::atomic_flag ImportInProgress = ATOMIC_FLAG_INIT;

struct ImportContext {
	base::weak_qptr<Ui::RpWidget> parent;
	base::weak_ptr<Main::Account> pending;
	base::weak_ptr<Window::Controller> window;
};

[[nodiscard]] Window::Controller *ResolveWindow(const ImportContext &context) {
	if (Core::Quitting() || Core::App().passcodeLocked()) {
		return nullptr;
	}
	const auto parent = context.parent.get();
	const auto pending = context.pending.get();
	const auto window = context.window.get();
	return (parent && pending && window
		&& !pending->sessionExists() && !pending->loggingOut()
		&& window->id().account == pending
		&& Core::App().findWindow(parent) == window)
		? window
		: nullptr;
}

void ReadFolder(
	const ImportContext &context,
	QString path,
	QByteArray passcode);

void BindToParent(
		not_null<Ui::RpWidget*> parent,
		not_null<Ui::GenericBox*> box) {
	QObject::connect(parent, &QObject::destroyed, box, [=] {
		box->closeBox();
	});
}

void ShowImportError(
		const ImportContext &context,
		const QString &text) {
	if (Core::Quitting() || Core::App().passcodeLocked()) {
		return;
	}
	if (const auto window = context.window.get()) {
		window->show(Ui::MakeInformBox(text), Ui::LayerOption::CloseOther);
	}
}

void AskPasscode(
		const ImportContext &context,
		QString path,
		bool invalid) {
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	const auto parent = not_null(context.parent.get());
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		BindToParent(parent, box);
		box->setTitle(rpl::single(u"Пароль папки tdata"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			invalid
				? u"Неверный локальный пароль. Попробуйте снова."_q
				: u"Введите локальный код-пароль исходного Telegram Desktop. Это не пароль двухэтапной аутентификации."_q,
			st::boxLabel));
		const auto row = box->addRow(object_ptr<Ui::RpWidget>(box));
		row->resize(row->width(), st::defaultInputField.heightMin);
		const auto field = Ui::CreateChild<Ui::PasswordInput>(
			row,
			st::defaultInputField,
			rpl::single(u"Локальный пароль"_q));
		row->sizeValue() | rpl::on_next([=](QSize size) {
			field->resize(size.width(), field->height());
		}, row->lifetime());
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto confirm = crl::guard(parent, [=] {
			if (!ResolveWindow(context)) {
				return;
			}
			auto passcode = field->getLastText().toUtf8();
			if (passcode.isEmpty()) {
				field->showError();
				return;
			}
			field->clear();
			ReadFolder(context, path, std::move(passcode));
		});
		QObject::connect(field, &Ui::MaskedInputField::submitted, confirm);
		box->addButton(rpl::single(u"Продолжить"_q), confirm);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}), Ui::LayerOption::CloseOther);
}

void ChooseAccount(
		const ImportContext &context,
		Storage::TdataImportResult result) {
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	if (result.accounts.empty()) {
		ShowImportError(context, u"В этой папке нет авторизованных аккаунтов."_q);
		return;
	}
	const auto accounts = std::make_shared<std::vector<Storage::ImportedAccount>>(
		std::move(result.accounts));
	const auto parent = not_null(context.parent.get());
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		BindToParent(parent, box);
		box->setTitle(rpl::single(u"Добавить аккаунт из tdata"_q));
		box->setWidth(st::boxWideWidth);
		box->setMaxHeight(st::introTdataBoxMaxHeight);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Выберите аккаунт. Исходная папка останется без изменений; чаты и медиа загрузятся из Telegram. Исходный Telegram Desktop должен быть закрыт во время переноса."_q,
			st::boxLabel));
		const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
		for (auto i = 0; i != int(accounts->size()); ++i) {
			const auto &account = (*accounts)[i];
			const auto name = account.displayName.isEmpty()
				? u"Аккаунт %1"_q.arg(i + 1)
				: account.displayName;
			const auto label = account.phone.isEmpty()
				? name
				: name + u" — "_q + account.phone;
			box->addRow(object_ptr<Ui::Radiobutton>(
				box, group, i, label, st::defaultBoxCheckbox));
		}
		box->addButton(rpl::single(u"Добавить аккаунт"_q), [=] {
			const auto selected = group->current();
			if (selected < 0 || selected >= int(accounts->size())) {
				return;
			}
			auto imported = std::move((*accounts)[selected]);
			box->closeBox();
			crl::on_main(&Core::App(), [context, imported = std::move(imported)] {
				if (!ResolveWindow(context)) {
					return;
				}
				const auto pending = context.pending.get();
				const auto status = Core::App().domain().importAccount(imported, pending);
				if (status == Main::TdataImportStatus::Failed) {
					ShowImportError(context, u"Аккаунт открыт, но изменения не удалось сохранить. Проверьте свободное место на диске."_q);
				} else if (status == Main::TdataImportStatus::Invalid) {
					ShowImportError(context, u"Не удалось добавить эту сессию. Выберите другую папку tdata."_q);
				}
			});
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}), Ui::LayerOption::CloseOther);
}

void ReadFolder(
		const ImportContext &context,
		QString path,
		QByteArray passcode) {
	const auto window = ResolveWindow(context);
	if (!window) {
		passcode.fill('\0');
		return;
	}
	if (ImportInProgress.test_and_set()) {
		passcode.fill('\0');
		ShowImportError(context, u"Предыдущая проверка tdata ещё выполняется. Подождите и попробуйте снова."_q);
		return;
	}
	const auto parent = not_null(context.parent.get());
	struct State {
		base::weak_qptr<Ui::GenericBox> box;
		bool completed = false;
		bool cancelled = false;
	};
	const auto state = std::make_shared<State>();
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		BindToParent(parent, box);
		state->box = base::make_weak(box);
		box->setTitle(rpl::single(u"Импорт tdata"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, u"Чтение и проверка папки…"_q, st::boxLabel));
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->boxClosing() | rpl::on_next([=] {
			if (!state->completed) {
				state->cancelled = true;
			}
		}, box->lifetime());
	}), Ui::LayerOption::CloseOther);
	auto done = crl::guard(parent, [=](Storage::TdataImportResult result) {
		if (state->cancelled) {
			if (const auto box = state->box.get()) {
				box->closeBox();
			}
			return;
		}
		state->completed = true;
		if (!ResolveWindow(context)) {
			if (const auto box = state->box.get()) {
				box->closeBox();
			}
			return;
		}
		switch (result.error) {
		case Storage::TdataImportError::PasscodeNeeded:
		case Storage::TdataImportError::WrongPasscode:
			AskPasscode(context, path,
				result.error == Storage::TdataImportError::WrongPasscode);
			return;
		case Storage::TdataImportError::NotFound:
			ShowImportError(context, u"Папка tdata не найдена. Выберите саму папку tdata или папку, в которой она находится."_q);
			return;
		case Storage::TdataImportError::Unsupported:
			ShowImportError(context, u"Эта версия или размер tdata пока не поддерживается."_q);
			return;
		case Storage::TdataImportError::Corrupt:
			ShowImportError(context, u"Не удалось прочитать tdata. Папка повреждена, скопирована не полностью или её изменяет запущенный Telegram Desktop."_q);
			return;
		case Storage::TdataImportError::None:
			break;
		}
		if (QDir(result.sourceFolder).canonicalPath()
			== QDir(cWorkingDir() + u"tdata"_q).canonicalPath()) {
			ShowImportError(context, u"Эта папка уже используется kukagram. Выберите другую копию tdata."_q);
			return;
		}
		ChooseAccount(context, std::move(result));
	});
	crl::async([path = std::move(path), passcode = std::move(passcode), done = std::move(done)]() mutable {
		auto result = Storage::ReadTdataAccounts(path, passcode);
		passcode.fill('\0');
		ImportInProgress.clear();
		crl::on_main([result = std::move(result), done = std::move(done)]() mutable {
			done(std::move(result));
		});
	});
}

} // namespace

void ShowTdataImport(
		not_null<Ui::RpWidget*> parent,
		not_null<Main::Account*> pending) {
	const auto window = Core::App().findWindow(parent);
	if (!window || window->id().account != pending.get()) {
		return;
	}
	const auto context = ImportContext{
		.parent = base::make_weak(parent),
		.pending = base::make_weak(pending),
		.window = base::make_weak(window),
	};
	FileDialog::GetFolder(
		parent.get(),
		u"Выберите папку tdata"_q,
		QString(),
		crl::guard(parent, [=](const QString &path) {
			if (!path.isEmpty() && ResolveWindow(context)) {
				ReadFolder(context, path, {});
			}
		}));
}

} // namespace Intro::details
