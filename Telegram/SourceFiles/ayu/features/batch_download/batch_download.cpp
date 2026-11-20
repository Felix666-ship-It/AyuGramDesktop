// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/batch_download/batch_download.h"

#include "base/base_file_utilities.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

namespace AyuFeatures::BatchDownload {
namespace {

constexpr auto kMaxConcurrentDownloads = 3;

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

[[nodiscard]] QString DocumentName(
		DocumentData *document,
		FullMsgId id) {
	auto name = base::FileNameFromUserString(document->filename());
	const auto extension = DocumentExtension(document);
	if (name.isEmpty()) {
		name = u"video_"_q + QString::number(id.msg.bare);
	}
	if (QFileInfo(name).suffix().isEmpty()) {
		name += extension;
	}
	return name;
}

[[nodiscard]] QString PhotoBaseName(FullMsgId id) {
	return u"photo_"_q + QString::number(id.msg.bare);
}

[[nodiscard]] QString UniquePath(
		const QString &folder,
		const QString &name,
		QSet<QString> &reserved) {
	const auto info = QFileInfo(name);
	auto base = info.completeBaseName();
	auto extension = info.suffix();
	if (base.isEmpty()) {
		base = u"file"_q;
	}
	if (!extension.isEmpty()) {
		extension = u"."_q + extension;
	}
	const auto prefix = FolderWithSlash(folder);
	auto candidate = prefix + base + extension;
	for (auto i = 1;
		QFileInfo::exists(candidate)
			|| reserved.contains(candidate.toCaseFolded());
		++i) {
		candidate = prefix + base + u" (%1)"_q.arg(i) + extension;
	}
	reserved.insert(candidate.toCaseFolded());
	return candidate;
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

class Task final : public std::enable_shared_from_this<Task> {
public:
	Task(
		not_null<Main::Session*> session,
		MessageIdsList ids,
		QString folder,
		base::weak_ptr<Window::SessionController> controller,
		Fn<void()> onFinished)
	: _session(session)
	, _controller(std::move(controller))
	, _folder(QDir(folder).absolutePath())
	, _ids(std::move(ids))
	, _onFinished(std::move(onFinished)) {
	}

	~Task() {
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
		}
	}

	void start() {
		if (_finished) {
			return;
		}
		if (!QDir().mkpath(_folder)) {
			_failed = int(_ids.size());
			_index = int(_ids.size());
			finish();
			return;
		}

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

		if (!_ids.empty()) {
			showStartToast();
		}
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
		QString path;
		DocumentData *document = nullptr;
		PhotoData *photo = nullptr;
		std::shared_ptr<Data::PhotoMedia> photoMedia;
		int photoAttempts = 0;
		uint64 generation = 0;
	};

	void advance() {
		while (!_finished
			&& int(_currents.size()) < kMaxConcurrentDownloads) {
			if (_index >= int(_ids.size())) {
				break;
			}
			const auto id = _ids[_index++];
			const auto item = _session->data().message(id);
			if (!item) {
				++_skipped;
				continue;
			}
			const auto media = item->media();
			if (!media) {
				++_skipped;
				continue;
			}
			if (const auto photo = media->photo()) {
				if (photo->isNull()) {
					++_skipped;
					continue;
				}
				auto current = std::make_shared<Current>();
				current->kind = Kind::Photo;
				current->id = id;
				current->photo = photo;
				current->generation = ++_generation;
				_currents.push_back(current);
				preparePhoto(current);
			} else if (const auto document = media->document()) {
				if (!IsDownloadableDocument(document)) {
					++_skipped;
					continue;
				}
				auto current = std::make_shared<Current>();
				current->kind = Kind::Document;
				current->id = id;
				current->document = document;
				current->generation = ++_generation;
				current->path = UniquePath(
					_folder,
					DocumentName(document, id),
					_reserved);
				_currents.push_back(current);
				prepareDocument(current);
			} else {
				++_skipped;
			}
		}
		if (!_finished && _currents.empty() && _index >= int(_ids.size())) {
			finish();
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
		if (document->loading()) {
			current->state = State::WaitingDownload;
		} else {
			finishDocument(current);
		}
	}

	void checkCurrent(const std::shared_ptr<Current> &current) {
		if (!current->active) {
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
		current->path = UniquePath(
			_folder,
			PhotoBaseName(current->id) + extension,
			_reserved);
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

	void finishCurrent(
			const std::shared_ptr<Current> &current,
			bool saved) {
		if (!current->active) {
			return;
		}
		current->active = false;
		if (saved) {
			++_saved;
		} else {
			++_failed;
		}
		const auto i = std::find(_currents.begin(), _currents.end(), current);
		if (i != _currents.end()) {
			_currents.erase(i);
		}
	}

	void finish() {
		if (_finished || !_currents.empty()) {
			return;
		}
		_finished = true;
		_lifetime.destroy();
		showResultToast();
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		_onFinished();
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

	void showStartToast() {
		showToast(tr::ayu_BatchDownloadStarted(tr::now));
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
	const MessageIdsList _ids;
	const Fn<void()> _onFinished;
	rpl::lifetime _lifetime;
	QSet<QString> _reserved;
	std::vector<std::shared_ptr<Current>> _currents;
	int _index = 0;
	int _saved = 0;
	int _failed = 0;
	int _skipped = 0;
	uint64 _generation = 0;
	bool _finished = false;
};

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

class Manager::Impl {
public:
	explicit Impl(not_null<Main::Session*> session)
	: _session(session) {
	}

	void enqueue(
		MessageIdsList ids,
		QString folder,
		base::weak_ptr<Window::SessionController> controller,
		Fn<void()> onStarted) {
		auto task = std::make_shared<Task>(
			_session,
			std::move(ids),
			std::move(folder),
			std::move(controller),
			[this] { taskFinished(); });
		_queue.push_back(std::move(task));
		if (onStarted) {
			onStarted();
		}
		pump();
	}

private:
	void pump() {
		if (_active || _queue.empty()) {
			return;
		}
		_active = std::move(_queue.front());
		_queue.pop_front();
		_active->start();
	}

	void taskFinished() {
		_active = nullptr;
		pump();
	}

	const not_null<Main::Session*> _session;
	std::deque<std::shared_ptr<Task>> _queue;
	std::shared_ptr<Task> _active;

};

Manager::Manager(not_null<Main::Session*> session)
: _impl(std::make_unique<Impl>(session)) {
}

Manager::~Manager() = default;

void Manager::chooseFolderAndStart(
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
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
				std::move(onStarted));
		});
}

} // namespace AyuFeatures::BatchDownload
