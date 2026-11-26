#pragma once

#include "core/kukagram_account_groups_dialog.h"

#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPixmap>

#include <array>
#include <unordered_map>

namespace Core::KukagramAccountGroups {

[[nodiscard]] inline QColor GridGroupColor(int index) {
	static const auto colors = std::array{
		QColor(143, 99, 242),
		QColor(29, 158, 117),
		QColor(216, 90, 48),
		QColor(55, 138, 221),
		QColor(212, 83, 126),
		QColor(186, 117, 23),
		QColor(99, 153, 34),
		QColor(136, 135, 128),
	};
	return colors[index % int(colors.size())];
}

[[nodiscard]] inline QString GridInitials(const QString &name) {
	auto result = QString();
	for (const auto &word : name.split(' ', Qt::SkipEmptyParts)) {
		for (const auto ch : word) {
			if (ch.isLetterOrNumber()) {
				result += ch.toUpper();
				break;
			}
		}
		if (result.size() == 2) {
			break;
		}
	}
	return result.isEmpty() ? u"?"_q : result;
}

[[nodiscard]] inline QPixmap GridAvatar(
		const QString &name,
		std::optional<QColor> ring,
		bool current,
		qreal ratio) {
	constexpr auto kSize = 64;
	auto pixmap = QPixmap(QSize(kSize, kSize) * ratio);
	pixmap.setDevicePixelRatio(ratio);
	pixmap.fill(Qt::transparent);
	auto p = QPainter(&pixmap);
	p.setRenderHint(QPainter::Antialiasing);
	const auto hue = int(qHash(name) % 360);
	const auto outer = QRectF(2, 2, kSize - 4, kSize - 4);
	if (ring) {
		p.setPen(QPen(*ring, 3.));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(outer);
	}
	const auto inner = outer.adjusted(5, 5, -5, -5);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor::fromHsl(hue, 120, 92));
	p.drawEllipse(inner);
	auto font = p.font();
	font.setPixelSize(18);
	font.setBold(true);
	p.setFont(font);
	p.setPen(QColor::fromHsl(hue, 150, 235));
	p.drawText(inner, Qt::AlignCenter, GridInitials(name));
	if (current) {
		const auto dot = QRectF(kSize - 18, kSize - 18, 14, 14);
		p.setPen(QPen(QColor(255, 255, 255), 2.));
		p.setBrush(QColor(29, 158, 117));
		p.drawEllipse(dot);
	}
	return pixmap;
}

class GridDialog final : public QDialog {
public:
	GridDialog(QWidget *parent, not_null<Main::Domain*> domain)
	: QDialog(parent)
	, _domain(base::make_weak(domain)) {
		setAttribute(Qt::WA_DeleteOnClose);
		setAttribute(Qt::WA_QuitOnClose, false);
		setWindowModality(Qt::ApplicationModal);
		setWindowTitle(u"Аккаунты"_q);
		resize(640, 520);

		const auto layout = new QVBoxLayout(this);
		_search = new QLineEdit(this);
		_search->setAccessibleName(u"Поиск аккаунта"_q);
		_search->setPlaceholderText(u"Имя, подпись, @username или номер"_q);
		layout->addWidget(_search);

		_list = new QListWidget(this);
		_list->setAccessibleName(u"Аккаунты"_q);
		_list->setViewMode(QListView::IconMode);
		_list->setIconSize(QSize(64, 64));
		_list->setGridSize(QSize(112, 106));
		_list->setResizeMode(QListView::Adjust);
		_list->setMovement(QListView::Static);
		_list->setWordWrap(true);
		_list->setUniformItemSizes(true);
		layout->addWidget(_list, 1);

		_legend = new QLabel(this);
		_legend->setWordWrap(true);
		_legend->setTextFormat(Qt::RichText);
		layout->addWidget(_legend);

		connect(_search, &QLineEdit::textChanged, this, [=] { filter(); });
		connect(_search, &QLineEdit::returnPressed, this, [=] {
			activate(firstVisible());
		});
		connect(_list, &QListWidget::itemActivated, this, [=](
				QListWidgetItem *item) {
			activate(item);
		});
		connect(_list, &QListWidget::itemClicked, this, [=](
				QListWidgetItem *item) {
			activate(item);
		});
		build();
		_search->setFocus();
	}

private:
	[[nodiscard]] QListWidgetItem *firstVisible() const {
		if (const auto current = _list->currentItem()
			; current && !current->isHidden()) {
			return current;
		}
		for (auto i = 0; i != _list->count(); ++i) {
			if (!_list->item(i)->isHidden()) {
				return _list->item(i);
			}
		}
		return nullptr;
	}

	void build() {
		const auto domain = _domain.get();
		if (!domain) {
			return;
		}
		const auto groups = Load(cWorkingDir());
		auto colorOf = std::unordered_map<quint64, QColor>();
		auto legend = QStringList();
		if (groups.ok) {
			auto index = 0;
			for (const auto &group : groups.groups) {
				const auto color = GridGroupColor(index++);
				for (const auto id : group.members) {
					colorOf.emplace(id, color);
				}
				legend.push_back(u"<span style=\"color:%1\">●</span> %2"_q.arg(
					color.name(),
					group.name.toHtmlEscaped()));
			}
		}
		_legend->setText(legend.join(u"&nbsp;&nbsp; "_q));
		_legend->setVisible(!legend.isEmpty());

		const auto labels = QSettings(
			QDir(cWorkingDir()).filePath(u"account-labels.ini"_q),
			QSettings::IniFormat);
		const auto currentId = [&]() -> quint64 {
			if (const auto session = domain->active().maybeSession()) {
				return session->uniqueId();
			}
			return 0;
		}();
		const auto ratio = devicePixelRatioF();
		for (const auto &account : domain->storedAccounts()) {
			const auto id = account.uniqueId;
			if (!id) {
				continue;
			}
			const auto alias = NormalizeName(
				labels.value(QString::number(id)).toString());
			const auto name = account.name.isEmpty()
				? u"Аккаунт %1"_q.arg(account.index + 1)
				: account.name;
			const auto title = alias.isEmpty() ? name : alias;
			const auto ring = colorOf.find(id);
			const auto item = new QListWidgetItem(
				QIcon(GridAvatar(
					name,
					(ring != end(colorOf))
						? std::make_optional(ring->second)
						: std::nullopt,
					id == currentId,
					ratio)),
				title,
				_list);
			item->setData(Qt::UserRole, QVariant::fromValue(qulonglong(id)));
			item->setData(
				Qt::UserRole + 1,
				QStringList{
					name,
					alias,
					account.username,
					DigitsOnly(account.phone),
				}.join(' '));
			item->setToolTip(account.phone.isEmpty()
				? name
				: (name + u"\n"_q + Ui::FormatPhone(account.phone)));
			item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
			if (id == currentId) {
				_list->setCurrentItem(item);
			}
		}
	}

	void filter() {
		const auto query = _search->text().trimmed();
		for (auto i = 0; i != _list->count(); ++i) {
			const auto item = _list->item(i);
			const auto haystack = item->data(Qt::UserRole + 1).toString();
			item->setHidden(!query.isEmpty()
				&& !LabelMatches(haystack, query));
		}
	}

	void activate(QListWidgetItem *item) {
		if (!item || item->isHidden() || !_domain) {
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
			for (const auto &account : domain->storedAccounts()) {
				if (account.uniqueId == id) {
					domain->activateStored(account.index);
					return;
				}
			}
		});
	}

	base::weak_ptr<Main::Domain> _domain;
	QLineEdit *_search = nullptr;
	QListWidget *_list = nullptr;
	QLabel *_legend = nullptr;

};

} // namespace Core::KukagramAccountGroups

namespace Core {

inline void ShowKukagramAccountGrid() {
	if (Quitting() || App().passcodeLocked() || !App().domain().started()) {
		return;
	}
	if (const auto modal = QApplication::activeModalWidget()) {
		modal->raise();
		modal->activateWindow();
		return;
	}
	App().activate();
	const auto dialog = new KukagramAccountGroups::GridDialog(
		QApplication::activeWindow(),
		&App().domain());
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

} // namespace Core
