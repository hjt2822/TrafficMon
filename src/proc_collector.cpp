#include "proc_collector.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tlhelp32.h>

#include <cstring>
#include <cctype>
#include <cwchar>
#include <vector>

namespace {

const wchar_t kSessionName[] = L"TrafficMonM1";

// Microsoft-Windows-Kernel-Network
const GUID kKernelNetwork = {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

// Microsoft-Windows-Kernel-Process
const GUID kKernelProcess = {0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

const ULONGLONG kKeywordIPv4 = 0x10;
const ULONGLONG kKeywordIPv6 = 0x20;
const ULONGLONG kKeywordProcess = 0x10;  // WINEVENT_KEYWORD_PROCESS
const ULONGLONG kKeywordImage = 0x40;    // WINEVENT_KEYWORD_IMAGE

std::string ToUtf8(const wchar_t* text, int length) {
    if (length <= 0) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    return out;
}

int64_t FileTimeValue(const FILETIME& t) {
    return static_cast<int64_t>((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime);
}

bool IsPrivateV4(const uint8_t* b) {
    if (b[0] == 10) return true;
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 169 && b[1] == 254) return true;
    if (b[0] == 127) return true;
    if (b[0] >= 224) return true;  // multicast / broadcast
    return false;
}

bool IsPrivateV6(const uint8_t* b) {
    if ((b[0] & 0xfe) == 0xfc) return true;                 // fc00::/7 unique local
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true; // fe80::/10 link local
    if (b[0] == 0xff) return true;                          // multicast
    bool loop = true;
    for (int i = 0; i < 15; ++i) loop = loop && b[i] == 0;
    return loop && b[15] == 1;
}

void WINAPI EventCallback(PEVENT_RECORD rec) {
    auto* self = static_cast<ProcNetCollector*>(rec->UserContext);
    if (!self) return;

    // Microsoft-Windows-Kernel-Process: id 1 = ProcessStart, 2 = ProcessStop.
    // ProcessStart carries no image name, but the process usually still exists when the
    // event arrives, so resolve its path eagerly and cache it for its whole lifetime.
    if (IsEqualGUID(rec->EventHeader.ProviderId, kKernelProcess)) {
        const uint8_t* d = static_cast<const uint8_t*>(rec->UserData);
        if (!d || rec->UserDataLength < 4) return;
        const int id = rec->EventHeader.EventDescriptor.Id;
        self->processEvents_++;
        if (id == 1 || id == 2) {
            uint32_t pid;
            std::memcpy(&pid, d, 4);
            int64_t created = 0;
            if (rec->UserDataLength >= 20) std::memcpy(&created, d + 12, 8);
            if (id == 1) {
                self->OnProcessStart(pid, created);
            } else {
                std::string name;
                if (rec->UserDataLength > 84) {
                    const char* text = reinterpret_cast<const char*>(d + 84);
                    size_t max = rec->UserDataLength - 84, n = 0;
                    while (n < max && text[n]) ++n;
                    name.assign(text, n);
                }
                self->OnProcessStop(pid, created, name);
            }
            return;
        }
        // ImageLoad: ImageBase(8), ImageSize(8), ProcessID(4)@16, ..., ImageName@36.
        // The first image loaded by a process is its executable, with a full NT path.
        if (id == 5) {
            if (rec->UserDataLength < 44) return;
            uint32_t pid;
            std::memcpy(&pid, d + 16, 4);
            const wchar_t* text = reinterpret_cast<const wchar_t*>(d + 36);
            size_t max = (rec->UserDataLength - 36) / 2, n = 0;
            while (n < max && text[n]) ++n;
            if (pid && n && text[0] == L'\\') self->OnImageLoad(pid, ToUtf8(text, static_cast<int>(n)));
            return;
        }
        return;
    }

    if (!IsEqualGUID(rec->EventHeader.ProviderId, kKernelNetwork)) return;

    bool isSend;
    bool v6;
    bool remoteIsSaddr = false;  // UDP receive events carry the local address in daddr
    switch (rec->EventHeader.EventDescriptor.Id) {
        case 10: case 42: isSend = true;  v6 = false; break;
        case 11:          isSend = false; v6 = false; break;
        case 43:          isSend = false; v6 = false; remoteIsSaddr = true; break;
        case 26: case 58: isSend = true;  v6 = true;  break;
        case 27:          isSend = false; v6 = true;  break;
        case 59:          isSend = false; v6 = true;  remoteIsSaddr = true; break;
        default: return;
    }

    const uint8_t* d = static_cast<const uint8_t*>(rec->UserData);
    const size_t addrLen = v6 ? 16 : 4;
    const size_t need = 8 + 2 * addrLen;
    if (!d || rec->UserDataLength < need) return;

    uint32_t pid, size;
    std::memcpy(&pid, d, 4);
    std::memcpy(&size, d + 4, 4);
    const uint8_t* remote = d + 8 + (remoteIsSaddr ? addrLen : 0);
    bool lan = v6 ? IsPrivateV6(remote) : IsPrivateV4(remote);
    self->events_++;
    self->OnData(pid, size, isSend, lan, rec->EventHeader.TimeStamp.QuadPart);
}

}  // namespace

std::string ProcNetCollector::IdentityFor(uint32_t pid, int64_t eventFileTime) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = identities_.find(pid);
        if (it != identities_.end() && it->second.createTime <= eventFileTime) return it->second.path;
    }
    std::string path;
    int64_t created = 0;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle) {
        FILETIME creation{}, exited{}, kernel{}, user{};
        wchar_t buffer[32768];
        DWORD length = 32768;
        if (GetProcessTimes(handle, &creation, &exited, &kernel, &user)) created = FileTimeValue(creation);
        if (created <= eventFileTime && QueryFullProcessImageNameW(handle, 0, buffer, &length))
            path = ToUtf8(buffer, static_cast<int>(length));
        CloseHandle(handle);
    }
    if (!path.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        if (identities_.size() > 100000) identities_.clear();  // lost ProcessStop events
        identities_[pid] = CachedIdentity{path, created};
    }
    return path;
}

void ProcNetCollector::SeedIdentities() {
    // Processes already running when the session starts may exit before their first network
    // event is delivered (ETW real-time latency is ~1-3 s), so snapshot identities up front.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!entry.th32ProcessID) continue;
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!handle) continue;
            FILETIME creation{}, exited{}, kernel{}, user{};
            wchar_t path[32768];
            DWORD length = 32768;
            if (GetProcessTimes(handle, &creation, &exited, &kernel, &user) &&
                QueryFullProcessImageNameW(handle, 0, path, &length)) {
                identities_[entry.th32ProcessID] = CachedIdentity{
                    ToUtf8(path, static_cast<int>(length)), FileTimeValue(creation)};
            }
            CloseHandle(handle);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

void ProcNetCollector::OnProcessStart(uint32_t pid, int64_t createTime) {
    std::string path;
    int64_t actual = 0;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle) {
        FILETIME creation{}, exited{}, kernel{}, user{};
        wchar_t buffer[32768];
        DWORD length = 32768;
        if (GetProcessTimes(handle, &creation, &exited, &kernel, &user)) actual = FileTimeValue(creation);
        if (QueryFullProcessImageNameW(handle, 0, buffer, &length))
            path = ToUtf8(buffer, static_cast<int>(length));
        CloseHandle(handle);
    }
    if (actual == 0) actual = createTime;
    std::lock_guard<std::mutex> lk(mu_);
    if (started_.size() > 100000) started_.clear();  // lost ProcessStop events
    started_[pid] = actual;
    if (path.empty()) return;  // ImageLoad (delivered later) still carries the full path
    identities_[pid] = CachedIdentity{path, actual};
    FlushPendingLocked(pid, path);
}

void ProcNetCollector::OnImageLoad(uint32_t pid, const std::string& ntPath) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_.count(pid)) return;  // ignore DLL loads of processes started before this session
    if (images_.count(pid)) return;    // the first image is the executable
    if (images_.size() > 100000) images_.clear();
    images_[pid] = NormalizeImagePath(ntPath);
}

void ProcNetCollector::OnProcessStop(uint32_t pid, int64_t createTime, const std::string& imageName) {
    std::string identity = "unknown:" + std::string(sessionName_.begin(), sessionName_.end()) + ":" +
                           std::to_string(pid);
    std::lock_guard<std::mutex> lk(mu_);
    const auto image = images_.find(pid);
    const auto started = started_.find(pid);
    const bool sameInstance = started == started_.end() || started->second == 0 ||
                              createTime == 0 || started->second == createTime;
    // ProcessStop's ImageName is a basename, used to reject a candidate that is really a DLL.
    if (image != images_.end() && sameInstance) {
        const std::string& candidate = image->second;
        size_t slash = candidate.find_last_of('\\');
        std::string base = slash == std::string::npos ? candidate : candidate.substr(slash + 1);
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return s;
        };
        if (imageName.empty() || lower(base) == lower(imageName)) identity = candidate;
    }
    FlushPendingLocked(pid, identity);
    identities_.erase(pid);
    images_.erase(pid);
    started_.erase(pid);
}

void ProcNetCollector::FlushPendingLocked(uint32_t pid, const std::string& identity) {
    auto pending = pending_.find(pid);
    if (pending == pending_.end()) return;
    for (const auto& [hour, hourData] : pending->second) {
        TrafficDelta& record = history_[{hour, identity}];
        record.identity = identity; record.utcSeconds = hour; record.lanSplit = true;
        record.up += static_cast<int64_t>(hourData.up);
        record.down += static_cast<int64_t>(hourData.down);
        record.upLan += static_cast<int64_t>(hourData.upLan);
        record.downLan += static_cast<int64_t>(hourData.downLan);
    }
    pending_.erase(pending);
}

void ProcNetCollector::FlushAllPendingLocked() {
    std::vector<uint32_t> pids;
    pids.reserve(pending_.size());
    for (const auto& item : pending_) pids.push_back(item.first);
    for (uint32_t pid : pids) {
        const auto image = images_.find(pid);
        FlushPendingLocked(pid, image != images_.end()
            ? image->second
            : "unknown:" + std::string(sessionName_.begin(), sessionName_.end()) + ":" + std::to_string(pid));
    }
}

std::string ProcNetCollector::NormalizeImagePath(const std::string& ntPath) const {
    // ImageLoad reports \Device\HarddiskVolumeN\...; QueryFullProcessImageName reports C:\...
    // Map the volume prefix so both paths produce one identity.
    if (ntPath.rfind("\\Device\\", 0) != 0) return ntPath;
    size_t end = ntPath.find('\\', 8);
    if (end == std::string::npos) return ntPath;
    auto mapped = deviceMap_.find(ntPath.substr(0, end));
    if (mapped == deviceMap_.end()) return ntPath;
    return mapped->second + ntPath.substr(end);
}

void ProcNetCollector::OnData(uint32_t pid, uint32_t size, bool isSend, bool isLan, int64_t fileTime) {
    std::string identity = IdentityFor(pid, fileTime);
    if (identity.empty() && (pid == 0 || pid == 4)) identity = "System";  // cannot be opened by handle
    int64_t seconds = fileTime / 10000000 - 11644473600LL;
    int64_t hour = seconds - seconds % 3600;
    std::lock_guard<std::mutex> lk(mu_);
    if (!identity.empty()) {
        auto& record = history_[{hour, identity}];
        record.identity = identity; record.utcSeconds = hour; record.lanSplit = true;
        if (isSend) { record.up += size; if (isLan) record.upLan += size; }
        else        { record.down += size; if (isLan) record.downLan += size; }
    } else {
        if (pending_.size() > 10000 && !pending_.count(pid)) FlushAllPendingLocked();
        PendingHour& p = pending_[pid][hour];
        if (isSend) { p.up += size; if (isLan) p.upLan += size; }
        else        { p.down += size; if (isLan) p.downLan += size; }
    }
    ProcTraffic& t = acc_[pid];
    if (isSend) (isLan ? t.upLan : t.upWan) += size;
    else        (isLan ? t.downLan : t.downWan) += size;
}

std::vector<TrafficDelta> ProcNetCollector::DrainHistory() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TrafficDelta> result;
    for (const auto& item : history_) result.push_back(item.second);
    history_.clear();
    return result;
}

std::unordered_map<uint32_t, ProcTraffic> ProcNetCollector::Drain() {
    std::unordered_map<uint32_t, ProcTraffic> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.swap(acc_);
    return out;
}

static std::vector<char> MakeProperties(size_t& total) {
    const size_t nameBytes = 1024;
    total = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
    std::vector<char> buf(total, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(total);
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC timestamps
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->FlushTimer = 1;  // seconds; default delivers events in coarse batches
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buf;
}

// A session whose owner was force-killed never reaches Stop(); reap it so the
// kernel logger does not keep accumulating events nobody drains.
static void SweepOrphanSessions() {
    constexpr ULONG kMaxSessions = 64;
    constexpr size_t kNameBytes = 1024;
    const size_t stride = sizeof(EVENT_TRACE_PROPERTIES) + kNameBytes;
    std::vector<char> storage(stride * kMaxSessions, 0);
    std::vector<EVENT_TRACE_PROPERTIES*> props(kMaxSessions);
    for (ULONG i = 0; i < kMaxSessions; ++i) {
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data() + i * stride);
        p->Wnode.BufferSize = static_cast<ULONG>(stride);
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props[i] = p;
    }
    ULONG count = 0;
    if (QueryAllTracesW(props.data(), kMaxSessions, &count) != ERROR_SUCCESS) return;

    const std::wstring prefix = L"TrafficMon-";
    for (ULONG i = 0; i < count && i < kMaxSessions; ++i) {
        auto* p = props[i];
        const wchar_t* name = reinterpret_cast<const wchar_t*>(
            reinterpret_cast<const char*>(p) + p->LoggerNameOffset);
        const std::wstring session(name);
        if (session.compare(0, prefix.size(), prefix) != 0) continue;

        const size_t dash = session.find(L'-', prefix.size());
        if (dash == std::wstring::npos) continue;
        const DWORD pid = static_cast<DWORD>(std::wcstoul(session.c_str() + prefix.size(), nullptr, 10));
        if (pid == 0 || pid == GetCurrentProcessId()) continue;

        HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (owner) {
            const DWORD wait = WaitForSingleObject(owner, 0);
            CloseHandle(owner);
            if (wait == WAIT_TIMEOUT) continue;  // owner still running
        }

        size_t total = 0;
        std::vector<char> buf = MakeProperties(total);
        ControlTraceW(0, name, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data()),
                      EVENT_TRACE_CONTROL_STOP);
    }
}

bool ProcNetCollector::Start(std::wstring& error) {
    SweepOrphanSessions();
    deviceMap_.clear();
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const wchar_t letter[3] = {static_cast<wchar_t>(L'A' + i), L':', L'\0'};
        wchar_t target[1024];
        const DWORD length = QueryDosDeviceW(letter, target, 1024);
        if (length) {
            const size_t chars = std::wcslen(target);
            deviceMap_[ToUtf8(target, static_cast<int>(chars))] =
                std::string(1, static_cast<char>('A' + i)) + ":";
        }
    }

    size_t total = 0;
    std::vector<char> buf = MakeProperties(total);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());

    TRACEHANDLE session = 0;
    sessionName_ = L"TrafficMon-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    ULONG rc = StartTraceW(&session, sessionName_.c_str(), props);
    if (rc != ERROR_SUCCESS) {
        error = L"StartTrace failed, error " + std::to_wstring(rc) +
                (rc == ERROR_ACCESS_DENIED ? L" (administrator required)" : L"");
        return false;
    }
    sessionHandle_ = session;

    rc = EnableTraceEx2(session, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, kKeywordIPv4 | kKeywordIPv6, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        error = L"EnableTraceEx2 failed, error " + std::to_wstring(rc);
        Stop();
        return false;
    }

    // Identity cache is an optimization: without it short-lived processes degrade to
    // "unknown", but byte accounting still works, so a failure here is not fatal.
    processProviderEnabled_ =
        EnableTraceEx2(session, &kKernelProcess, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                       TRACE_LEVEL_INFORMATION, kKeywordProcess | kKeywordImage, 0, 0, nullptr) == ERROR_SUCCESS;

    SeedIdentities();  // before ProcessTrace starts, so no callback races with this map

    EVENT_TRACE_LOGFILEW lf;
    std::memset(&lf, 0, sizeof(lf));
    lf.LoggerName = sessionName_.data();
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = EventCallback;
    lf.Context = this;

    TRACEHANDLE th = OpenTraceW(&lf);
    if (th == INVALID_PROCESSTRACE_HANDLE) {
        error = L"OpenTrace failed, error " + std::to_wstring(GetLastError());
        Stop();
        return false;
    }
    traceHandle_ = th;
    running_ = true;

    worker_ = std::thread([th]() {
        TRACEHANDLE h = th;
        ProcessTrace(&h, 1, nullptr, nullptr);  // blocks until the session is stopped
    });
    return true;
}

void ProcNetCollector::Stop() {
    if (sessionHandle_ || running_) {
        size_t total = 0;
        std::vector<char> buf = MakeProperties(total);
        ControlTraceW(sessionHandle_, nullptr, reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data()),
                      EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = 0;
    }
    if (worker_.joinable()) worker_.join();
    if (traceHandle_) {
        CloseTrace(traceHandle_);
        traceHandle_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        FlushAllPendingLocked();  // no ProcessStop arrived before shutdown
        identities_.clear();
        images_.clear();
        started_.clear();
    }
    running_ = false;
}
