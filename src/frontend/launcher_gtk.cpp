// SPDX-License-Identifier: GPL-2.0-or-later
// Native Linux game-library frontend. Game discovery is shared with the Windows launcher.
#include "frontend/game_library.h"
#include "common/fileutil.h"
#include "common/settings.h"
#include "pspc_version.h"
#include <gtk/gtk.h>
#include <cstdio>

namespace {
namespace fs = std::filesystem;
std::vector<frontend::GameInfo> games;
fs::path exeDir;
GtkWidget *window, *list, *search, *status;

fs::path FindUp(fs::path start, const fs::path &relative) {
	for (;;) {
		if (fs::exists(start / relative)) return start / relative;
		if (start == start.root_path() || start.empty()) return {};
		start = start.parent_path();
	}
}
void Error(const std::string &message) {
	GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message.c_str());
	gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
}
void ChildDone(GObject *object, GAsyncResult *result, gpointer) {
	GError *error = nullptr;
	if (!g_subprocess_wait_finish(G_SUBPROCESS(object), result, &error)) {
		Error(error->message);
		g_error_free(error);
	} else if (!g_subprocess_get_successful(G_SUBPROCESS(object))) {
		Error("The child process failed. Its output is in the terminal that started the launcher.");
	} else {
		gtk_label_set_text(GTK_LABEL(status), "Process completed successfully.");
	}
	g_object_unref(object);
}
void Spawn(const std::vector<std::string> &args) {
	std::vector<const char *> argv;
	for (auto &arg : args) argv.push_back(arg.c_str());
	argv.push_back(nullptr);
	GError *error = nullptr;
	auto *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_set_cwd(launcher, exeDir.c_str());
	auto *child = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
	g_object_unref(launcher);
	if (!child) { Error(error->message); g_error_free(error); return; }
	gtk_label_set_text(GTK_LABEL(status), "Process started; output goes to the launcher's terminal.");
	g_subprocess_wait_async(child, nullptr, ChildDone, nullptr);
}
const frontend::GameInfo *Selected() {
	auto *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	return row ? &games.at(gtk_list_box_row_get_index(row)) : nullptr;
}
void Play(GtkButton *, gpointer interp) {
	if (auto *game = Selected()) {
		std::vector<std::string> args{(exeDir / "pspc").string()};
		if (interp) args.push_back("--interp");
		args.push_back(game->path.string());
		Spawn(args);
	}
}
void Activate(GtkListBox *, GtkListBoxRow *, gpointer) { Play(nullptr, nullptr); }
void BootXmb(GtkButton *, gpointer) { Spawn({(exeDir / "pspc").string(), "--vsh"}); }
void Build(GtkButton *, gpointer) {
	if (auto *game = Selected()) {
		auto script = FindUp(exeDir, "tools/pspc_build.py");
		if (script.empty()) { Error("Native builds require the PSPC source tree and its build tools."); return; }
		Spawn({"python3", "-I", script.string(), game->path.string(), "--cache", (exeDir / "cache/aot").string(),
		       "--build-dir", exeDir.parent_path().string()});
	}
}
void Settings(GtkButton *, gpointer) {
	auto path = settings::DefaultPath();
	if (!fs::exists(path) && !settings::Save(path, settings::Settings{})) {
		Error("Cannot create " + path.string()); return;
	}
	GError *error = nullptr;
	char *uri = g_filename_to_uri(path.c_str(), nullptr, &error);
	if (uri) {
		gtk_show_uri_on_window(GTK_WINDOW(window), uri, GDK_CURRENT_TIME, &error);
		g_free(uri);
	}
	if (error) { Error(error->message); g_error_free(error); }
}
gboolean Filter(GtkListBoxRow *row, gpointer) {
	const auto &game = games.at(gtk_list_box_row_get_index(row));
	char *haystack = g_utf8_casefold((game.title + " " + game.id).c_str(), -1);
	char *needle = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(search)), -1);
	bool match = strstr(haystack, needle) != nullptr;
	g_free(haystack); g_free(needle);
	return match;
}
void SearchChanged(GtkEditable *, gpointer) { gtk_list_box_invalidate_filter(GTK_LIST_BOX(list)); }
void Quit(GtkWidget *, gpointer) { gtk_main_quit(); }
void Button(GtkWidget *box, const char *text, GCallback callback, gpointer data = nullptr) {
	auto *button = gtk_button_new_with_label(text);
	g_signal_connect(button, "clicked", callback, data);
	gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
}
void Show() {
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "PSPC " PSPC_VERSION);
	gtk_window_set_default_size(GTK_WINDOW(window), 860, 620);
	g_signal_connect(window, "destroy", G_CALLBACK(Quit), nullptr);
	auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(window), box);
	search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search games by title or disc ID");
	gtk_box_pack_start(GTK_BOX(box), search, FALSE, FALSE, 0);
	auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	for (auto &game : games) {
		auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
		gtk_container_set_border_width(GTK_CONTAINER(row), 8);
		if (!game.icon.empty()) {
			auto *loader = gdk_pixbuf_loader_new();
			gdk_pixbuf_loader_set_size(loader, 144, 80);
			bool written = gdk_pixbuf_loader_write(loader, game.icon.data(), game.icon.size(), nullptr);
			bool closed = gdk_pixbuf_loader_close(loader, nullptr);
			if (written && closed) {
				auto *image = gtk_image_new_from_pixbuf(gdk_pixbuf_loader_get_pixbuf(loader));
				gtk_box_pack_start(GTK_BOX(row), image, FALSE, FALSE, 0);
			}
			g_object_unref(loader);
		}
		std::string text = game.title + "\n" + game.id + " · " + std::to_string(game.size / (1024 * 1024)) + " MiB";
		auto *label = gtk_label_new(text.c_str());
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
		gtk_widget_set_tooltip_text(row, game.path.c_str());
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
	}
	gtk_list_box_set_filter_func(GTK_LIST_BOX(list), Filter, nullptr, nullptr);
	g_signal_connect(search, "changed", G_CALLBACK(SearchChanged), nullptr);
	g_signal_connect(list, "row-activated", G_CALLBACK(Activate), nullptr);
	auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
	Button(actions, "Play", G_CALLBACK(Play));
	Button(actions, "Interpreter", G_CALLBACK(Play), GINT_TO_POINTER(1));
	Button(actions, "Build native code", G_CALLBACK(Build));
	Button(actions, "Boot XMB", G_CALLBACK(BootXmb));
	Button(actions, "Open settings file", G_CALLBACK(Settings));
	status = gtk_label_new("Double-click a game to play. XMB needs installed firmware; native builds need development tools.");
	gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
	gtk_widget_show_all(window);
	if (!games.empty()) gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
	printf("Launcher ready: %zu games\n", games.size());
	fflush(stdout);
}
}

int main(int argc, char **argv) {
	bool listOnly = false;
	fs::path directory;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--list") listOnly = true;
		else if (arg == "--help") { puts("Usage: pspc-launcher [games directory] [--list]"); return 0; }
		else if (arg.starts_with("--") || !directory.empty()) { fprintf(stderr, "Invalid argument: %s\n", arg.c_str()); return 2; }
		else directory = fs::absolute(arg);
	}
	try {
		exeDir = ExecutableDir();
		if (directory.empty()) directory = FindUp(exeDir, "Games");
		if (directory.empty()) directory = exeDir / "Games";
		games = frontend::ScanGames(directory);
		if (listOnly) {
			for (auto &game : games) printf("%s\t%s\t%s\n", game.id.c_str(), game.title.c_str(), game.path.c_str());
			return 0;
		}
		if (!gtk_init_check(nullptr, nullptr)) { fprintf(stderr, "Cannot open the display; --list works without a display.\n"); return 1; }
		Show();
		gtk_main();
	} catch (const std::exception &error) { fprintf(stderr, "%s\n", error.what()); return 1; }
	return 0;
}
