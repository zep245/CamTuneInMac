// camtune - keeps a USB (UVC) webcam's picture settings where you want them, without any app open.
//
// macOS drives USB webcams with its own built-in driver and never changes the camera's brightness, exposure, gamma or
// backlight settings, so a camera that relies on its Windows driver to set them looks dark on macOS. camtune finds the
// webcam, reads the range of each control, and sets the values from its settings file (in percent of that range). It
// runs in the background (a LaunchAgent) and re-checks every 3 seconds, so the settings come back after the camera is
// unplugged, after sleep and after an app resets them. A control is only written when its value differs.
//
//   camtune             run in the background (what the LaunchAgent starts)
//   camtune --once      apply the settings once and exit
//   camtune --list      show every control: range, default, current value
//   camtune --set k=v   change one setting in the settings file and apply it (e.g. --set brightness=65)
//
// Settings file: ~/Library/Application Support/CamTune/camtune.conf (created with defaults on first run).

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#define VERSION "1.0.0"

// UVC 1.1 requests and selectors
enum { SET_CUR = 0x01, GET_CUR = 0x81, GET_MIN = 0x82, GET_MAX = 0x83, GET_RES = 0x84, GET_INFO = 0x86, GET_DEF = 0x87 };
enum { CT = 1, PU = 2 };

typedef struct {
    const char *key;      // settings-file name
    int unit;             // CT or PU
    int sel;              // control selector
    int len;              // bytes
    int is_signed;
    const char *def;      // default setting written to a new settings file
    const char *help;
} control_t;

static const control_t controls[] = {
    { "powerline",    PU, 0x05, 1, 0, "50hz",    "mains frequency: 50hz (India, Europe), 60hz, off, auto" },
    { "exposure",     CT, 0x02, 1, 0, "auto",    "auto, or manual:<percent> for a fixed exposure time" },
    { "autopriority", CT, 0x03, 1, 0, "1",       "1 = the camera may lower the frame rate in dim light (brighter picture)" },
    { "exposuretime", CT, 0x04, 4, 0, "default", "(used by exposure=manual:<percent>)" },
    { "backlight",    PU, 0x01, 2, 0, "100",     "backlight compensation: brightens faces in front of a bright background" },
    { "brightness",   PU, 0x02, 2, 1, "60",      "percent of the camera's range" },
    { "gamma",        PU, 0x09, 2, 0, "65",      "higher lifts dark areas (faces) more than bright ones" },
    { "gain",         PU, 0x04, 2, 0, "default", "higher = brighter but grainier" },
    { "contrast",     PU, 0x03, 2, 0, "default", "percent of the camera's range" },
    { "saturation",   PU, 0x07, 2, 0, "default", "percent of the camera's range" },
    { "sharpness",    PU, 0x08, 2, 0, "default", "percent of the camera's range" },
};
#define NCONTROLS (sizeof(controls) / sizeof(controls[0]))

typedef struct {
    IOUSBInterfaceInterface220 **intf;
    UInt8 ifnum;
    UInt8 ct_id, pu_id;   // camera terminal and processing unit IDs (0 = none)
    char name[128];
    UInt32 location;
} camera_t;

static int g_verbose = 0;
static char g_conf[PATH_MAX];

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s camtune: ", ts);
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------- UVC requests ---------------------------------------
static IOReturn uvc_req(camera_t *c, int req, int unit, int sel, void *buf, int len) {
    UInt8 uid = unit == CT ? c->ct_id : c->pu_id;
    if (!uid) return kIOReturnUnsupported;
    IOUSBDevRequest r;
    r.bmRequestType = (req & 0x80) ? USBmakebmRequestType(kUSBIn, kUSBClass, kUSBInterface)
                                   : USBmakebmRequestType(kUSBOut, kUSBClass, kUSBInterface);
    r.bRequest = (UInt8)req;
    r.wValue = (UInt16)(sel << 8);
    r.wIndex = (UInt16)((uid << 8) | c->ifnum);
    r.wLength = (UInt16)len;
    r.pData = buf;
    r.wLenDone = 0;
    return (*c->intf)->ControlRequest(c->intf, 0, &r);
}

static long decode(const UInt8 *b, int len, int is_signed) {
    if (len == 1) return is_signed ? (long)(SInt8)b[0] : (long)b[0];
    if (len == 2) { UInt16 v = (UInt16)(b[0] | (b[1] << 8)); return is_signed ? (long)(SInt16)v : (long)v; }
    UInt32 v = (UInt32)b[0] | ((UInt32)b[1] << 8) | ((UInt32)b[2] << 16) | ((UInt32)b[3] << 24);
    return is_signed ? (long)(SInt32)v : (long)v;
}
static void encode(UInt8 *b, int len, long v) { for (int i = 0; i < len; i++) b[i] = (UInt8)((unsigned long)v >> (8 * i)); }

static int get_val(camera_t *c, const control_t *k, int req, long *out) {
    UInt8 b[4] = {0};
    if (uvc_req(c, req, k->unit, k->sel, b, k->len) != kIOReturnSuccess) return -1;
    *out = decode(b, k->len, k->is_signed);
    return 0;
}
static int set_val(camera_t *c, const control_t *k, long v) {
    UInt8 b[4] = {0}; encode(b, k->len, v);
    IOReturn r = uvc_req(c, SET_CUR, k->unit, k->sel, b, k->len);
    if (r != kIOReturnSuccess && g_verbose) logmsg("%s: set %ld failed (0x%x)", k->key, v, r);
    return r == kIOReturnSuccess ? 0 : -1;
}
static int supported(camera_t *c, const control_t *k) {
    UInt8 info = 0;
    if (uvc_req(c, GET_INFO, k->unit, k->sel, &info, 1) != kIOReturnSuccess) return 0;
    return (info & 0x01) && (info & 0x02);   // supports GET and SET
}

// ---------------------------------------------------------------- finding the camera ---------------------------------
static void parse_vc_descriptors(camera_t *c) {
    IOUSBDescriptorHeader *d = NULL;
    while ((d = (*c->intf)->FindNextAssociatedDescriptor(c->intf, d, kUSBAnyDesc)) != NULL) {
        const UInt8 *p = (const UInt8 *)d;
        if (p[0] < 4 || p[1] != 0x24) continue;   // CS_INTERFACE
        if (p[2] == 0x02 && p[0] >= 8) {          // VC_INPUT_TERMINAL
            UInt16 type = (UInt16)(p[4] | (p[5] << 8));
            if (type == 0x0201 && !c->ct_id) c->ct_id = p[3];   // ITT_CAMERA
        } else if (p[2] == 0x05 && !c->pu_id) {   // VC_PROCESSING_UNIT
            c->pu_id = p[3];
        }
    }
}

// If the descriptors could not be read, find the units by asking: a processing unit answers for brightness (selector 2),
// a camera terminal for auto-exposure mode (selector 2). Unit IDs are small numbers.
static void probe_units(camera_t *c) {
    for (UInt8 id = 1; id < 16 && (!c->ct_id || !c->pu_id); id++) {
        UInt8 info = 0;
        if (!c->pu_id) { c->pu_id = id; if (uvc_req(c, GET_INFO, PU, 0x02, &info, 1) == kIOReturnSuccess && (info & 3) == 3) continue; c->pu_id = 0; }
        if (!c->ct_id && id != c->pu_id) { c->ct_id = id; if (uvc_req(c, GET_INFO, CT, 0x02, &info, 1) == kIOReturnSuccess && (info & 3) == 3) continue; c->ct_id = 0; }
    }
}

static void device_name(io_service_t intf_service, char *out, size_t n, UInt32 *loc) {
    snprintf(out, n, "USB camera"); *loc = 0;
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry(intf_service, kIOServicePlane, &parent) != KERN_SUCCESS) return;
    CFTypeRef name = IORegistryEntryCreateCFProperty(parent, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
    if (!name) name = IORegistryEntryCreateCFProperty(parent, CFSTR("kUSBProductString"), kCFAllocatorDefault, 0);
    if (name && CFGetTypeID(name) == CFStringGetTypeID()) CFStringGetCString(name, out, (CFIndex)n, kCFStringEncodingUTF8);
    if (name) CFRelease(name);
    CFTypeRef l = IORegistryEntryCreateCFProperty(parent, CFSTR("locationID"), kCFAllocatorDefault, 0);
    if (l && CFGetTypeID(l) == CFNumberGetTypeID()) CFNumberGetValue(l, kCFNumberSInt32Type, loc);
    if (l) CFRelease(l);
    IOObjectRelease(parent);
}

// Every USB Video Class "video control" interface (class 14, subclass 1).
static int find_cameras(camera_t *cams, int max) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostInterface");
    if (!match) return 0;
    SInt32 cls = 14, sub = 1;
    CFNumberRef ncls = CFNumberCreate(NULL, kCFNumberSInt32Type, &cls), nsub = CFNumberCreate(NULL, kCFNumberSInt32Type, &sub);
    CFDictionarySetValue(match, CFSTR("bInterfaceClass"), ncls);
    CFDictionarySetValue(match, CFSTR("bInterfaceSubClass"), nsub);
    CFRelease(ncls); CFRelease(nsub);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return 0;
    int n = 0;
    io_service_t s;
    while ((s = IOIteratorNext(it)) && n < max) {
        IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) == KERN_SUCCESS && plug) {
            IOUSBInterfaceInterface220 **intf = NULL;
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID220), (LPVOID *)&intf);
            IODestroyPlugInInterface(plug);
            if (intf) {
                camera_t *c = &cams[n];
                memset(c, 0, sizeof *c);
                c->intf = intf;
                (*intf)->GetInterfaceNumber(intf, &c->ifnum);
                device_name(s, c->name, sizeof c->name, &c->location);
                parse_vc_descriptors(c);
                if (!c->ct_id || !c->pu_id) probe_units(c);
                if (c->ct_id || c->pu_id) n++;
                else (*intf)->Release(intf);
            }
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return n;
}
static void release_cameras(camera_t *cams, int n) { for (int i = 0; i < n; i++) if (cams[i].intf) (*cams[i].intf)->Release(cams[i].intf); }

// ---------------------------------------------------------------- settings file -------------------------------------
static void conf_path(void) {
    const char *h = getenv("HOME"); if (!h) h = "/tmp";
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/CamTune", h);
    mkdir(dir, 0755);
    snprintf(g_conf, sizeof g_conf, "%s/camtune.conf", dir);
}
static void write_default_conf(void) {
    FILE *f = fopen(g_conf, "w"); if (!f) { logmsg("cannot write %s", g_conf); return; }
    fprintf(f, "# camtune settings. Numbers are percent of the camera's own range (0-100); \"default\" = the camera's default.\n"
               "# Changes are picked up within 3 seconds. See the ranges with: camtune --list\n");
    for (size_t i = 0; i < NCONTROLS; i++) {
        if (!strcmp(controls[i].key, "exposuretime")) continue;
        fprintf(f, "%s=%s\t\t# %s\n", controls[i].key, controls[i].def, controls[i].help);
    }
    fclose(f);
}
static int conf_get(const char *key, char *out, size_t n) {
    FILE *f = fopen(g_conf, "r"); if (!f) return -1;
    char line[256]; int found = -1; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || strncmp(p, key, kl) || p[kl] != '=') continue;
        p += kl + 1;
        char *e = p; while (*e && *e != '#' && *e != '\n' && *e != '\t' && *e != ' ') e++;
        *e = 0; snprintf(out, n, "%s", p); found = 0;
    }
    fclose(f);
    return found;
}
static int conf_set(const char *key, const char *val) {
    FILE *f = fopen(g_conf, "r"); if (!f) return -1;
    char *buf = NULL; size_t cap = 0, len = 0; char line[256]; int done = 0; size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        char out[300];
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!done && *p != '#' && !strncmp(p, key, kl) && p[kl] == '=') { snprintf(out, sizeof out, "%s=%s\n", key, val); done = 1; }
        else snprintf(out, sizeof out, "%s", line);
        size_t ol = strlen(out);
        if (len + ol + 1 > cap) { cap = (len + ol + 1) * 2; buf = realloc(buf, cap); if (!buf) { fclose(f); return -1; } }
        memcpy(buf + len, out, ol); len += ol; buf[len] = 0;
    }
    fclose(f);
    if (!done) { char out[300]; snprintf(out, sizeof out, "%s=%s\n", key, val); size_t ol = strlen(out);
        if (len + ol + 1 > cap) { cap = len + ol + 1; buf = realloc(buf, cap); if (!buf) return -1; }
        memcpy(buf + len, out, ol); len += ol; buf[len] = 0; }
    f = fopen(g_conf, "w"); if (!f) { free(buf); return -1; }
    fwrite(buf, 1, len, f); fclose(f); free(buf);
    return 0;
}

// percent of [min,max] snapped to the resolution
static long from_percent(camera_t *c, const control_t *k, double pct) {
    long mn, mx, res = 1;
    if (get_val(c, k, GET_MIN, &mn) || get_val(c, k, GET_MAX, &mx)) return LONG_MIN;
    if (get_val(c, k, GET_RES, &res) || res <= 0) res = 1;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    long v = mn + (long)((double)(mx - mn) * pct / 100.0 + 0.5);
    v = mn + ((v - mn) / res) * res;
    if (v > mx) v = mx;
    if (v < mn) v = mn;
    return v;
}

static const control_t *ctl(const char *key) { for (size_t i = 0; i < NCONTROLS; i++) if (!strcmp(controls[i].key, key)) return &controls[i]; return NULL; }

// Wanted value of one control from the settings file. Returns 0 and *want, or -1 (leave it alone).
static int wanted(camera_t *c, const control_t *k, long *want) {
    char v[64];
    if (!strcmp(k->key, "exposuretime")) {
        if (conf_get("exposure", v, sizeof v) || strncasecmp(v, "manual:", 7)) return -1;
        long w = from_percent(c, k, atof(v + 7)); if (w == LONG_MIN) return -1; *want = w; return 0;
    }
    if (conf_get(k->key, v, sizeof v) || !*v) return -1;
    if (!strcasecmp(v, "default")) return get_val(c, k, GET_DEF, want);
    if (!strcmp(k->key, "powerline")) {
        if (!strcasecmp(v, "off")) *want = 0; else if (!strcasecmp(v, "50hz") || !strcmp(v, "50")) *want = 1;
        else if (!strcasecmp(v, "60hz") || !strcmp(v, "60")) *want = 2; else if (!strcasecmp(v, "auto")) *want = 3; else return -1;
        return 0;
    }
    if (!strcmp(k->key, "exposure")) {
        if (!strcasecmp(v, "auto")) {   // aperture priority (8) is what "auto" means on almost every webcam; else full auto (2)
            long res = 0; *want = (get_val(c, k, GET_RES, &res) == 0 && (res & 8)) ? 8 : ((res & 2) ? 2 : 8); return 0; }
        if (!strncasecmp(v, "manual", 6)) { *want = 1; return 0; }
        return -1;
    }
    if (!strcmp(k->key, "autopriority")) { *want = atoi(v) ? 1 : 0; return 0; }
    long w = from_percent(c, k, atof(v)); if (w == LONG_MIN) return -1; *want = w; return 0;
}

// ---------------------------------------------------------------- apply / list --------------------------------------
static int apply(camera_t *c, int announce) {
    int changed = 0;
    for (size_t i = 0; i < NCONTROLS; i++) {
        const control_t *k = &controls[i];
        if (!supported(c, k)) continue;
        long want, cur;
        if (wanted(c, k, &want)) continue;
        if (get_val(c, k, GET_CUR, &cur) == 0 && cur == want) continue;
        if (set_val(c, k, want) == 0) { changed++; if (announce || g_verbose) logmsg("%s: %s %ld -> %ld", c->name, k->key, cur, want); }
    }
    return changed;
}

static void list(camera_t *c) {
    printf("%s (interface %u, camera terminal %u, processing unit %u)\n", c->name, c->ifnum, c->ct_id, c->pu_id);
    printf("  %-13s %8s %8s %8s %8s %8s   %s\n", "control", "min", "max", "default", "current", "wanted", "setting");
    for (size_t i = 0; i < NCONTROLS; i++) {
        const control_t *k = &controls[i];
        if (!supported(c, k)) { printf("  %-13s   (not supported by this camera)\n", k->key); continue; }
        long mn = 0, mx = 0, df = 0, cur = 0, want = 0; char s[64] = "-", ws[24] = "-";
        get_val(c, k, GET_MIN, &mn); get_val(c, k, GET_MAX, &mx); get_val(c, k, GET_DEF, &df); get_val(c, k, GET_CUR, &cur);
        if (!wanted(c, k, &want)) snprintf(ws, sizeof ws, "%ld", want);
        if (strcmp(k->key, "exposuretime")) conf_get(k->key, s, sizeof s);
        printf("  %-13s %8ld %8ld %8ld %8ld %8s   %s\n", k->key, mn, mx, df, cur, ws, s);
    }
}

static time_t conf_mtime(void) { struct stat st; return stat(g_conf, &st) == 0 ? st.st_mtime : 0; }

int main(int argc, char **argv) {
    int once = 0, do_list = 0; const char *setkv = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--list")) do_list = 1;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--set") && i + 1 < argc) setkv = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("camtune %s\n", VERSION); return 0; }
        else { fprintf(stderr, "usage: camtune [--once | --list | --set key=value] [--verbose]\n"); return 2; }
    }
    conf_path();
    if (access(g_conf, F_OK)) { write_default_conf(); logmsg("created %s with default settings", g_conf); }
    if (setkv) {
        char key[64], val[64]; const char *eq = strchr(setkv, '=');
        if (!eq || eq == setkv || (size_t)(eq - setkv) >= sizeof key) { fprintf(stderr, "use --set key=value\n"); return 2; }
        snprintf(key, sizeof key, "%.*s", (int)(eq - setkv), setkv); snprintf(val, sizeof val, "%s", eq + 1);
        if (!ctl(key) || !strcmp(key, "exposuretime")) { fprintf(stderr, "unknown setting %s\n", key); return 2; }
        if (conf_set(key, val)) { fprintf(stderr, "cannot write %s\n", g_conf); return 1; }
        printf("%s=%s saved\n", key, val);
        once = 1;
    }
    camera_t cams[4];
    if (do_list || once) {
        int n = find_cameras(cams, 4);
        if (!n) { printf("no USB camera found\n"); return 1; }
        for (int i = 0; i < n; i++) { if (do_list) list(&cams[i]); else printf("%s: %d setting(s) changed\n", cams[i].name, apply(&cams[i], 1)); }
        release_cameras(cams, n);
        return 0;
    }
    logmsg("camtune %s running, settings in %s", VERSION, g_conf);
    UInt32 last_loc = 0xFFFFFFFF; time_t last_conf = 0; int last_n = -1;
    for (;;) {
        int n = find_cameras(cams, 4);
        time_t mt = conf_mtime();
        int fresh = (n != last_n) || (n && cams[0].location != last_loc) || mt != last_conf;
        if (n != last_n) logmsg(n ? "camera found: %s" : "no camera", n ? cams[0].name : "");
        for (int i = 0; i < n; i++) apply(&cams[i], fresh);
        last_n = n; last_loc = n ? cams[0].location : 0; last_conf = mt;
        release_cameras(cams, n);
        sleep(3);
    }
}
