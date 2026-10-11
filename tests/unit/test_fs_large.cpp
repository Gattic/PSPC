// SPDX-License-Identifier: GPL-2.0-or-later
// Disc images of several GB (docs/large-iso-plan.md): files past 2, 4 and 8 GB, a 4 GB - 1 file,
// seeks deep into it, umd0: sectors and /sce_lbn extents there, and CSO/ZSO indexes reaching past
// 4 GB. The images live in a sparse in-memory source, so nothing is allocated or written to disk.
#include <cstdio>
#include <cstring>
#include <map>

#include "fs/block_device.h"
#include "fs/iso_fs.h"
#include "test_fs_helpers.h"
#include "testing.h"

using namespace vfs;

namespace {

constexpr u64 kGB = 1ull << 30;

// A ByteSource of a declared size that reads as zeros except where data was put.
class SparseSource : public ByteSource {
public:
	explicit SparseSource(u64 size) : size_(size) {}
	void Put(u64 offset, std::vector<u8> data) {
		CHECK(offset + data.size() <= size_);
		patches_[offset] = std::move(data);
	}
	u64 Size() const override { return size_; }
	size_t ReadAt(u64 offset, void *out, size_t size) override {
		if (offset >= size_) return 0;
		const size_t n = (size_t)std::min<u64>(size, size_ - offset);
		memset(out, 0, n);
		auto it = patches_.upper_bound(offset);
		if (it != patches_.begin()) --it;
		for (; it != patches_.end() && it->first < offset + n; ++it) {
			const u64 from = std::max(it->first, offset);
			const u64 to = std::min(it->first + it->second.size(), offset + n);
			if (from < to) memcpy((u8 *)out + (from - offset), it->second.data() + (from - it->first), (size_t)(to - from));
		}
		return n;
	}

private:
	u64 size_;
	std::map<u64, std::vector<u8>> patches_;
};

// "NAME@0xOFFSET" padded with '.' to 64 bytes: what a file holds at OFFSET.
std::vector<u8> Marker(const std::string &name, u64 off) {
	char buf[65];
	snprintf(buf, sizeof(buf), "%s@0x%llx", name.c_str(), (unsigned long long)off);
	std::vector<u8> m(64, '.');
	memcpy(m.data(), buf, strlen(buf));
	return m;
}

struct BigFile {
	std::string name;  // "NAME.BIN"
	u64 at;            // byte offset in the image (sector aligned)
	u32 size;
	std::vector<u64> marks;  // offsets in the file that get a Marker
	u8 flags = 0;            // 0x80 = more extents follow
};

// A flat ISO 9660 image: PVD at 16, root directory at 18, the files where they say.
std::unique_ptr<SparseSource> MakeBigIso(u64 imageBytes, const std::vector<BigFile> &files) {
	auto src = std::make_unique<SparseSource>(imageBytes);
	std::vector<u8> dir(2048, 0);
	size_t used = 0;
	auto record = [&](const std::string &name, u32 lba, u32 size, u8 flags) {
		const u32 len = (u32)((33 + name.size() + 1) & ~(size_t)1);
		u8 *r = dir.data() + used;
		r[0] = (u8)len;
		fstest::Put32Both(r + 2, lba);
		fstest::Put32Both(r + 10, size);
		memcpy(r + 18, fstest::kIsoDate, 7);
		r[25] = flags;
		fstest::Put16Both(r + 28, 1);
		r[32] = (u8)name.size();
		memcpy(r + 33, name.data(), name.size());
		used += len;
	};
	record(std::string(1, '\0'), 18, 2048, 2);
	record(std::string(1, '\1'), 18, 2048, 2);
	for (const BigFile &f : files) {
		record(f.name + ";1", (u32)(f.at / 2048), f.size, f.flags);
		for (u64 m : f.marks) src->Put(f.at + m, Marker(f.name, m));
	}
	CHECK(used <= 2048);
	std::vector<u8> pvd(2048, 0);
	pvd[0] = 1;
	memcpy(&pvd[1], "CD001", 5);
	pvd[6] = 1;
	memset(&pvd[8], ' ', 64);
	memcpy(&pvd[40], "BIGTEST", 7);
	fstest::Put32Both(&pvd[80], (u32)((imageBytes + 2047) / 2048));
	fstest::Put16Both(&pvd[120], 1);
	fstest::Put16Both(&pvd[124], 1);
	fstest::Put16Both(&pvd[128], 2048);
	memcpy(&pvd[156], dir.data(), 34);  // the root's "." record
	pvd[881] = 1;
	std::vector<u8> term(2048, 0);
	term[0] = 255;
	memcpy(&term[1], "CD001", 5);
	term[6] = 1;
	src->Put(16 * 2048, std::move(pvd));
	src->Put(17 * 2048, std::move(term));
	src->Put(18 * 2048, std::move(dir));
	return src;
}

// 8 GB + 4 MB: files straddling and just past 2 GB and 4 GB, a 4 GB - 1 file that crosses 8 GB,
// one just past 8 GB, and one whose record claims more than the image holds.
const u64 kImageBytes = 8 * kGB + (4u << 20);
const u32 kBigSize = 0xFFFFFFFF;
std::vector<BigFile> LargeLayout() {
	return {
		{"LO2.BIN", 2 * kGB - 4096, 65536, {0, 4096 - 32, 65536 - 64}},
		{"HI2.BIN", 2 * kGB + 65536, 65536, {0, 65536 - 64}},
		{"LO4.BIN", 4 * kGB - 4096, 65536, {0, 4096 - 32, 65536 - 64}},
		{"HI4.BIN", 4 * kGB + 65536, 65536, {0, 65536 - 64}},
		{"BIG.BIN", 4 * kGB + (1u << 20), kBigSize, {0, 0x7FFFFFE0, 0x80100000, 4 * kGB - (1u << 20) - 32, kBigSize - 64}},
		{"HI8.BIN", 8 * kGB + (2u << 20), 65536, {0, 1000, 65536 - 64}},
		{"PASTEND.BIN", kImageBytes - 8192, 65536, {0}},
	};
}

std::vector<u8> ReadAt(IsoFileSystem &fs, s32 fd, s64 off, s32 whence, s64 *pos, u32 n = 64) {
	std::vector<u8> buf(n, 0xEE);
	*pos = fs.Seek(fd, off, whence);
	s64 got = fs.Read(fd, buf.data(), n);
	buf.resize(got < 0 ? 0 : (size_t)got);
	return buf;
}

// Bytes [at, at + n) of the image as MakeBigIso laid it out (markers over zeros).
std::vector<u8> Expected(const std::vector<BigFile> &files, u64 at, size_t n) {
	SparseSource img(kImageBytes);
	for (const BigFile &f : files)
		for (u64 m : f.marks) img.Put(f.at + m, Marker(f.name, m));
	std::vector<u8> out(n);
	img.ReadAt(at, out.data(), n);
	return out;
}

const BigFile &Find(const std::vector<BigFile> &files, const char *name) {
	for (const BigFile &f : files)
		if (f.name == name) return f;
	return files[0];
}

}  // namespace

TEST(fs_large_iso_files) {
	const std::vector<BigFile> files = LargeLayout();
	auto dev = OpenBlockDevice(MakeBigIso(kImageBytes, files));
	CHECK(dev != nullptr);
	if (!dev) return;
	CHECK_EQ(dev->SizeBytes(), kImageBytes);
	CHECK_EQ(dev->NumBlocks(), (u32)(kImageBytes / 2048));
	IsoFileSystem fs(std::move(dev));
	CHECK(fs.IsValid());

	// Every file reads its markers, from the front and from the end.
	for (const BigFile &f : files) {
		if (f.name == "PASTEND.BIN") continue;
		const std::string path = "/" + f.name;
		SceIoStat st{};
		CHECK_EQ(fs.GetStat(path, &st), 0);
		CHECK_EQ(st.st_size, (s64)f.size);
		CHECK_EQ(st.st_private[0], (u32)(f.at / 2048));
		s32 fd = fs.Open(path, PSP_O_RDONLY, 0);
		CHECK(fd > 0);
		for (u64 m : f.marks) {
			s64 pos;
			CHECK(ReadAt(fs, fd, (s64)m, PSP_SEEK_SET, &pos) == Marker(f.name, m));
			CHECK_EQ(pos, (s64)m);
		}
		s64 pos;
		CHECK(ReadAt(fs, fd, -64, PSP_SEEK_END, &pos) == Marker(f.name, f.size - 64));
		CHECK_EQ(pos, (s64)f.size - 64);
		// At the end: nothing more.
		u8 b;
		CHECK_EQ(fs.Read(fd, &b, 1), (s64)0);
		CHECK_EQ(fs.Close(fd), 0);
	}

	// Reads that straddle the image's 2 GB and 4 GB marks, and BIG.BIN's own 2 GB.
	s32 lo2 = fs.Open("/LO2.BIN", PSP_O_RDONLY, 0);
	s64 pos;
	CHECK(ReadAt(fs, lo2, 4096 - 40, PSP_SEEK_SET, &pos, 80) == Expected(files, 2 * kGB - 40, 80));
	fs.Close(lo2);
	s32 lo4 = fs.Open("/LO4.BIN", PSP_O_RDONLY, 0);
	CHECK(ReadAt(fs, lo4, 4096 - 40, PSP_SEEK_SET, &pos, 80) == Expected(files, 4 * kGB - 40, 80));
	fs.Close(lo4);

	// BIG.BIN (4 GB - 1, from 4 GB + 1 MB to past 8 GB): seeks of every kind.
	const BigFile &big = Find(files, "BIG.BIN");
	s32 fd = fs.Open("/BIG.BIN", PSP_O_RDONLY, 0);
	CHECK(fd > 0);
	CHECK(ReadAt(fs, fd, 0x7FFFFFE0, PSP_SEEK_SET, &pos) == Marker("BIG.BIN", 0x7FFFFFE0));
	CHECK(ReadAt(fs, fd, 0x80100000, PSP_SEEK_SET, &pos) == Marker("BIG.BIN", 0x80100000));
	// From there, SEEK_CUR to the last 64 bytes, and back by a negative SEEK_CUR.
	CHECK(ReadAt(fs, fd, (s64)(kBigSize - 64) - (0x80100000 + 64), PSP_SEEK_CUR, &pos) == Marker("BIG.BIN", kBigSize - 64));
	CHECK_EQ(pos, (s64)kBigSize - 64);
	CHECK(ReadAt(fs, fd, -(s64)((kBigSize - 64 + 64) - 0x80100000), PSP_SEEK_CUR, &pos) == Marker("BIG.BIN", 0x80100000));
	// Across the image's 8 GB mark.
	const u64 at8 = 8 * kGB - big.at;
	CHECK(ReadAt(fs, fd, (s64)at8 - 32, PSP_SEEK_SET, &pos) == Expected(files, 8 * kGB - 32, 64));
	// A read that runs into the end gets what is left.
	CHECK_EQ(ReadAt(fs, fd, -10, PSP_SEEK_END, &pos, 64).size(), (size_t)10);
	// Before the start: -1, position unchanged.
	CHECK_EQ(fs.Seek(fd, 0x1000, PSP_SEEK_SET), (s64)0x1000);
	CHECK_EQ(fs.Seek(fd, -0x1001, PSP_SEEK_CUR), (s64)-1);
	CHECK_EQ(fs.Seek(fd, 0, PSP_SEEK_CUR), (s64)0x1000);
	// The UMD ioctls: 64-bit size and seek; TELL is the PSP's u32.
	u8 out8[8] = {};
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_GET_SIZE, nullptr, 0, out8, 8), 0);
	CHECK_EQ((u64)out8[0] | ((u64)out8[1] << 8) | ((u64)out8[2] << 16) | ((u64)out8[3] << 24) | ((u64)out8[4] << 32), (u64)kBigSize);
	u8 seek[16] = {};
	fstest::Put32LE(seek, (u32)(kBigSize - 64));
	fstest::Put32LE(seek + 12, PSP_SEEK_SET);
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SEEK, seek, 16, nullptr, 0), 0);
	u8 tell[4] = {};
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_TELL, nullptr, 0, tell, 4), 0);
	CHECK_EQ((u32)tell[0] | ((u32)tell[1] << 8) | ((u32)tell[2] << 16) | ((u32)tell[3] << 24), kBigSize - 64);
	std::vector<u8> buf(64);
	CHECK_EQ(fs.Read(fd, buf.data(), 64), (s64)64);
	CHECK(buf == Marker("BIG.BIN", kBigSize - 64));
	// Past the end with the ioctl is refused.
	fstest::Put32LE(seek, 0);
	fstest::Put32LE(seek + 4, 1);  // 4 GB
	CHECK_EQ(fs.Ioctl(fd, UMD_IOCTL_SEEK, seek, 16, nullptr, 0), SCE_ERRNO_EIO);
	fs.Close(fd);

	// A record that claims more than the image holds is cut at the end of the image.
	SceIoStat st{};
	CHECK_EQ(fs.GetStat("/PASTEND.BIN", &st), 0);
	CHECK_EQ(st.st_size, (s64)8192);

	// umd0:, the whole disc in sectors: past 4 GB (sector 0x200000) and 8 GB (0x400000).
	s32 u = fs.Open("", PSP_O_RDONLY, 0);
	CHECK(u > 0);
	HandleInfo info;
	CHECK_EQ(fs.GetHandleInfo(u, &info), 0);
	CHECK(info.sectorMode);
	CHECK_EQ(info.size, kImageBytes / 2048);
	std::vector<u8> sec(2048);
	const u64 hi4 = Find(files, "HI4.BIN").at, hi8 = Find(files, "HI8.BIN").at;
	CHECK_EQ(fs.Seek(u, (s64)(hi4 / 2048), PSP_SEEK_SET), (s64)(hi4 / 2048));
	CHECK_EQ(fs.Read(u, sec.data(), 1), (s64)1);
	CHECK(std::vector<u8>(sec.begin(), sec.begin() + 64) == Marker("HI4.BIN", 0));
	const u64 bigEnd = big.at + kBigSize - 64;
	CHECK_EQ(fs.Seek(u, (s64)(bigEnd / 2048), PSP_SEEK_SET), (s64)(bigEnd / 2048));
	CHECK_EQ(fs.Read(u, sec.data(), 1), (s64)1);
	CHECK(std::vector<u8>(sec.begin() + bigEnd % 2048, sec.begin() + bigEnd % 2048 + 64) == Marker("BIG.BIN", kBigSize - 64));
	CHECK_EQ(fs.Seek(u, (s64)(hi8 / 2048), PSP_SEEK_SET), (s64)(hi8 / 2048));
	CHECK_EQ(fs.Read(u, sec.data(), 1), (s64)1);
	CHECK(std::vector<u8>(sec.begin(), sec.begin() + 64) == Marker("HI8.BIN", 0));
	// A sector-mode handle can't be read "to the end" in bytes (sceKernelLoadModuleByID).
	std::vector<u8> rest;
	CHECK(!ReadRest(fs, u, rest));
	fs.Close(u);
	SceIoStat dst{};
	CHECK_EQ(fs.GetStat("", &dst), 0);
	CHECK_EQ(dst.st_size, (s64)(kImageBytes / 2048));

	// /sce_lbn extents past 8 GB, in bytes and in sector mode.
	char lbn[32];
	snprintf(lbn, sizeof(lbn), "/sce_lbn0x%x_size0x10000", (u32)(hi8 / 2048));
	s32 l = fs.Open(lbn, PSP_O_RDONLY, 0);
	CHECK(l > 0);
	CHECK(ReadAt(fs, l, 1000, PSP_SEEK_SET, &pos) == Marker("HI8.BIN", 1000));
	fs.Close(l);
	l = fs.Open(lbn, PSP_O_RDONLY | PSPC_O_SECTOR_MODE, 0);
	CHECK(l > 0);
	CHECK(!ReadRest(fs, l, rest));
	CHECK_EQ(fs.Seek(l, 31, PSP_SEEK_SET), (s64)31);
	CHECK_EQ(fs.Read(l, sec.data(), 1), (s64)1);
	CHECK(std::vector<u8>(sec.end() - 64, sec.end()) == Marker("HI8.BIN", 65536 - 64));
	fs.Close(l);

	// ReadRest from a byte handle reads the rest and leaves the position alone.
	s32 h8 = fs.Open("/HI8.BIN", PSP_O_RDONLY, 0);
	CHECK_EQ(fs.Seek(h8, 1000, PSP_SEEK_SET), (s64)1000);
	CHECK(ReadRest(fs, h8, rest));
	CHECK_EQ(rest.size(), (size_t)(65536 - 1000));
	CHECK(rest.size() >= 64 && std::vector<u8>(rest.begin(), rest.begin() + 64) == Marker("HI8.BIN", 1000));
	CHECK_EQ(fs.Seek(h8, 0, PSP_SEEK_CUR), (s64)1000);
	fs.Close(h8);

	// Reading a metadata file whole is capped, before anything is allocated.
	std::vector<u8> whole = {1, 2, 3};
	CHECK_EQ(fs.ReadWholeFile("/BIG.BIN", whole, kMaxMetadataBytes), SCE_ERRNO_EFBIG);
	CHECK(whole.empty());
	CHECK_EQ(fs.ReadWholeFile("/HI2.BIN", whole, kMaxMetadataBytes), 0);
	CHECK_EQ(whole.size(), (size_t)65536);
}

TEST(fs_large_iso_multi_extent) {
	// A file of 4 GB or more is several records with the same name, all but the last flagged
	// 0x80 (ISO 9660 level 3). PSPC shows the first extent only, once, and logs a warning.
	std::vector<BigFile> files = {
		{"A.BIN", 1 * kGB, 4096, {0}},
		{"HUGE.BIN", 1 * kGB + 65536, 0xFFFFF800, {0, 0xFFFFF800 - 64}, 0x80},
		{"HUGE.BIN", 5 * kGB + 65536, 0x100000, {0}},
		{"Z.BIN", 6 * kGB, 4096, {0}},
	};
	auto dev = OpenBlockDevice(MakeBigIso(7 * kGB, files));
	CHECK(dev != nullptr);
	if (!dev) return;
	IsoFileSystem fs(std::move(dev));
	std::vector<std::string> names;
	std::vector<s64> sizes;
	s32 dd = fs.DOpen("/");
	SceIoDirent e{};
	while (fs.DRead(dd, &e) == 1) {
		names.push_back(e.d_name);
		sizes.push_back(e.d_stat.st_size);
	}
	fs.DClose(dd);
	CHECK((names == std::vector<std::string>{"A.BIN", "HUGE.BIN", "Z.BIN"}));
	CHECK(sizes.size() == 3 && sizes[1] == 0xFFFFF800);
	s32 fd = fs.Open("/HUGE.BIN", PSP_O_RDONLY, 0);
	s64 pos;
	CHECK(ReadAt(fs, fd, -64, PSP_SEEK_END, &pos) == Marker("HUGE.BIN", 0xFFFFF800 - 64));
	fs.Close(fd);
	std::vector<u8> z;
	CHECK_EQ(fs.ReadWholeFile("/Z.BIN", z), 0);
	CHECK(z.size() == 4096 && std::vector<u8>(z.begin(), z.begin() + 64) == Marker("Z.BIN", 0));
}

namespace {

// A CSO/ZSO header and index whose frames are all stored uncompressed at the given file
// positions (a gap before a stored frame is never read, so the file can be sparse).
std::unique_ptr<SparseSource> MakeIndexedImage(fstest::CsoFlavor flavor, u32 shift, u64 total, const std::vector<u64> &framePos,
                                               u64 endPos, const std::map<u32, std::vector<u8>> &frameData) {
	auto src = std::make_unique<SparseSource>(endPos);
	const u32 frames = (u32)framePos.size();
	std::vector<u8> head(24 + ((size_t)frames + 1) * 4, 0);
	memcpy(head.data(), flavor == fstest::CsoFlavor::Zso ? "ZISO" : "CISO", 4);
	fstest::Put32LE(&head[4], 0x18);
	fstest::Put32LE(&head[8], (u32)total);
	fstest::Put32LE(&head[12], (u32)(total >> 32));
	fstest::Put32LE(&head[16], 2048);
	head[20] = flavor == fstest::CsoFlavor::V2 ? 2 : 1;
	head[21] = (u8)shift;
	// v1 and ZSO flag a stored frame; v2 knows it from its stored size (here the gap to the next).
	const u32 flag = flavor == fstest::CsoFlavor::V2 ? 0 : 0x80000000u;
	for (u32 f = 0; f < frames; f++) fstest::Put32LE(&head[24 + (size_t)f * 4], (u32)(framePos[f] >> shift) | flag);
	fstest::Put32LE(&head[24 + (size_t)frames * 4], (u32)(endPos >> shift));
	src->Put(0, std::move(head));
	for (const auto &[f, data] : frameData) src->Put(framePos[f], data);
	return src;
}

std::vector<u8> FrameOf(const char *tag, u32 frame) {
	std::vector<u8> d(2048, 0);
	snprintf((char *)d.data(), 64, "%s frame %u", tag, frame);
	return d;
}

}  // namespace

TEST(fs_large_cso_index) {
	for (auto flavor : {fstest::CsoFlavor::V1, fstest::CsoFlavor::V2, fstest::CsoFlavor::Zso}) {
		// Frames stored at 2 GB, 4 GB and 6 GB of the file: index entries with shifts 1 and 2.
		for (u32 shift : {1u, 2u}) {
			std::vector<u64> pos = {0x10000, 2 * kGB, 4 * kGB, 6 * kGB};
			const u64 end = 6 * kGB + 4096;
			if ((end >> shift) > 0x7FFFFFFF) continue;  // shift 1 reaches 4 GB only
			std::map<u32, std::vector<u8>> data;
			for (u32 f = 0; f < 4; f++) data[f] = FrameOf("cso", f);
			std::string err;
			auto dev = OpenBlockDevice(MakeIndexedImage(flavor, shift, 4 * 2048, pos, end, data), &err);
			CHECK(dev != nullptr);
			if (!dev) {
				printf("  %s\n", err.c_str());
				continue;
			}
			std::vector<u8> sec(2048);
			for (u32 f = 0; f < 4; f++) {
				CHECK(dev->ReadBlock(f, sec.data()));
				CHECK(sec == data[f]);
			}
		}
		{
			std::vector<u64> pos = {0x10000, 2 * kGB - 2048, 3 * kGB};
			std::map<u32, std::vector<u8>> data = {{0, FrameOf("s1", 0)}, {1, FrameOf("s1", 1)}, {2, FrameOf("s1", 2)}};
			auto dev = OpenBlockDevice(MakeIndexedImage(flavor, 1, 3 * 2048, pos, 3 * kGB + 2048, data));
			CHECK(dev != nullptr);
			std::vector<u8> sec(2048);
			if (dev) {
				CHECK(dev->ReadBlock(2, sec.data()));
				CHECK(sec == data[2]);
			}
		}
		// An uncompressed image over 4 GB (2^21 + 2 frames), all frames but the last two sharing
		// one stored copy. (Not v2: it tells a stored frame by its size, which sharing makes 0.)
		if (flavor != fstest::CsoFlavor::V2) {
			const u32 frames = (1u << 21) + 2;
			const u64 index = 24 + ((u64)frames + 1) * 4;
			const u64 shared = (index + 0xFFFF) & ~0xFFFFull;
			std::vector<u64> pos(frames, shared);
			pos[frames - 2] = shared + 2048;
			pos[frames - 1] = shared + 4096;
			std::map<u32, std::vector<u8>> data = {{0, FrameOf("shared", 0)}, {frames - 2, FrameOf("big", frames - 2)}, {frames - 1, FrameOf("big", frames - 1)}};
			std::string err;
			auto dev = OpenBlockDevice(MakeIndexedImage(flavor, 0, (u64)frames * 2048, pos, shared + 6144, data), &err);
			CHECK(dev != nullptr);
			if (!dev) {
				printf("  %s\n", err.c_str());
				continue;
			}
			CHECK_EQ(dev->SizeBytes(), (u64)frames * 2048);
			CHECK(dev->SizeBytes() > 4 * kGB);
			std::vector<u8> sec(2048);
			CHECK(dev->ReadBlock(frames - 1, sec.data()));
			CHECK(sec == data[frames - 1]);
			CHECK(dev->ReadBlock(frames - 2, sec.data()));
			CHECK(sec == data[frames - 2]);
			CHECK(dev->ReadBlock((1u << 21) - 5, sec.data()));
			CHECK(sec == data[0]);
		}
	}

	// A shift-0 index whose positions pass 2 GB wraps into the flag bit: refused, saying why.
	{
		std::vector<u64> pos = {0x10000, 0x80000100};
		std::map<u32, std::vector<u8>> data = {{0, FrameOf("x", 0)}};
		auto src = MakeIndexedImage(fstest::CsoFlavor::V1, 0, 2 * 2048, pos, 0x80000100 + 2048, data);
		std::string err;
		CHECK(OpenBlockDevice(std::move(src), &err) == nullptr);
		CHECK(err.find("overflows") != std::string::npos);
		CHECK(err.find("index shift") != std::string::npos);
	}
	// An index left zeroed (a writer that died after reserving it) is refused too.
	{
		auto src = std::make_unique<SparseSource>(1 << 20);
		std::vector<u8> head(24, 0);
		memcpy(head.data(), "CISO", 4);
		fstest::Put32LE(&head[4], 0x18);
		fstest::Put32LE(&head[8], 64 * 2048);
		fstest::Put32LE(&head[16], 2048);
		head[20] = 1;
		src->Put(0, head);
		std::string err;
		CHECK(OpenBlockDevice(std::move(src), &err) == nullptr);
		CHECK(err.find("corrupt") != std::string::npos);
	}
}
