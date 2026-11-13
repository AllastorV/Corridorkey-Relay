#include "../plugin/ck2_ofx.cpp"

#include <atomic>
#include <cstdarg>
#include <functional>
#include <map>
#include <memory>
#include <thread>

namespace host {

struct Val {
    int i = 0;
    double d = 0;
    std::string s;
    void* p = nullptr;
};

struct Props {
    std::mutex m;
    std::map<std::string, std::vector<Val>> v;
    Val& at(const char* k, int idx) {
        auto& vec = v[k];
        if (static_cast<int>(vec.size()) <= idx) vec.resize(idx + 1);
        return vec[idx];
    }
    bool has(const char* k, int idx) {
        auto it = v.find(k);
        return it != v.end() && static_cast<int>(it->second.size()) > idx;
    }
};

Props* P(OfxPropertySetHandle h) { return reinterpret_cast<Props*>(h); }
OfxPropertySetHandle H(Props* p) { return reinterpret_cast<OfxPropertySetHandle>(p); }

OfxStatus setPtr(OfxPropertySetHandle h, const char* k, int i, void* x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    P(h)->at(k, i).p = x;
    return kOfxStatOK;
}
OfxStatus setStr(OfxPropertySetHandle h, const char* k, int i, const char* x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    P(h)->at(k, i).s = x ? x : "";
    return kOfxStatOK;
}
OfxStatus setDbl(OfxPropertySetHandle h, const char* k, int i, double x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    P(h)->at(k, i).d = x;
    return kOfxStatOK;
}
OfxStatus setInt(OfxPropertySetHandle h, const char* k, int i, int x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    P(h)->at(k, i).i = x;
    return kOfxStatOK;
}
OfxStatus setDblN(OfxPropertySetHandle h, const char* k, int n, const double* x) {
    for (int i = 0; i < n; ++i) setDbl(h, k, i, x[i]);
    return kOfxStatOK;
}
OfxStatus setIntN(OfxPropertySetHandle h, const char* k, int n, const int* x) {
    for (int i = 0; i < n; ++i) setInt(h, k, i, x[i]);
    return kOfxStatOK;
}
OfxStatus getPtr(OfxPropertySetHandle h, const char* k, int i, void** x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    if (!P(h)->has(k, i)) return kOfxStatErrUnknown;
    *x = P(h)->at(k, i).p;
    return kOfxStatOK;
}
OfxStatus getStr(OfxPropertySetHandle h, const char* k, int i, char** x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    if (!P(h)->has(k, i)) return kOfxStatErrUnknown;
    *x = const_cast<char*>(P(h)->at(k, i).s.c_str());
    return kOfxStatOK;
}
OfxStatus getDbl(OfxPropertySetHandle h, const char* k, int i, double* x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    if (!P(h)->has(k, i)) return kOfxStatErrUnknown;
    *x = P(h)->at(k, i).d;
    return kOfxStatOK;
}
OfxStatus getInt(OfxPropertySetHandle h, const char* k, int i, int* x) {
    std::lock_guard<std::mutex> l(P(h)->m);
    if (!P(h)->has(k, i)) return kOfxStatErrUnknown;
    *x = P(h)->at(k, i).i;
    return kOfxStatOK;
}
OfxStatus getDblN(OfxPropertySetHandle h, const char* k, int n, double* x) {
    for (int i = 0; i < n; ++i)
        if (getDbl(h, k, i, &x[i]) != kOfxStatOK) return kOfxStatErrUnknown;
    return kOfxStatOK;
}
OfxStatus getIntN(OfxPropertySetHandle h, const char* k, int n, int* x) {
    for (int i = 0; i < n; ++i)
        if (getInt(h, k, i, &x[i]) != kOfxStatOK) return kOfxStatErrUnknown;
    return kOfxStatOK;
}
OfxStatus unsupported() { return kOfxStatErrUnsupported; }

struct Param {
    std::string type;
    Props desc;
    int i = 0;
    double d[3] = {0, 0, 0};
    std::string s;
};

struct Image {
    Props props;
    std::vector<float> data;
    struct Clip* clip = nullptr;
    double t = 0;
};

struct Clip {
    std::string name;
    Props props;
    bool output = false;
    OfxRectI rod{0, 0, 64, 36};
    std::function<void(int, int, double, float*)> pixel;
    int pad = 16;
    std::mutex m;
    std::map<double, std::unique_ptr<Image>> outputs;
    std::atomic<int> fetches{0};
    std::vector<double> times;
};

struct Effect {
    Props props;
    std::map<std::string, std::unique_ptr<Param>> params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
    OfxRectI out_bounds{0, 0, 64, 36};
};

Effect* E(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
Effect* E(OfxParamSetHandle h) { return reinterpret_cast<Effect*>(h); }

OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props) {
    auto p = std::make_unique<Param>();
    p->type = type;
    if (props) *props = H(&p->desc);
    E(ps)->params[name] = std::move(p);
    return kOfxStatOK;
}
OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props) {
    auto it = E(ps)->params.find(name);
    if (it == E(ps)->params.end()) return kOfxStatErrUnknown;
    *h = reinterpret_cast<OfxParamHandle>(it->second.get());
    if (props) *props = H(&it->second->desc);
    return kOfxStatOK;
}
bool is_int(const std::string& t) {
    return t == kOfxParamTypeInteger || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice;
}
OfxStatus get_value(Param* p, va_list ap) {
    if (is_int(p->type)) *va_arg(ap, int*) = p->i;
    else if (p->type == kOfxParamTypeDouble) *va_arg(ap, double*) = p->d[0];
    else if (p->type == kOfxParamTypeRGB) {
        for (int k = 0; k < 3; ++k) *va_arg(ap, double*) = p->d[k];
    } else if (p->type == kOfxParamTypeString) *va_arg(ap, char**) = const_cast<char*>(p->s.c_str());
    else return kOfxStatErrUnsupported;
    return kOfxStatOK;
}
OfxStatus paramGetValue(OfxParamHandle h, ...) {
    va_list ap;
    va_start(ap, h);
    OfxStatus s = get_value(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime, ...) {
    va_list ap;
    va_start(ap, h);
    va_arg(ap, double);
    va_end(ap);
    va_list aq;
    OfxTime dummy = 0;
    (void)dummy;
    return kOfxStatOK;
}
OfxStatus paramSetValue(OfxParamHandle h, ...) {
    Param* p = reinterpret_cast<Param*>(h);
    va_list ap;
    va_start(ap, h);
    if (is_int(p->type)) p->i = va_arg(ap, int);
    else if (p->type == kOfxParamTypeDouble) p->d[0] = va_arg(ap, double);
    else if (p->type == kOfxParamTypeRGB)
        for (int k = 0; k < 3; ++k) p->d[k] = va_arg(ap, double);
    else if (p->type == kOfxParamTypeString) p->s = va_arg(ap, const char*);
    va_end(ap);
    return kOfxStatOK;
}

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* p) {
    *p = H(&E(e)->props);
    return kOfxStatOK;
}
OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p) {
    *p = reinterpret_cast<OfxParamSetHandle>(e);
    return kOfxStatOK;
}
OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* p) {
    auto c = std::make_unique<Clip>();
    c->name = name;
    c->output = !std::strcmp(name, kOfxImageEffectOutputClipName);
    *p = H(&c->props);
    E(e)->clips[name] = std::move(c);
    return kOfxStatOK;
}
OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* c, OfxPropertySetHandle* p) {
    auto it = E(e)->clips.find(name);
    if (it == E(e)->clips.end()) return kOfxStatErrBadHandle;
    *c = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (p) *p = H(&it->second->props);
    return kOfxStatOK;
}
Clip* C(OfxImageClipHandle c) { return reinterpret_cast<Clip*>(c); }
OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* p) {
    *p = H(&C(c)->props);
    return kOfxStatOK;
}

thread_local OfxRectI tOutBounds{0, 0, 64, 36};

OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime t, const OfxRectD*, OfxPropertySetHandle* out) {
    Clip* c = C(ch);
    auto* img = new Image();
    img->clip = c;
    img->t = t;
    OfxRectI b = c->output ? tOutBounds : c->rod;
    if (!c->output) {
        int connected = 1;
        getInt(H(&c->props), kOfxImageClipPropConnected, 0, &connected);
        if (!connected || !c->pixel) {
            delete img;
            return kOfxStatFailed;
        }
        c->fetches++;
        std::lock_guard<std::mutex> l(c->m);
        c->times.push_back(t);
    }
    int w = b.x2 - b.x1, h = b.y2 - b.y1;
    int stride = w * 4 + c->pad;
    img->data.assign(static_cast<size_t>(stride) * h, c->output ? -7.0f : 0.0f);
    if (!c->output)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) c->pixel(b.x1 + x, b.y1 + y, t, &img->data[static_cast<size_t>(y) * stride + x * 4]);
    setPtr(H(&img->props), kOfxImagePropData, 0, img->data.data());
    setInt(H(&img->props), kOfxImagePropRowBytes, 0, stride * 4);
    setIntN(H(&img->props), kOfxImagePropBounds, 4, &b.x1);
    setStr(H(&img->props), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
    setStr(H(&img->props), kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    *out = H(&img->props);
    return kOfxStatOK;
}
OfxStatus clipReleaseImage(OfxPropertySetHandle p) {
    Image* img = reinterpret_cast<Image*>(reinterpret_cast<char*>(P(p)) - offsetof(Image, props));
    if (img->clip->output) {
        std::lock_guard<std::mutex> l(img->clip->m);
        img->clip->outputs[img->t].reset(img);
        return kOfxStatOK;
    }
    delete img;
    return kOfxStatOK;
}
OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle c, OfxTime, OfxRectD* r) {
    r->x1 = C(c)->rod.x1, r->y1 = C(c)->rod.y1, r->x2 = C(c)->rod.x2, r->y2 = C(c)->rod.y2;
    return kOfxStatOK;
}

std::vector<std::string> gMessages;
std::mutex gMsgM;
OfxStatus message(void*, const char* type, const char*, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(gMsgM);
    gMessages.push_back(std::string(type) + ": " + buf);
    return !std::strcmp(type, kOfxMessageQuestion) ? kOfxStatReplyYes : kOfxStatOK;
}

OfxPropertySuiteV1 gPropSuite;
OfxParameterSuiteV1 gParamSuite;
OfxImageEffectSuiteV1 gEffectSuite;
OfxMessageSuiteV1 gMsgSuite;
Props gHostProps;
OfxHost gHostStruct;

const void* fetchSuite(OfxPropertySetHandle, const char* name, int) {
    if (!std::strcmp(name, kOfxPropertySuite)) return &gPropSuite;
    if (!std::strcmp(name, kOfxParameterSuite)) return &gParamSuite;
    if (!std::strcmp(name, kOfxImageEffectSuite)) return &gEffectSuite;
    if (!std::strcmp(name, kOfxMessageSuite)) return &gMsgSuite;
    return nullptr;
}

}

OfxStatus paramGetValueAtTimeImpl(OfxParamHandle h, OfxTime, ...);

namespace host {
OfxStatus pgvat(OfxParamHandle h, OfxTime t, ...) {
    va_list ap;
    va_start(ap, t);
    OfxStatus s = get_value(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
}

using namespace host;

int gFailures = 0;
int gChecks = 0;
#define CHECK(cond, msg)                                                                 \
    do {                                                                                 \
        ++gChecks;                                                                       \
        if (!(cond)) {                                                                   \
            ++gFailures;                                                                 \
            std::printf("FAIL %s:%d %s\n", __func__, __LINE__, std::string(msg).c_str()); \
        }                                                                                \
    } while (0)

OfxPlugin* plug() { return OfxGetPlugin(0); }
OfxStatus act(const char* a, Effect* e, Props* in = nullptr, Props* out = nullptr) {
    return plug()->mainEntry(a, e, in ? H(in) : nullptr, out ? H(out) : nullptr);
}

std::wstring gTemp;

std::unique_ptr<Effect> make_effect(const char* context = kOfxImageEffectContextGeneral) {
    auto e = std::make_unique<Effect>();
    act(kOfxActionDescribe, e.get());
    Props in;
    setStr(H(&in), kOfxImageEffectPropContext, 0, context);
    act(kOfxImageEffectActionDescribeInContext, e.get(), &in);
    for (auto& [name, p] : e->params) {
        if (is_int(p->type)) getInt(H(&p->desc), kOfxParamPropDefault, 0, &p->i);
        if (p->type == kOfxParamTypeDouble) getDbl(H(&p->desc), kOfxParamPropDefault, 0, &p->d[0]);
        if (p->type == kOfxParamTypeRGB)
            for (int k = 0; k < 3; ++k) getDbl(H(&p->desc), kOfxParamPropDefault, k, &p->d[k]);
        if (p->type == kOfxParamTypeString) {
            char* s = nullptr;
            if (getStr(H(&p->desc), kOfxParamPropDefault, 0, &s) == kOfxStatOK && s) p->s = s;
        }
    }
    for (auto& [name, c] : e->clips) {
        double range[2] = {10, 19};
        setDblN(H(&c->props), kOfxImageEffectPropFrameRange, 2, range);
        setDbl(H(&c->props), kOfxImageEffectPropFrameRate, 0, 25.0);
        setInt(H(&c->props), kOfxImageClipPropConnected, 0, name == std::string(kHintClip) ? 0 : 1);
    }
    act(kOfxActionCreateInstance, e.get());
    return e;
}

Clip& clip(Effect* e, const char* n) { return *e->clips.at(n); }
Param& prm(Effect* e, const char* n) { return *e->params.at(n); }

void press(Effect* e, const char* name, double t = 10) {
    Props in;
    setStr(H(&in), kOfxPropType, 0, kOfxTypeParameter);
    setStr(H(&in), kOfxPropName, 0, name);
    setStr(H(&in), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    setDbl(H(&in), kOfxPropTime, 0, t);
    act(kOfxActionInstanceChanged, e, &in);
}

Image* render(Effect* e, double t, OfxRectI win, double scale = 1.0, OfxRectI bounds = {0, 0, 64, 36}) {
    tOutBounds = bounds;
    Props in;
    setDbl(H(&in), kOfxPropTime, 0, t);
    setIntN(H(&in), kOfxImageEffectPropRenderWindow, 4, &win.x1);
    double rs[2] = {scale, scale};
    setDblN(H(&in), kOfxImageEffectPropRenderScale, 2, rs);
    OfxStatus s = act(kOfxImageEffectActionRender, e, &in);
    Clip& out = clip(e, kOfxImageEffectOutputClipName);
    std::lock_guard<std::mutex> l(out.m);
    auto it = out.outputs.find(t);
    return s == kOfxStatOK && it != out.outputs.end() ? it->second.get() : nullptr;
}

const float* at(Image* img, int x, int y) {
    int b[4];
    getIntN(H(&img->props), kOfxImagePropBounds, 4, b);
    int rb = 0;
    getInt(H(&img->props), kOfxImagePropRowBytes, 0, &rb);
    return &img->data[static_cast<size_t>(y - b[1]) * (rb / 4) + (x - b[0]) * 4];
}

bool close_to(float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; }

void write_half_file(const std::wstring& path, const std::vector<float>& v) {
    std::vector<uint16_t> h(v.size());
    for (size_t i = 0; i < v.size(); ++i) h[i] = to_half(v[i]);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size() * 2));
}

std::string make_done_job(int w, int h, int full_w, int full_h, int x0, int y0, int count, double start,
                          const std::function<void(int, int, int, float*)>& px) {
    std::string id = new_id();
    std::wstring dir = job_dir(id);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\out").c_str(), nullptr);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "{\"width\": %d, \"height\": %d, \"full_w\": %d, \"full_h\": %d, \"x0\": %d, \"y0\": %d, \"count\": %d, "
                  "\"start\": %.1f, \"fps\": 25, \"settings\": \"x\"}",
                  w, h, full_w, full_h, x0, y0, count, start);
    write_text(dir + L"\\job.json", buf);
    for (int i = 0; i < count; ++i) {
        std::vector<float> v(static_cast<size_t>(w) * h * 4);
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < w; ++c) px(i, r, c, &v[(static_cast<size_t>(r) * w + c) * 4]);
        wchar_t name[40];
        std::swprintf(name, 40, L"\\out\\%05d.rgba", i);
        write_half_file(dir + name, v);
    }
    write_text(dir + L"\\done", "ok");
    return id;
}

void source_gradient(Effect* e) {
    clip(e, kOfxImageEffectSimpleSourceClipName).pixel = [](int x, int y, double t, float* p) {
        p[0] = x / 1000.0f, p[1] = y / 1000.0f, p[2] = static_cast<float>(t) / 100.0f, p[3] = 1.0f;
    };
}

void test_half_exhaustive() {
    int bad = 0;
    for (uint32_t h = 0; h < 65536; ++h) {
        float f = from_half(static_cast<uint16_t>(h));
        if (std::isnan(f)) continue;
        if (to_half(f) != h) ++bad;
    }
    CHECK(bad == 0, "half roundtrip mismatches: " + std::to_string(bad));
    CHECK(to_half(1e9f) == 0x7c00, "overflow -> inf");
    CHECK(to_half(-1e9f) == 0xfc00, "negative overflow -> -inf");
    CHECK(to_half(1e-10f) == 0, "underflow -> 0");
    CHECK(std::isnan(from_half(to_half(std::nanf("")))), "nan stays nan");
    double worst = 0;
    for (int i = 1; i < 200000; ++i) {
        float f = std::ldexp(static_cast<float>(i % 1000 + 1000) / 1000.0f, (i % 30) - 14);
        worst = std::max(worst, static_cast<double>(std::fabs(from_half(to_half(f)) - f) / f));
    }
    CHECK(worst <= 1.0 / 2048 + 1e-7, "relative rounding error " + std::to_string(worst));
}

void test_despill() {
    float a[3] = {0.2f, 0.8f, 0.2f};
    despill(a, 1, 0, 1.0f);
    CHECK(close_to(a[1], 0.2f) && close_to(a[0], 0.5f) && close_to(a[2], 0.5f), "average green");
    float b[3] = {0.2f, 0.8f, 0.2f};
    despill(b, 1, 0, 0.0f);
    CHECK(close_to(b[1], 0.8f), "strength 0 is identity");
    float c[3] = {0.1f, 0.3f, 0.9f};
    despill(c, 2, 1, 1.0f);
    CHECK(close_to(c[2], 0.3f) && close_to(c[0], 0.1f), "max blue");
    float d[3] = {0.1f, 0.3f, 0.9f};
    despill(d, 2, 2, 1.0f);
    CHECK(close_to(d[2], 0.1f), "min blue");
    float e[3] = {0.9f, 0.1f, 0.9f};
    despill(e, 1, 0, 1.0f);
    CHECK(close_to(e[1], 0.1f) && close_to(e[0], 0.9f), "no spill no change");
}

void test_describe_contexts() {
    auto g = make_effect(kOfxImageEffectContextGeneral);
    CHECK(g->clips.count(kHintClip) == 1, "general context has Hint input");
    auto f = make_effect(kOfxImageEffectContextFilter);
    CHECK(f->clips.count(kHintClip) == 0, "filter context has no Hint input");
    for (const char* n : {"keyClip", "refresh", "cancel", "clearCache", "status", "jobId", "reference", "view"})
        CHECK(g->params.count(n) == 1, std::string("param ") + n);
    CHECK(prm(g.get(), "status").s.find("Not keyed") != std::string::npos, "initial status");
}

void test_vram_limit_settings() {
    auto e = make_effect();
    Param& v = prm(e.get(), "vramLimit");
    CHECK(v.d[0] >= kMinVramGb, "vram limit default >= minimum");
    CHECK(model_settings(reinterpret_cast<OfxImageEffectHandle>(e.get()), 0).find("\"auto\": true") != std::string::npos, "auto by default");
    CHECK(model_settings(reinterpret_cast<OfxImageEffectHandle>(e.get()), 0).find("\"limit_gb\": 0.00") != std::string::npos, "no limit in auto");
    prm(e.get(), "memory").i = 2;
    v.d[0] = 4.5;
    std::string s = model_settings(reinterpret_cast<OfxImageEffectHandle>(e.get()), 0);
    CHECK(s.find("\"auto\": true") != std::string::npos && s.find("\"limit_gb\": 4.50") != std::string::npos,
          "vram limit mode: " + s);
    v.d[0] = 1.0;
    CHECK(model_settings(reinterpret_cast<OfxImageEffectHandle>(e.get()), 0).find("\"limit_gb\": 3.50") != std::string::npos, "limit clamped to minimum");
    prm(e.get(), "memory").i = 1;
    s = model_settings(reinterpret_cast<OfxImageEffectHandle>(e.get()), 0);
    CHECK(s.find("\"auto\": false") != std::string::npos && s.find("\"limit_gb\": 0.00") != std::string::npos,
          "manual has no limit");
}

void test_collect_orientation_and_scale() {
    auto e = make_effect();
    source_gradient(e.get());
    prm(e.get(), "resolution").i = 1;
    press(e.get(), "keyClip");
    std::string id = prm(e.get(), "jobId").s;
    CHECK(!id.empty(), "job id set");
    std::wstring dir = job_dir(id);
    JobInfo j = load_job(dir);
    CHECK(j.ok && j.width == 32 && j.height == 18 && j.count == 10, "job geometry");
    CHECK(exists(dir + L"\\collected"), "frames collected in instanceChanged");
    std::ifstream f(dir + L"\\in\\00003.raw", std::ios::binary);
    std::vector<uint16_t> raw(32 * 18 * 3);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
    CHECK(static_cast<bool>(f), "raw size");
    CHECK(close_to(from_half(raw[0]), 0.0005f, 1e-4f), "top-left R = avg x 0,1");
    CHECK(close_to(from_half(raw[1]), 0.0345f, 2e-4f), "top row G = avg y 35,34 (flipped)");
    CHECK(close_to(from_half(raw[2]), 0.13f, 1e-3f), "frame 3 -> time 13");
    size_t last = (static_cast<size_t>(17) * 32 + 31) * 3;
    CHECK(close_to(from_half(raw[last]), 0.0625f, 2e-4f), "bottom-right R = avg x 62,63");
    CHECK(close_to(from_half(raw[last + 1]), 0.0005f, 1e-4f), "bottom row G = avg y 1,0");
    std::string status = prm(e.get(), "status").s;
    CHECK(status.find("runtime not found") != std::string::npos, "missing runtime reported: " + status);
    CHECK(clip(e.get(), kOfxImageEffectSimpleSourceClipName).fetches == 10, "each frame fetched once");
}

void test_nan_source_and_odd_size() {
    auto e = make_effect();
    Clip& s = clip(e.get(), kOfxImageEffectSimpleSourceClipName);
    s.rod = {0, 0, 333, 187};
    s.pixel = [](int x, int y, double, float* p) {
        p[0] = (x + y) % 7 == 0 ? std::nanf("") : 0.5f;
        p[1] = (x * y) % 11 == 0 ? INFINITY : 0.25f;
        p[2] = -INFINITY, p[3] = 1;
    };
    prm(e.get(), "resolution").i = 2;
    press(e.get(), "keyClip");
    std::wstring dir = job_dir(prm(e.get(), "jobId").s);
    JobInfo j = load_job(dir);
    CHECK(j.width == 83 && j.height == 46, "333x187 quarter -> 83x46");
    std::ifstream f(dir + L"\\in\\00000.raw", std::ios::binary | std::ios::ate);
    CHECK(static_cast<long long>(f.tellg()) == 83LL * 46 * 3 * 2, "raw byte size");
    f.seekg(0);
    std::vector<uint16_t> raw(83 * 46 * 3);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
    int nonfinite = 0;
    for (auto h : raw)
        if (!std::isfinite(from_half(h))) ++nonfinite;
    CHECK(nonfinite == 0, "NaN/Inf written as finite: " + std::to_string(nonfinite));
}

void test_render_views_orientation_scale() {
    auto e = make_effect();
    source_gradient(e.get());
    std::string id = make_done_job(4, 2, 64, 36, 0, 0, 2, 10, [](int, int r, int, float* p) {
        if (r == 0) p[0] = 1, p[1] = 0, p[2] = 0, p[3] = 1;
        else p[0] = 0, p[1] = 0, p[2] = 1, p[3] = 0.5f;
    });
    prm(e.get(), "jobId").s = id;
    prm(e.get(), "despill").d[0] = 0;
    Image* img = render(e.get(), 10, {0, 0, 64, 36});
    CHECK(img, "render ok");
    if (!img) return;
    const float* top = at(img, 10, 35);
    const float* bot = at(img, 10, 0);
    CHECK(close_to(top[0], 1) && close_to(top[3], 1), "top of image is model row 0 (red)");
    CHECK(close_to(bot[2], 0.5f) && close_to(bot[3], 0.5f), "bottom premultiplied blue");
    prm(e.get(), "view").i = 1;
    img = render(e.get(), 11, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 10, 0)[2], 1.0f) && close_to(at(img, 10, 0)[3], 0.5f), "straight view");
    prm(e.get(), "view").i = 2;
    img = render(e.get(), 10, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 5, 0)[0], 0.5f) && close_to(at(img, 5, 0)[3], 1), "alpha view");
    prm(e.get(), "view").i = 4;
    img = render(e.get(), 10, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 5, 0)[3], 1) && at(img, 5, 0)[0] > 0.1f, "checker opaque");
    prm(e.get(), "view").i = 0;
    img = render(e.get(), 15, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 20, 7)[0], 0.020f) && close_to(at(img, 20, 7)[2], 0.15f), "outside range = source");
    img = render(e.get(), 10, {0, 0, 32, 18}, 0.5, {0, 0, 32, 18});
    CHECK(img && close_to(at(img, 5, 17)[0], 1) && close_to(at(img, 5, 0)[2], 0.5f), "render scale 0.5");
    img = render(e.get(), 11, {16, 4, 24, 30});
    CHECK(img && at(img, 0, 0)[0] == -7.0f && at(img, 63, 35)[0] == -7.0f, "outside render window untouched");
    CHECK(img && at(img, 16, 29)[3] > 0, "inside render window written");
    prm(e.get(), "clipBlack").d[0] = 0.9;
    prm(e.get(), "clipWhite").d[0] = 0.1;
    img = render(e.get(), 10, {0, 0, 64, 36});
    bool finite = true;
    for (int y = 0; img && y < 36; ++y)
        for (int x = 0; x < 64; ++x)
            for (int k = 0; k < 4; ++k) finite = finite && std::isfinite(at(img, x, y)[k]);
    CHECK(finite, "inverted clip black/white stays finite");
}

void test_offset_origin() {
    auto e = make_effect();
    Clip& s = clip(e.get(), kOfxImageEffectSimpleSourceClipName);
    s.rod = {100, 50, 164, 86};
    source_gradient(e.get());
    std::string id = make_done_job(64, 36, 64, 36, 100, 50, 1, 10, [](int, int r, int c, float* p) {
        p[0] = c / 64.0f, p[1] = r / 36.0f, p[2] = 0, p[3] = 1;
    });
    prm(e.get(), "jobId").s = id;
    prm(e.get(), "despill").d[0] = 0;
    Image* img = render(e.get(), 10, {100, 50, 164, 86}, 1.0, {100, 50, 164, 86});
    CHECK(img && close_to(at(img, 100, 85)[0], 0) && close_to(at(img, 100, 85)[1], 0), "offset top-left");
    CHECK(img && close_to(at(img, 163, 50)[0], 63 / 64.0f) && close_to(at(img, 163, 50)[1], 35 / 36.0f), "offset bottom-right");
}

void test_concurrent_renders() {
    auto e = make_effect();
    source_gradient(e.get());
    std::string id = make_done_job(64, 36, 64, 36, 0, 0, 10, 10, [](int i, int r, int c, float* p) {
        p[0] = i / 10.0f, p[1] = r / 36.0f, p[2] = c / 64.0f, p[3] = 1;
    });
    prm(e.get(), "jobId").s = id;
    prm(e.get(), "despill").d[0] = 0;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int k = 0; k < 8; ++k)
        threads.emplace_back([&, k] {
            for (int n = 0; n < 40; ++n) {
                double t = 10 + (n + k) % 10;
                tOutBounds = {0, 0, 64, 36};
                Props in;
                setDbl(H(&in), kOfxPropTime, 0, t);
                OfxRectI win{0, 0, 64, 36};
                setIntN(H(&in), kOfxImageEffectPropRenderWindow, 4, &win.x1);
                double rs[2] = {1, 1};
                setDblN(H(&in), kOfxImageEffectPropRenderScale, 2, rs);
                if (act(kOfxImageEffectActionRender, e.get(), &in) != kOfxStatOK) errors++;
            }
        });
    for (auto& t : threads) t.join();
    CHECK(errors == 0, "concurrent render failures: " + std::to_string(errors.load()));
    Clip& out = clip(e.get(), kOfxImageEffectOutputClipName);
    for (int i = 0; i < 10; ++i) {
        auto it = out.outputs.find(10 + i);
        CHECK(it != out.outputs.end() && close_to(at(it->second.get(), 3, 3)[0], i / 10.0f), "frame " + std::to_string(i));
    }
}

void test_concurrent_collect_in_render() {
    auto e = make_effect();
    source_gradient(e.get());
    Clip& s = clip(e.get(), kOfxImageEffectSimpleSourceClipName);
    std::string id = new_id();
    std::wstring dir = job_dir(id);
    CreateDirectoryW(dir.c_str(), nullptr);
    write_text(dir + L"\\job.json",
               "{\"width\": 64, \"height\": 36, \"full_w\": 64, \"full_h\": 36, \"x0\": 0, \"y0\": 0, \"count\": 10, "
               "\"start\": 10, \"fps\": 25, \"scale\": 1, \"hint\": {\"mode\": \"none\"}, \"settings\": \"x\"}");
    prm(e.get(), "jobId").s = id;
    std::vector<std::thread> threads;
    for (int k = 0; k < 6; ++k)
        threads.emplace_back([&, k] {
            tOutBounds = {0, 0, 64, 36};
            Props in;
            setDbl(H(&in), kOfxPropTime, 0, 10.0 + k);
            OfxRectI win{0, 0, 64, 36};
            setIntN(H(&in), kOfxImageEffectPropRenderWindow, 4, &win.x1);
            double rs[2] = {1, 1};
            setDblN(H(&in), kOfxImageEffectPropRenderScale, 2, rs);
            act(kOfxImageEffectActionRender, e.get(), &in);
        });
    for (auto& t : threads) t.join();
    CHECK(exists(dir + L"\\collected"), "collected once");
    int range_fetches = 0;
    for (double t : s.times) range_fetches += 1;
    CHECK(range_fetches == 10 + 6, "10 frames collected once + 6 render fetches, got " + std::to_string(range_fetches));
    Progress p = load_progress(dir);
    CHECK(!p.error.empty(), "launch failure recorded");
}

void test_hint_input_not_connected() {
    auto e = make_effect();
    source_gradient(e.get());
    prm(e.get(), "hintMode").i = 1;
    prm(e.get(), "hintSource").i = 1;
    press(e.get(), "keyClip");
    std::string status = prm(e.get(), "status").s;
    CHECK(status.find("nothing is connected to the Hint input") != std::string::npos, "status: " + status);
    CHECK(clip(e.get(), kOfxImageEffectSimpleSourceClipName).fetches == 0, "no frames read without hint");
}

void test_hint_input_connected() {
    auto e = make_effect();
    source_gradient(e.get());
    Clip& h = clip(e.get(), kHintClip);
    setInt(H(&h.props), kOfxImageClipPropConnected, 0, 1);
    h.pixel = [](int x, int, double, float* p) { p[0] = p[1] = p[2] = x < 32 ? 1.0f : 0.0f, p[3] = 0.25f; };
    prm(e.get(), "hintMode").i = 1;
    prm(e.get(), "hintSource").i = 1;
    prm(e.get(), "reference").i = 4;
    press(e.get(), "keyClip");
    std::wstring dir = job_dir(prm(e.get(), "jobId").s);
    CHECK(exists(dir + L"\\in\\hint_00004.raw") && !exists(dir + L"\\in\\hint_00000.raw"), "only reference hint read");
    CHECK(h.fetches == 1, "hint fetched once");
    std::ifstream f(dir + L"\\in\\hint_00004.raw", std::ios::binary);
    std::vector<uint16_t> raw(64 * 36);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
    CHECK(close_to(from_half(raw[0]), 1) && close_to(from_half(raw[63]), 0), "luminance hint");
}

void test_status_and_broken_cache() {
    auto e = make_effect();
    source_gradient(e.get());
    std::string id = make_done_job(8, 4, 64, 36, 0, 0, 10, 10, [](int, int, int, float* p) { p[0] = p[1] = p[2] = p[3] = 1; });
    prm(e.get(), "jobId").s = id;
    press(e.get(), "refresh");
    CHECK(prm(e.get(), "status").s.find("Settings changed") != std::string::npos, "settings mismatch detected");
    double range[2] = {10, 30};
    setDblN(H(&clip(e.get(), kOfxImageEffectSimpleSourceClipName).props), kOfxImageEffectPropFrameRange, 2, range);
    press(e.get(), "refresh");
    CHECK(prm(e.get(), "status").s.find("Clip range changed") != std::string::npos, "range change detected");
    std::wstring dir = job_dir(id);
    std::ofstream(dir + L"\\out\\00002.rgba", std::ios::binary | std::ios::trunc) << "x";
    Image* img = render(e.get(), 12, {0, 0, 64, 36});
    CHECK(img != nullptr, "truncated frame does not fail the render");
    prm(e.get(), "jobId").s = "deadbeef";
    img = render(e.get(), 13, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 20, 7)[0], 0.020f), "missing cache = source");
    write_text(job_dir(id) + L"\\job.json", "{not json");
    prm(e.get(), "jobId").s = id;
    img = render(e.get(), 14, {0, 0, 64, 36});
    CHECK(img && close_to(at(img, 20, 7)[0], 0.020f), "corrupt job.json = source");
}

void test_single_frame_and_empty_range() {
    auto e = make_effect();
    source_gradient(e.get());
    double one[2] = {5, 5};
    setDblN(H(&clip(e.get(), kOfxImageEffectSimpleSourceClipName).props), kOfxImageEffectPropFrameRange, 2, one);
    press(e.get(), "keyClip", 5);
    JobInfo j = load_job(job_dir(prm(e.get(), "jobId").s));
    CHECK(j.ok && j.count == 1, "one-frame clip");
    auto f = make_effect();
    source_gradient(f.get());
    double bad[2] = {20, 10};
    setDblN(H(&clip(f.get(), kOfxImageEffectSimpleSourceClipName).props), kOfxImageEffectPropFrameRange, 2, bad);
    std::string before = prm(f.get(), "jobId").s;
    press(f.get(), "keyClip", 20);
    CHECK(prm(f.get(), "jobId").s == before, "reversed range refused");
}

void test_set_reference_and_clear_cache() {
    auto e = make_effect();
    source_gradient(e.get());
    press(e.get(), "setReference", 16);
    CHECK(prm(e.get(), "reference").i == 6 && prm(e.get(), "hintMode").i == 1, "reference from playhead");
    std::string id = make_done_job(8, 4, 64, 36, 0, 0, 10, 10, [](int, int, int, float* p) { p[0] = p[1] = p[2] = p[3] = 1; });
    prm(e.get(), "jobId").s = id;
    press(e.get(), "clearCache");
    CHECK(!exists(job_dir(id)), "cache moved to recycle bin");
    CHECK(prm(e.get(), "jobId").s.empty(), "job id cleared");
}

void test_clip_preferences() {
    auto e = make_effect();
    const char* expect[] = {kOfxImagePreMultiplied, kOfxImageUnPreMultiplied, kOfxImageOpaque,
                            kOfxImageOpaque, kOfxImageOpaque, kOfxImagePreMultiplied};
    for (int v = 0; v < 6; ++v) {
        prm(e.get(), "view").i = v;
        Props out;
        act(kOfxImageEffectActionGetClipPreferences, e.get(), nullptr, &out);
        char* s = nullptr;
        getStr(H(&out), kOfxImageEffectPropPreMultiplication, 0, &s);
        CHECK(s && !std::strcmp(s, expect[v]), "premult for view " + std::to_string(v));
    }
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    gTemp = std::wstring(tmp) + L"ck2ofx_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(gTemp.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", gTemp.c_str());
    SetEnvironmentVariableW(L"CORRIDORKEY2_RUNTIME", (gTemp + L"\\no-runtime").c_str());
    gPropSuite = {setPtr, setStr, setDbl, setInt,
                  [](OfxPropertySetHandle, const char*, int, void* const*) { return unsupported(); },
                  [](OfxPropertySetHandle, const char*, int, const char* const*) { return unsupported(); },
                  setDblN, setIntN, getPtr, getStr, getDbl, getInt,
                  [](OfxPropertySetHandle, const char*, int, void**) { return unsupported(); },
                  [](OfxPropertySetHandle, const char*, int, char**) { return unsupported(); },
                  getDblN, getIntN,
                  [](OfxPropertySetHandle, const char*) { return unsupported(); },
                  [](OfxPropertySetHandle, const char*, int*) { return unsupported(); }};
    gParamSuite = {};
    gParamSuite.paramDefine = paramDefine;
    gParamSuite.paramGetHandle = paramGetHandle;
    gParamSuite.paramGetValue = paramGetValue;
    gParamSuite.paramGetValueAtTime = pgvat;
    gParamSuite.paramSetValue = paramSetValue;
    gEffectSuite = {};
    gEffectSuite.getPropertySet = getPropertySet;
    gEffectSuite.getParamSet = getParamSet;
    gEffectSuite.clipDefine = clipDefine;
    gEffectSuite.clipGetHandle = clipGetHandle;
    gEffectSuite.clipGetPropertySet = clipGetPropertySet;
    gEffectSuite.clipGetImage = clipGetImage;
    gEffectSuite.clipReleaseImage = clipReleaseImage;
    gEffectSuite.clipGetRegionOfDefinition = clipGetRegionOfDefinition;
    gMsgSuite = {};
    gMsgSuite.message = message;
    gHostStruct.host = H(&gHostProps);
    gHostStruct.fetchSuite = fetchSuite;
    plug()->setHost(&gHostStruct);
    act(kOfxActionLoad, nullptr);

    test_half_exhaustive();
    test_despill();
    test_describe_contexts();
    test_vram_limit_settings();
    test_collect_orientation_and_scale();
    test_nan_source_and_odd_size();
    test_render_views_orientation_scale();
    test_offset_origin();
    test_concurrent_renders();
    test_concurrent_collect_in_render();
    test_hint_input_not_connected();
    test_hint_input_connected();
    test_status_and_broken_cache();
    test_single_frame_and_empty_range();
    test_set_reference_and_clear_cache();
    test_clip_preferences();

    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
