// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/batch_download/batch_download.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "data/data_download_manager.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace AyuFeatures::BatchDownload {
namespace {

constexpr auto kWatchdogInterval = crl::time(5000);
constexpr auto kStallTimeout = crl::time(25000);
constexpr auto kMaxRetries = 3;

[[nodiscard]] QString FolderWithSlash(const QString &folder) {
	return folder.endsWith('/') ? folder : (folder + '/');
}

[[nodiscard]] QString DocumentExtension(DocumentData *document) {
	const auto nameSuffix = QFileInfo(document->filename()).suffix();
	if (!nameSuffix.isEmpty()) {
		return u"."_q + nameSuffix;
	}
	const auto patterns = Core::MimeTypeForName(
		document->mimeString()).globPatterns();
	for (const auto &pattern : patterns) {
		const auto index = pattern.lastIndexOf('.');
		if (index >= 0 && (index + 1 < pattern.size())) {
			return pattern.mid(index);
		}
	}
	return u".mp4"_q;
}

[[nodiscard]] bool TargetMatches(
		const QString &path,
		DocumentData *document) {
	const auto info = QFileInfo(path);
	if (!info.exists() || info.size() <= 0) {
		return false;
	}
	return !document
		|| document->size <= 0
		|| info.size() == document->size;
}

[[nodiscard]] QString FirstLine(QString text) {
	text = text.trimmed();
	text.replace('\r', ' ');
	const auto index = text.indexOf('\n');
	return (index < 0 ? text : text.left(index)).trimmed();
}

[[nodiscard]] QString CleanFolderName(QString value) {
	value.replace(QRegularExpression(u"[\\\\/:*?\"<>|]"_q), QString());
	for (auto i = 0; i != value.size(); ++i) {
		if (value[i].unicode() < 0x20) {
			value[i] = QChar(' ');
		}
	}
	value = value.trimmed();
	while (value.endsWith('.') || value.endsWith(' ')) {
		value.chop(1);
	}
	if (value.size() > 80) {
		value.truncate(80);
	}
	while (value.endsWith('.') || value.endsWith(' ')) {
		value.chop(1);
	}
	return value;
}

[[nodiscard]] QString GroupKey(HistoryItem *item) {
	const auto fullId = item->fullId();
	const auto groupId = item->groupId();
	if (!groupId.empty()) {
		return u"g:%1:%2"_q.arg(
			fullId.peer.value).arg(groupId.raw());
	}
	return u"m:%1:%2"_q.arg(
		fullId.peer.value).arg(fullId.msg.bare);
}

[[nodiscard]] QString UniqueFolderName(
		const QString &parent,
		QString base,
		QSet<QString> &reserved) {
	base = CleanFolderName(std::move(base));
	if (base.isEmpty()) {
		base = u"\u6d88\u606f"_q;
	}
	const auto folderWithSlash = FolderWithSlash(parent);
	const auto isBusy = [&](const QString &name) {
		const auto path = folderWithSlash + name;
		return reserved.contains(path.toCaseFolded())
			|| QFileInfo::exists(path);
	};
	auto candidate = base;
	if (isBusy(candidate)) {
		candidate = base + u" - \u526f\u672c"_q;
		for (auto i = 2; isBusy(candidate); ++i) {
			candidate = base + u" - \u526f\u672c (%1)"_q.arg(i);
		}
	}
	reserved.insert((folderWithSlash + candidate).toCaseFolded());
	return candidate;
}

struct DownloadedState {
	QSet<QString> keys;
	bool loaded = false;
};

[[nodiscard]] DownloadedState &DownloadedMarks() {
	static DownloadedState result;
	return result;
}

[[nodiscard]] QString DownloadedFilePath() {
	return cWorkingDir() + u"tdata/ayu_batch_downloaded.txt"_q;
}

void LoadDownloadedMarks() {
	auto &state = DownloadedMarks();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	auto file = QFile(DownloadedFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto lines = QString::fromUtf8(file.readAll()).split(QChar('\n'));
	for (auto line : lines) {
		line = line.trimmed();
		if (!line.isEmpty()) {
			state.keys.insert(line);
		}
	}
}

[[nodiscard]] QString DownloadedKey(not_null<const HistoryItem*> item) {
	const auto history = item->history();
	const auto userId = history->session().userId().bare;
	const auto peerId = history->peer->id.value;
	const auto groupId = item->groupId();
	if (!groupId.empty()) {
		return u"g:%1:%2:%3"_q
			.arg(userId)
			.arg(peerId)
			.arg(groupId.raw());
	}
	return u"m:%1:%2:%3"_q
		.arg(userId)
		.arg(peerId)
		.arg(item->fullId().msg.bare);
}

void RefreshDownloadedMark(not_null<const HistoryItem*> item) {
	const auto owner = &item->history()->owner();
	if (const auto group = owner->groups().find(item)) {
		for (const auto &other : group->items) {
			owner->requestItemViewRefresh(other);
			owner->requestItemResize(other);
		}
		return;
	}
	owner->requestItemViewRefresh(item);
	owner->requestItemResize(item);
}

class Task;

struct SharedState {
	QSet<QString> reservedSequences;
	QHash<QString, int> folderCounters;
	QSet<QString> reservedFolders;
	std::vector<std::weak_ptr<Task>> tasks;
	rpl::lifetime lifetime;
	int activeDownloads = 0;
	bool waking = false;
	bool wakeAgain = false;
	bool shuttingDown = false;

	SharedState();

	[[nodiscard]] bool tryAcquire() {
		const auto limit = std::clamp(
			AyuSettings::getInstance().batchDownloadConcurrency(),
			1,
			64);
		if (activeDownloads >= limit) {
			return false;
		}
		++activeDownloads;
		return true;
	}

	void release();
	void registerTask(const std::shared_ptr<Task> &task);
	void wakeTasks();
};

SharedState::SharedState() {
	AyuSettings::getInstance().batchDownloadConcurrencyChanges(
	) | rpl::on_next([this](int) {
		wakeTasks();
	}, lifetime);
}

class Task final : public std::enable_shared_from_this<Task> {
public:
	Task(
		not_null<Main::Session*> session,
		MessageIdsList ids,
		QString folder,
		base::weak_ptr<Window::SessionController> controller,
		Mode mode,
		std::shared_ptr<SharedState> shared,
		Fn<void(Task*)> onFinished)
	: _session(session)
	, _controller(std::move(controller))
	, _folder(QDir(folder).absolutePath())
	, _ids(std::move(ids))
	, _mode(mode)
	, _shared(std::move(shared))
	, _onFinished(std::move(onFinished)) {
		std::sort(_ids.begin(), _ids.end(), [](const FullMsgId &a, const FullMsgId &b) {
			return a.msg.bare < b.msg.bare;
		});
	}

	~Task() {
		_watchdog.cancel();
		_lifetime.destroy();
		for (const auto &current : _currents) {
			if (current->active
				&& current->kind == Kind::Document
				&& current->state == State::WaitingDownload
				&& current->document) {
				current->document->cancel();
			}
			if (current->active && !current->path.isEmpty()) {
				QFile::remove(current->path);
			}
			releaseSlot(current);
		}
	}

	void start() {
		if (_finished) {
			return;
		}
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		const auto count = countDownloadable();
		if (!count) {
			finish();
			return;
		}
		if (!QDir().mkpath(_folder)) {
			_failed = count;
			_index = int(_ids.size());
			finish();
			return;
		}
		prepareGroups();

		_session->data().documentLoadProgress(
		) | rpl::on_next([this](not_null<DocumentData*> document) {
			const auto i = findDocument(document.get());
			if (i < 0 || _currents[i]->starting) {
				return;
			}
			checkCurrent(_currents[i]);
			advance();
		}, _lifetime);

		_session->data().photoLoadProgress(
		) | rpl::on_next([this](not_null<PhotoData*> photo) {
			const auto i = findPhoto(photo.get());
			if (i < 0 || _currents[i]->starting) {
				return;
			}
			checkCurrent(_currents[i]);
			advance();
		}, _lifetime);

		_session->downloaderTaskFinished(
		) | rpl::on_next([this] {
			const auto currents = _currents;
			for (const auto &current : currents) {
				if (current->active) {
					checkCurrent(current);
				}
			}
			advance();
		}, _lifetime);

		showStartToast(count);
		_watchdog.setCallback([this] {
			checkStalls();
		});
		_watchdog.callEach(kWatchdogInterval);
		advance();
	}

	void wake() {
		advance();
	}

private:
	enum class Kind {
		Document,
		Photo,
	};
	enum class State {
		None,
		WaitingExistingLoad,
		WaitingDownload,
	};
	struct Current {
		bool active = true;
		bool starting = false;
		Kind kind = Kind::Document;
		State state = State::None;
		FullMsgId id;
		QString folder;
		QString sequence;
		QString path;
		DocumentData *document = nullptr;
		PhotoData *photo = nullptr;
		std::shared_ptr<Data::PhotoMedia> photoMedia;
		int photoAttempts = 0;
		uint64 generation = 0;
		int64 lastProgress = 0;
		crl::time lastProgressAt = 0;
		int retryCount = 0;
		bool retrying = false;
		bool slotHeld = true;
	};

	[[nodiscard]] int countDownloadable() const {
		auto result = 0;
		for (const auto &id : _ids) {
			const auto item = _session->data().message(id);
			if (item && IsDownloadableMedia(item->media())) {
				++result;
			}
		}
		return result;
	}

	void prepareDirectFolder() {
		auto caption = QString();
		auto firstId = FullMsgId();
		auto hasItem = false;
		auto hasMedia = false;
		for (const auto &id : _ids) {
			const auto item = _session->data().message(id);
			if (!item) {
				continue;
			}
			if (!hasItem) {
				hasItem = true;
				firstId = id;
			}
			if (caption.isEmpty()) {
				caption = FirstLine(item->originalText().text);
			}
			if (!hasMedia && IsDownloadableMedia(item->media())) {
				hasMedia = true;
			}
		}
		if (!hasMedia) {
			return;
		}
		auto base = CleanFolderName(caption);
		if (base.isEmpty()) {
			base = u"\u6d88\u606f_"_q
				+ QString::number(firstId.msg.bare);
		}
		const auto name = UniqueFolderName(
			_folder,
			base,
			_shared->reservedFolders);
		_directFolder = FolderWithSlash(_folder) + name;
		QDir().mkpath(_directFolder);
	}

	void prepareGroups() {
		if (_mode == Mode::Direct) {
			prepareDirectFolder();
			return;
		}
		for (const auto &id : _ids) {
			const auto item = _session->data().message(id);
			if (!item || !IsDownloadableMedia(item->media())) {
				continue;
			}
			const auto key = GroupKey(item);
			if (!_groupCaptions.contains(key)) {
				_groupOrder.push_back(key);
				_groupFallback.insert(key, id);
				_groupCaptions.insert(key, QString());
			}
			if (_groupCaptions.value(key).isEmpty()) {
				_groupCaptions[key] = FirstLine(item->originalText().text);
			}
		}
		if (_groupOrder.empty()) {
			return;
		}

		const auto addFolder = [&](const QString &base) {
			const auto name = UniqueFolderName(
				_folder,
				base,
				_shared->reservedFolders);
			const auto path = FolderWithSlash(_folder) + name;
			QDir().mkpath(path);
			return path;
		};

		if (_mode == Mode::Group) {
			if (_groupOrder.size() == std::size_t(1)) {
				return;
			}
			for (const auto &key : _groupOrder) {
				auto base = _groupCaptions.value(key);
				if (base.isEmpty()) {
					base = u"\u6d88\u606f_"_q
						+ QString::number(_groupFallback.value(key).msg.bare);
				}
				_groupFolders.insert(key, addFolder(base));
			}
			return;
		}

		auto captionFolders = QHash<QString, QString>();
		auto currentFolder = QString();
		const auto fallbackName = u"\u672a\u547d\u540d\u5408\u96c6"_q;
		for (const auto &key : _groupOrder) {
			const auto caption = FirstLine(_groupCaptions.value(key));
			auto target = QString();
			if (!caption.isEmpty()) {
				auto base = CleanFolderName(caption);
				if (base.isEmpty()) {
					base = fallbackName;
				}
				const auto normalized = base.toCaseFolded();
				const auto i = captionFolders.constFind(normalized);
				if (i != captionFolders.cend()) {
					target = i.value();
				} else {
					target = addFolder(base);
					captionFolders.insert(normalized, target);
				}
			} else if (currentFolder.isEmpty()) {
				target = addFolder(fallbackName);
				captionFolders.insert(fallbackName.toCaseFolded(), target);
			} else {
				target = currentFolder;
			}
			_groupFolders.insert(key, target);
			currentFolder = target;
		}
	}

	[[nodiscard]] QString NextSequence(const QString &folder) {
		const auto folderKey = folder.toCaseFolded();
		auto counter = _shared->folderCounters.value(folderKey, 1);
		while (true) {
			const auto sequence = QString::number(counter).rightJustified(
				3,
				QChar('0'));
			const auto path = FolderWithSlash(folder) + sequence;
			const auto reserved = folderKey + u"|"_q + sequence;
			if (!QFileInfo::exists(path)
				&& !_shared->reservedSequences.contains(reserved)
				&& QDir(folder).entryList(
					QStringList{ sequence + u".*"_q },
					QDir::Files).isEmpty()) {
				_shared->reservedSequences.insert(reserved);
				_shared->folderCounters.insert(folderKey, counter + 1);
				return sequence;
			}
			++counter;
		}
	}

	void prepareDocument(const std::shared_ptr<Current> &current) {
		const auto document = current->document;
		if (!document) {
			failCurrent(current);
			return;
		}
		if (document->loading()) {
			current->state = State::WaitingExistingLoad;
			return;
		}
		startDocumentSave(current);
	}

	void startDocumentSave(const std::shared_ptr<Current> &current) {
		const auto document = current->document;
		if (!document || current->path.isEmpty()) {
			failCurrent(current);
			return;
		}
		const auto localPath = document->filepath(true);
		if (!localPath.isEmpty()
			&& localPath != current->path
			&& QFileInfo::exists(localPath)
			&& (document->size <= 0
				|| QFileInfo(localPath).size() == document->size)
			&& QFile::copy(localPath, current->path)) {
			finishCurrent(current, true);
			return;
		}
		current->starting = true;
		document->save(
			Data::FileOrigin(current->id),
			current->path,
			LoadFromCloudOrLocal,
			false,
			false);
		current->starting = false;
		if (!current->active || _finished) {
			return;
		}
		if (document->loading()
			&& !document->loadingFilePath().isEmpty()) {
			if (const auto item = _session->data().message(current->id)) {
				Core::App().downloadManager().addLoading({
					.item = item,
					.document = document,
				});
			}
		}
		if (document->loading()) {
			current->state = State::WaitingDownload;
		} else {
			finishDocument(current);
		}
	}

	void checkCurrent(const std::shared_ptr<Current> &current) {
		if (!current->active || current->retrying) {
			return;
		}
		if (current->kind == Kind::Document) {
			checkDocument(current);
		} else {
			checkPhoto(current);
		}
	}

	void checkDocument(const std::shared_ptr<Current> &current) {
		const auto document = current->document;
		if (!document) {
			failCurrent(current);
			return;
		}
		if (current->state == State::WaitingExistingLoad) {
			if (document->loading()) {
				return;
			}
			startDocumentSave(current);
			return;
		}
		if (document->loading()) {
			return;
		}
		if (document->cancelled()) {
			failCurrent(current);
			return;
		}
		finishDocument(current);
	}

	void finishDocument(const std::shared_ptr<Current> &current) {
		if (TargetMatches(current->path, current->document)) {
			finishCurrent(current, true);
		} else {
			failCurrent(current);
		}
	}

	void preparePhoto(const std::shared_ptr<Current> &current) {
		const auto photo = current->photo;
		if (!photo) {
			failCurrent(current);
			return;
		}
		current->photoMedia = photo->activeMediaView()
			? photo->activeMediaView()
			: photo->createMediaView();
		const auto media = current->photoMedia;
		if (media->loaded()) {
			savePhoto(current);
			return;
		}
		if (photo->failed(Data::PhotoSize::Large)) {
			if (current->photoAttempts > 0) {
				failCurrent(current);
				return;
			}
			current->photoAttempts++;
			photo->clearFailed(Data::PhotoSize::Large);
		}
		current->starting = true;
		media->wanted(Data::PhotoSize::Large, Data::FileOrigin(current->id));
		current->starting = false;
		if (!current->active || _finished) {
			return;
		}
		if (media->loaded()) {
			savePhoto(current);
			return;
		}
		if (photo->loading(Data::PhotoSize::Large)) {
			current->state = State::WaitingDownload;
			return;
		}
		failCurrent(current);
	}

	void checkPhoto(const std::shared_ptr<Current> &current) {
		const auto media = current->photoMedia;
		const auto photo = current->photo;
		if (!media || !photo) {
			failCurrent(current);
			return;
		}
		if (media->loaded()) {
			savePhoto(current);
			return;
		}
		if (photo->failed(Data::PhotoSize::Large) || photo->cancelled()) {
			failCurrent(current);
			return;
		}
		if (!photo->loading(Data::PhotoSize::Large)) {
			failCurrent(current);
		}
	}

	void savePhoto(const std::shared_ptr<Current> &current) {
		const auto media = current->photoMedia;
		if (!media) {
			failCurrent(current);
			return;
		}
		const auto extension = media->videoContent(Data::PhotoSize::Large).isEmpty()
			? u".jpg"_q
			: u".mp4"_q;
		if (current->sequence.isEmpty()) {
			failCurrent(current);
			return;
		}
		current->path = FolderWithSlash(current->folder)
			+ current->sequence
			+ extension;
		if (media->saveToFile(current->path)) {
			finishCurrent(current, true);
		} else {
			failCurrent(current);
		}
	}

	void failCurrent(const std::shared_ptr<Current> &current) {
		if (!current->path.isEmpty()) {
			QFile::remove(current->path);
		}
		finishCurrent(current, false);
	}

	void cancelCurrent(const std::shared_ptr<Current> &current) {
		if (current->kind == Kind::Document && current->document) {
			current->document->cancel();
		} else if (current->kind == Kind::Photo && current->photo) {
			current->photo->cancel();
		}
	}

	void retryCurrent(const std::shared_ptr<Current> &current) {
		if (!current->active || current->retrying) {
			return;
		}
		current->retrying = true;
		if (current->retryCount >= kMaxRetries) {
			cancelCurrent(current);
			failCurrent(current);
			return;
		}

		++current->retryCount;
		++current->generation;
		cancelCurrent(current);
		if (!current->path.isEmpty()) {
			const auto info = QFileInfo(current->path);
			if (info.exists() && info.size() == 0) {
				QFile::remove(current->path);
			}
		}
		current->lastProgress = 0;
		current->lastProgressAt = crl::now();
		current->state = State::None;
		current->starting = false;
		current->photoAttempts = 0;
		current->photoMedia.reset();
		current->retrying = false;
		if (current->kind == Kind::Document) {
			prepareDocument(current);
		} else {
			preparePhoto(current);
		}
	}

	void checkStalls() {
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		if (_finished) {
			return;
		}
		const auto now = crl::now();
		const auto currents = _currents;
		for (const auto &current : currents) {
			if (!current->active || current->retrying) {
				continue;
			}
			auto progress = int64(-1);
			if (current->kind == Kind::Document
				&& current->document
				&& current->document->loading()) {
				progress = current->document->loadOffset();
			} else if (current->kind == Kind::Photo
				&& current->photo
				&& current->photo->loading(Data::PhotoSize::Large)) {
				progress = current->photo->loadOffset();
			}
			if (progress < 0) {
				continue;
			}
			if (progress > current->lastProgress) {
				current->lastProgress = progress;
				current->lastProgressAt = now;
			} else if (now - current->lastProgressAt >= kStallTimeout) {
				retryCurrent(current);
			}
		}
	}

	void releaseSlot(const std::shared_ptr<Current> &current) {
		if (current->slotHeld) {
			current->slotHeld = false;
			_shared->release();
		}
	}

	void finishCurrent(
			const std::shared_ptr<Current> &current,
			bool saved) {
		if (!current->active) {
			return;
		}
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		current->active = false;
		if (saved) {
			++_saved;
			if (const auto item = _session->data().message(current->id)) {
				MarkBatchDownloaded(item);
			}
		} else {
			++_failed;
		}
		const auto i = std::find(_currents.begin(), _currents.end(), current);
		if (i != _currents.end()) {
			_currents.erase(i);
		}
		releaseSlot(current);
	}

	void finish() {
		if (_finished || !_currents.empty()) {
			return;
		}
		_finished = true;
		_watchdog.cancel();
		_lifetime.destroy();
		showResultToast();
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		_onFinished(this);
	}

	void advance() {
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		if (_advancing || _finished) {
			return;
		}
		_advancing = true;
		while (!_finished && _index < int(_ids.size())) {
			const auto id = _ids[_index];
			const auto item = _session->data().message(id);
			if (!item) {
				++_index;
				++_skipped;
				continue;
			}
			const auto media = item->media();
			if (!media) {
				++_index;
				++_skipped;
				continue;
			}
			const auto photo = media->photo();
			const auto document = media->document();
			const auto isPhoto = photo && !photo->isNull();
			const auto isDocument = !isPhoto
				&& document
				&& IsDownloadableDocument(document);
			if (!isPhoto && !isDocument) {
				++_index;
				++_skipped;
				continue;
			}
			if (!_shared->tryAcquire()) {
				break;
			}
			++_index;
			auto folder = _folder;
			if (_mode == Mode::Direct) {
				folder = _directFolder;
			} else {
				folder = _groupFolders.value(GroupKey(item), _folder);
			}
			if (isPhoto) {
				auto current = std::make_shared<Current>();
				current->kind = Kind::Photo;
				current->id = id;
				current->folder = folder;
				current->sequence = NextSequence(folder);
				current->photo = photo;
				current->generation = ++_generation;
				current->lastProgressAt = crl::now();
				_currents.push_back(current);
				preparePhoto(current);
			} else {
				auto current = std::make_shared<Current>();
				current->kind = Kind::Document;
				current->id = id;
				current->folder = folder;
				current->sequence = NextSequence(folder);
				current->document = document;
				current->generation = ++_generation;
				current->lastProgressAt = crl::now();
				current->path = FolderWithSlash(current->folder)
					+ current->sequence
					+ DocumentExtension(document);
				_currents.push_back(current);
				prepareDocument(current);
			}
		}
		_advancing = false;
		if (!_finished && _currents.empty() && _index >= int(_ids.size())) {
			finish();
		}
	}

	int findDocument(DocumentData *document) const {
		for (auto i = 0; i != int(_currents.size()); ++i) {
			if (_currents[i]->active
				&& _currents[i]->kind == Kind::Document
				&& _currents[i]->document == document) {
				return i;
			}
		}
		return -1;
	}

	int findPhoto(PhotoData *photo) const {
		for (auto i = 0; i != int(_currents.size()); ++i) {
			if (_currents[i]->active
				&& _currents[i]->kind == Kind::Photo
				&& _currents[i]->photo == photo) {
				return i;
			}
		}
		return -1;
	}

	void showStartToast(int count) {
		showToast(u"已开始批量下载（共 %1 项）"_q.arg(count));
	}

	void showResultToast() {
		if (_failed > 0) {
			showToast(tr::ayu_BatchDownloadPartial(
				tr::now,
				lt_saved,
				QString::number(_saved),
				lt_failed,
				QString::number(_failed)));
		} else if (_saved > 0) {
			showToast(tr::ayu_BatchDownloadSaved(
				tr::now,
				lt_count,
				float64(_saved)));
		}
	}

	void showToast(const QString &text) {
		auto controller = _controller.get();
		if (!controller) {
			controller = _session->tryResolveWindow();
		}
		if (controller) {
			controller->showToast(text);
		}
	}

	const not_null<Main::Session*> _session;
	const base::weak_ptr<Window::SessionController> _controller;
	const QString _folder;
	MessageIdsList _ids;
	const Mode _mode;
	const std::shared_ptr<SharedState> _shared;
	const Fn<void(Task*)> _onFinished;
	rpl::lifetime _lifetime;
	base::Timer _watchdog;
	QString _directFolder;
	std::vector<QString> _groupOrder;
	QHash<QString, QString> _groupCaptions;
	QHash<QString, FullMsgId> _groupFallback;
	QHash<QString, QString> _groupFolders;
	std::vector<std::shared_ptr<Current>> _currents;
	int _index = 0;
	int _saved = 0;
	int _failed = 0;
	int _skipped = 0;
	uint64 _generation = 0;
	bool _advancing = false;
	bool _finished = false;
};

void SharedState::release() {
	if (activeDownloads > 0) {
		--activeDownloads;
	}
	wakeTasks();
}

void SharedState::registerTask(const std::shared_ptr<Task> &task) {
	tasks.push_back(task);
}

void SharedState::wakeTasks() {
	if (shuttingDown) {
		return;
	}
	if (waking) {
		wakeAgain = true;
		return;
	}
	waking = true;
	do {
		wakeAgain = false;
		auto pending = std::move(tasks);
		tasks.clear();
		for (const auto &weak : pending) {
			if (const auto task = weak.lock()) {
				tasks.push_back(task);
				task->wake();
			}
		}
	} while (wakeAgain && !shuttingDown);
	waking = false;
}

} // namespace

bool IsDownloadableDocument(DocumentData *document) {
	if (!document || document->sticker()) {
		return false;
	}
	return document->isVideoFile()
		|| document->isVideoMessage()
		|| document->isAnimation();
}

bool IsDownloadableMedia(Data::Media *media) {
	if (!media) {
		return false;
	} else if (const auto photo = media->photo()) {
		return !photo->isNull();
	} else if (const auto document = media->document()) {
		return IsDownloadableDocument(document);
	}
	return false;
}

void MarkBatchDownloaded(not_null<const HistoryItem*> item) {
	LoadDownloadedMarks();
	auto &state = DownloadedMarks();
	const auto key = DownloadedKey(item);
	auto changed = false;
	if (!state.keys.contains(key)) {
		state.keys.insert(key);
		changed = true;
		QDir().mkpath(cWorkingDir() + u"tdata"_q);
		auto file = QFile(DownloadedFilePath());
		if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
			file.write(key.toUtf8());
			file.write("\n", 1);
		}
	}
	if (changed) {
		RefreshDownloadedMark(item);
	}
}

bool IsBatchDownloaded(not_null<const HistoryItem*> item) {
	LoadDownloadedMarks();
	return DownloadedMarks().keys.contains(DownloadedKey(item));
}

class Manager::Impl : public base::has_weak_ptr {
public:
	explicit Impl(not_null<Main::Session*> session)
	: _session(session) {
	}

	~Impl() {
		_shared->shuttingDown = true;
	}

	void enqueue(
		MessageIdsList ids,
		QString folder,
		base::weak_ptr<Window::SessionController> controller,
		Mode mode,
		Fn<void()> onStarted) {
		const auto weak = base::make_weak(this);
		auto task = std::make_shared<Task>(
			_session,
			std::move(ids),
			std::move(folder),
			std::move(controller),
			mode,
			_shared,
			[weak](Task *finished) {
				if (const auto impl = weak.get()) {
					impl->taskFinished(finished);
				}
			});
		_shared->registerTask(task);
		_activeTasks.push_back(std::move(task));
		_activeTasks.back()->start();
		if (onStarted) {
			onStarted();
		}
	}

private:
	void taskFinished(Task *task) {
		const auto i = std::find_if(
			_activeTasks.begin(),
			_activeTasks.end(),
			[task](const std::shared_ptr<Task> &active) {
				return active.get() == task;
			});
		if (i != _activeTasks.end()) {
			_activeTasks.erase(i);
		}
	}

	const not_null<Main::Session*> _session;
	std::shared_ptr<SharedState> _shared = std::make_shared<SharedState>();
	std::vector<std::shared_ptr<Task>> _activeTasks;
};

Manager::Manager(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
}

Manager::~Manager() = default;

void Manager::chooseFolderAndStart(
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Mode mode,
		Fn<void()> onStarted) {
	const auto weakManager = base::make_weak(this);
	const auto weakController = base::make_weak(controller.get());
	FileDialog::GetFolder(
		controller->window().widget().get(),
		tr::lng_download_path_choose(tr::now),
		QString(),
		[weakManager,
			weakController,
			ids = std::move(ids),
			mode,
			onStarted = std::move(onStarted)](QString &&folder) mutable {
			if (folder.isEmpty()) {
				return;
			}
			const auto manager = weakManager.get();
			if (!manager) {
				return;
			}
			manager->_impl->enqueue(
				std::move(ids),
				std::move(folder),
				weakController,
				mode,
				std::move(onStarted));
		});
}

base::unique_qptr<Ui::PopupMenu> Manager::MakeModeMenu(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Fn<void()> onStarted) {
	auto menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	const auto weakManager = base::make_weak(this);
	const auto weakController = base::make_weak(controller.get());
	const auto add = [&](Mode mode, const QString &title) {
		auto modeIds = ids;
		menu->addAction(
			title,
			[weakManager,
			 weakController,
			 ids = std::move(modeIds),
			 mode,
			 onStarted]() mutable {
				const auto manager = weakManager.get();
				const auto controller = weakController.get();
				if (manager && controller) {
					manager->chooseFolderAndStart(
						controller,
						std::move(ids),
						mode,
						onStarted);
				}
			});
	};
	add(Mode::Direct, u"单文件夹下载（按序命名）"_q);
	add(Mode::Group, u"按分组建子文件夹（按序命名）"_q);
	add(Mode::MergeByCaption, u"按首条文本合并文件夹（按序命名）"_q);
	return menu;
}

} // namespace AyuFeatures::BatchDownload
