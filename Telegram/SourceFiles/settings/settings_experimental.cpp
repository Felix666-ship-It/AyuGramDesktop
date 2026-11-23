/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_experimental.h"

#include "settings/settings_common.h"
#include "data/components/passkeys.h"
#include "ui/layers/generic_box.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/search_field_controller.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/kinetic_scroller.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/vertical_list.h"
#include "ui/gl/gl_detection.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/controls/compose_ai_button_factory.h"
#include "base/options.h"
#include "boxes/moderate_messages_box.h"
#include "core/application.h"
#include "core/launcher.h"
#include "core/sandbox.h"
#include "chat_helpers/tabbed_panel.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_widget.h"
#include "dialogs/ui/dialogs_layout.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "history/history_item_components.h"
#include "history/view/controls/compose_controls_common.h"
#include "history/view/history_view_message.h"
#include "info/profile/info_profile_actions.h"
#include "info/profile/tabs/adapters/info_profile_tab_media.h"
#include "info/profile/tabs/info_profile_tabs_host.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "mainwidget.h"
#include "media/player/media_player_instance.h"
#include "mtproto/session_private.h"
#include "webview/webview_embed.h"
#include "window/main_window.h"
#include "window/window_filters_favorite.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "window/window_controller.h"
#include "window/notifications_manager.h"
#include "info/info_flexible_scroll.h"
#include "chat_helpers/stickers_list_widget.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_settings.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonDocument>
#include <QtGui/QGuiApplication>

// AyuGram includes
#include "ayu/ui/settings/settings_main.h"
#include "settings/settings_builder.h"


namespace Settings {
namespace {

const auto kOptionsClipboardPrefix = u"tdesktop-flags:"_q;

struct DecodeOptionsResult {
	bool ok = false;
	QString json;
};

struct ResolvedReferrer {
	QString controlId;
	Type section = AyuMain::Id();
};

[[nodiscard]] QString EncodeOptionsToText(const QString &json) {
	const auto flags = QByteArray::Base64UrlEncoding
		| QByteArray::OmitTrailingEquals;
	return kOptionsClipboardPrefix
		+ qs(qCompress(json.toLatin1(), 9).toBase64(flags));
}

[[nodiscard]] DecodeOptionsResult DecodeOptionsFromText(const QString &text) {
	auto result = DecodeOptionsResult();
	if (!text.startsWith(kOptionsClipboardPrefix)) {
		return result;
	}
	auto encoded = QStringView(text).mid(
		kOptionsClipboardPrefix.size()).toLatin1();
	const auto compressed = QByteArray::fromBase64Encoding(
		std::move(encoded),
		QByteArray::Base64UrlEncoding
			| QByteArray::AbortOnBase64DecodingErrors);
	if (!compressed || (*compressed).isEmpty()) {
		return result;
	}
	const auto decoded = qUncompress(*compressed);
	if (decoded.isEmpty()) {
		return result;
	}

	auto error = QJsonParseError();
	const auto parsed = QJsonDocument::fromJson(decoded, &error);
	if ((error.error != QJsonParseError::NoError) || !parsed.isObject()) {
		return result;
	}
	result.ok = true;
	result.json = QString::fromUtf8(decoded);
	return result;
}

[[nodiscard]] ResolvedReferrer ResolveReferrer(
		const QString &controlId,
		not_null<Main::Session*> session) {
	const auto &registry = Builder::SearchRegistry::Instance();
	const auto entries = registry.collectAll(session);
	for (const auto &entry : entries) {
		if (!entry.section) {
			continue;
		}
		if (entry.id == controlId) {
			return {
				.controlId = entry.id,
				.section = entry.section,
			};
		}
		if (entry.altIds.contains(controlId)) {
			return {
				.controlId = entry.id,
				.section = entry.section,
			};
		}
	}
	return {
		.controlId = controlId,
	};
}

[[nodiscard]] QString OptionReferrer(const base::options::option<bool> &option) {
	const auto &id = option.id();
	if (id == u"tabbed-panel-show-on-click"_q) {
		return u"ayu/showEmojiPopup"_q;
	} else if (id == u"show-peer-id-below-about"_q) {
		return u"ayu/showPeerId"_q;
	} else if (id == u"use-small-msg-bubble-radius"_q) {
		return u"ayu/messageBubbleRadius"_q;
	} else if (id == u"unlimited-recent-stickers"_q) {
		return u"ayu/unlimitedRecentStickers"_q;
	} else if (id == u"hide-ai-button"_q) {
		return u"ayu/showAiEditorButtonInMessageField"_q;
	} else if (id == u"unlimited-message-width"_q) {
		return u"ayu/wideMultiplier"_q;
	}
	return QString();
}

struct LocalizedOptionText {
	QString name;
	QString description;
	bool found = false;
};

[[nodiscard]] LocalizedOptionText LocalizedOption(const QString &id) {
	if (id == u"forum-hide-chats-list"_q) {
		return { u"在论坛中隐藏聊天列表"_q, u"不保留窄幅聊天列表栏。"_q, true };
	} else if (id == u"dialogs-unread-on-top"_q) {
		return {
			u"未静音未读聊天置顶"_q,
			u"将收到新未静音消息的聊天排在置顶聊天下方，并保持到已读。"_q,
			true,
		};
	} else if (id == u"dialogs-mute-icon"_q) {
		return {
			u"聊天列表中的静音图标"_q,
			u"在已静音聊天名称旁显示小静音图标。"_q,
			true,
		};
	} else if (id == u"use-new-chat-view"_q) {
		return { u"新版聊天视图"_q, u"通过新分区打开聊天。"_q, true };
	} else if (id == u"auto-scroll-inactive-chat"_q) {
		return {
			u"将非活动聊天标记为已读"_q,
			u"窗口未聚焦时也将新消息标记为已读并滚动聊天。"_q,
			true,
		};
	} else if (id == u"moderate-common-groups"_q) {
		return { u"一次封禁多个群组中的用户。"_q, QString(), true };
	} else if (id == u"classic-profile-scroll"_q) {
		return {
			u"使用经典个人资料滚动处理。"_q,
			u"将个人资料封面滚动恢复为之前的（基于填充的）实现。"_q,
			true,
		};
	} else if (id == u"use-small-msg-bubble-radius"_q) {
		return {
			u"使用更小的消息气泡圆角"_q,
			u"使大多数消息气泡接近方形。"_q,
			true,
		};
	} else if (id == u"unlimited-message-width"_q) {
		return {
			u"无限消息宽度"_q,
			u"允许纯文本消息气泡超过默认最大宽度。"_q,
			true,
		};
	} else if (id == u"mac-cmd-reply-immediately"_q) {
		return {
			u"Mac：Cmd + 上/下键立即回复"_q,
			u"按 Cmd + 上/下键时直接回复上一条或下一条消息，而不是先移动输入光标。按住 Shift 仍可移动光标。"_q,
			true,
		};
	} else if (id == u"hide-ai-button"_q) {
		return {
			u"隐藏 AI 按钮"_q,
			u"隐藏消息输入框中的 AI 工具按钮。"_q,
			true,
		};
	} else if (id == u"force-compose-search-one-column"_q) {
		return {
			u"强制在聊天中使用内嵌搜索"_q,
			u"在单列模式下强制使用聊天内嵌搜索。"_q,
			true,
		};
	} else if (id == u"view-profile-in-chats-list-context-menu"_q) {
		return {
			u"添加「查看个人资料」"_q,
			u"在聊天列表右键菜单中添加「查看个人资料」。"_q,
			true,
		};
	} else if (id == u"show-peer-id-below-about"_q) {
		return {
			u"在个人资料中显示 Peer ID"_q,
			u"在用户简介/描述下方显示来自 API 的 Peer ID。将联系人 ID 添加到导出数据。"_q,
			true,
		};
	} else if (id == u"show-channel-joined-below-about"_q) {
		return {
			u"在个人资料中显示频道加入日期"_q,
			u"在频道描述下方显示加入时间。"_q,
			true,
		};
	} else if (id == u"profile-media-tabs"_q) {
		return {
			u"在个人资料中以标签页显示共享媒体。"_q,
			u"将个人资料中的共享媒体按钮替换为一排标签页，内嵌显示媒体列表。功能仍在开发中。"_q,
			true,
		};
	} else if (id == u"profile-media-tabs-expanded"_q) {
		return {
			u"将个人资料媒体标签页拆分为照片和视频。"_q,
			u"在个人资料中分别显示照片和视频标签页，而不是单一媒体标签页。"_q,
			true,
		};
	} else if (id == u"tabbed-panel-show-on-click"_q) {
		return {
			u"点击显示标签面板"_q,
			u"仅在点击后显示表情/贴纸/GIF 面板。"_q,
			true,
		};
	} else if (id == u"unlimited-recent-stickers"_q) {
		return {
			u"无限最近贴纸"_q,
			u"显示服务器提供的全部最近贴纸。"_q,
			true,
		};
	} else if (id == u"disable-autoplay-next"_q) {
		return {
			u"禁用自动播放下一首"_q,
			u"禁用自动播放下一首音频文件/语音消息/视频消息。"_q,
			true,
		};
	} else if (id == u"external-media-viewer"_q) {
		return {
			u"外部媒体查看器"_q,
			u"使用系统媒体查看器而不是内置查看器。"_q,
			true,
		};
	} else if (id == u"ffmpeg-multithread"_q) {
		return {
			u"多线程视频解码"_q,
			u"允许 FFmpeg 使用线程池进行解码，通常每个 CPU 线程对应一个线程。"_q,
			true,
		};
	} else if (id == u"hide-reply-button"_q) {
		return {
			u"隐藏回复按钮"_q,
			u"在通知中隐藏回复按钮。"_q,
			true,
		};
	} else if (id == u"custom-notification"_q) {
		return {
			u"强制非原生通知可用"_q,
			u"即使当前平台的自定义通知不可用，也允许禁用原生通知。"_q,
			true,
		};
	} else if (id == u"gnotification"_q) {
		return {
			u"GNotification"_q,
			u"强制启用 GLib 的 GNotification。禁用时使用自动检测。"_q,
			true,
		};
	} else if (id == u"mac-modern-notifications"_q) {
		return {
			u"现代 macOS 通知"_q,
			u"使用 UserNotifications 框架发送原生通知（macOS 10.14+）。系统将在首次启动时请求通知权限。"_q,
			true,
		};
	} else if (id == u"fractional-scaling-enabled"_q) {
		return {
			u"启用精确 High DPI 缩放"_q,
			u"严格遵循系统界面缩放设置。"_q,
			true,
		};
	} else if (id == u"high-dpi-downscale"_q) {
		return {
			u"High DPI 降采样"_q,
			u"严格遵循系统界面缩放设置（另一种实现，质量可能更好）。"_q,
			true,
		};
	} else if (id == u"use-qt-rhi"_q) {
		return {
			u"使用 Qt RHI 渲染器"_q,
			u"在 GPU 而非 CPU 上渲染主窗口。"_q,
			true,
		};
	} else if (id == u"enable-vulkan-rhi"_q) {
		return {
			u"启用 Vulkan 渲染器"_q,
			u"可用时使用 Vulkan 进行 GPU 渲染，而不是 OpenGL。"_q,
			true,
		};
	} else if (id == u"freetype"_q) {
		return {
			u"FreeType 字体引擎"_q,
			u"使用 Linux 的字体引擎而不是系统字体引擎。"_q,
			true,
		};
	} else if (id == u"kinetic-scroller"_q) {
		return { u"触摸板自定义惯性滚动"_q, QString(), true };
	} else if (id == u"touchbar-disabled"_q) {
		return { u"禁用 Touch Bar（仅 macOS）。"_q, QString(), true };
	} else if (id == u"new-windows-size-as-first"_q) {
		return {
			u"调整新聊天窗口大小"_q,
			u"以主窗口大小打开新窗口。"_q,
			true,
		};
	} else if (id == u"prefer-ipv6"_q) {
		return {
			u"优先使用 IPv6"_q,
			u"如果可用则优先使用 IPv6。需要启用「尝试通过 IPv6 连接」。"_q,
			true,
		};
	} else if (id == u"skip-url-scheme-register"_q) {
		return {
			u"跳过 URL Scheme 注册"_q,
			u"自动更新时不要重新注册 tg:// URL Scheme。"_q,
			true,
		};
	} else if (id == u"deadlock-detector"_q) {
		return {
			u"死锁检测器"_q,
			u"每 30 秒检查一次主线程是否仍有响应。"_q,
			true,
		};
	} else if (id == u"webview-debug-enabled"_q) {
		return {
			u"启用 WebView 检查"_q,
			u"在 WebView 窗口右键选择「检查」。（macOS 上启动 Safari，从开发菜单打开）"_q,
			true,
		};
	} else if (id == u"webview-legacy-edge"_q) {
		return {
			u"强制使用旧版 Edge WebView"_q,
			u"跳过现代 CoreWebView2 检查，在 Windows 上强制使用旧版 Edge WebView。"_q,
			true,
		};
	} else if (id == u"fast-buttons-mode"_q) {
		return {
			u"快捷按钮模式"_q,
			u"使用 1-9 键盘按键触发内联键盘按钮。"_q,
			true,
		};
	} else if (id == u"folder-favorite-link"_q) {
		return {
			u"收藏文件夹按钮"_q,
			u"文件夹栏底部的收藏按钮，可打开 tg:// 或 t.me/ 链接。"_q,
			true,
		};
	}
	return {};
}

void SetupCopyDeepLink(
		not_null<Window::Controller*> window,
		not_null<Button*> button,
		const QString &id) {
	const auto link = u"tg://settings/experimental/"_q + id;
	const auto menu
		= button->lifetime().make_state<base::unique_qptr<Ui::PopupMenu>>();
	button->events(
	) | rpl::filter([](not_null<QEvent*> e) {
		return e->type() == QEvent::ContextMenu;
	}) | rpl::on_next([=](not_null<QEvent*> e) {
		*menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		(*menu)->addAction(u"复制深层链接"_q, [=] {
			TextUtilities::SetClipboardText({ link });
			window->showToast({
				.text = { u"深层链接已复制到剪贴板。"_q },
				.iconLottie = u"toast/voip_invite"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		}, &st::menuIconCopy);
		(*menu)->popup(QCursor::pos());
		e->accept();
	}, button->lifetime());
}

[[nodiscard]] not_null<Button*> AddOptionRow(
		not_null<Ui::VerticalLayout*> container,
		const QString &name,
		const QString &description,
		const style::SettingsButton &st) {
	if (description.isEmpty()) {
		return container->add(object_ptr<Button>(
			container,
			rpl::single(name),
			st));
	}
	const auto &titlePadding = st::settingsExperimentalTitlePadding;
	const auto &aboutPadding = st::settingsExperimentalAboutPadding;
	const auto button = Ui::CreateChild<Button>(
		container.get(),
		rpl::single(QString()),
		st);
	const auto title = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			name,
			st::settingsExperimentalTitle),
		titlePadding);
	const auto about = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			description,
			st::settingsExperimentalAbout),
		aboutPadding);
	title->setAttribute(Qt::WA_TransparentForMouseEvents);
	about->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		container->widthValue(),
		title->heightValue(),
		about->heightValue()
	) | rpl::on_next([=](int width, int titleHeight, int aboutHeight) {
		button->resize(width, titlePadding.top()
			+ titleHeight
			+ titlePadding.bottom()
			+ aboutPadding.top()
			+ aboutHeight
			+ aboutPadding.bottom());
	}, button->lifetime());
	title->topValue(
	) | rpl::on_next([=](int top) {
		button->moveToLeft(0, top - titlePadding.top());
	}, button->lifetime());
	button->show();
	return button;
}

QString AddOption(
		not_null<Window::Controller*> window,
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		base::options::option<bool> &option,
		rpl::producer<> resetClicks,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto localized = LocalizedOption(option.id());
	const auto name = localized.found
		? localized.name
		: (option.name().isEmpty() ? option.id() : option.name());
	const auto description = localized.found
		? localized.description
		: option.description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto &lifetime = inner->lifetime();
	const auto toggles = lifetime.make_state<rpl::event_stream<bool>>();
	std::move(
		resetClicks
	) | rpl::map_to(
		option.defaultValue()
	) | rpl::start_to_stream(*toggles, lifetime);
	std::move(reloadOptionsRequests) | rpl::on_next([=, &option] {
		toggles->fire_copy(option.value());
	}, lifetime);

	const auto referrer = OptionReferrer(option);
	const auto button = AddOptionRow(
		inner,
		name,
		description,
		(!referrer.isEmpty() || option.relevant())
			? st::settingsButtonNoIcon
			: st::settingsOptionDisabled);
	if (!referrer.isEmpty()) {
		button->addClickHandler([=] {
			const auto resolved = ResolveReferrer(
				referrer,
				&controller->session());
			controller->setHighlightControlId(resolved.controlId);
			controller->showSettings(resolved.section);
			window->activate();
		});
	} else {
		button->toggleOn(toggles->events_starting_with(option.value()));
	}

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option.id(), button);
	}

	SetupCopyDeepLink(window, button, option.id());

	const auto restarter = (referrer.isEmpty()
		&& option.relevant()
		&& option.restartRequired())
		? button->lifetime().make_state<base::Timer>()
		: nullptr;
	if (restarter) {
		restarter->setCallback([=] {
			window->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		});
	}
	if (referrer.isEmpty()) {
		button->toggledChanges(
		) | rpl::on_next([=, &option](bool toggled) {
			if (!option.relevant() && toggled != option.defaultValue()) {
				toggles->fire_copy(option.defaultValue());
				window->showToast(
					u"此选项不适用于当前系统。"_q);
				return;
			}
			option.set(toggled);
			if (restarter) {
				restarter->callOnce(st::settingsButtonNoIcon.toggle.duration);
			}
		}, inner->lifetime());
	}

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

QString AddFavoriteLinkButton(
		not_null<Window::Controller*> window,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto option = &base::options::lookup<QString>(
		Window::kOptionFolderFavoriteLink);
	const auto localized = LocalizedOption(option->id());
	const auto name = localized.found
		? localized.name
		: (option->name().isEmpty() ? option->id() : option->name());
	const auto description = localized.found
		? localized.description
		: option->description();

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)),
		style::margins(),
		style::al_justify);
	const auto inner = wrap->entity();

	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		option->changes()
	) | rpl::map([option] {
		return option->value();
	});
	const auto button = AddButtonWithLabel(
		inner,
		rpl::single(name),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		window->show(Box(Window::EditFolderFavoriteLinkBox));
	});

	if (registerHighlight) {
		registerHighlight(u"experimental/"_q + option->id(), button);
	}

	SetupCopyDeepLink(window, button, option->id());

	const auto searchable = name + ' ' + description;
	const auto terms = SearchWords(searchable);
	std::move(
		query
	) | rpl::on_next([=](const QString &text) {
		wrap->toggle(
			MatchesWords(terms, SearchWords(text)),
			anim::type::instant);
	}, wrap->lifetime());

	return searchable;
}

void SetupExperimental(
		not_null<Window::Controller*> window,
		not_null<Window::SessionController*> controller,
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<> reloadOptionsRequests,
		rpl::producer<QString> query,
		Fn<void(const QString&, not_null<QWidget*>)> registerHighlight) {
	const auto headerWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto header = headerWrap->entity();

	Ui::AddSkip(header, st::settingsCheckboxesSkip);

	header->add(
		object_ptr<Ui::FlatLabel>(
			header,
			rpl::single(u"警告！这些是实验性设置。部分设置可能无法正常工作，另一些可能导致应用异常。其中任何一项都可能在下一版本中无声消失。请自行承担使用风险。"_q),
			st::boxLabel),
		st::defaultBoxDividerLabelPadding);

	auto reset = (Button*)nullptr;
	if (base::options::changed()) {
		const auto wrap = header->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				header,
				object_ptr<Ui::VerticalLayout>(header)));
		const auto inner = wrap->entity();
		Ui::AddDivider(inner);
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
		reset = inner->add(object_ptr<Button>(
			inner,
			rpl::single(u"恢复默认值"_q),
			st::settingsButtonNoIcon));
		reset->addClickHandler([=] {
			base::options::reset();
			wrap->hide(anim::type::normal);
		});
		Ui::AddSkip(inner, st::settingsCheckboxesSkip);
	}

	Ui::AddDivider(header);

	rpl::duplicate(
		query
	) | rpl::on_next([=](const QString &text) {
		headerWrap->toggle(text.trimmed().isEmpty(), anim::type::instant);
	}, headerWrap->lifetime());

	struct Category {
		QString title;
		std::vector<const char*> options;
	};
	const auto categories = std::vector<Category>{
		{
			u"聊天"_q,
			{
				Dialogs::kOptionForumHideChatsList,
				Dialogs::kOptionDialogsUnreadOnTop,
				Dialogs::Ui::kOptionDialogsMuteIcon,
				kOptionUseNewChatView,
				kOptionAutoScrollInactiveChat,
				kModerateCommonGroups,
				Info::kClassicProfileScroll,
			}
		},
		{
			u"消息"_q,
			{
				Ui::kOptionUseSmallMsgBubbleRadius,
				HistoryView::kOptionUnlimitedMessageWidth,
				HistoryView::Controls::kOptionMacCmdReplyImmediately,
				Ui::kOptionHideAiButton,
				kForceComposeSearchOneColumn,
			}
		},
		{
			u"个人资料"_q,
			{
				Window::kOptionViewProfileInChatsListContextMenu,
				Info::Profile::kOptionShowPeerIdBelowAbout,
				Info::Profile::kOptionShowChannelJoinedBelowAbout,
				Info::Profile::kOptionProfileMediaTabs,
				Info::Profile::kOptionProfileMediaTabsExpanded,
			}
		},
		{
			u"贴纸与表情"_q,
			{
				ChatHelpers::kOptionTabbedPanelShowOnClick,
				ChatHelpers::kOptionUnlimitedRecentStickers,
			}
		},
		{
			u"媒体"_q,
			{
				Media::Player::kOptionDisableAutoplayNext,
				Window::kOptionExternalMediaViewer,
				FFmpeg::kOptionFFmpegMultiThread,
			}
		},
		{
			u"通知"_q,
			{
				Window::Notifications::kOptionHideReplyButton,
				Window::Notifications::kOptionCustomNotification,
				Window::Notifications::kOptionGNotification,
				Window::Notifications::kOptionMacModernNotifications,
			}
		},
		{
			u"界面"_q,
			{
				Core::kOptionFractionalScalingEnabled,
				Core::kOptionHighDpiDownscale,
				Ui::GL::kOptionUseQtRhi,
				Ui::GL::kOptionEnableVulkanRhi,
				Core::kOptionFreeType,
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
				Ui::kOptionKineticScroller,
#endif
				Window::kOptionDisableTouchbar,
				Window::kOptionNewWindowsSizeAsFirst,
			}
		},
		{
			u"系统"_q,
			{
				MTP::details::kOptionPreferIPv6,
				Core::kOptionSkipUrlSchemeRegister,
				Core::kOptionDeadlockDetector,
				Webview::kOptionWebviewDebugEnabled,
				Webview::kOptionWebviewLegacyEdge,
			}
		},
	};

	const auto addOption = [&](
			not_null<Ui::VerticalLayout*> inner,
			const char name[]) {
		return AddOption(
			window,
			controller,
			inner,
			base::options::lookup<bool>(name),
			(reset
				? (reset->clicks() | rpl::to_empty)
				: rpl::producer<>()),
			rpl::duplicate(reloadOptionsRequests),
			rpl::duplicate(query),
			registerHighlight);
	};
	const auto addCategory = [&](
			const QString &title,
			auto &&fill) {
		const auto wrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto inner = wrap->entity();
		Ui::AddSkip(inner);
		Ui::AddSubsectionTitle(inner, rpl::single(title));
		auto searchable = std::vector<QString>();
		fill(inner, searchable);
		Ui::AddSkip(inner);
		Ui::AddDivider(inner);

		auto terms = std::vector<QStringList>();
		for (const auto &entry : searchable) {
			terms.push_back(SearchWords(entry));
		}
		rpl::duplicate(
			query
		) | rpl::on_next([=](const QString &text) {
			const auto words = SearchWords(text);
			const auto matches = words.isEmpty()
				|| ranges::any_of(terms, [&](const QStringList &entry) {
					return MatchesWords(entry, words);
				});
			wrap->toggle(matches, anim::type::instant);
		}, wrap->lifetime());
	};

	for (const auto &category : categories) {
		addCategory(category.title, [&](
				not_null<Ui::VerticalLayout*> inner,
				std::vector<QString> &searchable) {
			for (const auto name : category.options) {
				searchable.push_back(addOption(inner, name));
			}
		});
	}

	addCategory(u"其他"_q, [&](
			not_null<Ui::VerticalLayout*> inner,
			std::vector<QString> &searchable) {
		if (base::options::lookup<bool>(kOptionFastButtonsMode).value()) {
			searchable.push_back(addOption(inner, kOptionFastButtonsMode));
		}
		searchable.push_back(AddFavoriteLinkButton(
			window,
			inner,
			rpl::duplicate(query),
			registerHighlight));
	});
}

} // namespace

Experimental::Experimental(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

Experimental::~Experimental() = default;

rpl::producer<QString> Experimental::title() {
	return rpl::single(u"实验性功能设置"_q);
}

void Experimental::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	const auto window = &controller()->window();
	addAction(
		u"导出"_q,
		[=] {
			TextUtilities::SetClipboardText(
				{ EncodeOptionsToText(base::options::serialize()) });
			window->showToast({
				.text = { u"实验性功能设置代码已复制到剪贴板。"_q },
				.iconLottie = u"toast/copy"_q,
				.iconLottieSize = st::toastLottieIconSize,
			});
		},
		&st::menuIconCopy);
	if (!DecodeOptionsFromText(QGuiApplication::clipboard()->text()).ok) {
		return;
	}
	addAction(
		u"导入"_q,
		[=] {
			const auto decoded = DecodeOptionsFromText(
				QGuiApplication::clipboard()->text());
			if (!decoded.ok) {
				window->showToast(u"剪贴板中没有有效的实验性功能设置代码。"_q);
				return;
			}
			if (!base::options::deserialize(decoded.json)) {
				window->showToast(u"实验性功能设置代码有效，但数据格式不受支持。"_q);
				return;
			}
			_reloadOptionsRequests.fire({});
			window->showToast(u"已从剪贴板代码导入实验性功能设置。"_q);
		},
		&st::menuIconImportTheme);
}

void Experimental::setInnerFocus() {
	if (_searchField) {
		_searchField->setFocus();
	} else {
		setFocus();
	}
}

void Experimental::showFinished() {
	AbstractSection::showFinished();
	for (const auto &[id, widget] : _highlights) {
		if (widget) {
			controller()->checkHighlightControl(id, widget);
		}
	}
}

base::weak_qptr<Ui::RpWidget> Experimental::createPinnedToTop(
		not_null<QWidget*> parent) {
	auto search = CreateSectionSearchRow(parent, _query.current());
	_searchController = std::move(search.controller);
	const auto row = search.row;
	_searchField = search.field;

	_searchController->queryValue(
	) | rpl::on_next([=](QString text) {
		_query = std::move(text);
	}, row->lifetime());

	return base::make_weak(row);
}

void Experimental::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	SetupExperimental(
		&controller()->window(),
		controller(),
		content,
		_reloadOptionsRequests.events(),
		_query.value(),
		[this](const QString &id, not_null<QWidget*> widget) {
			_highlights.push_back({ id, widget.get() });
		});

	Ui::ResizeFitChild(this, content);
}

} // namespace Settings
