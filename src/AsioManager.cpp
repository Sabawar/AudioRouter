#include "AsioManager.h"
#include "Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

// COM support
#include <objbase.h>
#include <combaseapi.h>

static const CLSID s_nullClsid = {};  // zeroed CLSID for null-check

AsioManager* AsioManager::s_instance = nullptr;

// ──────────────────────────────────────────────────────────────────────────────
//  Helper: get DLL path for a CLSID from registry
// ──────────────────────────────────────────────────────────────────────────────
static std::wstring getDllPathForClsid(const CLSID& clsid) {
    LPOLESTR clsidStr = nullptr;
    if (FAILED(StringFromCLSID(clsid, &clsidStr))) return {};

    std::wstring path = L"CLSID\\";
    path += clsidStr;
    path += L"\\InprocServer32";
    CoTaskMemFree(clsidStr);

    for (HKEY root : { HKEY_CLASSES_ROOT, HKEY_LOCAL_MACHINE }) {
        HKEY key = nullptr;
        LONG res = (root == HKEY_CLASSES_ROOT)
            ? RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key)
            : RegOpenKeyExW(root,
                           (std::wstring(L"SOFTWARE\\Classes\\") + path).c_str(),
                           0, KEY_READ, &key);
        if (res != ERROR_SUCCESS) continue;

        wchar_t dllPath[MAX_PATH] {};
        DWORD   sz   = sizeof(dllPath);
        DWORD   type = 0;
        LONG    qr   = RegQueryValueExW(key, nullptr, nullptr, &type,
                                        (LPBYTE)dllPath, &sz);
        RegCloseKey(key);
        if (qr == ERROR_SUCCESS && type == REG_SZ && dllPath[0])
            return dllPath;
    }
    return {};
}

// ──────────────────────────────────────────────────────────────────────────────
//  Registry enumeration of ASIO drivers
// ──────────────────────────────────────────────────────────────────────────────
std::vector<AsioDeviceInfo> AsioManager::enumerateDrivers() {
    std::vector<AsioDeviceInfo> result;

    // Enumerate both native (64-bit) and WOW32 ASIO keys
    const char* keys[] = {
        "SOFTWARE\\ASIO",
        "SOFTWARE\\WOW6432Node\\ASIO"  // 32-bit drivers on 64-bit Windows
    };

    for (auto* regPath : keys) {
        HKEY root = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPath, 0, KEY_READ, &root) != ERROR_SUCCESS)
            continue;

        char  name[256];
        DWORD nameLen = 256;
        DWORD index   = 0;

        while (RegEnumKeyExA(root, index++, name, &nameLen, nullptr,
                             nullptr, nullptr, nullptr) == ERROR_SUCCESS)
        {
            AsioDeviceInfo info;
            info.name = name;

            HKEY sub = nullptr;
            if (RegOpenKeyExA(root, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
                char  val[64];
                DWORD valLen = sizeof(val);
                DWORD type;
                if (RegQueryValueExA(sub, "CLSID", nullptr, &type,
                                     (LPBYTE)val, &valLen) == ERROR_SUCCESS) {
                    info.clsid = val;
                    std::wstring wclsid(info.clsid.begin(), info.clsid.end());
                    CLSIDFromString(wclsid.c_str(), &info.guid);

                    // Check if DLL actually exists
                    info.dllPath = getDllPathForClsid(info.guid);
                    info.is32bit = (strstr(regPath, "WOW6432") != nullptr);
                }
                RegCloseKey(sub);
            }

            // Avoid duplicates (same CLSID might appear in both keys)
            bool dup = false;
            for (auto& ex : result)
                if (ex.clsid == info.clsid) { dup = true; break; }
            if (!dup && !info.clsid.empty())
                result.push_back(info);

            nameLen = 256;
        }
        RegCloseKey(root);
    }
    return result;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ──────────────────────────────────────────────────────────────────────────────
AsioManager::AsioManager() = default;
AsioManager::~AsioManager() { unloadDriver(); }

// ──────────────────────────────────────────────────────────────────────────────
//  Load driver — direct DLL approach (reliable on all Windows versions)
//  Falls back to CoCreateInstance if DLL load fails.
// ──────────────────────────────────────────────────────────────────────────────
bool AsioManager::loadDriver(const AsioDeviceInfo& dev, HWND ownerWindow) {
    unloadDriver();

    if (IsEqualCLSID(dev.guid, s_nullClsid)) {
        LOG_INFO("ASIO: пустой CLSID — драйвер не выбран");
        return false;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ── Method 1: Direct DLL load (bypasses COM apartment issues) ──────────
    std::wstring dllPath = getDllPathForClsid(dev.guid);
    if (!dllPath.empty()) {
        LOG_INFO("ASIO: загружаем DLL напрямую: %S", dllPath.c_str());
        m_driverDll = LoadLibraryExW(dllPath.c_str(), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m_driverDll) {
            typedef HRESULT (WINAPI *DllGetClassObjectFn)(REFCLSID, REFIID, LPVOID*);
            auto DllGetClassObject = (DllGetClassObjectFn)
                GetProcAddress(m_driverDll, "DllGetClassObject");
            if (DllGetClassObject) {
                IClassFactory* factory = nullptr;
                HRESULT hr = DllGetClassObject(dev.guid, IID_IClassFactory,
                                               (void**)&factory);
                if (SUCCEEDED(hr) && factory) {
                    hr = factory->CreateInstance(nullptr, dev.guid,
                                                 (void**)&m_asio);
                    factory->Release();
                    if (FAILED(hr) || !m_asio) {
                        // Try IID_IUnknown and cast directly
                        IUnknown* pUnk = nullptr;
                        hr = DllGetClassObject(dev.guid, IID_IUnknown,
                                               (void**)&pUnk);
                        if (SUCCEEDED(hr) && pUnk)
                            m_asio = reinterpret_cast<IASIO*>(pUnk);
                    }
                }
            }
            if (!m_asio) {
                // DLL loaded but no interface — free it, try COM
                FreeLibrary(m_driverDll);
                m_driverDll = nullptr;
                LOG_INFO("ASIO: DllGetClassObject не дал интерфейс, пробуем COM...");
            }
        } else {
            DWORD err = GetLastError();
            LOG_INFO("ASIO: LoadLibrary('%S') failed err=%u, пробуем COM...",
                     dllPath.c_str(), err);
        }
    } else {
        LOG_INFO("ASIO: DLL-путь не найден в реестре для '%s', пробуем COM...",
                 dev.name.c_str());
    }

    // ── Method 2: Standard CoCreateInstance fallback ──────────────────────
    if (!m_asio) {
        HRESULT hr = CoCreateInstance(dev.guid, nullptr, CLSCTX_INPROC_SERVER,
                                      dev.guid, (void**)&m_asio);
        if (FAILED(hr) || !m_asio) {
            LOG_INFO("ASIO: оба метода провалились для '%s' (COM hr=0x%08X).\n"
                     "Возможные причины:\n"
                     "  - 32-bit драйвер в 64-bit процессе (ASIO4ALL)\n"
                     "  - Драйвер не установлен в системе\n"
                     "  - Требуется запуск от администратора",
                     dev.name.c_str(), (unsigned)hr);
            m_asio = nullptr;
            return false;
        }
    }

    // ── Init ──────────────────────────────────────────────────────────────
    if (!m_asio->init((void*)ownerWindow)) {
        LOG_INFO("ASIO: init() вернул false для '%s'. "
                 "Проверьте что устройство подключено и не занято другой программой.",
                 dev.name.c_str());
        m_asio->Release();
        m_asio = nullptr;
        if (m_driverDll) { FreeLibrary(m_driverDll); m_driverDll = nullptr; }
        return false;
    }

    char nameBuf[64] {};
    m_asio->getDriverName(nameBuf);
    m_driverName = nameBuf;
    m_current    = dev;

    long inCh = 0, outCh = 0;
    m_asio->getChannels(&inCh, &outCh);
    m_numIn  = (int)inCh;
    m_numOut = (int)outCh;

    long minSize, maxSize, prefSize, granul;
    m_asio->getBufferSize(&minSize, &maxSize, &prefSize, &granul);

    m_bufSizeMin  = (int)minSize;
    m_bufSizePref = (int)prefSize;
    m_bufSize     = dev.useMinBuffer ? (int)minSize : (int)prefSize;

    ASIOSampleRate sr;
    m_asio->getSampleRate(&sr);
    m_sampleRate = sr;

    LOG_INFO("ASIO: '%s' загружен. In=%d Out=%d BufMin=%d BufPref=%d BufUsed=%d SR=%.0f",
             nameBuf, m_numIn, m_numOut, (int)minSize, (int)prefSize,
             m_bufSize, m_sampleRate);

    s_instance = this;
    return true;
}

void AsioManager::unloadDriver() {
    stopAndDisposeBuffers();
    if (m_asio) {
        m_asio->Release();
        m_asio = nullptr;
    }
    // Free DLL only if we loaded it directly (not via COM)
    if (m_driverDll) {
        FreeLibrary(m_driverDll);
        m_driverDll = nullptr;
    }
    if (s_instance == this) s_instance = nullptr;
    m_driverName.clear();
    m_numIn = m_numOut = 0;
}

void AsioManager::showControlPanel() {
    if (m_asio) m_asio->controlPanel();
}

bool AsioManager::queryLiveSettings() {
    if (!m_asio) return false;

    double prevSr  = m_sampleRate;
    int    prevBuf = m_bufSize;
    int    prevMin = m_bufSizeMin;
    int    prevPrf = m_bufSizePref;

    // Re-query driver
    ASIOSampleRate sr{};
    m_asio->getSampleRate(&sr);
    m_sampleRate = sr;

    long mn, mx, pf, gr;
    m_asio->getBufferSize(&mn, &mx, &pf, &gr);
    m_bufSizeMin  = (int)mn;
    m_bufSizePref = (int)pf;
    // bufSize (currently used) can only change after driver reload

    bool changed = (m_sampleRate != prevSr  ||
                    m_bufSizeMin  != prevMin ||
                    m_bufSizePref != prevPrf);
    if (changed)
        LOG_INFO("ASIO: live settings updated — SR=%.0f BufMin=%d BufPref=%d",
                 m_sampleRate, m_bufSizeMin, m_bufSizePref);
    return changed;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Create buffers & start
// ──────────────────────────────────────────────────────────────────────────────
bool AsioManager::createBuffersAndStart(int numInputs, int numOutputs,
                                         AsioBufferCallback callback)
{
    if (!m_asio || m_running) return false;

    numInputs  = std::min(numInputs,  m_numIn);
    numOutputs = std::min(numOutputs, m_numOut);

    m_bufInfos.clear();
    m_bufInfos.resize(numInputs + numOutputs);

    int idx = 0;
    for (int i = 0; i < numInputs; ++i) {
        m_bufInfos[idx].isInput    = ASIOTrue;
        m_bufInfos[idx].channelNum = i;
        m_bufInfos[idx].buffers[0] = m_bufInfos[idx].buffers[1] = nullptr;
        ++idx;
    }
    for (int i = 0; i < numOutputs; ++i) {
        m_bufInfos[idx].isInput    = ASIOFalse;
        m_bufInfos[idx].channelNum = i;
        m_bufInfos[idx].buffers[0] = m_bufInfos[idx].buffers[1] = nullptr;
        ++idx;
    }

    // Get channel info to know sample type
    for (int i = 0; i < numInputs; ++i) {
        m_chanInfo[i].channel = i;
        m_chanInfo[i].isInput = ASIOTrue;
        m_asio->getChannelInfo(&m_chanInfo[i]);
    }
    for (int i = 0; i < numOutputs; ++i) {
        m_chanInfo[numInputs + i].channel = i;
        m_chanInfo[numInputs + i].isInput = ASIOFalse;
        m_asio->getChannelInfo(&m_chanInfo[numInputs + i]);
    }

    m_callbacks.bufferSwitch         = &AsioManager::onBufferSwitch;
    m_callbacks.sampleRateDidChange  = &AsioManager::onSampleRateChanged;
    m_callbacks.asioMessage          = &AsioManager::onAsioMessage;
    m_callbacks.bufferSwitchTimeInfo = &AsioManager::onBufferSwitchTimeInfo;

    ASIOError err = m_asio->createBuffers(m_bufInfos.data(),
                                          (long)m_bufInfos.size(),
                                          m_bufSize, &m_callbacks);
    if (err != ASE_OK) return false;

    m_userCallback = callback;
    m_inputBuf.resize(numInputs  * m_bufSize, 0.0f);
    m_outputBuf.resize(numOutputs * m_bufSize, 0.0f);
    m_inPeaks.assign(numInputs,  0.0f);
    m_outPeaks.assign(numOutputs, 0.0f);

    m_asio->start();
    m_running = true;
    return true;
}

void AsioManager::stopAndDisposeBuffers() {
    if (!m_asio || !m_running) return;
    m_asio->stop();
    m_asio->disposeBuffers();
    m_bufInfos.clear();
    m_running = false;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Buffer processing
// ──────────────────────────────────────────────────────────────────────────────
static float asioSampleToFloat(void* buf, int index, ASIOSampleType type) {
    switch (type) {
    case ASIOSTFloat32LSB: return reinterpret_cast<float*>(buf)[index];
    case ASIOSTFloat64LSB: return (float)reinterpret_cast<double*>(buf)[index];
    case ASIOSTInt32LSB:   return reinterpret_cast<INT32*>(buf)[index] / 2147483648.0f;
    case ASIOSTInt16LSB:   return reinterpret_cast<INT16*>(buf)[index] / 32768.0f;
    case ASIOSTInt24LSB: {
        const BYTE* p = (const BYTE*)buf + index * 3;
        INT32 v = p[0] | (p[1]<<8) | (p[2]<<16);
        if (v & 0x800000) v |= 0xFF000000;
        return v / 8388608.0f;
    }
    default: return 0.0f;
    }
}

static void floatToAsioSample(float f, void* buf, int index, ASIOSampleType type) {
    f = std::clamp(f, -1.0f, 1.0f);
    switch (type) {
    case ASIOSTFloat32LSB: reinterpret_cast<float*>(buf)[index] = f; break;
    case ASIOSTFloat64LSB: reinterpret_cast<double*>(buf)[index] = f; break;
    case ASIOSTInt32LSB:   reinterpret_cast<INT32*>(buf)[index]  = (INT32)(f * 2147483647.0f); break;
    case ASIOSTInt16LSB:   reinterpret_cast<INT16*>(buf)[index]  = (INT16)(f * 32767.0f); break;
    case ASIOSTInt24LSB: {
        INT32 v = (INT32)(f * 8388607.0f);
        BYTE* p = (BYTE*)buf + index * 3;
        p[0] = v & 0xFF; p[1] = (v>>8) & 0xFF; p[2] = (v>>16) & 0xFF;
        break;
    }
    default: break;
    }
}

void AsioManager::processBuffers(long bufIndex) {
    int numIn  = (int)m_inPeaks.size();
    int numOut = (int)m_outPeaks.size();

    // Convert ASIO planar buffers to interleaved float: [ch0s0,ch1s0, ch0s1,ch1s1,...]
    m_inputBuf.resize(numIn * m_bufSize);
    for (int ch = 0; ch < numIn; ++ch) {
        void* buf = m_bufInfos[ch].buffers[bufIndex];
        if (!buf) continue;
        ASIOSampleType type = m_chanInfo[ch].type;
        float pk = 0.0f;
        for (int i = 0; i < m_bufSize; ++i) {
            float s = asioSampleToFloat(buf, i, type);
            m_inputBuf[i * numIn + ch] = s;   // interleaved
            pk = std::max(pk, std::abs(s));
        }
        m_inPeaks[ch] = std::max(pk, m_inPeaks[ch] * 0.99f);
    }

    // Zero output buffer (interleaved layout)
    m_outputBuf.assign(numOut * m_bufSize, 0.0f);

    // Invoke user callback — both buffers are interleaved
    if (m_userCallback)
        m_userCallback(bufIndex, m_inputBuf, m_outputBuf,
                       m_bufSize, numIn, numOut);

    // Convert interleaved output back to ASIO planar buffers
    for (int ch = 0; ch < numOut; ++ch) {
        void* buf = m_bufInfos[numIn + ch].buffers[bufIndex];
        if (!buf) continue;
        ASIOSampleType type = m_chanInfo[numIn + ch].type;
        float pk = 0.0f;
        for (int i = 0; i < m_bufSize; ++i) {
            float s = m_outputBuf[i * numOut + ch];   // interleaved
            pk = std::max(pk, std::abs(s));
            floatToAsioSample(s, buf, i, type);
        }
        m_outPeaks[ch] = std::max(pk, m_outPeaks[ch] * 0.99f);
    }

    m_asio->outputReady();
}
#include "Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

// COM support
#include <objbase.h>
#include <combaseapi.h>


//  Static ASIO callbacks
// ──────────────────────────────────────────────────────────────────────────────
void AsioManager::onBufferSwitch(long index, ASIOBool) {
    if (s_instance) s_instance->processBuffers(index);
}
void AsioManager::onSampleRateChanged(ASIOSampleRate rate) {
    if (s_instance) s_instance->m_sampleRate = rate;
}
long AsioManager::onAsioMessage(long sel, long, void*, double*) {
    switch (sel) {
    case kAsioEngineVersion:    return 2;
    case kAsioSupportsTimeInfo: return 1;
    default:                    return 0;
    }
}
ASIOTime* AsioManager::onBufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    if (s_instance) s_instance->processBuffers(index);
    return t;
}

float AsioManager::inputPeak(int ch)  const {
    return (ch < (int)m_inPeaks.size())  ? m_inPeaks[ch]  : 0.0f; }
float AsioManager::outputPeak(int ch) const {
    return (ch < (int)m_outPeaks.size()) ? m_outPeaks[ch] : 0.0f; }
