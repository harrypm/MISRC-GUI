#include "gui_cxadc.h"
#include "gui_cxadc_macos.h"

#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../output/gui_record.h"
#include "../signal/gui_headswitch_lock.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include "../../common/cxadc_rate_tiers.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#ifndef LIBASOUND_ENABLED
#define LIBASOUND_ENABLED 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Shield raylib symbols from conflicting Win32 API declarations
// (same pattern as gui_capture.c): raylib.h is already included via gui_app.h.
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <wbemcli.h>
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
// Define PKEY_Device_FriendlyName locally to avoid initguid conflicts.
static const PROPERTYKEY s_PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};
// PKEY_Device_InstanceId: returns device instance path like USB\VID_1209&PID_0001&MI_00\...
static const PROPERTYKEY s_PKEY_Device_InstanceId = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0xfc } }, 42
};
// MISRC Clockgen USB identifiers (Raspberry Pi Pico + Si5351 clockgen mod).
// Keep both PID values to support mixed firmware generations.
#define MISRC_CLOCKGEN_USB_VID_W L"vid_1209"
#define MISRC_CLOCKGEN_USB_PID_PRIMARY_W L"pid_0002"
#define MISRC_CLOCKGEN_USB_PID_ALT_W L"pid_0001"
#else
#include <unistd.h>
#include <fcntl.h>
#if LIBASOUND_ENABLED
#include <alsa/asoundlib.h>
#endif
#endif

#if defined(_WIN32)
// --- CXADC-Win configuration via WMI (direct ROOT\WMI access) ---
// The cxadc-win driver exposes its configuration as standard WMI data
// blocks (cxadc-win/src/driver/cxadc-win.mof):
//   CxadcWin_Path   : InstanceName <-> Win32 device path (\\.\cxadcN)
//   CxadcWin_Config : VideoMux / Level / EnableTenbit / EnableSixDB /
//                     CenterOffset (all [read, write])
// The upstream CxadcWin PowerShell module is only a thin wrapper over these
// classes (Get-/Set-CxadcWinConfig use Get-/Set-CimInstance). The GUI talks
// WMI directly so configuration works regardless of the installed
// PowerShell edition/version (the module requires pwsh 7.4+ Core, while
// shelling out launched Windows PowerShell 5.1) and without the module
// being importable by name. WMI reads need no elevation; writes
// (PutInstance) can be denied for non-admin users, mapped to EACCES below
// so callers take the same 8-bit fallback path as the Linux EPERM case.
// The Windows driver's center-offset range is 0..63
// (CX_CTRL_CONFIG_CENTER_OFFSET_MAX in cxadc-win/src/shared/cx_ctl_codes.h),
// unlike the Linux sysfs range of 0..255.
#define CXADC_WIN_CENTER_OFFSET_MAX 63

static const CLSID s_CLSID_WbemLocator = {
    0x4590f811, 0x1d3a, 0x11d0, { 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 }
};
static const IID s_IID_IWbemLocator = {
    // IID_IWbemLocator as defined in MinGW's wbemcli.h (DEFINE_GUID):
    // {DC12A687-737F-11CF-884D-00AA004B2E24} (same 00aa004b2e24 tail as
    // CLSID_WbemLocator above).
    0xdc12a687, 0x737f, 0x11cf, { 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24 }
};

typedef struct {
    IWbemServices *svc;
    bool com_initialized;  // this call owns the CoInitializeEx (-> CoUninitialize)
} cxadc_win_wmi_t;

static int cxadc_win_wmi_errno_from_hresult(HRESULT hr)
{
    // Casts: MinGW's wbemcli.h defines the WBEM_E_* codes as an enum
    // (tag_WBEMSTATUS) while HRESULT is a signed LONG.
    if (hr == (HRESULT)WBEM_E_ACCESS_DENIED) return EACCES;
    if (hr == (HRESULT)WBEM_E_NOT_FOUND ||
        hr == (HRESULT)WBEM_E_INVALID_CLASS ||
        hr == (HRESULT)WBEM_E_INVALID_OBJECT ||
        hr == (HRESULT)WBEM_E_NOT_AVAILABLE) return ENOENT;
    if (hr == (HRESULT)WBEM_E_INVALID_PARAMETER) return EINVAL;
    if (hr == E_OUTOFMEMORY) return ENOMEM;
    return EIO;
}

static void cxadc_win_wmi_log_failure(const char *what, HRESULT hr)
{
    fprintf(stderr, "[CXADC] WMI: %s failed (hr=0x%08lX)\n",
            what, (unsigned long)hr);
}

static void cxadc_win_wmi_close(cxadc_win_wmi_t *w)
{
    if (!w) return;
    if (w->svc) {
        w->svc->lpVtbl->Release(w->svc);
        w->svc = NULL;
    }
    if (w->com_initialized) {
        CoUninitialize();
        w->com_initialized = false;
    }
}

static int cxadc_win_wmi_open(cxadc_win_wmi_t *w)
{
    if (!w) return -1;
    memset(w, 0, sizeof(*w));

    // Per-thread COM init (same pattern as the WASAPI audio open path).
    // RPC_E_CHANGED_MODE: the thread already has COM up in another apartment
    // model (e.g. STA); COM is still usable for WMI, we just don't own the
    // init and must not CoUninitialize it.
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        w->com_initialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        cxadc_win_wmi_log_failure("CoInitializeEx", hr);
        errno = EIO;
        return -1;
    }

    IWbemLocator *loc = NULL;
    hr = CoCreateInstance(&s_CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &s_IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("CoCreateInstance(WbemLocator)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        cxadc_win_wmi_close(w);
        return -1;
    }

    hr = loc->lpVtbl->ConnectServer(loc, L"ROOT\\WMI", NULL, NULL, NULL,
                                    0L, NULL, NULL, &w->svc);
    loc->lpVtbl->Release(loc);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("ConnectServer(ROOT\\WMI)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        cxadc_win_wmi_close(w);
        return -1;
    }

    // Standard WMI client proxy blanket: local provider access needs
    // impersonation so the driver is touched on behalf of the caller.
    hr = CoSetProxyBlanket((IUnknown *)w->svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("CoSetProxyBlanket", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        cxadc_win_wmi_close(w);
        return -1;
    }
    return 0;
}

// Build the Win32 device path (\\.\cxadcN) as a wide string without CRT
// format ambiguity across MinGW/MSVC runtimes.
static int cxadc_win_card_path_w(int card_idx, wchar_t *out, size_t cap)
{
    if (!out || cap < 12) return -1;
    if (card_idx < 0 || card_idx >= 2) return -1;
    static const wchar_t base[] = L"\\\\.\\cxadc";
    size_t pos = 0;
    while (base[pos] != L'\0') {
        out[pos] = base[pos];
        pos++;
    }
    out[pos++] = L'0' + (wchar_t)card_idx;
    out[pos] = L'\0';
    return 0;
}

// Enumerate CxadcWin_Path and find the InstanceName for the given card path.
// All instances are compared in C (at most two cards) to sidestep WQL string
// escaping pitfalls with the backslashes in \\.\cxadcN.
static int cxadc_win_wmi_find_card_instance(cxadc_win_wmi_t *w, int card_idx,
                                             wchar_t *instance_out,
                                             size_t instance_cap)
{
    wchar_t card_path[16];
    if (cxadc_win_card_path_w(card_idx, card_path, sizeof(card_path) / sizeof(card_path[0])) != 0) {
        errno = EINVAL;
        return -1;
    }

    IEnumWbemClassObject *iter = NULL;
    HRESULT hr = w->svc->lpVtbl->ExecQuery(
        w->svc, L"WQL", L"SELECT * FROM CxadcWin_Path",
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &iter);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("ExecQuery(CxadcWin_Path)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }

    bool found = false;
    for (;;) {
        IWbemClassObject *obj = NULL;
        ULONG returned = 0;
        hr = iter->lpVtbl->Next(iter, WBEM_INFINITE, 1, &obj, &returned);
        if (hr != WBEM_S_NO_ERROR || returned == 0) {
            break;  // end of enumeration (WBEM_S_FALSE) or error
        }
        VARIANT vp;
        VariantInit(&vp);
        VARIANT vi;
        VariantInit(&vi);
        if (SUCCEEDED(obj->lpVtbl->Get(obj, L"Path", 0, &vp, NULL, NULL)) &&
            vp.vt == VT_BSTR && vp.bstrVal &&
            wcscmp(vp.bstrVal, card_path) == 0 &&
            SUCCEEDED(obj->lpVtbl->Get(obj, L"InstanceName", 0, &vi, NULL, NULL)) &&
            vi.vt == VT_BSTR && vi.bstrVal) {
            wcsncpy(instance_out, vi.bstrVal, instance_cap - 1);
            instance_out[instance_cap - 1] = L'\0';
            found = true;
        }
        VariantClear(&vp);
        VariantClear(&vi);
        obj->lpVtbl->Release(obj);
        if (found) break;
    }
    iter->lpVtbl->Release(iter);

    if (!found) {
        fprintf(stderr,
                "[CXADC] WMI: no CxadcWin_Path instance for card %d "
                "(driver not loaded or card not present)\n",
                card_idx);
        errno = ENOENT;
        return -1;
    }
    return 0;
}

// Build the WMI object path for a CxadcWin_Config instance, escaping the
// backslashes in the InstanceName (PCI\VEN_14F1&... -> PCI\\VEN_14F1&...).
static int cxadc_win_wmi_config_object_path(const wchar_t *instance,
                                             wchar_t *out, size_t out_cap)
{
    static const wchar_t prefix[] = L"CxadcWin_Config.InstanceName=\"";
    size_t pos = 0;
    for (size_t i = 0; prefix[i] != L'\0'; i++) {
        if (pos + 1 >= out_cap) return -1;
        out[pos++] = prefix[i];
    }
    for (size_t i = 0; instance[i] != L'\0'; i++) {
        size_t need = (instance[i] == L'\\') ? 2 : 1;
        if (pos + need + 1 >= out_cap) return -1;
        if (instance[i] == L'\\') out[pos++] = L'\\';
        out[pos++] = instance[i];
    }
    if (pos + 2 >= out_cap) return -1;
    out[pos++] = L'"';
    out[pos] = L'\0';
    return 0;
}

static int cxadc_win_wmi_get_config(cxadc_win_wmi_t *w, const wchar_t *obj_path,
                                     IWbemClassObject **obj_out)
{
    if (!obj_out) {
        errno = EINVAL;
        return -1;
    }
    *obj_out = NULL;
    BSTR bpath = SysAllocString(obj_path);
    if (!bpath) {
        errno = ENOMEM;
        return -1;
    }
    HRESULT hr = w->svc->lpVtbl->GetObject(w->svc, bpath, 0, NULL, obj_out, NULL);
    SysFreeString(bpath);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("GetObject(CxadcWin_Config)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    return 0;
}

static int cxadc_win_wmi_read_bool(IWbemClassObject *obj, const wchar_t *name,
                                    bool *out)
{
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = obj->lpVtbl->Get(obj, name, 0, &v, NULL, NULL);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("Get(property)", hr);
        VariantClear(&v);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    if (v.vt != VT_BOOL) {
        // VT_NULL: property absent (driver too old / MOF mismatch)
        VariantClear(&v);
        errno = EIO;
        return -1;
    }
    *out = (v.boolVal != VARIANT_FALSE);
    VariantClear(&v);
    return 0;
}

static int cxadc_win_wmi_read_long(IWbemClassObject *obj, const wchar_t *name,
                                    long *out)
{
    VARIANT v;
    VariantInit(&v);
    HRESULT hr = obj->lpVtbl->Get(obj, name, 0, &v, NULL, NULL);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("Get(property)", hr);
        VariantClear(&v);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    if (v.vt != VT_I4) {
        VariantClear(&v);
        errno = EIO;
        return -1;
    }
    *out = v.lVal;
    VariantClear(&v);
    return 0;
}

static int cxadc_win_wmi_put_bool(IWbemClassObject *obj, const wchar_t *name,
                                  bool value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = obj->lpVtbl->Put(obj, name, 0, &v, 0);
    VariantClear(&v);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("Put(property)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    return 0;
}

static int cxadc_win_wmi_put_long(IWbemClassObject *obj, const wchar_t *name,
                                  long value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = value;
    HRESULT hr = obj->lpVtbl->Put(obj, name, 0, &v, 0);
    VariantClear(&v);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure("Put(property)", hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    return 0;
}

static int cxadc_win_wmi_put_instance(cxadc_win_wmi_t *w, IWbemClassObject *obj,
                                       const char *what)
{
    HRESULT hr = w->svc->lpVtbl->PutInstance(w->svc, obj, 0, NULL, NULL);
    if (FAILED(hr)) {
        cxadc_win_wmi_log_failure(what, hr);
        errno = cxadc_win_wmi_errno_from_hresult(hr);
        return -1;
    }
    return 0;
}

static int cxadc_win_get_tenbit(int card_idx, bool *enabled_out)
{
    if (!enabled_out) return -1;
    cxadc_win_wmi_t w;
    if (cxadc_win_wmi_open(&w) != 0) return -1;

    int rc = -1;
    wchar_t instance[256];
    wchar_t obj_path[512];
    do {
        if (cxadc_win_wmi_find_card_instance(&w, card_idx, instance,
                                             sizeof(instance) / sizeof(instance[0])) != 0) break;
        if (cxadc_win_wmi_config_object_path(instance, obj_path,
                                             sizeof(obj_path) / sizeof(obj_path[0])) != 0) {
            errno = EIO;
            break;
        }
        IWbemClassObject *cfg = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &cfg) != 0) break;
        rc = cxadc_win_wmi_read_bool(cfg, L"EnableTenbit", enabled_out);
        cfg->lpVtbl->Release(cfg);
    } while (0);

    cxadc_win_wmi_close(&w);
    return rc;
}

static int cxadc_win_set_tenbit(int card_idx, bool enabled)
{
    cxadc_win_wmi_t w;
    if (cxadc_win_wmi_open(&w) != 0) return -1;

    int rc = -1;
    wchar_t instance[256];
    wchar_t obj_path[512];
    do {
        if (cxadc_win_wmi_find_card_instance(&w, card_idx, instance,
                                             sizeof(instance) / sizeof(instance[0])) != 0) break;
        if (cxadc_win_wmi_config_object_path(instance, obj_path,
                                             sizeof(obj_path) / sizeof(obj_path[0])) != 0) {
            errno = EIO;
            break;
        }
        IWbemClassObject *cfg = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &cfg) != 0) break;
        if (cxadc_win_wmi_put_bool(cfg, L"EnableTenbit", enabled) != 0) {
            cfg->lpVtbl->Release(cfg);
            break;
        }
        if (cxadc_win_wmi_put_instance(&w, cfg, "PutInstance(EnableTenbit)") != 0) {
            cfg->lpVtbl->Release(cfg);
            break;
        }
        cfg->lpVtbl->Release(cfg);

        // Readback via a fresh GetObject: confirm the driver really applied
        // the mode (mirrors the sysfs readback on Linux).
        IWbemClassObject *chk = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &chk) != 0) break;
        bool readback = false;
        rc = cxadc_win_wmi_read_bool(chk, L"EnableTenbit", &readback);
        chk->lpVtbl->Release(chk);
        if (rc != 0) break;
        if (readback != enabled) {
            fprintf(stderr,
                    "[CXADC] WMI: EnableTenbit readback mismatch on card %d "
                    "(expected %d got %d)\n",
                    card_idx, enabled ? 1 : 0, readback ? 1 : 0);
            errno = EIO;
            rc = -1;
        }
    } while (0);

    cxadc_win_wmi_close(&w);
    return rc;
}

static int cxadc_win_get_center_offset(int card_idx, int *value_out)
{
    if (!value_out) return -1;
    cxadc_win_wmi_t w;
    if (cxadc_win_wmi_open(&w) != 0) return -1;

    int rc = -1;
    wchar_t instance[256];
    wchar_t obj_path[512];
    do {
        if (cxadc_win_wmi_find_card_instance(&w, card_idx, instance,
                                             sizeof(instance) / sizeof(instance[0])) != 0) break;
        if (cxadc_win_wmi_config_object_path(instance, obj_path,
                                             sizeof(obj_path) / sizeof(obj_path[0])) != 0) {
            errno = EIO;
            break;
        }
        IWbemClassObject *cfg = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &cfg) != 0) break;
        long value = 0;
        rc = cxadc_win_wmi_read_long(cfg, L"CenterOffset", &value);
        if (rc == 0) {
            *value_out = (int)value;
        }
        cfg->lpVtbl->Release(cfg);
    } while (0);

    cxadc_win_wmi_close(&w);
    return rc;
}

static int cxadc_win_set_center_offset(int card_idx, int value)
{
    if (value < 0) value = 0;
    if (value > CXADC_WIN_CENTER_OFFSET_MAX) value = CXADC_WIN_CENTER_OFFSET_MAX;

    cxadc_win_wmi_t w;
    if (cxadc_win_wmi_open(&w) != 0) return -1;

    int rc = -1;
    wchar_t instance[256];
    wchar_t obj_path[512];
    do {
        if (cxadc_win_wmi_find_card_instance(&w, card_idx, instance,
                                             sizeof(instance) / sizeof(instance[0])) != 0) break;
        if (cxadc_win_wmi_config_object_path(instance, obj_path,
                                             sizeof(obj_path) / sizeof(obj_path[0])) != 0) {
            errno = EIO;
            break;
        }
        IWbemClassObject *cfg = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &cfg) != 0) break;
        if (cxadc_win_wmi_put_long(cfg, L"CenterOffset", (long)value) != 0) {
            cfg->lpVtbl->Release(cfg);
            break;
        }
        if (cxadc_win_wmi_put_instance(&w, cfg, "PutInstance(CenterOffset)") != 0) {
            cfg->lpVtbl->Release(cfg);
            break;
        }
        cfg->lpVtbl->Release(cfg);

        // Readback via a fresh GetObject to confirm the applied value.
        IWbemClassObject *chk = NULL;
        if (cxadc_win_wmi_get_config(&w, obj_path, &chk) != 0) break;
        long readback = 0;
        rc = cxadc_win_wmi_read_long(chk, L"CenterOffset", &readback);
        chk->lpVtbl->Release(chk);
        if (rc != 0) break;
        if ((int)readback != value) {
            fprintf(stderr,
                    "[CXADC] WMI: CenterOffset readback mismatch on card %d "
                    "(expected %d got %d)\n",
                    card_idx, value, (int)readback);
            errno = EIO;
            rc = -1;
        }
    } while (0);

    cxadc_win_wmi_close(&w);
    return rc;
}
#endif

extern volatile atomic_int do_exit;

// CXADC_MAX_CARDS lives in gui_cxadc.h
#define CXADC_SAMPLE_RATE_8BIT_HZ 40000000U
#define CXADC_SAMPLE_RATE_TENBIT_HZ 20000000U
// Stock CXADC card crystal is 28.636 MHz (8-bit 28.6 MSPS / 10-bit 14.3 MSPS).
// Used as the fallback when the real rate can't be detected (Windows, no
// sysfs). The 40/20 MSPS constants above are the CXADC Clockgen Mod baseline.
#define CXADC_STOCK_RATE_8BIT_HZ 28636363U
#define CXADC_STOCK_RATE_TENBIT_HZ 14318181U
#define CXADC_READ_CHUNK_BYTES 65536
#define CXADC_AUDIO_SAMPLE_RATE_HZ 46875U
#define CXADC_AUDIO_CHANNEL_COUNT 3
// 48 frames (~1 ms @ 46,875 Hz) keeps the BUF_CAPTURE_AUDIO ring advancing
// at ~1 ms granularity so the record-start alignment (the discard-to-now in
// gui_audio.c) is bounded by the USB audio delivery quantum instead of the
// old ~21.8 ms (1024-frame) period.
#define CXADC_AUDIO_READ_FRAMES 48
#define CXADC_AUDIO_PACKED_FRAME_BYTES 12

typedef enum {
    CXADC_AUDIO_FMT_NONE = 0,
    CXADC_AUDIO_FMT_S24_3LE,
    CXADC_AUDIO_FMT_S32_LE,
    CXADC_AUDIO_FMT_S16_LE,
    CXADC_AUDIO_FMT_FLOAT32
} cxadc_audio_format_t;

typedef struct {
    gui_app_t *app;
    atomic_bool running;
    thrd_t rf_thread;
    bool rf_thread_started;
    thrd_t audio_thread;
    bool audio_thread_started;
    int card_count;
    bool misrc_clockgen_mode;
    bool tenbit_mode[CXADC_MAX_CARDS];
    uint32_t card_sample_rate_hz[CXADC_MAX_CARDS];
    uint32_t rf_sample_rate_hz;
    // Audio format (shared across platforms)
    cxadc_audio_format_t audio_format;
    size_t audio_sample_bytes;
    uint32_t audio_sample_rate_hz;
    char audio_device_name[64];
    int audio_channels;
#if defined(_WIN32)
    HANDLE card_handles[CXADC_MAX_CARDS];
    // WASAPI clockgen audio capture
    IMMDevice *audio_endpoint;
    IAudioClient *audio_client;
    IAudioCaptureClient *audio_capture;
    bool audio_com_initialized;
#else
    int card_fds[CXADC_MAX_CARDS];
#if LIBASOUND_ENABLED
    snd_pcm_t *audio_pcm;
#endif
#endif
} cxadc_ctx_t;

static cxadc_ctx_t s_cxadc = {0};

#if !defined(_WIN32)
#define CXADC_SYSFS_CARD_MAX 255
#define CXADC_SYSFS_CENTER_OFFSET_MIN 0
#define CXADC_SYSFS_CENTER_OFFSET_MAX 255

static int cxadc_sysfs_build_param_path(int card_idx,
                                        const char *param_name,
                                        char *path_out,
                                        size_t path_out_len)
{
    if (!param_name || !param_name[0] || !path_out || path_out_len == 0) {
        return -1;
    }
    if (card_idx < 0 || card_idx > CXADC_SYSFS_CARD_MAX) {
        return -1;
    }
    int n = snprintf(path_out, path_out_len,
                     "/sys/class/cxadc/cxadc%d/device/parameters/%s",
                     card_idx, param_name);
    if (n <= 0 || (size_t)n >= path_out_len) {
        return -1;
    }
    return 0;
}

static int cxadc_sysfs_read_int_param(int card_idx,
                                      const char *param_name,
                                      int *value_out)
{
    if (!value_out) return -1;

    char path[160];
    if (cxadc_sysfs_build_param_path(card_idx, param_name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(buf, &endptr, 10);
    if (errno != 0 || endptr == buf) {
        return -1;
    }
    while (*endptr && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr != '\0') {
        return -1;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }

    *value_out = (int)parsed;
    return 0;
}

static int cxadc_sysfs_write_int_param(int card_idx,
                                       const char *param_name,
                                       int value)
{
    char path[160];
    if (cxadc_sysfs_build_param_path(card_idx, param_name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    int wrote = fprintf(f, "%d\n", value);
    int close_rc = fclose(f);
    if (wrote <= 0 || close_rc != 0) {
        return -1;
    }
    return 0;
}
#endif

int gui_cxadc_get_center_offset(int card_idx, int *value_out)
{
    if (!value_out) return -1;
#if defined(_WIN32)
    return cxadc_win_get_center_offset(card_idx, value_out);
#else
    #if defined(__APPLE__)
    (void)card_idx; return gui_cxadc_macos_get("center_offset", value_out);
#else
    return cxadc_sysfs_read_int_param(card_idx, "center_offset", value_out);
#endif
#endif
}

// Maximum CXADC center offset accepted by the platform driver:
// 0..255 on Linux/macOS, 0..63 on Windows (cxadc-win
// CX_CTRL_CONFIG_CENTER_OFFSET_MAX in cxadc-win/src/shared/cx_ctl_codes.h).
// Values above the driver max are rejected by the hardware path, so the GUI
// clamps to this range before writing.
int gui_cxadc_center_offset_max(void)
{
#if defined(_WIN32)
    return CXADC_WIN_CENTER_OFFSET_MAX;
#else
    return CXADC_SYSFS_CENTER_OFFSET_MAX;
#endif
}

int gui_cxadc_set_center_offset(int card_idx, int value)
{
    if (value < 0) value = 0;
    int offset_max = gui_cxadc_center_offset_max();
    if (value > offset_max) value = offset_max;
#if defined(_WIN32)
    return cxadc_win_set_center_offset(card_idx, value);
#elif defined(__APPLE__)
    (void)card_idx; return gui_cxadc_macos_set("center_offset", value);
#else
    return cxadc_sysfs_write_int_param(card_idx, "center_offset", value);
#endif
}

int gui_cxadc_adjust_center_offset(int card_idx, int delta, int *new_value_out)
{
    int current = 0;
    if (gui_cxadc_get_center_offset(card_idx, &current) != 0) {
        return -1;
    }
    int next = current + delta;
    if (next < 0) next = 0;
    int offset_max = gui_cxadc_center_offset_max();
    if (next > offset_max) next = offset_max;
    if (gui_cxadc_set_center_offset(card_idx, next) != 0) {
        return -1;
    }
    if (new_value_out) {
        *new_value_out = next;
    }
    return 0;
}

int gui_cxadc_get_tenbit(int card_idx, int *value_out)
{
    if (!value_out) return -1;
#if defined(_WIN32)
    bool enabled = false;
    if (cxadc_win_get_tenbit(card_idx, &enabled) != 0) return -1;
    *value_out = enabled ? 1 : 0;
    return 0;
#else
    #if defined(__APPLE__)
    (void)card_idx; *value_out=0; return 0;
    #endif
    int raw = 0;
    if (cxadc_sysfs_read_int_param(card_idx, "tenbit", &raw) != 0) {
        return -1;
    }
    *value_out = (raw != 0) ? 1 : 0;
    return 0;
#endif
}

int gui_cxadc_set_tenbit(int card_idx, bool enabled)
{
#if defined(_WIN32)
    return cxadc_win_set_tenbit(card_idx, enabled);
#else
    #if defined(__APPLE__)
    (void)card_idx; return enabled ? -1 : 0;
    #endif
    return cxadc_sysfs_write_int_param(card_idx, "tenbit", enabled ? 1 : 0);
#endif
}

// --- Hardware sample-rate detection ---
// cxadc has no sample_rate sysfs param; the rate is derived from crystal +
// tenxfsc + tenbit. We present rates per the cxadc card reality:
//   - 8-bit + tenxfsc=1 (crystal*10/8 = 35.8 on stock) is just upsampling the
//     crystal, so it is NOT presented: report the crystal rate (28.6) instead.
//   - 10-bit + tenxfsc=1 (crystal*5/8 = 17.9 on stock) IS exposed as a real
//     rate (the one useful tier above the 14.3 stock-10-bit floor).
//   - tenxfsc=2 forces 40 MSPS; tenxfsc=3 is broken in HW but the driver maps
//     it to 40 MSPS, so treat both as 40 (20 in 10-bit).
//   - A 54 MHz crystal mod with tenxfsc=0 yields 54 MSPS (27 in 10-bit).
// crystal + tenxfsc are tenbit-independent, so they're cached per card with a
// short TTL; the caller passes its intended tenbit so a just-toggled mode is
// reflected immediately.
#define CXADC_RATE_CACHE_TTL_S 1.0
#define CXADC_DEFAULT_CRYSTAL_HZ 28636363U
static uint32_t s_cxadc_crystal_cache_hz[CXADC_MAX_CARDS] = { 0, 0 };
static int s_cxadc_tenxfsc_cache[CXADC_MAX_CARDS] = { -1, -1 };
static bool s_cxadc_param_cache_valid[CXADC_MAX_CARDS] = { false, false };
static double s_cxadc_param_cache_time_s = 0.0;

bool gui_cxadc_get_sample_rate_hz(int card_idx, bool tenbit, uint32_t *rate_hz_out)
{
    if (!rate_hz_out) return false;
    *rate_hz_out = 0;
#if defined(__APPLE__)
    if (card_idx>=0 && card_idx<CXADC_MAX_CARDS && !tenbit) {*rate_hz_out=28636363;return true;}
    return false;
#endif
    if (card_idx < 0 || card_idx >= CXADC_MAX_CARDS) return false;
#if defined(_WIN32)
    // CxadcWin PowerShell config doesn't expose tenxfsc/crystal; callers use
    // the 40/20 MSPS clockgen baseline.
    return false;
#else
    double now = gui_runtime_time();
    if (!s_cxadc_param_cache_valid[card_idx] ||
        (now - s_cxadc_param_cache_time_s) >= CXADC_RATE_CACHE_TTL_S) {
        int tenxfsc = 0, crystal = 0;
        bool have_tenxfsc = (cxadc_sysfs_read_int_param(card_idx, "tenxfsc", &tenxfsc) == 0);
        bool have_crystal = (cxadc_sysfs_read_int_param(card_idx, "crystal", &crystal) == 0);
        if (!have_tenxfsc && !have_crystal) {
            return false;  // no cxadcN node / sysfs missing
        }
        if (!have_tenxfsc) tenxfsc = 0;
        if (!have_crystal || crystal <= 0) crystal = (int)CXADC_DEFAULT_CRYSTAL_HZ;
        s_cxadc_crystal_cache_hz[card_idx] = (uint32_t)crystal;
        s_cxadc_tenxfsc_cache[card_idx] = tenxfsc;
        s_cxadc_param_cache_valid[card_idx] = true;
        s_cxadc_param_cache_time_s = now;
    }

    uint32_t crystal_hz = s_cxadc_crystal_cache_hz[card_idx];
    int tenxfsc = s_cxadc_tenxfsc_cache[card_idx];
    uint32_t rate_hz;

    if (tenxfsc == 2 || tenxfsc == 3) {
        rate_hz = 40000000U;
        if (tenbit) rate_hz /= 2;
    } else if (tenxfsc == 1) {
        if (tenbit) {
            rate_hz = (crystal_hz * 5U) / 8U;  // 17.9 on stock — expose
        } else {
            rate_hz = crystal_hz;  // 8-bit: 35.8 is upsampled → present crystal
        }
    } else {
        rate_hz = crystal_hz;  // tenxfsc=0: crystal (28.6 stock / 40 mod / 54 mod)
        if (tenbit) rate_hz /= 2;
    }

    *rate_hz_out = rate_hz;
    return true;
#endif
}

static int cxadc_apply_tenbit_modes(gui_app_t *app, int card_count, const bool enabled[CXADC_MAX_CARDS])
{
    if (!app) return -1;
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;
    for (int i = 0; i < card_count; i++) {
        bool mode = (enabled != NULL) ? enabled[i] : false;

        // Read the card's current tenbit setting first. The cxadc driver
        // defaults to 8-bit (tenbit=0), so when the user wants 8-bit (the
        // common case) and the card is already in 8-bit, skip the sysfs write
        // entirely. This restores the pre-v1.1.6 behaviour where a plain
        // 8-bit capture start needed no sysfs write permission at all: the
        // /sys/class/cxadc/cxadcN/device/parameters/* files are root:root
        // 0644 by default and need a one-time `chgrp video` (+ group memship)
        // to be writable by non-root users. Writing tenbit=0 on every start
        // regressed that by requiring write access for an 8-bit capture.
        int current = -1;
        bool have_current = (gui_cxadc_get_tenbit(i, &current) == 0);
        if (have_current && (((current != 0) ? true : false) == mode)) {
            continue;  // already in the requested mode; no sysfs write needed
        }

        if (gui_cxadc_set_tenbit(i, mode) != 0) {
            int saved_errno = errno;
            fprintf(stderr, "[CXADC] set_tenbit(card %d, %s) failed: %s (errno %d)\n",
                    i, mode ? "10-bit" : "8-bit", strerror(saved_errno), saved_errno);
            // If the write was denied for permissions, fall back to 8-bit
            // (the driver default) instead of aborting the whole capture.
            // 8-bit needs no elevated write; degrading to 8-bit is better
            // than refusing to start. Revert this card's tenbit flag so the
            // live sample-rate readout matches reality. On Windows the
            // denial comes from the WMI config write (PutInstance denied
            // for non-admin users) - the popup text in gui_ui.c carries the
            // Windows fix instructions, the stderr line stays Linux-only
            // because the sysfs fix it names does not exist on Windows.
            if (saved_errno == EACCES || saved_errno == EPERM) {
#if defined(_WIN32)
                fprintf(stderr, "[CXADC] WMI config write denied; falling back to 8-bit (re-run as administrator for 10-bit)\n");
#else
                fprintf(stderr, "[CXADC] sysfs write denied; falling back to 8-bit (run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/* for 10-bit)\n");
#endif
                if (mode) {
                    s_cxadc.tenbit_mode[i] = false;
                    app->settings.cxadc_tenbit_mode_card[i] = false;
                    if (i == 0) app->settings.rf_bits_a = 8;
                    else app->settings.rf_bits_b = 8;
                    gui_settings_save(&app->settings);
#if defined(_WIN32)
                    gui_app_set_status(app, "CXADC 10-bit write denied - fell back to 8-bit (re-run as administrator)");
#else
                    gui_app_set_status(app, "CXADC 10-bit permission denied - fell back to 8-bit (run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*)");
#endif
                    // Ask the UI thread to show a help popup with the full
                    // one-time setup instructions (consumed in gui_handle_interactions).
                    atomic_store(&app->cxadc_perm_help_pending, true);
                }
                continue;
            }
            return -1;
        }
        int readback = -1;
        if (gui_cxadc_get_tenbit(i, &readback) != 0) {
            fprintf(stderr, "[CXADC] tenbit readback failed on card %d: %s (errno %d)\n",
                    i, strerror(errno), errno);
            return -1;
        }
        if (((readback != 0) ? true : false) != mode) {
            fprintf(stderr, "[CXADC] tenbit readback mismatch on card %d: expected %s got %s\n",
                    i, mode ? "10-bit" : "8-bit", readback ? "10-bit" : "8-bit");
            errno = EIO;
            return -1;
        }
    }
    return 0;
}
static inline uint32_t cxadc_encode_raw_sample(int16_t sample_a, int16_t sample_b)
{
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    return ch_a | (ch_b << 20);
}
#include "cxadc_samples.h"

static inline int16_t cxadc_decode_sample_tenbit(uint8_t lo, uint8_t hi)
{
    uint16_t raw = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return (int16_t)(((int)raw - 32768) >> 4);
}

static void cxadc_reset_stats(gui_app_t *app, uint32_t rf_sample_rate_hz)
{
    if (!app) return;
    if (rf_sample_rate_hz == 0) {
        rf_sample_rate_hz = CXADC_SAMPLE_RATE_8BIT_HZ;
    }
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, rf_sample_rate_hz);
    atomic_store(&app->audio_sample_rate, CXADC_AUDIO_SAMPLE_RATE_HZ);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;
}

#if defined(_WIN32)
static HANDLE cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\cxadc%d", card_idx);
    return CreateFileA(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

static int cxadc_read_card(HANDLE handle, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    DWORD read_bytes = 0;
    BOOL ok = ReadFile(handle, buf, (DWORD)len, &read_bytes, NULL);
    if (!ok) {
        return -1;
    }
    return (int)read_bytes;
}

static void cxadc_close_handle(HANDLE *handle)
{
    if (!handle) return;
    if (*handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}
#else
static int cxadc_open_card(int card_idx)
{
#if defined(__APPLE__)
    return gui_cxadc_macos_open(card_idx);
#endif
    char path[32];
    snprintf(path, sizeof(path), "/dev/cxadc%d", card_idx);
    return open(path, O_RDONLY);
}

static int cxadc_read_card(int fd, uint8_t *buf, size_t len)
{
#if defined(__APPLE__)
    return gui_cxadc_macos_read(fd,buf,len);
#endif
    if (!buf || len == 0 || fd < 0) {
        return -1;
    }

    ssize_t r = read(fd, buf, len);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    return (int)r;
}

static void cxadc_close_fd(int *fd)
{
    if (!fd) return;
    if (*fd >= 0) {
#if defined(__APPLE__)
        gui_cxadc_macos_close(*fd);
#else
        close(*fd);
#endif
        *fd = -1;
    }
}
#endif

static void cxadc_close_cards(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        cxadc_close_handle(&ctx->card_handles[i]);
#else
        cxadc_close_fd(&ctx->card_fds[i]);
#endif
    }
}

static int cxadc_open_cards(cxadc_ctx_t *ctx, int card_count)
{
    if (!ctx) return -1;
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    for (int i = 0; i < card_count; i++) {
#if defined(_WIN32)
        ctx->card_handles[i] = cxadc_open_card(i);
        if (ctx->card_handles[i] == INVALID_HANDLE_VALUE) {
            cxadc_close_cards(ctx);
            return -1;
        }
#else
        ctx->card_fds[i] = cxadc_open_card(i);
        if (ctx->card_fds[i] < 0) {
            cxadc_close_cards(ctx);
            return -1;
        }
#endif
    }
    return 0;
}

// --- Measured (empirical) card feed-rate check ---
// sysfs crystal/tenxfsc report what the driver believes. A hardware-modded
// card (e.g. a 40 MHz oscillator swap) can still carry the stock 28.636 MHz
// crystal parameter, which caps the rate readout and the resample options
// at 28.6 while the card actually feeds 40 MSPS. The measured rate times a
// real blocking-read window from the card, so it is the hardware truth.
// Cached per (card, tenbit mode) for the session (these statics deliberately
// live outside the memset-on-stop s_cxadc context, so one measurement
// corrects the whole session). Probed at startup enumeration (short window,
// fresh open) and at every capture start (uses the already-open card fd).
#define CXADC_RATE_PROBE_STARTUP_BYTES ((size_t)2 * 1024 * 1024)
#define CXADC_RATE_PROBE_START_BYTES  ((size_t)4 * 1024 * 1024)
#define CXADC_RATE_PROBE_CHUNK_BYTES  ((size_t)256 * 1024)
#define CXADC_RATE_PROBE_MIN_ELAPSED_US 20000
#define CXADC_RATE_PROBE_MAX_ELAPSED_US 2000000
// Drain phase bounds: the card free-runs since boot/driver load, so
// /dev/cxadcN holds a backlog that reads return far faster than the live
// feed; a timed window over the backlog reports a bogus rate (observed on a
// real 40 MSPS card: 50-72 MHz "measurements"). The probe first drains
// until a chunk takes as long as a live one (256 KiB at <= 64 MB/s takes
// >= 4 ms), then times the all-live window. Bounded by byte/time caps; if
// the live stream is not reached within the caps the measurement fails and
// the sysfs-derived rate stays (never a wrong override).
#define CXADC_RATE_PROBE_DRAIN_CHUNK_MIN_US 4000
#define CXADC_RATE_PROBE_DRAIN_MAX_BYTES ((size_t)1024 * 1024 * 1024)
#define CXADC_RATE_PROBE_DRAIN_MAX_US (3ULL * 1000000ULL)
static uint32_t s_cxadc_measured_rate_hz[CXADC_MAX_CARDS][2];
typedef enum {
    CXADC_RATE_STATE_UNKNOWN = 0,   // not measured this session (card, mode)
    CXADC_RATE_STATE_TIER,          // measured and snapped to a known tier
    CXADC_RATE_STATE_NO_TIER,       // measured but matched no tier / failed;
                                    // sysfs-derived rate stays (negative
                                    // cache so enumeration does not retry)
} cxadc_rate_state_t;
static cxadc_rate_state_t s_cxadc_measured_rate_state[CXADC_MAX_CARDS][2];
// Total bytes consumed from each card's fd during the CURRENT capture
// session (the capture-start probe + the synchronized-start reads). The
// driver's initial_page is always 512-page aligned, so release boundaries
// recur at exactly 2,097,152-byte multiples of this count - the exact-alignment
// grid reference for cxadc_sync_card_starts(). Reset per card at the
// capture-start probe; the startup-enumeration probe (a separate, closed fd
// session) accumulates into it first and is cleared by that reset.
static size_t s_cxadc_probe_consumed_bytes[CXADC_MAX_CARDS];

// Time a blocking read window on the card and derive the feed rate
// (samples/s = bytes read / elapsed / bytes-per-sample). Uses the capture
// context's already-open fd while a capture is running; otherwise opens
// the card fresh and closes it again. Returns false when no trustworthy
// measurement was possible (open failure, read error, or a window too
// short to be meaningful).
static bool cxadc_measure_card_rate_hz(int card_idx, bool tenbit, size_t probe_bytes,
                                        uint32_t *measured_hz_out)
{
#if defined(__APPLE__)
    (void)card_idx;(void)tenbit;(void)probe_bytes;
    if(measured_hz_out)*measured_hz_out=0;
    return false; /* configured nominal rate, never claim a calibrated measurement */
#endif
    if (card_idx < 0 || card_idx >= CXADC_MAX_CARDS || !measured_hz_out) {
        return false;
    }
    *measured_hz_out = 0;

    uint8_t *buf = (uint8_t *)malloc(CXADC_RATE_PROBE_CHUNK_BYTES);
    if (!buf) {
        return false;
    }

#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    bool close_after = false;
    if (atomic_load(&s_cxadc.running) &&
        s_cxadc.card_handles[card_idx] != INVALID_HANDLE_VALUE) {
        h = s_cxadc.card_handles[card_idx];
    } else {
        h = cxadc_open_card(card_idx);
        close_after = true;
    }
#else
    int fd = -1;
    bool close_after = false;
    if (atomic_load(&s_cxadc.running) && s_cxadc.card_fds[card_idx] >= 0) {
        fd = s_cxadc.card_fds[card_idx];
    } else {
        fd = cxadc_open_card(card_idx);
        close_after = true;
    }
#endif

    bool ok = false;

    // Phase 1: drain the driver's backlog until a chunk takes as long as a
    // live one, so the timed window below measures the live feed only.
    {
        uint64_t drain_start_us = get_time_us();
        size_t drained_bytes = 0;
        bool reached_live = false;
        while (drained_bytes < CXADC_RATE_PROBE_DRAIN_MAX_BYTES) {
            uint64_t chunk_t0_us = get_time_us();
            int got;
#if defined(_WIN32)
            got = (h != INVALID_HANDLE_VALUE)
                      ? cxadc_read_card(h, buf, CXADC_RATE_PROBE_CHUNK_BYTES) : -1;
#else
            got = (fd >= 0) ? cxadc_read_card(fd, buf, CXADC_RATE_PROBE_CHUNK_BYTES) : -1;
#endif
            uint64_t chunk_us = get_time_us() - chunk_t0_us;
            if (got < 0) {
                break;
            }
            if (got == 0) {
                thrd_sleep_ms(1);
            } else {
                drained_bytes += (size_t)got;
                if (chunk_us >= CXADC_RATE_PROBE_DRAIN_CHUNK_MIN_US) {
                    reached_live = true;
                    break;
                }
            }
            if ((get_time_us() - drain_start_us) >= CXADC_RATE_PROBE_DRAIN_MAX_US) {
                break;
            }
        }
        if (!reached_live) {
            fprintf(stderr,
                    "[CXADC] rate check: card %d could not reach the live stream while draining the backlog; using the sysfs-derived rate\n",
                    card_idx);
        }
        s_cxadc_probe_consumed_bytes[card_idx] += drained_bytes;
    }

    // Phase 2: timed all-live window.
    size_t total_bytes = 0;
    uint64_t start_us = get_time_us();
    uint64_t elapsed_us = 0;

    while (total_bytes < probe_bytes) {
        size_t want = probe_bytes - total_bytes;
        if (want > CXADC_RATE_PROBE_CHUNK_BYTES) {
            want = CXADC_RATE_PROBE_CHUNK_BYTES;
        }
        int got;
#if defined(_WIN32)
        got = (h != INVALID_HANDLE_VALUE) ? cxadc_read_card(h, buf, want) : -1;
#else
        got = (fd >= 0) ? cxadc_read_card(fd, buf, want) : -1;
#endif
        if (got < 0) {
            break;  // read error: no trustworthy measurement
        }
        if (got == 0) {
            thrd_sleep_ms(1);
        } else {
            total_bytes += (size_t)got;
        }
        elapsed_us = get_time_us() - start_us;
        if (elapsed_us >= CXADC_RATE_PROBE_MAX_ELAPSED_US) {
            break;
        }
    }

    elapsed_us = get_time_us() - start_us;
    if (total_bytes > 0 && elapsed_us >= CXADC_RATE_PROBE_MIN_ELAPSED_US) {
        uint64_t bytes_per_sample = tenbit ? 2u : 1u;
        uint64_t hz = ((uint64_t)total_bytes * 1000000ULL) /
                      (elapsed_us * bytes_per_sample);
        if (hz > 0 && hz <= 100000000ULL) {  // sanity: 0 < rate <= 100 MSPS
            *measured_hz_out = (uint32_t)hz;
            ok = true;
        }
    }

probe_done:
    s_cxadc_probe_consumed_bytes[card_idx] += total_bytes;
    free(buf);
#if defined(_WIN32)
    if (close_after && h != INVALID_HANDLE_VALUE) {
        cxadc_close_handle(&h);
    }
#else
    if (close_after && fd >= 0) {
        close(fd);
    }
#endif
    return ok;
}

// Probe one card in the given mode, snap to a known tier, cache it, and
// compare with the sysfs-derived rate. Logs agreement, disagreement (the
// stale-sysfs case) and measurement failures; sets a status line when a
// correction was applied and app is available.
static void cxadc_probe_and_cache_card_rate(gui_app_t *app, int card_idx, bool tenbit,
                                             size_t probe_bytes)
{
    int t = tenbit ? 1 : 0;
    uint32_t measured_hz = 0;
    if (!cxadc_measure_card_rate_hz(card_idx, tenbit, probe_bytes, &measured_hz)) {
        fprintf(stderr,
                "[CXADC] rate check: card %d measurement failed; using the sysfs-derived rate\n",
                card_idx);
        s_cxadc_measured_rate_state[card_idx][t] = CXADC_RATE_STATE_NO_TIER;
        return;
    }
    uint32_t snapped_hz = 0;
    if (!cxadc_snap_rate_tier_hz(measured_hz, &snapped_hz)) {
        fprintf(stderr,
                "[CXADC] rate check: card %d measured %.3f MHz matches no known rate tier; using the sysfs-derived rate\n",
                card_idx, (double)measured_hz / 1.0e6);
        s_cxadc_measured_rate_state[card_idx][t] = CXADC_RATE_STATE_NO_TIER;
        return;
    }
    s_cxadc_measured_rate_hz[card_idx][t] = snapped_hz;
    s_cxadc_measured_rate_state[card_idx][t] = CXADC_RATE_STATE_TIER;

    uint32_t sysfs_hz = 0;
    bool have_sysfs = gui_cxadc_get_sample_rate_hz(card_idx, tenbit, &sysfs_hz) &&
                      sysfs_hz > 0;
    if (have_sysfs) {
        uint64_t diff = (snapped_hz > sysfs_hz) ? ((uint64_t)snapped_hz - sysfs_hz)
                                                : ((uint64_t)sysfs_hz - snapped_hz);
        bool agree = (diff * 100ULL) <= ((uint64_t)sysfs_hz * 2ULL);  // within 2%
        if (agree) {
            fprintf(stderr,
                    "[CXADC] rate check: card %d measured %.3f MHz, sysfs %.3f MHz (agree)\n",
                    card_idx, (double)snapped_hz / 1.0e6, (double)sysfs_hz / 1.0e6);
            return;
        }
        fprintf(stderr,
                "[CXADC] rate check: card %d sysfs says %.3f MHz but the card feeds %.3f MHz (measured) - using the measured rate (stale sysfs crystal parameter on a modded card?)\n",
                card_idx, (double)sysfs_hz / 1.0e6, (double)snapped_hz / 1.0e6);
        if (app) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "CXADC card %d: sysfs %.1f but measured %.1f MSPS - using measured",
                     card_idx, (double)sysfs_hz / 1.0e6, (double)snapped_hz / 1.0e6);
            gui_app_set_status(app, msg);
        }
    } else {
        fprintf(stderr,
                "[CXADC] rate check: card %d measured %.3f MHz (sysfs unreadable)\n",
                card_idx, (double)snapped_hz / 1.0e6);
    }
}

bool gui_cxadc_get_effective_rate_hz(int card_idx, bool tenbit, uint32_t *rate_hz_out)
{
    if (!rate_hz_out) return false;
    if (card_idx >= 0 && card_idx < CXADC_MAX_CARDS) {
        int t = tenbit ? 1 : 0;
        if (s_cxadc_measured_rate_state[card_idx][t] == CXADC_RATE_STATE_TIER) {
            *rate_hz_out = s_cxadc_measured_rate_hz[card_idx][t];
            return true;
        }
    }
    return gui_cxadc_get_sample_rate_hz(card_idx, tenbit, rate_hz_out);
}

int gui_cxadc_probe_card_rates(int card_count, const bool tenbit_modes[CXADC_MAX_CARDS])
{
    if (card_count < 0) card_count = 0;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;
    if (card_count == 0) return 0;
    if (atomic_load(&s_cxadc.running)) {
        // A capture owns the cards (and its own capture-start probe covers
        // the rate check); do not read under it here.
        return 0;
    }
    const char *opt_out = getenv("MISRC_GUI_NO_CXADC_RATE_PROBE");
    if (opt_out && opt_out[0] == '1') {
        return 0;
    }
    int probed = 0;
    for (int i = 0; i < card_count; i++) {
        int t = tenbit_modes[i] ? 1 : 0;
        if (s_cxadc_measured_rate_state[i][t] != CXADC_RATE_STATE_UNKNOWN) {
            continue;  // already measured (or ruled out) this session
        }
        cxadc_probe_and_cache_card_rate(NULL, i, tenbit_modes[i],
                                         CXADC_RATE_PROBE_STARTUP_BYTES);
        if (s_cxadc_measured_rate_state[i][t] == CXADC_RATE_STATE_TIER) {
            probed++;
        }
    }
    return probed;
}

int gui_cxadc_detect_cards(void)
{
#if defined(__APPLE__)
    return gui_cxadc_macos_detect();
#endif
    int count = 0;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        HANDLE h = cxadc_open_card(i);
        if (h != INVALID_HANDLE_VALUE) {
            count++;
            CloseHandle(h);
        }
#else
        char path[32];
        snprintf(path, sizeof(path), "/dev/cxadc%d", i);
        if (access(path, R_OK) == 0 || access(path, W_OK) == 0) {
            count++;
        }
#endif
    }
    return count;
}

static inline void cxadc_store_s24le(uint8_t *dst, int32_t sample)
{
    if (sample > 8388607) sample = 8388607;
    if (sample < -8388608) sample = -8388608;
    uint32_t raw = (uint32_t)(sample & 0x00FFFFFF);
    dst[0] = (uint8_t)(raw & 0xFF);
    dst[1] = (uint8_t)((raw >> 8) & 0xFF);
    dst[2] = (uint8_t)((raw >> 16) & 0xFF);
}

#if !defined(_WIN32) && LIBASOUND_ENABLED
typedef struct {
    snd_pcm_format_t alsa_format;
    cxadc_audio_format_t cxadc_format;
    size_t sample_bytes;
} cxadc_audio_format_desc_t;

static bool cxadc_str_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool cxadc_alsa_card_name_matches_cxadc_clockgen(const char *name, const char *longname)
{
    return cxadc_str_contains_nocase(name, "cxadc") ||
           cxadc_str_contains_nocase(name, "clockgen") ||
           cxadc_str_contains_nocase(longname, "cxadc") ||
           cxadc_str_contains_nocase(longname, "clockgen");
}

static bool cxadc_alsa_card_name_matches_misrc_clockgen(const char *name, const char *longname)
{
    bool name_has_misrc = cxadc_str_contains_nocase(name, "misrc") ||
                          cxadc_str_contains_nocase(longname, "misrc");
    bool name_has_clockgen = cxadc_str_contains_nocase(name, "clockgen") ||
                             cxadc_str_contains_nocase(longname, "clockgen");
    bool name_has_pcm1802 = cxadc_str_contains_nocase(name, "pcm1802") ||
                            cxadc_str_contains_nocase(longname, "pcm1802");
    return name_has_misrc && (name_has_clockgen || name_has_pcm1802);
}

static bool cxadc_alsa_card_name_matches_target(const cxadc_ctx_t *ctx,
                                                 const char *name,
                                                 const char *longname)
{
    if (ctx && ctx->misrc_clockgen_mode) {
        if (cxadc_alsa_card_name_matches_misrc_clockgen(name, longname)) {
            return true;
        }
        // Fallback so MISRC mode still works with legacy/unbranded card names.
        return cxadc_alsa_card_name_matches_cxadc_clockgen(name, longname);
    }
    return cxadc_alsa_card_name_matches_cxadc_clockgen(name, longname);
}

static int cxadc_configure_audio_pcm(snd_pcm_t *pcm,
                                     snd_pcm_format_t format,
                                     unsigned int *out_rate_hz)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);

    int r;
    if ((r = snd_pcm_hw_params_any(pcm, params)) < 0) {
        fprintf(stderr, "[CXADC] ALSA hw_params_any failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_access(RW_INTERLEAVED) failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_format(pcm, params, format)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_format(%s) failed: %s\n",
                snd_pcm_format_name(format), snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_channels(pcm, params, CXADC_AUDIO_CHANNEL_COUNT)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_channels(%d) failed: %s "
                        "(clockgen may not expose %d channels via this device id)\n",
                CXADC_AUDIO_CHANNEL_COUNT, snd_strerror(r), CXADC_AUDIO_CHANNEL_COUNT);
        return -1;
    }

    unsigned int rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
    int dir = 0;
    if ((r = snd_pcm_hw_params_set_rate_near(pcm, params, &rate_hz, &dir)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_rate_near(%u) failed: %s\n",
                CXADC_AUDIO_SAMPLE_RATE_HZ, snd_strerror(r));
        return -1;
    }
    if (rate_hz < 46000 || rate_hz > 48000) {
        fprintf(stderr, "[CXADC] ALSA rate %u Hz out of range (need 46000-48000)\n", rate_hz);
        return -1;
    }

    snd_pcm_uframes_t period_frames = CXADC_AUDIO_READ_FRAMES;
    // With the small ~1 ms period, keep a proportionally larger ALSA buffer
    // (64 periods ≈ 65 ms) so scheduling hiccups do not overrun the stream.
    snd_pcm_uframes_t buffer_frames = CXADC_AUDIO_READ_FRAMES * 64;
    (void)snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, &dir);
    (void)snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);

    if ((r = snd_pcm_hw_params(pcm, params)) < 0) {
        fprintf(stderr, "[CXADC] ALSA hw_params apply failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "[CXADC] ALSA prepare failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_nonblock(pcm, 1)) < 0) {
        fprintf(stderr, "[CXADC] ALSA nonblock failed: %s\n", snd_strerror(r));
        return -1;
    }

    if (out_rate_hz) {
        *out_rate_hz = rate_hz;
    }
    return 0;
}

static int cxadc_try_open_audio_device(cxadc_ctx_t *ctx, const char *device_name)
{
    if (!ctx || !device_name || !device_name[0]) return -1;

    snd_pcm_t *pcm = NULL;
    int open_rc = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (open_rc < 0) {
        fprintf(stderr, "[CXADC] ALSA snd_pcm_open('%s', CAPTURE) failed: %s\n",
                device_name, snd_strerror(open_rc));
        return -1;
    }

    static const cxadc_audio_format_desc_t formats[] = {
        { SND_PCM_FORMAT_S24_3LE, CXADC_AUDIO_FMT_S24_3LE, 3 },
        { SND_PCM_FORMAT_S32_LE,  CXADC_AUDIO_FMT_S32_LE,  4 },
        { SND_PCM_FORMAT_S16_LE,  CXADC_AUDIO_FMT_S16_LE,  2 },
    };

    for (size_t i = 0; i < (sizeof(formats) / sizeof(formats[0])); i++) {
        unsigned int configured_rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
        if (cxadc_configure_audio_pcm(pcm, formats[i].alsa_format, &configured_rate_hz) == 0) {
            ctx->audio_pcm = pcm;
            ctx->audio_format = formats[i].cxadc_format;
            ctx->audio_sample_bytes = formats[i].sample_bytes;
            ctx->audio_sample_rate_hz = configured_rate_hz;
            snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name), "%s", device_name);
            return 0;
        }
    }

    snd_pcm_close(pcm);
    return -1;
}

static int cxadc_probe_alsa_cards_for_audio(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    int card = -1;
    if (snd_card_next(&card) < 0) return -1;

    bool matched_named_clockgen_card = false;
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);

        bool likely_clockgen = cxadc_alsa_card_name_matches_target(ctx, name, longname);
        if (likely_clockgen) {
            matched_named_clockgen_card = true;
            char usbstream_dev[32];
            snprintf(usbstream_dev, sizeof(usbstream_dev), "usbstream:CARD=%d", card);
            if (cxadc_try_open_audio_device(ctx, usbstream_dev) == 0) {
                if (name) free(name);
                if (longname) free(longname);
                return 0;
            }
            char hw_dev[32];
            char plughw_dev[32];
            snprintf(hw_dev, sizeof(hw_dev), "hw:%d", card);
            snprintf(plughw_dev, sizeof(plughw_dev), "plughw:%d", card);
            if (cxadc_try_open_audio_device(ctx, hw_dev) == 0 ||
                cxadc_try_open_audio_device(ctx, plughw_dev) == 0) {
                if (name) free(name);
                if (longname) free(longname);
                return 0;
            }
        }

        if (name) free(name);
        if (longname) free(longname);

        if (snd_card_next(&card) < 0) break;
    }

    // Fallback: if no obvious clockgen card name was found, probe all cards.
    if (matched_named_clockgen_card) return -1;

    card = -1;
    if (snd_card_next(&card) < 0) return -1;
    while (card >= 0) {
        char usbstream_dev[32];
        snprintf(usbstream_dev, sizeof(usbstream_dev), "usbstream:CARD=%d", card);
        if (cxadc_try_open_audio_device(ctx, usbstream_dev) == 0) {
            return 0;
        }
        char hw_dev[32];
        char plughw_dev[32];
        snprintf(hw_dev, sizeof(hw_dev), "hw:%d", card);
        snprintf(plughw_dev, sizeof(plughw_dev), "plughw:%d", card);
        if (cxadc_try_open_audio_device(ctx, hw_dev) == 0 ||
            cxadc_try_open_audio_device(ctx, plughw_dev) == 0) {
            return 0;
        }
        if (snd_card_next(&card) < 0) break;
    }

    return -1;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;
    // Internal support note (ClockGen Lite feed, Linux):
    // If the aux/headswitch feed is missing, verify mixer state with:
    //   alsamixer -D usbstream:CARD=CXADCADCClockGe
    // and make sure "USB Stream Output" is enabled/unmuted.

    const char *env_device = ctx->misrc_clockgen_mode
        ? getenv("MISRC_CLOCKGEN_ALSA_DEVICE")
        : getenv("MISRC_CXADC_ALSA_DEVICE");
    if ((!env_device || !env_device[0]) && ctx->misrc_clockgen_mode) {
        // Backward-compatible override name.
        env_device = getenv("MISRC_CXADC_ALSA_DEVICE");
    }

    if (env_device && env_device[0]) {
        if (cxadc_try_open_audio_device(ctx, env_device) == 0) {
            return 0;
        }
    }

    static const char *cxadc_candidates[] = {
        "usbstream:CARD=CXADCADCClockGe",
        "usbstream:CARD=CXADCADCClockGen",
        "usbstream:CARD=CXADCClockGen",
        "usbstream:CARD=CXADCClockGe",
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        "hw:CARD=CXADCClockGen",
        "hw:CARD=CXADCClockGe",
        "plughw:CARD=CXADCADCClockGe",
        "plughw:CARD=CXADCADCClockGen",
        "plughw:CARD=CXADCClockGen",
        "plughw:CARD=CXADCClockGe",
        NULL
    };
    static const char *misrc_candidates[] = {
        "usbstream:CARD=MISRCClockgen",
        "usbstream:CARD=MISRCClockGe",
        "usbstream:CARD=MISRCClock",
        "hw:CARD=MISRCClockgen",
        "hw:CARD=MISRCClockGe",
        "hw:CARD=MISRCClock",
        "plughw:CARD=MISRCClockgen",
        "plughw:CARD=MISRCClockGe",
        "plughw:CARD=MISRCClock",
        "usbstream:CARD=PCM1802",
        "hw:CARD=PCM1802",
        "plughw:CARD=PCM1802",
        "usbstream:CARD=CXADCADCClockGe",
        "usbstream:CARD=CXADCADCClockGen",
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        NULL
    };

    const char *const *candidates = ctx->misrc_clockgen_mode ? misrc_candidates : cxadc_candidates;
    for (size_t i = 0; candidates[i]; i++) {
        if (cxadc_try_open_audio_device(ctx, candidates[i]) == 0) {
            return 0;
        }
    }

    return cxadc_probe_alsa_cards_for_audio(ctx);
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
    snd_pcm_close(ctx->audio_pcm);
    ctx->audio_pcm = NULL;
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_device_name[0] = '\0';
}
#elif defined(_WIN32)
// --- WASAPI clockgen audio capture (cxadc-win) ---

static const GUID s_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
// Explicit COM interface/class GUIDs for MinGW builds where the extern IID/CLSID
// symbols are declared by headers but not provided by import libs.
static const GUID s_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID s_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID s_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID s_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

static bool cxadc_guid_equal(const GUID *a, const GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 && memcmp(a->Data4, b->Data4, 8) == 0;
}

static int cxadc_wasapi_parse_format(const WAVEFORMATEX *wfx, cxadc_ctx_t *ctx)
{
    if (!wfx || !ctx) return -1;

    WORD bits = wfx->wBitsPerSample;
    WORD valid_bits = bits;
    WORD format_tag = wfx->wFormatTag;
    bool is_float = false;

    if (format_tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE *wfxe = (WAVEFORMATEXTENSIBLE *)wfx;
        valid_bits = wfxe->Samples.wValidBitsPerSample;
        if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_PCM)) {
            format_tag = WAVE_FORMAT_PCM;
        } else if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            is_float = true;
        } else {
            return -1;
        }
    } else if (format_tag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (format_tag != WAVE_FORMAT_PCM) {
        return -1;
    }

    if (is_float) {
        if (bits != 32) return -1;
        ctx->audio_format = CXADC_AUDIO_FMT_FLOAT32;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 16 && bits <= 16) {
        ctx->audio_format = CXADC_AUDIO_FMT_S16_LE;
        ctx->audio_sample_bytes = 2;
    } else if (valid_bits <= 24 && bits <= 24) {
        ctx->audio_format = CXADC_AUDIO_FMT_S24_3LE;
        ctx->audio_sample_bytes = 3;
    } else if (valid_bits <= 24 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 32 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else {
        return -1;
    }

    ctx->audio_channels = (int)wfx->nChannels;
    if (ctx->audio_channels < 1) ctx->audio_channels = 1;
    ctx->audio_sample_rate_hz = (uint32_t)wfx->nSamplesPerSec;
    return 0;
}

static bool cxadc_wasapi_wstr_contains_nocase(const wchar_t *haystack, const wchar_t *needle)
{
    if (!haystack || !needle || !needle[0]) return false;

    wchar_t haystack_lower[512];
    wcsncpy(haystack_lower, haystack, 511);
    haystack_lower[511] = L'\0';
    _wcslwr(haystack_lower);

    wchar_t needle_lower[128];
    wcsncpy(needle_lower, needle, 127);
    needle_lower[127] = L'\0';
    _wcslwr(needle_lower);

    return wcsstr(haystack_lower, needle_lower) != NULL;
}

static bool cxadc_wasapi_instance_matches_misrc_clockgen(const wchar_t *instance_id)
{
    if (!instance_id) return false;

    wchar_t id_lower[512];
    wcsncpy(id_lower, instance_id, 511);
    id_lower[511] = L'\0';
    _wcslwr(id_lower);
    if (!wcsstr(id_lower, MISRC_CLOCKGEN_USB_VID_W)) {
        return false;
    }
    return (wcsstr(id_lower, MISRC_CLOCKGEN_USB_PID_PRIMARY_W) != NULL) ||
           (wcsstr(id_lower, MISRC_CLOCKGEN_USB_PID_ALT_W) != NULL);
}

static bool cxadc_wasapi_device_matches_env_override(const char *env_name,
                                                     const wchar_t *friendly_name,
                                                     const wchar_t *instance_id)
{
    const char *env_device = getenv(env_name);
    if (env_device && env_device[0]) {
        wchar_t env_w[256];
        int len = MultiByteToWideChar(CP_UTF8, 0, env_device, -1, env_w, 256);
        if (len > 0) {
            if (friendly_name && _wcsicmp(friendly_name, env_w) == 0) return true;
            if (friendly_name && wcsstr(friendly_name, env_w) != NULL) return true;
            if (instance_id && wcsstr(instance_id, env_w) != NULL) return true;
        }
    }
    return false;
}

// Match a WASAPI capture endpoint against the selected clockgen variant.
// Checks: explicit env override, USB instance VID/PID, and friendly-name fallbacks.
static bool cxadc_wasapi_device_matches(const cxadc_ctx_t *ctx,
                                        const wchar_t *friendly_name,
                                        const wchar_t *instance_id)
{
    if (ctx && ctx->misrc_clockgen_mode) {
        if (cxadc_wasapi_device_matches_env_override("MISRC_CLOCKGEN_WASAPI_DEVICE",
                                                     friendly_name,
                                                     instance_id)) {
            return true;
        }
        if (cxadc_wasapi_device_matches_env_override("MISRC_CXADC_WASAPI_DEVICE",
                                                     friendly_name,
                                                     instance_id)) {
            return true;
        }
        if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id)) {
            return true;
        }
        if (friendly_name &&
            (cxadc_wasapi_wstr_contains_nocase(friendly_name, L"misrc clockgen") ||
             cxadc_wasapi_wstr_contains_nocase(friendly_name, L"pcm1802"))) {
            return true;
        }
        return false;
    }

    if (cxadc_wasapi_device_matches_env_override("MISRC_CXADC_WASAPI_DEVICE",
                                                 friendly_name,
                                                 instance_id)) {
        return true;
    }
    // Keep explicit MISRC mode and regular CXADC mode distinct by default.
    if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id)) {
        return false;
    }
    if (friendly_name) {
        const wchar_t *patterns[] = { L"CXADC", L"ClockGen", L"Clockgen", L"clockgen", NULL };
        for (int i = 0; patterns[i]; i++) {
            if (wcsstr(friendly_name, patterns[i]) != NULL) return true;
        }
    }
    return false;
}

// Clockgen ADC supports 48000 or 46875 Hz (configurable via CxadcClockGen module).
static bool cxadc_wasapi_rate_supported(uint32_t rate_hz)
{
    return rate_hz == 48000U || rate_hz == 46875U;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return -1;
    }
    bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) {
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IMMDeviceCollection *pCollection = NULL;
    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    UINT count = 0;
    pCollection->lpVtbl->GetCount(pCollection, &count);

    IMMDevice *pDevice = NULL;
    bool found = false;
    for (UINT i = 0; i < count && !found; i++) {
        hr = pCollection->lpVtbl->Item(pCollection, i, &pDevice);
        if (FAILED(hr)) continue;

        IPropertyStore *pProps = NULL;
        hr = pDevice->lpVtbl->OpenPropertyStore(pDevice, STGM_READ, &pProps);
        if (FAILED(hr)) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
            continue;
        }

        wchar_t *friendly_name_w = NULL;
        PROPVARIANT pv_name;
        memset(&pv_name, 0, sizeof(pv_name));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_FriendlyName, &pv_name);
        if (SUCCEEDED(hr) && pv_name.vt == VT_LPWSTR && pv_name.pwszVal) {
            friendly_name_w = pv_name.pwszVal;
        }

        wchar_t *instance_id_w = NULL;
        PROPVARIANT pv_id;
        memset(&pv_id, 0, sizeof(pv_id));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_InstanceId, &pv_id);
        if (SUCCEEDED(hr) && pv_id.vt == VT_LPWSTR && pv_id.pwszVal) {
            instance_id_w = pv_id.pwszVal;
        }

        if (cxadc_wasapi_device_matches(ctx, friendly_name_w, instance_id_w)) {
            found = true;
            if (ctx->misrc_clockgen_mode) {
                snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name),
                         "%s", "WASAPI:MISRC Clockgen");
            } else {
                const wchar_t *label = friendly_name_w ? friendly_name_w : instance_id_w;
                snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name),
                         "WASAPI:%ls", label ? label : L"clockgen");
            }
        }

        if (pv_name.vt == VT_LPWSTR && pv_name.pwszVal) CoTaskMemFree(pv_name.pwszVal);
        if (pv_id.vt == VT_LPWSTR && pv_id.pwszVal) CoTaskMemFree(pv_id.pwszVal);
        pProps->lpVtbl->Release(pProps);
        if (!found) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
        }
    }
    pCollection->lpVtbl->Release(pCollection);

    if (!found || !pDevice) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioClient *pClient = NULL;
    hr = pDevice->lpVtbl->Activate(pDevice, &s_IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void **)&pClient);
    if (FAILED(hr)) {
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    WAVEFORMATEX *pwfx = NULL;
    hr = pClient->lpVtbl->GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    if (cxadc_wasapi_parse_format(pwfx, ctx) != 0) {
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    // Clockgen ADC supports 48000 or 46875 Hz. Warn (but continue) if the
    // device reports a different rate — the user may need to configure the
    // clockgen ADC rate via the CxadcClockGen PowerShell module first.
    if (!cxadc_wasapi_rate_supported(ctx->audio_sample_rate_hz)) {
        fprintf(stderr, "[CXADC] WASAPI: device rate %u Hz is not 48000/46875; "
                "configure clockgen ADC rate via CxadcClockGen module\n",
                ctx->audio_sample_rate_hz);
    }

    REFERENCE_TIME buffer_duration = 500000; // 50ms in 100ns units
    hr = pClient->lpVtbl->Initialize(pClient, AUDCLNT_SHAREMODE_SHARED, 0,
                                     buffer_duration, 0, pwfx, NULL);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioCaptureClient *pCapture = NULL;
    hr = pClient->lpVtbl->GetService(pClient, &s_IID_IAudioCaptureClient,
                                     (void **)&pCapture);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    hr = pClient->lpVtbl->Start(pClient);
    if (FAILED(hr)) {
        pCapture->lpVtbl->Release(pCapture);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    ctx->audio_endpoint = pDevice;
    ctx->audio_client = pClient;
    ctx->audio_capture = pCapture;
    ctx->audio_com_initialized = com_initialized;
    pEnum->lpVtbl->Release(pEnum);
    return 0;
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_client) return;
    ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
    }
    if (ctx->audio_capture) {
        ctx->audio_capture->lpVtbl->Release(ctx->audio_capture);
        ctx->audio_capture = NULL;
    }
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Release(ctx->audio_client);
        ctx->audio_client = NULL;
    }
    if (ctx->audio_endpoint) {
        ctx->audio_endpoint->lpVtbl->Release(ctx->audio_endpoint);
        ctx->audio_endpoint = NULL;
    }
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_channels = 0;
    ctx->audio_device_name[0] = '\0';
    if (ctx->audio_com_initialized) {
        CoUninitialize();
        ctx->audio_com_initialized = false;
    }
}
#else
static int cxadc_open_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; return -1; }
static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
static void cxadc_close_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
#endif

static int cxadc_audio_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;
    gui_app_t *app = ctx->app;

#if defined(_WIN32)
    if (!ctx->audio_capture || !ctx->audio_client ||
        ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);
    int dev_channels = ctx->audio_channels;
    if (dev_channels < 1) dev_channels = 1;
    size_t input_frame_bytes = (size_t)dev_channels * ctx->audio_sample_bytes;

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        UINT32 packet_frames = 0;
        HRESULT hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
        if (FAILED(hr)) {
            gui_app_set_status(app, "CXADC WASAPI audio read error");
            break;
        }
        if (packet_frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        while (packet_frames > 0) {
            BYTE *data = NULL;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = ctx->audio_capture->lpVtbl->GetBuffer(ctx->audio_capture, &data,
                                                       &frames_available, &flags,
                                                       NULL, NULL);
            if (FAILED(hr)) {
                gui_app_set_status(app, "CXADC WASAPI GetBuffer error");
                goto cxadc_wasapi_done;
            }

            if (data && frames_available > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                size_t output_bytes = (size_t)frames_available * CXADC_AUDIO_PACKED_FRAME_BYTES;
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO,
                                                             output_bytes, NULL);
                if (out) {
                    for (UINT32 i = 0; i < frames_available; i++) {
                        const uint8_t *src_frame = data + ((size_t)i * input_frame_bytes);
                        uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
                        for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                            uint8_t *dst = dst_frame + ((size_t)ch * 3);
                            if (ch < dev_channels) {
                                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                                    int32_t s32 = (int32_t)((uint32_t)src[0] |
                                                            ((uint32_t)src[1] << 8) |
                                                            ((uint32_t)src[2] << 16) |
                                                            ((uint32_t)src[3] << 24));
                                    cxadc_store_s24le(dst, s32 >> 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                                    int16_t s16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                                    cxadc_store_s24le(dst, ((int32_t)s16) << 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_FLOAT32) {
                                    float f;
                                    memcpy(&f, src, 4);
                                    int32_t s32 = (int32_t)(f * 8388607.0f);
                                    cxadc_store_s24le(dst, s32);
                                } else {
                                    dst[0] = dst[1] = dst[2] = 0;
                                }
                            } else {
                                dst[0] = dst[1] = dst[2] = 0;
                            }
                        }
                        // CH4: the clockgen device is 3-channel (CH1/CH2 + headswitch
                        // CH3). There is no CH4 input — always leave it zero. Do
                        // NOT mirror CH3 into CH4 (the clockgen has no CH3/4
                        // pair control path on any platform).
                        dst_frame[9] = 0;
                        dst_frame[10] = 0;
                        dst_frame[11] = 0;
                    }
                    gui_headswitch_lock_ingest_s24le_interleaved(out, output_bytes, ctx->audio_sample_rate_hz);
                    bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
                    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
                    atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
                    atomic_store(&app->last_callback_time_ms, get_time_ms());
                } else {
                    atomic_fetch_add(&app->rb_drop_count, 1);
                }
            }

            ctx->audio_capture->lpVtbl->ReleaseBuffer(ctx->audio_capture, frames_available);
            hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
            if (FAILED(hr)) {
                packet_frames = 0;
                break;
            }
        }
    }
cxadc_wasapi_done:
    return 0;
#elif !LIBASOUND_ENABLED
    (void)app;
    return 0;
#else
    if (!ctx->audio_pcm || ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    size_t input_frame_bytes = (size_t)CXADC_AUDIO_CHANNEL_COUNT * ctx->audio_sample_bytes;
    size_t input_buffer_bytes = (size_t)CXADC_AUDIO_READ_FRAMES * input_frame_bytes;
    uint8_t *input_buf = (uint8_t *)malloc(input_buffer_bytes);
    if (!input_buf) {
        gui_app_set_status(app, "CXADC: failed to allocate ALSA audio buffer");
        return -1;
    }

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        snd_pcm_sframes_t frames = snd_pcm_readi(ctx->audio_pcm, input_buf, CXADC_AUDIO_READ_FRAMES);
        if (frames == -EAGAIN || frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (frames == -EPIPE) {
            fprintf(stderr, "[CXADC] ALSA underrun (EPIPE) on %s, preparing\n",
                    ctx->audio_device_name);
            (void)snd_pcm_prepare(ctx->audio_pcm);
            continue;
        }
        if (frames < 0) {
            fprintf(stderr, "[CXADC] ALSA readi error on %s: %s (errno %d)\n",
                    ctx->audio_device_name, snd_strerror((int)frames), (int)frames);
            int rec = snd_pcm_recover(ctx->audio_pcm, (int)frames, 1);
            if (rec < 0) {
                fprintf(stderr, "[CXADC] ALSA recover failed: %s\n", snd_strerror(rec));
            }
            if (rec >= 0) {
                continue;
            }
            fprintf(stderr, "[CXADC] ALSA audio thread exiting after unrecoverable read error\n");
            gui_app_set_status(app, "CXADC ALSA audio read error");
            break;
        }

        size_t output_bytes = (size_t)frames * CXADC_AUDIO_PACKED_FRAME_BYTES;
        uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            const uint8_t *src_frame = input_buf + ((size_t)i * input_frame_bytes);
            uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
            for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                uint8_t *dst = dst_frame + ((size_t)ch * 3);
                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                    int32_t sample32 = (int32_t)((uint32_t)src[0] |
                                                 ((uint32_t)src[1] << 8) |
                                                 ((uint32_t)src[2] << 16) |
                                                 ((uint32_t)src[3] << 24));
                    cxadc_store_s24le(dst, sample32 >> 8);
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                    int16_t sample16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                    cxadc_store_s24le(dst, ((int32_t)sample16) << 8);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            dst_frame[9] = 0;
            dst_frame[10] = 0;
            dst_frame[11] = 0;
        }
        gui_headswitch_lock_ingest_s24le_interleaved(out, output_bytes, ctx->audio_sample_rate_hz);

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
    }

    free(input_buf);
    return 0;
#endif
}

static int cxadc_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;

    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *card_buf_a = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    uint8_t *card_buf_b = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    if (!card_buf_a || !card_buf_b) {
        free(card_buf_a);
        free(card_buf_b);
        gui_app_set_status(app, "CXADC: failed to allocate capture buffers");
        return -1;
    }
    const int input_bytes_per_sample_a = ctx->tenbit_mode[0] ? 2 : 1;
    const int input_bytes_per_sample_b = ctx->tenbit_mode[1] ? 2 : 1;
    uint32_t tap_frame_index = 0;
#if defined(__APPLE__)
    uint64_t native_last_data=get_time_ms();
#endif

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, ctx->rf_sample_rate_hz);

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
#if defined(_WIN32)
        int read_a = cxadc_read_card(ctx->card_handles[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#else
        int read_a = cxadc_read_card(ctx->card_fds[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#endif
        if (read_a < 0) {
#if defined(__APPLE__)
            gui_app_set_status(app,gui_cxadc_macos_error());
            gui_cxadc_macos_mark_failed();
            atomic_store(&app->dropout_stop_reason,GUI_DROPOUT_DEVICE_ERROR);
            atomic_store(&app->dropout_stop_requested,true);
#else
            gui_app_set_status(app, "CXADC read error on card 0");
#endif
            break;
        }
        if (read_a == 0) {
#if defined(__APPLE__)
            if(get_time_ms()-native_last_data>2000){
                gui_app_set_status(app,"macOS CXADC stopped delivering samples (device/driver unavailable)");
                gui_cxadc_macos_mark_failed();
                atomic_store(&app->dropout_stop_reason,GUI_DROPOUT_CALLBACK_GAP);
                atomic_store(&app->dropout_stop_requested,true);break;
            }
#endif
            thrd_sleep_ms(1);
            continue;
        }

#if defined(__APPLE__)
        native_last_data=get_time_ms();
#endif
        int output_samples = read_a / input_bytes_per_sample_a;
        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }
        int read_b = 0;
        if (ctx->card_count > 1) {
#if defined(_WIN32)
            read_b = cxadc_read_card(ctx->card_handles[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#else
            read_b = cxadc_read_card(ctx->card_fds[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#endif
            if (read_b < 0) {
                gui_app_set_status(app, "CXADC read error on card 1");
                break;
            }
            if (read_b == 0) {
                thrd_sleep_ms(1);
                continue;
            }
            int output_samples_b = read_b / input_bytes_per_sample_b;
            if (output_samples_b < output_samples) {
                output_samples = output_samples_b;
            }
        }

        // Direct native RAW taps (cards 0/1 -> channels A/B): the exact bytes
        // just read, before the A/B pairing truncation above (which truncates
        // both reads to the shorter one and drops card A's data when card B
        // returns 0). Gated per iteration: both channels' taps are entered
        // (marked in-flight, so the record-stop handshake's wait-idle covers
        // the whole iteration) and the record decision is latched ONCE, then
        // both chunks are pushed with the same decision — a record start/stop
        // handshake landing between the per-channel pushes can no longer
        // split the channels by one chunk (observed a 64 KiB A/B count
        // difference before this gate). No pushes unless the channels were
        // set up as direct native at record start.
        {
            const bool tap_a_ch = gui_record_direct_tap_channel_active(0);
            const bool tap_b_ch = (ctx->card_count > 1) &&
                                  gui_record_direct_tap_channel_active(1);
            if (tap_a_ch || tap_b_ch) {
                bool a_on = tap_a_ch ? gui_record_tap_iteration_enter(0) : true;
                bool b_on = tap_b_ch ? gui_record_tap_iteration_enter(1) : true;
                bool tap_this_iter = a_on && b_on;
                if (tap_a_ch && tap_this_iter) {
                    gui_record_direct_tap_push_latched(0, true, card_buf_a,
                                                       (size_t)read_a, tap_frame_index);
                }
                if (tap_b_ch && tap_this_iter) {
                    gui_record_direct_tap_push_latched(1, true, card_buf_b,
                                                       (size_t)read_b, tap_frame_index + 1);
                }
                if (tap_a_ch) gui_record_tap_iteration_leave(0);
                if (tap_b_ch) gui_record_tap_iteration_leave(1);
                tap_frame_index += 2;
            }
        }

        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t output_bytes = (size_t)output_samples * sizeof(uint32_t);
        uint32_t *raw_out = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, output_bytes, NULL);
        if (!raw_out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
#if defined(__APPLE__)
            gui_cxadc_macos_mark_failed();
            atomic_store(&app->dropout_stop_reason,GUI_DROPOUT_BACKPRESSURE);
            atomic_store(&app->dropout_stop_requested,true);break;
#endif
            continue;
        }

        for (int i = 0; i < output_samples; i++) {
            int16_t sample_a = 0;
            if (ctx->tenbit_mode[0]) {
                int idx = i * 2;
                sample_a = cxadc_decode_sample_tenbit(card_buf_a[idx], card_buf_a[idx + 1]);
            } else {
                sample_a = cxadc_decode_sample_8bit(card_buf_a[i]);
            }
            int16_t sample_b = 0;
            if (ctx->card_count > 1) {
                if (ctx->tenbit_mode[1]) {
                    int idx = i * 2;
                    sample_b = cxadc_decode_sample_tenbit(card_buf_b[idx], card_buf_b[idx + 1]);
                } else {
                    sample_b = cxadc_decode_sample_8bit(card_buf_b[i]);
                }
            }
            raw_out[i] = cxadc_encode_raw_sample(sample_a, sample_b);
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        atomic_store(&app->last_callback_time_ms, get_time_ms());
        atomic_fetch_add(&app->frame_count, 1);
    }

    free(card_buf_a);
    free(card_buf_b);
    return 0;
}

bool gui_cxadc_detect_misrc_clockgen_audio(void)
{
#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) { if (com_initialized) CoUninitialize(); return false; }

    IMMDeviceCollection *pCollection = NULL;
    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return false;
    }

    UINT count = 0;
    pCollection->lpVtbl->GetCount(pCollection, &count);
    bool found = false;
    for (UINT i = 0; i < count && !found; i++) {
        IMMDevice *pDevice = NULL;
        hr = pCollection->lpVtbl->Item(pCollection, i, &pDevice);
        if (FAILED(hr)) continue;
        IPropertyStore *pProps = NULL;
        hr = pDevice->lpVtbl->OpenPropertyStore(pDevice, STGM_READ, &pProps);
        if (SUCCEEDED(hr)) {
            wchar_t *friendly_name_w = NULL;
            PROPVARIANT pv_name;
            memset(&pv_name, 0, sizeof(pv_name));
            hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_FriendlyName, &pv_name);
            if (SUCCEEDED(hr) && pv_name.vt == VT_LPWSTR && pv_name.pwszVal) {
                friendly_name_w = pv_name.pwszVal;
            }
            wchar_t *instance_id_w = NULL;
            PROPVARIANT pv_id;
            memset(&pv_id, 0, sizeof(pv_id));
            hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_InstanceId, &pv_id);
            if (SUCCEEDED(hr) && pv_id.vt == VT_LPWSTR && pv_id.pwszVal) {
                instance_id_w = pv_id.pwszVal;
            }
            // Reuse the MISRC matchers (USB instance VID/PID + friendly name).
            if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id_w) ||
                (friendly_name_w &&
                 (cxadc_wasapi_wstr_contains_nocase(friendly_name_w, L"misrc clockgen") ||
                  cxadc_wasapi_wstr_contains_nocase(friendly_name_w, L"pcm1802")))) {
                found = true;
            }
            if (pv_name.vt == VT_LPWSTR && pv_name.pwszVal) CoTaskMemFree(pv_name.pwszVal);
            if (pv_id.vt == VT_LPWSTR && pv_id.pwszVal) CoTaskMemFree(pv_id.pwszVal);
            pProps->lpVtbl->Release(pProps);
        }
        pDevice->lpVtbl->Release(pDevice);
    }
    pCollection->lpVtbl->Release(pCollection);
    pEnum->lpVtbl->Release(pEnum);
    if (com_initialized) CoUninitialize();
    return found;
#elif !defined(_WIN32) && LIBASOUND_ENABLED
    int card = -1;
    if (snd_card_next(&card) < 0) return false;
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);
        bool match = cxadc_alsa_card_name_matches_misrc_clockgen(name, longname);
        if (name) free(name);
        if (longname) free(longname);
        if (match) return true;
        if (snd_card_next(&card) < 0) break;
    }
    return false;
#else
    return false;
#endif
}

/* --- F1b: synchronized card starts (exact boundary grid) ---------------
 *
 * The per-card capture-start rate probes (drain + timed 4 MiB live window,
 * ~105-155 ms each) run sequentially, so card 0's stream position is left
 * ~one probe duration behind card 1's in wall-clock content time. The RF
 * capture thread then reads both cards in lockstep, which preserves that
 * content offset for the whole capture (measured 114 ms on 2026-10-06).
 *
 * This routine re-aligns the streams to a single common content instant
 * BEFORE the RF thread starts, using the driver's exact grid: open() latches
 * initial_page = a 512-page-aligned boundary index, so a card's release
 * boundaries recur at exactly 2,097,152-byte multiples of the fd's total
 * consumed byte count, and lgpcnt is always on the same grid - so a
 * grid-aligned reader is always a whole number of 2 MiB releases behind
 * lgpcnt (never mid-release).
 *
 * Per card:
 *   1. Drain to the current release boundary (bounded; efficiency only).
 *   2. Grid-align: consume the exact byte remainder to the next boundary.
 *   3. Measure a boundary crossing EXACTLY: consume whole 2 MiB grid reads
 *      until one blocks a full release period (the stream is exactly at
 *      lgpcnt: at a boundary with nothing readable), then a 1-byte read that
 *      blocks until the NEXT boundary crossing and completes within
 *      microseconds of it. That completion IS the content time of the
 *      stream position - measured directly, not derived from period
 *      arithmetic (no mid-release ambiguity, no chunk overshoot).
 *   4. Skip to T_ref = max(card crossing times) + margin with EXACT byte
 *      counts (64 KiB chunks + an exact-remainder final read; blocking
 *      reads fill exactly the requested count).
 *
 * Residual: the 1-byte probe's IRQ/wakeup slop (tens of microseconds,
 * near-identical per card on the same system) + the measured-rate error
 * over the skip (microseconds) - byte-exact A/B content alignment.
 */
#define CXADC_SYNC_CHUNK_BYTES       ((size_t)64 * 1024)
#define CXADC_SYNC_RELEASE_BYTES     (2u * 1024 * 1024) /* driver 512-page boundary grid */
#define CXADC_SYNC_LIVE_CHUNK_MIN_US 4000
#define CXADC_SYNC_GRID_LIVE_MIN_US  20000
#define CXADC_SYNC_TREF_MARGIN_US    1000
#define CXADC_SYNC_MAX_DRAIN_BYTES   ((size_t)64 * 1024 * 1024)
#define CXADC_SYNC_MAX_TOTAL_US      (4000ULL * 1000ULL)
#define CXADC_SYNC_MAX_GRID_READS    64

static int cxadc_sync_read_card(cxadc_ctx_t *ctx, int i, uint8_t *buf, size_t n)
{
#if defined(_WIN32)
    return cxadc_read_card(ctx->card_handles[i], buf, n);
#else
    return cxadc_read_card(ctx->card_fds[i], buf, n);
#endif
}

static void cxadc_sync_card_starts(cxadc_ctx_t *ctx, int card_count)
{
    if (!ctx || card_count < 2) return;

    uint8_t *buf = (uint8_t *)malloc(CXADC_SYNC_RELEASE_BYTES);
    if (!buf) {
        fprintf(stderr, "[CXADC] sync-start: failed to allocate sync buffer; skipping sync\n");
        return;
    }

    uint64_t sync_start_us = get_time_us();
    uint64_t deadline_us = sync_start_us + CXADC_SYNC_MAX_TOTAL_US;
    size_t consumed_total[CXADC_MAX_CARDS];
    uint64_t crossing_us[CXADC_MAX_CARDS] = { 0 };
    size_t skip_bytes[CXADC_MAX_CARDS] = { 0 };
    bool measured[CXADC_MAX_CARDS] = { false };

    for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
        consumed_total[i] = s_cxadc_probe_consumed_bytes[i];
    }

    for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
        // Phase 1: drain to the current release boundary (bounded; the
        // exactness comes from phases 2-3, not this heuristic).
        size_t drained_here = 0;
        while (drained_here < CXADC_SYNC_MAX_DRAIN_BYTES &&
               get_time_us() < deadline_us) {
            uint64_t t0 = get_time_us();
            int got = cxadc_sync_read_card(ctx, i, buf, CXADC_SYNC_CHUNK_BYTES);
            uint64_t took_us = get_time_us() - t0;
            if (got < 0) break;
            if (got > 0) {
                consumed_total[i] += (size_t)got;
                drained_here += (size_t)got;
            }
            if (took_us >= CXADC_SYNC_LIVE_CHUNK_MIN_US) break; // caught up (approx)
        }

        // Phase 2: grid-align - consume the exact remainder to the next
        // 2,097,152-byte boundary multiple of the fd's consumed count.
        size_t rem = (size_t)(CXADC_SYNC_RELEASE_BYTES -
                              (consumed_total[i] % CXADC_SYNC_RELEASE_BYTES)) %
                     CXADC_SYNC_RELEASE_BYTES;
        while (rem > 0 && get_time_us() < deadline_us) {
            size_t want = (rem > CXADC_SYNC_CHUNK_BYTES) ? CXADC_SYNC_CHUNK_BYTES : rem;
            int got = cxadc_sync_read_card(ctx, i, buf, want);
            if (got < 0) { rem = 0; break; }
            if (got == 0) continue; // interrupted; blocking reads fill exactly
            consumed_total[i] += (size_t)got;
            rem = ((size_t)got >= rem) ? 0 : (rem - (size_t)got);
        }

        // Phase 3: exact boundary-crossing measurement - whole 2 MiB grid
        // reads until one blocks a full release period (the stream is
        // exactly at lgpcnt), then a 1-byte probe whose completion IS the
        // next boundary crossing (microsecond-level slop).
        for (size_t g = 0; g < CXADC_SYNC_MAX_GRID_READS && get_time_us() < deadline_us; g++) {
            uint64_t t0 = get_time_us();
            int got = cxadc_sync_read_card(ctx, i, buf, CXADC_SYNC_RELEASE_BYTES);
            uint64_t took_us = get_time_us() - t0;
            if (got < 0) break;
            if (got == 0) continue; // interrupted; retry the grid read
            consumed_total[i] += (size_t)got;
            if (took_us >= CXADC_SYNC_GRID_LIVE_MIN_US) {
                // The stream was exactly at lgpcnt and this read waited a
                // full release period. A 1-byte read now blocks until the
                // next boundary crossing; accept the measurement only if
                // it truly blocked (guards a phase-2 grid failure).
                uint64_t p0 = get_time_us();
                int one = cxadc_sync_read_card(ctx, i, buf, 1);
                uint64_t probe_us = get_time_us() - p0;
                if (one == 1) {
                    consumed_total[i] += 1;
                    if (probe_us >= 1000) {
                        crossing_us[i] = get_time_us() - sync_start_us;
                        measured[i] = true;
                    }
                }
                break;
            }
        }
    }

    // Phase 4: skip every measured card to T_ref with exact byte counts
    // (64 KiB chunks + an exact-remainder final read).
    uint64_t t_ref = 0;
    for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
        if (measured[i] && crossing_us[i] > t_ref) t_ref = crossing_us[i];
    }
    if (t_ref == 0) {
        fprintf(stderr, "[CXADC] sync-start: no boundary crossing measured within "
                        "budget (caps or read errors); streams left at drain positions (unaligned)\n");
        free(buf);
        return;
    }
    t_ref += CXADC_SYNC_TREF_MARGIN_US;

    for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
        if (!measured[i]) continue;
        uint64_t bytes_per_sample = ctx->tenbit_mode[i] ? 2u : 1u;
        uint64_t byte_rate = (uint64_t)ctx->card_sample_rate_hz[i] * bytes_per_sample;
        if (byte_rate == 0) byte_rate = 40000000u;
        uint64_t delta_us = (t_ref > crossing_us[i])
                                ? (t_ref - crossing_us[i])
                                : CXADC_SYNC_TREF_MARGIN_US;
        skip_bytes[i] = (size_t)((delta_us * byte_rate + 500000ULL) / 1000000ULL);
        size_t rem = skip_bytes[i];
        while (rem > 0 && get_time_us() < deadline_us) {
            size_t want = (rem > CXADC_SYNC_CHUNK_BYTES) ? CXADC_SYNC_CHUNK_BYTES : rem;
            int got = cxadc_sync_read_card(ctx, i, buf, want);
            if (got < 0) break;
            if (got == 0) continue;
            consumed_total[i] += (size_t)got;
            rem = ((size_t)got >= rem) ? 0 : (rem - (size_t)got);
        }
    }

    free(buf);

    fprintf(stderr, "[CXADC] sync-start: exact boundary alignment to a common content instant;");
    for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
        fprintf(stderr, " card%d%s: crossing@+%.3fms skip=%.1fKB total=%.2fMB",
                i, measured[i] ? "" : "(unmeasured)",
                (double)crossing_us[i] / 1000.0,
                (double)skip_bytes[i] / 1024.0,
                (double)consumed_total[i] / (1024.0 * 1024.0));
    }
    fprintf(stderr, " -> all streams at T_ref (byte-exact; residual ~tens of us)\n");
}

int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)
{
    if (!app) return -1;
#if defined(__APPLE__)
    int native_index=0;
    if(app->selected_device>=0 && app->selected_device<app->device_count)
        sscanf(app->devices[app->selected_device].serial,"CXADC_MACOS_%d",&native_index);
    gui_cxadc_macos_select(native_index);
    if(card_count!=1 || misrc_clockgen_mode){gui_app_set_status(app,"macOS CXADC supports one stock RAW8 card per capture");return -1;}
    app->settings.cxadc_tenbit_mode_card[0]=false;
    app->settings.rf_bits_a=8;app->settings.enable_resample_a=false;
    app->settings.capture_a=true;app->settings.capture_b=false;
    app->settings.enable_audio_4ch=false;app->settings.enable_audio_2ch_12=false;app->settings.enable_audio_2ch_34=false;
    for(int n=0;n<4;n++)app->settings.enable_audio_1ch[n]=false;
    app->settings.audio_monitor_playback=false;app->settings.use_flac=false;
    app->settings.stop_on_dropout=true;
#endif
    if (atomic_load(&s_cxadc.running)) return 0;

    // card_count == 0 is valid for the audio-only MISRC Clockgen path (no
    // cxadcN RF card nodes; the clockgen USB-audio feed is the sole source).
    // Clamp negatives to 0; cap at CXADC_MAX_CARDS.
    if (card_count < 0) card_count = 0;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = card_count;
    s_cxadc.misrc_clockgen_mode = misrc_clockgen_mode;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        bool mode = app->settings.cxadc_tenbit_mode_card[i];
        if (i >= card_count) {
            mode = false;
        }
        s_cxadc.tenbit_mode[i] = mode;
        // Resolve the card's real hardware rate. The CXADC Clockgen Mod
        // (card_count > 1) drives the cards at a true 40 MHz base regardless
        // of the sysfs crystal param, so force 40/20 for it. A single stock
        // card (card_count == 1) runs at 28.6 MHz; detect the real rate from
        // sysfs and fall back to the stock 28.6/14.3 baseline when
        // undetectable (Windows, missing sysfs) so the live sample-rate
        // readout matches the card instead of always 40.
        uint32_t detected_hz = 0;
        if (card_count > 1) {
            detected_hz = mode ? CXADC_SAMPLE_RATE_TENBIT_HZ : CXADC_SAMPLE_RATE_8BIT_HZ;
        } else if (!gui_cxadc_get_sample_rate_hz(i, mode, &detected_hz) || detected_hz == 0) {
            detected_hz = mode ? CXADC_STOCK_RATE_TENBIT_HZ : CXADC_STOCK_RATE_8BIT_HZ;
        }
        s_cxadc.card_sample_rate_hz[i] = detected_hz;
    }
    s_cxadc.rf_sample_rate_hz = s_cxadc.card_sample_rate_hz[0];
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
    s_cxadc.audio_endpoint = NULL;
    s_cxadc.audio_client = NULL;
    s_cxadc.audio_capture = NULL;
    s_cxadc.audio_com_initialized = false;
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
#endif
#endif
    // Shared audio format fields (zeroed by memset, set explicitly for clarity)
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_channels = 0;
    s_cxadc.audio_device_name[0] = '\0';

    gui_headswitch_lock_reset();
    // Skip CXADC card programming/open when there are no RF cards (audio-only
    // MISRC Clockgen). tenbit/center-offset sysfs writes would fail and abort.
    if (card_count > 0) {
        if (cxadc_apply_tenbit_modes(app, card_count, s_cxadc.tenbit_mode) != 0) {
            int saved_errno = errno;
            fprintf(stderr, "[CXADC] failed to apply tenbit mode on %d card(s): %s (errno %d)\n",
                    card_count, strerror(saved_errno), saved_errno);
#if defined(_WIN32)
            // Permission denials normally never reach here: the WMI
            // EACCES case inside cxadc_apply_tenbit_modes already fell back
            // to 8-bit and continued. Kept for safety in case a write is
            // denied in a way that bypassed the fallback.
            if (saved_errno == EACCES || saved_errno == EPERM) {
                gui_app_set_status(app, "CXADC: 10-bit write denied - re-run as administrator");
            } else {
                gui_app_set_status(app, "CXADC: failed to apply tenbit mode");
            }
#else
            if (saved_errno == EACCES || saved_errno == EPERM) {
                fprintf(stderr, "[CXADC] sysfs write denied. For 10-bit mode you need one-time setup:\n"
                                "          sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*\n"
                                "          sudo chmod g+w   /sys/class/cxadc/cxadc*/device/parameters/*\n"
                                "          sudo usermod -aG video $USER   (then log out/in)\n");
                gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
            } else {
                gui_app_set_status(app, "CXADC: failed to apply tenbit mode");
            }
#endif
            return -1;
        }

        if (cxadc_open_cards(&s_cxadc, card_count) != 0) {
#if defined(__APPLE__)
            gui_app_set_status(app,gui_cxadc_macos_error());
            return -1;
#endif
#if !defined(_WIN32)
            int saved_errno = errno;
            fprintf(stderr, "[CXADC] failed to open /dev/cxadc0..%d: %s (errno %d)\n",
                    card_count - 1, strerror(saved_errno), saved_errno);
            fprintf(stderr, "[CXADC] check the cxadc driver is loaded and /dev/cxadcN exists (ls -l /dev/cxadc*)\n");
            if (saved_errno == EACCES || saved_errno == EPERM) {
                // /dev/cxadcN is root:root 0600 by default. The user
                // needs to be in a group with read access (e.g. video)
                // or the node needs a udev rule. Defer a help popup to the
                // UI thread so the user sees the one-time setup instructions.
                atomic_store(&app->cxadc_open_perm_help_pending, true);
                gui_app_set_status(app, "CXADC: card device permission denied (see popup)");
            } else {
                gui_app_set_status(app, "CXADC: failed to open card device(s)");
            }
#else
            fprintf(stderr, "[CXADC] failed to open card device(s)\\n");
            gui_app_set_status(app, "CXADC: failed to open card device(s)");
#endif
            return -1;
        }
    }

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[CXADC] failed to initialize RF ringbuffer\n");
        gui_app_set_status(app, "CXADC: failed to initialize RF buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        fprintf(stderr, "[CXADC] failed to initialize audio ringbuffer\n");
        gui_app_set_status(app, "CXADC: failed to initialize audio buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    cxadc_reset_stats(app, s_cxadc.rf_sample_rate_hz);

    bool audio_capture_available = false;
    if (cxadc_open_audio_capture(&s_cxadc) == 0) {
        audio_capture_available = true;
        if (s_cxadc.audio_sample_rate_hz > 0) {
            atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
        }
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] audio capture device: %s (%u Hz, %d ch)\n",
                s_cxadc.audio_device_name,
                s_cxadc.audio_sample_rate_hz,
                s_cxadc.audio_channels);
#endif
    } else {
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] %s audio device not available; continuing RF-only\n",
                s_cxadc.misrc_clockgen_mode ? "MISRC clockgen" : "clockgen");
#if !defined(_WIN32) && LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] support note: for ClockGen Lite feed, check alsamixer card "
                        "\"CXADC+ADC-ClockGen\" (e.g. -D usbstream:CARD=CXADCADCClockGe) "
                        "and ensure \"USB Stream Output\" is enabled\n");
#endif
#endif
    }

    app->is_capturing = true;
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[CXADC] failed to start extraction thread\n");
        gui_app_set_status(app, "CXADC: failed to start extraction thread");
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[CXADC] Failed to start display thread (non-fatal)\n");
        }
    }

    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&s_cxadc.running, true);
    // Measured feed-rate check (uses the already-open card fds): corrects
    // card_sample_rate_hz when the sysfs crystal/tenxfsc belief is stale
    // (e.g. a 40 MHz-modded card still reporting the stock 28.636 crystal).
    // Runs before the RF thread starts so the rate readout, resample options
    // and the direct RAW availability check all use the measured truth.
    if (card_count > 0) {
        for (int i = 0; i < card_count && i < CXADC_MAX_CARDS; i++) {
#if defined(__APPLE__)
            continue; /* on-demand DMA has no boot backlog; keep configured nominal rate */
#endif
            // Reset the per-session consumed counter before the capture-start
            // probe so the exact-alignment grid references this fd session only.
            s_cxadc_probe_consumed_bytes[i] = 0;
            cxadc_probe_and_cache_card_rate(app, i, s_cxadc.tenbit_mode[i],
                                             CXADC_RATE_PROBE_START_BYTES);
            int t = s_cxadc.tenbit_mode[i] ? 1 : 0;
            if (s_cxadc_measured_rate_state[i][t] == CXADC_RATE_STATE_TIER) {
                s_cxadc.card_sample_rate_hz[i] = s_cxadc_measured_rate_hz[i][t];
            }
        }
        s_cxadc.rf_sample_rate_hz = s_cxadc.card_sample_rate_hz[0];
    }
    // F1b: the sequential probes above leave card 0's stream position ~one
    // probe duration (~105-155 ms) behind card 1's in wall-clock content
    // time, and the RF thread's lockstep preserves that offset for the whole
    // capture (measured 114 ms on 2026-10-06). Re-align both streams to a
    // common content instant before the RF thread starts (2+ cards only).
    if (card_count > 1) {
        cxadc_sync_card_starts(&s_cxadc, card_count);
    }
    // Publish the final measured rates before any RF write reaches the
    // extraction sideband; initial sysfs rates may have been corrected above.
    atomic_store(&app->rf_source_rate_hz[0], s_cxadc.card_sample_rate_hz[0]);
    atomic_store(&app->rf_source_rate_hz[1], s_cxadc.card_sample_rate_hz[1]);
    atomic_store(&app->rf_preview_input_supported,
                  card_count < 2 || s_cxadc.card_sample_rate_hz[0] == s_cxadc.card_sample_rate_hz[1]);
    // The RF card-reading thread only applies when there are cxadcN cards.
    // In audio-only MISRC Clockgen mode (card_count == 0) the clockgen USB-audio
    // feed is the sole source, so skip the RF thread and run audio-only.
    if (card_count > 0) {
        if (thrd_create_with_priority(&s_cxadc.rf_thread,
                                      cxadc_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_CRITICAL) != thrd_success) {
            fprintf(stderr, "[CXADC] failed to create RF capture thread\n");
            gui_app_set_status(app, "CXADC: failed to start RF capture thread");
            atomic_store(&s_cxadc.running, false);
            gui_audio_stop(app);
            if (app->display_thread) {
                gui_display_thread_stop(app->display_thread);
            }
            gui_extract_stop();
            app->is_capturing = false;
            cxadc_close_audio_capture(&s_cxadc);
            cxadc_close_cards(&s_cxadc);
            return -1;
        }
        s_cxadc.rf_thread_started = true;
    }

    if (audio_capture_available) {
        if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                      cxadc_audio_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_ABOVE) != thrd_success) {
            fprintf(stderr, "[CXADC] Failed to start audio capture thread; continuing RF-only\n");
            cxadc_close_audio_capture(&s_cxadc);
        } else {
            s_cxadc.audio_thread_started = true;
        }
    }

    // Status is mode-aware: MISRC Clockgen is reported regardless of card_count
    // (audio-only rigs have 0 cards); CXADC Clockgen only applies with >1 card.
    if (s_cxadc.misrc_clockgen_mode) {
        gui_app_set_status(app, "MISRC Clockgen capture running");
    } else if (card_count > 1) {
        gui_app_set_status(app, "CXADC Clockgen capture running");
    } else {
        gui_app_set_status(app, "CXADC capture running");
    }
    return 0;
}

void gui_cxadc_stop(gui_app_t *app)
{
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.rf_thread_started && !s_cxadc.audio_thread_started) {
        return;
    }

    if (!app) {
        app = s_cxadc.app;
    }
    // MISRC Clockgen is a clockgen variant even with 0 RF cards (audio-only).
    bool was_clockgen_mode = (s_cxadc.card_count > 1) || s_cxadc.misrc_clockgen_mode;
    bool was_misrc_clockgen_mode = s_cxadc.misrc_clockgen_mode;

    if (app) {
        app->is_capturing = false;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);

    if (s_cxadc.rf_thread_started) {
        thrd_join(s_cxadc.rf_thread, NULL);
        s_cxadc.rf_thread_started = false;
    }
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }

    if (app && app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }
    gui_audio_stop(app);
    gui_extract_stop();
    cxadc_close_audio_capture(&s_cxadc);

    cxadc_close_cards(&s_cxadc);
    gui_headswitch_lock_reset();

    if (app) {
        atomic_store(&app->stream_synced, false);
        atomic_store(&app->dropout_stop_requested, false);
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
        if (was_clockgen_mode) {
            gui_app_set_status(app, was_misrc_clockgen_mode
                ? "MISRC Clockgen capture stopped"
                : "CXADC Clockgen capture stopped");
        } else {
            gui_app_set_status(app, "CXADC capture stopped");
        }
    }

    memset(&s_cxadc, 0, sizeof(s_cxadc));
}

bool gui_cxadc_is_running(void)
{
    return atomic_load(&s_cxadc.running);
}

bool gui_cxadc_direct_record_available(int channel, uint8_t rf_bits, size_t *bytes_per_sample_out)
{
    if (channel < 0 || channel >= CXADC_MAX_CARDS) return false;
    if (!atomic_load(&s_cxadc.running)) return false;
    if (channel >= s_cxadc.card_count) return false;
    // The requested output width must match the card's live sample width so
    // the passthrough bytes are exactly the requested samples (8-bit mode
    // -> unsigned u8 bytes, tenbit mode -> unsigned u16 words).
    bool tenbit = s_cxadc.tenbit_mode[channel];
    bool want_tenbit = (rf_bits != 8);
    if (tenbit != want_tenbit) return false;
    if (bytes_per_sample_out) {
        *bytes_per_sample_out = tenbit ? 2u : 1u;
    }
    return true;
}

int gui_cxadc_start_clockgen_audio(gui_app_t *app, bool misrc_clockgen_mode)
{
    // Audio-only subset of gui_cxadc_start: opens the clockgen USB-audio capture
    // device (WASAPI/ALSA) and starts the cxadc audio thread feeding
    // BUF_CAPTURE_AUDIO + headswitch ingest. Does NOT open CXADC RF cards, start
    // the RF thread, or own extraction/display/audio-monitor threads (the caller's
    // RF path owns those). Used by MISRC Clockgen, which runs the hsdaoh raw-
    // parser RF feed (A+B) in parallel and shares one set of extraction/display/
    // audio-monitor threads.
    if (!app) return -1;
    if (atomic_load(&s_cxadc.running)) return 0;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = 0;  // audio-only; no CXADC RF cards
    s_cxadc.misrc_clockgen_mode = misrc_clockgen_mode;
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
    s_cxadc.audio_endpoint = NULL;
    s_cxadc.audio_client = NULL;
    s_cxadc.audio_capture = NULL;
    s_cxadc.audio_com_initialized = false;
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
#endif
#endif
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_channels = 0;
    s_cxadc.audio_device_name[0] = '\0';

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        gui_app_set_status(app, "MISRC Clockgen: failed to initialize audio buffer");
        return -1;
    }

    gui_headswitch_lock_reset();

    if (cxadc_open_audio_capture(&s_cxadc) != 0) {
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] %s audio device not available; continuing RF-only\n",
                s_cxadc.misrc_clockgen_mode ? "MISRC clockgen" : "clockgen");
#endif
        return -1;
    }

    if (s_cxadc.audio_sample_rate_hz > 0) {
        atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
    }
#if defined(_WIN32) || LIBASOUND_ENABLED
    fprintf(stderr, "[CXADC] audio capture device: %s (%u Hz, %d ch)\n",
            s_cxadc.audio_device_name,
            s_cxadc.audio_sample_rate_hz,
            s_cxadc.audio_channels);
#endif

    atomic_store(&s_cxadc.running, true);
    if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                  cxadc_audio_capture_thread,
                                  &s_cxadc,
                                  THRD_PRIORITY_ABOVE) != thrd_success) {
        fprintf(stderr, "[CXADC] Failed to start clockgen audio thread; continuing RF-only\n");
        cxadc_close_audio_capture(&s_cxadc);
        atomic_store(&s_cxadc.running, false);
        return -1;
    }
    s_cxadc.audio_thread_started = true;
    return 0;
}

void gui_cxadc_stop_clockgen_audio(void)
{
    // Audio-only subset of gui_cxadc_stop: stops/joins the cxadc audio thread
    // and closes the audio device. Does NOT stop extraction/display/audio-
    // monitor/cards (owned by the caller's RF path).
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.audio_thread_started) {
        return;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }
    cxadc_close_audio_capture(&s_cxadc);
    gui_headswitch_lock_reset();
    memset(&s_cxadc, 0, sizeof(s_cxadc));
}
