/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/storage_domain.h"
#include "storage/storage_account.h"

#include "core/version.h"
#include "storage/details/storage_file_utilities.h"
#include "storage/details/storage_settings_scheme.h"
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "mtproto/mtproto_config.h"
#include "main/main_domain.h"
#include "main/main_account.h"
#include "base/openssl_help.h"
#include "base/random.h"
#include "tgw/tgw_storage.h"

namespace Storage {

struct PasscodeWrap final {
	details::PasscodeKdf kdf;
	quint32 generation = 0;
	QByteArray salt;
	QByteArray keyEncrypted;
};

// The two legacy flags travel together and are cleared together. legacy says
// the file carries only the three old blobs, so openKeyEncrypted is whatever
// the pre-change build wrote there; legacyPasscode says that blob is wrapped
// under a passcode rather than under the empty string. The parse cannot tell
// the two apart - both look like a non-empty openKeyEncrypted - so the second
// flag is set at the point the typed passcode proved which one it is, and it
// is what lets hasPasscode() and appLockEnabled() keep reporting the truth
// while a migration that failed to derive its wrap waits for the next start.
struct KeyData final {
	QByteArray openSalt;
	QByteArray openKeyEncrypted;
	std::vector<PasscodeWrap> passcodeWraps;
	quint32 committed = 0;
	bool legacy = false;
	bool legacyPasscode = false;
	bool openKeyVerified = false;
};

namespace {

using namespace details;

enum class ReadKeyDataResult {
	Success,
	UnsupportedKdf,
	Failed,
};

// A legacy key_data stream holds exactly three blobs, so it is at the end
// right after infoEncrypted and any bytes past it mean the new layout. The
// magic word turns "there are extra bytes" into a positive identification.
// AppVersion does not change with this format, so the version stamped in the
// file cannot discriminate the two shapes and the migration is driven by the
// parse-time legacy flag instead. The trailer travels as one appended blob
// because FileWriteDescriptor writes blobs and hashes the file over exactly
// those, so raw stream fields would not be covered by its md5.
constexpr auto kKeyDataMagic = quint32(0x4B443200);
constexpr auto kKeyDataFormatVersion = quint32(2);
constexpr auto kMaxWrapCount = quint32(2);

[[nodiscard]] QString BaseGlobalPath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString ComputeKeyName(const QString &dataName) {
	// We dropped old test authorizations when migrated to multi auth.
	//return "key_" + dataName + (cTestMode() ? "[test]" : "");
	return "key_" + dataName;
}

[[nodiscard]] QString ComputeWalletKeyringName(const QString &dataName) {
	return u"wallet_keyring_"_q + dataName;
}

[[nodiscard]] QByteArray RandomSalt() {
	auto result = QByteArray(LocalEncryptSaltSize, Qt::Uninitialized);
	base::RandomFill(result.data(), result.size());
	return result;
}

[[nodiscard]] bool SameWrapKey(const PasscodeWrap &a, const PasscodeWrap &b) {
	return (a.kdf.kind == b.kdf.kind)
		&& (a.kdf.memory == b.kdf.memory)
		&& (a.kdf.time == b.kdf.time)
		&& (a.kdf.parallel == b.kdf.parallel)
		&& (a.salt == b.salt);
}

[[nodiscard]] QByteArray WrapLocalKey(
		const MTP::AuthKeyPtr &localKey,
		const MTP::AuthKeyPtr &wrapKey) {
	Expects(localKey != nullptr);
	Expects(wrapKey != nullptr);

	EncryptedDescriptor data(MTP::AuthKey::kSize);
	localKey->write(data.stream);
	return PrepareEncrypted(data, wrapKey);
}

[[nodiscard]] MTP::AuthKeyPtr UnwrapLocalKey(
		const QByteArray &keyEncrypted,
		const MTP::AuthKeyPtr &wrapKey) {
	if (!wrapKey) {
		return nullptr;
	}
	EncryptedDescriptor data;
	if (!DecryptLocal(data, keyEncrypted, wrapKey)) {
		return nullptr;
	}
	auto key = Serialize::read<MTP::AuthKey::Data>(data.stream);
	if (data.stream.status() != QDataStream::Ok || !data.stream.atEnd()) {
		return nullptr;
	}
	return std::make_shared<MTP::AuthKey>(key);
}

[[nodiscard]] bool WrapOpensLocalKey(
		const QByteArray &keyEncrypted,
		const MTP::AuthKeyPtr &wrapKey,
		const MTP::AuthKeyPtr &localKey) {
	const auto recovered = UnwrapLocalKey(keyEncrypted, wrapKey);
	return recovered && recovered->equals(localKey);
}

constexpr auto kImportKeyFileLimit = qint64(1024 * 1024);
constexpr auto kImportMapFileLimit = qint64(16 * 1024 * 1024);
constexpr auto kImportAuthFileLimit = qint64(64 * 1024);
constexpr auto kImportSelfLimit = qsizetype(64 * 1024);
constexpr auto kImportMaxKeys = qint32(128);
constexpr auto kImportMaxDcId = qint32(1000);
constexpr auto kImportSelfSerialized = quint32(0x15);
constexpr auto kImportConfigVersion = qint32(1);
constexpr auto kImportPeerVersionTag = quint64(0x77FF'FFFF'FFFF'FFFFULL);
constexpr auto kImportMaxPeerVersion = qint32(3);
constexpr auto kImportPasscodeLimit = qsizetype(4096);
constexpr auto kImportSaltLimit = qsizetype(1024);
constexpr auto kImportAggregateLimit = qsizetype(64 * 1024 * 1024);
constexpr auto kImportMaxScryptN = quint32(131'072);
constexpr auto kImportMaxScryptR = quint32(8);
constexpr auto kImportMaxScryptP = quint32(16);
constexpr auto kImportWrappedKeySize = qsizetype(16
	+ ((sizeof(uint32) + MTP::AuthKey::kSize + 15) / 16) * 16);

[[nodiscard]] bool ReadImportBytes(
		QDataStream &stream,
		QByteArray &result,
		qsizetype limit) {
	auto size = quint32();
	stream >> size;
	if (stream.status() != QDataStream::Ok) {
		return false;
	} else if (size == std::numeric_limits<quint32>::max()) {
		result = {};
		return true;
	} else if (size > limit || size > stream.device()->bytesAvailable()) {
		stream.setStatus(QDataStream::ReadCorruptData);
		return false;
	}
	result.resize(size);
	if (stream.readRawData(result.data(), size) != size) {
		stream.setStatus(QDataStream::ReadPastEnd);
		return false;
	}
	return true;
}

[[nodiscard]] ReadKeyDataResult ReadKeyData(
		QDataStream &stream,
		KeyData &data,
		QByteArray &infoEncrypted,
		bool strict = false) {
	if (strict) {
		if (!ReadImportBytes(stream, data.openSalt, LocalEncryptSaltSize)
			|| !ReadImportBytes(stream, data.openKeyEncrypted, kImportSaltLimit)
			|| !ReadImportBytes(stream, infoEncrypted, kImportKeyFileLimit)) {
			return ReadKeyDataResult::Failed;
		}
	} else {
		stream >> data.openSalt >> data.openKeyEncrypted >> infoEncrypted;
	}
	if (!CheckStreamStatus(stream)) {
		return ReadKeyDataResult::Failed;
	} else if (data.openSalt.size() != LocalEncryptSaltSize) {
		LOG(("App Error: bad salt in info file, size: %1"
			).arg(data.openSalt.size()));
		return ReadKeyDataResult::Failed;
	}
	if (stream.atEnd()) {
		data.legacy = true;
		return ReadKeyDataResult::Success;
	}
	auto trailer = QByteArray();
	if (strict) {
		if (!ReadImportBytes(stream, trailer, kImportKeyFileLimit)) {
			return ReadKeyDataResult::Failed;
		}
	} else {
		stream >> trailer;
	}
	if (!CheckStreamStatus(stream)) {
		return ReadKeyDataResult::Failed;
	}
	QBuffer buffer(&trailer);
	if (!buffer.open(QIODevice::ReadOnly)) {
		LOG(("App Error: could not open the key data trailer."));
		return ReadKeyDataResult::Failed;
	}
	QDataStream inner(&buffer);
	inner.setVersion(QDataStream::Qt_5_1);

	auto magic = quint32();
	auto formatVersion = quint32();
	auto wrapCount = quint32();
	inner >> magic >> formatVersion >> data.committed >> wrapCount;
	if (!CheckStreamStatus(inner)) {
		return ReadKeyDataResult::Failed;
	} else if (magic != kKeyDataMagic) {
		LOG(("App Error: bad key data trailer magic."));
		return ReadKeyDataResult::Failed;
	} else if (formatVersion > kKeyDataFormatVersion) {
		LOG(("App Error: too new key data format: %1").arg(formatVersion));
		return ReadKeyDataResult::Failed;
	} else if (wrapCount > kMaxWrapCount) {
		LOG(("App Error: bad key data wrap count: %1").arg(wrapCount));
		return ReadKeyDataResult::Failed;
	}
	auto unsupportedCost = false;
	data.passcodeWraps.reserve(wrapCount);
	for (auto i = quint32(); i != wrapCount; ++i) {
		auto wrap = PasscodeWrap();
		inner >> wrap.kdf.kind
			>> wrap.generation
			>> wrap.kdf.memory
			>> wrap.kdf.time
			>> wrap.kdf.parallel;
		if (strict) {
			if (!ReadImportBytes(inner, wrap.salt, kImportSaltLimit)
				|| !ReadImportBytes(inner, wrap.keyEncrypted, kImportSaltLimit)) {
				return ReadKeyDataResult::Failed;
			}
		} else {
			inner >> wrap.salt >> wrap.keyEncrypted;
		}
		if (!CheckStreamStatus(inner)) {
			return ReadKeyDataResult::Failed;
		} else if (!wrap.kdf.costWithinLimits()) {
			unsupportedCost = true;
		} else if (!wrap.kdf.valid()) {
			LOG(("App Error: bad key data KDF family: %1").arg(wrap.kdf.kind));
			return ReadKeyDataResult::Failed;
		}
		if (wrap.salt.size() < kPasscodeSaltMinSize) {
			LOG(("App Error: bad key data wrap salt size: %1"
				).arg(wrap.salt.size()));
			return ReadKeyDataResult::Failed;
		}
		for (const auto &already : data.passcodeWraps) {
			if (already.generation == wrap.generation) {
				LOG(("App Error: duplicate key data wrap generation: %1"
					).arg(wrap.generation));
				return ReadKeyDataResult::Failed;
			}
		}
		data.passcodeWraps.push_back(std::move(wrap));
	}
	if (strict && (!inner.atEnd() || !stream.atEnd())) {
		return ReadKeyDataResult::Failed;
	}
	return unsupportedCost
		? ReadKeyDataResult::UnsupportedKdf
		: ReadKeyDataResult::Success;
}

[[nodiscard]] TdataImportError ReadImportFile(
		FileReadDescriptor &result,
		const QString &name,
		const QString &basePath,
		qint64 limit) {
	const auto folder = QFileInfo(QDir::cleanPath(basePath));
	if (folder.isSymLink()) {
		return TdataImportError::Corrupt;
	}
	const auto base = basePath + name;
	auto candidates = QStringList();
	const auto modern = QFileInfo(base + 's');
	if (modern.exists() || modern.isSymLink()) {
		candidates.push_back(modern.filePath());
	} else {
		const auto zero = QFileInfo(base + '0');
		const auto one = QFileInfo(base + '1');
		if (zero.exists() || zero.isSymLink()) {
			candidates.push_back(zero.filePath());
		}
		if (one.exists() || one.isSymLink()) {
			candidates.push_back(one.filePath());
		}
		if (candidates.size() == 2 && zero.lastModified() < one.lastModified()) {
			std::swap(candidates[0], candidates[1]);
		}
	}
	if (candidates.empty()) {
		return TdataImportError::NotFound;
	}
	auto error = TdataImportError::Corrupt;
	for (const auto &path : candidates) {
		const auto info = QFileInfo(path);
		if (!info.isFile() || info.isSymLink() || info.size() > limit) {
			continue;
		}
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		auto bytes = file.read(limit + 1);
		if (bytes.size() < 24 || bytes.size() > limit || !file.atEnd()) {
			continue;
		}
		constexpr auto magic = std::array{ 'T', 'D', 'F', '$' };
		if (memcmp(bytes.constData(), magic.data(), magic.size())) {
			continue;
		}
		auto version = qint32();
		memcpy(&version, bytes.constData() + magic.size(), sizeof(version));
		if (version < 0) {
			continue;
		} else if (version > AppVersion) {
			error = TdataImportError::Unsupported;
			continue;
		}
		const auto payloadSize = qint32(bytes.size() - 24);
		const auto payload = bytes.constData() + 8;
		auto md5 = HashMd5();
		md5.feed(payload, payloadSize);
		md5.feed(&payloadSize, sizeof(payloadSize));
		md5.feed(&version, sizeof(version));
		md5.feed(magic.data(), magic.size());
		if (memcmp(md5.result(), payload + payloadSize, 16)) {
			continue;
		}
		result.data = QByteArray(payload, payloadSize);
		result.version = version;
		result.buffer.setBuffer(&result.data);
		result.buffer.open(QIODevice::ReadOnly);
		result.stream.setDevice(&result.buffer);
		result.stream.setVersion(QDataStream::Qt_5_1);
		return TdataImportError::None;
	}
	return error;
}

[[nodiscard]] TdataImportError ReadImportEncrypted(
		EncryptedDescriptor &result,
		FileReadDescriptor &file,
		const QString &name,
		const QString &basePath,
		qint64 limit,
		const MTP::AuthKeyPtr &key) {
	const auto error = ReadImportFile(file, name, basePath, limit);
	if (error != TdataImportError::None) {
		return error;
	}
	auto encrypted = QByteArray();
	return (ReadImportBytes(file.stream, encrypted, limit)
		&& file.stream.atEnd()
		&& DecryptLocal(result, encrypted, key))
		? TdataImportError::None
		: TdataImportError::Corrupt;
}

[[nodiscard]] QString ImportAccountName(int index) {
	const auto name = (index > 0)
		? u"data#"_q + QString::number(index + 1)
		: u"data"_q;
	const auto utf8 = name.toUtf8();
	auto hash = std::array<FileKey, 2>{};
	hashMd5(utf8.constData(), utf8.size(), hash.data());
	return ToFilePart(hash[0]);
}

[[nodiscard]] bool ReadImportAuthorization(
		ImportedAccount &result,
		QDataStream &data) {
	auto type = quint32();
	auto authorization = QByteArray();
	data >> type;
	if (!ReadImportBytes(data, authorization, kImportAuthFileLimit)
		|| !data.atEnd()
		|| type != dbiMtpAuthorization) {
		return false;
	}
	auto stream = QDataStream(authorization);
	stream.setVersion(QDataStream::Qt_5_1);
	auto first = quint32();
	auto second = quint32();
	stream >> first >> second;
	auto id = quint64(first);
	auto dc = qint32(second);
	if (((quint64(first) << 32) | second) == ~quint64(0)) {
		stream >> id >> dc;
	}
	if (stream.status() != QDataStream::Ok
		|| id > PeerId::kChatTypeMask
		|| (id && dc <= 0)
		|| dc > kImportMaxDcId) {
		return false;
	}
	auto keys = QByteArray();
	auto keyStream = QDataStream(&keys, QIODevice::WriteOnly);
	keyStream.setVersion(QDataStream::Qt_5_1);
	auto hasMain = false;
	for (auto list = 0; list != 2; ++list) {
		auto count = qint32();
		stream >> count;
		if (stream.status() != QDataStream::Ok
			|| count < 0
			|| count > kImportMaxKeys
			|| (!list && id && !count)) {
			return false;
		}
		if (!list) keyStream << count;
		auto dcs = base::flat_set<qint32>();
		for (auto i = 0; i != count; ++i) {
			auto keyDc = qint32();
			stream >> keyDc;
			const auto key = Serialize::read<MTP::AuthKey::Data>(stream);
			if (stream.status() != QDataStream::Ok
				|| keyDc <= 0
				|| keyDc > kImportMaxDcId
				|| !dcs.emplace(keyDc).second
				|| ranges::all_of(key, [](auto byte) { return byte == gsl::byte{}; })) {
				return false;
			}
			if (!list) {
				keyStream << keyDc;
				keyStream.writeRawData(reinterpret_cast<const char*>(key.data()), key.size());
				hasMain = hasMain || (keyDc == dc);
			}
		}
	}
	if (stream.status() != QDataStream::Ok || !stream.atEnd() || (id && !hasMain)) {
		return false;
	}
	auto normalized = QDataStream(&result.authorization, QIODevice::WriteOnly);
	normalized.setVersion(QDataStream::Qt_5_1);
	normalized << ~quint64(0) << id << dc;
	normalized.writeRawData(keys.constData(), keys.size());
	normalized << qint32(0);
	result.uniqueId = id;
	return normalized.status() == QDataStream::Ok;
}

[[nodiscard]] TdataImportError ReadImportAccount(
		ImportedAccount &result,
		const QString &basePath,
		const MTP::AuthKeyPtr &key,
		EncryptedDescriptor *legacyMap = nullptr,
		int legacyVersion = 0) {
	const auto name = ImportAccountName(result.sourceIndex);
	const auto accountPath = basePath + name + '/';
	auto auth = EncryptedDescriptor();
	auto authFile = FileReadDescriptor();
	const auto error = ReadImportEncrypted(
		auth, authFile, name, basePath, kImportAuthFileLimit, key);
	if (error != TdataImportError::None) {
		return error;
	} else if (!ReadImportAuthorization(result, auth.stream)) {
		return TdataImportError::Corrupt;
	} else if (!result.uniqueId) {
		return TdataImportError::None;
	}
	auto mapFile = FileReadDescriptor();
	auto map = EncryptedDescriptor();
	auto mapData = legacyMap;
	auto mapVersion = legacyVersion;
	if (!mapData) {
		const auto mapError = ReadImportFile(
			mapFile, u"map"_q, accountPath, kImportMapFileLimit);
		if (mapError != TdataImportError::None && mapError != TdataImportError::NotFound) {
			return mapError;
		} else if (mapError == TdataImportError::None) {
			auto salt = QByteArray();
			auto oldKey = QByteArray();
			auto encrypted = QByteArray();
			if (!ReadImportBytes(mapFile.stream, salt, kImportSaltLimit)
				|| !ReadImportBytes(mapFile.stream, oldKey, kImportKeyFileLimit)
				|| !ReadImportBytes(mapFile.stream, encrypted, kImportMapFileLimit)
				|| !mapFile.stream.atEnd()
				|| !DecryptLocal(map, encrypted, key)) {
				return TdataImportError::Corrupt;
			}
			mapData = &map;
			mapVersion = mapFile.version;
		}
	}
	if (mapData && !mapData->stream.atEnd()) {
		auto type = quint32();
		mapData->stream >> type;
		if (mapData->stream.status() != QDataStream::Ok) {
			return TdataImportError::Corrupt;
		}
		if (type == kImportSelfSerialized) {
			if (!ReadImportBytes(mapData->stream, result.self, kImportSelfLimit)) {
				return TdataImportError::Corrupt;
			}
			auto selfStream = QDataStream(result.self);
			const auto identity = Serialize::peekUserIdentity(mapVersion, selfStream);
			auto peerStream = QDataStream(result.self);
			auto peer = quint64();
			auto tag = quint64();
			auto version = qint32();
			peerStream >> peer >> tag;
			if (tag == kImportPeerVersionTag) {
				peerStream >> version;
			}
			const auto peerId = DeserializePeerId(peer);
			if (!identity || !peerIsUser(peerId)
				|| peerToUser(peerId).bare != result.uniqueId
				|| version < 0 || version > kImportMaxPeerVersion) {
				return TdataImportError::Corrupt;
			}
			auto username = QString();
			auto access = quint64();
			auto flags = qint32();
			auto placeholder = QString();
			auto lastseen = quint32();
			auto contact = qint32();
			auto botInfo = qint32();
			auto supportsGuestChat = qint32();
			auto about = QString();
			selfStream >> username >> access;
			if (mapVersion >= 9012) selfStream >> flags;
			if (mapVersion >= 9016) selfStream >> placeholder;
			selfStream >> lastseen >> contact >> botInfo;
			if (version > 2) selfStream >> supportsGuestChat;
			selfStream >> about;
			if (selfStream.status() != QDataStream::Ok || !selfStream.atEnd()) {
				return TdataImportError::Corrupt;
			}
			result.displayName = (identity->firstName + ' ' + identity->lastName).trimmed();
			result.phone = identity->phone;
			result.selfStreamVersion = mapVersion;
		}
	}
	auto config = EncryptedDescriptor();
	auto configFile = FileReadDescriptor();
	const auto configError = ReadImportEncrypted(
		config, configFile, u"config"_q, accountPath, kImportKeyFileLimit, key);
	if (configError != TdataImportError::NotFound) {
		if (configError != TdataImportError::None) {
			return configError;
		}
		auto serialized = QByteArray();
		if (!ReadImportBytes(config.stream, serialized, kImportKeyFileLimit)) {
			return TdataImportError::Corrupt;
		}
		auto prefix = QDataStream(serialized);
		prefix.setVersion(QDataStream::Qt_5_1);
		auto version = qint32();
		auto environment = qint32();
		prefix >> version >> environment;
		if (version > kImportConfigVersion) {
			return TdataImportError::Unsupported;
		} else if (config.stream.status() != QDataStream::Ok
			|| !config.stream.atEnd()
			|| prefix.status() != QDataStream::Ok
			|| version != kImportConfigVersion
			|| (environment != qint32(MTP::Environment::Production)
				&& environment != qint32(MTP::Environment::Test))) {
			return TdataImportError::Corrupt;
		}
		result.testMode = (environment == qint32(MTP::Environment::Test));
	}
	if (result.testMode) {
		result.uniqueId |= 0x0100'0000'0000'0000ULL;
	}
	if (result.displayName.isEmpty()) {
		result.displayName = result.phone.isEmpty()
			? u"Account "_q + QString::number(result.sourceIndex + 1)
			: result.phone;
	}
	return TdataImportError::None;
}

void WriteKeyData(
		FileWriteDescriptor &file,
		const KeyData &data,
		const QByteArray &infoEncrypted) {
	file.writeData(data.openSalt);
	file.writeData(data.openKeyEncrypted);
	file.writeData(infoEncrypted);
	if (data.legacy) {
		return;
	}
	auto trailer = QByteArray();
	QBuffer buffer(&trailer);
	const auto opened = buffer.open(QIODevice::WriteOnly);
	Assert(opened);
	QDataStream stream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kKeyDataMagic
		<< kKeyDataFormatVersion
		<< data.committed
		<< quint32(data.passcodeWraps.size());
	for (const auto &wrap : data.passcodeWraps) {
		stream
			<< wrap.kdf.kind
			<< wrap.generation
			<< wrap.kdf.memory
			<< wrap.kdf.time
			<< wrap.kdf.parallel
			<< wrap.salt
			<< wrap.keyEncrypted;
	}
	buffer.close();
	file.writeData(trailer);
}

} // namespace

TdataImportResult ReadTdataAccounts(
		const QString &sourceDirectory,
		const QByteArray &passcode) {
	const auto directory = QFileInfo(sourceDirectory);
	auto folder = directory.canonicalFilePath();
	const auto fail = [&](TdataImportError error) {
		return TdataImportResult{ .error = error, .sourceFolder = folder };
	};
	if (!directory.isDir() || folder.isEmpty()) {
		return fail(TdataImportError::NotFound);
	} else if (passcode.size() > kImportPasscodeLimit) {
		return fail(TdataImportError::Unsupported);
	}
	auto basePath = folder + '/';
	auto keyFile = FileReadDescriptor();
	auto mapFile = FileReadDescriptor();
	auto legacy = false;
	const auto readHeader = [&] {
		const auto error = ReadImportFile(
			keyFile, u"key_data"_q, basePath, kImportKeyFileLimit);
		legacy = (error == TdataImportError::NotFound);
		return legacy
			? ReadImportFile(
				mapFile,
				u"map"_q,
				basePath + ImportAccountName(0) + '/',
				kImportMapFileLimit)
			: error;
	};
	auto headerError = readHeader();
	if (headerError == TdataImportError::NotFound) {
		const auto nested = QFileInfo(basePath + u"tdata"_q);
		if (nested.isSymLink()) {
			return fail(TdataImportError::Corrupt);
		} else if (nested.isDir()) {
			const auto canonical = nested.canonicalFilePath();
			if (!canonical.isEmpty()) {
				folder = canonical;
				basePath = folder + '/';
				headerError = readHeader();
			}
		}
	}
	if (headerError != TdataImportError::None) {
		return fail(headerError);
	}
	auto localKey = MTP::AuthKeyPtr();
	auto indexes = std::vector<int>();
	auto legacyMap = EncryptedDescriptor();
	auto legacyVersion = 0;
	if (legacy) {
		auto salt = QByteArray();
		auto encryptedKey = QByteArray();
		auto encryptedMap = QByteArray();
		if (!ReadImportBytes(mapFile.stream, salt, kImportSaltLimit)
			|| !ReadImportBytes(mapFile.stream, encryptedKey, kImportKeyFileLimit)
			|| !ReadImportBytes(mapFile.stream, encryptedMap, kImportMapFileLimit)
			|| !mapFile.stream.atEnd()
			|| salt.size() != LocalEncryptSaltSize
			|| encryptedKey.size() != kImportWrappedKeySize) {
			return fail(TdataImportError::Corrupt);
		}
		localKey = UnwrapLocalKey(encryptedKey, CreateLegacyLocalKey(passcode, salt));
		if (!localKey) {
			return fail(passcode.isEmpty()
				? TdataImportError::PasscodeNeeded
				: TdataImportError::WrongPasscode);
		} else if (!DecryptLocal(legacyMap, encryptedMap, localKey)) {
			return fail(TdataImportError::Corrupt);
		}
		legacyVersion = mapFile.version;
		indexes.push_back(0);
	} else {
		auto data = KeyData();
		auto encryptedInfo = QByteArray();
		const auto parsed = ReadKeyData(keyFile.stream, data, encryptedInfo, true);
		if (parsed != ReadKeyDataResult::Success) {
			return fail(parsed == ReadKeyDataResult::UnsupportedKdf
				? TdataImportError::Unsupported
				: TdataImportError::Corrupt);
		}
		if (data.legacy) {
			if (data.openKeyEncrypted.size() != kImportWrappedKeySize) {
				return fail(TdataImportError::Corrupt);
			}
			localKey = UnwrapLocalKey(data.openKeyEncrypted, CreateLocalKey(passcode, data.openSalt));
		} else if (passcode.isEmpty() && !data.openKeyEncrypted.isEmpty()) {
			if (data.openKeyEncrypted.size() != kImportWrappedKeySize) {
				return fail(TdataImportError::Corrupt);
			}
			localKey = UnwrapLocalKey(data.openKeyEncrypted, CreateLocalKey({}, data.openSalt));
		} else {
			const auto live = ranges::count_if(data.passcodeWraps, [&](const auto &wrap) {
				return wrap.generation == data.committed;
			});
			if (passcode.isEmpty() && live == 1) {
				return fail(TdataImportError::PasscodeNeeded);
			} else if (live != 1) {
				return fail(TdataImportError::Corrupt);
			}
			const auto i = ranges::find(data.passcodeWraps, data.committed, &PasscodeWrap::generation);
			const auto &wrap = *i;
			if (wrap.keyEncrypted.size() != kImportWrappedKeySize || wrap.salt.size() > kImportSaltLimit) {
				return fail(TdataImportError::Corrupt);
			} else if (wrap.kdf.kind == kPasscodeKdfScrypt
				&& (wrap.kdf.memory > kImportMaxScryptN
					|| wrap.kdf.time > kImportMaxScryptR
					|| wrap.kdf.parallel > kImportMaxScryptP)) {
				return fail(TdataImportError::Unsupported);
			}
			auto wrapKey = (wrap.kdf.kind == 0)
				? CreateLocalKey(passcode, wrap.salt)
				: CreatePasscodeKey(passcode, wrap.salt, wrap.kdf);
			if (!wrapKey) {
				return fail(TdataImportError::Unsupported);
			}
			localKey = UnwrapLocalKey(wrap.keyEncrypted, wrapKey);
		}
		if (!localKey) {
			return fail(passcode.isEmpty()
				? TdataImportError::PasscodeNeeded
				: TdataImportError::WrongPasscode);
		}
		auto info = EncryptedDescriptor();
		if (!DecryptLocal(info, encryptedInfo, localKey)) {
			return fail(TdataImportError::Corrupt);
		}
		auto count = qint32();
		info.stream >> count;
		if (info.stream.status() != QDataStream::Ok
			|| count <= 0
			|| count > Main::Domain::kLocalMaxAccounts) {
			return fail(TdataImportError::Corrupt);
		}
		auto seen = base::flat_set<int>();
		for (auto i = 0; i != count; ++i) {
			auto index = qint32();
			info.stream >> index;
			if (info.stream.status() != QDataStream::Ok
				|| index < 0
				|| index >= Main::Domain::kLocalMaxAccounts
				|| !seen.emplace(index).second) {
				return fail(TdataImportError::Corrupt);
			}
			indexes.push_back(index);
		}
		if (!info.stream.atEnd()) {
			auto selected = qint32();
			info.stream >> selected;
			if (!seen.contains(selected)) {
				return fail(TdataImportError::Corrupt);
			}
		}
		if (info.stream.status() != QDataStream::Ok || !info.stream.atEnd()) {
			return fail(TdataImportError::Corrupt);
		}
	}
	auto result = TdataImportResult{ .sourceFolder = folder };
	result.accounts.reserve(indexes.size());
	auto total = qsizetype();
	for (const auto index : indexes) {
		auto account = ImportedAccount{ .sourceIndex = index };
		const auto error = ReadImportAccount(
			account,
			basePath,
			localKey,
			legacy ? &legacyMap : nullptr,
			legacyVersion);
		if (error == TdataImportError::NotFound && !legacy) {
			continue;
		} else if (error != TdataImportError::None) {
			return fail(error);
		} else if (account.uniqueId) {
			total += account.authorization.size() + account.self.size();
			if (total > kImportAggregateLimit) {
				return fail(TdataImportError::Unsupported);
			}
			result.accounts.push_back(std::move(account));
		}
	}
	return result.accounts.empty() ? fail(TdataImportError::NotFound) : std::move(result);
}

PasscodeVerification::PasscodeVerification(quint64 nonce)
: _nonce(nonce) {
}

PasscodeDerivation::PasscodeDerivation(
	std::unique_ptr<PasscodeWrap> wrap,
	const QByteArray &passcode)
: _wrap(std::move(wrap))
, _passcode(passcode.constData(), passcode.size()) {
}

PasscodeDerivation::PasscodeDerivation(PasscodeDerivation &&other) noexcept
: _wrap(base::take(other._wrap))
, _passcode(base::take(other._passcode))
, _key(base::take(other._key))
, _cleansed(base::take(other._cleansed)) {
}

PasscodeDerivation &PasscodeDerivation::operator=(
		PasscodeDerivation &&other) noexcept {
	cleanse();
	_wrap = base::take(other._wrap);
	_passcode = base::take(other._passcode);
	_key = base::take(other._key);
	_cleansed = base::take(other._cleansed);
	return *this;
}

PasscodeDerivation::~PasscodeDerivation() {
	cleanse();
}

void PasscodeDerivation::run() {
	if (!_wrap || _passcode.isEmpty()) {
		return;
	} else if (_wrap->kdf.kind == 0) {
		_key = CreateLocalKey(_passcode, _wrap->salt);
		return;
	}
	_key = CreatePasscodeKey(_passcode, _wrap->salt, _wrap->kdf);
	cleanse();
}

bool PasscodeDerivation::empty() const {
	return _passcode.isEmpty();
}

MTP::AuthKeyPtr PasscodeDerivation::keyFor(const PasscodeWrap &wrap) {
	const auto matched = _wrap && SameWrapKey(*_wrap, wrap);
	if (matched && _key) {
		return _key;
	} else if (_cleansed) {
		if (!matched) {
			LOG(("App Error: the passcode wrap changed under the derivation."));
		}
		return nullptr;
	} else if (wrap.kdf.kind == 0) {
		return CreateLocalKey(_passcode, wrap.salt);
	} else if (_passcode.isEmpty()) {
		return nullptr;
	}
	return CreatePasscodeKey(_passcode, wrap.salt, wrap.kdf);
}

void PasscodeDerivation::cleanse() {
	if (!_passcode.isEmpty()) {
		OPENSSL_cleanse(_passcode.data(), _passcode.size());
	}
	_cleansed = true;
}

Domain::Domain(not_null<Main::Domain*> owner, const QString &dataName)
: _owner(owner)
, _dataName(dataName)
, _keyData(std::make_unique<KeyData>()) {
}

Domain::~Domain() = default;

StartResult Domain::start(PasscodeDerivation derived) {
	const auto modern = startModern(derived);
	if (modern == StartModernResult::Success) {
		if (_keyData->legacy || _keyDataDirty || _oldVersion < AppVersion) {
			writeAccounts();
		}
		return StartResult::Success;
	} else if (modern == StartModernResult::IncorrectPasscode) {
		return StartResult::IncorrectPasscode;
	} else if (modern == StartModernResult::Failed) {
		return StartResult::ProfileReadError;
	}
	auto legacy = std::make_unique<Main::Account>(_owner, _dataName, 0);
	const auto result = legacy->legacyStart(derived._passcode);
	if (result == StartResult::Success) {
		_oldVersion = legacy->local().oldMapVersion();
		startWithSingleAccount(derived._passcode, std::move(legacy));
	}
	return result;
}

void Domain::startAdded(
		not_null<Main::Account*> account,
		std::unique_ptr<MTP::Config> config) {
	Expects(_localKey != nullptr);

	account->prepareToStartAdded(_localKey);
	account->start(std::move(config));
}

void Domain::startWithSingleAccount(
		const QByteArray &passcode,
		std::unique_ptr<Main::Account> account) {
	Expects(account != nullptr);

	if (auto localKey = account->local().peekLegacyLocalKey()) {
		_localKey = std::move(localKey);
		installLegacyWrap(*_keyData, passcode);
		migrateFromLegacy(passcode);
		account->start(nullptr);
	} else {
		generateLocalKey();
		account->start(account->prepareToStart(_localKey));
	}
	_owner->accountAddedInStorage(Main::Domain::AccountWithIndex{
		.account = std::move(account)
	});
	writeAccounts();
}

void Domain::generateLocalKey() {
	Expects(_localKey == nullptr);
	Expects(_keyData->openKeyEncrypted.isEmpty());
	Expects(_keyData->passcodeWraps.empty());

	auto pass = QByteArray(MTP::AuthKey::kSize, Qt::Uninitialized);
	base::RandomFill(pass.data(), pass.size());
	const auto salt = RandomSalt();
	_localKey = CreateLocalKey(pass, salt);

	installOpenWrap(*_keyData);
}

void Domain::installOpenWrap(KeyData &data) const {
	Expects(_localKey != nullptr);

	data.openSalt = RandomSalt();
	const auto wrapKey = CreateLocalKey(QByteArray(), data.openSalt);
	data.openKeyEncrypted = WrapLocalKey(_localKey, wrapKey);
	data.openKeyVerified = WrapOpensLocalKey(
		data.openKeyEncrypted,
		wrapKey,
		_localKey);
}

// Turning the app lock on drops the open wrap, recording its absence with
// an empty openKeyEncrypted - never with an empty openSalt. A build of the
// same AppVersion that predates this format reads only the three legacy
// blobs: a real-size salt beside an empty wrap makes it fail inside
// DecryptLocal and ask for a passcode, while an empty salt would make it fail
// the salt-size check, start from scratch and orphan every account store. So
// the salt stays 32 real random bytes in both states.
void Domain::dropOpenWrap(KeyData &data) const {
	data.openSalt = RandomSalt();
	data.openKeyEncrypted = QByteArray();
	data.openKeyVerified = false;
}

void Domain::installLegacyWrap(
		KeyData &data,
		const QByteArray &passcode) const {
	Expects(_localKey != nullptr);

	data.openSalt = RandomSalt();
	const auto wrapKey = CreateLocalKey(passcode, data.openSalt);
	data.openKeyEncrypted = WrapLocalKey(_localKey, wrapKey);
	data.legacy = true;
	data.legacyPasscode = !passcode.isEmpty();
	data.openKeyVerified = passcode.isEmpty()
		&& WrapOpensLocalKey(data.openKeyEncrypted, wrapKey, _localKey);
}

MTP::AuthKeyPtr Domain::installPasscodeWrap(
		KeyData &data,
		PasscodeDerivation &derived,
		quint32 generation) const {
	Expects(_localKey != nullptr);
	Expects(!derived.empty());
	Expects(derived._wrap && derived._wrap->keyEncrypted.isEmpty());

	auto wrap = *derived._wrap;
	wrap.generation = generation;
	const auto wrapKey = derived.keyFor(wrap);
	if (!wrapKey) {
		LOG(("App Error: could not derive the passcode key, family: %1"
			).arg(wrap.kdf.kind));
		return nullptr;
	}
	wrap.keyEncrypted = WrapLocalKey(_localKey, wrapKey);
	if (!WrapOpensLocalKey(wrap.keyEncrypted, wrapKey, _localKey)) {
		LOG(("App Error: a fresh passcode wrap does not open the local key."));
		return nullptr;
	}
	data.passcodeWraps.push_back(std::move(wrap));
	return wrapKey;
}

// Migration keeps "a passcode means the app lock is on", so hasLocalPasscode(),
// the auto-lock and the lock icon read exactly as they did before it ran. A
// derivation failure is not allowed to lose either the data or the app lock:
// the key data is then left in the legacy shape it was read in, which writes
// back byte for byte what the pre-change build wrote, and the next start
// simply tries the migration again. In that state openKeyEncrypted still holds
// the passcode-derived legacy blob, which legacyPasscode records, so the
// predicates keep reporting a passcode with the app lock armed and every
// mutator refuses instead of relabelling that blob as an open wrap.
void Domain::migrateFromLegacy(const QByteArray &passcode) {
	Expects(_keyData->legacy);
	Expects(_localKey != nullptr);

	if (passcode.isEmpty()) {
		installOpenWrap(*_keyData);
		_keyData->passcodeWraps.clear();
		_keyData->committed = 0;
	} else {
		auto fresh = prepareNewWrap(passcode);
		if (!installPasscodeWrap(*_keyData, fresh, 1)) {
			return;
		}
		dropOpenWrap(*_keyData);
		_keyData->committed = 1;
	}
	_keyData->legacy = false;
	_keyData->legacyPasscode = false;
	_keyDataDirty = true;
	_verificationNonce = 0;
}

Domain::StartModernResult Domain::startModern(
		PasscodeDerivation &derived) {
	const auto name = ComputeKeyName(_dataName);

	FileReadDescriptor file;
	if (!ReadFile(file, name, BaseGlobalPath(), false)) {
		auto base = BaseGlobalPath() + name;
		return KeyAlreadyUsed(base)
			? StartModernResult::Failed
			: StartModernResult::Empty;
	}
	LOG(("App Info: reading accounts info..."));

	auto parsed = KeyData();
	auto infoEncrypted = QByteArray();
	const auto read = ReadKeyData(file.stream, parsed, infoEncrypted);
	if (read == ReadKeyDataResult::Failed) {
		return StartModernResult::Failed;
	} else if (read == ReadKeyDataResult::UnsupportedKdf) {
		return StartModernResult::IncorrectPasscode;
	}
	*_keyData = std::move(parsed);

	// Probe the stored open wrap independently of the typed passcode result.
	// PrepareEncrypted adds a 16-byte prefix to the padded length and key.
	// Only this ciphertext size can hold the fixed-size local key. Bound
	// this extra decrypt before it allocates, keeping the selected unwrap
	// and startup results intact. Compare once that unwrap gives a key.
	constexpr auto kOpenWrapSize = 16
		+ ((sizeof(uint32) + MTP::AuthKey::kSize + 15) / 16) * 16;
	auto openKey = (_keyData->openKeyEncrypted.size() == kOpenWrapSize)
		? UnwrapLocalKey(
			_keyData->openKeyEncrypted,
			CreateLocalKey(QByteArray(), _keyData->openSalt))
		: nullptr;

	auto wrapKey = MTP::AuthKeyPtr();
	auto localKeyEncrypted = QByteArray();
	if (_keyData->legacy) {
		const auto blob = PasscodeWrap{
			.salt = _keyData->openSalt,
			.keyEncrypted = _keyData->openKeyEncrypted,
		};
		wrapKey = derived.keyFor(blob);
		if (!wrapKey) {
			return StartModernResult::IncorrectPasscode;
		}
		localKeyEncrypted = blob.keyEncrypted;
	} else {
		// A passcode wrap is live only while its generation equals the
		// committed one. That drops a staged wrap left above committed by a
		// crash in the middle of a passcode change and a stale wrap left
		// below it, and it is also what turns an interrupted creation of a
		// first passcode back into the "no passcode" file it was before.
		auto live = std::vector<PasscodeWrap>();
		for (auto &wrap : _keyData->passcodeWraps) {
			if (wrap.generation == _keyData->committed) {
				live.push_back(std::move(wrap));
			}
		}
		if (live.size() != _keyData->passcodeWraps.size()) {
			_keyDataDirty = true;
		}
		_keyData->passcodeWraps = std::move(live);

		if (derived.empty()) {
			if (_keyData->openKeyEncrypted.isEmpty()) {
				LOG(("App Info: the app lock is on, a passcode is needed."));
				return StartModernResult::IncorrectPasscode;
			}
			wrapKey = CreateLocalKey(QByteArray(), _keyData->openSalt);
			localKeyEncrypted = _keyData->openKeyEncrypted;
		} else if (_keyData->passcodeWraps.size() != 1) {
			LOG(("App Info: no passcode wrap to open the local key with."));
			return StartModernResult::IncorrectPasscode;
		} else {
			const auto &wrap = _keyData->passcodeWraps.front();
			wrapKey = derived.keyFor(wrap);
			if (!wrapKey) {
				LOG(("App Error: passcode KDF family %1 is unavailable."
					).arg(wrap.kdf.kind));
				return StartModernResult::IncorrectPasscode;
			}
			localKeyEncrypted = wrap.keyEncrypted;
		}
	}

	EncryptedDescriptor keyInnerData, info;
	if (!DecryptLocal(keyInnerData, localKeyEncrypted, wrapKey)) {
		LOG(("App Info: could not decrypt pass-protected key from info file, "
			"maybe bad password..."));
		return StartModernResult::IncorrectPasscode;
	}
	// An authentic envelope that does not hold a local key cannot be told
	// from an overwritten open field by the empty start alone, so while a
	// live passcode wrap survives, asking for it beats resetting what that
	// passcode still opens, and the file stays untouched for the typed
	// retry. Nothing is published yet here - the candidate key and its
	// verified-open claim are settled below - so returning is all this
	// refusal has to do.
	auto key = Serialize::read<MTP::AuthKey::Data>(keyInnerData.stream);
	if (keyInnerData.stream.status() != QDataStream::Ok
		|| !keyInnerData.stream.atEnd()) {
		if (derived.empty() && _keyData->passcodeWraps.size() == 1) {
			LOG(("App Info: the open wrap does not hold a local key, "
				"a passcode is needed."));
			return StartModernResult::IncorrectPasscode;
		}
		LOG(("App Error: could not read pass-protected key from info file"));
		return StartModernResult::Failed;
	}
	_localKey = std::make_shared<MTP::AuthKey>(key);
	_keyData->openKeyVerified = openKey && openKey->equals(_localKey);
	openKey = nullptr;

	if (_keyData->legacy) {
		_keyData->legacyPasscode = !derived.empty();
		migrateFromLegacy(derived._passcode);
	}

	// The empty start cannot tell an open wrap that holds another key from a
	// broken accounts info: only the passcode can, and while a live passcode
	// wrap survives asking for it is the honest answer. The candidate key and
	// its verified-open claim were already published above, so a refusal here
	// withdraws both, exactly as the earlier refusals leave them, and the file
	// stays untouched for the typed retry. With no passcode wrap there is no
	// secret left to ask for, so the scratch recovery keeps that case.
	if (!DecryptLocal(info, infoEncrypted, _localKey)) {
		if (derived.empty() && _keyData->passcodeWraps.size() == 1) {
			LOG(("App Info: the open wrap does not open the accounts info, "
				"a passcode is needed."));
			_localKey = nullptr;
			_keyData->openKeyVerified = false;
			return StartModernResult::IncorrectPasscode;
		}
		LOG(("App Error: could not decrypt info."));
		return StartModernResult::Failed;
	}
	LOG(("App Info: reading encrypted info..."));
	auto count = qint32();
	info.stream >> count;
	if (count <= 0 || count > Main::Domain::kLocalMaxAccounts) {
		LOG(("App Error: bad accounts count: %1").arg(count));
		return StartModernResult::Failed;
	}

	_oldVersion = file.version;

	auto indexes = std::vector<int>();
	indexes.reserve(count);
	auto tried = base::flat_set<int>();
	for (auto i = 0; i != count; ++i) {
		auto index = qint32();
		info.stream >> index;
		if (!CheckStreamStatus(info.stream)
			|| index < 0
			|| index >= Main::Domain::kLocalMaxAccounts
			|| !tried.emplace(index).second) {
			return StartModernResult::Failed;
		}
		indexes.push_back(index);
	}
	auto active = indexes.front();
	if (info.stream.device()->bytesAvailable() >= qint64(sizeof(qint32))) {
		info.stream >> active;
	}
	if (!CheckStreamStatus(info.stream)) {
		return StartModernResult::Failed;
	}
	if (!ranges::contains(indexes, active)) {
		active = indexes.front();
	}
	auto summaries = std::vector<Main::StoredAccount>();
	summaries.reserve(indexes.size());
	for (const auto index : indexes) {
		auto account = Main::Account(_owner, _dataName, index);
		summaries.push_back(account.local().readStoredAccount(_localKey, index));
	}
	const auto authenticated = ranges::find_if(summaries, [](const Main::StoredAccount &account) {
		return account.uniqueId != 0;
	});
	if (authenticated != end(summaries)) {
		const auto selected = ranges::find(summaries, active, &Main::StoredAccount::index);
		if (selected == end(summaries) || !selected->uniqueId) {
			active = authenticated->index;
		}
		summaries.erase(std::remove_if(begin(summaries), end(summaries), [](const Main::StoredAccount &account) {
			return account.authorizationKnown && !account.uniqueId;
		}), end(summaries));
	}
	for (auto &summary : summaries) {
		_owner->storedAccountAdded(std::move(summary));
	}
	auto account = std::make_unique<Main::Account>(_owner, _dataName, active);
	account->start(account->prepareToStart(_localKey));
	_owner->accountAddedInStorage({
		.index = active,
		.account = std::move(account),
	});

	_owner->activateFromStorage(active);

	return StartModernResult::Success;
}

void Domain::writeAccounts() {
	Expects(!_owner->accounts().empty());
	Expects(!_keyData->openKeyEncrypted.isEmpty()
		|| !_keyData->passcodeWraps.empty());
	Expects(_keyData->passcodeWraps.size() <= 1);

	writeKeyData(*_keyData, false);
	_keyDataDirty = false;
}

void Domain::startStored(not_null<Main::Account*> account) {
	Expects(_localKey != nullptr);
	account->start(account->prepareToStart(_localKey));
}

bool Domain::writeAccountsChecked() {
	return writeKeyData(*_keyData, true);
}

QByteArray Domain::prepareAccountsInfo() const {
	Expects(_localKey != nullptr);

	const auto list = _owner->storedAccounts();
	auto keySize = sizeof(qint32) + sizeof(qint32) * list.size();

	EncryptedDescriptor info(keySize);
	info.stream << qint32(list.size());
	for (const auto &account : list) {
		info.stream << qint32(account.index);
	}
	info.stream << qint32(_owner->activeForStorage());
	return PrepareEncrypted(info, _localKey);
}

bool Domain::writeKeyData(const KeyData &data, bool sync) const {
	const auto path = BaseGlobalPath();
	if (!QDir().exists(path)) {
		QDir().mkpath(path);
	}

	FileWriteDescriptor key(ComputeKeyName(_dataName), path, sync);
	WriteKeyData(key, data, prepareAccountsInfo());
	return key.finish();
}

// Unlike writeAccounts(), which queues the bytes and always reports success,
// this one commits them on the storage thread and answers whether they really
// reached the disk. Every path that changes which secret opens the local key
// goes through it, because a removal or a rewrap that only looked like it was
// written would leave the user with a file none of the secrets they know can
// open. details::Sync() is deliberately not called afterwards: the sync
// descriptor already drops any queued entry for the same base and performs the
// write before finish() returns.
bool Domain::writeKeyDataChecked(const KeyData &data) const {
	Expects(!_owner->accounts().empty());
	Expects(!data.legacy);
	Expects(!data.openKeyEncrypted.isEmpty() || !data.passcodeWraps.empty());
	Expects(data.passcodeWraps.size() <= kMaxWrapCount);

	const auto written = writeKeyData(data, true);
	if (!written) {
		LOG(("App Error: could not write the accounts key file."));
	}
	return written;
}

// The staged wrap was already proved to open the local key in memory, so what
// is left to prove is that the bytes the write path put on disk are those same
// bytes. Comparing the parsed wrap field by field against the staged one and
// then unwrapping with the key the derivation already produced keeps the whole
// read-back round trip - a serialization that dropped, reordered or truncated
// a field still fails here - while paying for the memory-hard derivation once
// instead of twice: byte-equal salt and parameters can only re-derive the same
// key, so re-deriving it would prove nothing the comparison has not.
bool Domain::wrapOnDiskOpensLocalKey(
		const PasscodeWrap &staged,
		const MTP::AuthKeyPtr &wrapKey) const {
	Expects(_localKey != nullptr);
	Expects(wrapKey != nullptr);

	FileReadDescriptor file;
	if (!ReadFile(file, ComputeKeyName(_dataName), BaseGlobalPath())) {
		return false;
	}
	auto parsed = KeyData();
	auto infoEncrypted = QByteArray();
	const auto read = ReadKeyData(file.stream, parsed, infoEncrypted);
	if (read != ReadKeyDataResult::Success || parsed.legacy) {
		return false;
	}
	for (const auto &wrap : parsed.passcodeWraps) {
		if (wrap.generation != staged.generation) {
			continue;
		} else if (!SameWrapKey(wrap, staged)
			|| wrap.keyEncrypted != staged.keyEncrypted) {
			LOG(("App Error: the staged passcode wrap changed on disk."));
			return false;
		}
		return WrapOpensLocalKey(wrap.keyEncrypted, wrapKey, _localKey);
	}
	return false;
}

WalletEngineValue Domain::readWalletKeyring() const {
	using State = WalletEngineValue::State;
	const auto name = ComputeWalletKeyringName(_dataName);
	const auto path = BaseGlobalPath();
	const auto base = path + name;
	if (!QFileInfo::exists(base + 's')
		&& !QFileInfo::exists(base + '0')
		&& !QFileInfo::exists(base + '1')) {
		return { .state = State::Absent };
	} else if (!_localKey) {
		return { .state = State::Broken };
	}
	FileReadDescriptor file;
	if (!ReadEncryptedFile(file, name, path, _localKey)) {
		return { .state = State::Broken };
	}
	auto bytes = QByteArray();
	file.stream >> bytes;
	if (!CheckStreamStatus(file.stream) || !file.stream.atEnd()) {
		return { .state = State::Broken };
	}
	return { .state = State::Read, .bytes = std::move(bytes) };
}

bool Domain::writeWalletKeyring(const QByteArray &bytes) const {
	Expects(_localKey != nullptr);

	const auto path = BaseGlobalPath();
	if (!QDir().exists(path) && !QDir().mkpath(path)) {
		return false;
	}
	EncryptedDescriptor data(Serialize::bytearraySize(bytes));
	data.stream << bytes;
	FileWriteDescriptor file(ComputeWalletKeyringName(_dataName), path, true);
	file.writeEncrypted(data, _localKey);
	const auto written = file.finish();
	if (!written) {
		LOG(("Wallet Error: could not write the device keyring."));
	}
	return written;
}

bool Domain::removeWalletKeyring() {
	const auto base = BaseGlobalPath() + ComputeWalletKeyringName(_dataName);
	auto result = true;
	for (const auto suffix : { 's', '0', '1' }) {
		const auto path = base + suffix;
		if (QFileInfo::exists(path) && !QFile::remove(path)) {
			result = false;
		}
	}
	return result;
}

void Domain::startFromScratch() {
	if (!removeWalletKeyring()) {
		LOG(("Wallet Error: could not remove the discarded device keyring."));
	}
	Tgw::EraseDomainStorage(_owner, _dataName);
	*_keyData = KeyData();
	_keyDataDirty = false;
	_verificationNonce = 0;
	_localKey = nullptr;
	startWithSingleAccount(
		QByteArray(),
		std::make_unique<Main::Account>(_owner, _dataName, 0));
}

std::unique_ptr<PasscodeWrap> Domain::wrapToOpen() const {
	if (_keyData->legacy || _keyData->legacyPasscode) {
		return std::make_unique<PasscodeWrap>(PasscodeWrap{
			.salt = _keyData->openSalt,
			.keyEncrypted = _keyData->openKeyEncrypted,
		});
	} else if (_keyData->passcodeWraps.size() != 1) {
		return nullptr;
	}
	return std::make_unique<PasscodeWrap>(_keyData->passcodeWraps.front());
}

PasscodeDerivation Domain::prepareOpen(const QByteArray &passcode) const {
	return PasscodeDerivation(wrapToOpen(), passcode);
}

PasscodeDerivation Domain::prepareNewWrap(const QByteArray &passcode) const {
	return PasscodeDerivation(
		std::make_unique<PasscodeWrap>(PasscodeWrap{
			.kdf = DefaultPasscodeKdf(),
			.salt = RandomSalt(),
		}),
		passcode);
}

// The only decisions this makes before the derivation runs are which wrap the
// passcode has to open and whether such a wrap exists at all; neither depends
// on what was typed and so neither tells an attacker anything about it. From
// there a wrong passcode and the right one walk the same derivation and the
// same unwrap attempt, and the verdict is the comparison of the recovered key
// with the one already in memory - the typed passcode is never needed to open
// the file at start, only to prove it is the one the wrap was made from.
//
// The retained legacy state carries no passcode wrap: the only secret that
// opens the local key there is the passcode-derived blob the pre-change build
// left in openKeyEncrypted, so it is checked exactly the way startModern()
// opens it. That state reports a passcode with the app lock armed, and this
// leg is what lets the lock it arms be opened by the passcode that armed it.
bool Domain::checkPasscode(PasscodeDerivation derived) const {
	Expects(_localKey != nullptr);

	const auto wrap = wrapToOpen();
	if (!wrap) {
		return false;
	}
	const auto key = derived.keyFor(*wrap);
	return key && WrapOpensLocalKey(wrap->keyEncrypted, key, _localKey);
}

std::optional<PasscodeVerification> Domain::verifyPasscode(
		const QByteArray &passcode) {
	return verifyPasscode(prepareOpen(passcode));
}

std::optional<PasscodeVerification> Domain::verifyPasscode(
		PasscodeDerivation derived) {
	if (!checkPasscode(std::move(derived))) {
		return std::nullopt;
	}
	auto nonce = quint64();
	while (!nonce) {
		nonce = base::RandomValue<quint64>();
	}
	_verificationNonce = nonce;
	return PasscodeVerification(nonce);
}

bool Domain::accepts(PasscodeVerification verification) const {
	return _verificationNonce && (verification._nonce == _verificationNonce);
}

SetPasscodeResult Domain::setPasscode(
		const QByteArray &passcode,
		PasscodeVerification verification) {
	if (passcode.isEmpty()) {
		return changePasscode(nullptr, verification);
	}
	auto derived = prepareNewWrap(passcode);
	return changePasscode(&derived, verification);
}

SetPasscodeResult Domain::setPasscode(
		PasscodeDerivation derived,
		PasscodeVerification verification) {
	Expects(!derived.empty());

	return changePasscode(&derived, verification);
}

// A passcode change is staged so that no crash window can leave a file only a
// passcode nobody has typed can open. The new wrap is written beside the old
// one first, with the committed generation left where it was, so a start that
// happens in that window drops the staged wrap by generation and finds exactly
// the previous truth; the bytes are then read back from disk and proved to
// open the local key, and only after that does a second write strip the old
// wrap and move committed forward. No write on this path ever adds a fresh
// open wrap, because a file carrying both the old passcode wrap and a fresh
// open wrap would keep the old passcode working while silently turning the app
// lock off - which no reconciliation rule at the next start can tell from an
// intended state. A removal is the mirror of the same rule and needs no
// staging: nothing new has to be proved, so it is a single atomic checked
// write of the open wrap with no passcode wrap left.
//
// This is the passcode-role mutator and it does not move the app-lock role.
// The open wrap is dropped only where setPasscode() creates a first passcode,
// the one case that has to keep the Settings behaviour of "setting a passcode
// locks the app"; createPasscodeWithoutAppLock() runs the same staged
// sequence for a first passcode in the wallet-only role and keeps the
// verified open wrap it already has, and a passcode change carries whatever
// open-wrap state the file already had straight through. A file still in the
// legacy shape is refused outright, because its openKeyEncrypted may be the
// passcode-derived blob and nothing here could honestly relabel that as an
// open wrap.
SetPasscodeResult Domain::changePasscode(
		PasscodeDerivation *derived,
		PasscodeVerification verification) {
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	if (_keyData->legacy) {
		LOG(("App Error: refusing a passcode change before the migration."));
		return SetPasscodeResult::Failed;
	} else if (!_keyData->passcodeWraps.empty() && !accepts(verification)) {
		return SetPasscodeResult::NeedsVerification;
	} else if (derived) {
		return installPasscode(*derived, !_keyData->passcodeWraps.empty());
	}
	auto updated = *_keyData;
	updated.passcodeWraps.clear();
	updated.committed = _keyData->committed + 1;
	installOpenWrap(updated);

	Assert(!updated.openKeyEncrypted.isEmpty());
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

SetPasscodeResult Domain::installPasscode(
		PasscodeDerivation &derived,
		bool keepOpenWrap) {
	const auto generation = _keyData->committed + 1;
	auto staged = *_keyData;
	const auto wrapKey = installPasscodeWrap(staged, derived, generation);
	if (!wrapKey) {
		return SetPasscodeResult::Failed;
	} else if (!writeKeyDataChecked(staged)) {
		return SetPasscodeResult::Failed;
	} else if (!wrapOnDiskOpensLocalKey(
			staged.passcodeWraps.back(),
			wrapKey)) {
		if (!writeKeyDataChecked(*_keyData)) {
			LOG(("App Error: could not drop the staged passcode wrap."));
		}
		return SetPasscodeResult::Failed;
	}
	auto updated = *_keyData;
	updated.passcodeWraps.clear();
	updated.passcodeWraps.push_back(staged.passcodeWraps.back());
	updated.committed = generation;
	if (!keepOpenWrap) {
		dropOpenWrap(updated);
	}

	Assert(!updated.passcodeWraps.empty());
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

SetPasscodeResult Domain::createPasscodeWithoutAppLock(
		PasscodeDerivation derived) {
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	if (_keyData->legacy || _keyData->legacyPasscode) {
		LOG(("App Error: refusing a passcode without the app lock "
			"before the migration."));
		return SetPasscodeResult::Failed;
	} else if (!_keyData->passcodeWraps.empty()) {
		return SetPasscodeResult::NeedsVerification;
	} else if (_keyData->openKeyEncrypted.isEmpty()
		|| !_keyData->openKeyVerified) {
		LOG(("App Error: refusing a passcode without the app lock "
			"over an unverified open wrap."));
		return SetPasscodeResult::Failed;
	}
	return installPasscode(derived, true);
}

// Turning the lock off installs an open wrap over a file a passcode still
// protects - the same weakening of the data at rest a removal is - so it
// carries the same proof a removal does. Turning it on proves nothing new
// and only needs a passcode wrap to exist. The gsl::finally still spends the
// nonce on every exit, whichever way the lock moves, so a token that
// authorized one change cannot authorize another.
SetPasscodeResult Domain::setAppLockEnabled(
		bool enabled,
		PasscodeVerification verification) {
	Expects(_localKey != nullptr);

	const auto singleUse = gsl::finally([&] { _verificationNonce = 0; });

	const auto hasWrap = !_keyData->passcodeWraps.empty();
	if (_keyData->legacy) {
		LOG(("App Error: refusing an app lock change before the migration."));
		return SetPasscodeResult::Failed;
	} else if ((enabled && !hasWrap)
		|| (!enabled && hasWrap && !accepts(verification))) {
		return SetPasscodeResult::NeedsVerification;
	}
	auto updated = *_keyData;
	if (enabled) {
		dropOpenWrap(updated);
	} else {
		installOpenWrap(updated);
	}
	if (!writeKeyDataChecked(updated)) {
		return SetPasscodeResult::Failed;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;

	_verificationNonce = 0;
	_passcodeKeyChanged.fire({});
	return SetPasscodeResult::Success;
}

// Main::Domain::removePasscodeIfEmpty() calls this after Local::reset() has
// destroyed every store the local key protected. Nothing remains for the old
// passcode to guard or to ask for it - the account it belonged to is gone.
// Wallet's confirmed forgotten-passcode reset calls it after the shared
// keyring and all signed-in custody records are gone. DropUnusedPasscode()
// calls it when the verified app lock is off and no live keyring entry uses
// the Passcode policy, at startup, logout and terminal wallet operations.
// Each caller establishes that the passcode guards nothing left to ask
// for, which is why this removal carries no verification and has its own
// name instead of being reachable through setPasscode(). The write stays
// checked so failure does not claim the passcode was removed.
void Domain::clearPasscodeAfterReset() {
	Expects(_localKey != nullptr);

	auto updated = *_keyData;
	updated.legacy = false;
	updated.legacyPasscode = false;
	updated.passcodeWraps.clear();
	updated.committed = _keyData->committed + 1;
	installOpenWrap(updated);

	Assert(!updated.openKeyEncrypted.isEmpty());
	if (!writeKeyDataChecked(updated)) {
		return;
	}
	*_keyData = std::move(updated);
	_keyDataDirty = false;
	_verificationNonce = 0;

	_passcodeKeyChanged.fire({});
}

int Domain::oldVersion() const {
	return _oldVersion;
}

void Domain::clearOldVersion() {
	_oldVersion = 0;
}

rpl::producer<> Domain::localPasscodeChanged() const {
	return _passcodeKeyChanged.events();
}

bool Domain::hasPasscode() const {
	return !_keyData->passcodeWraps.empty() || _keyData->legacyPasscode;
}

bool Domain::appLockEnabled() const {
	return _keyData->legacyPasscode || !_keyData->openKeyVerified;
}

bool Domain::hasLocalPasscode() const {
	// This historical API means verified app locking, not a wallet passcode.
	return appLockEnabled();
}

} // namespace Storage
