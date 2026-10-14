// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/settings.h"
#include <functional>
#include <gtk/gtk.h>
namespace gtkui {
inline int busy = 0;
void Message(GtkWindow *parent, const std::string &message, GtkMessageType type = GTK_MESSAGE_ERROR);
GtkWidget *Button(GtkWidget *box, const char *label, std::function<void()> action);
std::string Choose(GtkWindow *parent, const char *title, GtkFileChooserAction action);
std::string Narrow(const std::wstring &text);
std::wstring Widen(const std::string &text);
void Settings(GtkWindow *parent, settings::Settings &settings);
void Plugins(GtkWindow *parent, const settings::Settings &settings);
void Account(GtkWindow *parent, settings::Settings &settings);
void Online(GtkWindow *parent, settings::Settings &settings, const std::wstring &umd);
} // namespace gtkui
