/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_domain.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/shortcuts.h"
#include "core/crash_reports.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_download_manager.h"
#include "data/data_document.h"
#include "data/data_user.h"
#include "calls/calls_instance.h"
#include "calls/calls_call.h"
#include "calls/group/calls_group_call.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "export/export_manager.h"
#include "inline_bots/bot_attach_web_view.h"
#include "iv/iv_instance.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_dc_options.h"
#include "storage/storage_domain.h"
#include "storage/storage_account.h"
#include "storage/localstorage.h"
#include "export/export_settings.h"
#include "wallet/wallet_unlock.h"
#include "wallet/wallet_vault.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "mainwindow.h"
#include "webview/webview_dialog.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include "settings.h"
#include <bitset>
#include <unordered_map>

namespace Main {
namespace {

constexpr auto kRetirementRetryDelay = crl::time(1000);
constexpr auto kBlockedRetirementRetryDelay = crl::time(10000);

[[nodiscard]] QString NotificationPolicyFile() {
#if defined Q_OS_MAC || defined Q_OS_WIN
	return cWorkingDir() + u"notification-accounts.ini"_q;
#else
	return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
		+ u"/kukagram-notification-accounts.ini"_q;
#endif
}

} // namespace

Domain::Domain(const QString &dataName)
: _dataName(dataName)
, _local(std::make_unique<Storage::Domain>(this, dataName))
, _retirementTimer([=] { retireInactiveAccounts(kBlockedRetirementRetryDelay); }) {
	// Notifications are not part of Telegram account credentials.
	const auto policy = QSettings(NotificationPolicyFile(), QSettings::IniFormat);
	_notificationAllowlistConfigured = policy.value(
		u"configured"_q, false).toBool();
	for (const auto &text : policy.value(u"allowed"_q).toStringList()) {
		bool valid = false;
		const auto id = text.toULongLong(&valid);
		if (valid && id) {
			_notificationAllowed.insert(id);
		}
	}
	_active.changes(
	) | rpl::take(1) | rpl::on_next([=] {
		// In case we had a legacy passcoded app we start settings here.
		Core::App().startSettingsAndBackground();

		crl::on_main(this, [=] {
			Core::App().notifications().createManager();
		});
	}, _lifetime);

	_active.changes(
	) | rpl::map([](Main::Account *account) {
		return account ? account->sessionValue() : rpl::never<Session*>();
		}) | rpl::flatten_latest(
	) | rpl::map([](Main::Session *session) {
		return session
			? session->changes().peerFlagsValue(
				session->user(),
				Data::PeerUpdate::Flag::Username)
			: rpl::never<Data::PeerUpdate>();
	}) | rpl::flatten_latest(
	) | rpl::on_next([](const Data::PeerUpdate &update) {
		CrashReports::SetAnnotation("Username", update.peer->username());
	}, _lifetime);
}

Domain::~Domain() {
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
}

Wallet::VaultRuntime &Domain::walletKeyring() {
	if (!_walletKeyring) {
		_walletKeyring = std::make_shared<Wallet::VaultRuntime>(*this);
	}
	return *_walletKeyring;
}

bool Domain::started() const {
	return !_accounts.empty();
}

Storage::StartResult Domain::start(const QByteArray &passcode) {
	return startWith(_local->prepareOpen(passcode));
}

Storage::StartResult Domain::startWith(
		Storage::PasscodeDerivation derived) {
	Expects(!started());

	const auto result = _local->start(std::move(derived));
	if (result == Storage::StartResult::Success) {
		activateAfterStarting();
		Wallet::DropUnusedPasscode();
		crl::on_main(&Core::App(), [=] { suggestExportIfNeeded(); });
	} else {
		Assert(!started());
		if (result == Storage::StartResult::ProfileReadError) {
			crl::on_main(this, [] {
				if (Core::Quitting()) {
					return;
				}
				const auto window = Core::App().activePrimaryWindow();
				const auto box = new QMessageBox(
					QMessageBox::Critical,
					u"kukagram"_q,
					u"Не удалось прочитать локальный профиль. Проверьте доступ к папке "
					u"профиля или восстановите её из резервной копии, затем откройте kukagram снова."_q,
					QMessageBox::Close,
					window ? window->widget().get() : nullptr);
				box->setAttribute(Qt::WA_DeleteOnClose);
				box->setWindowModality(Qt::ApplicationModal);
				QObject::connect(box, &QMessageBox::finished, box, [] { Core::Quit(); });
				box->show();
			});
		}
	}
	return result;
}

bool Domain::tryPasscode(
		const QByteArray &passcode,
		Fn<void(bool correct)> done) {
	if (_passcodeDeriving) {
		return false;
	}
	_passcodeDeriving = true;
	const auto cold = !started();
	Storage::DeriveOnWorker(
		_local->prepareOpen(passcode),
		crl::guard(this, [=](Storage::PasscodeDerivation &&derived) {
			_passcodeDeriving = false;
			if (Core::Quitting()) {
				return;
			}
			if (cold == started()) {
				done(false);
			} else if (!cold) {
				done(_local->checkPasscode(std::move(derived)));
			} else {
				const auto result = startWith(std::move(derived));
				if (result != Storage::StartResult::ProfileReadError) {
					done(result == Storage::StartResult::Success);
				}
			}
		}));
	return true;
}

void Domain::finish() {
	_retirementTimer.cancel();
	// Capture the live selection before tearing down the accounts. Local::finish()
	// drains the storage writer, but not callbacks still queued on the UI thread.
	if (base::take(_writeAccountsScheduled) && started()) {
		_local->writeAccounts();
	}
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
	_accountToActivate = -1;
	_active.reset(nullptr);
	base::take(_accounts);
}

void Domain::suggestExportIfNeeded() {
	Expects(started());

	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			const auto settings = session->local().readExportSettings();
			if (const auto availableAt = settings.availableAt) {
				session->data().suggestStartExport(availableAt);
			}
		}
	}
}

void Domain::accountAddedInStorage(AccountWithIndex accountWithIndex) {
	Expects(accountWithIndex.account != nullptr);

	for (const auto &[index, _] : _accounts) {
		if (index == accountWithIndex.index) {
			Unexpected("Repeated account index.");
		}
	}
	if (!accountWithIndex.account->sessionExists()) {
		_pendingAccounts.push_back(accountWithIndex.index);
	}
	_accounts.push_back(std::move(accountWithIndex));
}

void Domain::activateFromStorage(int index) {
	_accountToActivate = index;
}

int Domain::activeForStorage() const {
	return _accountToActivate;
}

void Domain::resetWithForgottenPasscode() {
	if (_walletKeyring) {
		_walletKeyring->clear();
	}
	if (_accounts.empty()) {
		_local->startFromScratch();
		activateAfterStarting();
	} else {
		_resettingStored = true;
		_storedAccounts.clear();
		for (const auto &[index, account] : _accounts) {
			account->logOutThen(crl::guard(this, [=] {
				crl::on_main(this, [=] { removeRedundantAccounts(); });
			}));
		}
	}
}

bool Domain::finishPasscodeClearAfterReset() {
	if (!passcodeRemovalAuthorized()) {
		return false;
	}
	return clearPasscodeAfterLastLogout();
}

void Domain::activateAfterStarting() {
	Expects(started());

	auto toActivate = _accounts.front().account.get();
	for (const auto &[index, account] : _accounts) {
		if (index == _accountToActivate) {
			toActivate = account.get();
		}
		watchSession(account.get());
	}

	activate(toActivate);
	removePasscodeIfEmpty();
}

const std::vector<Domain::AccountWithIndex> &Domain::accounts() const {
	return _accounts;
}

std::vector<not_null<Account*>> Domain::orderedAccounts() const {
	const auto order = Core::App().settings().accountsOrder();
	auto accounts = ranges::views::all(
		_accounts
	) | ranges::views::transform([](const Domain::AccountWithIndex &a) {
		return not_null{ a.account.get() };
	}) | ranges::to_vector;
	auto positions = std::unordered_map<uint64, std::size_t>();
	positions.reserve(order.size());
	for (auto i = std::size_t(0); i != order.size(); ++i) {
		positions.try_emplace(order[i], i);
	}
	const auto position = [&](not_null<Account*> account) {
		if (account->sessionExists()) {
			const auto i = positions.find(account->session().uniqueId());
			if (i != end(positions)) {
				return i->second;
			}
		}
		return order.size();
	};
	ranges::stable_sort(accounts, std::less<>(), position);
	return accounts;
}

std::vector<Domain::StoredAccount> Domain::storedAccounts() const {
	auto result = _storedAccounts;
	for (const auto &[index, account] : _accounts) {
		auto current = StoredAccount{
			.index = index,
			.authorizationKnown = true,
		};
		if (const auto session = account->maybeSession()) {
			current.uniqueId = session->uniqueId();
			current.name = session->user()->name();
			current.phone = session->user()->phone();
			current.username = session->user()->username();
		} else if (!ranges::contains(_pendingAccounts, index)) {
			continue;
		}
		const auto existing = ranges::find(result, index, &StoredAccount::index);
		if (existing == end(result)) {
			result.push_back(std::move(current));
		} else {
			*existing = std::move(current);
		}
	}
	if (result.empty() && !_accounts.empty()) {
		result.push_back({ .index = _accounts.front().index });
	}
	const auto order = Core::App().settings().accountsOrder();
	auto positions = std::unordered_map<uint64, std::size_t>();
	positions.reserve(order.size());
	for (auto i = std::size_t(0); i != order.size(); ++i) {
		positions.try_emplace(order[i], i);
	}
	ranges::stable_sort(result, std::less<>(), [&](const StoredAccount &account) {
		const auto i = positions.find(account.uniqueId);
		return (i == end(positions)) ? order.size() : i->second;
	});
	return result;
}

int Domain::storedAccountsCount() const {
	auto indexes = std::bitset<kLocalMaxAccounts>();
	for (const auto &stored : _storedAccounts) {
		indexes.set(stored.index);
	}
	for (const auto &[index, account] : _accounts) {
		if (account->sessionExists()
			|| ranges::contains(_pendingAccounts, index)) {
			indexes.set(index);
		}
	}
	return std::max(int(indexes.count()), _accounts.empty() ? 0 : 1);
}

bool Domain::cycleStoredAccount(int direction) {
	if (!started() || !direction || _switchingStored || _resettingStored
		|| _retiringInactiveAccounts || _clearingCaches || Core::Quitting()
		|| Core::App().passcodeLocked()) {
		return false;
	}
	const auto all = storedAccounts();
	auto available = std::vector<int>();
	available.reserve(all.size());
	for (const auto &stored : all) {
		if (stored.uniqueId) {
			available.push_back(stored.index);
		}
	}
	if (available.size() < 2) {
		return false;
	}
	const auto current = ranges::find(available, _accountToActivate);
	const auto index = (current == end(available))
		? (direction > 0 ? 0 : int(available.size()) - 1)
		: (int(current - begin(available))
			+ (direction > 0 ? 1 : -1)
			+ int(available.size())) % int(available.size());
	activateStored(available[index]);
	return true;
}

bool Domain::canClearAccountCaches() const {
	return started()
		&& !_switchingStored
		&& !_resettingStored
		&& !_retiringInactiveAccounts
		&& !_clearingCaches
		&& !Core::Quitting();
}

std::vector<QString> Domain::accountCacheFolders(bool unloadedOnly) const {
	auto result = std::vector<QString>();
	for (const auto &stored : storedAccounts()) {
		const auto loaded = ranges::contains(
			_accounts,
			stored.index,
			&AccountWithIndex::index);
		if (unloadedOnly && loaded) {
			continue;
		}
		const auto base = Storage::Account::DatabasePathFor(
			ComposeDataString(_dataName, stored.index));
		result.push_back(base + u"cache"_q);
		result.push_back(base + u"media_cache"_q);
	}
	return result;
}

void Domain::clearLoadedAccountCaches() {
	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			session->data().cache().clear();
			session->data().cacheBigFile().clear();
		}
	}
}

void Domain::setClearingAccountCaches(bool clearing) {
	_clearingCaches = clearing;
}

bool Domain::notificationsAllowed(uint64 sessionId) const {
	return !_notificationAllowlistConfigured
		|| _notificationAllowed.contains(sessionId);
}

bool Domain::saveNotificationAllowlist(
		const std::vector<uint64> &allowed) {
	const auto path = NotificationPolicyFile();
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto ids = QStringList();
	auto unique = std::unordered_set<uint64>();
	for (const auto id : allowed) {
		if (id && unique.insert(id).second) {
			ids.push_back(QString::number(id));
		}
	}
	{
		QSettings policy(path, QSettings::IniFormat);
		policy.setValue(u"allowed"_q, ids);
		policy.setValue(u"configured"_q, true);
		policy.sync();
		if (policy.status() != QSettings::NoError) {
			return false;
		}
	}
	_notificationAllowed = std::move(unique);
	_notificationAllowlistConfigured = true;
	// Remove already queued system alerts and update Dock counters.
	Core::App().notifications().clearAll();
	scheduleUpdateUnreadBadge();
	if (_accounts.size() > 1) {
		_retirementTimer.callOnce(kRetirementRetryDelay);
	}
	return true;
}

void Domain::storedAccountAdded(StoredAccount account) {
	const auto i = ranges::find(_storedAccounts, account.index, &StoredAccount::index);
	if (i == end(_storedAccounts)) {
		_storedAccounts.push_back(std::move(account));
	} else {
		*i = std::move(account);
	}
}

void Domain::finishLogin(not_null<Account*> account) {
	scheduleWriteAccounts();
	crl::on_main(account, [=] {
		if (Core::Quitting() || !account->sessionExists()) {
			return;
		}
		_accountsChanges.fire({});
		retireInactiveAccounts(kRetirementRetryDelay);
	});
}

TdataImportStatus Domain::importAccount(
		const Storage::ImportedAccount &data,
		not_null<Account*> pending) {
	if (Core::Quitting() || _switchingStored || _retiringInactiveAccounts
		|| data.authorization.isEmpty() || !data.uniqueId
		|| pending->sessionExists() || pending->loggingOut()) {
		return TdataImportStatus::Invalid;
	}
	const auto entry = ranges::find(_accounts, pending.get(), [](const AccountWithIndex &value) {
		return value.account.get();
	});
	if (entry == end(_accounts) || !ranges::contains(_pendingAccounts, entry->index)) {
		return TdataImportStatus::Invalid;
	}
	const auto index = entry->index;
	const auto introWindow = Core::App().separateWindowFor(pending);
	const auto weakIntroWindow = introWindow
		? base::make_weak(introWindow)
		: base::weak_ptr<Window::Controller>();
	for (const auto &stored : storedAccounts()) {
		if (stored.index != index && stored.uniqueId == data.uniqueId) {
			activateStored(stored.index, {}, index);
			return TdataImportStatus::Existing;
		}
	}
	const auto environment = data.testMode
		? MTP::Environment::Test
		: MTP::Environment::Production;
	auto config = data.testMode
		? std::make_unique<MTP::Config>(environment)
		: std::make_unique<MTP::Config>(Core::App().fallbackProductionConfig());
	auto imported = std::make_unique<Account>(this, _dataName, index);
	imported->setMtpAuthorization(data.authorization);
	if (imported->willHaveSessionUniqueId(config.get()) != data.uniqueId) {
		return TdataImportStatus::Invalid;
	}
	imported->setSessionFromStorage(
		std::make_unique<SessionSettings>(),
		QByteArray(data.self),
		data.selfStreamVersion);
	_switchingStored = true;
	const auto switching = gsl::finally([&] { _switchingStored = false; });
	auto previous = std::move(entry->account);
	previous->prepareForRetirement();
	Local::sync();
	_local->startAdded(imported.get(), std::move(config));
	const auto added = imported.get();
	entry->account = std::move(imported);
	watchSession(added);
	const auto persisted = added->local().writeImportedSession();
	const auto registered = _local->writeAccountsChecked();
	const auto written = persisted && registered;
	if (const auto window = weakIntroWindow.get()) {
		Core::App().setActivePrimaryWindow(window);
	}
	activate(added);
	_accountsChanges.fire({});
	previous.reset();
	finishLogin(added);
	return written ? TdataImportStatus::Added : TdataImportStatus::Failed;
}

bool Domain::canRetireAccount(not_null<Account*> account) const {
	const auto session = account->maybeSession();
	if (!session || account == _active.current()
		|| account->loggingOut() || account->destroyingSession()
		|| QApplication::activePopupWidget() || QApplication::activeModalWidget()
		|| QApplication::mouseButtons() != Qt::NoButton
		|| Webview::InsideBlockingPopup()
		|| session->uploadsInProgress()
		|| Core::App().downloadManager().loadingInProgress(session)
		|| Core::App().exportManager().inProgress(session)
		|| session->attachWebView().hasOpenInstances()
		|| Core::App().iv().hasActiveWindow(session)) {
		return false;
	}
	if (const auto call = Core::App().calls().currentCall();
		call && &call->user()->session() == session) {
		return false;
	}
	if (const auto call = Core::App().calls().currentGroupCall();
		call && &call->peer()->session() == session) {
		return false;
	}
	for (const auto type : { AudioMsgId::Type::Song, AudioMsgId::Type::Voice }) {
		const auto state = Media::Player::instance()->getState(type);
		const auto document = state.id.audio();
		if (document && &document->session() == session
			&& !Media::Player::IsStoppedOrStopping(state.state)) {
			return false;
		}
	}
	auto ownedWindow = false;
	Core::App().enumerateWindows([&](not_null<Window::Controller*> window) {
		ownedWindow |= window->id().account == account;
	});
	if (ownedWindow) {
		return false;
	}
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (!widget->isVisible() || widget->windowType() == Qt::ToolTip) {
			continue;
		}
		auto primary = false;
		Core::App().enumerateWindows([&](not_null<Window::Controller*> window) {
			primary |= window->widget() == widget;
		});
		if (!primary) {
			return false;
		}
	}
	return true;
}

void Domain::retireInactiveAccounts(crl::time retryDelay) {
	_retirementTimer.cancel();
	if (!started() || Core::Quitting() || _resettingStored
		|| _switchingStored || _retiringInactiveAccounts || _clearingCaches
		|| !_active.current() || !_active.current()->sessionExists()) {
		return;
	}
	auto candidates = std::vector<int>();
	auto retry = false;
	for (const auto &[index, account] : _accounts) {
		if (account.get() == _active.current() || !account->sessionExists()
			|| ranges::contains(_pendingAccounts, index)) {
			continue;
		}
		// A small, explicit notification allowlist may keep those sessions hot.
		// Do not do this with the default all-allowed policy (up to 1000).
		if (_notificationAllowlistConfigured
			&& notificationsAllowed(account->session().uniqueId())) {
			continue;
		}
		if (!canRetireAccount(account.get())) {
			retry = true;
			continue;
		}
		// The master file does not verify this account's authorization or map.
		if (!account->checkpointForRetirement()) {
			retry = true;
			continue;
		}
		const auto session = account->maybeSession();
		storedAccountAdded({
			.index = index,
			.uniqueId = session->uniqueId(),
			.name = session->user()->name(),
			.phone = session->user()->phone(),
			.username = session->user()->username(),
			.authorizationKnown = true,
		});
		candidates.push_back(index);
	}
	if (!candidates.empty() && _local->writeAccountsChecked()) {
		_retiringInactiveAccounts = true;
		auto retired = std::vector<AccountWithIndex>();
		for (auto i = _accounts.begin(); i != _accounts.end();) {
			if (!ranges::contains(candidates, i->index)) {
				++i;
				continue;
			}
			retired.push_back(std::move(*i));
			i = _accounts.erase(i);
		}
		_accountsChanges.fire({});
		for (const auto &entry : retired) {
			entry.account->prepareForRetirement();
		}
		retired.clear();
		_retiringInactiveAccounts = false;
		Local::sync();
		scheduleUpdateUnreadBadge();
	} else if (!candidates.empty()) {
		retry = true;
	}
	if (retry) {
		_retirementTimer.callOnce(retryDelay);
	}
}

void Domain::activateStored(
		int index,
		QString startUrl,
		int discardPendingIndex,
		bool newWindow) {
	if (_switchingStored || _resettingStored || _clearingCaches
		|| _retiringInactiveAccounts || Core::Quitting()
		|| Core::App().passcodeLocked()) {
		return;
	}
	const auto stored = storedAccounts();
	const auto target = ranges::find(stored, index, &StoredAccount::index);
	if (target == end(stored)) {
		return;
	} else if (!target->uniqueId && !target->authorizationKnown) {
		if (const auto window = Core::App().activePrimaryWindow()) {
			window->showToast(tr::lng_kukagram_account_read_failed(tr::now));
		}
		return;
	}
	const auto targetId = target->uniqueId;
	auto invoke = crl::guard(this, [=] {
		if (_switchingStored || _resettingStored || _clearingCaches
			|| _retiringInactiveAccounts || Core::Quitting()
			|| Core::App().passcodeLocked()) {
			return;
		}
		const auto list = storedAccounts();
		const auto target = ranges::find(list, index, &StoredAccount::index);
		if (target == end(list) || target->uniqueId != targetId) {
			return;
		}
		_switchingStored = true;
		const auto switching = gsl::finally([&] {
			_switchingStored = false;
			if (_accounts.size() > 1 && !Core::Quitting()) {
				_retirementTimer.callOnce(kRetirementRetryDelay);
			}
		});
		const auto was = _accountToActivate;
		const auto pending = _pendingAccounts;
		const auto summaries = _storedAccounts;
		if (discardPendingIndex >= 0 && discardPendingIndex != index) {
			const auto discarded = ranges::find(_accounts, discardPendingIndex, &AccountWithIndex::index);
			if (discarded == end(_accounts) || discarded->account->sessionExists()
				|| !ranges::contains(_pendingAccounts, discardPendingIndex)) {
				return;
			}
			_pendingAccounts.erase(std::remove(begin(_pendingAccounts), end(_pendingAccounts), discardPendingIndex), end(_pendingAccounts));
			_storedAccounts.erase(std::remove_if(begin(_storedAccounts), end(_storedAccounts), [=](const StoredAccount &stored) {
				return stored.index == discardPendingIndex;
			}), end(_storedAccounts));
		}
		_accountToActivate = index;
		const auto written = _local->writeAccountsChecked();
		_accountToActivate = was;
		if (!written) {
			_pendingAccounts = pending;
			_storedAccounts = summaries;
			if (const auto window = Core::App().activePrimaryWindow()) {
				window->showToast(tr::lng_kukagram_account_switch_failed(tr::now));
			}
			return;
		}
		const auto hot = ranges::find(_accounts, index, &AccountWithIndex::index);
		if (hot == end(_accounts)) {
			auto account = std::make_unique<Account>(this, _dataName, index);
			_local->startStored(account.get());
			if (!account->sessionExists()
				|| account->session().uniqueId() != target->uniqueId) {
				if (account->sessionExists()) {
					account->prepareForRetirement();
				}
				_pendingAccounts = pending;
				_storedAccounts = summaries;
				if (!_local->writeAccountsChecked()) {
					LOG(("App Error: could not restore account selection after failed activation."));
				}
				if (const auto window = Core::App().activePrimaryWindow()) {
					window->showToast(tr::lng_kukagram_account_read_failed(tr::now));
				}
				return;
			}
			const auto added = account.get();
			accountAddedInStorage({ .index = index, .account = std::move(account) });
			watchSession(added);
		}
		const auto selected = not_null(ranges::find(
			_accounts, index, &AccountWithIndex::index)->account.get());
		if (newWindow) {
			Core::App().ensureSeparateWindowFor(selected);
		}
		activate(selected);
		_accountsChanges.fire({});
		crl::on_main(this, [=] {
			removeRedundantAccounts();
			retireInactiveAccounts(kRetirementRetryDelay);
			if (!startUrl.isEmpty() && _accountToActivate == index) {
				cRefStartUrls().push_back(QUrl(startUrl));
				Core::App().checkStartUrls();
			}
		});
	});
	if (newWindow) {
		invoke();
	} else {
		Core::App().preventOrInvoke(std::move(invoke));
	}
}

rpl::producer<> Domain::accountsChanges() const {
	return _accountsChanges.events();
}

Account *Domain::maybeLastOrSomeAuthedAccount() {
	auto result = (Account*)nullptr;
	for (const auto &[index, account] : _accounts) {
		if (!account->sessionExists()) {
			continue;
		} else if (index == _lastActiveIndex) {
			return account.get();
		} else if (!result) {
			result = account.get();
		}
	}
	return result;
}

int Domain::accountsAuthedCount() const {
	return int(ranges::count_if(storedAccounts(), [](const StoredAccount &account) {
		return account.uniqueId != 0;
	}));
}

rpl::producer<Account*> Domain::activeValue() const {
	return _active.value();
}

Account &Domain::active() const {
	Expects(!_accounts.empty());

	Ensures(_active.current() != nullptr);
	return *_active.current();
}

rpl::producer<not_null<Account*>> Domain::activeChanges() const {
	return _active.changes() | rpl::map([](Account *value) {
		return not_null{ value };
	});
}

rpl::producer<Session*> Domain::activeSessionChanges() const {
	return _activeSessions.events();
}

rpl::producer<Session*> Domain::activeSessionValue() const {
	const auto current = _accounts.empty()
		? nullptr
		: active().maybeSession();
	return rpl::single(current) | rpl::then(_activeSessions.events());
}

int Domain::unreadBadge() const {
	return _unreadBadge;
}

bool Domain::unreadBadgeMuted() const {
	return _unreadBadgeMuted;
}

rpl::producer<> Domain::unreadBadgeChanges() const {
	return _unreadBadgeChanges.events();
}

void Domain::notifyUnreadBadgeChanged() {
	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			session->data().notifyUnreadBadgeChanged();
		}
	}
}

void Domain::updateUnreadBadge() {
	_unreadBadge = 0;
	_unreadBadgeMuted = true;
	for (const auto &[index, account] : _accounts) {
		if (const auto session = account->maybeSession()) {
			if (!notificationsAllowed(session->uniqueId())) {
				continue;
			}
			const auto data = &session->data();
			_unreadBadge += data->unreadBadge();
			if (!data->unreadBadgeMuted()) {
				_unreadBadgeMuted = false;
			}
		}
	}
	_unreadBadgeChanges.fire({});
}

void Domain::scheduleUpdateUnreadBadge() {
	if (_unreadBadgeUpdateScheduled || Core::Quitting()) {
		return;
	}
	_unreadBadgeUpdateScheduled = true;
	Core::App().postponeCall(crl::guard(&Core::App(), [=] {
		_unreadBadgeUpdateScheduled = false;
		updateUnreadBadge();
	}));
}

int Domain::firstFreeAccountIndex() const {
	auto occupied = std::bitset<kLocalMaxAccounts>();
	for (const auto &entry : _storedAccounts) {
		occupied.set(entry.index);
	}
	// Sessionless accounts can still own their local storage while a
	// window or logout callback keeps them live.
	for (const auto &entry : _accounts) {
		occupied.set(entry.index);
	}
	for (auto index = 0; index != kLocalMaxAccounts; ++index) {
		if (!occupied.test(index)) {
			return index;
		}
	}
	return -1;
}

not_null<Main::Account*> Domain::add(
		MTP::Environment environment,
		int index) {
	Expects(started());

	static const auto cloneConfig = [](const MTP::Config &config) {
		return std::make_unique<MTP::Config>(config);
	};
	auto mainDcId = MTP::Instance::Fields::kNotSetMainDc;
	const auto accountConfig = [&](not_null<Account*> account) {
		mainDcId = account->mtp().mainDcId();
		return cloneConfig(account->mtp().config());
	};
	auto config = [&] {
		if (_active.current()->mtp().environment() == environment) {
			return accountConfig(_active.current());
		}
		for (const auto &[index, account] : _accounts) {
			if (account->mtp().environment() == environment) {
				return accountConfig(account.get());
			}
		}
		return (environment == MTP::Environment::Production)
			? cloneConfig(Core::App().fallbackProductionConfig())
			: std::make_unique<MTP::Config>(environment);
	}();
	_accounts.push_back(AccountWithIndex{
		.index = index,
		.account = std::make_unique<Account>(this, _dataName, index)
	});
	const auto account = _accounts.back().account.get();
	account->setMtpMainDcId(mainDcId);
	_local->startAdded(account, std::move(config));
	_pendingAccounts.push_back(index);
	watchSession(account);
	_accountsChanges.fire({});

	auto &settings = Core::App().settings();
	if (storedAccountsCount() >= 2 && !settings.mainMenuAccountsShown()) {
		settings.setMainMenuAccountsShown(true);
		Core::App().saveSettingsDelayed();
	}

	return account;
}

void Domain::addActivated(MTP::Environment environment, bool newWindow) {
	if (_retiringInactiveAccounts || _switchingStored
		|| _resettingStored || Core::Quitting()) {
		return;
	}
	const auto added = [&](not_null<Main::Account*> account) {
		if (newWindow) {
			Core::App().ensureSeparateWindowFor(account);
		} else if (const auto window = Core::App().separateWindowFor(
				account)) {
			window->activate();
		} else {
			activate(account);
		}
	};
	for (auto &[index, account] : accounts()) {
		if (!account->sessionExists()
			&& !account->loggingOut()
			&& !account->destroyingSession()
			&& account->mtp().environment() == environment) {
			const auto restored = !ranges::contains(_pendingAccounts, index);
			if (restored) {
				_pendingAccounts.push_back(index);
				scheduleWriteAccounts();
			}
			added(account.get());
			if (restored) {
				_accountsChanges.fire({});
			}
			return;
		}
	}
	if (const auto index = firstFreeAccountIndex(); index >= 0) {
		added(add(environment, index));
	}
}

void Domain::watchSession(not_null<Account*> account) {
	account->sessionValue(
	) | rpl::filter([=](Session *session) {
		return session != nullptr;
	}) | rpl::on_next([=](Session *session) {
		const auto entry = ranges::find(_accounts, account.get(), [](const AccountWithIndex &value) {
			return value.account.get();
		});
		Assert(entry != end(_accounts));
		const auto index = entry->index;
		_pendingAccounts.erase(std::remove(begin(_pendingAccounts), end(_pendingAccounts), index), end(_pendingAccounts));
		storedAccountAdded({
			.index = index,
			.uniqueId = session->uniqueId(),
			.name = session->user()->name(),
			.phone = session->user()->phone(),
			.username = session->user()->username(),
			.authorizationKnown = true,
		});

		session->data().unreadBadgeChanges(
		) | rpl::on_next([=] {
			scheduleUpdateUnreadBadge();
		}, session->lifetime());
	}, account->lifetime());

	account->sessionChanges(
	) | rpl::filter([=](Session *session) {
		return !session;
	}) | rpl::on_next([=] {
		const auto entry = ranges::find(_accounts, account.get(), [](const AccountWithIndex &value) {
			return value.account.get();
		});
		if (entry != end(_accounts)) {
			const auto index = entry->index;
			_storedAccounts.erase(std::remove_if(begin(_storedAccounts), end(_storedAccounts), [=](const StoredAccount &stored) {
				return stored.index == index;
			}), end(_storedAccounts));
			_pendingAccounts.erase(std::remove(begin(_pendingAccounts), end(_pendingAccounts), index), end(_pendingAccounts));
			if (!_storedAccounts.empty() && _accounts.size() == 1) {
				const auto next = ranges::find_if(_storedAccounts, [](const auto &stored) {
					return stored.uniqueId != 0;
				});
				if (next != end(_storedAccounts)) {
					crl::on_main(this, [=, index = next->index] { activateStored(index); });
				}
			}
		}

		scheduleUpdateUnreadBadge();
		closeAccountWindows(account);
		if (!Core::Quitting()) {
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
		}
	}, account->lifetime());
}

void Domain::closeAccountWindows(not_null<Main::Account*> account) {
	auto another = (Main::Account*)nullptr;
	for (auto i = _accounts.begin(); i != _accounts.end(); ++i) {
		const auto other = not_null(i->account.get());
		if (other == account) {
			continue;
		} else if (Core::App().separateWindowFor(other)) {
			const auto that = Core::App().separateWindowFor(account);
			if (that) {
				that->close();
			}
		} else if (!another
			|| (other->sessionExists() && !another->sessionExists())) {
			another = other;
		}
	}
	if (another) {
		activate(another);
	}
}

bool Domain::removePasscodeIfEmpty() {
	if (storedAccountsCount() != 1 || !_storedAccounts.empty()
		|| _active.current()->sessionExists()) {
		return false;
	}
	Local::reset();

	// We completely logged out, remove the passcode if it was there.
	if (Core::App().passcodeLocked()) {
		Core::App().unlockPasscode();
	}
	if (!_local->hasPasscode()) {
		return false;
	} else if (!clearPasscodeAfterLastLogout()) {
		reportFailedPasscodeClear();
		return false;
	}
	return true;
}

bool Domain::passcodeRemovalAuthorized() const {
	return (_accounts.size() == 1)
		&& _storedAccounts.empty()
		&& !_active.current()->sessionExists()
		&& _local->hasPasscode();
}

bool Domain::clearPasscodeAfterLastLogout() {
	Expects(passcodeRemovalAuthorized());

	_local->clearPasscodeAfterReset();
	if (_local->hasPasscode()) {
		return false;
	}
	Core::App().settings().setSystemUnlockEnabled(false);
	Core::App().saveSettingsDelayed();
	return true;
}

void Domain::reportFailedPasscodeClear() {
	crl::on_main(this, [=] {
		if (!passcodeRemovalAuthorized()) {
			return;
		}
		if (const auto window = Core::App().activePrimaryWindow()) {
			window->showPasscodeClearFailed(crl::guard(this, [=] {
				return finishPasscodeClearAfterReset();
			}));
		}
	});
}

void Domain::removeRedundantAccounts() {
	if (Core::Quitting() || !started() || _retiringInactiveAccounts) {
		return;
	}

	for (const auto &one : _accounts) {
		if (one.account->destroyingSession()) {
			// We were invoked from a crl::on_main drain that a nested
			// event dispatch started from inside that account's own
			// destroySession(). Erasing it now would free the object
			// under its own stack frame. Retry after unwinding.
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
			return;
		}
	}

	if (_resettingStored) {
		if (ranges::any_of(_accounts, [](const AccountWithIndex &entry) {
				return entry.account->sessionExists() || entry.account->loggingOut();
			})) {
			return;
		}
		_resettingStored = false;
		_activeLifetime.destroy();
		_active.reset(nullptr);
		base::take(_accounts);
		_pendingAccounts.clear();
		_storedAccounts.clear();
		_local->startFromScratch();
		activateAfterStarting();
		_accountsChanges.fire({});
		return;
	}
	const auto was = _accounts.size();
	for (auto i = _accounts.begin(); i != _accounts.end();) {
		if (Core::App().separateWindowFor(not_null(i->account.get()))
			|| i->account->sessionExists()
			|| (_accounts.size() == 1 && !_storedAccounts.empty())) {
			++i;
			continue;
		}
		checkForLastProductionConfig(i->account.get());
		const auto index = i->index;
		_pendingAccounts.erase(std::remove(begin(_pendingAccounts), end(_pendingAccounts), index), end(_pendingAccounts));
		i = _accounts.erase(i);
	}

	if (!removePasscodeIfEmpty() && _accounts.size() != was) {
		scheduleWriteAccounts();
		_accountsChanges.fire({});
	}
	// Session teardown already removed the logged-out account's live keyring
	// membership, including when its window keeps the account in the list.
	// The last-logout case retains its checked clear and retry route, and
	// passcodeRemovalAuthorized() stays true until that clear has landed.
	if (!passcodeRemovalAuthorized()) {
		Wallet::DropUnusedPasscode();
	}
}

void Domain::checkForLastProductionConfig(
		not_null<Main::Account*> account) {
	const auto mtp = &account->mtp();
	if (mtp->environment() != MTP::Environment::Production) {
		return;
	}
	for (const auto &[index, other] : _accounts) {
		if (other.get() != account
			&& other->mtp().environment() == MTP::Environment::Production) {
			return;
		}
	}
	Core::App().refreshFallbackProductionConfig(mtp->config());
}

void Domain::maybeActivate(not_null<Main::Account*> account) {
	if (Core::App().separateWindowFor(account)) {
		activate(account);
	} else {
		Core::App().preventOrInvoke(crl::guard(account, [=] {
			activate(account);
		}));
	}
}

void Domain::activate(not_null<Main::Account*> account) {
	if (const auto window = Core::App().separateWindowFor(account)) {
		window->activate();
	}
	if (_active.current() == account.get()) {
		return;
	}
	const auto i = ranges::find(_accounts, account.get(), [](
			const AccountWithIndex &value) {
		return value.account.get();
	});
	Assert(i != end(_accounts));
	const auto changed = (_accountToActivate != i->index);
	auto wasAuthed = false;

	_activeLifetime.destroy();
	if (_active.current()) {
		_lastActiveIndex = _accountToActivate;
		wasAuthed = _active.current()->sessionExists();
	}
	_accountToActivate = i->index;
	_active = account.get();
	_active.current()->sessionValue(
	) | rpl::start_to_stream(_activeSessions, _activeLifetime);

	if (_accounts.size() > 1 && !_switchingStored && !_resettingStored
		&& !Core::Quitting()) {
		_retirementTimer.callOnce(kRetirementRetryDelay);
	}

	if (changed) {
		if (wasAuthed) {
			scheduleWriteAccounts();
		} else {
			crl::on_main(&Core::App(), [=] {
				removeRedundantAccounts();
			});
		}
	}
}

void Domain::scheduleWriteAccounts() {
	if (_writeAccountsScheduled) {
		return;
	}
	_writeAccountsScheduled = true;
	crl::on_main(this, [=] {
		if (base::take(_writeAccountsScheduled) && started()) {
			_local->writeAccounts();
		}
	});
}

} // namespace Main
