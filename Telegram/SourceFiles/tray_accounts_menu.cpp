/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tray_accounts_menu.h"

#include "base/weak_ptr.h"
#include "base/qt/qt_key_modifiers.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "info/profile/info_profile_values.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "styles/style_window.h"
#include "styles/style_menu_icons.h"
#include "ui/dynamic_image.h"
#include "ui/dynamic_thumbnails.h"

#ifdef Q_OS_MAC
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#endif

namespace Core::TrayAccountsMenu {

void SetupChangesSubscription(Fn<void()> callback, rpl::lifetime &lifetime) {
	const auto accountSessionsLifetime = lifetime.make_state<rpl::lifetime>();
	const auto watchAccountSessions = [=] {
		accountSessionsLifetime->destroy();
		for (const auto &[index, account] : Core::App().domain().accounts()) {
			account->sessionChanges(
			) | rpl::on_next([=](Main::Session*) {
				callback();
			}, *accountSessionsLifetime);
		}
	};
	Core::App().domain().accountsChanges() | rpl::on_next([=] {
		watchAccountSessions();
		callback();
	}, lifetime);
	watchAccountSessions();
}

void Fill(Platform::Tray &tray) {
	if (Core::App().passcodeLocked()) {
		return;
	}
	const auto accounts = Core::App().domain().storedAccounts();
	const auto visible = ranges::count_if(accounts, [](const auto &stored) {
		return stored.uniqueId || !stored.name.isEmpty();
	});
	if (visible <= 1) {
		return;
	}
	tray.addSeparator();
	constexpr auto kMaxLength = 30;
#ifdef Q_OS_MAC
	auto placeholder = QIcon();
#endif
	for (const auto &stored : accounts) {
		if (!stored.uniqueId && stored.name.isEmpty()) {
			continue;
		}
		const auto &live = Core::App().domain().accounts();
		const auto found = ranges::find(live, stored.index,
			&Main::Domain::AccountWithIndex::index);
		const auto account = (found != end(live))
			? found->account.get()
			: nullptr;
		if (!account || !account->sessionExists()) {
			const auto &name = stored.name.isEmpty() ? stored.phone : stored.name;
			auto title = rpl::single((name.size() > kMaxLength)
				? (name.mid(0, kMaxLength) + Ui::kQEllipsis)
				: name);
			const auto activate = [index = stored.index, id = stored.uniqueId] {
				const auto &app = Core::App();
				if (app.passcodeLocked()) {
					return;
				}
				auto &domain = app.domain();
				const auto accounts = domain.storedAccounts();
				const auto entry = ranges::find(
					accounts, index, &Main::Domain::StoredAccount::index);
				if (entry != end(accounts) && entry->uniqueId == id) {
					domain.activateStored(index, {}, -1, base::IsCtrlPressed());
				}
			};
#ifdef Q_OS_MAC
			if (placeholder.isNull()) {
				const auto icon = Ui::MakeIconThumbnail(st::menuIconProfile);
				placeholder = QIcon(QPixmap::fromImage(
					icon->image(st::notifyMacPhotoSize)));
			}
			tray.addAction(std::move(title), activate, placeholder);
#else
			tray.addAction(std::move(title), activate,
				Ui::MakeIconThumbnail(st::menuIconProfile),
				st::notifyMacPhotoSize);
#endif
			continue;
		}
		const auto user = account->session().user();
		const auto weak = base::make_weak(account);
		tray.addAction(
			Info::Profile::NameValue(
				user
			) | rpl::map([=](const QString &name) {
				return (name.size() > kMaxLength)
					? (name.mid(0, kMaxLength) + Ui::kQEllipsis)
					: name;
			}),
			[weak] {
				const auto strong = weak.get();
				if (Core::App().passcodeLocked()
					|| !strong || !strong->sessionExists()) {
					return;
				}
				if (base::IsCtrlPressed()) {
					Core::App().ensureSeparateWindowFor({ strong });
				} else {
					Core::App().domain().maybeActivate(strong);
				}
			},
			Ui::MakeUserpicThumbnail(user, true),
			st::notifyMacPhotoSize);
	}
}

} // namespace Core::TrayAccountsMenu
