#include <Geode/loader/Mod.hpp>
#include <Geode/utils/cocos.hpp>
#include "../DevTools.hpp"
#include "../ImGui.hpp"
#include <chrono>
#include <algorithm>
#include <span>
#include <fmt/chrono.h>

using namespace geode::prelude;

#if defined(GEODE_IS_WINDOWS)

#include <Windows.h>

bool canReadAddr(uintptr_t addr, size_t size) {
    if (addr <= 0x10000) return false;
    // TODO: doesnt check size.. though shouldnt matter most of the time
    // https://stackoverflow.com/a/35576777/9124836
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi))) {
        DWORD mask = (PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY);
        bool isBadRead = !(mbi.Protect & mask);
        // check the page is not a guard page
        if (mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS))
            isBadRead = true;

        return !isBadRead;
    }
    return false;
}

#elif defined(GEODE_IS_ANDROID)

#include <sys/mman.h>
#include <unistd.h>

auto const& getReadableAddresses() {
    using namespace std::chrono_literals;
    static std::vector<std::pair<uintptr_t, uintptr_t>> cache;
    // static auto lastCheck = std::chrono::high_resolution_clock::now();
    // auto now = std::chrono::high_resolution_clock::now();
    if (cache.empty()) {
        cache.clear();
        std::ifstream mappings("/proc/self/maps");
        std::string line;
        while (std::getline(mappings, line)) {
            uintptr_t start, end;
            char flags[4];
            std::sscanf(line.c_str(), "%" PRIxPTR "-%" PRIxPTR " %4c", &start, &end, flags);
            if (flags[0] == 'r') {
                cache.push_back({ start, end });
            }
        }
    }
    return cache;
}

bool canReadAddr(uintptr_t addr, size_t size) {
    if (addr <= 0x10000) return false;
#ifdef GEODE_IS_ANDROID64
    // if ((addr & 0xFF00000000000000) == 0) return false;
    addr = addr & ~(0xFF00000000000000);
#elif defined(GEODE_IS_ANDROID32)
    if (addr >= 0xFFFFF000) return false;
#endif

    // check with msync first

    // get page size
    static const size_t pageSize = sysconf(_SC_PAGESIZE);
    // find the page base
    // god this is nasty
    void* base = (void*)((((size_t)addr) / pageSize) * pageSize);
    if (msync(base, pageSize, MS_ASYNC) != 0)
        return false;

    // sometimes msync can return success even on an invalid address,
    // we hope that map parsing will catch that.

    auto const& mappings = getReadableAddresses();
    auto value = std::make_pair(addr, addr + size);
    // get the largest start which is <= addr
    auto it = std::upper_bound(mappings.rbegin(), mappings.rend(), value, [](auto const& a, auto const& b) {
        return a.first >= b.first;
    });
    if (it == mappings.rend()) return false;
    // it->first is already known to be <= addr,
    // now just check the end
    return value.second < it->second;
}

#else

bool canReadAddr(uintptr_t addr, size_t size) {
    return false;
}

#endif

struct SafePtr {
    uintptr_t addr = 0;
    SafePtr(uintptr_t addr) : addr(addr) {}
    SafePtr(const void* addr) : addr(reinterpret_cast<uintptr_t>(addr)) {}

    bool operator==(void* ptr) const { return as_ptr() == ptr; }
    // breaks clang
    // operator bool() const { return addr != 0; }

    void* as_ptr() const { return reinterpret_cast<void*>(addr); }

    bool is_safe(int size) {
        return addr % 4 == 0 && canReadAddr(addr, size);
    }

    bool read_into(void* buffer, int size) {
        if (!is_safe(size)) return false;

        std::memcpy(buffer, as_ptr(), size);
        return true;
    }

    template <class T>
    T read() {
        T result{};
        read_into(&result, sizeof(result));
        return result;
    }

    template <class T>
    std::optional<T> read_opt() {
        T result;
        if (!read_into(&result, sizeof(result))) return std::nullopt;
        return result;
    }

    SafePtr read_ptr() {
        return SafePtr(this->read<uintptr_t>());
    }

    // read offset relative to base as a 32-bit int
    SafePtr read_ptr32() {
        auto res = this->read<uint32_t>();
        if (res == 0) return SafePtr(nullptr);

        // this is pretty dumb but idk how else i'd do it
        uintptr_t base = geode::base::get();
    #ifdef GEODE_IS_WINDOWS
        if (this->addr - geode::base::getCocos() < 0x200000) {
            base = geode::base::getCocos();
        }
    #endif
        return SafePtr(base + res);
    }

    SafePtr operator+(intptr_t offset) const {
        return SafePtr(addr + offset);
    }
    SafePtr operator-(intptr_t offset) const {
        return SafePtr(addr - offset);
    }

    std::string_view read_c_str(int max_size = 512) {
        if (!is_safe(max_size)) return "";
        auto* c_str = reinterpret_cast<const char*>(as_ptr());
        for (int i = 0; i < max_size; ++i) {
            if (c_str[i] == 0)
                return std::string_view(c_str, i);
        }
        return "";
    }
};

std::string_view demangle(std::string_view mangled) {
    static std::unordered_map<std::string_view, std::string> cached;
    auto it = cached.find(mangled);
    if (it != cached.end()) {
        return it->second;
    }
#if defined(GEODE_IS_WINDOWS)
    if (mangled.size() <= 4) {
        return mangled;
    }
    auto parts = utils::string::split(std::string(mangled.substr(4)), "@");
    std::string result;
    for (const auto& part : utils::ranges::reverse(parts)) {
        if (part.empty()) continue;
        if (!result.empty())
            result += "::";
        result += part;
    }
    return cached[mangled] = result;
#else
    std::string result;
    int status = 0;
    auto demangle = abi::__cxa_demangle(mangled.data(), 0, 0, &status);
    if (status == 0) {
        result = demangle;
    } else {
        result = std::string(mangled);
    }
    free(demangle);

    return cached[mangled] = result;
#endif
}

struct RttiInfo {
    SafePtr ptr;
    RttiInfo(SafePtr ptr) : ptr(ptr) {}

    std::optional<std::string_view> class_name() {
        // TODO: maybe cache from the typeinfo pointer?
    #if defined(GEODE_IS_WINDOWS)
        auto vtable = ptr.read_ptr();
        if (!vtable.addr) return {};

        auto rttiObj = (vtable - sizeof(void*)).read_ptr();
        if (!rttiObj.addr) return {};
        // always 1 ?
        auto signature = rttiObj.read<int>();
        // if (signature != 1) return {};
        auto rttiDescriptor = (rttiObj + sizeof(unsigned int) * 3).read_ptr32();
        if (!rttiDescriptor.addr) return {};
        return demangle((rttiDescriptor + sizeof(void*) * 2).read_c_str());
        // pretty sure its a valid object at this point, so this shouldnt crash :-)
        // return typeid(*reinterpret_cast<CCObject*>(ptr.as_ptr())).name();
    #else
        auto vtable = ptr.read_ptr();
        if (!vtable.addr) return {};
        auto typeinfo = (vtable - sizeof(void*)).read_ptr();
        if (!typeinfo.addr) return {};
        auto typeinfoName = (typeinfo + sizeof(void*)).read_ptr();
        if (!typeinfoName.addr) return {};
        return demangle(typeinfoName.read_c_str());
    #endif
    }
};

std::optional<std::string_view> findStdString(SafePtr ptr) {
#if defined(GEODE_IS_WINDOWS)
    // scan for std::string (msvc)
    // char inline_data[16];
    // size_t size; + 16
    // size_t capacity; + 16 + sizeof(void*)
    auto size = (ptr + 16).read<size_t>();
    auto capacity = (ptr + 16 + sizeof(void*)).read<size_t>();
    if (size > capacity || capacity < 15) return {};
    // dont care about ridiculous sizes (> 100mb)
    if (capacity > 1e8) return {};
    char* data = nullptr;
    if (capacity == 15) {
        data = reinterpret_cast<char*>(ptr.as_ptr());
    } else {
        data = reinterpret_cast<char*>(ptr.read_ptr().as_ptr());
    }
#elif defined(GEODE_IS_ANDROID)
    static gd::string emptyStdString;
    auto internalData = ptr.read_ptr();
    if (!internalData.addr) return {};
    auto size = (internalData - (3 * sizeof(void*))).read<size_t>();
    auto capacity = (internalData - (2 * sizeof(void*))).read<size_t>();
    auto refCount = (internalData - (1 * sizeof(void*))).read<int>();
    if (size > capacity || refCount < 0) return {};
    if (capacity > 1e8) return {};
    char* data = reinterpret_cast<char*>(internalData.as_ptr());
    if (size == 0 && capacity == 0 && data != emptyStdString.data()) return {};
#else
    char* data;
    size_t size, capacity;
    return {};
#endif
    if (data == nullptr || !SafePtr(data).is_safe(capacity)) return {};
    // quick null term check
    if (data[size] != 0) return {};
    if (strlen(data) != size) return {};
    return std::string_view(data, size);
}

enum class TextType {
    Pointer,
    Array,
    Dictionary,
    String,
    Raw
};

struct TextInfo {
    TextType type = TextType::Raw;
    std::string str = "";
    std::string data = "";
    void* ptr = nullptr;
    CCObject** arrayPtr = nullptr;
    Ref<CCArray> array = nullptr;
    CCDictElement* dictPtr = nullptr;
    Ref<CCDictionary> dict = nullptr;
};

void DevTools::drawMemory() {
    using namespace std::chrono_literals;
    static auto lastRender = std::chrono::high_resolution_clock::now();

    static char buffer[256] = {'0', '\0'};
    bool changed = ImGui::InputText("Addr", buffer, sizeof(buffer));
    ImGui::SameLine();
    if (ImGui::Button("Paste")) {
        auto str = string::trim(clipboard::read());
        auto stripped = false;
        if (string::startsWith(str, "0x")) {
            str = str.substr(2);
            stripped = true;
        }
        if (std::all_of(str.begin(), str.end(), [](char c) {
            return std::isxdigit(c);
        })) {
            if (stripped) str = "0x" + str;
            std::memcpy(buffer, str.c_str(), str.size() + 1);
            changed = true;
        }
    }
    static int size = 0x100;
    changed |= ImGui::DragInt("Size", &size, 16.f, 0, 0, "%x");
    if (size < 4) {
        size = 4;
    }
    static bool showRawBytes = false;
    changed |= ImGui::Checkbox("Show raw bytes", &showRawBytes);

    if (ImGui::Button("Selected Node")) {
        auto str = fmt::format("{}", fmt::ptr(m_selectedNode.data()));
        std::memcpy(buffer, str.c_str(), str.size() + 1);
        changed = true;
    }
    ImGui::SameLine();
    changed |= ImGui::Button("Refresh");

    auto const timeNow = std::chrono::high_resolution_clock::now();

    // if (timeNow - lastRender < 0.5s) return;
    // lastRender = timeNow;

    uintptr_t addr = 0;
    try {
        addr = std::stoull(buffer, nullptr, 16);
    } catch (...) {}

    static std::vector<std::string> textSaving;
    if (ImGui::Button("Save to file")) {
        auto timeEpoch = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        auto name = fmt::format("Memory Dump {:%Y-%m-%d %H.%M.%S}.txt", fmt::localtime(timeEpoch));
        (void) utils::file::writeString(Mod::get()->getSaveDir() / name, fmt::to_string(fmt::join(textSaving, "\n")));
    }

    static std::vector<std::string> texts;
    static std::vector<TextInfo> textInfo;
    if (changed) {
        texts.clear();
        textSaving.clear();
        textInfo.clear();
        for (int offset = 0; offset < size; offset += sizeof(void*)) {
            SafePtr ptr = addr + offset;
            RttiInfo info(ptr.read_ptr());
            auto name = info.class_name();
            if (name) {
                auto voidPtr = reinterpret_cast<void**>(ptr.as_ptr());
                auto objectPtr = reinterpret_cast<CCObject*>(*voidPtr);
                auto formattedPtr = fmt::ptr(*voidPtr);
                if (auto arr = typeinfo_cast<CCArray*>(objectPtr)) {
                    texts.push_back(fmt::format("[{:04x}] cocos2d::CCArray ({}, size {}, data {})",
                        offset, formattedPtr, arr->data->num, fmt::ptr(arr->data->arr)));
                    textSaving.push_back(fmt::format("{:x}: a cocos2d::CCArray ({}, size {}, data {})",
                        offset, formattedPtr, arr->data->num, fmt::ptr(arr->data->arr)));
                    textInfo.push_back({
                        .type = TextType::Array,
                        .ptr = *voidPtr,
                        .arrayPtr = arr->data->arr,
                        .array = arr
                    });
                } else if (auto dict = typeinfo_cast<CCDictionary*>(objectPtr)) {
                    texts.push_back(fmt::format("[{:04x}] cocos2d::CCDictionary ({}, size {}, data {})",
                        offset, formattedPtr, HASH_COUNT(dict->m_pElements), fmt::ptr(dict->m_pElements)));
                    textSaving.push_back(fmt::format("{:x}: d cocos2d::CCDictionary ({}, size {}, data {})",
                        offset, formattedPtr, HASH_COUNT(dict->m_pElements), fmt::ptr(dict->m_pElements)));
                    textInfo.push_back({
                        .type = TextType::Dictionary,
                        .ptr = *voidPtr,
                        .dictPtr = dict->m_pElements,
                        .dict = dict
                    });
                } else {
                    auto nodeID = std::string();
                    auto foundID = std::string();
                    auto type = "p";
                    if (auto node = typeinfo_cast<CCNode*>(objectPtr)) {
                        foundID = node->getID();
                        if (!foundID.empty()) nodeID = fmt::format(" \"{}\"", foundID);
                        type = "n";
                    }
                    texts.push_back(fmt::format("[{:04x}] {} ({}){}", offset, *name, formattedPtr, nodeID));
                    textSaving.push_back(fmt::format("{:x}: {} {} ({}){}", offset, type, *name, formattedPtr, nodeID));
                    textInfo.push_back({
                        .type = TextType::Pointer,
                        .ptr = *voidPtr
                    });
                }
            } else if (auto maybeStr = findStdString(ptr); maybeStr) {
                auto str = maybeStr->substr(0, 30);
                // escapes new lines and stuff for me :3
                if (auto fmted = matjson::Value(std::string(str)).dump(0)) {
                    auto fmtedStr = fmted.unwrap();
                    texts.push_back(fmt::format("[{:04x}] maybe std::string {}, {}", offset, maybeStr->size(), fmtedStr));
                    textSaving.push_back(fmt::format("{:x}: s {}", offset, fmtedStr));
                    textInfo.push_back({
                        .type = TextType::String,
                        .str = std::string(maybeStr->data())
                    });
                }
            } else if (auto valueOpt = ptr.read_opt<uintptr_t>()) {
                auto value = *valueOpt;
                auto data = std::span(reinterpret_cast<uint8_t*>(&value), sizeof(void*));
                if (showRawBytes) {
                    texts.push_back(fmt::format("[{:04x}] raw {:02x}", offset, fmt::join(data, " ")));
                    textInfo.push_back({
                        .type = TextType::Raw,
                        .data = fmt::format("{:02x}", fmt::join(data, " ")),
                        .ptr = reinterpret_cast<void*>(value)
                    });
                }
                textSaving.push_back(fmt::format("{:x}: r {:02x}", offset, fmt::join(data, " ")));
            }
        }
    }

    ImGui::PushFont(m_monoFont);
    for (size_t i = 0; i < texts.size(); ++i) {
        const auto& text = texts[i];
        const auto& info = textInfo[i];
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        if (info.type == TextType::Raw) {
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy Data##{}", i).c_str())) {
                clipboard::write(info.data.c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy As Pointer##{}", i).c_str())) {
                clipboard::write(fmt::format("{}", fmt::ptr(info.ptr)).c_str());
            }
        }
        else if (info.type != TextType::String) {
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy Pointer##{}", i).c_str())) {
                clipboard::write(fmt::format("{}", fmt::ptr(info.ptr)).c_str());
            }
        }

        if (info.type == TextType::String && !info.str.empty()) {
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy String##{}", i).c_str())) {
                clipboard::write(info.str.c_str());
            }
        }
        else if (info.type == TextType::Array) {
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy Array Address##{}", i).c_str())) {
                clipboard::write(fmt::format("{}", fmt::ptr(info.arrayPtr)).c_str());
            }
            if (info.array && info.array->data->num > 0) {
                ImGui::SameLine();
                if (ImGui::Button(fmt::format("+##array{}", i).c_str())) {
                    selectArray(info.array);
                }
            }
        }
        else if (info.type == TextType::Dictionary) {
            ImGui::SameLine();
            if (ImGui::Button(fmt::format("Copy Dictionary Address##{}", i).c_str())) {
                clipboard::write(fmt::format("{}", fmt::ptr(info.dictPtr)).c_str());
            }
            if (info.dict && HASH_COUNT(info.dict->m_pElements) > 0) {
                ImGui::SameLine();
                if (ImGui::Button(fmt::format("+##dict{}", i).c_str())) {
                    selectDictionary(info.dict);
                }
            }
        }
    }
    ImGui::PopFont();
}

void DevTools::drawArray() {
    if (!m_selectedArr) return;

    ImGui::PushFont(m_monoFont);
    for (int i = 0; i < m_selectedArr->data->num; ++i) {
        SafePtr ptr = reinterpret_cast<uintptr_t>(m_selectedArr->data->arr) + i * sizeof(void*);
        RttiInfo info(ptr.read_ptr());
        auto name = info.class_name();
        if (name) {
            auto objectPtr = reinterpret_cast<CCObject**>(ptr.as_ptr());
            auto formattedPtr = fmt::ptr(*objectPtr);
            if (auto arr = typeinfo_cast<CCArray*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCArray ({}, size {}, data {})",
                    i, formattedPtr, arr->data->num, fmt::ptr(arr->data->arr)).c_str());
            } else if (auto dict = typeinfo_cast<CCDictionary*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCDictionary ({}, size {}, data {})",
                    i, formattedPtr, HASH_COUNT(dict->m_pElements), fmt::ptr(dict->m_pElements)).c_str());
            } else if (auto boolean = typeinfo_cast<CCBool*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCBool ({}) {}",
                    i, formattedPtr, boolean->getValue()).c_str());
            } else if (auto string = typeinfo_cast<CCString*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCString ({}) {}",
                    i, formattedPtr, matjson::Value(std::string(string->getCString()).substr(0, 30)).dump(0).unwrapOr("\"\"")).c_str());
            } else if (auto integer = typeinfo_cast<CCInteger*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCInteger ({}) {}",
                    i, formattedPtr, integer->getValue()).c_str());
            } else if (auto floating = typeinfo_cast<CCFloat*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCFloat ({}) {}",
                    i, formattedPtr, floating->getValue()).c_str());
            } else if (auto doubleprec = typeinfo_cast<CCDouble*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCDouble ({}) {}",
                    i, formattedPtr, doubleprec->getValue()).c_str());
            } else {
                auto nodeID = std::string();
                if (auto node = typeinfo_cast<CCNode*>(*objectPtr)) {
                    auto foundID = node->getID();
                    if (!foundID.empty()) nodeID = fmt::format(" \"{}\"", foundID);
                }
                ImGui::TextUnformatted(fmt::format("[{}] {} ({}){}", i, *name, formattedPtr, nodeID).c_str());
            }
        } else {
            ImGui::TextUnformatted(fmt::format("[{}] unknown ({})", i, fmt::ptr(*reinterpret_cast<void**>(ptr.as_ptr()))).c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(fmt::format("Copy Pointer##{}", i).c_str())) {
            clipboard::write(fmt::format("{}", fmt::ptr(*reinterpret_cast<void**>(ptr.as_ptr()))).c_str());
        }
    }
    ImGui::PopFont();
}

void DevTools::drawDictionary() {
    if (!m_selectedDict) return;

    CCDictElement* elt, *tmp;
    ImGui::PushFont(m_monoFont);
    auto i = 0;
    HASH_ITER(hh, m_selectedDict->m_pElements, elt, tmp) {
        auto key = m_selectedDict->m_eDictType == cocos2d::CCDictionary::kCCDictStr ? elt->getStrKey() : fmt::format("{}", elt->getIntKey());
        SafePtr ptr = reinterpret_cast<uintptr_t>(elt) + 256 + sizeof(intptr_t);
        RttiInfo info(ptr.read_ptr());
        auto name = info.class_name();
        if (name) {
            auto objectPtr = reinterpret_cast<CCObject**>(ptr.as_ptr());
            auto formattedPtr = fmt::ptr(*objectPtr);
            if (auto arr = typeinfo_cast<CCArray*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCArray ({}, size {}, data {})",
                    key, formattedPtr, arr->data->num, fmt::ptr(arr->data->arr)).c_str());
            } else if (auto dict = typeinfo_cast<CCDictionary*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCDictionary ({}, size {}, data {})",
                    key, formattedPtr, HASH_COUNT(dict->m_pElements), fmt::ptr(dict->m_pElements)).c_str());
            } else if (auto boolean = typeinfo_cast<CCBool*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCBool ({}) {}",
                    key, formattedPtr, boolean->getValue()).c_str());
            } else if (auto string = typeinfo_cast<CCString*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCString ({}) {}",
                    key, formattedPtr, matjson::Value(std::string(string->getCString()).substr(0, 30)).dump(0).unwrapOr("\"\"")).c_str());
            } else if (auto integer = typeinfo_cast<CCInteger*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCInteger ({}) {}",
                    key, formattedPtr, integer->getValue()).c_str());
            } else if (auto floating = typeinfo_cast<CCFloat*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCFloat ({}) {}",
                    key, formattedPtr, floating->getValue()).c_str());
            } else if (auto doubleprec = typeinfo_cast<CCDouble*>(*objectPtr)) {
                ImGui::TextUnformatted(fmt::format("[{}] cocos2d::CCDouble ({}) {}",
                    key, formattedPtr, doubleprec->getValue()).c_str());
            } else {
                auto nodeID = std::string();
                if (auto node = typeinfo_cast<CCNode*>(*objectPtr)) {
                    auto foundID = node->getID();
                    if (!foundID.empty()) nodeID = fmt::format(" \"{}\"", foundID);
                }
                ImGui::TextUnformatted(fmt::format("[{}] {} ({}){}", key, *name, formattedPtr, nodeID).c_str());
            }
        } else {
            ImGui::TextUnformatted(fmt::format("[{}] unknown ({})", key, fmt::ptr(*reinterpret_cast<void**>(ptr.as_ptr()))).c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(fmt::format("Copy Pointer##{}", i++).c_str())) {
            clipboard::write(fmt::format("{}", fmt::ptr(*reinterpret_cast<void**>(ptr.as_ptr()))).c_str());
        }
    }
    ImGui::PopFont();
}
