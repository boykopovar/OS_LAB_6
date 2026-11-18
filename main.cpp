#include <windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<unsigned char>;
using Name = std::pair<std::wstring, std::wstring>;
using Value = std::pair<DWORD, Bytes>;
using Keys = std::map<std::wstring, bool>;
using Values = std::map<Name, Value>;

struct Snapshot {
    Keys Sections;
    Values Entries;
};

int Number(std::string_view text, int min, int max) {
    int value;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < min || value > max) throw std::invalid_argument("Invalid argument");
    return value;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::system_error(GetLastError(), std::system_category(), "WideCharToMultiByte");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, result.data(), size, nullptr, nullptr))
        throw std::system_error(GetLastError(), std::system_category(), "WideCharToMultiByte");
    return result;
}

void Check(LSTATUS status, const char* operation, const std::wstring& object) {
    if (status == ERROR_SUCCESS) return;
    std::cerr << "object=" << Utf8(object) << " code=" << status << '\n';
    throw std::system_error(status, std::system_category(), operation);
}

class Key {
public:
    Key(HKEY parent, const std::wstring& path, bool create) {
        if (create) {
            DWORD disposition;
            Check(RegCreateKeyExW(parent, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &handle, &disposition), "RegCreateKeyExW", path);
            if (disposition != REG_CREATED_NEW_KEY) {
                Check(RegCloseKey(handle), "RegCloseKey", path);
                throw std::runtime_error("Registry key already exists");
            }
        } else Check(RegOpenKeyExW(parent, path.c_str(), 0, KEY_READ, &handle), "RegOpenKeyExW", path);
    }
    ~Key() { RegCloseKey(handle); }
    HKEY Get() const { return handle; }
private:
    HKEY handle{};
};

void Capture(HKEY key, const std::wstring& path, Snapshot& result) {
    result.Sections.emplace(path, true);
    DWORD subkeys, maxKey, values, maxName;
    Check(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, &maxKey, nullptr, &values, &maxName, nullptr, nullptr, nullptr), "RegQueryInfoKeyW", path);
    std::vector<wchar_t> buffer(static_cast<std::size_t>(1) + std::max(maxKey, maxName));
    for (DWORD i = 0; i < values; ++i) {
        DWORD length = static_cast<DWORD>(buffer.size());
        Check(RegEnumValueW(key, i, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr), "RegEnumValueW", path);
        std::wstring name(buffer.data(), length);
        DWORD type, size;
        Check(RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size), "RegQueryValueExW", path);
        Bytes data(size);
        Check(RegQueryValueExW(key, name.c_str(), nullptr, &type, data.data(), &size), "RegQueryValueExW", path);
        if (size != data.size()) throw std::runtime_error("Registry data changed during snapshot");
        result.Entries.emplace(Name{path, name}, Value{type, std::move(data)});
    }
    for (DWORD i = 0; i < subkeys; ++i) {
        DWORD length = static_cast<DWORD>(buffer.size());
        Check(RegEnumKeyExW(key, i, buffer.data(), &length, nullptr, nullptr, nullptr, nullptr), "RegEnumKeyExW", path);
        const std::wstring name(buffer.data(), length);
        const Key child(key, name, false);
        Capture(child.Get(), path.empty() ? name : path + L"\\" + name, result);
    }
}

void WriteName(std::ostream& output, const std::wstring& name) {
    output << ' ' << name.size();
    for (auto character : name) output << ' ' << static_cast<unsigned>(character);
}

std::wstring ReadName(std::istream& input) {
    std::size_t size;
    if (!(input >> size) || size > 32767) throw std::runtime_error("Invalid snapshot name size");
    std::wstring name(size, L'\0');
    for (auto& character : name) {
        unsigned unit;
        if (!(input >> unit) || unit == 0 || unit > 65535) throw std::runtime_error("Invalid snapshot name character");
        character = static_cast<wchar_t>(unit);
    }
    return name;
}

void Save(const Snapshot& snapshot, const std::filesystem::path& path) {
    std::ofstream file(path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << "OSISP6 1\n";
    for (const auto& [name, unused] : snapshot.Sections) {
        file << 'K';
        WriteName(file, name);
        file << '\n';
    }
    for (const auto& [name, value] : snapshot.Entries) {
        file << 'V';
        WriteName(file, name.first);
        WriteName(file, name.second);
        file << ' ' << value.first << ' ' << value.second.size();
        for (auto byte : value.second) file << ' ' << static_cast<unsigned>(byte);
        file << '\n';
    }
    file << "END\n";
    file.close();
}

Snapshot Load(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string magic;
    int version;
    if (!(file >> magic >> version) || magic != "OSISP6" || version != 1) throw std::runtime_error("Invalid snapshot header");
    Snapshot result;
    std::string record;
    while (file >> record && record != "END") {
        auto key = ReadName(file);
        if (record == "K") {
            if (!result.Sections.emplace(key, true).second) throw std::runtime_error("Duplicate key");
        } else if (record == "V") {
            auto name = ReadName(file);
            DWORD type;
            std::size_t size;
            if (!result.Sections.contains(key) || !(file >> type >> size) || size > 1048576) throw std::runtime_error("Invalid snapshot value");
            Bytes bytes(size);
            for (auto& byte : bytes) {
                unsigned value;
                if (!(file >> value) || value > 255) throw std::runtime_error("Invalid snapshot byte");
                byte = static_cast<unsigned char>(value);
            }
            if (!result.Entries.emplace(Name{key, name}, Value{type, std::move(bytes)}).second) throw std::runtime_error("Duplicate value");
        } else throw std::runtime_error("Invalid snapshot record");
    }
    if (record != "END" || !result.Sections.contains(L"") || file >> record || !file.eof()) throw std::runtime_error("Invalid snapshot end");
    return result;
}

using Changes = std::array<std::vector<Name>, 5>;

Changes Compare(const Snapshot& before, const Snapshot& after) {
    Changes changes;
    for (const auto& [key, unused] : after.Sections) if (!before.Sections.contains(key)) changes[0].emplace_back(key, L"");
    for (const auto& [key, unused] : before.Sections) if (!after.Sections.contains(key)) changes[1].emplace_back(key, L"");
    for (const auto& [name, value] : after.Entries) {
        const auto previous = before.Entries.find(name);
        if (previous == before.Entries.end()) changes[2].push_back(name);
        else if (previous->second != value) changes[4].push_back(name);
    }
    for (const auto& [name, unused] : before.Entries) if (!after.Entries.contains(name)) changes[3].push_back(name);
    return changes;
}

void Log(const wchar_t* operation, const std::wstring& object, Clock::time_point start) {
    std::cout << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() << " us " << Utf8(operation) << " " << Utf8(object) << " status=OK\n";
}

void Print(const Changes& changes, Clock::time_point start) {
    const std::array labels{L"KEY_ADDED", L"KEY_REMOVED", L"VALUE_ADDED", L"VALUE_REMOVED", L"VALUE_CHANGED"};
    for (std::size_t i = 0; i < changes.size(); ++i) {
        for (const auto& [key, name] : changes[i]) Log(labels[i], L"key=" + key + (i >= 2 ? L" value=" + name : L""), start);
        std::cout << Utf8(labels[i]) << " count=" << changes[i].size() << '\n';
    }
}

void SetValue(HKEY root, const std::wstring& name, DWORD value) {
    Check(RegSetValueExW(root, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)), "RegSetValueExW", name);
}

std::wstring Root(int student, int run) {
    return L"Software\\OSiSP\\Lab06\\Student_" + std::to_wstring(student) + L"\\Run_" + std::to_wstring(run);
}

void Demo(int student, int run, int count, const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto path = Root(student, run);
    const auto start = Clock::now();
    if (std::filesystem::exists(first) || std::filesystem::exists(second)
        || std::filesystem::absolute(first).lexically_normal() == std::filesystem::absolute(second).lexically_normal()) throw std::invalid_argument("Snapshot paths must be distinct new files");
    Changes expected;
    {
        const Key root(HKEY_CURRENT_USER, path, true);
        try {
            Log(L"REG_CREATE", path, start);
            std::mt19937 random(1000 + student * 97);
            for (int i = 0; i < count; ++i) {
                const auto name = std::to_wstring(i);
                if (i % 5 == 1) {
                    const Key child(root.Get(), name, true);
                }
                if (i % 5 >= 3) SetValue(root.Get(), name, random() & 65535);
            }
            Snapshot before;
            Capture(root.Get(), L"", before);
            Save(before, first);
            Log(L"REG_SNAPSHOT", first.wstring(), start);
            for (int i = 0; i < count; ++i) {
                const auto name = std::to_wstring(i);
                const auto type = i % 5;
                expected[type].emplace_back(type < 2 ? name : L"", type < 2 ? L"" : name);
                if (type == 0) {
                    const Key child(root.Get(), name, true);
                }
                if (type == 1) Check(RegDeleteKeyW(root.Get(), name.c_str()), "RegDeleteKeyW", name);
                if (type == 2 || type == 4) SetValue(root.Get(), name, 65536 + (random() & 65535));
                if (type == 3) Check(RegDeleteValueW(root.Get(), name.c_str()), "RegDeleteValueW", name);
            }
            Log(L"REG_MUTATE", path, start);
            Snapshot after;
            Capture(root.Get(), L"", after);
            Save(after, second);
            Log(L"REG_SNAPSHOT", second.wstring(), start);
        } catch (...) {
            Check(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()), "RegDeleteTreeW", path);
            throw;
        }
    }
    Check(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()), "RegDeleteTreeW", path);
    Log(L"REG_DELETE", path, start);
    const auto before = Load(first);
    const auto after = Load(second);
    const auto comparison = Clock::now();
    const auto actual = Compare(before, after);
    const auto compareUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - comparison).count();
    for (auto& category : expected) std::sort(category.begin(), category.end());
    if (actual != expected) throw std::logic_error("Unexpected snapshot differences");
    Print(actual, start);
    std::cout << "verified=" << count << " compare_us=" << compareUs << '\n';
}

int main(int argc, char** argv) try {
    if (argc < 2) throw std::invalid_argument("Usage: registry demo student new-directory | demo student run changes before after | save student run file | compare before after");
    const std::string_view mode = argv[1];
    const auto start = Clock::now();
    if (mode == "compare" && argc == 4) {
        const auto before = Load(argv[2]);
        const auto after = Load(argv[3]);
        Print(Compare(before, after), start);
    } else if (mode == "save" && argc == 5) {
        const auto path = Root(Number(argv[2], 0, 999), Number(argv[3], 1, 999999999));
        const Key root(HKEY_CURRENT_USER, path, false);
        Snapshot snapshot;
        Capture(root.Get(), L"", snapshot);
        Save(snapshot, argv[4]);
        Log(L"REG_SNAPSHOT", path, start);
    } else if (mode == "demo" && argc == 7) {
        const int student = Number(argv[2], 0, 999);
        Demo(student, Number(argv[3], 1, 999999999), Number(argv[4], 5, 10000), argv[5], argv[6]);
    } else if (mode == "demo" && argc == 4) {
        const int student = Number(argv[2], 0, 999);
        const std::filesystem::path directory = argv[3];
        if (!std::filesystem::create_directories(directory)) {
            throw std::invalid_argument("Choose a new demo output directory");
        }
        std::mt19937 random(std::random_device{}());
        int run = std::uniform_int_distribution<int>(100000000, 999999990)(random);
        for (const int count : {10, 50, 100}) {
            const auto suffix = std::to_string(count) + ".txt";
            Demo(student, run++, count, directory / ("before-" + suffix), directory / ("after-" + suffix));
        }
    } else throw std::invalid_argument("Invalid mode or arguments");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
