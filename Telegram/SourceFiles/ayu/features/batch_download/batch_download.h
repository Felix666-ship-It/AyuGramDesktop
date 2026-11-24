// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "data/data_types.h"

#include <memory>

class DocumentData;
class QWidget;

namespace Data {
class Media;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace AyuFeatures::BatchDownload {

enum class Mode {
	Direct,
	Group,
	MergeByCaption,
};

[[nodiscard]] bool IsDownloadableMedia(Data::Media *media);
[[nodiscard]] bool IsDownloadableDocument(DocumentData *document);

class Manager final : public base::has_weak_ptr {
public:
	explicit Manager(not_null<Main::Session*> session);
	~Manager();

	void chooseFolderAndStart(
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Mode mode,
		Fn<void()> onStarted = nullptr);

	[[nodiscard]] base::unique_qptr<Ui::PopupMenu> MakeModeMenu(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Fn<void()> onStarted = nullptr);

private:
	class Impl;
	std::unique_ptr<Impl> _impl;

};

} // namespace AyuFeatures::BatchDownload
