// SPDX-License-Identifier: GPL-3.0-or-later
#include "module.h"

#include <algorithm>

#include "formats.h"
#include "inflate.h"
#include "kl4e.h"
#include "lzrc.h"
#include "psar.h"

static const size_t kMaxOutput = 256u * 1024 * 1024;

bool DecompressAny(const u8 *data, size_t size, size_t sizeHint, Bytes *out, std::string *method, size_t *consumed, std::string *error) {
	*consumed = 0;
	if (IsGzipMagic(data, size)) {
		*method = "gzip";
		return InflateStream(data, size, DeflateWrapper::Gzip, out, consumed, error, kMaxOutput);
	}
	bool isKL3E = false;
	if (IsKL4EMagic(data, size, &isKL3E)) {
		*method = isKL3E ? "KL3E" : "KL4E";
		// The stream has no length of its own. Start from the header's elf_size (a stored stream
		// must be strictly smaller than the buffer) and grow if that turns out to be short.
		size_t capacity = sizeHint ? sizeHint + 0x1000 : 16u * 1024 * 1024;
		while (true) {
			out->assign(capacity, 0);
			const u8 *end = nullptr;
			const int n = DecompressKL4E(out->data(), (int)capacity, data + 4, size - 4, &end, isKL3E);
			if (n == KL4E_ERROR_INVALID_SIZE && capacity < kMaxOutput) {
				capacity = std::min(capacity * 4, kMaxOutput);
				continue;
			}
			if (n < 0) {
				*error = StringPrintf("%s stream is malformed (0x%08x)", method->c_str(), (u32)n);
				out->clear();
				return false;
			}
			out->resize((size_t)n);
			*consumed = end ? (size_t)(end - data) : size;
			return true;
		}
	}
	if (HasMagic(data, size, "2RLZ", 4)) {
		*method = "2RLZ";
		size_t capacity = sizeHint ? sizeHint + 0x1000 : 16u * 1024 * 1024;
		if (size - 4 > 0x7FFFFFFF) {
			*error = "2RLZ stream too large";
			return false;
		}
		while (true) {
			out->assign(capacity, 0);
			int used = 0;
			const int n = DecompressLZRC(out->data(), (int)capacity, data + 4, (int)(size - 4), &used);
			if (n < 0 && capacity < kMaxOutput) {
				// The decoder doesn't distinguish overflow from corruption; retry bigger once or twice.
				capacity = std::min(capacity * 4, kMaxOutput);
				continue;
			}
			if (n < 0) {
				*error = "2RLZ stream is malformed";
				out->clear();
				return false;
			}
			out->resize((size_t)n);
			*consumed = 4 + (size_t)used;
			return true;
		}
	}
	if (IsZlibHeader(data, size)) {
		*method = "zlib";
		return InflateStream(data, size, DeflateWrapper::Zlib, out, consumed, error, kMaxOutput);
	}
	*error = "unknown compression";
	return false;
}

static bool IsCompressionMagic(const u8 *p, size_t size) {
	bool kl3e;
	return IsGzipMagic(p, size) || IsKL4EMagic(p, size, &kl3e) || HasMagic(p, size, "2RLZ", 4);
}

// Decompresses a decrypted payload as the header asks (or as its magic says) into r->output.
static bool DecompressPayload(Bytes &payload, const PspHeader &h, DecodeResult *r) {
	const bool compressed = (h.compAttribute & 1) != 0;
	const bool magic = IsCompressionMagic(payload.data(), payload.size());
	if (compressed && !magic) {
		r->error = "header says compressed, but the payload has no known compression magic";
		return false;
	}
	if (!magic) {
		r->output = std::move(payload);
		return true;
	}
	Bytes out;
	size_t consumed = 0;
	std::string err;
	if (!DecompressAny(payload.data(), payload.size(), h.elfSize, &out, &r->compression, &consumed, &err)) {
		r->error = r->compression + ": " + err;
		return false;
	}
	r->steps.push_back(r->compression);
	if (!compressed) {
		r->warnings.push_back("payload was compressed although comp_attribute says it isn't");
	}
	// Some firmware files carry a second stream after the first (pspdecrypt's "part 2").
	size_t rest = consumed;
	while (rest < payload.size() && rest < consumed + 16 && payload[rest] == 0) {
		rest++;
	}
	if (rest < payload.size() && IsCompressionMagic(payload.data() + rest, payload.size() - rest)) {
		std::string method2;
		size_t consumed2 = 0;
		if (DecompressAny(payload.data() + rest, payload.size() - rest, 0, &r->trailing, &method2, &consumed2, &err)) {
			r->warnings.push_back(StringPrintf("second %s stream after the first (%zu bytes)", method2.c_str(), r->trailing.size()));
		} else {
			r->trailing.clear();
		}
	}
	r->output = std::move(out);
	return true;
}

static void DecodePsp(const u8 *data, size_t size, const u8 *seed, DecodeResult *r) {
	PspHeader &h = r->psp;
	if (!ParsePspHeader(data, size, &h)) {
		r->error = "truncated ~PSP header";
		return;
	}
	r->hadPspHeader = true;
	r->tag = h.tag;
	if (h.pspSize < PSP_HEADER_SIZE) {
		r->error = StringPrintf("psp_size 0x%x is smaller than the header", h.pspSize);
		return;
	}
	if (h.pspSize > size) {
		r->error = StringPrintf("truncated: psp_size is %u bytes but only %zu are present", h.pspSize, size);
		return;
	}
	const u8 *body = data + PSP_HEADER_SIZE;
	const size_t bodySize = h.pspSize - PSP_HEADER_SIZE;

	// Custom firmware packing (M33's psp-packer and friends): the ~PSP header sits in front of a
	// plain gzip stream with no encryption at all. The tag and oe_tag are only there to satisfy
	// the CFW loader; the gzip magic right after the header is the tell. A stream that inflates
	// (so its CRC checks out) to something other than an ELF is only a fallback: KIRK gets the
	// first say, since ciphertext can start with the gzip magic by chance.
	Bytes inflated;
	bool haveInflated = false;
	if (IsGzipMagic(body, bodySize)) {
		size_t consumed = 0;
		std::string err;
		haveInflated = InflateStream(body, bodySize, DeflateWrapper::Gzip, &inflated, &consumed, &err, kMaxOutput);
	}
	Bytes payload;
	PrxDecryptInfo info;
	if (haveInflated && IsElf(inflated.data(), inflated.size())) {
		r->cfwPacked = true;
	} else if (IsElf(body, bodySize)) {
		r->steps.push_back("~PSP header in front of a plain ELF");
		r->output.assign(body, body + bodySize);
	} else if (DecryptPRX(data, h.pspSize, size, &payload, seed, &info) >= 0) {
		r->encrypted = true;
		r->prxType = info.type;
		r->steps.push_back(StringPrintf("KIRK decrypt (type %d, tag 0x%08x)", info.type, h.tag));
		if (!DecompressPayload(payload, h, r)) {
			return;
		}
	} else if (haveInflated) {
		r->cfwPacked = true;
	} else {
		r->status = info.error == PrxError::UnknownTag || info.error == PrxError::NeedsSeed ? DecodeStatus::NeedsKey : DecodeStatus::Failed;
		r->error = StringPrintf("cannot decrypt ~PSP (tag 0x%08x): %s", h.tag, PrxErrorString(info.error));
		return;
	}
	if (r->cfwPacked) {
		r->compression = "gzip";
		r->steps.push_back("~PSP header without encryption (custom firmware packing)");
		r->steps.push_back("gzip");
		r->output = std::move(inflated);
	}

	r->status = DecodeStatus::Ok;
	if (h.elfSize != 0 && r->output.size() != h.elfSize) {
		r->warnings.push_back(StringPrintf("output is %zu bytes, header elf_size says %u", r->output.size(), h.elfSize));
	}
	if (!IsElf(r->output.data(), r->output.size())) {
		r->warnings.push_back("output is not an ELF");
	}
}

void DecodeExecutable(const u8 *data, size_t size, const DecodeOptions &options, DecodeResult *r) {
	*r = DecodeResult();
	const u8 *seed = options.seed;
	u8 derivedSeed[16];
	bool edatNeedsLicensee = false;

	// Peel wrappers. Each one strictly shrinks the buffer, and a handful is plenty.
	for (int depth = 0; depth < 6; depth++) {
		const FileKind kind = SniffKind(data, size);
		switch (kind) {
		case FileKind::Edat: {
			EdatHeader e;
			ParseEdat(data, size, &e);
			if (e.payloadOffset < 0x90 || e.payloadOffset >= size) {
				r->error = "EDAT header has a bad payload offset";
				return;
			}
			if (HasMagic(data + e.payloadOffset, size - e.payloadOffset, "\0PGD", 4)) {
				r->error = "EDAT data file (PGD), not a module";
				return;
			}
			r->steps.push_back("PSPEDAT wrapper (" + e.contentId + ")");
			if (!seed) {
				if (DeriveEdatModuleSeed(data, size, derivedSeed, options.licenseeKey)) {
					seed = derivedSeed;
					r->steps.push_back("NPDRM seed derived from the content ID");
				} else if (e.flags & 1) {
					edatNeedsLicensee = true;
				}
			}
			data += e.payloadOffset;
			size -= e.payloadOffset;
			continue;
		}
		case FileKind::SceWrapped: {
			const u32 headerSize = size >= 8 ? rd32(data + 4) : 0;
			if (headerSize < 8 || headerSize >= size) {
				r->error = "~SCE header has a bad size";
				return;
			}
			r->steps.push_back(StringPrintf("~SCE header (0x%x bytes)", headerSize));
			data += headerSize;
			size -= headerSize;
			continue;
		}
		case FileKind::Pbp: {
			PbpFile pbp;
			if (!pbp.Parse(data, size, &r->error)) {
				return;
			}
			if (pbp.size[PbpFile::DATA_PSP] == 0) {
				r->error = "PBP has no DATA.PSP";
				return;
			}
			r->steps.push_back("PBP DATA.PSP");
			data += pbp.offset[PbpFile::DATA_PSP];
			size = pbp.size[PbpFile::DATA_PSP];
			continue;
		}
		case FileKind::Elf:
			r->steps.push_back("plain ELF");
			r->output.assign(data, data + size);
			r->status = DecodeStatus::Ok;
			return;
		case FileKind::PspModule:
			DecodePsp(data, size, seed, r);
			if (r->status != DecodeStatus::Ok && edatNeedsLicensee) {
				r->status = DecodeStatus::NeedsKey;
				r->error += " (this EDAT module also needs the game's licensee key: --licensee-key)";
			}
			return;
		case FileKind::Gzip:
		case FileKind::Zlib:
		case FileKind::KL4E:
		case FileKind::KL3E:
		case FileKind::LZRC: {
			size_t consumed = 0;
			if (!DecompressAny(data, size, 0, &r->output, &r->compression, &consumed, &r->error)) {
				return;
			}
			r->steps.push_back(r->compression);
			if (!IsElf(r->output.data(), r->output.size())) {
				r->warnings.push_back("output is not an ELF");
			}
			r->status = DecodeStatus::Ok;
			return;
		}
		default:
			r->error = std::string("not a PSP executable (") + FileKindName(kind) + ")";
			return;
		}
	}
	r->error = "too many nested wrappers";
}

std::vector<size_t> FindEmbeddedModules(const u8 *data, size_t size, size_t start) {
	std::vector<size_t> found;
	size_t pos = start;
	while (pos + PSP_HEADER_SIZE <= size) {
		const u8 *hit = (const u8 *)memchr(data + pos, '~', size - PSP_HEADER_SIZE - pos + 1);
		if (!hit) {
			break;
		}
		pos = hit - data;
		PspHeader h;
		bool ok = memcmp(hit, "~PSP", 4) == 0 && ParsePspHeader(hit, size - pos, &h);
		ok = ok && h.pspSize >= PSP_HEADER_SIZE && h.pspSize <= size - pos && h.elfSize != 0;
		if (ok) {
			// A real header names its module in printable ASCII.
			const u8 *name = hit + 0x0A;
			int len = 0;
			while (len < 28 && name[len] != 0) {
				ok = ok && name[len] >= 0x20 && name[len] < 0x7F;
				len++;
			}
			ok = ok && len > 0;
		}
		if (ok) {
			found.push_back(pos);
			pos += h.pspSize;
		} else {
			pos++;
		}
	}
	return found;
}

Json DescribePspHeader(const PspHeader &h) {
	Json j = Json::Object();
	j.Set("module_name", h.modname);
	std::string attr = Hex32(h.attribute);
	attr += (h.attribute & 0x1000) ? " (kernel)" : (h.attribute & 0x0800) ? " (vsh)" : " (user)";
	j.Set("attribute", attr);
	std::string comp = Hex32(h.compAttribute);
	if (h.compAttribute & 1) {
		comp += (h.compAttribute & 0xF00) ? " (compressed, KL4E/KL3E)" : " (compressed)";
	} else {
		comp += " (not compressed)";
	}
	j.Set("comp_attribute", comp);
	j.Set("module_version", StringPrintf("%d.%d", h.moduleVerHi, h.moduleVerLo));
	j.Set("header_version", h.version);
	j.Set("nsegments", h.nsegments);
	j.Set("elf_size", h.elfSize);
	j.Set("psp_size", h.pspSize);
	j.Set("comp_size", (long long)h.compSize);
	j.Set("entry", Hex32(h.entry));
	j.Set("modinfo_offset", Hex32(h.modinfoOffset));
	j.Set("bss_size", (long long)h.bssSize);
	j.Set("devkit_version", Hex32(h.devkitVersion));
	j.Set("decrypt_mode", h.decryptMode);
	j.Set("tag", Hex32(h.tag));
	j.Set("oe_tag", Hex32(h.oeTag));
	const int known = PrxTagKnown(h.tag);
	j.Set("tag_key", known == 0 ? "unknown" : known == 1 ? "144-byte (types 0/1)" : known == 2 ? "16-byte (types 2/5/6/9)" : "both tables");
	Json segs = Json::Array();
	for (int i = 0; i < 4 && i < h.nsegments; i++) {
		Json s = Json::Object();
		s.Set("address", Hex32(h.segAddress[i]));
		s.Set("size", (long long)h.segSize[i]);
		s.Set("align", h.segAlign[i]);
		segs.Push(s);
	}
	j.Set("segments", segs);
	return j;
}

static Json DescribeDecode(const u8 *data, size_t size, const DecodeOptions &options) {
	DecodeResult r;
	DecodeExecutable(data, size, options, &r);
	Json j = Json::Object();
	j.Set("status", r.status == DecodeStatus::Ok ? "ok" : r.status == DecodeStatus::NeedsKey ? "needs-key" : "failed");
	if (!r.error.empty()) {
		j.Set("error", r.error);
	}
	Json steps = Json::Array();
	for (const std::string &s : r.steps) {
		steps.Push(s);
	}
	j.Set("steps", steps);
	if (r.status == DecodeStatus::Ok) {
		j.Set("encrypted", r.encrypted);
		j.Set("cfw_packed", r.cfwPacked);
		if (r.prxType >= 0) {
			j.Set("prx_type", r.prxType);
		}
		j.Set("compression", r.compression);
		j.Set("output_size", (unsigned long long)r.output.size());
		j.Set("output_sha1", Sha1Hex(r.output.data(), r.output.size()));
		if (IsElf(r.output.data(), r.output.size())) {
			j.Set("elf", DescribeElf(r.output.data(), r.output.size()));
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

Json DescribeFile(const u8 *data, size_t size, const DecodeOptions &options, size_t baseOffset) {
	Json j = Json::Object();
	const FileKind kind = SniffKind(data, size);
	j.Set("kind", FileKindName(kind));
	j.Set("size", (unsigned long long)size);
	switch (kind) {
	case FileKind::Elf:
		j.Set("elf", DescribeElf(data, size));
		break;
	case FileKind::PspModule: {
		PspHeader h;
		if (ParsePspHeader(data, size, &h)) {
			j.Set("psp_header", DescribePspHeader(h));
		}
		j.Set("decode", DescribeDecode(data, size, options));
		break;
	}
	case FileKind::SceWrapped: {
		const u32 headerSize = size >= 8 ? rd32(data + 4) : 0;
		j.Set("sce_header_size", Hex32(headerSize));
		if (headerSize >= 8 && headerSize < size) {
			j.Set("inner", DescribeFile(data + headerSize, size - headerSize, options, baseOffset + headerSize));
		}
		break;
	}
	case FileKind::Edat: {
		EdatHeader e;
		ParseEdat(data, size, &e);
		Json ej = Json::Object();
		ej.Set("content_id", e.contentId);
		ej.Set("key_mode", e.keyMode);
		ej.Set("flags", Hex32(e.flags));
		ej.Set("needs_licensee_key", (e.flags & 1) != 0);
		ej.Set("payload_offset", Hex32(e.payloadOffset));
		j.Set("edat", ej);
		if (e.payloadOffset >= 0x90 && e.payloadOffset < size) {
			PspHeader h;
			if (ParsePspHeader(data + e.payloadOffset, size - e.payloadOffset, &h)) {
				j.Set("psp_header", DescribePspHeader(h));
			}
		}
		j.Set("decode", DescribeDecode(data, size, options));
		break;
	}
	case FileKind::Pbp: {
		PbpFile pbp;
		std::string err;
		if (!pbp.Parse(data, size, &err)) {
			j.Set("error", err);
			break;
		}
		j.Set("version", Hex32(pbp.version));
		Json sections = Json::Array();
		for (int i = 0; i < PbpFile::kSections; i++) {
			Json s = Json::Object();
			s.Set("name", PbpFile::kNames[i]);
			s.Set("offset", Hex32(pbp.offset[i]));
			s.Set("size", pbp.size[i]);
			s.Set("kind", pbp.size[i] ? FileKindName(SniffKind(data + pbp.offset[i], pbp.size[i])) : "empty");
			sections.Push(s);
		}
		j.Set("sections", sections);
		std::map<std::string, SfoEntry> sfo;
		if (pbp.size[PbpFile::PARAM_SFO] && ParseSfo(data + pbp.offset[PbpFile::PARAM_SFO], pbp.size[PbpFile::PARAM_SFO], &sfo)) {
			j.Set("param_sfo", SfoToJson(sfo));
		}
		if (pbp.size[PbpFile::DATA_PSP]) {
			j.Set("data_psp", DescribeFile(data + pbp.offset[PbpFile::DATA_PSP], pbp.size[PbpFile::DATA_PSP], options, baseOffset + pbp.offset[PbpFile::DATA_PSP]));
		}
		if (pbp.size[PbpFile::DATA_PSAR]) {
			const u8 *psar = data + pbp.offset[PbpFile::DATA_PSAR];
			const u32 psarSize = pbp.size[PbpFile::DATA_PSAR];
			const FileKind pk = SniffKind(psar, psarSize);
			if (pk == FileKind::Psar) {
				j.Set("data_psar", DescribePsar(psar, psarSize));
			} else {
				Json pj = Json::Object();
				pj.Set("kind", FileKindName(pk));
				pj.Set("size", psarSize);
				if (pk == FileKind::NpUmdImg) {
					pj.Set("note", "PSN game image (NPDRM-encrypted ISO), not handled by this tool");
				}
				j.Set("data_psar", pj);
			}
		}
		break;
	}
	case FileKind::Psar:
		j.Set("psar", DescribePsar(data, size));
		break;
	case FileKind::Sfo: {
		std::map<std::string, SfoEntry> sfo;
		if (ParseSfo(data, size, &sfo)) {
			j.Set("param_sfo", SfoToJson(sfo));
		}
		break;
	}
	case FileKind::Gzip:
	case FileKind::Zlib:
	case FileKind::KL4E:
	case FileKind::KL3E:
	case FileKind::LZRC:
		j.Set("decode", DescribeDecode(data, size, options));
		break;
	default:
		break;
	}
	if (kind == FileKind::Elf || kind == FileKind::Unknown) {
		const std::vector<size_t> embedded = FindEmbeddedModules(data, size, kind == FileKind::Elf ? 4 : 0);
		if (!embedded.empty()) {
			Json list = Json::Array();
			for (size_t off : embedded) {
				PspHeader h;
				ParsePspHeader(data + off, size - off, &h);
				Json m = Json::Object();
				m.Set("offset", Hex32((u32)off));
				if (baseOffset != 0) {
					m.Set("file_offset", Hex32((u32)(baseOffset + off)));
				}
				m.Set("module_name", h.modname);
				m.Set("psp_size", h.pspSize);
				m.Set("elf_size", h.elfSize);
				m.Set("tag", Hex32(h.tag));
				list.Push(m);
			}
			j.Set("embedded_modules", list);
		}
	}
	return j;
}
