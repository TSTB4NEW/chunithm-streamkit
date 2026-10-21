// chunithm-streamkit — songdb.h
// 歌曲信息库：
//  1) 优先加载插件目录下的 music_db.tsv（UTF-8，Tab 分隔，无表头）：
//     id<TAB>title<TAB>artist<TAB>bpm<TAB>basic<TAB>advanced<TAB>expert<TAB>master<TAB>ultima<TAB>we
//     （后 6 列为各难度定数，可留空）
//  2) 若 TSV 不存在，自动在游戏目录寻找 music.xml 并导入（歌曲信息“自动读取”）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "state.h"
#include "util.h"
#include "log.h"

namespace csk {

class SongDb {
public:
    size_t size() const { return songs_.size(); }

    const SongInfo* FindById(uint32_t id) const {
        auto it = songs_.find(id);
        return it == songs_.end() ? nullptr : &it->second;
    }

    bool LoadTsv(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char line[8192];
        size_t n = 0;
        while (fgets(line, sizeof(line), f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#') continue;
            // 去 UTF-8 BOM
            if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
                (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
                s = s.substr(3);
            auto cols = split(s, '\t');
            if (cols.size() < 3) continue;
            SongInfo si;
            si.id = (uint32_t)parse_u64(cols[0]);
            si.title = cols[1];
            si.artist = cols[2];
            si.bpm = cols.size() > 3 ? parse_f64(cols[3]) : 0.0;
            for (size_t i = 0; i < 6; ++i) {
                if (cols.size() > 4 + i) si.diff_const[i] = parse_f64(cols[4 + i]);
            }
            if (cols.size() > 10) si.we_tag = cols[10];
            if (cols.size() > 10) si.we_tag = cols[10];
            if (cols.size() > 11) si.genre = cols[11];
            if (cols.size() > 12) si.version = cols[12];
            si.known = true;
            songs_[si.id] = std::move(si);
            ++n;
        }
        fclose(f);
        L().Info("music_db.tsv loaded: %zu songs", n);
        return n > 0;
    }

    // 点歌别名表：每行 id<TAB>别名[<TAB>别名…]
    bool LoadAliases(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        std::lock_guard<std::mutex> g(aliasMtx_);
        char line[4096];
        size_t n = 0;
        while (fgets(line, sizeof(line), f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#') continue;
            if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
                (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
                s = s.substr(3);
            auto cols = split(s, '\t');
            if (cols.size() < 2) continue;
            uint32_t id = (uint32_t)parse_u64(cols[0]);
            if (songs_.find(id) == songs_.end()) continue;
            auto& vec = aliases_[id];
            for (size_t i = 1; i < cols.size(); ++i) {
                std::string a = NormText(trim(cols[i]));
                if (a.empty()) continue;
                if (std::find(vec.begin(), vec.end(), a) != vec.end()) continue;
                vec.push_back(a);
                ++n;
            }
        }
        fclose(f);
        L().Info("music_alias.tsv loaded: %zu aliases", n);
        return n > 0;
    }

    // 合并落雪 /api/v0/chunithm/alias/list 的 JSON（不清空已有别名）
    size_t MergeAliasesJson(const std::string& json) {
        std::unordered_map<uint32_t, std::vector<std::string>> incoming;
        size_t p = 0;
        while ((p = json.find("\"song_id\"", p)) != std::string::npos) {
            p += 9;
            size_t colon = json.find(':', p);
            if (colon == std::string::npos) break;
            size_t q = colon + 1;
            while (q < json.size() && (json[q] < '0' || json[q] > '9')) ++q;
            uint32_t id = 0;
            while (q < json.size() && json[q] >= '0' && json[q] <= '9') {
                id = id * 10 + (uint32_t)(json[q] - '0');
                ++q;
            }
            size_t a = json.find("\"aliases\"", q);
            if (a == std::string::npos) break;
            size_t lb = json.find('[', a);
            if (lb == std::string::npos) break;
            size_t rb = json.find(']', lb);
            if (rb == std::string::npos) break;
            if (songs_.find(id) != songs_.end()) {
                auto& vec = incoming[id];
                size_t s = lb + 1;
                while (s < rb) {
                    size_t q1 = json.find('"', s);
                    if (q1 == std::string::npos || q1 > rb) break;
                    std::string val;
                    size_t q2 = q1 + 1;
                    while (q2 < rb && json[q2] != '"') {
                        char c = json[q2++];
                        if (c == '\\' && q2 < rb) {
                            char e = json[q2++];
                            if (e == 'n') val += '\n';
                            else if (e == 't') val += '\t';
                            else if (e == 'r') val += '\r';
                            else if (e == 'u' && q2 + 4 <= rb) q2 += 4;   // 跳过 \uXXXX
                            else val += e;
                        } else {
                            val += c;
                        }
                    }
                    s = (q2 < rb) ? q2 + 1 : rb;
                    std::string norm = NormText(trim(val));
                    if (!norm.empty() &&
                        std::find(vec.begin(), vec.end(), norm) == vec.end())
                        vec.push_back(norm);
                }
            }
            p = rb + 1;
        }

        std::lock_guard<std::mutex> g(aliasMtx_);
        size_t added = 0;
        for (auto& kv : incoming) {
            auto& dst = aliases_[kv.first];
            for (const auto& a : kv.second) {
                if (std::find(dst.begin(), dst.end(), a) == dst.end()) {
                    dst.push_back(a);
                    ++added;
                }
            }
        }
        return added;
    }

    size_t AliasCount() const {
        std::lock_guard<std::mutex> g(aliasMtx_);
        size_t n = 0;
        for (const auto& kv : aliases_) n += kv.second.size();
        return n;
    }

    static std::string VersionName(uint32_t code) {
        switch (code) {
        case 10000: return "CHUNITHM";
        case 10500: return "CHUNITHM PLUS";
        case 11000: return "AIR";
        case 11500: return "AIR PLUS";
        case 12000: return "AMAZON";
        case 12500: return "AMAZON PLUS";
        case 13000: return "CRYSTAL";
        case 13500: return "CRYSTAL PLUS";
        case 14000: return "PARADISE";
        case 14500: return "PARADISE LOST";
        case 15000: return "NEW";
        case 15500: return "NEW PLUS";
        case 16000: return "SUN";
        case 16500: return "SUN PLUS";
        case 17000: return "LUMINOUS";
        case 17500: return "LUMINOUS PLUS";
        default: return "V" + std::to_string(code);
        }
    }

    // 仅用于导入/测试。运行时不要调用：标题、曲师、定数、分类、版本
    // 一律以本地 music_db.tsv 为准；在线只允许补充别名。
    size_t MergeSongMetaJson(const std::string& json) {
        auto fieldString = [](const std::string& o, const char* key) {
            std::string pat = std::string("\"") + key + "\"";
            size_t k = o.find(pat);
            if (k == std::string::npos) return std::string();
            size_t c = o.find(':', k);
            if (c == std::string::npos) return std::string();
            size_t q = o.find('"', c);
            if (q == std::string::npos) return std::string();
            std::string out;
            for (++q; q < o.size(); ++q) {
                if (o[q] == '\\') {
                    if (q + 1 < o.size()) out += o[++q];
                } else if (o[q] == '"') {
                    break;
                } else {
                    out += o[q];
                }
            }
            return out;
        };
        auto fieldUint = [](const std::string& o, const char* key) {
            std::string pat = std::string("\"") + key + "\"";
            size_t k = o.find(pat);
            if (k == std::string::npos) return (uint32_t)0;
            size_t c = o.find(':', k);
            if (c == std::string::npos) return (uint32_t)0;
            uint32_t v = 0;
            bool started = false;
            for (size_t i = c + 1; i < o.size(); ++i) {
                char ch = o[i];
                if (ch >= '0' && ch <= '9') {
                    started = true;
                    v = v * 10 + (uint32_t)(ch - '0');
                } else if (started) {
                    break;
                } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                    break;
                }
            }
            return v;
        };

        size_t p = json.find("\"songs\":[");
        if (p == std::string::npos) return 0;
        p = json.find('{', p);
        size_t n = 0;
        std::lock_guard<std::mutex> g(aliasMtx_);
        while (p != std::string::npos && p < json.size()) {
            int depth = 0;
            bool inStr = false, esc = false;
            size_t e = p;
            for (; e < json.size(); ++e) {
                char c = json[e];
                if (inStr) {
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') inStr = false;
                } else if (c == '"') {
                    inStr = true;
                } else if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    if (--depth == 0) break;
                }
            }
            if (e >= json.size()) break;
            std::string obj = json.substr(p, e - p + 1);
            uint32_t id = fieldUint(obj, "id");
            std::string genre = fieldString(obj, "genre");
            uint32_t ver = fieldUint(obj, "version");
            auto it = songs_.find(id);
            if (it != songs_.end()) {
                if (!genre.empty()) it->second.genre = genre;
                if (ver > 0) {
                    it->second.version_code = ver;
                    it->second.version = VersionName(ver);
                }
                ++n;
            }
            p = json.find('{', e + 1);
        }
        return n;
    }

    std::vector<SongInfo> SnapshotSongs() const {
        std::lock_guard<std::mutex> g(aliasMtx_);
        std::vector<SongInfo> out;
        out.reserve(songs_.size());
        for (const auto& kv : songs_) out.push_back(kv.second);
        return out;
    }

    // 在游戏根目录下按候选路径寻找 music.xml 并尝试导入
    // 返回实际使用的文件路径（空 = 未找到）
    std::string TryImportXml(const std::string& gameRoot) {
        static const char* candidates[] = {
            "data/music/music.xml",
            "data/music.xml",
            "music/music.xml",
            "music.xml",
            "option/music.xml",
            "data/Music.xml",
            "Music.xml",
        };
        for (auto rel : candidates) {
            std::string p = gameRoot + "\\" + rel;
            if (ImportXml(p)) {
                L().Info("music.xml imported from %s (%zu songs)", p.c_str(), songs_.size());
                return p;
            }
        }
        return std::string();
    }

    bool ImportXml(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (64 << 20)) { fclose(f); return false; }
        std::string xml;
        xml.resize((size_t)sz);
        size_t rd = fread(&xml[0], 1, (size_t)sz, f);
        fclose(f);
        xml.resize(rd);

        size_t n = 0;
        size_t pos = 0;
        while (true) {
            size_t b = xml.find("<MusicData", pos);
            if (b == std::string::npos) break;
            size_t e = xml.find("</MusicData>", b);
            if (e == std::string::npos) break;
            std::string block = xml.substr(b, e - b);
            pos = e + 12;
            SongInfo si;
            if (!ParseMusicBlock(block, si)) continue;
            si.known = true;
            songs_[si.id] = std::move(si);
            ++n;
        }
        if (n == 0) return false;
        L().Info("music.xml parsed %zu entries", n);
        return true;
    }

    bool SaveTsv(const std::string& path) const {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        // UTF-8 BOM，便于 Excel 直接打开
        fwrite("\xEF\xBB\xBF", 1, 3, f);
        for (auto& kv : songs_) {
            const SongInfo& si = kv.second;
            fprintf(f, "%u\t%s\t%s\t%.2f", si.id, si.title.c_str(), si.artist.c_str(), si.bpm);
            for (int i = 0; i < 6; ++i) {
                if (si.diff_const[i] > 0)
                    fprintf(f, "\t%.2f", si.diff_const[i]);
                else
                    fputs("\t", f);
            }
            fputs("\r\n", f);
        }
        fclose(f);
        return true;
    }

    // 查询歌曲；难度超界或未知返回 diff_const=0 / known=true 的条目
    const SongInfo* Lookup(uint32_t musicId) const {
        auto it = songs_.find(musicId);
        return it == songs_.end() ? nullptr : &it->second;
    }

    // 点歌模糊匹配：标题精确 > 前缀 > 包含，其次曲师包含；纯数字按 ID。
    // 忽略大小写、空白与常见标点，避免「点歌 xxx」因为空格/符号匹配不到。
    struct SongMatch {
        const SongInfo* song = nullptr;
        int score = 0;
        size_t titleLen = 0;
    };

    static std::vector<uint32_t> Utf8Codes(const std::string& s) {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) {
                out.push_back(c);
                ++i;
            } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
                out.push_back(((uint32_t)(c & 0x1F) << 6) |
                              ((uint32_t)s[i + 1] & 0x3F));
                i += 2;
            } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
                out.push_back(((uint32_t)(c & 0x0F) << 12) |
                              (((uint32_t)s[i + 1] & 0x3F) << 6) |
                              ((uint32_t)s[i + 2] & 0x3F));
                i += 3;
            } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
                out.push_back(((uint32_t)(c & 0x07) << 18) |
                              (((uint32_t)s[i + 1] & 0x3F) << 12) |
                              (((uint32_t)s[i + 2] & 0x3F) << 6) |
                              ((uint32_t)s[i + 3] & 0x3F));
                i += 4;
            } else {
                out.push_back(c);
                ++i;
            }
        }
        return out;
    }

    static int EditDistance(const std::string& a, const std::string& b, int maxD) {
        std::vector<uint32_t> ca = Utf8Codes(a), cb = Utf8Codes(b);
        int la = (int)ca.size(), lb = (int)cb.size();
        int diff = la > lb ? la - lb : lb - la;
        if (diff > maxD) return maxD + 1;
        if (la == 0) return lb;
        if (lb == 0) return la;
        std::vector<int> prev((size_t)lb + 1), cur((size_t)lb + 1);
        for (int j = 0; j <= lb; ++j) prev[(size_t)j] = j;
        for (int i = 1; i <= la; ++i) {
            cur[0] = i;
            int rowMin = cur[0];
            for (int j = 1; j <= lb; ++j) {
                int cost = (ca[(size_t)i - 1] == cb[(size_t)j - 1]) ? 0 : 1;
                int v = std::min(std::min(prev[(size_t)j] + 1, cur[(size_t)j - 1] + 1),
                                 prev[(size_t)j - 1] + cost);
                cur[(size_t)j] = v;
                if (v < rowMin) rowMin = v;
            }
            if (rowMin > maxD) return maxD + 1;
            prev.swap(cur);
        }
        return prev[(size_t)lb];
    }

    std::vector<SongMatch> FindCandidates(const std::string& query, size_t maxN = 8) const {
        std::lock_guard<std::mutex> g(aliasMtx_);
        std::vector<SongMatch> out;
        std::string q = NormText(query);
        if (q.empty()) return out;
        int qChars = (int)Utf8Codes(q).size();
        bool fuzzyQ = qChars >= 3;
        int fuzzyMax = qChars <= 6 ? 1 : 2;

        bool digits = true;
        for (char c : q) {
            if (c < '0' || c > '9') { digits = false; break; }
        }
        if (digits) {
            const SongInfo* byId = Lookup((uint32_t)strtoul(q.c_str(), nullptr, 10));
            if (byId) out.push_back({byId, 2000, 0});
            return out;
        }

        for (const auto& kv : songs_) {
            const SongInfo& si = kv.second;
            std::string t = NormText(si.title);
            int sc = 0;
            if (t == q) {
                sc = 1000;
            } else if (t.size() >= q.size() && t.compare(0, q.size(), q) == 0) {
                sc = 800 - (int)std::min<size_t>(199, t.size() - q.size());
            } else {
                size_t p = t.find(q);
                if (p != std::string::npos) sc = 600 - (int)std::min<size_t>(199, p);
            }
            if (!sc) {
                std::string a = NormText(si.artist);
                if (!a.empty() && a.find(q) != std::string::npos) sc = 300;
            }
            // 别名匹配（精确 > 前缀 > 包含），低于标题精确、高于曲师
            auto ait = aliases_.find(si.id);
            if (ait != aliases_.end()) {
                for (const auto& a : ait->second) {
                    int asc = 0;
                    if (a == q) {
                        asc = 950;
                    } else if (a.size() >= q.size() && a.compare(0, q.size(), q) == 0) {
                        asc = 780 - (int)std::min<size_t>(199, a.size() - q.size());
                    } else {
                        size_t p = a.find(q);
                        if (p != std::string::npos) asc = 560 - (int)std::min<size_t>(199, p);
                    }
                    if (asc == 0 && fuzzyQ) {
                        int d = EditDistance(a, q, fuzzyMax);
                        if (d <= fuzzyMax) asc = 450 - d * 50;
                    }
                    if (asc > sc) sc = asc;
                    if (sc >= 950) break;
                }
            }
            if (fuzzyQ && sc < 450) {
                int d = EditDistance(t, q, fuzzyMax);
                if (d <= fuzzyMax) {
                    int fs = 470 - d * 60;
                    if (fs > sc) sc = fs;
                }
            }
            if (sc > 0) out.push_back({&si, sc, t.size()});
        }
        std::sort(out.begin(), out.end(), [](const SongMatch& a, const SongMatch& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.titleLen < b.titleLen;
        });
        if (out.size() > maxN) out.resize(maxN);
        return out;
    }

    const SongInfo* FindBest(const std::string& query) const {
        auto v = FindCandidates(query, 1);
        return v.empty() ? nullptr : v[0].song;
    }

private:
    // 归一化：小写、去空白/常见标点、全角空格；CJK 原样保留
    static std::string NormText(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
                // we're → we are，便于 "we are back" 命中 "WE'RE BACK!!"
                if (c == '\'' && i + 2 < s.size() &&
                    (s[i + 1] == 'r' || s[i + 1] == 'R') &&
                    (s[i + 2] == 'e' || s[i + 2] == 'E')) {
                    out += "are";
                    i += 3;
                    continue;
                }
                if (c == '-' || c == '_' || c == '.' || c == ',' || c == ':' ||
                    c == ';' || c == '\'' || c == '"' || c == '(' || c == ')' ||
                    c == '[' || c == ']' || c == '{' || c == '}' || c == '!' ||
                    c == '?' || c == '/' || c == '\\' || c == '&' || c == '+') {
                    ++i;
                    continue;
                }
                out += (char)std::tolower(c);
                ++i;
            } else if (c == 0xE3 && i + 2 < s.size() &&
                       (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) {
                i += 3;   // 全角空格 U+3000
            } else {
                out += s[i++];
            }
        }
        return out;
    }

    // 从 <...>...</...> 块中提取内容；支持 <name><str>文本</str></name> 嵌套形式
    static std::string ExtractField(const std::string& block, const std::string& tag) {
        size_t b = block.find("<" + tag);
        if (b == std::string::npos) return std::string();
        size_t gt = block.find('>', b);
        if (gt == std::string::npos) return std::string();
        size_t e = block.find("</" + tag + ">", gt);
        if (e == std::string::npos) return std::string();
        std::string inner = block.substr(gt + 1, e - gt - 1);
        // 嵌套 <str> 优先
        size_t sb = inner.find("<str>");
        if (sb != std::string::npos) {
            size_t se = inner.find("</str>", sb);
            if (se != std::string::npos) inner = inner.substr(sb + 5, se - sb - 5);
        } else {
            // 去掉所有残余标签
            while (true) {
                size_t tb = inner.find('<');
                if (tb == std::string::npos) break;
                size_t te = inner.find('>', tb);
                if (te == std::string::npos) break;
                inner.erase(tb, te - tb + 1);
            }
        }
        return trim(inner);
    }

    static double ExtractNum(const std::string& block, const std::string& tag) {
        std::string v = ExtractField(block, tag);
        return parse_f64(v);
    }

    static bool ParseMusicBlock(const std::string& block, SongInfo& si) {
        uint64_t id = parse_u64(ExtractField(block, "id"), 0xFFFFFFFFull);
        if (id > 0xFFFFFFFEull) return false;
        si.id = (uint32_t)id;
        si.title = ExtractField(block, "name");
        si.artist = ExtractField(block, "artist");
        if (si.artist.empty()) si.artist = ExtractField(block, "artistName");
        si.bpm = ExtractNum(block, "bpm");
        // 难度定数：每个 <difficulty> 子块内取 <str> 名与第一个 <const>
        size_t pos = 0;
        while (true) {
            size_t b = block.find("<difficulty", pos);
            if (b == std::string::npos) break;
            size_t e = block.find("</difficulty>", b);
            if (e == std::string::npos) break;
            std::string d = block.substr(b, e - b);
            pos = e + 13;
            std::string name = ExtractField(d, "str");
            double c = ExtractNum(d, "const");
            for (int i = 0; i < 6; ++i) {
                if (iequals(name, si.diff_names[i])) si.diff_const[i] = c;
            }
        }
        return !si.title.empty();
    }

    std::unordered_map<uint32_t, SongInfo> songs_;
    std::unordered_map<uint32_t, std::vector<std::string>> aliases_;   // 已归一化
    mutable std::mutex aliasMtx_;
};

} // namespace csk
