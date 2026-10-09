// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 PSPC contributors
//
// Ghidra pre-analysis script for PSP PRX files loaded with ghidra-allegrex.
//
// Locates SceModuleInfo, walks the library export (.lib.ent) and import
// (.lib.stub) tables, and names every export and import stub from a NID
// database (data/nids.csv, columns library,nid,name,source,hash_ok).
// Import stubs become thunks of external functions in a per-library external
// namespace, so the decompiler shows real call targets and infers arguments
// at the call sites.  Unknown NIDs are named <library>_<NID>.
//
// Usage (headless): -preScript PspcApplyNids.java <path/to/nids.csv>
//@category PSPC

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.database.mem.FileBytes;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.ExternalLocation;
import ghidra.program.model.symbol.ExternalManager;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class PspcApplyNids extends GhidraScript {

	private final Map<String, Map<Long, String>> byLib = new HashMap<>();
	private final Map<Long, Set<String>> byNid = new HashMap<>();
	private int namedImports, unnamedImports, namedExports, unnamedExports;

	private static final Map<Long, String> SYSLIB = new HashMap<>();
	static {
		SYSLIB.put(0xD3744BE0L, "module_bootstart");
		SYSLIB.put(0x2F064FA6L, "module_reboot_before");
		SYSLIB.put(0xADF12745L, "module_reboot_phase");
		SYSLIB.put(0xD632ACDBL, "module_start");
		SYSLIB.put(0xCEE8593CL, "module_stop");
		SYSLIB.put(0xF01D73A7L, "module_info");
		SYSLIB.put(0x0F7C276CL, "module_start_thread_parameter");
		SYSLIB.put(0xCF0CC697L, "module_stop_thread_parameter");
		SYSLIB.put(0x11B97506L, "module_sdk_version");
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length > 0 && new java.io.File(args[0]).isFile()) {
			loadNids(args[0]);
		}
		else {
			println("PspcApplyNids: no NID database given, using <library>_<NID> names");
		}
		Address mi = findModuleInfo();
		if (mi == null) {
			// Raw code blob (e.g. a reboot payload): start disassembly at the
			// lowest address, which is where such payloads are entered.
			Address start = currentProgram.getMinAddress();
			if (start != null && currentProgram.getExecutableFormat() != null
				&& currentProgram.getExecutableFormat().toLowerCase().contains("raw")) {
				makeFunction(start, "payload_entry");
				println("PspcApplyNids: raw binary, created payload_entry at " + start);
			}
			else {
				println("PspcApplyNids: SceModuleInfo not found, nothing to do");
			}
			return;
		}
		String modName = readCString(mi.add(4), 28);
		println(String.format("PspcApplyNids: module '%s', module info at %s", modName, mi));
		createLabel(mi, "module_info", true, SourceType.IMPORTED);
		setPlateComment(mi, "SceModuleInfo of " + modName);
		applyExports(mi);
		applyImports(mi);
		println(String.format("PspcApplyNids: imports %d named / %d unnamed, exports %d named / %d unnamed",
			namedImports, unnamedImports, namedExports, unnamedExports));
	}

	// ------------------------------------------------------------------
	// NID database

	private void loadNids(String path) throws IOException {
		int rows = 0;
		try (BufferedReader r = new BufferedReader(new FileReader(path))) {
			String header = r.readLine();
			if (header == null) {
				return;
			}
			String line;
			while ((line = r.readLine()) != null) {
				String[] f = splitCsv(line);
				if (f.length < 3) {
					continue;
				}
				long nid;
				try {
					nid = Long.parseLong(f[1].replace("0x", "").replace("0X", ""), 16);
				}
				catch (NumberFormatException e) {
					continue;
				}
				byLib.computeIfAbsent(f[0], k -> new HashMap<>()).put(nid, f[2]);
				byNid.computeIfAbsent(nid, k -> new HashSet<>()).add(f[2]);
				rows++;
			}
		}
		println("PspcApplyNids: loaded " + rows + " NIDs from " + path);
	}

	private static String[] splitCsv(String line) {
		// Minimal CSV: fields may be quoted with "", quotes doubled inside.
		java.util.ArrayList<String> out = new java.util.ArrayList<>();
		StringBuilder cur = new StringBuilder();
		boolean q = false;
		for (int i = 0; i < line.length(); i++) {
			char c = line.charAt(i);
			if (q) {
				if (c == '"') {
					if (i + 1 < line.length() && line.charAt(i + 1) == '"') {
						cur.append('"');
						i++;
					}
					else {
						q = false;
					}
				}
				else {
					cur.append(c);
				}
			}
			else if (c == '"') {
				q = true;
			}
			else if (c == ',') {
				out.add(cur.toString());
				cur.setLength(0);
			}
			else {
				cur.append(c);
			}
		}
		out.add(cur.toString());
		return out.toArray(new String[0]);
	}

	private String lookup(String lib, long nid) {
		if (lib == null && SYSLIB.containsKey(nid)) {
			return SYSLIB.get(nid);
		}
		Map<Long, String> m = byLib.get(lib == null ? "syslib" : lib);
		if (m != null && m.containsKey(nid)) {
			return m.get(nid);
		}
		Set<String> c = byNid.get(nid);
		if (c != null && c.size() == 1) {
			return c.iterator().next();
		}
		return SYSLIB.get(nid);
	}

	// ------------------------------------------------------------------
	// Module info

	private Address findModuleInfo() throws Exception {
		Memory mem = currentProgram.getMemory();
		MemoryBlock b = mem.getBlock(".rodata.sceModuleInfo");
		if (b != null) {
			return b.getStart();
		}
		// Section-less PRX: phdr[0].p_paddr holds the file offset of the
		// module info (bit 31 marks kernel modules).
		List<FileBytes> fbs = mem.getAllFileBytes();
		if (fbs.isEmpty()) {
			return null;
		}
		FileBytes fb = fbs.get(0);
		if (fb.getSize() < 0x54 || fileU32(fb, 0) != 0x464C457FL) {
			return null; // not an ELF (raw binary import)
		}
		long phoff = fileU32(fb, 0x1C);
		long paddr = fileU32(fb, phoff + 12) & 0x7FFFFFFFL;
		List<Address> addrs = mem.locateAddressesForFileBytesOffset(fb, paddr);
		return addrs.isEmpty() ? null : addrs.get(0);
	}

	private static long fileU32(FileBytes fb, long off) throws IOException {
		byte[] b = new byte[4];
		fb.getOriginalBytes(off, b);
		return (b[0] & 0xFFL) | ((b[1] & 0xFFL) << 8) | ((b[2] & 0xFFL) << 16) | ((b[3] & 0xFFL) << 24);
	}

	private long u32(Address a) throws Exception {
		return getInt(a) & 0xFFFFFFFFL;
	}

	private String readCString(Address a, int max) {
		StringBuilder sb = new StringBuilder();
		try {
			for (int i = 0; i < max; i++) {
				byte c = getByte(a.add(i));
				if (c == 0) {
					break;
				}
				sb.append((char) (c & 0xFF));
			}
		}
		catch (Exception e) {
			// unreadable memory: return what we have
		}
		return sb.toString();
	}

	// ------------------------------------------------------------------
	// Exports

	private void applyExports(Address mi) throws Exception {
		long top = u32(mi.add(0x24)), end = u32(mi.add(0x28));
		long va = top;
		int guard = 0;
		while (va < end && guard++ < 512) {
			Address e = toAddr(va);
			long namePtr = u32(e);
			int size = getByte(e.add(8)) & 0xFF;
			int nvars = getByte(e.add(9)) & 0xFF;
			int nfuncs = getShort(e.add(10)) & 0xFFFF;
			long tbl = u32(e.add(12));
			if (size < 4) {
				break;
			}
			String lib = namePtr != 0 ? readCString(toAddr(namePtr), 64) : null;
			String libLabel = lib == null ? "syslib" : lib;
			int n = nfuncs + nvars;
			for (int i = 0; i < n; i++) {
				long nid = u32(toAddr(tbl + 4L * i));
				long addr = u32(toAddr(tbl + 4L * (n + i)));
				String name = lookup(lib, nid);
				boolean known = name != null;
				if (!known) {
					name = String.format("%s_%08X", libLabel, nid);
				}
				Address target = toAddr(addr);
				String cmt = String.format("Export %s NID 0x%08X%s", libLabel, nid, known ? "" : " (name unknown)");
				if (i < nfuncs) {
					MemoryBlock blk = currentProgram.getMemory().getBlock(target);
					if (blk == null || !blk.isExecute()) {
						continue;
					}
					Function f = makeFunction(target, name);
					if (f != null) {
						appendPlate(target, cmt);
					}
				}
				else {
					createLabel(target, name, true, SourceType.IMPORTED);
					appendPlate(target, cmt);
				}
				if (known) {
					namedExports++;
				}
				else {
					unnamedExports++;
				}
			}
			va += size * 4L;
		}
	}

	// ------------------------------------------------------------------
	// Imports

	private void applyImports(Address mi) throws Exception {
		long top = u32(mi.add(0x2C)), end = u32(mi.add(0x30));
		long va = top;
		int guard = 0;
		ExternalManager em = currentProgram.getExternalManager();
		while (va < end && guard++ < 1024) {
			Address e = toAddr(va);
			long namePtr = u32(e);
			int size = getByte(e.add(8)) & 0xFF;
			int nfuncs = getShort(e.add(10)) & 0xFFFF;
			long nidTbl = u32(e.add(12));
			long stubTbl = u32(e.add(16));
			if (size < 5) {
				break;
			}
			String lib = namePtr != 0 ? readCString(toAddr(namePtr), 64) : "unknown";
			for (int i = 0; i < nfuncs; i++) {
				long nid = u32(toAddr(nidTbl + 4L * i));
				Address stub = toAddr(stubTbl + 8L * i);
				String name = lookup(lib, nid);
				boolean known = name != null;
				if (!known) {
					name = String.format("%s_%08X", lib, nid);
				}
				Function f = makeFunction(stub, name);
				if (f == null) {
					continue;
				}
				try {
					ExternalLocation loc = em.addExtFunction(lib, name, null, SourceType.IMPORTED);
					Function ext = loc.getFunction();
					if (ext != null) {
						f.setThunkedFunction(ext);
					}
				}
				catch (Exception ex) {
					printerr("PspcApplyNids: cannot make thunk for " + name + ": " + ex.getMessage());
				}
				appendPlate(stub, String.format("Import %s NID 0x%08X%s", lib, nid, known ? "" : " (name unknown)"));
				if (known) {
					namedImports++;
				}
				else {
					unnamedImports++;
				}
			}
			va += size * 4L;
		}
	}

	// ------------------------------------------------------------------

	private Function makeFunction(Address a, String name) throws Exception {
		if (getInstructionAt(a) == null) {
			DisassembleCommand cmd = new DisassembleCommand(new AddressSet(a), null, true);
			cmd.applyTo(currentProgram, monitor);
		}
		Function f = getFunctionAt(a);
		if (f == null) {
			f = createFunction(a, name);
		}
		if (f == null) {
			printerr("PspcApplyNids: cannot create function at " + a + " for " + name);
			return null;
		}
		Symbol s = f.getSymbol();
		if (name.equals(s.getName()) && s.getSource() != SourceType.IMPORTED) {
			f.setName(name, SourceType.IMPORTED);
		}
		else if (!name.equals(s.getName())) {
			// A second name for the same address (e.g. an export that is also
			// reachable under another library): keep the first, add a label.
			if (s.getSource() == SourceType.IMPORTED) {
				createLabel(a, name, false, SourceType.IMPORTED);
			}
			else {
				f.setName(name, SourceType.IMPORTED);
			}
		}
		return f;
	}

	private void appendPlate(Address a, String text) {
		String old = getPlateComment(a);
		if (old == null || old.isEmpty()) {
			setPlateComment(a, text);
		}
		else if (!old.contains(text)) {
			setPlateComment(a, old + "\n" + text);
		}
	}
}
