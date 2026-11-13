#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr const char* kPluginId = "com.alpcavas.corridorkey2";
constexpr const char* kResolutionLabels[] = {"Full", "Half", "Quarter"};
constexpr int kScales[] = {1, 2, 4};
constexpr const char* kMemoryLabels[] = {"Auto (free GPU memory)", "Manual"};
constexpr const char* kWindowLabels[] = {"1", "2", "4", "8", "24", "48", "80"};
constexpr int kWindows[] = {1, 2, 4, 8, 24, 48, 80};
constexpr const char* kHintLabels[] = {"None (model picks the subject)", "Reference frame", "Every frame"};
constexpr const char* kHintModes[] = {"none", "first", "video"};
constexpr const char* kHintSourceLabels[] = {"Chroma key", "Hint input (matte)"};
constexpr const char* kHintSources[] = {"chroma", "input"};
constexpr const char* kHintChannelLabels[] = {"Luminance", "Alpha"};
constexpr const char* kHintClip = "Hint";
constexpr const char* kViewLabels[] = {"Cutout (premultiplied)", "Cutout (straight)", "Alpha", "Foreground", "Checker",
                                       "Source"};
constexpr const char* kSpillColorLabels[] = {"Green", "Blue"};
constexpr const char* kSpillAlgoLabels[] = {"Average", "Max", "Min"};

OfxHost* gHost = nullptr;
OfxPropertySuiteV1* gProp = nullptr;
OfxImageEffectSuiteV1* gEffect = nullptr;
OfxParameterSuiteV1* gParam = nullptr;
OfxMessageSuiteV1* gMessage = nullptr;
OfxProgressSuiteV1* gProgress = nullptr;
std::mutex gCollect;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

HMODULE self_module() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &m);
    return m;
}

std::wstring parent_of(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(0, i);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool read_text(const std::wstring& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_text(const std::wstring& p, const std::string& s) {
    std::wstring tmp = p + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << s;
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) i = 3;
    return s.substr(i);
}

std::wstring resources_dir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(self_module(), buf, MAX_PATH * 2);
    return parent_of(parent_of(buf)) + L"\\Resources";
}

std::wstring app_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    std::wstring dir = (n ? std::wstring(buf, n) : L"C:\\Temp") + L"\\CorridorKey2OFX";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ck2_root() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"CORRIDORKEY2_RUNTIME", buf, MAX_PATH * 2);
    if (n && n < MAX_PATH * 2) return std::wstring(buf, n);
    std::string text;
    if (read_text(app_dir() + L"\\runtime.txt", text) && !trim(text).empty()) return widen(trim(text));
    return app_dir() + L"\\runtime";
}

std::wstring cache_root() {
    std::wstring dir = app_dir() + L"\\cache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void log_line(const std::string& text) {
    std::ofstream f(cache_root() + L"\\plugin.log", std::ios::app | std::ios::binary);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    f << stamp << text << "\n";
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if ((unsigned char)c < 0x20) continue;
        o += c;
    }
    return o;
}

std::string json_raw(const std::string& text, const std::string& key) {
    size_t k = text.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t c = text.find(':', k);
    if (c == std::string::npos) return "";
    size_t i = c + 1;
    while (i < text.size() && text[i] == ' ') ++i;
    if (i < text.size() && text[i] == '"') {
        std::string o;
        for (size_t j = i + 1; j < text.size(); ++j) {
            if (text[j] == '\\' && j + 1 < text.size()) {
                o += text[++j];
                continue;
            }
            if (text[j] == '"') break;
            o += text[j];
        }
        return o;
    }
    size_t e = text.find_first_of(",}", i);
    return trim(text.substr(i, e == std::string::npos ? std::string::npos : e - i));
}

void post(OfxImageEffectHandle effect, const char* type, const std::string& text) {
    if (gMessage) gMessage->message(effect, type, nullptr, "%s", text.c_str());
}

OfxParamHandle param(OfxImageEffectHandle effect, const char* name) {
    OfxParamSetHandle params;
    gEffect->getParamSet(effect, &params);
    OfxParamHandle h = nullptr;
    gParam->paramGetHandle(params, name, &h, nullptr);
    return h;
}

int get_int(OfxImageEffectHandle effect, const char* name, OfxTime t) {
    int v = 0;
    if (OfxParamHandle h = param(effect, name)) gParam->paramGetValueAtTime(h, t, &v);
    return v;
}

double get_double(OfxImageEffectHandle effect, const char* name, OfxTime t) {
    double v = 0.0;
    if (OfxParamHandle h = param(effect, name)) gParam->paramGetValueAtTime(h, t, &v);
    return v;
}

void get_rgb(OfxImageEffectHandle effect, const char* name, OfxTime t, double rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0.0;
    if (OfxParamHandle h = param(effect, name)) gParam->paramGetValueAtTime(h, t, &rgb[0], &rgb[1], &rgb[2]);
}

std::string get_string(OfxImageEffectHandle effect, const char* name) {
    char* s = nullptr;
    if (OfxParamHandle h = param(effect, name)) gParam->paramGetValue(h, &s);
    return s ? s : "";
}

void set_string(OfxImageEffectHandle effect, const char* name, const std::string& v) {
    if (OfxParamHandle h = param(effect, name)) gParam->paramSetValue(h, v.c_str());
}

void place(OfxPropertySetHandle p, const char* label, const char* hint, const char* parent) {
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    if (hint) gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    if (parent) gProp->propSetString(p, kOfxParamPropParent, 0, parent);
}

void define_group(OfxParamSetHandle params, const char* name, const char* label, bool open) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeGroup, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetInt(p, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
}

void define_double(OfxParamSetHandle params, const char* name, const char* label, double def, double lo, double hi,
                   const char* hint, const char* parent, bool animates) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeDouble, name, &p);
    place(p, label, hint, parent);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, lo);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, hi);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, lo);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, hi);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, animates ? 1 : 0);
}

void define_bool(OfxParamSetHandle params, const char* name, const char* label, int def, const char* hint,
                 const char* parent) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeBoolean, name, &p);
    place(p, label, hint, parent);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

void define_choice(OfxParamSetHandle params, const char* name, const char* label, const char* const* options, int count,
                   int def, const char* hint, const char* parent) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeChoice, name, &p);
    place(p, label, hint, parent);
    for (int i = 0; i < count; ++i) gProp->propSetString(p, kOfxParamPropChoiceOption, i, options[i]);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

void define_button(OfxParamSetHandle params, const char* name, const char* label, const char* hint, const char* parent) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypePushButton, name, &p);
    place(p, label, hint, parent);
}

void define_hidden_string(OfxParamSetHandle params, const char* name) {
    OfxPropertySetHandle p;
    gParam->paramDefine(params, kOfxParamTypeString, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, name);
    gProp->propSetString(p, kOfxParamPropDefault, 0, "");
    gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);
    gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
}

OfxStatus describe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffect->getPropertySet(effect, &props);
    gProp->propSetString(props, kOfxPropLabel, 0, "CorridorKey 2.0");
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Keying");
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    return kOfxStatOK;
}

OfxStatus describe_in_context(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    char* context = nullptr;
    gProp->propGetString(in, kOfxImageEffectPropContext, 0, &context);
    OfxPropertySetHandle clip;
    gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetInt(clip, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    if (context && !std::strcmp(context, kOfxImageEffectContextGeneral)) {
        gEffect->clipDefine(effect, kHintClip, &clip);
        gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);
        gProp->propSetInt(clip, kOfxImageClipPropOptional, 0, 1);
        gProp->propSetInt(clip, kOfxImageEffectPropTemporalClipAccess, 0, 1);
        gProp->propSetInt(clip, kOfxImageClipPropIsMask, 0, 1);
    }
    gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &clip);
    gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    OfxParamSetHandle params;
    gEffect->getParamSet(effect, &params);

    define_group(params, "processGroup", "Process", true);
    define_button(params, "keyClip", "Key Clip",
                  "Keys the whole clip with CorridorKey 2.0 in the background. The frames are read once; the result "
                  "plays from the cache.",
                  "processGroup");
    define_button(params, "refresh", "Refresh", "Updates the status and shows the result once keying is done.",
                  "processGroup");
    define_button(params, "cancel", "Cancel", "Stops the running keying.", "processGroup");
    define_button(params, "clearCache", "Clear Cache",
                  "Moves this clip's keyed frames to the Recycle Bin. The clip shows the source until keyed again.",
                  "processGroup");
    define_button(params, "openCache", "Open Cache Folder", "Opens the folder that holds the keyed frames.",
                  "processGroup");
    OfxPropertySetHandle sp;
    gParam->paramDefine(params, kOfxParamTypeString, "status", &sp);
    place(sp, "Status", "What the keyer is doing.", "processGroup");
    gProp->propSetString(sp, kOfxParamPropDefault, 0, "Not keyed. Press Key Clip.");
    gProp->propSetString(sp, kOfxParamPropStringMode, 0, kOfxParamStringIsLabel);
    gProp->propSetInt(sp, kOfxParamPropAnimates, 0, 0);
    gProp->propSetInt(sp, kOfxParamPropEvaluateOnChange, 0, 0);
    define_hidden_string(params, "jobId");
    OfxPropertySetHandle ep;
    gParam->paramDefine(params, kOfxParamTypeInteger, "epoch", &ep);
    gProp->propSetString(ep, kOfxPropLabel, 0, "epoch");
    gProp->propSetInt(ep, kOfxParamPropDefault, 0, 0);
    gProp->propSetInt(ep, kOfxParamPropSecret, 0, 1);
    gProp->propSetInt(ep, kOfxParamPropAnimates, 0, 0);

    define_group(params, "controlsGroup", "Controls", true);
    define_bool(params, "linearInput", "Linear input", 0,
                "On for scene-linear footage (EXR, linear timeline). Off for normal video (Rec.709, sRGB).",
                "controlsGroup");
    define_choice(params, "resolution", "Resolution", kResolutionLabels, 3, 0,
                  "Size the model works at. Half and Quarter are faster and need less GPU memory.", "controlsGroup");
    define_choice(params, "memory", "GPU memory", kMemoryLabels, 2, 0,
                  "Auto picks the temporal window and low-VRAM mode from free GPU memory.", "controlsGroup");
    define_choice(params, "window", "Temporal window", kWindowLabels, 7, 3,
                  "Manual only: frames the model sees at once. Larger is steadier and needs more GPU memory.",
                  "controlsGroup");
    define_bool(params, "lowVram", "Low VRAM", 0, "Manual only: uses less GPU memory, more disk and time.",
                "controlsGroup");

    define_group(params, "hintGroup", "Hint", false);
    define_choice(params, "hintMode", "Alpha hint", kHintLabels, 3, 0,
                  "None: the model finds the subject itself. Reference frame: one guide matte, carried forward and "
                  "backward. Every frame: a guide matte on each frame.",
                  "hintGroup");
    define_choice(params, "hintSource", "Hint from", kHintSourceLabels, 2, 0,
                  "Chroma key: made from the screen colour. Hint input: a matte connected to the Hint input "
                  "(Color page: Magic Mask, Qualifier, a window).",
                  "hintGroup");
    define_choice(params, "hintChannel", "Hint channel", kHintChannelLabels, 2, 0,
                  "Which channel of the Hint input is the matte.", "hintGroup");
    OfxPropertySetHandle rp;
    gParam->paramDefine(params, kOfxParamTypeInteger, "reference", &rp);
    place(rp, "Reference frame", "Clip frame (0 = first) where the reference hint is taken.", "hintGroup");
    gProp->propSetInt(rp, kOfxParamPropDefault, 0, 0);
    gProp->propSetInt(rp, kOfxParamPropMin, 0, 0);
    gProp->propSetInt(rp, kOfxParamPropDisplayMin, 0, 0);
    gProp->propSetInt(rp, kOfxParamPropDisplayMax, 0, 1000);
    gProp->propSetInt(rp, kOfxParamPropAnimates, 0, 0);
    define_button(params, "setReference", "Set Reference to Current Frame",
                  "Uses the frame under the playhead as the reference frame.", "hintGroup");
    OfxPropertySetHandle cp;
    gParam->paramDefine(params, kOfxParamTypeRGB, "screenColor", &cp);
    place(cp, "Screen colour", "Colour of the green or blue screen for the chroma hint.", "hintGroup");
    gProp->propSetDouble(cp, kOfxParamPropDefault, 0, 0.0);
    gProp->propSetDouble(cp, kOfxParamPropDefault, 1, 0.694);
    gProp->propSetDouble(cp, kOfxParamPropDefault, 2, 0.251);
    gProp->propSetInt(cp, kOfxParamPropAnimates, 0, 0);
    define_double(params, "similarity", "Similarity", 0.12, 0.01, 0.6, "How close a colour must be to the screen.",
                  "hintGroup", false);
    define_double(params, "softness", "Softness", 0.08, 0.0, 0.5, "Edge softness of the chroma hint.", "hintGroup",
                  false);
    define_double(params, "hintQuality", "Hint quality", 0.95, 0.0, 1.0,
                  "How much the model trusts the hint: 0.95 good matte, 0.75 rough, 0.5 or less very rough.",
                  "hintGroup", false);
    define_bool(params, "carryHint", "Carry hint", 1, "Passes the model's own matte from window to window.",
                "hintGroup");
    define_double(params, "carryQuality", "Carry quality", 0.95, 0.0, 1.0, "Trust in the carried matte.", "hintGroup",
                  false);

    define_group(params, "outputGroup", "Output", true);
    define_choice(params, "view", "View", kViewLabels, 6, 0,
                  "Cutout premultiplied composites over lower tracks. Alpha, Foreground and Checker are for checking.",
                  "outputGroup");
    define_double(params, "clipBlack", "Clip black", 0.0, 0.0, 0.99, "Alpha below this becomes fully transparent.",
                  "outputGroup", true);
    define_double(params, "clipWhite", "Clip white", 1.0, 0.01, 1.0, "Alpha above this becomes fully opaque.",
                  "outputGroup", true);

    define_group(params, "despillGroup", "Despill", true);
    define_choice(params, "despillColor", "Screen", kSpillColorLabels, 2, 0, "Colour of the spill to remove.",
                  "despillGroup");
    define_choice(params, "despillAlgo", "Algorithm", kSpillAlgoLabels, 3, 0,
                  "Average: CorridorKey's own. Max / Min: limit the screen channel to the larger / smaller of the "
                  "other two.",
                  "despillGroup");
    define_double(params, "despill", "Despill", 0.5, 0.0, 1.0, "Amount of spill removed (0 off).", "despillGroup",
                  true);
    return kOfxStatOK;
}

struct Range {
    double start = 0, end = 0;
};

Range source_range(OfxImageEffectHandle effect) {
    Range r;
    OfxImageClipHandle clip;
    if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &clip, nullptr) != kOfxStatOK) return r;
    OfxPropertySetHandle props;
    gEffect->clipGetPropertySet(clip, &props);
    gProp->propGetDouble(props, kOfxImageEffectPropFrameRange, 0, &r.start);
    gProp->propGetDouble(props, kOfxImageEffectPropFrameRange, 1, &r.end);
    return r;
}

double frame_rate(OfxImageEffectHandle effect) {
    OfxImageClipHandle clip;
    double fps = 24.0;
    if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &clip, nullptr) != kOfxStatOK) return fps;
    OfxPropertySetHandle props;
    gEffect->clipGetPropertySet(clip, &props);
    gProp->propGetDouble(props, kOfxImageEffectPropFrameRate, 0, &fps);
    return fps > 0 ? fps : 24.0;
}

std::wstring job_dir(const std::string& id) { return cache_root() + L"\\" + widen(id); }

std::string model_settings(OfxImageEffectHandle effect, OfxTime t) {
    double rgb[3];
    get_rgb(effect, "screenColor", t, rgb);
    int memory = get_int(effect, "memory", t);
    int hint = std::clamp(get_int(effect, "hintMode", t), 0, 2);
    int source = std::clamp(get_int(effect, "hintSource", t), 0, 1);
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "\"linear\": %s, \"scale\": %d, \"memory\": {\"auto\": %s, \"chunk\": %d, \"low_vram\": %s}, "
                  "\"hint\": {\"mode\": \"%s\", \"source\": \"%s\", \"channel\": %d, \"reference\": %d, "
                  "\"color\": [%.4f, %.4f, %.4f], \"similarity\": %.4f, "
                  "\"softness\": %.4f, \"quality\": %.4f}, \"carry_hint\": %s, \"carry_quality\": %.4f",
                  get_int(effect, "linearInput", t) ? "true" : "false",
                  kScales[std::clamp(get_int(effect, "resolution", t), 0, 2)], memory == 0 ? "true" : "false",
                  kWindows[std::clamp(get_int(effect, "window", t), 0, 6)],
                  get_int(effect, "lowVram", t) ? "true" : "false", kHintModes[hint], kHintSources[source],
                  get_int(effect, "hintChannel", t), std::max(0, get_int(effect, "reference", t)), rgb[0], rgb[1], rgb[2],
                  get_double(effect, "similarity", t), get_double(effect, "softness", t),
                  get_double(effect, "hintQuality", t), get_int(effect, "carryHint", t) ? "true" : "false",
                  get_double(effect, "carryQuality", t));
    return buf;
}

struct JobInfo {
    bool ok = false;
    int width = 0, height = 0, full_w = 0, full_h = 0, count = 0, x0 = 0, y0 = 0;
    double start = 0;
    std::string settings;
};

JobInfo load_job(const std::wstring& dir) {
    JobInfo j;
    std::string text;
    if (!read_text(dir + L"\\job.json", text)) return j;
    j.width = std::atoi(json_raw(text, "width").c_str());
    j.height = std::atoi(json_raw(text, "height").c_str());
    j.full_w = std::atoi(json_raw(text, "full_w").c_str());
    j.full_h = std::atoi(json_raw(text, "full_h").c_str());
    j.x0 = std::atoi(json_raw(text, "x0").c_str());
    j.y0 = std::atoi(json_raw(text, "y0").c_str());
    j.count = std::atoi(json_raw(text, "count").c_str());
    j.start = std::atof(json_raw(text, "start").c_str());
    j.settings = json_raw(text, "settings");
    j.ok = j.width > 0 && j.height > 0 && j.count > 0 && j.full_w > 0 && j.full_h > 0;
    return j;
}

struct Progress {
    std::string stage;
    int done = 0, total = 0;
    std::string error;
};

Progress load_progress(const std::wstring& dir) {
    Progress p;
    std::string text;
    if (!read_text(dir + L"\\progress.json", text)) return p;
    p.stage = json_raw(text, "stage");
    p.done = std::atoi(json_raw(text, "done").c_str());
    p.total = std::atoi(json_raw(text, "total").c_str());
    p.error = json_raw(text, "error");
    if (p.error == "null") p.error.clear();
    return p;
}

bool process_alive(const std::wstring& dir) {
    std::string text;
    if (!read_text(dir + L"\\pid", text)) return false;
    DWORD pid = static_cast<DWORD>(std::strtoul(trim(text).c_str(), nullptr, 10));
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

bool launch(const std::wstring& dir, std::string& error) {
    std::wstring root = ck2_root();
    std::wstring python = root + L"\\venv\\Scripts\\python.exe";
    std::wstring script = resources_dir() + L"\\ck2_job.py";
    if (!exists(python) || !exists(root + L"\\ck2\\corridorkey\\runtime.py")) {
        error = "CorridorKey 2.0 runtime not found at " + narrow(root) + ". Run install_runtime.ps1.";
        return false;
    }
    if (!exists(script)) {
        error = "ck2_job.py is missing from the plugin bundle. Reinstall the plugin.";
        return false;
    }
    std::wstring cmd = L"\"" + python + L"\" -u \"" + script + L"\" \"" + dir + L"\" \"" + root + L"\"";
    std::wstring log = dir + L"\\python.log";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                             root.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) {
        error = "Could not start Python (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    write_text(dir + L"\\pid", std::to_string(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct ImageRef {
    OfxPropertySetHandle handle = nullptr;
    char* data = nullptr;
    int row_bytes = 0;
    OfxRectI bounds{};
    ImageRef() = default;
    ImageRef(const ImageRef&) = delete;
    ImageRef& operator=(const ImageRef&) = delete;
    ~ImageRef() {
        if (handle) gEffect->clipReleaseImage(handle);
    }
    const float* px(int x, int y) const {
        return reinterpret_cast<const float*>(data + static_cast<ptrdiff_t>(y - bounds.y1) * row_bytes +
                                              static_cast<ptrdiff_t>(x - bounds.x1) * 4 * sizeof(float));
    }
    float* px(int x, int y) {
        return reinterpret_cast<float*>(data + static_cast<ptrdiff_t>(y - bounds.y1) * row_bytes +
                                        static_cast<ptrdiff_t>(x - bounds.x1) * 4 * sizeof(float));
    }
};

bool fetch(OfxImageClipHandle clip, OfxTime t, ImageRef& img) {
    if (gEffect->clipGetImage(clip, t, nullptr, &img.handle) != kOfxStatOK) return false;
    void* ptr = nullptr;
    gProp->propGetPointer(img.handle, kOfxImagePropData, 0, &ptr);
    gProp->propGetInt(img.handle, kOfxImagePropRowBytes, 0, &img.row_bytes);
    gProp->propGetIntN(img.handle, kOfxImagePropBounds, 4, &img.bounds.x1);
    char* depth = nullptr;
    gProp->propGetString(img.handle, kOfxImageEffectPropPixelDepth, 0, &depth);
    img.data = static_cast<char*>(ptr);
    return img.data && depth && !std::strcmp(depth, kOfxBitDepthFloat);
}

uint16_t to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff) return static_cast<uint16_t>(sign | 0x7c00 | (mant ? 0x200 : 0));
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000;
        uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    if (mant & 0x1000) ++h;
    return static_cast<uint16_t>(h);
}

float from_half(uint16_t h) {
    uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            x = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000 | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

bool write_frame(const ImageRef& img, const JobInfo& j, int scale, const std::wstring& path) {
    std::vector<uint16_t> row(static_cast<size_t>(j.width) * 3);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const float inv = 1.0f / static_cast<float>(scale * scale);
    for (int r = 0; r < j.height; ++r) {
        for (int c = 0; c < j.width; ++c) {
            float acc[3] = {0, 0, 0};
            for (int dy = 0; dy < scale; ++dy) {
                int y = j.y0 + j.full_h - 1 - (r * scale + dy);
                y = std::clamp(y, img.bounds.y1, img.bounds.y2 - 1);
                for (int dx = 0; dx < scale; ++dx) {
                    int x = std::clamp(j.x0 + c * scale + dx, img.bounds.x1, img.bounds.x2 - 1);
                    const float* p = img.px(x, y);
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
            }
            for (int k = 0; k < 3; ++k) {
                float v = acc[k] * inv;
                row[static_cast<size_t>(c) * 3 + k] = to_half(std::isfinite(v) ? v : 0.0f);
            }
        }
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * 2));
    }
    return static_cast<bool>(f);
}

bool write_hint(const ImageRef& img, const JobInfo& j, int scale, int channel, const std::wstring& path) {
    std::vector<uint16_t> row(static_cast<size_t>(j.width));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const float inv = 1.0f / static_cast<float>(scale * scale);
    for (int r = 0; r < j.height; ++r) {
        for (int c = 0; c < j.width; ++c) {
            float acc = 0.0f;
            for (int dy = 0; dy < scale; ++dy) {
                int y = std::clamp(j.y0 + j.full_h - 1 - (r * scale + dy), img.bounds.y1, img.bounds.y2 - 1);
                for (int dx = 0; dx < scale; ++dx) {
                    int x = std::clamp(j.x0 + c * scale + dx, img.bounds.x1, img.bounds.x2 - 1);
                    const float* p = img.px(x, y);
                    acc += channel == 1 ? p[3] : 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                }
            }
            float v = std::clamp(acc * inv, 0.0f, 1.0f);
            row[static_cast<size_t>(c)] = to_half(std::isfinite(v) ? v : 0.0f);
        }
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * 2));
    }
    return static_cast<bool>(f);
}

bool clip_connected(OfxImageEffectHandle effect, const char* name) {
    OfxImageClipHandle clip;
    if (gEffect->clipGetHandle(effect, name, &clip, nullptr) != kOfxStatOK) return false;
    OfxPropertySetHandle props;
    gEffect->clipGetPropertySet(clip, &props);
    int connected = 0;
    gProp->propGetInt(props, kOfxImageClipPropConnected, 0, &connected);
    return connected != 0;
}

bool collect(OfxImageEffectHandle effect, const std::wstring& dir, std::string& error) {
    std::lock_guard<std::mutex> lock(gCollect);
    if (exists(dir + L"\\collected") || exists(dir + L"\\done")) return true;
    JobInfo j = load_job(dir);
    if (!j.ok) {
        error = "Job file is missing or broken.";
        return false;
    }
    std::string text;
    read_text(dir + L"\\job.json", text);
    int scale = std::atoi(json_raw(text, "scale").c_str());
    if (scale < 1) scale = 1;
    OfxImageClipHandle src;
    if (gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &src, nullptr) != kOfxStatOK) {
        error = "No source clip.";
        return false;
    }
    std::string hint_mode = json_raw(text, "mode");
    bool hint_input = hint_mode != "none" && json_raw(text, "source") == "input";
    int hint_channel = std::atoi(json_raw(text, "channel").c_str());
    int reference = std::clamp(std::atoi(json_raw(text, "reference").c_str()), 0, j.count - 1);
    OfxImageClipHandle hint_clip = nullptr;
    if (hint_input) {
        if (!clip_connected(effect, kHintClip) ||
            gEffect->clipGetHandle(effect, kHintClip, &hint_clip, nullptr) != kOfxStatOK) {
            error = "Hint from: Hint input is selected but nothing is connected to the Hint input.";
            write_text(dir + L"\\progress.json",
                       "{\"stage\": \"error\", \"done\": 0, \"total\": 0, \"error\": \"" + json_escape(error) + "\"}");
            return false;
        }
    }
    std::wstring in = dir + L"\\in";
    CreateDirectoryW(in.c_str(), nullptr);
    bool progress = gProgress && gProgress->progressStart &&
                    gProgress->progressStart(effect, "CorridorKey 2.0: reading frames") == kOfxStatOK;
    for (int i = 0; i < j.count; ++i) {
        ImageRef img;
        if (!fetch(src, j.start + i, img)) {
            error = "Could not read source frame " + std::to_string(i) + ".";
            if (progress) gProgress->progressEnd(effect);
            return false;
        }
        wchar_t name[32];
        std::swprintf(name, 32, L"\\%05d.raw", i);
        if (!write_frame(img, j, scale, in + name)) {
            error = "Could not write frames to " + narrow(in) + " (disk full?).";
            if (progress) gProgress->progressEnd(effect);
            return false;
        }
        if (hint_clip && (hint_mode == "video" || i == reference)) {
            ImageRef matte;
            wchar_t hname[40];
            std::swprintf(hname, 40, L"\\hint_%05d.raw", i);
            if (!fetch(hint_clip, j.start + i, matte) || !write_hint(matte, j, scale, hint_channel, in + hname)) {
                error = "Could not read the Hint input at clip frame " + std::to_string(i) + ".";
                if (progress) gProgress->progressEnd(effect);
                return false;
            }
        }
        if (progress && gProgress->progressUpdate(effect, (i + 1.0) / j.count) == kOfxStatReplyNo) {
            gProgress->progressEnd(effect);
            error = "Cancelled.";
            return false;
        }
    }
    if (progress) gProgress->progressEnd(effect);
    write_text(dir + L"\\collected", "ok");
    std::string launch_error;
    if (!launch(dir, launch_error)) {
        error = launch_error;
        write_text(dir + L"\\progress.json",
                   "{\"stage\": \"error\", \"done\": 0, \"total\": 0, \"error\": \"" + json_escape(error) + "\"}");
        return false;
    }
    log_line("launched " + narrow(dir));
    return true;
}

std::string status_text(OfxImageEffectHandle effect, OfxTime t) {
    std::string id = get_string(effect, "jobId");
    if (id.empty()) return "Not keyed. Press Key Clip.";
    std::wstring dir = job_dir(id);
    JobInfo j = load_job(dir);
    if (!j.ok) return "Cache missing. Press Key Clip.";
    std::string size = std::to_string(j.width) + "x" + std::to_string(j.height);
    std::string changed = j.settings != model_settings(effect, t) ? " Settings changed: press Key Clip to apply." : "";
    Range r = source_range(effect);
    int now_count = static_cast<int>(std::floor(r.end - r.start + 0.5)) + 1;
    if (std::fabs(r.start - j.start) > 0.5 || now_count != j.count)
        changed += " Clip range changed since keying: press Key Clip again.";
    Progress p = load_progress(dir);
    std::string note;
    {
        std::string text;
        if (read_text(dir + L"\\progress.json", text)) note = json_raw(text, "note");
        if (note == "null") note.clear();
        if (!note.empty()) note = " " + note;
    }
    if (exists(dir + L"\\done"))
        return "Done: " + std::to_string(j.count) + " frames at " + size + "." + note + changed;
    if (!p.error.empty()) return "Error: " + p.error;
    if (p.stage == "queued") return "Queued: waiting for another CorridorKey job to finish.";
    if (!exists(dir + L"\\collected")) return "Waiting to read frames (play or scrub one frame).";
    if (!process_alive(dir) && p.stage != "done") return "Stopped. Press Key Clip to start again.";
    std::string stage = p.stage.empty() ? "starting" : p.stage;
    return "Keying (" + stage + ") " + std::to_string(p.done) + "/" + std::to_string(std::max(p.total, j.count)) +
           " at " + size + "...";
}

void refresh(OfxImageEffectHandle effect, OfxTime t, bool bump) {
    set_string(effect, "status", status_text(effect, t));
    if (bump)
        if (OfxParamHandle h = param(effect, "epoch")) {
            int v = 0;
            gParam->paramGetValue(h, &v);
            gParam->paramSetValue(h, v + 1);
        }
}

std::string new_id() {
    GUID g;
    CoCreateGuid(&g);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08lx%04x%04x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1]);
    return buf;
}

void cancel_job(const std::wstring& dir) {
    write_text(dir + L"\\cancel", "1");
    std::string text;
    if (read_text(dir + L"\\pid", text)) {
        DWORD pid = static_cast<DWORD>(std::strtoul(trim(text).c_str(), nullptr, 10));
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }
    if (!exists(dir + L"\\done"))
        write_text(dir + L"\\progress.json",
                   "{\"stage\": \"error\", \"done\": 0, \"total\": 0, \"error\": \"Cancelled.\"}");
}

OfxStatus key_clip(OfxImageEffectHandle effect, OfxTime t) {
    std::string old = get_string(effect, "jobId");
    if (!old.empty()) {
        std::wstring od = job_dir(old);
        if (process_alive(od)) cancel_job(od);
    }
    Range r = source_range(effect);
    int count = static_cast<int>(std::floor(r.end - r.start + 0.5)) + 1;
    if (count < 1 || count > 100000) {
        post(effect, kOfxMessageError, "CorridorKey 2.0: the clip has no frame range.");
        return kOfxStatFailed;
    }
    OfxImageClipHandle src;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
    OfxRectD rod{};
    gEffect->clipGetRegionOfDefinition(src, r.start, &rod);
    int x0 = static_cast<int>(std::floor(rod.x1)), y0 = static_cast<int>(std::floor(rod.y1));
    int full_w = static_cast<int>(std::ceil(rod.x2)) - x0, full_h = static_cast<int>(std::ceil(rod.y2)) - y0;
    if (full_w < 16 || full_h < 16) {
        post(effect, kOfxMessageError, "CorridorKey 2.0: the source size is unknown.");
        return kOfxStatFailed;
    }
    int scale = kScales[std::clamp(get_int(effect, "resolution", t), 0, 2)];
    int w = std::max(16, full_w / scale), h = std::max(16, full_h / scale);
    std::string settings = model_settings(effect, t);
    std::string id = new_id();
    std::wstring dir = job_dir(id);
    CreateDirectoryW(dir.c_str(), nullptr);
    char head[512];
    std::snprintf(head, sizeof head,
                  "{\"width\": %d, \"height\": %d, \"full_w\": %d, \"full_h\": %d, \"x0\": %d, \"y0\": %d, "
                  "\"count\": %d, \"start\": %.6f, \"fps\": %.6f, ",
                  w, h, full_w, full_h, x0, y0, count, r.start, frame_rate(effect));
    std::string body = std::string(head) + settings + ", \"settings\": \"" + json_escape(settings) + "\"}";
    if (!write_text(dir + L"\\job.json", body)) {
        post(effect, kOfxMessageError, "CorridorKey 2.0: cannot write to " + narrow(dir));
        return kOfxStatFailed;
    }
    ULARGE_INTEGER free_bytes{};
    GetDiskFreeSpaceExW(cache_root().c_str(), &free_bytes, nullptr, nullptr);
    double need = static_cast<double>(w) * h * count * (6.0 + 8.0 + 16.0);
    if (need > static_cast<double>(free_bytes.QuadPart)) {
        post(effect, kOfxMessageError,
             "CorridorKey 2.0: not enough disk space for the cache (" + std::to_string(int(need / 1073741824.0) + 1) +
                 " GB needed).");
        return kOfxStatFailed;
    }
    set_string(effect, "jobId", id);
    log_line("job " + id + " " + std::to_string(count) + " frames " + std::to_string(full_w) + "x" +
             std::to_string(full_h) + " start " + std::to_string(r.start));
    std::string error;
    if (!collect(effect, dir, error)) log_line("collect in instanceChanged failed: " + error);
    refresh(effect, t, true);
    return kOfxStatOK;
}

OfxStatus instance_changed(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    char* type = nullptr;
    char* name = nullptr;
    gProp->propGetString(in, kOfxPropType, 0, &type);
    gProp->propGetString(in, kOfxPropName, 0, &name);
    if (!type || !name || std::strcmp(type, kOfxTypeParameter)) return kOfxStatReplyDefault;
    OfxTime t = 0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    if (!std::strcmp(name, "keyClip")) return key_clip(effect, t);
    if (!std::strcmp(name, "refresh")) {
        refresh(effect, t, true);
        return kOfxStatOK;
    }
    if (!std::strcmp(name, "cancel")) {
        std::string id = get_string(effect, "jobId");
        if (!id.empty()) cancel_job(job_dir(id));
        refresh(effect, t, true);
        return kOfxStatOK;
    }
    if (!std::strcmp(name, "clearCache")) {
        std::string id = get_string(effect, "jobId");
        if (id.empty()) return kOfxStatOK;
        if (gMessage && gMessage->message(effect, kOfxMessageQuestion, nullptr,
                                          "Move this clip's CorridorKey cache to the Recycle Bin?") != kOfxStatReplyYes)
            return kOfxStatOK;
        std::wstring dir = job_dir(id);
        if (process_alive(dir)) cancel_job(dir);
        std::wstring from = dir;
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
            post(effect, kOfxMessageError, "CorridorKey 2.0: could not move the cache: " + narrow(dir));
            return kOfxStatFailed;
        }
        set_string(effect, "jobId", "");
        refresh(effect, t, true);
        return kOfxStatOK;
    }
    if (!std::strcmp(name, "setReference")) {
        Range r = source_range(effect);
        int index = std::max(0, static_cast<int>(std::floor(t - r.start + 0.5)));
        if (OfxParamHandle h = param(effect, "reference")) gParam->paramSetValue(h, index);
        if (get_int(effect, "hintMode", t) == 0)
            if (OfxParamHandle h = param(effect, "hintMode")) gParam->paramSetValue(h, 1);
        set_string(effect, "status", status_text(effect, t));
        return kOfxStatOK;
    }
    if (!std::strcmp(name, "openCache")) {
        std::string id = get_string(effect, "jobId");
        std::wstring dir = id.empty() ? cache_root() : job_dir(id);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return kOfxStatOK;
    }
    if (std::strcmp(name, "status") && std::strcmp(name, "epoch") && std::strcmp(name, "jobId"))
        set_string(effect, "status", status_text(effect, t));
    return kOfxStatOK;
}

bool read_result(const std::wstring& dir, const JobInfo& j, int index, std::vector<uint16_t>& data) {
    wchar_t name[40];
    std::swprintf(name, 40, L"\\out\\%05d.rgba", index);
    std::ifstream f(dir + name, std::ios::binary);
    if (!f) return false;
    data.resize(static_cast<size_t>(j.width) * j.height * 4);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size() * 2));
    return static_cast<bool>(f);
}

void despill(float rgb[3], int channel, int algo, float strength) {
    if (strength <= 0.0f) return;
    int a = channel == 1 ? 0 : 0, b = channel == 1 ? 2 : 1;
    float o[3] = {rgb[0], rgb[1], rgb[2]};
    if (algo == 0) {
        float spill = std::max(rgb[channel] - (rgb[a] + rgb[b]) * 0.5f, 0.0f);
        o[channel] -= spill;
        o[a] += spill * 0.5f;
        o[b] += spill * 0.5f;
    } else {
        float lim = algo == 1 ? std::max(rgb[a], rgb[b]) : std::min(rgb[a], rgb[b]);
        o[channel] = std::min(rgb[channel], lim);
    }
    for (int k = 0; k < 3; ++k) rgb[k] = rgb[k] * (1.0f - strength) + o[k] * strength;
}

void copy_source(ImageRef& out, const ImageRef& src, const OfxRectI& win) {
    for (int y = win.y1; y < win.y2; ++y)
        for (int x = win.x1; x < win.x2; ++x) {
            float* o = out.px(x, y);
            if (x >= src.bounds.x1 && x < src.bounds.x2 && y >= src.bounds.y1 && y < src.bounds.y2) {
                const float* s = src.px(x, y);
                o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = s[3];
            } else {
                o[0] = o[1] = o[2] = o[3] = 0.0f;
            }
        }
}

void overlay_bar(ImageRef& out, const OfxRectI& win, const OfxRectI& full, double frac, bool error) {
    int height = std::max(6, (full.y2 - full.y1) / 90);
    int filled = full.x1 + static_cast<int>((full.x2 - full.x1) * std::clamp(frac, 0.0, 1.0));
    for (int y = std::max(win.y1, full.y2 - height); y < std::min(win.y2, full.y2); ++y)
        for (int x = win.x1; x < win.x2; ++x) {
            float* o = out.px(x, y);
            bool on = x < filled;
            o[0] = error ? 0.9f : (on ? 1.0f : 0.15f);
            o[1] = error ? 0.1f : (on ? 0.55f : 0.15f);
            o[2] = error ? 0.1f : (on ? 0.05f : 0.15f);
            o[3] = 1.0f;
        }
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle in) {
    OfxTime t = 0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    OfxRectI win;
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, &win.x1);
    double rs[2] = {1.0, 1.0};
    gProp->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, rs);
    OfxImageClipHandle src_clip, out_clip;
    gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &src_clip, nullptr);
    gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &out_clip, nullptr);
    ImageRef out, src;
    if (!fetch(out_clip, t, out)) return kOfxStatFailed;
    bool have_src = fetch(src_clip, t, src);
    if (!have_src) {
        for (int y = win.y1; y < win.y2; ++y)
            for (int x = win.x1; x < win.x2; ++x) {
                float* o = out.px(x, y);
                o[0] = o[1] = o[2] = o[3] = 0.0f;
            }
        return kOfxStatOK;
    }
    int view = std::clamp(get_int(effect, "view", t), 0, 5);
    std::string id = get_string(effect, "jobId");
    if (view == 5 || id.empty()) {
        copy_source(out, src, win);
        return kOfxStatOK;
    }
    std::wstring dir = job_dir(id);
    JobInfo j = load_job(dir);
    if (!j.ok) {
        copy_source(out, src, win);
        return kOfxStatOK;
    }
    int index = static_cast<int>(std::floor(t - j.start + 0.5));
    if (!exists(dir + L"\\done")) {
        std::string error;
        if (!exists(dir + L"\\collected")) collect(effect, dir, error);
        Progress p = load_progress(dir);
        copy_source(out, src, win);
        bool failed = !p.error.empty() || !error.empty();
        double frac = failed ? 1.0 : (p.total > 0 ? double(p.done) / p.total : 0.0);
        if (p.stage == "keying") frac = 0.1 + 0.85 * frac;
        else if (p.stage == "finishing") frac = 0.95 + 0.05 * frac;
        else frac = 0.1 * frac;
        overlay_bar(out, win, src.bounds, frac, failed);
        return kOfxStatOK;
    }
    if (index < 0 || index >= j.count) {
        copy_source(out, src, win);
        return kOfxStatOK;
    }
    std::vector<uint16_t> data;
    if (!read_result(dir, j, index, data)) {
        copy_source(out, src, win);
        overlay_bar(out, win, src.bounds, 1.0, true);
        return kOfxStatOK;
    }
    int channel = get_int(effect, "despillColor", t) == 1 ? 2 : 1;
    int algo = std::clamp(get_int(effect, "despillAlgo", t), 0, 2);
    float strength = static_cast<float>(std::clamp(get_double(effect, "despill", t), 0.0, 1.0));
    float lo = static_cast<float>(get_double(effect, "clipBlack", t));
    float hi = static_cast<float>(get_double(effect, "clipWhite", t));
    if (hi <= lo + 1e-4f) hi = lo + 1e-4f;
    double sx = rs[0] > 0 ? rs[0] : 1.0, sy = rs[1] > 0 ? rs[1] : 1.0;
    double kx = double(j.width) / j.full_w, ky = double(j.height) / j.full_h;
    int checker = std::max(8, static_cast<int>(16 * sx));
    for (int y = win.y1; y < win.y2; ++y) {
        double fy = (y + 0.5) / sy - j.y0;
        double top = j.full_h - fy;
        double my = top * ky - 0.5;
        int r0 = static_cast<int>(std::floor(my));
        float wy = static_cast<float>(my - r0);
        int ra = std::clamp(r0, 0, j.height - 1), rb = std::clamp(r0 + 1, 0, j.height - 1);
        for (int x = win.x1; x < win.x2; ++x) {
            double fx = (x + 0.5) / sx - j.x0;
            double mx = fx * kx - 0.5;
            int c0 = static_cast<int>(std::floor(mx));
            float wx = static_cast<float>(mx - c0);
            int ca = std::clamp(c0, 0, j.width - 1), cb = std::clamp(c0 + 1, 0, j.width - 1);
            float v[4];
            for (int k = 0; k < 4; ++k) {
                auto at = [&](int r, int c) { return from_half(data[(static_cast<size_t>(r) * j.width + c) * 4 + k]); };
                float top_v = at(ra, ca) * (1 - wx) + at(ra, cb) * wx;
                float bot_v = at(rb, ca) * (1 - wx) + at(rb, cb) * wx;
                v[k] = top_v * (1 - wy) + bot_v * wy;
            }
            float a = std::clamp((v[3] - lo) / (hi - lo), 0.0f, 1.0f);
            float rgb[3] = {v[0], v[1], v[2]};
            despill(rgb, channel, algo, strength);
            float* o = out.px(x, y);
            switch (view) {
                case 0: o[0] = rgb[0] * a, o[1] = rgb[1] * a, o[2] = rgb[2] * a, o[3] = a; break;
                case 1: o[0] = rgb[0], o[1] = rgb[1], o[2] = rgb[2], o[3] = a; break;
                case 2: o[0] = o[1] = o[2] = a, o[3] = 1.0f; break;
                case 3: o[0] = rgb[0], o[1] = rgb[1], o[2] = rgb[2], o[3] = 1.0f; break;
                default: {
                    float bg = (((x / checker) + (y / checker)) & 1) ? 0.6f : 0.35f;
                    for (int k = 0; k < 3; ++k) o[k] = rgb[k] * a + bg * (1.0f - a);
                    o[3] = 1.0f;
                }
            }
        }
    }
    return kOfxStatOK;
}

OfxStatus frames_needed(OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    OfxTime t = 0;
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    double range[2] = {t, t};
    std::string id = get_string(effect, "jobId");
    if (!id.empty()) {
        std::wstring dir = job_dir(id);
        if (!exists(dir + L"\\collected") && !exists(dir + L"\\done")) {
            JobInfo j = load_job(dir);
            if (j.ok) range[0] = j.start, range[1] = j.start + j.count - 1;
        }
    }
    gProp->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, range);
    return kOfxStatOK;
}

OfxStatus clip_preferences(OfxImageEffectHandle effect, OfxPropertySetHandle out) {
    int view = std::clamp(get_int(effect, "view", 0.0), 0, 5);
    const char* premult = view == 0 || view == 5 ? kOfxImagePreMultiplied
                          : view == 1           ? kOfxImageUnPreMultiplied
                                                : kOfxImageOpaque;
    gProp->propSetString(out, kOfxImageEffectPropPreMultiplication, 0, premult);
    gProp->propSetInt(out, kOfxImageEffectFrameVarying, 0, 1);
    return kOfxStatOK;
}

OfxStatus create_instance(OfxImageEffectHandle effect) {
    set_string(effect, "status", status_text(effect, 0.0));
    return kOfxStatOK;
}

OfxStatus on_load() {
    if (!gHost) return kOfxStatErrMissingHostFeature;
    gProp = static_cast<OfxPropertySuiteV1*>(const_cast<void*>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1)));
    gEffect =
        static_cast<OfxImageEffectSuiteV1*>(const_cast<void*>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1)));
    gParam =
        static_cast<OfxParameterSuiteV1*>(const_cast<void*>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1)));
    gMessage = static_cast<OfxMessageSuiteV1*>(const_cast<void*>(gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1)));
    gProgress =
        static_cast<OfxProgressSuiteV1*>(const_cast<void*>(gHost->fetchSuite(gHost->host, kOfxProgressSuite, 1)));
    if (!gProp || !gEffect || !gParam) return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

OfxStatus dispatch(const char* action, OfxImageEffectHandle effect, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    if (!std::strcmp(action, kOfxActionLoad)) return on_load();
    if (!std::strcmp(action, kOfxActionDescribe)) return describe(effect);
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describe_in_context(effect, in);
    if (!std::strcmp(action, kOfxActionCreateInstance)) return create_instance(effect);
    if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(effect, in);
    if (!std::strcmp(action, kOfxActionInstanceChanged)) return instance_changed(effect, in);
    if (!std::strcmp(action, kOfxImageEffectActionGetFramesNeeded)) return frames_needed(effect, in, out);
    if (!std::strcmp(action, kOfxImageEffectActionGetClipPreferences)) return clip_preferences(effect, out);
    return kOfxStatReplyDefault;
}

OfxStatus main_entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!action) return kOfxStatErrBadHandle;
    try {
        return dispatch(action, effect, in, out);
    } catch (const std::exception& e) {
        log_line(std::string("exception in ") + action + ": " + e.what());
    } catch (...) {
        log_line(std::string("exception in ") + action);
    }
    return kOfxStatFailed;
}

void set_host(OfxHost* host) { gHost = host; }

OfxPlugin gPlugin = {kOfxImageEffectPluginApi, 1, kPluginId, 1, 0, set_host, main_entry};

}

OfxExport int OfxGetNumberOfPlugins(void) { return 1; }

OfxExport OfxPlugin* OfxGetPlugin(int nth) { return nth == 0 ? &gPlugin : nullptr; }
