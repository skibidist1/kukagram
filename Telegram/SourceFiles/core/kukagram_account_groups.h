/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <algorithm>
#include <utility>
#include <vector>

namespace Core::KukagramAccountGroups {

inline constexpr auto kMaxGroups = 100;
inline constexpr auto kMaxMembers = 1000;
inline constexpr auto kMaxNameLength = 64;

struct Group final {
	QString name;
	std::vector<quint64> members;

	bool operator==(const Group &) const = default;
};

using Groups = std::vector<Group>;

struct ReadResult final {
	Groups groups;
	bool ok = false;
};

[[nodiscard]] inline QString NormalizeName(const QString &name) {
	auto normalized = name.trimmed().normalized(QString::NormalizationForm_C);
	if (normalized.size() > kMaxNameLength) {
		return {};
	}
	for (const auto character : normalized) {
		if (character.category() == QChar::Other_Control
			|| character.category() == QChar::Separator_Line
			|| character.category() == QChar::Separator_Paragraph) {
			return {};
		}
	}
	return normalized;
}

[[nodiscard]] inline QString DigitsOnly(const QString &text) {
	auto result = QString();
	for (const auto character : text) {
		if (character.isDigit()) {
			result += character;
		}
	}
	return result;
}

class SearchQuery final {
public:
	explicit SearchQuery(const QString &query) {
		const auto simplified = query.simplified();
		_terms = simplified.split(QChar(' '), Qt::SkipEmptyParts);
		const auto phoneLike = std::all_of(
			simplified.begin(),
			simplified.end(),
			[](QChar character) {
				return character.isDigit()
					|| character == QChar('+')
					|| character == QChar(' ')
					|| character == QChar('-')
					|| character == QChar('(')
					|| character == QChar(')');
			});
		if (phoneLike) {
			_digits = DigitsOnly(simplified);
		}
	}

	[[nodiscard]] bool matches(
			const QString &label,
			const QString &phoneDigits) const {
		return (!_digits.isEmpty() && phoneDigits.contains(_digits))
			|| std::all_of(_terms.begin(), _terms.end(), [&](const QString &term) {
				return label.contains(term, Qt::CaseInsensitive);
			});
	}

	[[nodiscard]] bool empty() const {
		return _terms.isEmpty();
	}

private:
	QStringList _terms;
	QString _digits;

};

inline constexpr auto kPhoneDigitsRole = Qt::UserRole + 2;

namespace details {

inline constexpr auto kVersion = 1;
inline constexpr auto kMaxFileSize = qint64(4 * 1024 * 1024);

[[nodiscard]] inline QString FilePath(const QString &workDir) {
	return QDir(workDir).filePath(QStringLiteral("account-groups.json"));
}

[[nodiscard]] inline bool UsableDirectory(const QString &workDir) {
	if (workDir.isEmpty()) {
		return false;
	}
	const auto directory = QFileInfo(workDir);
#ifdef Q_OS_UNIX
	if (!directory.isExecutable()) {
		return false;
	}
#endif // Q_OS_UNIX
	return directory.isDir() && directory.isReadable();
}

[[nodiscard]] inline bool AddName(QSet<QString> &unique, const QString &name) {
	if (name.isEmpty()) {
		return false;
	}
	const auto key = name.toCaseFolded();
	if (unique.contains(key)) {
		return false;
	}
	unique.insert(key);
	return true;
}

[[nodiscard]] inline bool AddMember(QSet<quint64> &unique, quint64 id) {
	if (!id || unique.contains(id)) {
		return false;
	}
	unique.insert(id);
	return true;
}

} // namespace details

[[nodiscard]] inline ReadResult Load(const QString &workDir) {
	if (!details::UsableDirectory(workDir)) {
		return {};
	}
	const auto path = details::FilePath(workDir);
	const auto info = QFileInfo(path);
	if (info.isSymLink()) {
		return {};
	} else if (!info.exists()) {
		return { .ok = true };
	} else if (!info.isFile() || info.size() > details::kMaxFileSize) {
		return {};
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto bytes = file.read(details::kMaxFileSize + 1);
	if (file.error() != QFileDevice::NoError
		|| bytes.size() > details::kMaxFileSize
		|| !file.atEnd()) {
		return {};
	}
	const auto object = QJsonDocument::fromJson(bytes).object();
	const auto value = object.value(QStringLiteral("groups"));
	if (object.value(QStringLiteral("version")).toDouble() != details::kVersion
		|| !value.isArray()) {
		return {};
	}
	const auto array = value.toArray();
	if (array.size() > kMaxGroups) {
		return {};
	}

	auto groups = Groups();
	groups.reserve(array.size());
	auto names = QSet<QString>();
	for (const auto item : array) {
		const auto group = item.toObject();
		auto name = NormalizeName(group.value(QStringLiteral("name")).toString());
		const auto memberValue = group.value(QStringLiteral("members"));
		if (!details::AddName(names, name) || !memberValue.isArray()) {
			return {};
		}
		const auto members = memberValue.toArray();
		if (members.size() > kMaxMembers) {
			return {};
		}
		auto ids = QSet<quint64>();
		auto parsed = Group{ .name = std::move(name) };
		parsed.members.reserve(members.size());
		for (const auto member : members) {
			const auto text = member.toString();
			const auto id = text.toULongLong();
			if (text != QString::number(id) || !details::AddMember(ids, id)) {
				return {};
			}
			parsed.members.push_back(id);
		}
		groups.push_back(std::move(parsed));
	}
	return { std::move(groups), true };
}

[[nodiscard]] inline bool Save(
		const QString &workDir,
		const Groups &groups,
		const Groups *expected = nullptr) {
	if (groups.size() > kMaxGroups || !details::UsableDirectory(workDir)) {
		return false;
	}
	auto array = QJsonArray();
	auto names = QSet<QString>();
	for (const auto &group : groups) {
		const auto name = NormalizeName(group.name);
		if (!details::AddName(names, name)
			|| group.members.size() > kMaxMembers) {
			return false;
		}
		auto members = QJsonArray();
		auto ids = QSet<quint64>();
		// Account IDs may exceed the exact integer range of a JSON number.
		for (const auto id : group.members) {
			if (!details::AddMember(ids, id)) {
				return false;
			}
			members.append(QString::number(id));
		}
		array.append(QJsonObject{
			{ QStringLiteral("name"), name },
			{ QStringLiteral("members"), members },
		});
	}
	const auto path = details::FilePath(workDir);
	auto lock = QLockFile(path + QStringLiteral(".lock"));
	if (!lock.tryLock(0)) {
		return false;
	}
	const auto current = Load(workDir);
	if (!current.ok || (expected && current.groups != *expected)) {
		return false;
	}
	const auto object = QJsonObject{
		{ QStringLiteral("version"), details::kVersion },
		{ QStringLiteral("groups"), array },
	};
	const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly)
		&& file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
		&& file.write(bytes) == bytes.size()
		&& file.commit();
}

} // namespace Core::KukagramAccountGroups
