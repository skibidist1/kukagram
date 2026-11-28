#pragma once

#include "core/application.h"
#include "main/main_domain.h"
#include "settings.h"
#include "ui/emoji_config.h"
#include "ui/text/format_values.h"

#include <crl/crl_async.h>
#include <crl/crl_on_main.h>

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <vector>

namespace Core::KukagramCacheCleanup {

[[nodiscard]] inline bool IsAccountCacheFolder(const QString &path) {
	const auto info = QFileInfo(path);
	if (info.isSymLink()) {
		return false;
	}
	const auto name = info.fileName();
	if (name != u"cache"_q && name != u"media_cache"_q) {
		return false;
	}
	const auto parent = QFileInfo(info.absolutePath());
	if (parent.isSymLink() || !parent.fileName().startsWith(u"user_"_q)) {
		return false;
	}
	const auto root = QDir(cWorkingDir() + u"tdata"_q).absolutePath();
	return QDir(parent.absolutePath()).absolutePath().startsWith(root + '/');
}

[[nodiscard]] inline qint64 FolderSize(const QString &path) {
	if (!IsAccountCacheFolder(path) || !QFileInfo(path).isDir()) {
		return 0;
	}
	auto result = qint64();
	auto i = QDirIterator(
		path,
		QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
		QDirIterator::Subdirectories);
	while (i.hasNext()) {
		i.next();
		result += i.fileInfo().size();
	}
	return result;
}

[[nodiscard]] inline qint64 TotalSize(const std::vector<QString> &paths) {
	auto result = qint64();
	for (const auto &path : paths) {
		result += FolderSize(path);
	}
	return result;
}

class Dialog final : public QDialog {
public:
	Dialog(QWidget *parent, not_null<Main::Domain*> domain)
	: QDialog(parent)
	, _domain(base::make_weak(domain)) {
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_QuitOnClose, false);
		setWindowModality(Qt::ApplicationModal);
		setWindowTitle(u"Очистка кэша"_q);
		setMinimumWidth(420);

		const auto layout = new QVBoxLayout(this);
		_text = new QLabel(u"Считаю, сколько занимает кэш…"_q, this);
		_text->setWordWrap(true);
		layout->addWidget(_text);
		const auto about = new QLabel(
			u"Удаляются только сохранённые фото, видео, стикеры и файлы. "
			u"Аккаунты, переписки и настройки остаются, а медиа загрузятся "
			u"заново, когда понадобятся."_q,
			this);
		about->setWordWrap(true);
		layout->addWidget(about);

		const auto row = new QHBoxLayout();
		row->addStretch();
		_clear = new QPushButton(u"Очистить"_q, this);
		_clear->setEnabled(false);
		_close = new QPushButton(u"Закрыть"_q, this);
		row->addWidget(_clear);
		row->addWidget(_close);
		layout->addLayout(row);

		connect(_close, &QPushButton::clicked, this, &QDialog::reject);
		connect(_clear, &QPushButton::clicked, this, [=] { clear(); });
		measure();
	}

protected:
	void reject() override {
		if (!_clearing) {
			QDialog::reject();
		}
	}

private:
	void measure() {
		const auto domain = _domain.get();
		if (!domain) {
			return;
		}
		const auto paths = domain->accountCacheFolders(false);
		const auto accounts = domain->storedAccountsCount();
		const auto weak = QPointer<Dialog>(this);
		crl::async([=] {
			const auto size = TotalSize(paths);
			crl::on_main([=] {
				if (weak) {
					weak->measured(size, accounts);
				}
			});
		});
	}

	void measured(qint64 size, int accounts) {
		_before = size;
		_text->setText(u"Кэш %1 аккаунтов занимает %2."_q
			.arg(accounts)
			.arg(Ui::FormatSizeText(size)));
		_clear->setEnabled(size > 0);
	}

	void clear() {
		const auto domain = _domain.get();
		if (!domain || _clearing) {
			return;
		} else if (!domain->canClearAccountCaches()) {
			_text->setText(u"Сейчас идёт переключение аккаунтов. "
				u"Попробуй через пару секунд."_q);
			return;
		}
		_clearing = true;
		_clear->setEnabled(false);
		_close->setEnabled(false);
		_text->setText(u"Очищаю кэш…"_q);
		domain->setClearingAccountCaches(true);
		domain->clearLoadedAccountCaches();
		Ui::Emoji::ClearIrrelevantCache();
		const auto unloaded = domain->accountCacheFolders(true);
		const auto all = domain->accountCacheFolders(false);
		const auto weak = QPointer<Dialog>(this);
		const auto domainWeak = _domain;
		crl::async([=] {
			for (const auto &path : unloaded) {
				if (IsAccountCacheFolder(path)) {
					QDir(path).removeRecursively();
				}
			}
			crl::on_main([=] {
				if (const auto domain = domainWeak.get()) {
					domain->setClearingAccountCaches(false);
				}
				if (weak) {
					weak->cleared(all);
				}
			});
		});
	}

	void cleared(const std::vector<QString> &paths) {
		const auto weak = QPointer<Dialog>(this);
		QTimer::singleShot(1500, this, [=] {
			crl::async([=] {
				const auto after = TotalSize(paths);
				crl::on_main([=] {
					if (weak) {
						weak->finished(after);
					}
				});
			});
		});
	}

	void finished(qint64 after) {
		_clearing = false;
		_close->setEnabled(true);
		_text->setText(u"Готово: освобождено %1."_q.arg(
			Ui::FormatSizeText(std::max(_before - after, qint64()))));
	}

	base::weak_ptr<Main::Domain> _domain;
	QLabel *_text = nullptr;
	QPushButton *_clear = nullptr;
	QPushButton *_close = nullptr;
	qint64 _before = 0;
	bool _clearing = false;

};

} // namespace Core::KukagramCacheCleanup

namespace Core {

inline void ShowKukagramCacheCleanup() {
	if (Quitting() || App().passcodeLocked() || !App().domain().started()) {
		return;
	}
	if (const auto modal = QApplication::activeModalWidget()) {
		modal->raise();
		modal->activateWindow();
		return;
	}
	App().activate();
	const auto dialog = new KukagramCacheCleanup::Dialog(
		QApplication::activeWindow(),
		&App().domain());
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

} // namespace Core
