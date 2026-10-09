// SPDX-License-Identifier: GPL-3.0-or-later
// pspc-decrypt: turns encrypted/packed PSP executables and firmware updates into plain ELF/PRX.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version. See COPYING.
//
// It is a separate executable on purpose: libkirk is GPL-3.0-or-later, so PSPC's runtime invokes
// this as a subprocess and never links it.

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common.h"
#include "formats.h"
#include "json.h"
#include "module.h"
#include "psar.h"

namespace {

const char *kVersion = "pspc-decrypt 1.0";

enum ExitCode {
	EXIT_OK = 0,
	EXIT_USAGE = 1,
	EXIT_FAILED = 2,
	EXIT_NEEDS_KEY = 3,
};

const char *kUsage =
	"Usage: pspc-decrypt <command> [options] <args>\n"
	"\n"
	"Commands:\n"
	"  info <file>                  Describe an ELF/PRX, ~PSP module, PBP, PSAR, EDAT or SFO.\n"
	"  prx <in> <out>               Decrypt/unpack/decompress one executable to a plain ELF.\n"
	"                               <in>: ~PSP (KIRK or CFW-packed), ~SCE, PSPEDAT .sprx, a PBP\n"
	"                               (uses DATA.PSP), gzip/KL4E/KL3E/2RLZ, or a plain ELF (copied).\n"
	"  pbp <in.pbp> <outdir>        Extract every PBP section; DATA.PSP also as DATA.ELF.\n"
	"  psar <updater> <outdir>      Unpack a firmware updater (EBOOT.PBP, DATA.PSAR or DATA.BIN)\n"
	"                               into flash0/, flash1/, ipl/ with modules decrypted, plus\n"
	"                               manifest.json.\n"
	"  carve <file> <outdir>        Find ~PSP modules embedded in any file and decode each one.\n"
	"  batch <listfile>             One \"<in>\\t<out>\" per line, each done like prx.\n"
	"\n"
	"Options:\n"
	"  --json                 Print results as JSON on stdout.\n"
	"  --seed <32 hex>        NPDRM seed (version key) for type 5 modules that need one.\n"
	"  --licensee-key <32 hex>  Licensee key for PSPEDAT modules whose header asks for it.\n"
	"  --model <any|01g..12g> psar: resolve names against one model's file lists (default any).\n"
	"  --keep-encrypted       psar/carve: also keep the original encrypted modules.\n"
	"  -v, --verbose          psar: one line per entry on stderr.\n"
	"  -q, --quiet            No progress text on stdout.\n"
	"\n"
	"Exit codes: 0 success, 1 usage error, 2 failure, 3 needs a key we don't have.\n";

struct Args {
	std::string command;
	std::vector<std::string> positional;
	bool json = false;
	bool keepEncrypted = false;
	bool verbose = false;
	bool quiet = false;
	std::string model = "any";
	bool haveSeed = false;
	u8 seed[16] = {};
	bool haveLicensee = false;
	u8 licensee[16] = {};

	DecodeOptions Decode() const {
		DecodeOptions o;
		o.seed = haveSeed ? seed : nullptr;
		o.licenseeKey = haveLicensee ? licensee : nullptr;
		return o;
	}
};

int Usage(const std::string &message) {
	if (!message.empty()) {
		fprintf(stderr, "pspc-decrypt: %s\n\n", message.c_str());
	}
	fputs(kUsage, stderr);
	return EXIT_USAGE;
}

bool ParseArgs(const std::vector<std::string> &argv, Args *args, std::string *error) {
	for (size_t i = 0; i < argv.size(); i++) {
		const std::string &a = argv[i];
		auto value = [&](std::string *out) -> bool {
			if (i + 1 >= argv.size()) {
				*error = a + " needs a value";
				return false;
			}
			*out = argv[++i];
			return true;
		};
		std::string v;
		if (a == "--help" || a == "-h") {
			args->command = "help";
		} else if (a == "--json") {
			args->json = true;
		} else if (a == "--keep-encrypted") {
			args->keepEncrypted = true;
		} else if (a == "-v" || a == "--verbose") {
			args->verbose = true;
		} else if (a == "-q" || a == "--quiet") {
			args->quiet = true;
		} else if (a == "--model") {
			if (!value(&args->model)) return false;
		} else if (a == "--seed") {
			if (!value(&v)) return false;
			if (!ParseHex(v, args->seed, 16)) {
				*error = "--seed wants 32 hex digits";
				return false;
			}
			args->haveSeed = true;
		} else if (a == "--licensee-key") {
			if (!value(&v)) return false;
			if (!ParseHex(v, args->licensee, 16)) {
				*error = "--licensee-key wants 32 hex digits";
				return false;
			}
			args->haveLicensee = true;
		} else if (a.size() > 1 && a[0] == '-' && a != "-") {
			*error = "unknown option " + a;
			return false;
		} else if (args->command.empty()) {
			args->command = a;
		} else {
			args->positional.push_back(a);
		}
	}
	return true;
}

void PrintJson(const Json &j) {
	const std::string text = j.Dump(2) + "\n";
	fwrite(text.data(), 1, text.size(), stdout);
}

int StatusExit(DecodeStatus s) {
	return s == DecodeStatus::Ok ? EXIT_OK : s == DecodeStatus::NeedsKey ? EXIT_NEEDS_KEY : EXIT_FAILED;
}

const char *StatusName(DecodeStatus s) {
	return s == DecodeStatus::Ok ? "ok" : s == DecodeStatus::NeedsKey ? "needs-key" : "failed";
}

Json ResultJson(const DecodeResult &r, const std::string &in, const std::string &out) {
	Json j = Json::Object();
	j.Set("input", in);
	j.Set("output", out);
	j.Set("status", StatusName(r.status));
	if (!r.error.empty()) {
		j.Set("error", r.error);
	}
	Json steps = Json::Array();
	for (const std::string &s : r.steps) {
		steps.Push(s);
	}
	j.Set("steps", steps);
	if (r.hadPspHeader) {
		j.Set("module_name", r.psp.modname);
		j.Set("tag", Hex32(r.psp.tag));
	}
	if (r.status == DecodeStatus::Ok) {
		j.Set("encrypted", r.encrypted);
		j.Set("cfw_packed", r.cfwPacked);
		if (r.prxType >= 0) {
			j.Set("prx_type", r.prxType);
		}
		j.Set("compression", r.compression);
		j.Set("size", (unsigned long long)r.output.size());
		j.Set("sha1", Sha1Hex(r.output.data(), r.output.size()));
		if (IsElf(r.output.data(), r.output.size())) {
			j.Set("elf_type", Hex32(ElfType(r.output.data(), r.output.size())));
		}
	}
	if (!r.warnings.empty()) {
		Json w = Json::Array();
		for (const std::string &s : r.warnings) {
			w.Push(s);
		}
		j.Set("warnings", w);
	}
	return j;
}

std::string ResultLine(const DecodeResult &r, const std::string &in, const std::string &out) {
	std::string line = in + " -> " + out + ": ";
	if (r.status != DecodeStatus::Ok) {
		return line + StatusName(r.status) + ": " + r.error;
	}
	for (size_t i = 0; i < r.steps.size(); i++) {
		line += (i ? ", " : "") + r.steps[i];
	}
	line += StringPrintf(" -> %zu bytes", r.output.size());
	for (const std::string &w : r.warnings) {
		line += " [warning: " + w + "]";
	}
	return line;
}

// Decodes one file to one output. Shared by prx and batch.
DecodeResult DecodeOne(const Args &args, const std::string &in, const std::string &out, std::string *ioError) {
	DecodeResult r;
	Bytes data;
	if (!ReadFileBytes(in, &data, ioError)) {
		r.error = *ioError;
		return r;
	}
	DecodeExecutable(data.data(), data.size(), args.Decode(), &r);
	if (r.status == DecodeStatus::Ok && !WriteFileBytes(out, r.output, ioError)) {
		r.status = DecodeStatus::Failed;
		r.error = *ioError;
	}
	return r;
}

int CmdInfo(const Args &args) {
	if (args.positional.size() != 1) {
		return Usage("info takes one file");
	}
	Bytes data;
	std::string error;
	if (!ReadFileBytes(args.positional[0], &data, &error)) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	const Json out = DescribeFile(data.data(), data.size(), args.Decode());
	if (args.json) {
		Json top = Json::Object();
		top.Set("file", args.positional[0]);
		top.Set("info", out);
		PrintJson(top);
	} else {
		const std::string text = "file: " + args.positional[0] + "\n" + out.Text();
		fwrite(text.data(), 1, text.size(), stdout);
	}
	return EXIT_OK;
}

int CmdPrx(const Args &args) {
	if (args.positional.size() != 2) {
		return Usage("prx takes <in> <out>");
	}
	std::string error;
	const DecodeResult r = DecodeOne(args, args.positional[0], args.positional[1], &error);
	if (args.json) {
		PrintJson(ResultJson(r, args.positional[0], args.positional[1]));
	} else if (r.status == DecodeStatus::Ok) {
		if (!args.quiet) {
			printf("%s\n", ResultLine(r, args.positional[0], args.positional[1]).c_str());
		}
	} else {
		fprintf(stderr, "pspc-decrypt: %s\n", ResultLine(r, args.positional[0], args.positional[1]).c_str());
	}
	return StatusExit(r.status);
}

int CmdBatch(const Args &args) {
	if (args.positional.size() != 1) {
		return Usage("batch takes one list file");
	}
	Bytes list;
	std::string error;
	if (!ReadFileBytes(args.positional[0], &list, &error)) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	Json results = Json::Array();
	int ok = 0, failed = 0, needsKey = 0, lineNo = 0;
	size_t pos = 0;
	const std::string text(list.begin(), list.end());
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		lineNo++;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (lineNo == 1 && line.size() >= 3 && (u8)line[0] == 0xEF && (u8)line[1] == 0xBB && (u8)line[2] == 0xBF) {
			line.erase(0, 3);  // UTF-8 BOM
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const size_t tab = line.find('\t');
		if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
			failed++;
			const std::string msg = StringPrintf("line %d: expected \"<in>\\t<out>\"", lineNo);
			if (args.json) {
				Json j = Json::Object();
				j.Set("line", lineNo);
				j.Set("status", "failed");
				j.Set("error", msg);
				results.Push(j);
			} else {
				fprintf(stderr, "pspc-decrypt: %s\n", msg.c_str());
			}
			continue;
		}
		const std::string in = line.substr(0, tab), out = line.substr(tab + 1);
		std::string ioError;
		const DecodeResult r = DecodeOne(args, in, out, &ioError);
		if (r.status == DecodeStatus::Ok) {
			ok++;
		} else if (r.status == DecodeStatus::NeedsKey) {
			needsKey++;
		} else {
			failed++;
		}
		if (args.json) {
			Json j = ResultJson(r, in, out);
			j.Set("line", lineNo);
			results.Push(j);
		} else if (r.status == DecodeStatus::Ok) {
			if (!args.quiet) {
				printf("%s\n", ResultLine(r, in, out).c_str());
			}
		} else {
			fprintf(stderr, "pspc-decrypt: %s\n", ResultLine(r, in, out).c_str());
		}
	}
	if (args.json) {
		Json j = Json::Object();
		j.Set("ok", ok);
		j.Set("failed", failed);
		j.Set("needs_key", needsKey);
		j.Set("results", results);
		PrintJson(j);
	} else if (!args.quiet) {
		printf("batch: %d ok, %d failed, %d need a key\n", ok, failed, needsKey);
	}
	return failed ? EXIT_FAILED : needsKey ? EXIT_NEEDS_KEY : EXIT_OK;
}

int CmdPbp(const Args &args) {
	if (args.positional.size() != 2) {
		return Usage("pbp takes <in.pbp> <outdir>");
	}
	Bytes data;
	std::string error;
	if (!ReadFileBytes(args.positional[0], &data, &error)) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	PbpFile pbp;
	if (!pbp.Parse(data.data(), data.size(), &error)) {
		fprintf(stderr, "pspc-decrypt: %s: %s\n", args.positional[0].c_str(), error.c_str());
		return EXIT_FAILED;
	}
	const std::string &outDir = args.positional[1];
	Json files = Json::Array();
	int exitCode = EXIT_OK;
	for (int i = 0; i < PbpFile::kSections; i++) {
		if (pbp.size[i] == 0) {
			continue;
		}
		const u8 *section = data.data() + pbp.offset[i];
		Json f = Json::Object();
		f.Set("name", PbpFile::kNames[i]);
		f.Set("size", pbp.size[i]);
		f.Set("kind", FileKindName(SniffKind(section, pbp.size[i])));
		f.Set("sha1", Sha1Hex(section, pbp.size[i]));
		if (!WriteFileBytes(JoinPath(outDir, PbpFile::kNames[i]), section, pbp.size[i], &error)) {
			fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
			return EXIT_FAILED;
		}
		files.Push(f);
	}

	Json elf;
	const u8 *psp = data.data() + pbp.offset[PbpFile::DATA_PSP];
	const u32 pspSize = pbp.size[PbpFile::DATA_PSP];
	const FileKind kind = SniffKind(psp, pspSize);
	if (pspSize && (kind == FileKind::PspModule || kind == FileKind::SceWrapped || kind == FileKind::Edat)) {
		DecodeResult r;
		DecodeExecutable(psp, pspSize, args.Decode(), &r);
		const std::string out = JoinPath(outDir, "DATA.ELF");
		if (r.status == DecodeStatus::Ok && !WriteFileBytes(out, r.output, &error)) {
			r.status = DecodeStatus::Failed;
			r.error = error;
		}
		elf = ResultJson(r, "DATA.PSP", "DATA.ELF");
		exitCode = StatusExit(r.status);
		if (!args.json) {
			if (r.status == DecodeStatus::Ok) {
				if (!args.quiet) {
					printf("%s\n", ResultLine(r, "DATA.PSP", "DATA.ELF").c_str());
				}
			} else {
				fprintf(stderr, "pspc-decrypt: %s\n", ResultLine(r, "DATA.PSP", "DATA.ELF").c_str());
			}
		}
	}
	if (args.json) {
		Json j = Json::Object();
		j.Set("input", args.positional[0]);
		j.Set("output_dir", outDir);
		j.Set("sections", files);
		if (!elf.IsNull()) {
			j.Set("data_elf", elf);
		}
		PrintJson(j);
	} else if (!args.quiet) {
		for (size_t i = 0; i < files.Size(); i++) {
			const Json &f = files.At(i);
			printf("%s: %lld bytes (%s)\n", f.Get("name")->AsString().c_str(), f.Get("size")->AsInt(), f.Get("kind")->AsString().c_str());
		}
	}
	return exitCode;
}

int CmdPsar(const Args &args) {
	if (args.positional.size() != 2) {
		return Usage("psar takes <updater> <outdir>");
	}
	Bytes data;
	std::string error;
	if (!ReadFileBytes(args.positional[0], &data, &error)) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	int generation = 0;
	if (!PsarParseModel(args.model, &generation)) {
		return Usage("bad --model '" + args.model + "' (any, or 01g..12g)");
	}
	const u8 *psar = nullptr;
	size_t psarSize = 0;
	if (!LocatePsar(data.data(), data.size(), &psar, &psarSize, &error)) {
		fprintf(stderr, "pspc-decrypt: %s: %s\n", args.positional[0].c_str(), error.c_str());
		return EXIT_FAILED;
	}
	PsarOptions options;
	options.model = args.model;
	options.keepEncrypted = args.keepEncrypted;
	options.verbose = args.verbose;
	options.decode = args.Decode();
	Json manifest;
	PsarSummary s;
	const bool ok = UnpackPsar(psar, psarSize, args.positional[1], options, &manifest, &s, &error);
	if (!ok) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	const bool complete = manifest.Get("complete") && manifest.Get("complete")->AsBool();
	if (args.json) {
		Json j = Json::Object();
		j.Set("input", args.positional[0]);
		j.Set("output_dir", args.positional[1]);
		j.Set("manifest", JoinPath(args.positional[1], "manifest.json"));
		j.Set("firmware_version", s.firmwareVersion);
		j.Set("complete", complete);
		if (!complete) {
			j.Set("error", error);
		}
		j.Set("summary", *manifest.Get("summary"));
		PrintJson(j);
	} else {
		if (!args.quiet) {
			printf("firmware %s (PSAR version %d): %d entries, %d files, %d directories, %d file lists\n",
				s.firmwareVersion.c_str(), s.psarVersion, s.entries, s.files, s.directories, s.fileLists);
			printf("modules: %d, decrypted %d, need a key %d; failed entries %d, unnamed %d, other model %d, conflicts %d, derived %d\n",
				s.modules, s.modulesDecrypted, s.needsKey, s.failed, s.unnamed, s.otherModel, s.conflicts, s.derived);
			printf("manifest: %s\n", JoinPath(args.positional[1], "manifest.json").c_str());
		}
		if (!complete) {
			fprintf(stderr, "pspc-decrypt: archive walk stopped early: %s\n", error.c_str());
		}
	}
	if (!complete || s.failed > 0) {
		return EXIT_FAILED;
	}
	return s.needsKey > 0 ? EXIT_NEEDS_KEY : EXIT_OK;
}

std::string SafeName(const std::string &name) {
	std::string s;
	for (char c : name) {
		s.push_back((u8)c < 0x20 || strchr("<>:\"/\\|?* ", c) ? '_' : c);
	}
	return s.empty() ? "module" : s;
}

int CmdCarve(const Args &args) {
	if (args.positional.size() != 2) {
		return Usage("carve takes <file> <outdir>");
	}
	Bytes data;
	std::string error;
	if (!ReadFileBytes(args.positional[0], &data, &error)) {
		fprintf(stderr, "pspc-decrypt: %s\n", error.c_str());
		return EXIT_FAILED;
	}
	const std::vector<size_t> found = FindEmbeddedModules(data.data(), data.size());
	Json results = Json::Array();
	int ok = 0, failed = 0, needsKey = 0;
	for (size_t i = 0; i < found.size(); i++) {
		const size_t off = found[i];
		PspHeader h;
		ParsePspHeader(data.data() + off, data.size() - off, &h);
		const std::string base = StringPrintf("%02zu_", i) + SafeName(h.modname);
		const std::string out = JoinPath(args.positional[1], base + ".prx");
		DecodeResult r;
		// Hand the decoder everything up to the end of the file: the hash checks may read past
		// psp_size, and the decoder itself stops at psp_size.
		DecodeExecutable(data.data() + off, data.size() - off, args.Decode(), &r);
		if (r.status == DecodeStatus::Ok && !WriteFileBytes(out, r.output, &error)) {
			r.status = DecodeStatus::Failed;
			r.error = error;
		}
		if (args.keepEncrypted) {
			WriteFileBytes(JoinPath(args.positional[1], base + ".psp"), data.data() + off, h.pspSize, &error);
		}
		r.status == DecodeStatus::Ok ? ok++ : r.status == DecodeStatus::NeedsKey ? needsKey++ : failed++;
		Json j = ResultJson(r, StringPrintf("%s@0x%zx", args.positional[0].c_str(), off), out);
		j.Set("offset", (unsigned long long)off);
		results.Push(j);
		if (!args.json) {
			if (r.status == DecodeStatus::Ok) {
				if (!args.quiet) {
					printf("0x%08zx %s\n", off, ResultLine(r, h.modname, out).c_str());
				}
			} else {
				fprintf(stderr, "pspc-decrypt: 0x%08zx %s\n", off, ResultLine(r, h.modname, out).c_str());
			}
		}
	}
	if (args.json) {
		Json j = Json::Object();
		j.Set("input", args.positional[0]);
		j.Set("found", (unsigned long long)found.size());
		j.Set("ok", ok);
		j.Set("failed", failed);
		j.Set("needs_key", needsKey);
		j.Set("modules", results);
		PrintJson(j);
	} else if (!args.quiet) {
		printf("carve: %zu modules found, %d ok, %d failed, %d need a key\n", found.size(), ok, failed, needsKey);
	}
	if (found.empty()) {
		if (!args.json) {
			fprintf(stderr, "pspc-decrypt: no embedded ~PSP modules in %s\n", args.positional[0].c_str());
		}
		return EXIT_FAILED;
	}
	return failed ? EXIT_FAILED : needsKey ? EXIT_NEEDS_KEY : EXIT_OK;
}

int Run(const std::vector<std::string> &argv) {
	Args args;
	std::string error;
	if (!ParseArgs(argv, &args, &error)) {
		return Usage(error);
	}
	if (args.command.empty() || args.command == "help" || args.command == "--help" || args.command == "-h") {
		if (args.command.empty()) {
			return Usage(std::string());
		}
		fputs(kUsage, stdout);
		return EXIT_OK;
	}
	if (args.command == "version") {
		printf("%s\n", kVersion);
		return EXIT_OK;
	}
	static const std::map<std::string, int (*)(const Args &)> commands = {
		{ "info", CmdInfo },
		{ "prx", CmdPrx },
		{ "pbp", CmdPbp },
		{ "psar", CmdPsar },
		{ "carve", CmdCarve },
		{ "batch", CmdBatch },
	};
	const auto it = commands.find(args.command);
	if (it == commands.end()) {
		return Usage("unknown command '" + args.command + "'");
	}
	return it->second(args);
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		const int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s(len > 0 ? len - 1 : 0, '\0');
		if (len > 1) {
			WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), len, nullptr, nullptr);
		}
		args.push_back(s);
	}
	// Paths we print are UTF-8.
	SetConsoleOutputCP(CP_UTF8);
	return Run(args);
}
#else
int main(int argc, char **argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	return Run(args);
}
#endif
