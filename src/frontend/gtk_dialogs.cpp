// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend/gtk_dialogs.h"
#include "common/seplugins.h"
#include "common/app_paths.h"
#include "frontend/servers.h"
#include "net/np_client.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <thread>

namespace gtkui {
void Message(GtkWindow *parent, const std::string &text, GtkMessageType type) {
	auto *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_CLOSE, "%s", text.c_str());
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}
GtkWidget *Button(GtkWidget *box, const char *label, std::function<void()> action) {
	auto *button = gtk_button_new_with_label(label);
	auto *fn = new std::function<void()>(std::move(action));
	struct Pending {
		GtkWidget *button;
		std::function<void()> action;
	};
	g_signal_connect_data(
		button, "clicked", G_CALLBACK((+[](GtkButton *button, gpointer p) {
			// Modal loops must not hold the AT-SPI DoAction D-Bus dispatcher open.
			auto *pending = new Pending{GTK_WIDGET(g_object_ref(button)), *static_cast<std::function<void()> *>(p)};
			g_idle_add_full(
				G_PRIORITY_DEFAULT,
				+[](gpointer p) -> gboolean {
					auto &pending = *static_cast<Pending *>(p);
					if (gtk_widget_get_mapped(pending.button))
						pending.action();
					return G_SOURCE_REMOVE;
				},
				pending,
				+[](gpointer p) {
					auto *pending = static_cast<Pending *>(p);
					g_object_unref(pending->button);
					delete pending;
				});
		})),
		fn, +[](gpointer p, GClosure *) { delete static_cast<std::function<void()> *>(p); }, static_cast<GConnectFlags>(0));
	gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
	return button;
}
std::string Choose(GtkWindow *parent, const char *title, GtkFileChooserAction action) {
	auto *dialog = gtk_file_chooser_dialog_new(title, parent, action, "Cancel", GTK_RESPONSE_CANCEL, "Select",
											   GTK_RESPONSE_ACCEPT, nullptr);
	std::string path;
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		char *p = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		path = p;
		g_free(p);
	}
	gtk_widget_destroy(dialog);
	return path;
}
std::string Narrow(const std::wstring &text) {
	std::vector<gunichar> ucs(text.begin(), text.end());
	ucs.push_back(0);
	char *p = g_ucs4_to_utf8(ucs.data(), -1, nullptr, nullptr, nullptr);
	std::string result = p ? p : "Invalid Unicode";
	g_free(p);
	return result;
}
std::wstring Widen(const std::string &text) {
	char *valid = g_utf8_make_valid(text.data(), text.size());
	auto *p = g_utf8_to_ucs4(valid, -1, nullptr, nullptr, nullptr);
	std::wstring result;
	for (auto *i = p; i && *i; ++i)
		result.push_back((wchar_t)*i);
	g_free(p);
	g_free(valid);
	return result;
}
namespace {
GtkWidget *Dialog(GtkWindow *parent, const char *title, bool save = false) {
	auto *d = gtk_dialog_new_with_buttons(title, parent, GTK_DIALOG_MODAL, save ? "Cancel" : "Close",
										  GTK_RESPONSE_CANCEL, nullptr);
	if (save)
		gtk_dialog_add_button(GTK_DIALOG(d), "Save", GTK_RESPONSE_ACCEPT);
	gtk_window_set_default_size(GTK_WINDOW(d), 720, 580);
	gtk_container_set_border_width(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), 12);
	return d;
}
GtkWidget *Scroll(GtkWidget *content) {
	auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_container_add(GTK_CONTAINER(scroll), content);
	gtk_widget_set_hexpand(scroll, TRUE);
	gtk_widget_set_vexpand(scroll, TRUE);
	return scroll;
}
GtkWidget *Entry(GtkWidget *grid, int row, const std::string &label, const std::string &value = {}) {
	auto *text = gtk_label_new(label.c_str());
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	auto *entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(entry), value.c_str());
	gtk_label_set_mnemonic_widget(GTK_LABEL(text), entry);
	atk_object_set_name(gtk_widget_get_accessible(entry), label.c_str());
	gtk_widget_set_hexpand(entry, TRUE);
	gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
	return entry;
}
std::string Text(GtkWidget *entry) { return gtk_entry_get_text(GTK_ENTRY(entry)); }
bool Save(GtkWindow *parent, settings::Settings &destination, const settings::Settings &value) {
	if (!settings::Save(settings::DefaultPath(), value)) {
		Message(parent, "Cannot save pspc.ini");
		return false;
	}
	destination = value;
	return true;
}
void Work(GtkWindow *parent, const std::function<void()> &task) {
	auto *dialog = gtk_dialog_new_with_buttons("Working…", parent, GTK_DIALOG_MODAL, nullptr, nullptr);
	auto *spinner = gtk_spinner_new();
	gtk_spinner_start(GTK_SPINNER(spinner));
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), spinner);
	g_signal_connect(dialog, "delete-event",
					 G_CALLBACK(+[](GtkWidget *, GdkEvent *, gpointer) -> gboolean { return TRUE; }), nullptr);
	gtk_widget_show_all(dialog);
	++busy;
	auto future = std::async(std::launch::async, task);
	while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
		while (gtk_events_pending())
			gtk_main_iteration();
	gtk_widget_destroy(dialog);
	--busy;
	try {
		future.get();
	} catch (const std::exception &error) {
		Message(parent, error.what());
	}
}
} // namespace
void Settings(GtkWindow *parent, settings::Settings &settings) {
	auto *dialog = Dialog(parent, "Settings and input mappings", true);
	auto *tabs = gtk_notebook_new();
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), tabs);
	GtkWidget *grids[4];
	int rows[4]{};
	const char *names[] = {"General / graphics / audio", "Keyboard", "Controller", "Network / hosting"};
	for (int i = 0; i < 4; ++i) {
		grids[i] = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grids[i]), 8);
		gtk_grid_set_column_spacing(GTK_GRID(grids[i]), 12);
		gtk_container_set_border_width(GTK_CONTAINER(grids[i]), 12);
		gtk_notebook_append_page(GTK_NOTEBOOK(tabs), Scroll(grids[i]), gtk_label_new(names[i]));
	}
	const std::set<std::string> bools{"fullscreen",	  "vsync",
									  "aot",		  "plugins",
									  "mute",		  "network",
									  "host_network", "host_network_lan",
									  "host_sfco",	  "host_sfco_allow_local",
									  "host_gamespy", "host_adhoc"};
	struct Field {
		std::string key;
		GtkWidget *widget;
		bool check;
	};
	std::vector<Field> fields;
	for (const auto &key : settings::Keys()) {
		if (key == "np_token")
			continue;
		int page = key.starts_with("key.")	 ? 1
				   : key.starts_with("pad.") ? 2
				   : key.starts_with("host_") || key.starts_with("np_") || key == "network" || key == "infra_dns" ||
						   key == "adhoc_server"
					   ? 3
					   : 0;
		int row = rows[page]++;
		std::string label = key;
		std::replace(label.begin(), label.end(), '_', ' ');
		for (size_t i = 0; i < settings::kNumControls; ++i)
			if (key == "key." + std::string(settings::kControls[i].name) ||
				key == "pad." + std::string(settings::kControls[i].name))
				label = settings::kControls[i].label;
		GtkWidget *widget;
		bool check = bools.contains(key);
		if (check) {
			widget = gtk_check_button_new_with_label(label.c_str());
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), settings::Get(settings, key) == "1");
			gtk_grid_attach(GTK_GRID(grids[page]), widget, 0, row, 2, 1);
		} else {
			widget = Entry(grids[page], row, label, settings::Get(settings, key));
			const char *hint = page == 1   ? "Comma-separated SDL key names; empty unbinds"
							   : page == 2 ? "Comma-separated controller buttons; stick: left, right, both, none"
										   : nullptr;
			if (key == "renderer")
				hint = "software or opengl (OpenGL 4.5 with ordered fragment interlock)";
			if (key == "render_scale")
				hint = "Internal resolution: 1–16; empty uses 1";
			if (key == "texture_filter")
				hint = "auto, nearest, linear";
			if (key == "texture_upscale")
				hint = "xBR factor: 1 (off), 2, 3, 4";
			if (hint)
				gtk_widget_set_tooltip_text(widget, hint);
		}
		fields.push_back({key, widget, check});
	}
	gtk_widget_show_all(dialog);
	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		auto updated = settings;
		std::string error;
		for (const auto &field : fields) {
			std::string value = field.check ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field.widget)) ? "1" : "0"
											: Text(field.widget);
			if (!settings::Set(updated, field.key, value, &error))
				break;
		}
		if (!error.empty()) {
			Message(GTK_WINDOW(dialog), error);
			continue;
		}
		if (updated.npServer != settings.npServer)
			updated.npToken.clear();
		if (Save(GTK_WINDOW(dialog), settings, updated))
			break;
	}
	gtk_widget_destroy(dialog);
}
void Plugins(GtkWindow *parent, const settings::Settings &settings) {
	fs::path root = apppaths::DataPath(settings.pluginsDir.empty() ? "plugins" : settings.pluginsDir);
	auto *dialog = Dialog(parent, "Plugins — game, XMB and POPS", true);
	auto *tabs = gtk_notebook_new();
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), tabs);
	struct Row {
		seplugins::Entry entry;
		GtkWidget *box, *path, *enabled;
		seplugins::List list;
	};
	std::vector<std::unique_ptr<Row>> entries;
	auto add = [&](GtkWidget *box, seplugins::List list, seplugins::Entry entry) {
		auto row = std::make_unique<Row>();
		row->entry = std::move(entry);
		row->list = list;
		row->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_box_pack_start(GTK_BOX(box), row->box, FALSE, FALSE, 0);
		row->enabled = gtk_check_button_new();
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->enabled), row->entry.enabled);
		row->path = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(row->path), row->entry.path.c_str());
		gtk_box_pack_start(GTK_BOX(row->box), row->enabled, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row->box), row->path, TRUE, TRUE, 0);
		Button(row->box, "Remove", [box = row->box] { gtk_widget_hide(box); });
		gtk_widget_show_all(row->box);
		entries.push_back(std::move(row));
	};
	for (auto list : seplugins::kLists) {
		auto *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
		auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
		gtk_box_pack_start(GTK_BOX(page), Scroll(box), TRUE, TRUE, 0);
		Button(page, "Add PRX path", [&, box, list] { add(box, list, {"ms0:/seplugins/", true, -1}); });
		for (const auto &entry : seplugins::ReadList(root, list))
			add(box, list, entry);
		gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new(seplugins::FileName(list)));
	}
	gtk_widget_show_all(dialog);
	while (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		bool ok = true;
		for (auto list : seplugins::kLists) {
			std::vector<seplugins::Entry> selected;
			for (const auto &row : entries)
				if (row->list == list && gtk_widget_get_visible(row->box)) {
					auto entry = row->entry;
					entry.path = Text(row->path);
					entry.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(row->enabled));
					if (entry.path.empty() || entry.path.find_first_of("\r\n\t ") != std::string::npos) {
						ok = false;
						break;
					}
					selected.push_back(std::move(entry));
				}
			if (!ok || !seplugins::WriteList(root, list, selected)) {
				ok = false;
				break;
			}
		}
		if (ok)
			break;
		Message(GTK_WINDOW(dialog), "Cannot save plugin lists. Check PRX paths and folder permissions.");
	}
	gtk_widget_destroy(dialog);
}
void Account(GtkWindow *parent, settings::Settings &settings) {
	auto *dialog = Dialog(parent, "PSPC Network — account and friends");
	auto *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	auto *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
	auto *server = Entry(grid, 0, "Server URL", settings.npServer);
	auto *id = Entry(grid, 1, "Online ID", settings.npOnlineId);
	auto *password = Entry(grid, 2, "Password");
	gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
	auto *country = Entry(grid, 3, "Country", "us");
	auto *friendId = Entry(grid, 4, "Friend's online ID");
	auto *result = gtk_label_new("");
	gtk_label_set_selectable(GTK_LABEL(result), TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(result), TRUE);
	gtk_box_pack_start(GTK_BOX(box), Scroll(result), TRUE, TRUE, 0);
	const char *actions[] = {"Sign in",		 "Register", "Sign out", "Refresh friends", "Add / accept friend",
							 "Remove friend"};
	for (int i = 0; i < 6; ++i)
		gtk_dialog_add_button(GTK_DIALOG(dialog), actions[i], 100 + i);
	gtk_widget_show_all(dialog);
	for (;;) {
		int action = gtk_dialog_run(GTK_DIALOG(dialog));
		if (action < 100 || action > 105)
			break;
		std::string url = np::NormalizeServerUrl(Text(server)), onlineId = Text(id), secret = Text(password),
					region = Text(country), target = Text(friendId);
		np::NetworkClient client(url, url == np::NormalizeServerUrl(settings.npServer) ? settings.npToken : "");
		client.timeoutMs = 3000;
		np::Result response;
		np::Profile profile;
		std::string token;
		std::vector<np::Friend> friends;
		Work(GTK_WINDOW(dialog), [&] {
			switch (action) {
			case 100:
				response = client.SignIn(onlineId, secret, &profile, &token);
				break;
			case 101:
				response = client.Register(onlineId, secret, region, &profile, &token);
				break;
			case 102:
				response = client.SignOut();
				break;
			case 103:
				response = client.Friends(&friends);
				break;
			case 104: {
				np::Friend f;
				response = client.AddFriend(target, &f);
				break;
			}
			case 105:
				response = client.RemoveFriend(target);
				break;
			}
		});
		gtk_entry_set_text(GTK_ENTRY(password), "");
		if (!response) {
			gtk_label_set_text(GTK_LABEL(result), response.message.c_str());
			continue;
		}
		auto updated = settings;
		if (action == 100 || action == 101) {
			updated.npServer = url;
			updated.npOnlineId = profile.onlineId;
			updated.npToken = token;
			Save(GTK_WINDOW(dialog), settings, updated);
		}
		if (action == 102) {
			updated.npToken.clear();
			Save(GTK_WINDOW(dialog), settings, updated);
		}
		std::string message = "Completed.";
		if (action == 103) {
			message = friends.empty() ? "No friends or requests." : "";
			for (auto &f : friends)
				message += f.onlineId + " — " + f.status + "\n";
		}
		gtk_label_set_text(GTK_LABEL(result), message.c_str());
	}
	gtk_widget_destroy(dialog);
}
void Online(GtkWindow *parent, settings::Settings &settings, const std::wstring &umd) {
	auto *dialog = Dialog(parent, "Online — servers, administration and routing");
	auto *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	auto *choice = gtk_combo_box_text_new();
	for (const char *name : {"PSPC Network", "Combat Ops", "Ad hoc", "GameSpy"})
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(choice), name);
	gtk_combo_box_set_active(GTK_COMBO_BOX(choice), 0);
	gtk_box_pack_start(GTK_BOX(box), choice, FALSE, FALSE, 0);
	auto *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
	struct Operation {
		const char *label;
		std::vector<std::wstring> args;
		int value;
		bool confirm;
	};
	const std::vector<Operation> operations = {{"Help", {L"--help"}, 0, false},
											   {"Accounts / players", {L"--json", L"accounts"}, 0, false},
											   {"Ban player", {L"ban"}, 1, false},
											   {"Unban player", {L"unban"}, 1, false},
											   {"Set password", {L"passwd"}, 2, false},
											   {"Missions", {L"--json", L"missions", L"list"}, 0, false},
											   {"Hide mission", {L"missions", L"hide"}, 1, false},
											   {"Show mission", {L"missions", L"unhide"}, 1, false},
											   {"Delete mission", {L"missions", L"delete"}, 1, true},
											   {"News", {L"--json", L"news", L"list"}, 0, false},
											   {"Add news", {L"news", L"add"}, 3, false},
											   {"Delete news", {L"news", L"delete"}, 1, true},
											   {"Board posts", {L"--json", L"board", L"list"}, 0, false},
											   {"Hide post", {L"board", L"hide"}, 1, false},
											   {"Show post", {L"board", L"unhide"}, 1, false},
											   {"GameSpy careers", {L"--json", L"stats"}, 0, false},
											   {"GameSpy last games", {L"--json", L"games"}, 0, false},
											   {"Custom arguments", {}, 4, false}};
	auto *operation = gtk_combo_box_text_new();
	for (const auto &op : operations)
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(operation), op.label);
	gtk_combo_box_set_active(GTK_COMBO_BOX(operation), 0);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Administration"), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), operation, 1, 0, 1, 1);
	auto *identifier = Entry(grid, 1, "Player / item ID");
	auto *input = Entry(grid, 2, "Password / news text");
	gtk_entry_set_visibility(GTK_ENTRY(input), FALSE);
	auto *command = Entry(grid, 3, "Custom arguments", "--help");
	auto *disc = Entry(grid, 4, "Redirect disc ID", servers::kSfcoDiscId);
	auto *domain = Entry(grid, 5, "Service domain", servers::kSfcoDomain);
	auto *address = Entry(grid, 6, "Target host (empty removes)", Narrow(servers::LanAddress()));
	auto *log = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(log), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(log), TRUE);
	gtk_box_pack_start(GTK_BOX(box), Scroll(log), TRUE, TRUE, 0);
	const char *actions[] = {"Start", "Stop", "Status / log", "Run admin", "Save route", "Load route"};
	for (int i = 0; i < 6; ++i)
		gtk_dialog_add_button(GTK_DIALOG(dialog), actions[i], 100 + i);
	gtk_widget_show_all(dialog);
	for (;;) {
		int action = gtk_dialog_run(GTK_DIALOG(dialog));
		if (action < 100 || action > 105)
			break;
		auto kind = (servers::Kind)gtk_combo_box_get_active(GTK_COMBO_BOX(choice));
		std::string output;
		std::wstring error;
		if (action == 100 && !servers::Start(kind, settings, umd, &error))
			output = Narrow(error);
		if (action == 101)
			servers::Stop(kind);
		if (action <= 102 && output.empty()) {
			auto state = servers::Poll(kind);
			output = "State: " +
					 std::string(state == servers::State::Running	 ? "running"
								 : state == servers::State::Starting ? "starting"
								 : state == servers::State::Failed	 ? "failed"
																	 : "stopped") +
					 "\n" + servers::Status(kind) + "\n" + Narrow(servers::LogTail(kind, 100));
		}
		if (action == 103) {
			const auto &op = operations.at(gtk_combo_box_get_active(GTK_COMBO_BOX(operation)));
			auto args = op.args;
			std::string stdinText;
			if (op.value == 1 || op.value == 2) {
				if (Text(identifier).empty()) {
					Message(GTK_WINDOW(dialog), "Choose a player or item ID from the list first.");
					continue;
				}
				args.push_back(Widen(Text(identifier)));
			}
			if (op.value == 2) {
				args.push_back(L"--password-stdin");
				stdinText = Text(input);
			}
			if (op.value == 3)
				args.push_back(Widen(Text(input)));
			if (op.value == 4) {
				int argc = 0;
				char **argv = nullptr;
				GError *error = nullptr;
				if (!g_shell_parse_argv(Text(command).c_str(), &argc, &argv, &error)) {
					Message(GTK_WINDOW(dialog), error->message);
					g_error_free(error);
					continue;
				}
				for (int i = 0; i < argc; ++i)
					args.push_back(Widen(argv[i]));
				g_strfreev(argv);
				stdinText = Text(input);
			}
			if (op.confirm) {
				auto *confirm =
					gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
										   GTK_BUTTONS_YES_NO, "Permanently delete item %s?", Text(identifier).c_str());
				int answer = gtk_dialog_run(GTK_DIALOG(confirm));
				gtk_widget_destroy(confirm);
				if (answer != GTK_RESPONSE_YES)
					continue;
			}
			Work(GTK_WINDOW(dialog), [&] {
				if (!servers::Admin(kind, args, &output, stdinText))
					output = "Command failed:\n" + output;
			});
			gtk_entry_set_text(GTK_ENTRY(input), "");
		}
		if (action == 104)
			output = servers::SetRedirect(Text(disc), Text(disc), Text(domain), Text(address), &error) ? "Route saved."
																									   : Narrow(error);
		if (action == 105) {
			gtk_entry_set_text(GTK_ENTRY(address), servers::Redirect(Text(disc), Text(domain)).c_str());
			output = "Route loaded.";
		}
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(log)), output.c_str(), -1);
	}
	gtk_widget_destroy(dialog);
}
} // namespace gtkui
