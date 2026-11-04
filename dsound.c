// dsound.dll for Rainbow Six Vegas 2 (R6Vegas2_Game.exe, MD5 04e295c4c51bfd3a1e9ad42a86b6462a, and
// RainbowSixVegas2_SADS.exe, MD5 293587401f9f580c984a9d8f886738ca).
//
// Audio is forwarded untouched to the system DirectSound. Inline hooks keep the voice chat engine from starting, send
// end-of-burst cues reliably and end weapon loops that outlast any burst. The game modifications raise the player limits
// and the terrorist count. If the exe does not match, nothing is changed and the DLL is a plain forwarder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>

#define EXE_BASE 0x10900000u

// The DLL links no C runtime; CopyMemory expands to this.
void *memcpy(void *dst, const void *src, size_t n) {
	char *d = dst;
	const char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}

// ---------------------------------------------------------------------------------------------------------------
// Problems: a fix that could not be applied leaves a line in r6soundfix.log beside the DLL. Without a problem there is no
// such file.
// ---------------------------------------------------------------------------------------------------------------
static WCHAR g_logPath[MAX_PATH];

static void logProblem(const char *fmt, ...) {
	char line[1100];
	va_list ap;
	va_start(ap, fmt);
	int n = wvsprintfA(line, fmt, ap);
	va_end(ap);
	line[n++] = '\r';
	line[n++] = '\n';
	HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD w;
	WriteFile(f, line, n, &w, NULL);
	CloseHandle(f);
}

static void logOpen(HINSTANCE self) {
	DWORD n = GetModuleFileNameW(self, g_logPath, MAX_PATH);
	while (n && g_logPath[n - 1] != L'\\') n--;
	lstrcpyW(g_logPath + n, L"r6soundfix.log");
}

// ---------------------------------------------------------------------------------------------------------------
// Game object access. Addresses and offsets: .docs/r6v2lan-analyses.md, section "Sound loop".
// ---------------------------------------------------------------------------------------------------------------
#define FIELD(T, obj, off) (*(T *)((char *)(obj) + (off)))

static const char *fnameStr(DWORD name) {
	char *table = *(char **)0x1241d4a8u;
	if (!table) return "?";
	const char *s = *(const char **)(table + (name & 0x7ffffu) * 12);
	return s ? s : "?";
}

static const char *objName(void *o) { return o ? fnameStr(FIELD(DWORD, o, 0x24)) : "null"; }
static const char *className(void *o) { return o ? objName(FIELD(void *, o, 0x28)) : "null"; }

static int isWeaponCue(const char *n) {
	for (const char *p = n; *p; p++) {
		if (p[0] == 'W' && p[1] == '_' && (p == n || p[-1] == '_')) return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------
typedef void *(__thiscall *EventRHS_t)(void *, void *, DWORD, void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef void *(__thiscall *ClientHS_t)(void *, void *, DWORD, void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef void(__thiscall *Destroyed_t)(void *);

static EventRHS_t o_eventRHS;
static ClientHS_t o_clientHS;
static Destroyed_t o_destroyed;

// The fix for the weapon sound loop. Automatic fire is a looping cue that only an "End" cue stops, and the server sends
// cues through an unreliable RPC, so a lost packet leaves the loop running on that client for good. End cues therefore
// go out reliably: FUNC_NetReliable is set on ReplicatedHearSound for just that call. Only end cues, because a reliable
// buffer that overflows ends the connection. See .docs/r6v2lan-analyses.md, section "Sound loop".
#define RHS_FLAGS 0x01020942u
#define FUNC_NET_RELIABLE 0x80u
static DWORD g_findFunctionFn = 0x10fb0be0u; // the game's; DllMain switches to the server's

static int isEndCue(const char *n) {
	int len = lstrlenA(n);
	return (len > 4 && lstrcmpA(n + len - 4, "_End") == 0) || (len > 5 && lstrcmpA(n + len - 5, "_Ends") == 0);
}

// The flags are found by their value in the real function object, so a wrong guess about the layout changes nothing.
static DWORD *rhsFlags(void *pc) {
	static DWORD *flags;
	static int searched;
	if (searched) return flags;
	searched = 1;
	char *fn = ((char *(__thiscall *)(void *, DWORD, DWORD))g_findFunctionFn)(pc, *(DWORD *)0x124a8db0u, 0);
	if (fn) {
		for (int off = 0x20; off < 0x140; off += 4) {
			if (*(DWORD *)(fn + off) == RHS_FLAGS) {
				flags = (DWORD *)(fn + off);
				return flags;
			}
		}
	}
	logProblem("FIX NOT ACTIVE: ReplicatedHearSound flags not found, function %p", fn);
	return NULL;
}

// Server: the RPC call, also reached for local listeners.
static void *__thiscall d_eventRHS(void *pc, void *cue, DWORD slot, void *src, DWORD x, DWORD y, DWORD z, DWORD a7,
	DWORD a8, DWORD a9, DWORD a10) {
	const char *name = objName(cue);
	if (!isWeaponCue(name) || !isEndCue(name)) return o_eventRHS(pc, cue, slot, src, x, y, z, a7, a8, a9, a10);
	DWORD *flags = rhsFlags(pc);
	if (flags) *flags |= FUNC_NET_RELIABLE;
	void *r = o_eventRHS(pc, cue, slot, src, x, y, z, a7, a8, a9, a10);
	if (flags) *flags &= ~FUNC_NET_RELIABLE;
	return r;
}

// Second line of defence on the client, for hosts without this DLL: a weapon loop that outlasts the longest burst any
// weapon can fire gets its end cue played here. Limits from PCKellerWeaponsConfig.ini (largest magazine / m_iFireRateBurst):
// 5 s at most for rifles and SMGs, 15 s for the Mk46. See .docs/r6v2lan-analyses.md, section "Loop watchdog".
#define WD_LIMIT_MS 6000u
#define WD_LIMIT_LMG_MS 18000u
#define WD_MAX 32

typedef struct {
	void *src, *pc, *endCue;
	const char *name;
	DWORD slot, x, y, z, stop, useOwner, tri, zone;
	DWORD start, limit;
} OpenLoop;

static OpenLoop g_open[WD_MAX];

static int nameEndsWith(const char *n, const char *tail) {
	int a = lstrlenA(n), b = lstrlenA(tail);
	return a >= b && lstrcmpA(n + a - b, tail) == 0;
}

static int isLoopCue(const char *n) { return nameEndsWith(n, "_Loop") || nameEndsWith(n, "_Loops"); }

static int isLmgCue(const char *n) {
	static const char *const lmg[] = {"W_M249", "W_MK46", "W_21E", "W_MG36", "W_M60E4"};
	for (unsigned i = 0; i < sizeof(lmg) / sizeof(lmg[0]); i++) {
		int len = lstrlenA(lmg[i]);
		for (const char *p = n; *p; p++) {
			if (CompareStringA(LOCALE_INVARIANT, 0, p, len, lmg[i], len) == CSTR_EQUAL) return 1;
		}
	}
	return 0;
}

// The end cue belonging to a loop cue, from the cue table of the sound source (see the analyses).
static void *endCueOf(void *src, void *loop) {
	static const WORD pairs[][2] = {{0x284, 0x288}, {0x29c, 0x2a0}, {0x2d4, 0x2d8}};
	if (lstrcmpA(className(src), "R6SoundReplicationInfo") != 0) return NULL;
	char *table = FIELD(char *, src, 0x274);
	if (!table) return NULL;
	for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		if (*(void **)(table + pairs[i][0]) == loop) return *(void **)(table + pairs[i][1]);
	}
	return NULL;
}

static void watchLoop(void *pc, void *cue, const char *name, DWORD slot, void *src, DWORD x, DWORD y, DWORD z, DWORD stop,
	DWORD useOwner, DWORD tri, DWORD zone) {
	void *end = endCueOf(src, cue);
	if (!end) return;
	OpenLoop *e = NULL;
	for (int i = 0; i < WD_MAX; i++) {
		if (g_open[i].src == src && g_open[i].endCue == end) {
			e = &g_open[i];
			break;
		}
		if (!e && !g_open[i].src) e = &g_open[i];
	}
	if (!e) return;
	OpenLoop n = {src, pc, end, name, slot, x, y, z, stop, useOwner, tri, zone, GetTickCount(),
		isLmgCue(name) ? WD_LIMIT_LMG_MS : WD_LIMIT_MS};
	*e = n;
}

static void loopEnded(void *src, void *cue) {
	for (int i = 0; i < WD_MAX; i++) {
		if (g_open[i].src == src && g_open[i].endCue == cue) g_open[i].src = NULL;
	}
}

// Client: a loop cue arriving opens a watch on it, its end cue closes the watch.
static void *__thiscall d_clientHS(void *pc, void *cue, DWORD slot, void *src, DWORD x, DWORD y, DWORD z, DWORD stop,
	DWORD useOwner, DWORD tri, DWORD zone) {
	const char *name = objName(cue);
	void *r = o_clientHS(pc, cue, slot, src, x, y, z, stop, useOwner, tri, zone);
	if (src && isWeaponCue(name)) {
		if (isLoopCue(name)) watchLoop(pc, cue, name, slot, src, x, y, z, stop, useOwner, tri, zone);
		else if (isEndCue(name)) loopEnded(src, cue);
	}
	return r;
}

// Runs on the game thread every frame for every sound source, which is the safe place to play the end cue.
typedef void(__thiscall *TickSpecial_t)(void *, DWORD);
static TickSpecial_t o_tick;

static void __thiscall d_tick(void *self, DWORD dt) {
	static DWORD lastCheck;
	o_tick(self, dt);
	DWORD now = GetTickCount();
	if (now - lastCheck < 200) return;
	lastCheck = now;
	for (int i = 0; i < WD_MAX; i++) {
		OpenLoop *e = &g_open[i];
		if (!e->src || now - e->start < e->limit) continue;
		OpenLoop c = *e;
		e->src = NULL;
		o_clientHS(c.pc, c.endCue, c.slot, c.src, c.x, c.y, c.z, c.stop, c.useOwner, c.tri, c.zone);
	}
}

// Client: the sound source actor going away while a loop may still be playing.
static void __thiscall d_destroyed(void *self) {
	// The source is going away with a loop open, and weapon cues are played with bStopWhenOwnerDestroyed=0, so nothing
	// would ever end it. It is still valid here, so the end cue is played now.
	for (int i = 0; i < WD_MAX; i++) {
		if (g_open[i].src != self) continue;
		OpenLoop c = g_open[i];
		g_open[i].src = NULL;
		if (*(void **)0x124b1524u) {
			o_clientHS(c.pc, c.endCue, c.slot, c.src, c.x, c.y, c.z, c.stop, c.useOwner, c.tri, c.zone);
		}
	}
	o_destroyed(self);
}

// R6VoiceChatManagerInterface::PostAudioStartVoiceChatEngine, the real start of the voice chat engine. Not starting
// it is safe: every other voice function checks the engine pointer for NULL, the callers ignore this return value,
// and 0 is what the original returns when it does not start one. See .docs/r6v2lan-analyses.md, section "Voice chat".
static void *o_voiceStart;

static int __thiscall d_voiceStart(void *self) {
	(void)self;
	return 0;
}

typedef struct {
	const char *name;
	DWORD addr;
	BYTE orig[16];
	int stolen; // whole instructions, at least 5 bytes, nothing position dependent
	void *detour;
	void **tramp;
	DWORD addrServer; // address in RainbowSixVegas2_SADS.exe, 0 = not hooked there
} Hook;

// The voice engine is only blocked in the game: a dedicated server has no microphone and nobody listening.
static Hook g_hooks[] = {
	{"RepInfoTickSpecial", 0x10cc18b0u, {0xd9, 0x44, 0x24, 0x04, 0x56}, 5, d_tick, (void **)&o_tick, 0},
	{"PostAudioStartVoiceChatEngine", 0x10ab0120u, {0x6a, 0xff, 0x68, 0xab, 0x05, 0xb4, 0x11}, 7, d_voiceStart, &o_voiceStart, 0},
	{"eventReplicatedHearSound", 0x1134f510u, {0x83, 0xec, 0x2c, 0x8b, 0x44, 0x24, 0x30}, 7, d_eventRHS, (void **)&o_eventRHS, 0x1134f290u},
	{"ClientHearSound", 0x11352630u, {0x83, 0xec, 0x0c, 0x53, 0x8b, 0x5c, 0x24, 0x14}, 8, d_clientHS, (void **)&o_clientHS, 0},
	{"RepInfoDestroyed", 0x10cc21f0u, {0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x78, 0x02, 0x00, 0x00}, 9, d_destroyed, (void **)&o_destroyed, 0},
};
#define HOOK_COUNT (sizeof(g_hooks) / sizeof(g_hooks[0]))

enum { EXE_OTHER, EXE_GAME, EXE_SERVER };

// The voice page of the settings menu skips its update when there is no engine, which leaves the layout's placeholder
// "TALKING" on screen. The jump at 0x114ce707 goes to its "not talking" branch instead (0x114ce740, was 0x114ce77f).
#define TALKING_JUMP_ADDR 0x114ce708u
#define TALKING_JUMP_OLD 0x76
#define TALKING_JUMP_NEW 0x37

// All original bytes are compared before the first write, so a different exe stays completely unpatched.
static int hooksMatch(int exe) {
	for (unsigned i = 0; i < HOOK_COUNT; i++) {
		Hook *h = &g_hooks[i];
		if (h->addr == 0) continue;
		if (IsBadReadPtr((void *)h->addr, h->stolen)) return 0;
		for (int b = 0; b < h->stolen; b++) {
			if (((BYTE *)h->addr)[b] != h->orig[b]) {
				logProblem("MISMATCH %s at %p byte %d: %02x", h->name, (void *)h->addr, b, ((BYTE *)h->addr)[b]);
				return 0;
			}
		}
	}
	if (exe == EXE_SERVER) return 1;
	if (IsBadReadPtr((void *)TALKING_JUMP_ADDR, 1) || *(BYTE *)TALKING_JUMP_ADDR != TALKING_JUMP_OLD) {
		logProblem("MISMATCH voice menu jump at %p", (void *)TALKING_JUMP_ADDR);
		return 0;
	}
	return 1;
}

static int patchTalkingJump(void) {
	DWORD old;
	if (!VirtualProtect((void *)TALKING_JUMP_ADDR, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
	*(BYTE *)TALKING_JUMP_ADDR = TALKING_JUMP_NEW;
	VirtualProtect((void *)TALKING_JUMP_ADDR, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void *)TALKING_JUMP_ADDR, 1);
	return 1;
}

static int hookInstall(Hook *h) {
	BYTE *t = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!t) return 0;
	CopyMemory(t, (void *)h->addr, h->stolen);
	t[h->stolen] = 0xE9;
	*(DWORD *)(t + h->stolen + 1) = h->addr + h->stolen - ((DWORD)t + h->stolen + 5);
	*h->tramp = t;
	DWORD old;
	if (!VirtualProtect((void *)h->addr, h->stolen, PAGE_EXECUTE_READWRITE, &old)) return 0;
	BYTE *p = (BYTE *)h->addr;
	p[0] = 0xE9;
	*(DWORD *)(p + 1) = (DWORD)h->detour - (h->addr + 5);
	for (int i = 5; i < h->stolen; i++) p[i] = 0x90;
	VirtualProtect((void *)h->addr, h->stolen, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void *)h->addr, h->stolen);
	return 1;
}

static int endsWith(const WCHAR *path, DWORD n, const WCHAR *want) {
	DWORD wl = lstrlenW(want);
	return n >= wl && lstrcmpiW(path + n - wl, want) == 0;
}

static int detectExe(void) {
	WCHAR path[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if ((DWORD)GetModuleHandleW(NULL) != EXE_BASE) return EXE_OTHER;
	if (endsWith(path, n, L"r6vegas2_game.exe")) return EXE_GAME;
	if (endsWith(path, n, L"rainbowsixvegas2_sads.exe")) return EXE_SERVER;
	return EXE_OTHER;
}

// ---------------------------------------------------------------------------------------------------------------
// DirectSound forwarding, exported with the original ordinals (see dsound.def).
// ---------------------------------------------------------------------------------------------------------------
static HMODULE g_real;

static FARPROC realProc(const char *name) {
	if (!g_real) {
		WCHAR path[MAX_PATH];
		UINT n = GetSystemDirectoryW(path, MAX_PATH);
		lstrcpyW(path + n, L"\\dsound.dll");
		g_real = LoadLibraryW(path);
		if (!g_real) return NULL;
	}
	return GetProcAddress(g_real, name);
}

#define FORWARD_3(fn)                                                                  \
	__stdcall HRESULT Fwd_##fn(void *a, void *b, void *c) {                            \
		HRESULT(__stdcall * f)(void *, void *, void *) = (void *)realProc(#fn);        \
		return f ? f(a, b, c) : E_FAIL;                                                \
	}
#define FORWARD_2(fn)                                                  \
	__stdcall HRESULT Fwd_##fn(void *a, void *b) {                     \
		HRESULT(__stdcall * f)(void *, void *) = (void *)realProc(#fn); \
		return f ? f(a, b) : E_FAIL;                                   \
	}

FORWARD_3(DirectSoundCreate)
FORWARD_2(DirectSoundEnumerateA)
FORWARD_2(DirectSoundEnumerateW)
FORWARD_3(DllGetClassObject)
FORWARD_3(DirectSoundCaptureCreate)
FORWARD_2(DirectSoundCaptureEnumerateA)
FORWARD_2(DirectSoundCaptureEnumerateW)
FORWARD_2(GetDeviceID)
FORWARD_3(DirectSoundCaptureCreate8)
FORWARD_3(DirectSoundCreate8)

__stdcall HRESULT Fwd_DllCanUnloadNow(void) {
	HRESULT(__stdcall * f)(void) = (void *)realProc("DllCanUnloadNow");
	return f ? f() : S_FALSE;
}

__stdcall HRESULT Fwd_DirectSoundFullDuplexCreate(void *a, void *b, void *c, void *d, void *e, DWORD f1, void *g,
	void *h, void *i, void *j) {
	HRESULT(__stdcall * f)(void *, void *, void *, void *, void *, DWORD, void *, void *, void *, void *) =
		(void *)realProc("DirectSoundFullDuplexCreate");
	return f ? f(a, b, c, d, e, f1, g, h, i, j) : E_FAIL;
}

// ---------------------------------------------------------------------------------------------------------------
// LAN list: a dedicated server running terrorist hunt answers the game's LAN search with mode 1 (attack and defend), so
// it is missing from the terrorist hunt list although it plays that mode. Each such reply goes out with mode 8. See
// .docs/r6v2lan-analyses.md, section "Terrorist hunt on the dedicated server".
// ---------------------------------------------------------------------------------------------------------------
#define LAN_REPLY_LEN 285
#define LAN_MODE_OFFSET 0x105 // the fields sit one bit off in the packet
#define MODE_ATTACKDEFEND 1
#define MODE_TERRORHUNT 8

typedef int(__stdcall *sendto_t)(UINT_PTR, const char *, int, int, const void *, int);
static sendto_t o_sendto;

static int __stdcall d_sendto(UINT_PTR s, const char *buf, int len, int flags, const void *to, int tolen) {
	const BYTE *b = (const BYTE *)buf;
	if (len != LAN_REPLY_LEN || b[0] != 0x36 || (b[LAN_MODE_OFFSET] >> 1) != MODE_ATTACKDEFEND) {
		return o_sendto(s, buf, len, flags, to, tolen);
	}
	BYTE copy[LAN_REPLY_LEN];
	CopyMemory(copy, buf, LAN_REPLY_LEN);
	copy[LAN_MODE_OFFSET] = (BYTE)((copy[LAN_MODE_OFFSET] & 1) | (MODE_TERRORHUNT << 1));
	return o_sendto(s, (const char *)copy, len, flags, to, tolen);
}

static int commandLineHas(const WCHAR *what) {
	const WCHAR *cmd = GetCommandLineW();
	int n = lstrlenW(what);
	for (; *cmd; cmd++) {
		if (CompareStringW(LOCALE_INVARIANT, 0, cmd, n, what, n) == CSTR_EQUAL) return 1;
	}
	return 0;
}

static void installLanFix(void) {
	if (!commandLineHas(L"R6CoopTerroristHuntGame")) return;
	HMODULE ws = GetModuleHandleW(L"ws2_32.dll");
	if (!ws) ws = LoadLibraryW(L"ws2_32.dll");
	BYTE *fn = ws ? (BYTE *)GetProcAddress(ws, "sendto") : NULL;
	static const BYTE start[5] = {0x8b, 0xff, 0x55, 0x8b, 0xec};
	if (!fn) {
		logProblem("LANFIX NOT ACTIVE: no sendto");
		return;
	}
	// ReShade (d3d9.dll) may have redirected sendto already. Then we take its place and pass calls on to it.
	if (fn[0] == 0xE9) {
		DWORD old;
		o_sendto = (sendto_t)(fn + 5 + *(int *)(fn + 1));
		if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
			logProblem("LANFIX NOT ACTIVE: protect failed");
			return;
		}
		*(DWORD *)(fn + 1) = (DWORD)d_sendto - ((DWORD)fn + 5);
		VirtualProtect(fn, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), fn, 5);
		return;
	}
	for (int i = 0; i < 5; i++) {
		if (fn[i] != start[i]) {
			logProblem("LANFIX NOT ACTIVE: sendto at %p starts %02x %02x %02x %02x %02x", fn, fn[0], fn[1], fn[2], fn[3], fn[4]);
			return;
		}
	}
	Hook h = {"sendto", (DWORD)fn, {0}, 5, d_sendto, (void **)&o_sendto, 0};
	if (!hookInstall(&h)) logProblem("LANFIX NOT ACTIVE: hook failed");
}

// ---------------------------------------------------------------------------------------------------------------
// Game modifications: more players and more terrorists. Same bytes in both exes, the server's code sits 0x280 lower.
// Addresses are the game's. If one entry does not hold its original bytes, nothing is changed. The two terrorist values
// come from r6v2lan-mods.ini beside the DLL, which the launcher writes; without it the defaults apply.
// ---------------------------------------------------------------------------------------------------------------
#define TERRO_TOTAL_DEFAULT 99
#define TERRO_TOTAL_MAX 999
#define TERRO_AT_ONCE_DEFAULT 8
#define TERRO_AT_ONCE_MAX 32
#define SERVER_CODE_SHIFT 0x280u

static int g_terroTotal = TERRO_TOTAL_DEFAULT;
static int g_terroAtOnce = TERRO_AT_ONCE_DEFAULT;

typedef struct {
	DWORD addr;
	BYTE len;
	const char *orig;
	const char *mod;
	const int *value; // when set, goes into mod after its first byte: mov reg, value
} GameMod;

static const GameMod g_mods[] = {
	// Terrorists in a match: the function at 0x10a2b590 stores a set count instead of its argument.
	{0x10a2b60c, 4, "\x8b\x44\x24\x18", "\xeb\x45\x90\x90", NULL},
	{0x10a2b653, 7, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc", "\xb8\x00\x00\x00\x00\xeb\xb6", &g_terroTotal},
	// Terrorists alive at one time: m_iMaxTerrosExist (0x3c) takes a set value instead of the one from the object at
	// 0x12386ad0. eax and ecx are set again before their next use.
	{0x10a25a26, 8, "\xa1\xd0\x6a\x38\x12\x8b\x48\x04", "\xb9\x00\x00\x00\x00\x90\x90\x90", &g_terroAtOnce},
	// Value clamp at 0x10d4da6f: 16 is always passed on.
	{0x10d4da72, 2, "\x7e\x05", "\x90\x90", NULL},
	// Number spinner: one with a minimum of 2 (0x18) always holds 16 (0x14).
	{0x10d63911, 15, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc",
		"\x83\x7e\x18\x02\x75\x47\xc7\x46\x14\x10\x00\x00\x00\xeb\x18", NULL},
	{0x10d63936, 7, "\x83\x7e\x04\x00\x89\x46\x14", "\xeb\xd9\x83\x7e\x04\x00\x90", NULL},
	{0x10d6395e, 2, "\xcc\xcc", "\xeb\x4d", NULL},
	{0x10d639ad, 2, "\xcc\xcc", "\xeb\x42", NULL},
	{0x10d639f1, 8, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc", "\x89\x46\x14\xe9\x3f\xff\xff\xff", NULL},
	// The same spinner sets 16 before writing its value to the session settings (0xc8).
	{0x10d67c41, 12, "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc",
		"\xc7\x46\x14\x10\x00\x00\x00\x8b\x7e\x24\xeb\x1d", NULL},
	{0x10d67c67, 3, "\x8b\x7e\x24", "\xeb\xd8\x90", NULL},
	// TeamInfo.Size (0x270) and GameInfo.NumPlayers (0x290) stay 0, so neither a team nor the server is ever full.
	{0x111624a0, 6, "\x01\x99\x70\x02\x00\x00", "\x90\x90\x90\x90\x90\x90", NULL},
	{0x11162580, 7, "\x83\x81\x70\x02\x00\x00\xff", "\x90\x90\x90\x90\x90\x90\x90", NULL},
	{0x113b7db9, 7, "\x83\x83\x90\x02\x00\x00\xff", "\x90\x90\x90\x90\x90\x90\x90", NULL},
	{0x113ba630, 6, "\x01\xae\x90\x02\x00\x00", "\x90\x90\x90\x90\x90\x90", NULL},
	// Session settings 0xc8, copied from and to the options' 0x100, is 1 in both directions.
	{0x113de3a9, 6, "\x8b\x96\x00\x01\x00\x00", "\xba\x01\x00\x00\x00\x90", NULL},
	{0x113deac1, 6, "\x8b\x91\xc8\x00\x00\x00", "\xba\x01\x00\x00\x00\x90", NULL},
};
#define GAME_MOD_COUNT (sizeof(g_mods) / sizeof(g_mods[0]))

static int bytesEqual(const BYTE *a, const char *b, int n) {
	for (int i = 0; i < n; i++) {
		if (a[i] != (BYTE)b[i]) return 0;
	}
	return 1;
}

static int clampInt(int v, int low, int high) {
	return v < low ? low : v > high ? high : v;
}

static void readModSettings(HINSTANCE self) {
	WCHAR path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	while (n && path[n - 1] != L'\\') n--;
	lstrcpyW(path + n, L"r6v2lan-mods.ini");
	g_terroTotal = clampInt((int)GetPrivateProfileIntW(L"Terrorists", L"Total", TERRO_TOTAL_DEFAULT, path), 1, TERRO_TOTAL_MAX);
	g_terroAtOnce = clampInt((int)GetPrivateProfileIntW(L"Terrorists", L"AtOnce", TERRO_AT_ONCE_DEFAULT, path), 1, TERRO_AT_ONCE_MAX);
}

static void applyGameMods(DWORD shift) {
	for (unsigned i = 0; i < GAME_MOD_COUNT; i++) {
		const BYTE *p = (const BYTE *)(g_mods[i].addr - shift);
		if (!bytesEqual(p, g_mods[i].orig, g_mods[i].len)) {
			logProblem("MODS NOT ACTIVE: unexpected bytes at %08lx", (DWORD)p);
			return;
		}
	}
	for (unsigned i = 0; i < GAME_MOD_COUNT; i++) {
		BYTE *p = (BYTE *)(g_mods[i].addr - shift);
		DWORD old;
		if (!VirtualProtect(p, g_mods[i].len, PAGE_EXECUTE_READWRITE, &old)) {
			logProblem("MODS PARTLY ACTIVE: protect failed at %08lx after %u", (DWORD)p, i);
			return;
		}
		CopyMemory(p, g_mods[i].mod, g_mods[i].len);
		if (g_mods[i].value) *(int *)(p + 1) = *g_mods[i].value;
		VirtualProtect(p, g_mods[i].len, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, g_mods[i].len);
	}
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(self);
	logOpen(self);
	int exe = detectExe();
	if (exe == EXE_SERVER) {
		g_findFunctionFn = 0x10fb0960u;
		for (unsigned i = 0; i < HOOK_COUNT; i++) g_hooks[i].addr = g_hooks[i].addrServer;
	}
	if (exe == EXE_OTHER || !hooksMatch(exe)) {
		return TRUE;
	}
	int ok = 1;
	for (unsigned i = 0; i < HOOK_COUNT; i++) {
		if (g_hooks[i].addr) ok &= hookInstall(&g_hooks[i]);
	}
	readModSettings(self);
	applyGameMods(exe == EXE_SERVER ? SERVER_CODE_SHIFT : 0);
	if (exe == EXE_SERVER) installLanFix();
	if (exe == EXE_GAME) ok &= patchTalkingJump();
	if (!ok) logProblem("HOOKS incomplete");
	return TRUE;
}
