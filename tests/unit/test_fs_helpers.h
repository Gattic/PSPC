// SPDX-License-Identifier: GPL-2.0-or-later
// Helpers for the pspc_fs tests: deterministic data, a minimal ISO9660 image builder, small
// DEFLATE (fixed Huffman, stored) and LZ4 encoders, and CSO/ZSO/DAX image writers.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "common/common.h"
#include "common/fileutil.h"

namespace fstest {

// Same generator as the script that made the zlib vectors in test_fs_decompress.cpp.
struct Lcg {
	u32 s;
	explicit Lcg(u32 seed) : s(seed) {}
	u32 Next() {
		s = s * 1103515245u + 12345u;
		return (s >> 16) & 0x7FFF;
	}
};

inline std::vector<u8> Text(u32 seed, size_t n) {
	static const char *const words[16] = {"the", "quick", "brown", "fox", "PSP", "UMD", "sector", "ISO9660", "memory", "stick",
		"jumps", "over", "lazy", "dog", "0123456789", "EBOOT.BIN"};
	Lcg r(seed);
	std::vector<u8> out;
	while (out.size() < n) {
		const char *w = words[r.Next() % 16];
		out.insert(out.end(), w, w + strlen(w));
		out.push_back(r.Next() % 10 == 0 ? '\n' : ' ');
	}
	out.resize(n);
	return out;
}

inline std::vector<u8> Binary(u32 seed, size_t n) {
	Lcg r(seed);
	std::vector<u8> out;
	while (out.size() < n) {
		u32 k = r.Next() % 8;
		if (k == 0 && !out.empty()) {
			u32 run = 3 + r.Next() % 40;
			u8 b = out.back();
			out.insert(out.end(), run, b);
		} else if (k == 1 && out.size() > 300) {
			size_t d = 1 + r.Next() % 300;
			u32 len = 3 + r.Next() % 60;
			for (u32 i = 0; i < len; i++) {
				out.push_back(out[out.size() - d]);
			}
		} else {
			out.push_back((u8)(r.Next() & 0xFF));
		}
	}
	out.resize(n);
	return out;
}

inline void Put16LE(u8 *p, u16 v) {
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}
inline void Put32LE(u8 *p, u32 v) {
	for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i));
}
inline void Put32BE(u8 *p, u32 v) {
	for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (24 - 8 * i));
}
inline void Put16Both(u8 *p, u16 v) {
	Put16LE(p, v);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}
inline void Put32Both(u8 *p, u32 v) {
	Put32LE(p, v);
	Put32BE(p + 4, v);
}

class TempDir {
public:
	explicit TempDir(const char *tag) {
		static std::atomic<int> counter{0};
		auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = std::filesystem::temp_directory_path() / ("pspc_" + std::string(tag) + "_" + std::to_string(stamp) + "_" + std::to_string(counter++));
		std::filesystem::create_directories(path_);
	}
	~TempDir() {
		std::error_code ec;
		// Clear read-only attributes left by tests so the tree can go.
		for (auto it = std::filesystem::recursive_directory_iterator(path_, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
		}
		// A scanner can still hold a just-closed file, leaving it delete-pending for a moment.
		for (int i = 0; i < 40 && std::filesystem::exists(path_, ec); i++) {
			std::filesystem::remove_all(path_, ec);
			if (std::filesystem::exists(path_, ec)) std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
	}
	const std::filesystem::path &Path() const { return path_; }

private:
	std::filesystem::path path_;
};

// Finds a file of the repository (e.g. "reference/401M33/UPDATE/EBOOT.PBP") by walking up from
// the executable's directory and the current directory. Empty if not found.
inline std::filesystem::path FindRepoFile(const char *rel) {
	std::vector<std::filesystem::path> starts = {ExecutableDir(), std::filesystem::current_path()};
	if (const char *env = getenv("PSPC_REPO_ROOT")) {
		starts.insert(starts.begin(), std::filesystem::path(env));
	}
	for (auto dir : starts) {
		for (int i = 0; i < 8 && !dir.empty(); i++) {
			std::error_code ec;
			if (std::filesystem::exists(dir / rel, ec)) {
				return dir / rel;
			}
			if (dir == dir.parent_path()) {
				break;
			}
			dir = dir.parent_path();
		}
	}
	return {};
}

// ---- ISO9660 builder ---------------------------------------------------------------------------

struct IsoInput {
	std::string path;  // "PSP_GAME/SYSDIR/EBOOT.BIN"; a trailing '/' makes an (empty) directory
	std::vector<u8> data;
};

struct TestIso {
	std::vector<u8> image;
	std::map<std::string, u32> lba;  // file and directory path -> first sector
	std::vector<u8> pathTable;       // type L
	u32 pathTableLba = 0;
};

// Record date used for every entry: 2008-06-28 12:34:56.
constexpr u8 kIsoDate[7] = {108, 6, 28, 12, 34, 56, 0};

inline TestIso BuildIso(const std::vector<IsoInput> &inputs, bool endMidSector = false) {
	struct Dir {
		std::string path, name;
		int parent = -1;
		std::vector<int> subdirs;
		std::vector<int> files;
		u32 lba = 0, sectors = 1;
		u16 number = 0;
	};
	std::vector<Dir> dirs(1);
	auto findOrAddDir = [&](const std::string &path) {
		int cur = 0;
		size_t i = 0;
		while (i < path.size()) {
			size_t j = path.find('/', i);
			if (j == std::string::npos) j = path.size();
			std::string name = path.substr(i, j - i);
			std::string full = path.substr(0, j);
			int found = -1;
			for (int d : dirs[cur].subdirs) {
				if (dirs[d].name == name) found = d;
			}
			if (found < 0) {
				Dir nd;
				nd.path = full;
				nd.name = name;
				nd.parent = cur;
				dirs.push_back(nd);
				found = (int)dirs.size() - 1;
				dirs[cur].subdirs.push_back(found);
			}
			cur = found;
			i = j + 1;
		}
		return cur;
	};
	for (size_t f = 0; f < inputs.size(); f++) {
		const std::string &p = inputs[f].path;
		if (!p.empty() && p.back() == '/') {
			findOrAddDir(p.substr(0, p.size() - 1));
			continue;
		}
		size_t slash = p.rfind('/');
		int d = slash == std::string::npos ? 0 : findOrAddDir(p.substr(0, slash));
		dirs[d].files.push_back((int)f);
	}
	auto fileName = [&](int f) {
		const std::string &p = inputs[f].path;
		size_t slash = p.rfind('/');
		return slash == std::string::npos ? p : p.substr(slash + 1);
	};
	auto recordName = [&](int f) {
		std::string n = fileName(f);
		if (n.find('.') == std::string::npos) n += ".";
		return n + ";1";
	};
	// Path table order: breadth first, children sorted by name.
	for (Dir &d : dirs) {
		std::sort(d.subdirs.begin(), d.subdirs.end(), [&](int a, int b) { return dirs[a].name < dirs[b].name; });
		std::sort(d.files.begin(), d.files.end(), [&](int a, int b) { return fileName(a) < fileName(b); });
	}
	std::vector<int> order = {0};
	for (size_t i = 0; i < order.size(); i++) {
		for (int s : dirs[order[i]].subdirs) order.push_back(s);
	}
	for (size_t i = 0; i < order.size(); i++) dirs[order[i]].number = (u16)(i + 1);

	// Entries of a directory sorted by name, directories and files together: {isDir, index}.
	auto entriesOf = [&](const Dir &d) {
		std::vector<std::pair<bool, int>> e;
		for (int s : d.subdirs) e.push_back({true, s});
		for (int f : d.files) e.push_back({false, f});
		std::sort(e.begin(), e.end(), [&](const auto &a, const auto &b) {
			return (a.first ? dirs[a.second].name : fileName(a.second)) < (b.first ? dirs[b.second].name : fileName(b.second));
		});
		return e;
	};
	// Directory sizes: records never straddle a sector.
	auto recLen = [](size_t nameLen) { return (u32)((33 + nameLen + 1) & ~1u); };
	for (Dir &d : dirs) {
		std::vector<u32> lens = {34, 34};
		for (const auto &e : entriesOf(d)) lens.push_back(recLen(e.first ? dirs[e.second].name.size() : recordName(e.second).size()));
		u32 sectors = 1, used = 0;
		for (u32 l : lens) {
			if (used + l > 2048) {
				sectors++;
				used = 0;
			}
			used += l;
		}
		d.sectors = sectors;
	}
	TestIso out;
	u32 next = 20;  // 16 PVD, 17 terminator, 18 L path table, 19 M path table
	for (int d : order) {
		dirs[d].lba = next;
		next += dirs[d].sectors;
		out.lba[dirs[d].path] = dirs[d].lba;
	}
	std::vector<u32> fileLba(inputs.size(), 0);
	for (int d : order) {
		for (int f : dirs[d].files) {
			fileLba[f] = next;
			next += (u32)((inputs[f].data.size() + 2047) / 2048);
			out.lba[inputs[f].path] = fileLba[f];
		}
	}
	u64 imageBytes = (u64)next * 2048;
	if (endMidSector) {
		// Cut the image right after the last file's data.
		for (size_t f = 0; f < inputs.size(); f++) {
			if (!inputs[f].data.empty() && fileLba[f] + (inputs[f].data.size() + 2047) / 2048 == next) {
				imageBytes = (u64)fileLba[f] * 2048 + inputs[f].data.size();
			}
		}
	}
	out.image.assign((size_t)((u64)next * 2048), 0);
	u8 *img = out.image.data();

	auto writeRecord = [&](u8 *r, u32 lba, u32 size, bool dir, const std::string &name) {
		u32 len = recLen(name.size());
		r[0] = (u8)len;
		Put32Both(r + 2, lba);
		Put32Both(r + 10, size);
		memcpy(r + 18, kIsoDate, 7);
		r[25] = dir ? 2 : 0;
		Put16Both(r + 28, 1);
		r[32] = (u8)name.size();
		memcpy(r + 33, name.data(), name.size());
		return len;
	};
	for (Dir &d : dirs) {
		u8 *base = img + (size_t)d.lba * 2048;
		u32 sector = 0, used = 0;
		auto place = [&](u32 len) {
			if (used + len > 2048) {
				sector++;
				used = 0;
			}
			u8 *r = base + sector * 2048 + used;
			used += len;
			return r;
		};
		const Dir &parent = dirs[d.parent < 0 ? 0 : d.parent];
		writeRecord(place(34), d.lba, d.sectors * 2048, true, std::string(1, '\0'));
		writeRecord(place(34), parent.lba, parent.sectors * 2048, true, std::string(1, '\1'));
		for (const auto &e : entriesOf(d)) {
			if (e.first) {
				const Dir &s = dirs[e.second];
				writeRecord(place(recLen(s.name.size())), s.lba, s.sectors * 2048, true, s.name);
			} else {
				std::string rn = recordName(e.second);
				writeRecord(place(recLen(rn.size())), fileLba[e.second], (u32)inputs[e.second].data.size(), false, rn);
			}
		}
	}
	for (size_t f = 0; f < inputs.size(); f++) {
		if (!inputs[f].data.empty()) {
			memcpy(img + (size_t)fileLba[f] * 2048, inputs[f].data.data(), inputs[f].data.size());
		}
	}
	// Path tables.
	std::vector<u8> lt, mt;
	for (int d : order) {
		std::string name = d == 0 ? std::string(1, '\0') : dirs[d].name;
		u16 parentNum = d == 0 ? 1 : dirs[dirs[d].parent].number;
		size_t at = lt.size();
		lt.resize(at + 8 + name.size() + (name.size() & 1));
		mt.resize(lt.size());
		lt[at] = mt[at] = (u8)name.size();
		Put32LE(&lt[at + 2], dirs[d].lba);
		Put32BE(&mt[at + 2], dirs[d].lba);
		Put16LE(&lt[at + 6], parentNum);
		mt[at + 6] = (u8)(parentNum >> 8);
		mt[at + 7] = (u8)parentNum;
		memcpy(&lt[at + 8], name.data(), name.size());
		memcpy(&mt[at + 8], name.data(), name.size());
	}
	memcpy(img + 18 * 2048, lt.data(), lt.size());
	memcpy(img + 19 * 2048, mt.data(), mt.size());
	out.pathTable = lt;
	out.pathTableLba = 18;
	// Primary volume descriptor and terminator.
	u8 *pvd = img + 16 * 2048;
	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	memset(pvd + 8, ' ', 32);
	memcpy(pvd + 8, "PSP GAME", 8);
	memset(pvd + 40, ' ', 32);
	memcpy(pvd + 40, "PSPC_TEST", 9);
	Put32Both(pvd + 80, next);
	Put16Both(pvd + 120, 1);
	Put16Both(pvd + 124, 1);
	Put16Both(pvd + 128, 2048);
	Put32Both(pvd + 132, (u32)lt.size());
	Put32LE(pvd + 140, 18);
	Put32BE(pvd + 148, 19);
	writeRecord(pvd + 156, dirs[0].lba, dirs[0].sectors * 2048, true, std::string(1, '\0'));
	pvd[881] = 1;
	u8 *term = img + 17 * 2048;
	term[0] = 255;
	memcpy(term + 1, "CD001", 5);
	term[6] = 1;
	out.image.resize((size_t)imageBytes);
	return out;
}

// ---- Encoders ----------------------------------------------------------------------------------

class BitWriter {
public:
	void Put(u32 v, int n) {
		buf_ |= (u64)v << cnt_;
		cnt_ += n;
		while (cnt_ >= 8) {
			out.push_back((u8)buf_);
			buf_ >>= 8;
			cnt_ -= 8;
		}
	}
	// Huffman codes go MSB first.
	void PutCode(u32 code, int len) {
		for (int i = len - 1; i >= 0; i--) Put((code >> i) & 1, 1);
	}
	void AlignByte() {
		if (cnt_) Put(0, 8 - cnt_);
	}
	std::vector<u8> out;

private:
	u64 buf_ = 0;
	int cnt_ = 0;
};

inline void PutFixedLit(BitWriter &w, int sym) {
	if (sym < 144) w.PutCode(0x30 + sym, 8);
	else if (sym < 256) w.PutCode(0x190 + (sym - 144), 9);
	else if (sym < 280) w.PutCode(sym - 256, 7);
	else w.PutCode(0xC0 + (sym - 280), 8);
}

inline void PutFixedMatch(BitWriter &w, u32 len, u32 dist) {
	static const u16 lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const u8 lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const u16 dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const u8 dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	int li = 28;
	while (lbase[li] > len) li--;
	PutFixedLit(w, 257 + li);
	w.Put(len - lbase[li], lext[li]);
	int di = 29;
	while (dbase[di] > dist) di--;
	w.PutCode(di, 5);
	w.Put(dist - dbase[di], dext[di]);
}

// DEFLATE with greedy LZ77. Blocks of `blockSize` input bytes alternate between fixed Huffman and
// stored when `mixStored` is set; matches may reach back into earlier blocks.
inline std::vector<u8> Deflate(const u8 *src, size_t n, bool mixStored = false, size_t blockSize = 1500) {
	BitWriter w;
	std::vector<int> head(1 << 15, -1);
	auto hash = [&](size_t i) { return ((src[i] << 10) ^ (src[i + 1] << 5) ^ src[i + 2]) & 0x7FFF; };
	size_t pos = 0;
	int block = 0;
	do {
		size_t end = std::min(n, pos + blockSize);
		bool last = end == n;
		bool stored = mixStored && (block % 2 == 1);
		w.Put(last ? 1 : 0, 1);
		if (stored) {
			w.Put(0, 2);
			w.AlignByte();
			u16 len = (u16)(end - pos);
			w.out.push_back((u8)len);
			w.out.push_back((u8)(len >> 8));
			w.out.push_back((u8)~len);
			w.out.push_back((u8)(~len >> 8));
			w.out.insert(w.out.end(), src + pos, src + end);
			for (size_t i = pos; i < end && i + 3 <= n; i++) head[hash(i)] = (int)i;
			pos = end;
		} else {
			w.Put(1, 2);
			size_t i = pos;
			while (i < end) {
				u32 best = 0, dist = 0;
				if (i + 3 <= n) {
					int cand = head[hash(i)];
					head[hash(i)] = (int)i;
					if (cand >= 0 && i - cand <= 32768) {
						u32 len = 0;
						while (len < 258 && i + len < end && src[cand + len] == src[i + len]) len++;
						if (len >= 3) {
							best = len;
							dist = (u32)(i - cand);
						}
					}
				}
				if (best) {
					PutFixedMatch(w, best, dist);
					for (size_t k = i + 1; k < i + best && k + 3 <= n; k++) head[hash(k)] = (int)k;
					i += best;
				} else {
					PutFixedLit(w, src[i]);
					i++;
				}
			}
			PutFixedLit(w, 256);
			pos = end;
		}
		block++;
	} while (pos < n);
	w.AlignByte();
	return w.out;
}

inline std::vector<u8> ZlibWrap(const std::vector<u8> &deflated, const u8 *src, size_t n) {
	std::vector<u8> out = {0x78, 0x01};
	out.insert(out.end(), deflated.begin(), deflated.end());
	u32 a = 1, b = 0;
	for (size_t i = 0; i < n; i++) {
		a = (a + src[i]) % 65521;
		b = (b + a) % 65521;
	}
	u32 adler = (b << 16) | a;
	for (int i = 3; i >= 0; i--) out.push_back((u8)(adler >> (8 * i)));
	return out;
}

inline std::vector<u8> Lz4Compress(const u8 *src, size_t n) {
	std::vector<u8> out;
	std::vector<int> table(4096, -1);
	auto hash = [&](size_t i) {
		u32 v;
		memcpy(&v, src + i, 4);
		return (v * 2654435761u) >> 20;
	};
	auto putLen = [&](size_t len) {
		while (len >= 255) {
			out.push_back(255);
			len -= 255;
		}
		out.push_back((u8)len);
	};
	size_t anchor = 0, i = 0;
	const size_t limit = n >= 12 ? n - 12 : 0;
	while (i < limit) {
		u32 h = hash(i);
		int cand = table[h];
		table[h] = (int)i;
		if (cand >= 0 && i - cand <= 65535 && !memcmp(src + cand, src + i, 4)) {
			size_t len = 4;
			while (i + len < n - 5 && src[cand + len] == src[i + len]) len++;
			size_t lit = i - anchor;
			out.push_back((u8)((std::min<size_t>(lit, 15) << 4) | std::min<size_t>(len - 4, 15)));
			if (lit >= 15) putLen(lit - 15);
			out.insert(out.end(), src + anchor, src + i);
			u16 off = (u16)(i - cand);
			out.push_back((u8)off);
			out.push_back((u8)(off >> 8));
			if (len - 4 >= 15) putLen(len - 4 - 15);
			i += len;
			anchor = i;
		} else {
			i++;
		}
	}
	size_t lit = n - anchor;
	out.push_back((u8)(std::min<size_t>(lit, 15) << 4));
	if (lit >= 15) putLen(lit - 15);
	out.insert(out.end(), src + anchor, src + n);
	return out;
}

// ---- Image writers -----------------------------------------------------------------------------

enum class CsoFlavor { V1, V2, Zso };

// CSO family writer. Frame methods rotate so every code path of the reader gets used: v1 and ZSO
// store every fifth frame raw; v2 cycles deflate, LZ4 and raw.
inline std::vector<u8> MakeCso(const std::vector<u8> &iso, CsoFlavor flavor, u32 frameSize, u32 shift) {
	const u64 total = iso.size();
	const u32 frames = (u32)((total + frameSize - 1) / frameSize);
	std::vector<u8> out(24 + ((size_t)frames + 1) * 4, 0);
	memcpy(out.data(), flavor == CsoFlavor::Zso ? "ZISO" : "CISO", 4);
	Put32LE(&out[4], 0x18);
	Put32LE(&out[8], (u32)total);
	Put32LE(&out[12], (u32)(total >> 32));
	Put32LE(&out[16], frameSize);
	out[20] = flavor == CsoFlavor::V2 ? 2 : 1;
	out[21] = (u8)shift;
	const u32 align = 1u << shift;
	auto pad = [&]() {
		while (out.size() % align) out.push_back(0);
	};
	pad();
	std::vector<u32> index(frames + 1);
	for (u32 f = 0; f < frames; f++) {
		const u8 *chunk = iso.data() + (size_t)f * frameSize;
		const size_t len = (size_t)std::min<u64>(frameSize, total - (u64)f * frameSize);
		const bool full = len == frameSize;
		const bool lastFrame = f + 1 == frames;
		bool plain = false, flag = false;
		std::vector<u8> comp;
		if (flavor == CsoFlavor::V2) {
			int m = f % 3;
			if (m == 2 && full && !lastFrame) {
				plain = true;
			} else if (m == 1) {
				comp = Lz4Compress(chunk, len);
				flag = true;
			} else {
				comp = Deflate(chunk, len, f % 2 == 0);
			}
			if (!plain && AlignUp((u32)comp.size(), align) >= frameSize) {
				plain = true;
				flag = false;
			}
		} else {
			if (f % 5 != 3) {
				comp = flavor == CsoFlavor::Zso ? Lz4Compress(chunk, len) : Deflate(chunk, len, f % 2 == 0);
			}
			plain = f % 5 == 3 || comp.size() >= len;
			flag = plain;
		}
		index[f] = (u32)(out.size() >> shift) | (flag ? 0x80000000u : 0);
		if (plain) {
			out.insert(out.end(), chunk, chunk + len);
		} else {
			out.insert(out.end(), comp.begin(), comp.end());
		}
		pad();
	}
	index[frames] = (u32)(out.size() >> shift);
	for (u32 f = 0; f <= frames; f++) Put32LE(&out[24 + (size_t)f * 4], index[f]);
	return out;
}

// DAX writer: 8 KiB frames, frames 1 and 2 in an uncompressed (NC) area, the rest zlib-wrapped
// or (every third) raw deflate.
inline std::vector<u8> MakeDax(const std::vector<u8> &iso) {
	const u32 fs = 0x2000;
	const u32 total = (u32)iso.size();
	const u32 frames = (total + fs - 1) / fs;
	const bool nc = frames > 3;
	std::vector<u8> out(32 + (size_t)frames * 6 + (nc ? 8 : 0), 0);
	memcpy(out.data(), "DAX\0", 4);
	Put32LE(&out[4], total);
	Put32LE(&out[8], 1);
	Put32LE(&out[12], nc ? 1 : 0);
	if (nc) {
		Put32LE(&out[32 + (size_t)frames * 6], 1);
		Put32LE(&out[32 + (size_t)frames * 6 + 4], 2);
	}
	for (u32 f = 0; f < frames; f++) {
		const u8 *chunk = iso.data() + (size_t)f * fs;
		const size_t len = std::min<size_t>(fs, total - (size_t)f * fs);
		std::vector<u8> data;
		if (nc && (f == 1 || f == 2)) {
			data.assign(chunk, chunk + len);
		} else {
			data = Deflate(chunk, len, f % 2 == 1);
			if (f % 3 != 0) data = ZlibWrap(data, chunk, len);
		}
		Put32LE(&out[32 + (size_t)f * 4], (u32)out.size());
		Put16LE(&out[32 + (size_t)frames * 4 + (size_t)f * 2], (u16)data.size());
		out.insert(out.end(), data.begin(), data.end());
	}
	return out;
}

}  // namespace fstest
