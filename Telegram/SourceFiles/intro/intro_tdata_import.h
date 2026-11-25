/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Account;
} // namespace Main

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Intro::details {

void ShowTdataImport(
	not_null<Ui::RpWidget*> parent,
	not_null<Main::Account*> pending);

} // namespace Intro::details
