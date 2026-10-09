// SPDX-License-Identifier: GPL-2.0-or-later
// Ported from PPSSPP (Core/FileSystems/BlockDevices.cpp), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
// CSO v2 and ZSO follow maxcso's format notes; DAX follows the layout of Dark_AleX's DAX images.
#include "fs/block_device.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/log.h"
#include "fs/decompress.h"

namespace vfs {

ByteSource::~ByteSource() = default;
BlockDevice::~BlockDevice() = default;

const char *ImageFormatName(ImageFormat format) {
	switch (format) {
	case ImageFormat::Iso: return "ISO";
	case ImageFormat::CsoV1: return "CSO";
	case ImageFormat::CsoV2: return "CSOv2";
	case ImageFormat::Zso: return "ZSO";
	case ImageFormat::Dax: return "DAX";
	}
	return "?";
}

namespace {

u16 LE16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
u32 LE32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
u64 LE64(const u8 *p) { return (u64)LE32(p) | ((u64)LE32(p + 4) << 32); }

#ifdef _WIN32
class FileSource : public ByteSource {
public:
	explicit FileSource(HANDLE h, u64 size) : h_(h), size_(size) {}
	~FileSource() override { CloseHandle(h_); }
	u64 Size() const override { return size_; }
	size_t ReadAt(u64 offset, void *out, size_t size) override {
		// Positional reads on a synchronous handle are serialized by the kernel, so no lock here.
		size_t total = 0;
		u8 *dst = (u8 *)out;
		while (total < size) {
			DWORD chunk = (DWORD)std::min<size_t>(size - total, 1u << 30);
			OVERLAPPED ov{};
			u64 pos = offset + total;
			ov.Offset = (DWORD)pos;
			ov.OffsetHigh = (DWORD)(pos >> 32);
			DWORD got = 0;
			if (!ReadFile(h_, dst + total, chunk, &got, &ov)) {
				break;
			}
			total += got;
			if (got < chunk) {
				break;
			}
		}
		return total;
	}

private:
	HANDLE h_;
	u64 size_;
};
#else
class FileSource : public ByteSource {
public:
	FileSource(FILE *f, u64 size) : f_(f), size_(size) {}
	~FileSource() override { fclose(f_); }
	u64 Size() const override { return size_; }
	size_t ReadAt(u64 offset, void *out, size_t size) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (fseeko(f_, (off_t)offset, SEEK_SET) != 0) {
			return 0;
		}
		return fread(out, 1, size, f_);
	}

private:
	std::mutex mutex_;
	FILE *f_;
	u64 size_;
};
#endif

class MemorySource : public ByteSource {
public:
	explicit MemorySource(std::vector<u8> data) : data_(std::move(data)) {}
	u64 Size() const override { return data_.size(); }
	size_t ReadAt(u64 offset, void *out, size_t size) override {
		if (offset >= data_.size()) {
			return 0;
		}
		size_t n = (size_t)std::min<u64>(size, data_.size() - offset);
		memcpy(out, data_.data() + offset, n);
		return n;
	}

private:
	std::vector<u8> data_;
};

class PlainIsoDevice : public BlockDevice {
public:
	explicit PlainIsoDevice(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {
		size_ = src_->Size();
		blocks_ = (u32)std::min<u64>((size_ + kSectorSize - 1) / kSectorSize, 0xFFFFFFFFu);
	}
	bool ReadBlocks(u32 lba, u32 count, u8 *out) override {
		if (lba >= blocks_) {
			memset(out, 0, (size_t)count * kSectorSize);
			return false;
		}
		u32 n = std::min(count, blocks_ - lba);
		size_t bytes = (size_t)n * kSectorSize;
		u64 offset = (u64)lba * kSectorSize;
		size_t expected = (size_t)std::min<u64>(bytes, size_ - offset);
		size_t got = src_->ReadAt(offset, out, bytes);
		if (got < bytes) {
			memset(out + got, 0, bytes - got);
		}
		if (n < count) {
			memset(out + bytes, 0, (size_t)(count - n) * kSectorSize);
		}
		return got >= expected && n == count;
	}
	u32 NumBlocks() const override { return blocks_; }
	u64 SizeBytes() const override { return size_; }
	ImageFormat Format() const override { return ImageFormat::Iso; }

private:
	std::unique_ptr<ByteSource> src_;
	u64 size_ = 0;
	u32 blocks_ = 0;
};

// Images stored as independently compressed frames of frameSize_ bytes, with a small LRU cache
// of decompressed frames.
class FramedDevice : public BlockDevice {
public:
	bool ReadBlocks(u32 lba, u32 count, u8 *out) override {
		std::lock_guard<std::mutex> lock(mutex_);
		bool ok = true;
		u32 i = 0;
		while (i < count) {
			u32 block = lba + i;
			if (block >= numBlocks_ || block < lba) {
				memset(out + (size_t)i * kSectorSize, 0, (size_t)(count - i) * kSectorSize);
				return false;
			}
			u32 frame = block / sectorsPerFrame_;
			u32 inFrame = block % sectorsPerFrame_;
			u32 n = std::min(count - i, sectorsPerFrame_ - inFrame);
			n = std::min(n, numBlocks_ - block);
			const Slot &slot = GetFrame(frame);
			memcpy(out + (size_t)i * kSectorSize, slot.data.data() + (size_t)inFrame * kSectorSize, (size_t)n * kSectorSize);
			ok = ok && slot.ok;
			i += n;
		}
		return ok;
	}
	u32 NumBlocks() const override { return numBlocks_; }
	u64 SizeBytes() const override { return totalBytes_; }

protected:
	explicit FramedDevice(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}

	// Decompresses `frame` into `out` (frameSize_ bytes, zeroed by the caller). Mutex held.
	virtual bool DecodeFrame(u32 frame, u8 *out) = 0;

	// Bytes of uncompressed data in `frame` (only the last one can be short).
	u32 FrameBytes(u32 frame) const {
		u64 start = (u64)frame * frameSize_;
		return (u32)std::min<u64>(frameSize_, totalBytes_ - start);
	}

	void SetGeometry(u64 totalBytes, u32 frameSize) {
		totalBytes_ = totalBytes;
		frameSize_ = frameSize;
		sectorsPerFrame_ = frameSize / kSectorSize;
		numFrames_ = (u32)((totalBytes + frameSize - 1) / frameSize);
		numBlocks_ = (u32)((totalBytes + kSectorSize - 1) / kSectorSize);
		size_t slots = std::clamp<size_t>((size_t)(4u << 20) / frameSize, 1, 16);
		cache_.resize(slots);
	}

	std::unique_ptr<ByteSource> src_;
	u64 totalBytes_ = 0;
	u32 frameSize_ = kSectorSize;
	u32 numFrames_ = 0;
	std::vector<u8> scratch_;

private:
	struct Slot {
		u32 frame = 0xFFFFFFFF;
		u64 lastUse = 0;
		bool ok = false;
		std::vector<u8> data;
	};

	const Slot &GetFrame(u32 frame) {
		Slot *victim = &cache_[0];
		for (Slot &s : cache_) {
			if (s.frame == frame) {
				s.lastUse = ++useCounter_;
				return s;
			}
			if (s.lastUse < victim->lastUse) {
				victim = &s;
			}
		}
		victim->data.assign(frameSize_, 0);
		victim->ok = DecodeFrame(frame, victim->data.data());
		if (!victim->ok) {
			memset(victim->data.data(), 0, frameSize_);
			LOGW("fs", "%s: failed to decode frame %u", ImageFormatName(Format()), frame);
		}
		// Failed frames aren't kept, so a retry reads the image again.
		victim->frame = victim->ok ? frame : 0xFFFFFFFF;
		victim->lastUse = ++useCounter_;
		return *victim;
	}

	std::mutex mutex_;
	std::vector<Slot> cache_;
	u64 useCounter_ = 0;
	u32 sectorsPerFrame_ = 1;
	u32 numBlocks_ = 0;
};

// CISO header (all formats of the family share it):
//   +00 magic "CISO"/"ZISO"   +04 u32 header size (0x18; often 0 in v1)   +08 u64 uncompressed size
//   +10 u32 frame size        +14 u8 version                              +15 u8 index shift
// followed by numFrames + 1 u32 index entries, each a file offset >> shift. v1 (and v0): the high
// bit marks an uncompressed frame, the rest is raw deflate. ZSO: same, with LZ4 blocks. v2: a frame
// whose stored size is >= the frame size is uncompressed; otherwise the high bit selects LZ4 over
// deflate.
class CsoDevice : public FramedDevice {
public:
	explicit CsoDevice(std::unique_ptr<ByteSource> src) : FramedDevice(std::move(src)) {}

	bool Init(bool zso, std::string *error) {
		zso_ = zso;
		u8 h[24];
		if (src_->ReadAt(0, h, sizeof(h)) != sizeof(h)) {
			*error = "CSO header truncated";
			return false;
		}
		u32 headerSize = LE32(h + 4);
		u64 total = LE64(h + 8);
		u32 frameSize = LE32(h + 16);
		version_ = h[20];
		shift_ = h[21];
		if (zso_ ? version_ > 1 : version_ > 2) {
			*error = "unsupported CSO version " + std::to_string(version_);
			return false;
		}
		if (frameSize < kSectorSize || frameSize > (16u << 20) || (frameSize & (frameSize - 1)) != 0) {
			*error = "unsupported CSO frame size " + std::to_string(frameSize);
			return false;
		}
		if (shift_ > 20) {
			*error = "unsupported CSO index shift " + std::to_string(shift_);
			return false;
		}
		if (total / kSectorSize >= 0xFFFFFFFFull) {
			*error = "CSO image too large";
			return false;
		}
		SetGeometry(total, frameSize);
		const u64 fileSize = src_->Size();
		const u64 indexStart = (version_ >= 2 && headerSize >= 24 && headerSize < fileSize) ? headerSize : 24;
		const u64 indexBytes = ((u64)numFrames_ + 1) * 4;
		if (indexStart + indexBytes > fileSize) {
			*error = "CSO index truncated";
			return false;
		}
		std::vector<u8> raw((size_t)indexBytes);
		if (src_->ReadAt(indexStart, raw.data(), raw.size()) != raw.size()) {
			*error = "CSO index unreadable";
			return false;
		}
		index_.resize(numFrames_ + 1);
		for (u32 i = 0; i <= numFrames_; i++) {
			index_[i] = LE32(raw.data() + (size_t)i * 4);
			if (i > 0 && (index_[i] & 0x7FFFFFFF) < (index_[i - 1] & 0x7FFFFFFF)) {
				*error = "CSO index not monotonic at frame " + std::to_string(i - 1);
				return false;
			}
		}
		if (((u64)(index_[numFrames_] & 0x7FFFFFFF) << shift_) > fileSize) {
			*error = "CSO image truncated";
			return false;
		}
		maxStored_ = (size_t)frameSize * 2 + ((size_t)1 << shift_) + 64;
		return true;
	}

	ImageFormat Format() const override {
		return zso_ ? ImageFormat::Zso : (version_ >= 2 ? ImageFormat::CsoV2 : ImageFormat::CsoV1);
	}

protected:
	bool DecodeFrame(u32 frame, u8 *out) override {
		const u32 idx = index_[frame];
		const u64 pos = (u64)(idx & 0x7FFFFFFF) << shift_;
		const u64 end = (u64)(index_[frame + 1] & 0x7FFFFFFF) << shift_;
		const u64 stored = end - pos;
		const u32 want = FrameBytes(frame);
		bool plain, lz4;
		if (zso_) {
			plain = (idx & 0x80000000) != 0;
			lz4 = true;
		} else if (version_ >= 2) {
			plain = stored >= frameSize_;
			lz4 = (idx & 0x80000000) != 0;
		} else {
			plain = (idx & 0x80000000) != 0;
			lz4 = false;
		}
		if (plain) {
			return src_->ReadAt(pos, out, want) == want;
		}
		size_t n = (size_t)std::min<u64>(stored, maxStored_);
		scratch_.resize(n);
		n = src_->ReadAt(pos, scratch_.data(), n);
		s64 produced = lz4 ? Lz4DecompressBlock(scratch_.data(), n, out, want) : InflateRaw(scratch_.data(), n, out, frameSize_);
		if (produced < (s64)want && version_ >= 2 && !zso_ && stored >= want) {
			// A short last frame may have been stored raw at its own length.
			memset(out, 0, frameSize_);
			return src_->ReadAt(pos, out, want) == want;
		}
		return produced >= (s64)want;
	}

private:
	std::vector<u32> index_;
	size_t maxStored_ = 0;
	int version_ = 1;
	u32 shift_ = 0;
	bool zso_ = false;
};

// DAX: +00 "DAX\0", +04 u32 uncompressed size, +08 u32 version, +0C u32 NC area count, +10 four
// reserved words. Then u32 offsets[frames], u16 lengths[frames] and (version >= 1) NC areas of
// {u32 first frame, u32 frame count}, whose frames are stored uncompressed. Frames are 8 KiB, zlib.
class DaxDevice : public FramedDevice {
public:
	static constexpr u32 kFrameSize = 0x2000;

	explicit DaxDevice(std::unique_ptr<ByteSource> src) : FramedDevice(std::move(src)) {}

	bool Init(std::string *error) {
		u8 h[32];
		if (src_->ReadAt(0, h, sizeof(h)) != sizeof(h)) {
			*error = "DAX header truncated";
			return false;
		}
		u32 total = LE32(h + 4);
		u32 version = LE32(h + 8);
		u32 ncAreas = version >= 1 ? LE32(h + 12) : 0;
		SetGeometry(total, kFrameSize);
		const u64 fileSize = src_->Size();
		const u64 tableBytes = (u64)numFrames_ * 6 + (u64)ncAreas * 8;
		if (32 + tableBytes > fileSize) {
			*error = "DAX tables truncated";
			return false;
		}
		std::vector<u8> raw((size_t)tableBytes);
		if (src_->ReadAt(32, raw.data(), raw.size()) != raw.size()) {
			*error = "DAX tables unreadable";
			return false;
		}
		offsets_.resize(numFrames_);
		lengths_.resize(numFrames_);
		plain_.assign(numFrames_, 0);
		for (u32 i = 0; i < numFrames_; i++) {
			offsets_[i] = LE32(raw.data() + (size_t)i * 4);
			lengths_[i] = LE16(raw.data() + (size_t)numFrames_ * 4 + (size_t)i * 2);
		}
		const u8 *nc = raw.data() + (size_t)numFrames_ * 6;
		for (u32 a = 0; a < ncAreas; a++) {
			u32 first = LE32(nc + a * 8), count = LE32(nc + a * 8 + 4);
			for (u64 f = first; f < (u64)first + count && f < numFrames_; f++) {
				plain_[(size_t)f] = 1;
			}
		}
		return true;
	}

	ImageFormat Format() const override { return ImageFormat::Dax; }

protected:
	bool DecodeFrame(u32 frame, u8 *out) override {
		const u32 want = FrameBytes(frame);
		if (plain_[frame]) {
			return src_->ReadAt(offsets_[frame], out, want) == want;
		}
		scratch_.resize(lengths_[frame]);
		size_t n = src_->ReadAt(offsets_[frame], scratch_.data(), scratch_.size());
		s64 produced = IsZlibHeader(scratch_.data(), n) ? InflateZlib(scratch_.data(), n, out, kFrameSize) : InflateRaw(scratch_.data(), n, out, kFrameSize);
		return produced >= (s64)want;
	}

private:
	std::vector<u32> offsets_;
	std::vector<u16> lengths_;
	std::vector<u8> plain_;
};

}  // namespace

std::unique_ptr<ByteSource> OpenFileSource(const std::filesystem::path &path) {
#ifdef _WIN32
	HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	LARGE_INTEGER size;
	if (!GetFileSizeEx(h, &size)) {
		CloseHandle(h);
		return nullptr;
	}
	return std::make_unique<FileSource>(h, (u64)size.QuadPart);
#else
	FILE *f = fopen(path.string().c_str(), "rb");
	if (!f) {
		return nullptr;
	}
	fseeko(f, 0, SEEK_END);
	off_t size = ftello(f);
	return std::make_unique<FileSource>(f, (u64)size);
#endif
}

std::unique_ptr<ByteSource> MakeMemorySource(std::vector<u8> data) {
	return std::make_unique<MemorySource>(std::move(data));
}

std::unique_ptr<BlockDevice> OpenBlockDevice(std::unique_ptr<ByteSource> source, std::string *error) {
	std::string localError;
	if (!error) {
		error = &localError;
	}
	error->clear();
	if (!source) {
		*error = "no image";
		return nullptr;
	}
	u8 magic[8]{};
	size_t got = source->ReadAt(0, magic, sizeof(magic));
	if (got == 0) {
		*error = "empty image";
		return nullptr;
	}
	if (got >= 4 && (!memcmp(magic, "CISO", 4) || !memcmp(magic, "ZISO", 4))) {
		auto dev = std::make_unique<CsoDevice>(std::move(source));
		if (!dev->Init(magic[0] == 'Z', error)) {
			return nullptr;
		}
		return dev;
	}
	if (got >= 4 && !memcmp(magic, "DAX\0", 4)) {
		auto dev = std::make_unique<DaxDevice>(std::move(source));
		if (!dev->Init(error)) {
			return nullptr;
		}
		return dev;
	}
	if (got >= 4 && !memcmp(magic, "JISO", 4)) {
		*error = "JSO images are not supported";
		return nullptr;
	}
	if (got >= 8 && !memcmp(magic, "MComprHD", 8)) {
		*error = "CHD images are not supported";
		return nullptr;
	}
	if (got >= 4 && !memcmp(magic, "\0PBP", 4)) {
		*error = "PBP (PSN/NPDRM) images are not supported";
		return nullptr;
	}
	return std::make_unique<PlainIsoDevice>(std::move(source));
}

std::unique_ptr<BlockDevice> OpenBlockDevice(const std::filesystem::path &path, std::string *error) {
	auto src = OpenFileSource(path);
	if (!src) {
		if (error) {
			*error = "cannot open image file";
		}
		return nullptr;
	}
	return OpenBlockDevice(std::move(src), error);
}

}  // namespace vfs
