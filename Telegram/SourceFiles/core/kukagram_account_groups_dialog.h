/*
Local account groups for kukagram.
*/
#pragma once

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/kukagram_account_groups.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/text/format_values.h"
#include "window/window_controller.h"

#include <QtCore/QSignalBlocker>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QShortcut>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace Core::KukagramAccountGroups {

[[nodiscard]] inline rpl::variable<std::optional<Group>> &MenuGroup() {
	static auto value = rpl::variable<std::optional<Group>>();
	return value;
}

inline void RefreshMenuGroup(const Groups &groups, const QString &renamed = {}) {
	const auto &active = MenuGroup().current();
	if (!active) {
		return;
	}
	const auto &name = renamed.isEmpty() ? active->name : renamed;
	const auto found = std::find_if(groups.begin(), groups.end(), [&](const Group &group) {
		return group.name == name;
	});
	MenuGroup() = (found == groups.end()) ? std::nullopt : std::optional(*found);
}

class GroupsDialog final : public QDialog {
public:
	GroupsDialog(
		QWidget *parent,
		not_null<Main::Domain*> domain,
		QString workDir,
		Groups groups,
		Fn<void()> showMenu = {},
		bool quickSearch = false)
	: QDialog(parent)
	, _domain(base::make_weak(domain))
	, _workDir(std::move(workDir))
	, _groups(std::move(groups))
	, _quickSearch(quickSearch) {
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_QuitOnClose, false);
		setWindowModality(Qt::ApplicationModal);
		setWindowTitle(_quickSearch ? u"Поиск аккаунта"_q : u"Группы аккаунтов"_q);
		resize(560, 490);

		const auto layout = new QVBoxLayout(this);
		const auto groupsRow = new QHBoxLayout();
		layout->addLayout(groupsRow);
		_group = new QComboBox(this);
		_group->setAccessibleName(u"Группа аккаунтов"_q);
		_group->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		_group->setMinimumContentsLength(14);
		groupsRow->addWidget(_group, 1);
		_add = addButton(groupsRow, u"Создать"_q);
		_rename = addButton(groupsRow, u"Переименовать"_q);
		_remove = addButton(groupsRow, u"Удалить группу"_q);

		const auto toolsRow = new QHBoxLayout();
		layout->addLayout(toolsRow);
		_search = new QLineEdit(this);
		_search->setAccessibleName(u"Поиск аккаунта"_q);
		_search->setPlaceholderText(u"Имя, подпись, @username или номер"_q);
		termsChanged();
		toolsRow->addWidget(_search, 1);
		_view = new QComboBox(this);
		_view->setAccessibleName(u"Какие аккаунты показать"_q);
		_view->addItem(u"Участники"_q);
		_view->addItem(u"Все аккаунты"_q);
		toolsRow->addWidget(_view);

		_accounts = new QListWidget(this);
		_accounts->setAccessibleName(u"Список аккаунтов группы"_q);
		layout->addWidget(_accounts, 1);
		_status = new QLabel(this);
		_status->setWordWrap(true);
		layout->addWidget(_status);

		const auto actionsRow = new QHBoxLayout();
		layout->addLayout(actionsRow);
		_addCurrent = addButton(actionsRow, u"Добавить текущий аккаунт"_q);
		_open = addButton(actionsRow, u"Переключиться"_q);
		_openInMenu = addButton(actionsRow, u"Открыть в меню ☰"_q);
		_editLabel = addButton(actionsRow, u"Изменить подпись"_q);
		_editLabel->setVisible(_quickSearch);
		actionsRow->addStretch();
		const auto close = addButton(actionsRow, u"Закрыть"_q);

		connect(close, &QPushButton::clicked, this, &QDialog::reject);
		connect(_group, &QComboBox::currentIndexChanged, this, [=] {
			setDefaultView();
			rebuildAccounts();
		});
		connect(_view, &QComboBox::currentIndexChanged, this, [=] {
			filter();
		});
		connect(_search, &QLineEdit::textChanged, this, [=] {
			termsChanged();
			filter();
		});
		connect(_search, &QLineEdit::returnPressed, this, [=] {
			activateSelected();
		});
		connect(_add, &QPushButton::clicked, this, [=] { editName(-1); });
		connect(_rename, &QPushButton::clicked, this, [=] {
			editName(_group->currentIndex());
		});
		connect(_remove, &QPushButton::clicked, this, [=] { removeGroup(); });
		connect(_addCurrent, &QPushButton::clicked, this, [=] {
			if (const auto id = currentAccountId(); id && setMembership(id, true)) {
				rebuildAccounts();
			}
		});
		connect(_open, &QPushButton::clicked, this, [=] { activateSelected(); });
		connect(_editLabel, &QPushButton::clicked, this, [=] { editLabel(); });
		connect(_openInMenu, &QPushButton::clicked, this, [=] {
			const auto group = selectedGroup();
			if (!group || !liveDomain()) {
				return;
			}
			MenuGroup() = *group;
			Core::App().settings().setMainMenuAccountsShown(true);
			accept();
			if (showMenu) {
				QTimer::singleShot(0, QCoreApplication::instance(), showMenu);
			}
		});
		connect(_accounts, &QListWidget::currentItemChanged, this, [=] {
			updateButtons();
		});
		connect(_accounts, &QListWidget::itemChanged, this, [=](
				QListWidgetItem *item) {
			if (_quickSearch) {
				return;
			}
			const auto checked = (item->checkState() == Qt::Checked);
			if (setMembership(item->data(Qt::UserRole).toULongLong(), checked)) {
				filter();
			} else {
				const auto blocker = QSignalBlocker(_accounts);
				item->setCheckState(checked ? Qt::Unchecked : Qt::Checked);
			}
		});
		connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
			this, &QDialog::reject);
		connect(&Core::App(), &QObject::destroyed, this, &QDialog::reject);
		Core::App().passcodeLockChanges(
		) | rpl::filter([](bool locked) { return locked; }) | rpl::on_next([=] {
			reject();
		}, _lifetime);
		domain->accountsChanges() | rpl::on_next([=] {
			if (liveDomain()) rebuildAccounts();
		}, _lifetime);
		domain->activeSessionChanges() | rpl::on_next([=](Main::Session*) {
			if (liveDomain()) rebuildAccounts();
		}, _lifetime);

		if (_quickSearch) {
			for (const auto widget : std::initializer_list<QWidget*>{
				_group, _view,
				_add, _rename, _remove, _addCurrent, _openInMenu,
			}) {
				widget->hide();
			}
			connect(_accounts, &QListWidget::itemActivated, this, [=] {
				activateSelected();
			});
			for (const auto key : { Qt::Key_Up, Qt::Key_Down }) {
				const auto shortcut = new QShortcut(QKeySequence(key), _search);
				shortcut->setContext(Qt::WidgetShortcut);
				connect(shortcut, &QShortcut::activated, this, [=] {
					moveSelection(key == Qt::Key_Down ? 1 : -1);
				});
			}
			rebuildAccounts();
			_search->setFocus();
			return;
		}
		const auto &active = MenuGroup().current();
		const auto current = active
			? std::find_if(_groups.begin(), _groups.end(), [&](const Group &group) {
				return group.name == active->name;
			})
			: _groups.end();
		rebuildGroups(current == _groups.end() ? 0 : int(current - _groups.begin()));
	}

private:
	static QPushButton *addButton(QHBoxLayout *layout, const QString &text) {
		const auto result = new QPushButton(text);
		result->setAutoDefault(false);
		layout->addWidget(result);
		return result;
	}

	[[nodiscard]] Main::Domain *liveDomain() const {
		const auto domain = _domain.get();
		return (!Core::Quitting() && !Core::App().passcodeLocked()
			&& domain && domain->started())
			? domain : nullptr;
	}

	[[nodiscard]] quint64 currentAccountId() const {
		if (const auto domain = liveDomain()) {
			if (const auto session = domain->active().maybeSession()) {
				return session->uniqueId();
			}
		}
		return 0;
	}

	[[nodiscard]] const Group *selectedGroup() const {
		const auto index = _group->currentIndex();
		return (index >= 0 && index < int(_groups.size()))
			? &_groups[index] : nullptr;
	}

	void termsChanged() {
		_terms = _search->text().simplified().split(
			QChar(' '), Qt::SkipEmptyParts);
	}

	void warn(const QString &text) {
		const auto warning = new QMessageBox(
			QMessageBox::Warning, windowTitle(),
			text, QMessageBox::Ok, this);
		warning->setTextFormat(Qt::PlainText);
		warning->setAttribute(Qt::WA_DeleteOnClose);
		warning->open();
	}

	[[nodiscard]] QString labelsPath() const {
		return QDir(_workDir).filePath(u"account-labels.ini"_q);
	}

	void moveSelection(int direction) {
		for (auto i = _accounts->currentRow() + direction;
			i >= 0 && i < _accounts->count(); i += direction) {
			if (!_accounts->item(i)->isHidden()) {
				_accounts->setCurrentRow(i);
				_accounts->scrollToItem(_accounts->item(i));
				return;
			}
		}
	}

	void editLabel() {
		const auto item = _accounts->currentItem();
		if (!liveDomain() || !item || item->isHidden()) {
			return;
		}
		const auto id = item->data(Qt::UserRole).toULongLong();
		const auto key = QString::number(id);
		const auto path = labelsPath();
		if (QFileInfo(path).isSymLink()) {
			warn(u"Не удалось открыть файл локальных подписей."_q);
			return;
		}
		const auto settings = QSettings(path, QSettings::IniFormat);
		const auto input = new QInputDialog(this);
		input->setAttribute(Qt::WA_DeleteOnClose);
		input->setWindowTitle(u"Локальная подпись"_q);
		input->setLabelText(u"До %1 символов. Пустое поле удаляет подпись."_q.arg(kMaxNameLength));
		input->setTextValue(settings.value(key).toString());
		connect(input, &QInputDialog::textValueSelected, this, [=](const QString &text) {
			if (!liveDomain() || !id) {
				return;
			}
			const auto label = NormalizeName(text);
			if (label.isEmpty() && !text.trimmed().isEmpty()) {
				warn(u"Введи подпись до %1 символов без переносов строк."_q.arg(kMaxNameLength));
				return;
			}
			auto settings = QSettings(path, QSettings::IniFormat);
			if (QFileInfo(path).isSymLink() || settings.status() != QSettings::NoError) {
				warn(u"Не удалось открыть файл локальных подписей."_q);
				return;
			}
			if (label.isEmpty()) {
				settings.remove(key);
			} else {
				settings.setValue(key, label);
			}
			settings.sync();
			if (settings.status() != QSettings::NoError) {
				warn(u"Не удалось сохранить локальную подпись."_q);
				return;
			}
			rebuildAccounts();
		});
		input->open();
	}

	bool commit(Groups groups, const QString &renamedActive = {}) {
		if (!liveDomain()) {
			return false;
		}
		if (!Save(_workDir, groups, &_groups)) {
			warn(u"Не удалось сохранить группы. Попробуй закрыть окно и открыть его снова."_q);
			return false;
		}
		_groups = std::move(groups);
		RefreshMenuGroup(_groups, renamedActive);
		return true;
	}

	void editName(int index) {
		if (index >= int(_groups.size()) || !liveDomain()) {
			return;
		}
		const auto input = new QInputDialog(this);
		input->setAttribute(Qt::WA_DeleteOnClose);
		input->setWindowTitle(index < 0 ? u"Новая группа"_q : u"Переименовать группу"_q);
		input->setLabelText(u"Название группы"_q);
		input->setTextValue(index < 0 ? QString() : _groups[index].name);
		connect(input, &QInputDialog::textValueSelected, this, [=](const QString &text) {
			const auto name = NormalizeName(text);
			if (name.isEmpty()) {
				warn(u"Введи название группы до %1 символов."_q.arg(kMaxNameLength));
				return;
			}
			for (auto i = 0; i != int(_groups.size()); ++i) {
				if (i != index
					&& _groups[i].name.toCaseFolded() == name.toCaseFolded()) {
					warn(u"Группа с таким названием уже есть."_q);
					return;
				}
			}
			auto groups = _groups;
			if (index < 0 && groups.size() < kMaxGroups) {
				groups.push_back({ name, {} });
			} else if (index >= 0 && index < int(groups.size())) {
				groups[index].name = name;
			} else {
				return;
			}
			const auto &active = MenuGroup().current();
			const auto renamedActive = (index >= 0
				&& active
				&& active->name == _groups[index].name);
			if (commit(std::move(groups), renamedActive ? name : QString())) {
				rebuildGroups(index < 0 ? int(_groups.size()) - 1 : index);
			}
		});
		input->open();
	}

	void removeGroup() {
		if (!liveDomain() || !selectedGroup()) {
			return;
		}
		const auto index = _group->currentIndex();
		const auto prompt = new QMessageBox(
			QMessageBox::Question,
			u"Удалить группу"_q,
			u"Удалить группу «%1»? Аккаунты останутся в общем списке."_q
				.arg(_groups[index].name),
			QMessageBox::NoButton,
			this);
		prompt->setTextFormat(Qt::PlainText);
		prompt->setAttribute(Qt::WA_DeleteOnClose);
		const auto remove = prompt->addButton(u"Удалить группу"_q,
			QMessageBox::DestructiveRole);
		const auto cancel = prompt->addButton(QMessageBox::Cancel);
		prompt->setDefaultButton(cancel);
		connect(prompt, &QMessageBox::buttonClicked, this, [=](QAbstractButton *button) {
			if (button != remove || index >= int(_groups.size())) {
				return;
			}
			auto groups = _groups;
			groups.erase(groups.begin() + index);
			if (commit(std::move(groups))) {
				rebuildGroups(index);
			}
		});
		prompt->open();
	}

	bool setMembership(quint64 id, bool checked) {
		const auto group = selectedGroup();
		if (!group || !id || !liveDomain()) {
			return false;
		}
		auto groups = _groups;
		auto &members = groups[_group->currentIndex()].members;
		const auto found = std::find(members.begin(), members.end(), id);
		if (checked && found == members.end()) {
			members.push_back(id);
		} else if (!checked && found != members.end()) {
			members.erase(found);
		} else {
			return true;
		}
		return commit(std::move(groups));
	}

	void rebuildGroups(int index) {
		const auto blocker = QSignalBlocker(_group);
		_group->clear();
		for (const auto &group : _groups) {
			_group->addItem(group.name);
		}
		if (!_groups.empty()) {
			_group->setCurrentIndex(std::clamp(index, 0, int(_groups.size()) - 1));
		}
		setDefaultView();
		rebuildAccounts();
	}

	void setDefaultView() {
		const auto group = selectedGroup();
		const auto blocker = QSignalBlocker(_view);
		_view->setCurrentIndex((group && group->members.empty()) ? 1 : 0);
	}

	void rebuildAccounts() {
		const auto group = selectedGroup();
		const auto domain = liveDomain();
		const auto blocker = QSignalBlocker(_accounts);
		const auto selected = _accounts->currentItem()
			? _accounts->currentItem()->data(Qt::UserRole).toULongLong() : 0;
		_accounts->clear();
		if (domain && (group || _quickSearch)) {
			auto members = std::unordered_set<quint64>();
			if (group) {
				members.insert(group->members.begin(), group->members.end());
			}
			const auto path = labelsPath();
			const auto labels = QFileInfo(path).isSymLink()
				? nullptr
				: std::make_unique<QSettings>(path, QSettings::IniFormat);
			const auto currentId = currentAccountId();
			for (const auto &account : domain->storedAccounts()) {
				const auto id = account.uniqueId;
				if (!id) {
					continue;
				}
				auto label = account.name.isEmpty()
					? u"Аккаунт %1"_q.arg(account.index + 1)
					: account.name;
				if (labels) {
					const auto alias = NormalizeName(labels->value(QString::number(id)).toString());
					if (!alias.isEmpty()) {
						label = alias + u"  ·  "_q + label;
					}
				}
				if (!account.username.isEmpty()) {
					label += u"  ·  @"_q + account.username;
				}
				if (!account.phone.isEmpty()) {
					label += u"  ·  "_q + Ui::FormatPhone(account.phone);
				}
				if (id == currentId) {
					label += u"  ·  текущий"_q;
				}
				const auto item = new QListWidgetItem(label, _accounts);
				item->setData(Qt::UserRole, QVariant::fromValue(qulonglong(id)));
				if (!_quickSearch) {
					item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
					item->setCheckState(members.contains(id) ? Qt::Checked : Qt::Unchecked);
				}
				if (id == selected) {
					_accounts->setCurrentItem(item);
				}
			}
		}
		filter();
	}

	void filter() {
		const auto blocker = QSignalBlocker(_accounts);
		const auto group = selectedGroup();
		const auto membersOnly = !_quickSearch && (_view->currentIndex() == 0);
		auto shown = 0;
		auto members = 0;
		QListWidgetItem *first = nullptr;
		for (auto i = 0; i != _accounts->count(); ++i) {
			const auto item = _accounts->item(i);
			const auto checked = item->checkState() == Qt::Checked;
			const auto matches = LabelMatches(item->text(), _search->text());
			const auto show = (!membersOnly || checked) && matches;
			item->setHidden(!show);
			members += checked;
			if (show) {
				++shown;
				if (!first) first = item;
			}
		}
		const auto current = _accounts->currentItem();
		if (!current || current->isHidden()) {
			_accounts->setCurrentItem(first);
		}
		if (_quickSearch) {
			_status->setText(shown
				? u"Найдено: %1 · Enter — переключиться"_q.arg(shown)
				: u"Аккаунты не найдены."_q);
		} else if (!group) {
			_status->setText(u"Создай группу, чтобы добавить аккаунты."_q);
		} else if (!shown) {
			_status->setText(_terms.empty()
				? u"В группе пока нет доступных аккаунтов."_q
				: u"Аккаунты не найдены."_q);
		} else {
			_status->setText(u"Участников: %1"_q.arg(members));
		}
		updateButtons();
	}

	void updateButtons() {
		const auto group = selectedGroup();
		const auto id = currentAccountId();
		const auto item = _accounts->currentItem();
		_add->setEnabled(_groups.size() < kMaxGroups);
		_group->setEnabled(group != nullptr);
		_rename->setEnabled(group != nullptr);
		_remove->setEnabled(group != nullptr);
		_view->setEnabled(group != nullptr);
		_search->setEnabled(group != nullptr || _quickSearch);
		const auto canOpen = (group || _quickSearch) && item && !item->isHidden();
		_open->setEnabled(canOpen);
		_editLabel->setEnabled(canOpen);
		_openInMenu->setEnabled(group != nullptr);
		_addCurrent->setEnabled(group && id
			&& std::find(group->members.begin(), group->members.end(), id)
				== group->members.end());
	}

	void activateSelected() {
		const auto item = _accounts->currentItem();
		if (!liveDomain() || !item || item->isHidden()) {
			return;
		}
		const auto id = item->data(Qt::UserRole).toULongLong();
		const auto weak = _domain;
		accept();
		QTimer::singleShot(0, QCoreApplication::instance(), [=] {
			const auto domain = weak.get();
			if (!domain || Core::Quitting() || !domain->started()
				|| Core::App().passcodeLocked()) {
				return;
			}
			const auto accounts = domain->storedAccounts();
			const auto i = std::find_if(accounts.begin(), accounts.end(),
				[=](const Main::StoredAccount &account) { return account.uniqueId == id; });
			if (i != accounts.end()) {
				domain->activateStored(i->index);
			}
		});
	}

	base::weak_ptr<Main::Domain> _domain;
	QString _workDir;
	Groups _groups;
	bool _quickSearch = false;
	QStringList _terms;
	QComboBox *_group = nullptr;
	QComboBox *_view = nullptr;
	QLineEdit *_search = nullptr;
	QListWidget *_accounts = nullptr;
	QLabel *_status = nullptr;
	QPushButton *_add = nullptr;
	QPushButton *_rename = nullptr;
	QPushButton *_remove = nullptr;
	QPushButton *_addCurrent = nullptr;
	QPushButton *_open = nullptr;
	QPushButton *_openInMenu = nullptr;
	QPushButton *_editLabel = nullptr;
	rpl::lifetime _lifetime;
};

} // namespace Core::KukagramAccountGroups

namespace Core {

inline void ShowKukagramAccountGroupsDialog(
		Fn<void()> showMenu = {},
		bool quickSearch = false) {
	if (Quitting() || App().passcodeLocked() || !App().domain().started()) {
		return;
	}
	if (const auto modal = QApplication::activeModalWidget()) {
		modal->raise();
		modal->activateWindow();
		return;
	}
	App().activate();
	const auto parent = QApplication::activeWindow();
	auto result = quickSearch
		? KukagramAccountGroups::ReadResult{ .ok = true }
		: KukagramAccountGroups::Load(cWorkingDir());
	if (!result.ok) {
		const auto warning = new QMessageBox(
			QMessageBox::Warning,
			u"Группы аккаунтов"_q,
			u"Не удалось прочитать сохранённые группы аккаунтов."_q,
			QMessageBox::Ok,
			parent);
		warning->setAttribute(Qt::WA_DeleteOnClose);
		warning->open();
		return;
	}
	if (!quickSearch) {
		KukagramAccountGroups::RefreshMenuGroup(result.groups);
	}
	const auto dialog = new KukagramAccountGroups::GroupsDialog(
		parent,
		&App().domain(),
		cWorkingDir(),
		std::move(result.groups),
		std::move(showMenu),
		quickSearch);
	dialog->show();
}

inline void ShowKukagramAccountSearch() {
	ShowKukagramAccountGroupsDialog({}, true);
}

} // namespace Core
