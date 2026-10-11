// SPDX-License-Identifier: GPL-2.0-or-later
// Large disc image probe (docs/large-iso-plan.md 2.4): runs the commands in disc0:/PROBES.TXT
// and prints one line per command, "<command> => <what the firmware calls returned>". The disc
// is made by tools/make_big_iso.py and checked by tools/large_image_test.py, which works out the
// expected lines from the image itself.
//
//   open PATH                 sceIoOpen, kept as the current file
//   seek WHENCE OFFSET        sceIoLseek (64-bit): the new position
//   seek32 WHENCE OFFSET      sceIoLseek32 (int offset and result)
//   read                      sceIoRead of 64 bytes at the current position
//   close
//   stat PATH                 sceIoGetstat: st_size
//   umd SECTOR BYTE           64 bytes at BYTE of a sector read through umd0:
//   lbn START SIZE BYTE       64 bytes at BYTE of disc0:/sce_lbn<START>_size<SIZE>
//   lastlba                   devctl 0x01F20003 on umd0:: the disc's last sector
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspumd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("large_image", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

static char script[16384];
static unsigned char buf[2048] __attribute__((aligned(64)));

// 16 hex digits: newlib's printf may lack %ll.
static const char *Hex64(long long v) {
	static char s[19];
	unsigned long long u = (unsigned long long)v;
	snprintf(s, sizeof(s), "0x%08x%08x", (unsigned)(u >> 32), (unsigned)u);
	return s;
}

// Bytes as text: printable ASCII as is, anything else as '?'.
static void Show(const unsigned char *p, int n) {
	char s[65];
	int i;
	if (n < 0) n = 0;
	if (n > 64) n = 64;
	for (i = 0; i < n; i++) s[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
	s[n] = 0;
	printf(" [%s]", s);
}

static void Run(char *line, SceUID *fd) {
	char op[16] = "", arg[256] = "";
	long long a = 0, b = 0, c = 0;
	char *rest;
	if (sscanf(line, "%15s", op) != 1) return;
	rest = line + strlen(op);
	while (*rest == ' ') rest++;
	printf("%s =>", line);
	if (!strcmp(op, "open")) {
		sscanf(rest, "%255s", arg);
		if (*fd >= 0) sceIoClose(*fd);
		*fd = sceIoOpen(arg, PSP_O_RDONLY, 0);
		if (*fd >= 0) printf(" ok");
		else printf(" error %08x", (unsigned)*fd);
	} else if (!strcmp(op, "seek")) {
		char *end;
		a = strtoll(rest, &end, 0);
		b = strtoll(end, NULL, 0);
		printf(" %s", Hex64(sceIoLseek(*fd, b, (int)a)));
	} else if (!strcmp(op, "seek32")) {
		char *end;
		a = strtoll(rest, &end, 0);
		b = strtoll(end, NULL, 0);
		printf(" %d", sceIoLseek32(*fd, (int)b, (int)a));
	} else if (!strcmp(op, "read")) {
		int n;
		memset(buf, 0xEE, 64);
		n = sceIoRead(*fd, buf, 64);
		printf(" %d", n);
		Show(buf, n);
	} else if (!strcmp(op, "close")) {
		printf(" %d", sceIoClose(*fd));
		*fd = -1;
	} else if (!strcmp(op, "stat")) {
		SceIoStat st;
		int r;
		memset(&st, 0, sizeof(st));
		sscanf(rest, "%255s", arg);
		r = sceIoGetstat(arg, &st);
		printf(" %d %s", r, Hex64(st.st_size));
	} else if (!strcmp(op, "umd")) {
		SceUID u;
		char *end;
		a = strtoll(rest, &end, 0);
		b = strtoll(end, NULL, 0);
		u = sceIoOpen("umd0:", PSP_O_RDONLY, 0);
		if (u < 0) {
			printf(" error %08x", (unsigned)u);
		} else {
			long long pos = sceIoLseek(u, a, PSP_SEEK_SET);
			int n;
			memset(buf, 0xEE, sizeof(buf));
			n = sceIoRead(u, buf, 1);
			printf(" %s %d", Hex64(pos), n);
			Show(buf + (b & 2047), 64);
			sceIoClose(u);
		}
	} else if (!strcmp(op, "lbn")) {
		char path[48];
		SceUID l;
		char *end;
		a = strtoll(rest, &end, 0);
		b = strtoll(end, &end, 0);
		c = strtoll(end, NULL, 0);
		snprintf(path, sizeof(path), "disc0:/sce_lbn0x%x_size0x%x", (unsigned)a, (unsigned)b);
		l = sceIoOpen(path, PSP_O_RDONLY, 0);
		if (l < 0) {
			printf(" error %08x", (unsigned)l);
		} else {
			int n;
			sceIoLseek(l, c, PSP_SEEK_SET);
			memset(buf, 0xEE, 64);
			n = sceIoRead(l, buf, 64);
			printf(" %d", n);
			Show(buf, n);
			sceIoClose(l);
		}
	} else if (!strcmp(op, "lastlba")) {
		unsigned in[1] __attribute__((aligned(16))) = {0};
		unsigned out[1] __attribute__((aligned(16))) = {0};
		int r = sceIoDevctl("umd0:", 0x01F20003, in, 4, out, 4);
		printf(" %d 0x%x", r, out[0]);
	} else {
		printf(" unknown");
	}
	printf("\n");
}

int main(void) {
	SceUID fd = -1, s;
	int n;
	char *line, *next;
	sceUmdActivate(1, "disc0:");
	sceUmdWaitDriveStat(PSP_UMD_READY);
	s = sceIoOpen("disc0:/PROBES.TXT", PSP_O_RDONLY, 0);
	if (s < 0) {
		printf("no disc0:/PROBES.TXT (%08x)\n", (unsigned)s);
		sceKernelExitGame();
		return 0;
	}
	n = sceIoRead(s, script, sizeof(script) - 1);
	sceIoClose(s);
	script[n > 0 ? n : 0] = 0;
	for (line = script; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		if (*line && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;
		if (*line && *line != '#') Run(line, &fd);
	}
	if (fd >= 0) sceIoClose(fd);
	printf("done\n");
	sceKernelExitGame();
	return 0;
}
