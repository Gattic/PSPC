// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 PSPC contributors
//
// Ghidra post-analysis script: exports function boundaries, call graph, jump
// tables, string references and code pointers found in data as JSON
// (format documented in docs/analysis-json.md), and the decompiled C of every
// function.
//
// Usage (headless): -postScript PspcExportAnalysis.java <out_root> [nodecomp] [stem=<name>]
//   writes <out_root>/analysis/<stem>.json and <out_root>/decomp/<stem>.c
//   where <stem> defaults to the program name without its extension.
//@category PSPC

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.framework.Application;
import ghidra.program.database.mem.FileBytes;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.Symbol;

public class PspcExportAnalysis extends GhidraScript {

	private static final int FORMAT_VERSION = 1;
	private long imageBase;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("usage: PspcExportAnalysis.java <out_root> [nodecomp] [stem=<name>]");
			return;
		}
		File outRoot = new File(args[0]);
		boolean decomp = true;
		String stem = currentProgram.getName();
		int dot = stem.lastIndexOf('.');
		if (dot > 0) {
			stem = stem.substring(0, dot);
		}
		for (int i = 1; i < args.length; i++) {
			if (args[i].equals("nodecomp")) {
				decomp = false;
			}
			else if (args[i].startsWith("stem=")) {
				stem = args[i].substring(5);
			}
		}
		imageBase = currentProgram.getImageBase().getOffset();
		String fmt = currentProgram.getExecutableFormat();
		if (fmt != null && fmt.toLowerCase().contains("raw") && currentProgram.getMinAddress() != null) {
			// BinaryLoader leaves the image base at 0; the payload's load address is its base.
			imageBase = currentProgram.getMinAddress().getOffset();
		}

		File jsonDir = new File(outRoot, "analysis");
		File cDir = new File(outRoot, "decomp");
		jsonDir.mkdirs();
		cDir.mkdirs();

		JsonObject root = buildJson();
		Gson gson = new GsonBuilder().setPrettyPrinting().disableHtmlEscaping().create();
		File jf = new File(jsonDir, stem + ".json");
		try (Writer w = new OutputStreamWriter(new FileOutputStream(jf), StandardCharsets.UTF_8)) {
			gson.toJson(root, w);
			w.write("\n");
		}
		println("PspcExportAnalysis: wrote " + jf);

		if (decomp) {
			File cf = new File(cDir, stem + ".c");
			writeDecomp(cf);
			println("PspcExportAnalysis: wrote " + cf);
		}
	}

	// ------------------------------------------------------------------

	private String hex(Address a) {
		return String.format("0x%08X", a.getOffset());
	}

	private String hex(long v) {
		return String.format("0x%08X", v);
	}

	private boolean isLoaded(Address a) {
		MemoryBlock b = currentProgram.getMemory().getBlock(a);
		return b != null && b.isLoaded();
	}

	private boolean isExec(Address a) {
		MemoryBlock b = currentProgram.getMemory().getBlock(a);
		return b != null && b.isExecute();
	}

	private String sha1OfOriginal() {
		try {
			List<FileBytes> fbs = currentProgram.getMemory().getAllFileBytes();
			if (fbs.isEmpty()) {
				return null;
			}
			FileBytes fb = fbs.get(0);
			MessageDigest md = MessageDigest.getInstance("SHA-1");
			byte[] buf = new byte[65536];
			long size = fb.getSize();
			for (long off = 0; off < size; off += buf.length) {
				int n = (int) Math.min(buf.length, size - off);
				fb.getOriginalBytes(off, buf, 0, n);
				md.update(buf, 0, n);
			}
			StringBuilder sb = new StringBuilder();
			for (byte b : md.digest()) {
				sb.append(String.format("%02x", b));
			}
			return sb.toString();
		}
		catch (Exception e) {
			return null;
		}
	}

	private JsonObject buildJson() throws Exception {
		JsonObject root = new JsonObject();
		root.addProperty("format", "pspc-analysis");
		root.addProperty("version", FORMAT_VERSION);
		root.addProperty("generator", "tools/ghidra/PspcExportAnalysis.java");
		root.addProperty("ghidra_version", Application.getApplicationVersion());

		JsonObject prog = new JsonObject();
		prog.addProperty("name", currentProgram.getName());
		prog.addProperty("executable_path", currentProgram.getExecutablePath());
		prog.addProperty("sha1", sha1OfOriginal());
		prog.addProperty("language", currentProgram.getLanguageID().getIdAsString());
		prog.addProperty("compiler_spec", currentProgram.getCompilerSpec().getCompilerSpecID().getIdAsString());
		prog.addProperty("image_base", hex(imageBase));
		root.add("program", prog);

		root.add("module", moduleInfo());
		root.add("segments", segments());

		Listing listing = currentProgram.getListing();
		ReferenceManager rm = currentProgram.getReferenceManager();
		JsonArray funcs = new JsonArray();
		JsonArray imports = new JsonArray();
		JsonArray exports = new JsonArray();
		FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Function f = it.next();
			JsonObject fo = functionJson(f, listing, rm);
			funcs.add(fo);
			String plate = getPlateComment(f.getEntryPoint());
			if (plate != null) {
				for (String line : plate.split("\n")) {
					JsonObject e = parseNidComment(line, f);
					if (e == null) {
						continue;
					}
					if (line.startsWith("Import")) {
						imports.add(e);
					}
					else {
						exports.add(e);
					}
				}
			}
		}
		// Exported variables carry their comment on a data label.
		for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
			Address a = s.getAddress();
			if (!a.isMemoryAddress() || isExec(a) || !isLoaded(a)) {
				continue;
			}
			String plate = getPlateComment(a);
			if (plate == null || !s.isPrimary()) {
				continue;
			}
			for (String line : plate.split("\n")) {
				if (line.startsWith("Export")) {
					JsonObject e = parseNidComment(line, null);
					if (e != null) {
						e.addProperty("address", hex(a));
						e.addProperty("name", s.getName());
						e.addProperty("kind", "variable");
						exports.add(e);
					}
				}
			}
		}
		root.add("imports", imports);
		root.add("exports", exports);
		root.add("functions", funcs);
		root.add("code_pointers", codePointers(rm));
		return root;
	}

	private JsonObject parseNidComment(String line, Function f) {
		// "Import <lib> NID 0x12345678" / "Export <lib> NID 0x12345678 ..."
		String[] p = line.trim().split("\\s+");
		if (p.length < 4 || !(p[0].equals("Import") || p[0].equals("Export")) || !p[2].equals("NID")) {
			return null;
		}
		JsonObject e = new JsonObject();
		e.addProperty("library", p[1]);
		e.addProperty("nid", p[3].toUpperCase().replace("0X", "0x"));
		e.addProperty("name_known", !line.contains("(name unknown)"));
		if (f != null) {
			e.addProperty("name", f.getName());
			e.addProperty(p[0].equals("Import") ? "stub" : "address", hex(f.getEntryPoint()));
			if (p[0].equals("Export")) {
				e.addProperty("kind", "function");
			}
		}
		return e;
	}

	private JsonObject moduleInfo() {
		JsonObject m = new JsonObject();
		Symbol s = getSymbol("module_info", null);
		if (s == null) {
			return m;
		}
		Address a = s.getAddress();
		try {
			m.addProperty("address", hex(a));
			m.addProperty("attribute", String.format("0x%04X", getShort(a) & 0xFFFF));
			m.addProperty("version", String.format("%d.%d", getByte(a.add(3)) & 0xFF, getByte(a.add(2)) & 0xFF));
			StringBuilder sb = new StringBuilder();
			for (int i = 0; i < 28; i++) {
				byte c = getByte(a.add(4 + i));
				if (c == 0) {
					break;
				}
				sb.append((char) (c & 0xFF));
			}
			m.addProperty("name", sb.toString());
			m.addProperty("gp", hex(getInt(a.add(0x20)) & 0xFFFFFFFFL));
		}
		catch (Exception e) {
			m.addProperty("error", e.getMessage());
		}
		return m;
	}

	private JsonArray segments() {
		JsonArray arr = new JsonArray();
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (!b.getStart().isMemoryAddress() || !b.isLoaded()) {
				continue;
			}
			JsonObject o = new JsonObject();
			o.addProperty("name", b.getName());
			o.addProperty("start", hex(b.getStart()));
			o.addProperty("size", b.getSize());
			o.addProperty("read", b.isRead());
			o.addProperty("write", b.isWrite());
			o.addProperty("execute", b.isExecute());
			o.addProperty("initialized", b.isInitialized());
			arr.add(o);
		}
		return arr;
	}

	private JsonObject functionJson(Function f, Listing listing, ReferenceManager rm) {
		JsonObject o = new JsonObject();
		Address entry = f.getEntryPoint();
		AddressSetView body = f.getBody();
		o.addProperty("entry", hex(entry));
		o.addProperty("offset", hex(entry.getOffset() - imageBase));
		o.addProperty("name", f.getName());
		o.addProperty("name_source", f.getSymbol().getSource().toString().toLowerCase());
		o.addProperty("size", body.getNumAddresses());
		JsonArray ranges = new JsonArray();
		for (AddressRange r : body.getAddressRanges()) {
			JsonArray rr = new JsonArray();
			rr.add(hex(r.getMinAddress()));
			rr.add(hex(r.getMaxAddress().getOffset() + 1));
			ranges.add(rr);
		}
		o.add("ranges", ranges);
		boolean stub = false;
		String plate = getPlateComment(entry);
		if (plate != null && plate.contains("Import ")) {
			stub = true;
		}
		o.addProperty("is_import_stub", stub);
		o.addProperty("is_thunk", f.isThunk());
		o.addProperty("no_return", f.hasNoReturn());
		o.addProperty("signature", f.getPrototypeString(false, false));

		JsonArray calls = new JsonArray();
		JsonArray jumpTables = new JsonArray();
		JsonArray strings = new JsonArray();
		Set<String> seenStr = new LinkedHashSet<>();
		int ninsn = 0;
		InstructionIterator ii = listing.getInstructions(body, true);
		while (ii.hasNext()) {
			Instruction ins = ii.next();
			ninsn++;
			FlowType ft = ins.getFlowType();
			Reference[] refs = rm.getReferencesFrom(ins.getAddress());
			if (ft.isCall()) {
				boolean any = false;
				for (Reference r : refs) {
					if (r.getReferenceType().isCall()) {
						calls.add(callJson(ins, r.getToAddress(), ft.isComputed() ? "indirect" : "call"));
						any = true;
					}
				}
				if (!any) {
					JsonObject c = new JsonObject();
					c.addProperty("from", hex(ins.getAddress()));
					c.addProperty("kind", "indirect");
					c.addProperty("insn", ins.toString());
					calls.add(c);
				}
			}
			else if (ft.isJump() && ft.isComputed()) {
				JsonArray targets = new JsonArray();
				for (Reference r : refs) {
					RefType t = r.getReferenceType();
					if (t.isJump() || t.isFlow()) {
						targets.add(hex(r.getToAddress()));
					}
				}
				if (targets.size() > 0) {
					JsonObject jt = new JsonObject();
					jt.addProperty("at", hex(ins.getAddress()));
					jt.addProperty("insn", ins.toString());
					jt.add("targets", targets);
					jumpTables.add(jt);
				}
			}
			else if (ft.isJump() && !ft.isConditional()) {
				for (Address t : ins.getFlows()) {
					if (!body.contains(t)) {
						Function tf = getFunctionAt(t);
						if (tf != null) {
							calls.add(callJson(ins, t, "tail"));
						}
					}
				}
			}
			for (Reference r : refs) {
				if (!r.getReferenceType().isData() && !r.getReferenceType().isRead()) {
					continue;
				}
				Address to = r.getToAddress();
				if (!to.isMemoryAddress()) {
					continue;
				}
				String s = stringAt(to);
				if (s != null && seenStr.add(hex(ins.getAddress()) + hex(to))) {
					JsonObject so = new JsonObject();
					so.addProperty("from", hex(ins.getAddress()));
					so.addProperty("address", hex(to));
					so.addProperty("value", s);
					strings.add(so);
				}
			}
		}
		o.addProperty("instructions", ninsn);
		o.add("calls", calls);
		o.add("jump_tables", jumpTables);
		o.add("string_refs", strings);
		return o;
	}

	private JsonObject callJson(Instruction ins, Address to, String kind) {
		JsonObject c = new JsonObject();
		c.addProperty("from", hex(ins.getAddress()));
		c.addProperty("to", hex(to));
		c.addProperty("kind", kind);
		Function tf = getFunctionAt(to);
		if (tf != null) {
			c.addProperty("target", tf.getName());
		}
		return c;
	}

	private String stringAt(Address a) {
		Data d = getDataAt(a);
		if (d != null && d.hasStringValue()) {
			Object v = d.getValue();
			return v == null ? null : v.toString();
		}
		if (d != null && d.isDefined()) {
			return null;
		}
		// Undefined bytes: accept a printable NUL-terminated run of >= 4 chars.
		MemoryBlock b = currentProgram.getMemory().getBlock(a);
		if (b == null || b.isExecute() || !b.isInitialized()) {
			return null;
		}
		StringBuilder sb = new StringBuilder();
		try {
			for (int i = 0; i < 512; i++) {
				int c = getByte(a.add(i)) & 0xFF;
				if (c == 0) {
					return sb.length() >= 4 ? sb.toString() : null;
				}
				if ((c < 0x20 || c > 0x7E) && c != '\n' && c != '\r' && c != '\t') {
					return null;
				}
				sb.append((char) c);
			}
		}
		catch (Exception e) {
			return null;
		}
		return null;
	}

	private JsonArray codePointers(ReferenceManager rm) {
		// References from non-executable memory into executable memory:
		// function pointer tables, callback registrations, export tables.
		AddressSet data = new AddressSet();
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.getStart().isMemoryAddress() && b.isLoaded() && !b.isExecute()) {
				data.add(b.getStart(), b.getEnd());
			}
		}
		Map<Long, JsonObject> out = new TreeMap<>();
		var srcIt = rm.getReferenceSourceIterator(data, true);
		while (srcIt.hasNext()) {
			Address from = srcIt.next();
			for (Reference r : rm.getReferencesFrom(from)) {
				Address to = r.getToAddress();
				if (to.isMemoryAddress() && isExec(to)) {
					JsonObject o = new JsonObject();
					o.addProperty("from", hex(from));
					o.addProperty("to", hex(to));
					Function tf = getFunctionAt(to);
					if (tf != null) {
						o.addProperty("target", tf.getName());
					}
					out.put(from.getOffset(), o);
				}
			}
		}
		JsonArray arr = new JsonArray();
		out.values().forEach(arr::add);
		return arr;
	}

	// ------------------------------------------------------------------

	private void writeDecomp(File cf) throws Exception {
		DecompInterface di = new DecompInterface();
		DecompileOptions opts = new DecompileOptions();
		di.setOptions(opts);
		di.toggleCCode(true);
		di.toggleSyntaxTree(false);
		di.setSimplificationStyle("decompile");
		if (!di.openProgram(currentProgram)) {
			printerr("PspcExportAnalysis: decompiler failed to open program: " + di.getLastMessage());
			return;
		}
		int ok = 0, failed = 0;
		try (Writer w = new OutputStreamWriter(new FileOutputStream(cf), StandardCharsets.UTF_8)) {
			w.write("/*\n * Decompiled by Ghidra " + Application.getApplicationVersion()
				+ " (tools/ghidra/PspcExportAnalysis.java)\n * from " + currentProgram.getName()
				+ ", image base " + hex(imageBase)
				+ ".\n * Derived from copyrighted code: keep under private/, never commit or publish.\n */\n\n");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				if (f.isThunk()) {
					Function t = f.getThunkedFunction(true);
					w.write("/* " + hex(f.getEntryPoint()) + " " + f.getName() + ": "
						+ (t != null && t.isExternal() ? "import stub" : "thunk to " + (t == null ? "?" : t.getName()))
						+ " */\n\n");
					continue;
				}
				DecompileResults res = di.decompileFunction(f, 120, monitor);
				w.write("/* " + hex(f.getEntryPoint()) + " (offset " + hex(f.getEntryPoint().getOffset() - imageBase)
					+ ") " + f.getName() + " */\n");
				if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
					w.write(res.getDecompiledFunction().getC());
					w.write("\n");
					ok++;
				}
				else {
					w.write("/* decompilation failed: " + (res == null ? "null" : res.getErrorMessage()) + " */\n\n");
					failed++;
				}
			}
		}
		finally {
			di.dispose();
		}
		println("PspcExportAnalysis: decompiled " + ok + " functions, " + failed + " failed");
	}
}
