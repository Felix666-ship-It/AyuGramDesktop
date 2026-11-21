// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_infra.h"

#include "ayu/ayu_lang.h"
#include "ayu/ayu_settings.h"
#include "ayu/ayu_ui_settings.h"
#include "ayu/ayu_worker.h"
#include "ayu/data/ayu_database.h"
#include "ayu/ui/ayu_logo.h"
#include "features/translator/ayu_translator.h"
#include "lang/lang_instance.h"
#include "ui/chat/chat_style_radius.h"
#include "utils/rc_manager.h"
#include "window/themes/window_theme.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryFile>

#ifdef Q_OS_WIN
#include "ayu/utils/windows_utils.h"
#endif

namespace AyuInfra {

void initLang() {
	QString id = Lang::GetInstance().id();
	QString baseId = Lang::GetInstance().baseId();
	if (id.isEmpty()) {
		LOG(("Language is not loaded"));
		return;
	}
	AyuLanguage::init();
	AyuLanguage::currentInstance()->fetchLanguage(id, baseId);
}

void initUiSettings() {
	const auto &settings = AyuSettings::getInstance();

	AyuUiSettings::setMonoFont(settings.monoFont());
	AyuUiSettings::setWideMultiplier(settings.wideMultiplier());
	AyuUiSettings::setMaterialSwitches(settings.materialSwitches());
	AyuUiSettings::setAvatarCorners(settings.avatarCorners());
	Ui::SetAppliedBubbleRadius(settings.messageBubbleRadius());
}

void initDatabase() {
	AyuDatabase::initialize();
}

void initWorker() {
	AyuWorker::initialize();
}

void initRCManager() {
	RCManager::getInstance().start();
}

void initTranslator() {
	Ayu::Translator::TranslateManager::init();
}

void initIcon() {
#ifdef Q_OS_WIN
	AyuAssets::loadAppIco();
	reloadAppIconFromTaskBar();
#endif
}

void init() {
	initLang();
	initDatabase();
	initUiSettings();
	initIcon();
	initWorker();
	initRCManager();
	initTranslator();
}

void applyDefaultThemeIfNeeded() {
	auto &settings = AyuSettings::getInstance();
	if (settings.defaultWeChatThemeApplied()) {
		return;
	}
	const auto &theme = Window::Theme::Background()->themeObject();
	if (!theme.pathAbsolute.isEmpty() || theme.cloud.id) {
		settings.setDefaultWeChatThemeApplied(true);
		return;
	}

	auto file = QFile(u":/gui/themes/wechat_v81.tdesktop-theme"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("Default theme: embedded resource not found."));
		return;
	}
	const auto content = file.readAll();
	file.close();

	auto temporary = QTemporaryFile(
		QDir::tempPath() + u"/ayu_wechat_XXXXXX.tdesktop-theme"_q);
	if (!temporary.open()
		|| temporary.write(content) != content.size()) {
		LOG(("Default theme: could not write temporary theme."));
		return;
	}
	temporary.flush();
	temporary.close();

	if (Window::Theme::Apply(temporary.fileName())) {
		Window::Theme::KeepApplied();
		settings.setDefaultWeChatThemeApplied(true);
		LOG(("Default theme: WeChat 8.1 applied."));
	} else {
		LOG(("Default theme: failed to parse embedded theme."));
	}
}

}
