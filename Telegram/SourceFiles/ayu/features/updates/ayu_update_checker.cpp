// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/updates/ayu_update_checker.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "ui/toast/toast.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <vector>

#ifndef AYU_MOD_VERSION
#define AYU_MOD_VERSION "1.0.0"
#endif // AYU_MOD_VERSION

namespace AyuUpdate {
namespace {

constexpr auto kUpdateApiUrl
	= "https://api.github.com/repos/Felix666-ship-It/AyuGramDesktop/releases/latest";

QPointer<QNetworkReply> ActiveReply;

[[nodiscard]] QString CurrentVersion() {
	return QString::fromLatin1(AYU_MOD_VERSION);
}

[[nodiscard]] QString NormalizedVersion(QString value) {
	value = value.trimmed();
	if (value.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
		value.remove(0, 1);
	}
	return value;
}

[[nodiscard]] std::vector<int> VersionParts(QString value) {
	auto result = std::vector<int>();
	for (const auto &part : NormalizedVersion(std::move(value)).split('.')) {
		auto ok = false;
		const auto number = part.toInt(&ok);
		result.push_back(ok ? number : 0);
	}
	return result;
}

[[nodiscard]] bool IsNewerVersion(
		const QString &remote,
		const QString &local) {
	const auto a = VersionParts(remote);
	const auto b = VersionParts(local);
	const auto count = std::max(a.size(), b.size());
	for (auto i = std::size_t(0); i != count; ++i) {
		const auto left = (i < a.size()) ? a[i] : 0;
		const auto right = (i < b.size()) ? b[i] : 0;
		if (left != right) {
			return left > right;
		}
	}
	return false;
}

void ShowToast(const QString &text) {
	Ui::Toast::Show({
		.text = { text },
		.duration = 10 * crl::time(1000),
	});
}

void ShowFailure(bool manual) {
	if (manual) {
		ShowToast(tr::ayu_UpdateCheckFailed(tr::now));
	}
}

void ParseResponse(
		QNetworkReply *reply,
		QNetworkAccessManager *manager,
		bool manual) {
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto errorCode = reply->error();
	const auto body = reply->readAll();
	reply->deleteLater();
	manager->deleteLater();
	ActiveReply = nullptr;

	if (errorCode != QNetworkReply::NoError || status != 200) {
		ShowFailure(manual);
		return;
	}

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		ShowFailure(manual);
		return;
	}
	const auto object = document.object();
	const auto tag = object.value(u"tag_name"_q).toString();
	const auto url = object.value(u"html_url"_q).toString();
	if (tag.isEmpty() || url.isEmpty()) {
		ShowFailure(manual);
		return;
	}
	if (!IsNewerVersion(tag, CurrentVersion())) {
		if (manual) {
			ShowToast(tr::ayu_UpdateLatest(tr::now));
		}
		return;
	}

	const auto version = NormalizedVersion(tag);
	Ui::Toast::Show({
		.text = { tr::ayu_UpdateAvailable(tr::now, lt_version, version) },
		.filter = [url](const ClickHandlerPtr &, Qt::MouseButton) {
			QDesktopServices::openUrl(QUrl(url));
			return false;
		},
		.duration = 15 * crl::time(1000),
	});
}

} // namespace

void StartDelayed() {
	QTimer::singleShot(3000, &Core::App(), [] {
		CheckNow(false);
	});
}

void CheckNow(bool manual) {
	if (ActiveReply) {
		return;
	}
	const auto manager = new QNetworkAccessManager(
		QCoreApplication::instance());
	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kUpdateApiUrl)));
	request.setRawHeader(
		"User-Agent",
		QByteArray("AyuGramMod/") + CurrentVersion().toLatin1());
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	const auto reply = manager->get(request);
	ActiveReply = reply;
	QObject::connect(
		reply,
		&QNetworkReply::finished,
		reply,
		[=] { ParseResponse(reply, manager, manual); });
}

} // namespace AyuUpdate