// t6.exe with no arguments: everything a double-click needs.
//   - finds the game dump (<exe>\game, else <exe>\..\game, or [Paths] Game= in t6.ini)
//   - extracts GAME_208.elf from the debug SELF into <exe>\cache once
//   - creates the 16-byte USB key id file the game checks for
//   - sets the PS3_* paths and the arcade boot arguments
//   - turns t6.ini's [Video] section into the renderer's RSX_* settings
//   - sends the log to <exe>\t6.log when there is no console
// Variables already set in the environment win, so dev scripts can still override anything.
#include "t6_settings.h"
#include <direct.h>
#include <io.h>
#include <string>
#include <vector>

extern "C" void rsx_present_set_screen_aspect(float aspect);
extern "C" void rsx_present_set_stretch(int on);
extern "C" unsigned char* vm_base;

static std::string s_elf;
static float s_screen_aspect;

static std::string dirOf(const std::string& p) { size_t s = p.find_last_of("\\/"); return s == std::string::npos ? "." : p.substr(0, s); }
static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string fwd(std::string p) { for (char& c : p) if (c == '\\') c = '/'; return p; }
static void setDefault(const char* k, const std::string& v) { if (!getenv(k)) _putenv_s(k, v.c_str()); }

static bool readFile(const std::string& p, std::vector<unsigned char>& out)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    out.resize((size_t)n);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

static unsigned long long be64(const unsigned char* p) { unsigned long long v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }

// A debug SELF (key revision 0x8000) carries the original ELF verbatim after its header.
static bool extractElf(const std::string& self, const std::string& elf)
{
    std::vector<unsigned char> d;
    if (!readFile(self, d) || d.size() < 0x20 || memcmp(d.data(), "SCE\0", 4)) return false;
    const unsigned long long hl = be64(&d[0x10]), dl = be64(&d[0x18]);
    if (hl + dl > d.size() || memcmp(&d[hl], "\x7f" "ELF", 4)) return false;
    _mkdir(dirOf(elf).c_str());
    std::string tmp = elf + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(&d[hl], 1, (size_t)dl, f) == dl;
    fclose(f);
    return ok && MoveFileExA(tmp.c_str(), elf.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void fail(const std::string& msg)
{
    fprintf(stderr, "[t6] %s\n", msg.c_str());
    MessageBoxA(nullptr, msg.c_str(), "Tekken 6", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

// Returns the ELF to load. argv[1], when given, is used as is (dev runs).
extern "C" const char* t6_launch_prepare(int argc, char** argv)
{
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string dir = dirOf(exe), ini = dir + "\\t6.ini";

    // No console (double-click of the GUI-subsystem exe): log to a file.
    if (!GetStdHandle(STD_OUTPUT_HANDLE) || GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN) {
        const std::string log = dir + "\\t6.log";
        freopen(log.c_str(), "w", stdout);
        _dup2(_fileno(stdout), 2);
        freopen(log.c_str(), "a", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    if (!exists(ini)) t6::saveSettings(ini.c_str(), t6::defaultSettings());
    const t6::Settings st = t6::loadSettings(ini.c_str());

    // ---- video -> renderer
    int w, h;
    t6::resolutionSize(st.video.resolution, st.video.aspect, w, h);
    char buf[64];
    snprintf(buf, sizeof buf, "%dx%d", w, h); setDefault("RSX_WINDOW", buf);
    // Internal resolution: 1080p/1440p render the game's 1280x720 targets 1.5x/2x.
    // [Video] InternalScale=0 renders 1280x720 and lets the present scale it to the window.
    const bool internal = GetPrivateProfileIntA("Video", "InternalScale", 1, ini.c_str()) != 0;
    const float scale = internal && st.video.resolution != t6::Resolution::Original ? h / 720.0f : 1.0f;
    snprintf(buf, sizeof buf, "%.4f", scale); setDefault("RSX_SCALE", buf);
    setDefault("RSX_DISPLAY", st.video.fullscreen ? "borderless" : "windowed");
    setDefault("RSX_VSYNC", st.video.vsync ? "1" : "0");
    setDefault("RSX_WAIT_JOBS", st.video.skinSync ? "1" : "0");
    // 4:3: the game renders a 4:3 projection into its 16:9 targets (t6_launch_patch_guest)
    // and the frame is stretched over a 4:3 screen; the 2D layer comes out 25% narrower.
    s_screen_aspect = st.video.aspect == t6::Aspect::Narrow ? 4.0f / 3.0f : 0.0f;

    if (argc >= 2) return argv[1];

    // ---- game files
    char g[MAX_PATH] = {};
    GetPrivateProfileStringA("Paths", "Game", "", g, sizeof g, ini.c_str());
    std::string game = g[0] ? std::string(g) : dir + "\\game";
    if (!g[0] && !exists(game + "\\dev_hdd0")) game = dir + "\\..\\game";
    char full[MAX_PATH]; GetFullPathNameA(game.c_str(), MAX_PATH, full, nullptr); game = full;
    const std::string self = game + "\\dev_hdd0\\game\\SCEEXE000\\USRDIR\\GAME_208.SELF";
    if (!exists(self))
        fail("Game files not found.\n\nPut the extracted Tekken 6 Bloodline Rebellion (arcade) dump in a folder named \"game\" next to t6.exe "
             "(it must contain dev_hdd0\\game\\SCEEXE000\\USRDIR\\GAME_208.SELF), or set [Paths] Game= in t6.ini.\n\nLooked in: " + game);

    s_elf = dir + "\\cache\\GAME_208.elf";
    if (!exists(s_elf) && !extractElf(self, s_elf)) fail("Could not extract GAME_208.elf from " + self);

    // The USB key id file (/dev_usb000/T6.ID maps to the game root): 16 bytes, any content.
    const std::string id = game + "\\T6.ID";
    if (!exists(id)) { if (FILE* f = fopen(id.c_str(), "wb")) { static const char z[16] = "026450000000"; fwrite(z, 1, 16, f); fclose(f); } }

    setDefault("PS3_TITLE", "TEKKEN 6 BLOODLINE REBELLION");
    setDefault("PS3_VFS_ROOT", fwd(game));
    setDefault("PS3_HDD0_ROOT", fwd(game + "\\dev_hdd0"));
    if (exists(dir + "\\dev_flash")) setDefault("PS3_DEV_FLASH", fwd(dir + "\\dev_flash"));
    setDefault("PS3_BOOT_PATH", "/dev_hdd0/game/SCEEXE000/USRDIR/GAME_208.SELF");
    setDefault("PS3_ARGV", "ver=206;romver=B23;livemon=0;usbid=026450000000;usbnum=1;idpath=/dev_usb000/T6.ID;datainfo=DINF_004.BIN");
    setDefault("PS3_VERBOSE", "0");
    // BR's sound core (bnusCore) runs as a SPURS task fed through CellSpursQueue: the
    // queue header layout its SPU library uses, the PPU push, and no job-chain
    // completion events on the queues it attached (they reached its decoder thread).
    setDefault("SPURS_QUEUE_LAYOUT", "2");
    setDefault("SPURS_QUEUE_PUSH", "1");
    setDefault("SPURS_JC_DONE_EVENTS", "0");
    return s_elf.c_str();
}

// After the renderer is up (the present layout is read every frame).
extern "C" void t6_launch_apply_video(void)
{
    if (s_screen_aspect > 0.0f) rsx_present_set_screen_aspect(s_screen_aspect);
    if (s_screen_aspect > 0.0f && !getenv("T6_43_LETTERBOX")) rsx_present_set_stretch(1);
}

// 4:3 (T6_43_LETTERBOX=1: the old letterbox instead): the game's own aspect constants -- the
// camera's 16/9 (next to near 0.1 / far 10000) and the 16:9 entry of its screen-mode table
// (1.77, 16:9, 4:3, 3:2) -- set to 4/3, and the frame stretched over a 4:3 screen. GAME_208
// addresses; found by matching the bytes around T6's (0x72B194 / 0x72E090).
extern "C" void t6_launch_patch_guest(void)
{
    if (!(s_screen_aspect > 0.0f) || getenv("T6_43_LETTERBOX")) return;
    auto put_be_float = [](uint32_t ea, float f) {
        uint32_t u; memcpy(&u, &f, 4);
        unsigned char* p = vm_base + ea;
        p[0] = (unsigned char)(u >> 24); p[1] = (unsigned char)(u >> 16); p[2] = (unsigned char)(u >> 8); p[3] = (unsigned char)u;
    };
    put_be_float(0x7016F8, 4.0f / 3.0f);
    put_be_float(0x7042EC, 4.0f / 3.0f);
    fprintf(stderr, "[t6] 4:3: patched the camera and screen-mode aspect to 4/3\n");
}
