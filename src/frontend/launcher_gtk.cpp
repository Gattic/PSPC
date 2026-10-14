// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/fileutil.h"
#include "common/app_paths.h"
#include "common/settings.h"
#include "frontend/child_process.h"
#include "frontend/game_library.h"
#include "frontend/gtk_dialogs.h"
#include "frontend/native_cache.h"
#include "frontend/servers.h"
#include "pspc_sem_version.h"
#include "pspc_version.h"
#include <algorithm>
#include <cstdio>
#include <gtk/gtk.h>
#include <memory>

namespace {
namespace fs = std::filesystem;
std::vector<frontend::GameInfo> games;
fs::path exeDir, resourceDir, gamesDir;
settings::Settings config;
GtkWidget *window, *list, *search, *status, *output;
bool closing = false;
struct Job {
	struct WorkDirectory {
		fs::path path;
		~WorkDirectory() {
			std::error_code ec;
			if (!path.empty())
				fs::remove_all(path, ec);
		}
	} cleanup;
	// Members are destroyed in reverse order: reap the process before removing its work files.
	frontend::ChildProcess process;
	std::vector<std::vector<std::string>> steps;
	size_t step = 0;
	std::string role;
	frontend::GameInfo game;
	bool playAfter = false;
};
std::unique_ptr<Job> build, gameJob;

void Error(const std::string &message) { gtkui::Message(GTK_WINDOW(window), message); }
const frontend::GameInfo *Selected() {
	auto *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	return row ? &games.at(gtk_list_box_row_get_index(row)) : nullptr;
}
frontend::NativeInfo Native(const frontend::GameInfo &game) {
	return frontend::InspectNativeCache(apppaths::CacheRoot() / "aot", game, PSPC_SEM_VERSION);
}
void Status() {
	std::string text;
	if (build)
		text = build->role + ": step " + std::to_string(build->step + 1) + "/" + std::to_string(build->steps.size()) +
			   " — " + build->game.title + "\n";
	if (auto *game = Selected()) {
		auto n = Native(*game);
		text += std::string(frontend::NativeLabel(n.state)) + " — " + std::to_string(n.built) + "/" +
				std::to_string(n.total) + " modules; " + std::to_string(n.failed) + " failed";
		if (!n.error.empty())
			text += "\n" + n.error;
	}
	if (text.empty())
		text = "Choose a games folder or install firmware to boot the XMB.";
	gtk_label_set_text(GTK_LABEL(status), text.c_str());
}
bool StartStep(Job &job) {
	std::string error;
	if (!job.process.Start(job.steps[job.step], apppaths::DataRoot(), apppaths::StateRoot() / "logs" / (job.role + ".log"), &error)) {
		Error(error);
		return false;
	}
	Status();
	return true;
}
void LaunchGame(const frontend::GameInfo &game, bool interpreter) {
	if (gameJob) {
		Error("Stop the running game before starting another.");
		return;
	}
	auto job = std::make_unique<Job>();
	job->role = "runtime";
	job->game = game;
	std::vector<std::string> args{(exeDir / "pspc").string()};
	if (interpreter)
		args.push_back("--interp");
	if (game.path.empty())
		args.push_back("--vsh");
	else
		args.push_back(game.path.string());
	job->steps.push_back(std::move(args));
	if (StartStep(*job))
		gameJob = std::move(job);
}
void Build(const frontend::GameInfo &game, bool playAfter, bool force) {
	if (build) {
		Error("A build or firmware installation is already running.");
		return;
	}
	auto script = resourceDir / "tools/pspc_build.py";
	if (!fs::is_regular_file(script)) {
		Error("Cannot find tools/pspc_build.py. Install the AOT SDK or build from the PSPC source tree.");
		return;
	}
	auto job = std::make_unique<Job>();
	job->role = "native-build";
	job->game = game;
	job->playAfter = playAfter;
	std::error_code ec;
	fs::create_directories(apppaths::CacheRoot(), ec);
	if (ec) {
		Error(ec.message());
		return;
	}
	std::string work = (apppaths::CacheRoot() / "native-work-XXXXXX").string();
	if (!g_mkdtemp(work.data())) {
		Error("Cannot create a native-build work directory.");
		return;
	}
	job->cleanup.path = work;
	job->steps = {{"python3", "-I", "-u", script.string(), game.path.string(), "--cache",
				   (apppaths::CacheRoot() / "aot").string(), "--jobs", "4", "--work", work}};
	bool installed = fs::is_directory(exeDir / "aot");
	job->steps[0].insert(job->steps[0].end(), {installed ? "--pspc-dir" : "--build-dir",
											 (installed ? exeDir : exeDir.parent_path()).string()});
	if (force)
		job->steps[0].push_back("--force");
	if (StartStep(*job))
		build = std::move(job);
	Status();
}
void Play(bool interpreter = false) {
	auto *selected = Selected();
	if (!selected)
		return;
	auto game = *selected;
	if (interpreter || !config.aot || Native(game).state == frontend::Native::Built) {
		LaunchGame(game, interpreter || !config.aot);
		return;
	}
	auto *dialog = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
										  "Native code is missing, partial or stale. Build it before playing?");
	gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL, "Interpreter", 1, "Use available code", 2,
						   "Build and play", 3, nullptr);
	int answer = gtk_dialog_run(GTK_DIALOG(dialog));
	gtk_widget_destroy(dialog);
	if (answer == 3)
		Build(game, true, false);
	else if (answer == 1 || answer == 2)
		LaunchGame(game, answer == 1);
}
void Firmware(bool bootAfter = false) {
	if (build || gameJob) {
		Error("Finish the running game/build before installing firmware.");
		return;
	}
	auto script = resourceDir / "tools/pspc_install_fw.py";
	if (!fs::is_regular_file(script) || !fs::exists(exeDir / "pspc-decrypt")) {
		Error("Firmware installation needs pspc-decrypt and tools/pspc_install_fw.py.");
		return;
	}
	auto pbp = gtkui::Choose(GTK_WINDOW(window), "Sony PSP 4.01 update EBOOT.PBP", GTK_FILE_CHOOSER_ACTION_OPEN);
	if (pbp.empty())
		return;
	std::error_code ec;
	fs::create_directories(apppaths::CacheRoot(), ec);
	if (ec) {
		Error(ec.message());
		return;
	}
	auto available = fs::space(apppaths::CacheRoot(), ec).available;
	if (ec || available < 512 * 1024 * 1024) {
		Error("Firmware installation needs 512 MiB free disk space.");
		return;
	}
	std::string temp = (apppaths::CacheRoot() / "firmware-XXXXXX").string();
	if (!g_mkdtemp(temp.data())) {
		Error("Cannot create a firmware work directory.");
		return;
	}
	auto job = std::make_unique<Job>();
	job->role = "firmware";
	job->cleanup.path = temp;
	job->playAfter = bootAfter;
	job->steps.push_back({(exeDir / "pspc-decrypt").string(), "psar", pbp, temp, "--model", "02g"});
	job->steps.push_back({"python3", "-I", "-u", script.string(), "--fw", temp, "--pspc-dir", exeDir.string(),
		"--data-dir", apppaths::DataRoot().string(), "--cache", (apppaths::CacheRoot() / "aot").string(),
		"--work", (fs::path(temp) / "aot-work").string()});
	bool installed = fs::is_directory(exeDir / "aot");
	if (installed || fs::exists(exeDir.parent_path() / "lib/libpspc_sem.a")) {
		job->steps.back().insert(job->steps.back().end(), {"--aot-core", "--jobs", "4", "--modules-in-parallel", "1"});
		if (!installed)
			job->steps.back().insert(job->steps.back().end(), {"--build-dir", exeDir.parent_path().string()});
	} else
		job->steps.back().push_back("--no-aot");
	if (StartStep(*job))
		build = std::move(job);
	Status();
}
void Xmb() {
	if (!fs::exists(apppaths::DataRoot() / "flash0/vsh/module/vshmain.prx")) {
		Firmware(true);
		return;
	}
	LaunchGame({}, false);
}
gboolean Filter(GtkListBoxRow *row, gpointer) {
	const auto &game = games.at(gtk_list_box_row_get_index(row));
	char *haystack = g_utf8_casefold((game.title + " " + game.id).c_str(), -1);
	char *needle = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(search)), -1);
	bool match = strstr(haystack, needle) != nullptr;
	g_free(haystack);
	g_free(needle);
	return match;
}
void Populate() {
	auto *children = gtk_container_get_children(GTK_CONTAINER(list));
	for (auto *p = children; p; p = p->next)
		gtk_widget_destroy(GTK_WIDGET(p->data));
	g_list_free(children);
	for (const auto &game : games) {
		auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
		gtk_container_set_border_width(GTK_CONTAINER(row), 8);
		if (!game.icon.empty()) {
			auto *loader = gdk_pixbuf_loader_new();
			gdk_pixbuf_loader_set_size(loader, 144, 80);
			bool written = gdk_pixbuf_loader_write(loader, game.icon.data(), game.icon.size(), nullptr);
			bool closed = gdk_pixbuf_loader_close(loader, nullptr);
			if (written && closed)
				gtk_box_pack_start(GTK_BOX(row), gtk_image_new_from_pixbuf(gdk_pixbuf_loader_get_pixbuf(loader)), FALSE,
								   FALSE, 0);
			g_object_unref(loader);
		}
		std::string text = game.title + "\n" + game.id + " · " + std::to_string(game.size / (1024 * 1024)) + " MiB · " +
						   frontend::NativeLabel(Native(game).state);
		auto *label = gtk_label_new(text.c_str());
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
		gtk_widget_set_tooltip_text(row, game.path.c_str());
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
	}
	gtk_widget_show_all(list);
	if (!games.empty())
		gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
	Status();
}
void Refresh() {
	try {
		auto scanned = frontend::ScanGames(gamesDir);
		gtk_list_box_unselect_all(GTK_LIST_BOX(list));
		games = std::move(scanned);
		Populate();
	} catch (const std::exception &e) {
		Error(e.what());
	}
}
void OpenPath(const fs::path &path) {
	GError *error = nullptr;
	char *uri = g_filename_to_uri(path.c_str(), nullptr, &error);
	if (uri) {
		gtk_show_uri_on_window(GTK_WINDOW(window), uri, GDK_CURRENT_TIME, &error);
		g_free(uri);
	}
	if (error) {
		Error(error->message);
		g_error_free(error);
	}
}
gboolean Tick(gpointer) {
	for (int i = 0; i < servers::kKinds; ++i)
		servers::Poll((servers::Kind)i);
	for (auto *owner : {&build, &gameJob}) {
		if (!*owner)
			continue;
		auto &job = **owner;
		job.process.Poll();
		if (owner == &build || !build) {
			auto text = job.process.Output();
			if (!job.process.Running())
				text += "\n--- exit " + std::to_string(job.process.ExitCode()) +
						(job.process.Cancelled() ? " (cancelled)" : "") + " ---\n";
			char *valid = g_utf8_make_valid(text.data(), text.size());
			gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(output)), valid, -1);
			g_free(valid);
		}
		if (job.process.Running())
			continue;
		bool success = job.process.ExitCode() == 0 && !job.process.Cancelled();
		if (success && !closing && ++job.step < job.steps.size() && StartStep(job))
			continue;
		bool play = success && job.playAfter && !closing && job.step == job.steps.size();
		auto game = job.game;
		owner->reset();
		Populate();
		if (play)
			LaunchGame(game, false);
	}
	if (closing && !build && !gameJob && !gtkui::busy) {
		servers::StopAll();
		gtk_widget_destroy(window);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}
gboolean Close(GtkWidget *, GdkEvent *, gpointer) {
	if (gtkui::busy) {
		gtk_label_set_text(GTK_LABEL(status), "An account/admin request is finishing. Close again when it completes.");
		return TRUE;
	}
	if (build || gameJob || servers::AnyRunning()) {
		auto *dialog =
			gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
								   "Stop owned games, builds and servers and close PSPC?");
		int answer = gtk_dialog_run(GTK_DIALOG(dialog));
		gtk_widget_destroy(dialog);
		if (answer != GTK_RESPONSE_YES)
			return TRUE;
	}
	closing = true;
	if (build)
		build->process.Cancel();
	if (gameJob)
		gameJob->process.Cancel();
	gtk_widget_set_sensitive(window, FALSE);
	return TRUE;
}
void Show() {
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "PSPC " PSPC_VERSION);
	gtk_window_set_default_size(GTK_WINDOW(window), 1050, 720);
	g_signal_connect(window, "delete-event", G_CALLBACK(Close), nullptr);
	g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) { gtk_main_quit(); }), nullptr);
	auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 14);
	gtk_container_add(GTK_CONTAINER(window), box);
	auto *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, FALSE, 0);
	gtkui::Button(toolbar, "Games folder", [] {
		auto path = gtkui::Choose(GTK_WINDOW(window), "Games folder", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
		if (!path.empty()) {
			auto updated = config;
			updated.gamesDir = path;
			if (!settings::Save(settings::DefaultPath(), updated)) {
				Error("Cannot save games folder");
				return;
			}
			config = updated;
			gamesDir = path;
			Refresh();
		}
	});
	gtkui::Button(toolbar, "Refresh", Refresh);
	gtkui::Button(toolbar, "Artwork", [] {
		auto *game = Selected();
		if (!game)
			return;
		auto bytes = frontend::ReadPic1(game->path);
		if (bytes.empty()) {
			gtkui::Message(GTK_WINDOW(window), "No background artwork in this file.", GTK_MESSAGE_INFO);
			return;
		}
		auto *loader = gdk_pixbuf_loader_new();
		bool written = gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), nullptr);
		bool closed = gdk_pixbuf_loader_close(loader, nullptr);
		if (written && closed) {
			auto *dialog = gtk_dialog_new_with_buttons(game->title.c_str(), GTK_WINDOW(window), GTK_DIALOG_MODAL,
													   "Close", GTK_RESPONSE_CLOSE, nullptr);
			gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
							  gtk_image_new_from_pixbuf(gdk_pixbuf_loader_get_pixbuf(loader)));
			gtk_widget_show_all(dialog);
			gtk_dialog_run(GTK_DIALOG(dialog));
			gtk_widget_destroy(dialog);
		} else
			Error("Cannot decode the game's artwork.");
		g_object_unref(loader);
	});
	gtkui::Button(toolbar, "Settings / Input", [] {
		gtkui::Settings(GTK_WINDOW(window), config);
		gamesDir = apppaths::DataPath(config.gamesDir.empty() ? "Games" : config.gamesDir);
		Refresh();
	});
	gtkui::Button(toolbar, "Plugins", [] { gtkui::Plugins(GTK_WINDOW(window), config); });
	gtkui::Button(toolbar, "Account / Friends", [] { gtkui::Account(GTK_WINDOW(window), config); });
	gtkui::Button(toolbar, "Online / Servers", [] {
		auto *game = Selected();
		gtkui::Online(GTK_WINDOW(window), config, game ? gtkui::Widen(game->path.string()) : L"");
	});
	search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search games by title or disc ID");
	gtk_box_pack_start(GTK_BOX(box), search, FALSE, FALSE, 0);
	auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	list = gtk_list_box_new();
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
	gtk_list_box_set_filter_func(GTK_LIST_BOX(list), Filter, nullptr, nullptr);
	g_signal_connect(search, "changed",
					 G_CALLBACK(+[](GtkEditable *, gpointer) { gtk_list_box_invalidate_filter(GTK_LIST_BOX(list)); }),
					 nullptr);
	g_signal_connect(list, "row-selected", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *, gpointer) {
						 if (status)
							 Status();
					 }),
					 nullptr);
	auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
	auto *play = gtkui::Button(actions, "Play", [] { Play(); });
	g_signal_connect(list, "row-activated", G_CALLBACK(+[](GtkListBox *, GtkListBoxRow *, gpointer button) {
						 gtk_button_clicked(GTK_BUTTON(button));
					 }),
					 play);
	gtkui::Button(actions, "Interpreter", [] { Play(true); });
	gtkui::Button(actions, "Rebuild native", [] {
		if (auto *game = Selected())
			Build(*game, false, true);
	});
	gtkui::Button(actions, "Cancel build", [] {
		if (build)
			build->process.Cancel();
	});
	gtkui::Button(actions, "Stop game", [] {
		if (gameJob)
			gameJob->process.Cancel();
	});
	gtkui::Button(actions, "Boot XMB", Xmb);
	gtkui::Button(actions, "Install firmware", [] { Firmware(); });
	gtkui::Button(actions, "Show file", [] {
		if (auto *game = Selected())
			OpenPath(game->path.parent_path());
	});
	status = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
	auto *logScroll = gtk_scrolled_window_new(nullptr, nullptr);
	gtk_widget_set_size_request(logScroll, -1, 140);
	gtk_box_pack_start(GTK_BOX(box), logScroll, FALSE, TRUE, 0);
	output = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(output), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(output), TRUE);
	gtk_container_add(GTK_CONTAINER(logScroll), output);
	Populate();
	gtk_widget_show_all(window);
	g_timeout_add(100, Tick, nullptr);
	servers::Init(exeDir);
	bool start[] = {config.hostNetwork, config.hostSfco, config.hostAdhoc, config.hostGameSpy};
	for (int i = 0; i < servers::kKinds; ++i)
		if (start[i]) {
			std::wstring error;
			if (!servers::Start((servers::Kind)i, config, L"", &error))
				Error(gtkui::Narrow(error));
		}
	printf("Launcher ready: %zu games\n", games.size());
	fflush(stdout);
}
} // namespace
int main(int argc, char **argv) {
	bool listOnly = false;
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		if (arg == "--list")
			listOnly = true;
		else if (arg == "--help") {
			puts("Usage: pspc-launcher [games directory] [--list]");
			return 0;
		} else if (arg.starts_with("--") || !gamesDir.empty()) {
			fprintf(stderr, "Invalid argument: %s\n", arg.c_str());
			return 2;
		} else
			gamesDir = fs::absolute(arg);
	}
	try {
		exeDir = ExecutableDir();
		resourceDir = apppaths::ResourceRoot(exeDir);
		settings::Load(settings::DefaultPath(), config);
		if (gamesDir.empty()) {
			gamesDir = apppaths::DataPath(config.gamesDir.empty() ? "Games" : config.gamesDir);
			if (config.gamesDir.empty()) fs::create_directories(gamesDir);
		}
		games = frontend::ScanGames(gamesDir);
		if (listOnly) {
			for (const auto &game : games)
				printf("%s\t%s\t%s\n", game.id.c_str(), game.title.c_str(), game.path.c_str());
			return 0;
		}
		if (!gtk_init_check(nullptr, nullptr)) {
			fprintf(stderr, "Cannot open the display; --list works without a display.\n");
			return 1;
		}
		fs::create_directories(apppaths::DataRoot());
		Show();
		gtk_main();
		servers::StopAll();
		build.reset();
		gameJob.reset();
	} catch (const std::exception &error) {
		fprintf(stderr, "%s\n", error.what());
		return 1;
	}
	return 0;
}
