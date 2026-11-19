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

#include <deque>
#include <memory>

namespace AyuFeatures::BatchDownload {
namespace {

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
		if (_current.active
			&& _current.kind == Kind::Document
			&& _current.state == State::WaitingDownload
			&& _current.document) {
			_current.document->cancel();
		}
		if (_current.active && !_current.path.isEmpty()) {
			QFile::remove(_current.path);
		}
	}

	void start() {
		if (_finished) {
			return;
		}
		if (!QDir().mkpath(_folder)) {
			_failed = int(_ids.size());
			finish();
			return;
		}

		_session->data().documentLoadProgress(
		) | rpl::on_next([this](not_null<DocumentData*> document) {
			if (_inStart) {
				return;
			}
			if (_current.active
				&& _current.kind == Kind::Document
				&& _current.document == document.get()) {
				checkCurrent();
				if (!_current.active) {
					advance();
				}
			}
		}, _lifetime);

		_session->data().photoLoadProgress(
		) | rpl::on_next([this](not_null<PhotoData*> photo) {
			if (_inStart) {
				return;
			}
			if (_current.active
				&& _current.kind == Kind::Photo
				&& _current.photo == photo.get()) {
				checkCurrent();
				if (!_current.active) {
					advance();
				}
			}
		}, _lifetime);

		_session->downloaderTaskFinished(
		) | rpl::on_next([this] {
			if (_inStart) {
				return;
			}
			if (_current.active) {
				checkCurrent();
				if (!_current.active) {
					advance();
				}
			}
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
		bool active = false;
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
		while (!_finished && !_current.active) {
			if (_index >= int(_ids.size())) {
				finish();
				return;
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
				preparePhoto(id, photo);
			} else if (const auto document = media->document()) {
				if (!IsDownloadableDocument(document)) {
					++_skipped;
					continue;
				}
				prepareDocument(id, document);
			} else {
				++_skipped;
			}
		}
	}

	void prepareDocument(FullMsgId id, DocumentData *document) {
		_current = Current();
		_current.active = true;
		_current.kind = Kind::Document;
		_current.id = id;
		_current.document = document;
		_current.generation = ++_generation;
		_current.path = UniquePath(
			_folder,
			DocumentName(document, id),
			_reserved);
		if (document->loading()) {
			_current.state = State::WaitingExistingLoad;
			return;
		}
		startDocumentSave();
	}

	void startDocumentSave() {
		const auto document = _current.document;
		if (!document || _current.path.isEmpty()) {
			failCurrent();
			return;
		}
		const auto localPath = document->filepath(true);
		if (!localPath.isEmpty()
			&& localPath != _current.path
			&& QFileInfo::exists(localPath)
			&& (document->size <= 0
				|| QFileInfo(localPath).size() == document->size)
			&& QFile::copy(localPath, _current.path)) {
			finishCurrent(true);
			return;
		}
		const auto generation = _current.generation;
		_inStart = true;
		document->save(
			Data::FileOrigin(_current.id),
			_current.path,
			LoadFromCloudOrLocal,
			false,
			false);
		_inStart = false;
		if (_finished
			|| !_current.active
			|| _current.generation != generation) {
			return;
		}
		if (document->loading()) {
			_current.state = State::WaitingDownload;
		} else {
			finishDocument();
		}
	}

	void checkCurrent() {
		if (!_current.active) {
			return;
		}
		if (_current.kind == Kind::Document) {
			checkDocument();
		} else {
			checkPhoto();
		}
	}

	void checkDocument() {
		const auto document = _current.document;
		if (!document) {
			failCurrent();
			return;
		}
		if (_current.state == State::WaitingExistingLoad) {
			if (document->loading()) {
				return;
			}
			startDocumentSave();
			return;
		}
		if (document->loading()) {
			return;
		}
		if (document->cancelled()) {
			failCurrent();
			return;
		}
		finishDocument();
	}

	void finishDocument() {
		if (TargetMatches(_current.path, _current.document)) {
			finishCurrent(true);
		} else {
			failCurrent();
		}
	}

	void preparePhoto(FullMsgId id, PhotoData *photo) {
		_current = Current();
		_current.active = true;
		_current.kind = Kind::Photo;
		_current.id = id;
		_current.photo = photo;
		_current.photoMedia = photo->activeMediaView()
			? photo->activeMediaView()
			: photo->createMediaView();
		_current.generation = ++_generation;

		const auto media = _current.photoMedia;
		const auto generation = _current.generation;
		if (media->loaded()) {
			savePhoto();
			return;
		}
		if (photo->failed(Data::PhotoSize::Large)) {
			if (_current.photoAttempts > 0) {
				failCurrent();
				return;
			}
			_current.photoAttempts++;
			photo->clearFailed(Data::PhotoSize::Large);
		}
		_inStart = true;
		media->wanted(Data::PhotoSize::Large, Data::FileOrigin(id));
		_inStart = false;
		if (_finished
			|| !_current.active
			|| _current.generation != generation) {
			return;
		}
		if (media->loaded()) {
			savePhoto();
			return;
		}
		if (photo->loading(Data::PhotoSize::Large)) {
			_current.state = State::WaitingDownload;
			return;
		}
		failCurrent();
	}

	void checkPhoto() {
		const auto media = _current.photoMedia;
		const auto photo = _current.photo;
		if (!media || !photo) {
			failCurrent();
			return;
		}
		if (media->loaded()) {
			savePhoto();
			return;
		}
		if (photo->failed(Data::PhotoSize::Large) || photo->cancelled()) {
			failCurrent();
			return;
		}
		if (!photo->loading(Data::PhotoSize::Large)) {
			failCurrent();
		}
	}

	void savePhoto() {
		const auto media = _current.photoMedia;
		if (!media) {
			failCurrent();
			return;
		}
		const auto extension = media->videoContent(Data::PhotoSize::Large).isEmpty()
			? u".jpg"_q
			: u".mp4"_q;
		_current.path = UniquePath(
			_folder,
			PhotoBaseName(_current.id) + extension,
			_reserved);
		if (media->saveToFile(_current.path)) {
			finishCurrent(true);
		} else {
			failCurrent();
		}
	}

	void failCurrent() {
		if (!_current.path.isEmpty()) {
			QFile::remove(_current.path);
		}
		finishCurrent(false);
	}

	void finishCurrent(bool saved) {
		if (!_current.active) {
			return;
		}
		if (saved) {
			++_saved;
		} else {
			++_failed;
		}
		_current = Current();
	}

	void finish() {
		if (_finished) {
			return;
		}
		_finished = true;
		_lifetime.destroy();
		showResultToast();
		[[maybe_unused]] const auto keepAlive = shared_from_this();
		_onFinished();
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
				QString::number(_saved)));
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
	Current _current;
	int _index = 0;
	int _saved = 0;
	int _failed = 0;
	int _skipped = 0;
	uint64 _generation = 0;
	bool _inStart = false;
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
