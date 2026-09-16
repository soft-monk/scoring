// src/util.cc · scoring —— 定点舍入 / 归一化 / 摘要 / JSON 取值（纯函数，无状态）
//
// 权威依据：需求专篇 SCD-SCORE-02（归一显式可复现）、SCD-SCORE-06（明确舍入、确定性）、
//           SCD-NFR-05（浮点可复现）、protocol.md §5.2（schemaVersion MAJOR 校验）；
//           冲突裁决 ADR-C1-04（可复算）。
//
// 本文件 MUST NOT 出现任何业务取值（方案名 / 指标演示数值 / 硬编码优化）—— P6/P7。
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
namespace scoring {
namespace detail {

// ---------------------------------------------------------------- 定点工具

int64_t scaled(double value, int64_t scale) {
    if (scale == 0) return 0;
    if (!(value == value)) return 0;  // NaN → 0（MUST NOT 传播 NaN 到输出）
    const bool neg = value < 0;
    const double mag = neg ? -value : value;
    const int64_t q = static_cast<int64_t>(std::llround(mag * static_cast<double>(scale)));
    return neg ? -q : q;
}

std::string numToStableString(double v) {
    if (!(v == v)) return "0";
    if (v == 0) return "0";
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return std::string(buf);
    }
    // %.*g 的"最短表示"语义：17 位有效数字足以无损往返，尾部零自动去掉
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s(buf);
    // 去掉 %g 可能留下的指数写法里的多余零（保持跨平台一致）
    return s;
}

// ---------------------------------------------------------------- JSON 取值助手

const json* find(const json& obj, const std::string& key) {
    if (!obj.is_object()) return nullptr;
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &(*it);
}

bool hasKey(const json& obj, const std::string& key) { return find(obj, key) != nullptr; }

std::string getString(const json& obj, const std::string& key, const std::string& def) {
    const json* v = find(obj, key);
    if (!v || !v->is_string()) return def;
    return v->get<std::string>();
}

double getNumber(const json& obj, const std::string& key, double def) {
    const json* v = find(obj, key);
    if (!v) return def;
    if (v->is_number()) return v->get<double>();
    return def;
}

bool getBool(const json& obj, const std::string& key, bool def) {
    const json* v = find(obj, key);
    if (!v || !v->is_boolean()) return def;
    return v->get<bool>();
}

int getInt(const json& obj, const std::string& key, int def) {
    const json* v = find(obj, key);
    if (!v || !v->is_number()) return def;
    return static_cast<int>(v->get<double>());
}

std::vector<std::string> getStringArray(const json& obj, const std::string& key) {
    std::vector<std::string> out;
    const json* v = find(obj, key);
    if (!v || !v->is_array()) return out;
    for (const auto& e : *v) {
        if (e.is_string()) out.push_back(e.get<std::string>());
    }
    return out;
}

// ---------------------------------------------------------------- 规范化序列化

static void canonicalInto(const json& v, std::string& out) {
    switch (v.type()) {
        case json::value_t::null:
            out += "null";
            return;
        case json::value_t::boolean:
            out += (v.get<bool>() ? "true" : "false");
            return;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
            out += std::to_string(v.get<long long>());
            return;
        case json::value_t::number_float:
            out += numToStableString(v.get<double>());
            return;
        case json::value_t::string: {
            // JSON 字符串转义（只做必要转义，确定性由同一实现保证）
            out += '"';
            for (char c : v.get<std::string>()) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default: out += c; break;
                }
            }
            out += '"';
            return;
        }
        case json::value_t::array: {
            out += '[';
            bool first = true;
            for (const auto& e : v) {
                if (!first) out += ',';
                first = false;
                canonicalInto(e, out);
            }
            out += ']';
            return;
        }
        case json::value_t::object: {
            // 键按**字节升序**（ASCII 升序）；ordered_json 的迭代顺序不参与
            std::vector<std::string> keys;
            keys.reserve(v.size());
            for (auto it = v.begin(); it != v.end(); ++it) keys.push_back(it.key());
            std::sort(keys.begin(), keys.end());
            out += '{';
            bool first = true;
            for (const auto& k : keys) {
                if (!first) out += ',';
                first = false;
                out += '"';
                out += k;
                out += "\":";
                canonicalInto(v.at(k), out);
            }
            out += '}';
            return;
        }
        default:
            out += "null";
            return;
    }
}

std::string canonicalJson(const json& v) {
    std::string out;
    out.reserve(256);
    canonicalInto(v, out);
    return out;
}

// ---------------------------------------------------------------- 规则包骨架

SkeletonCheck checkSkeleton(const json& pkg, const std::string& expectedKind) {
    SkeletonCheck r;
    if (!pkg.is_object()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "policies package MUST be a JSON object";
        r.issues.push_back({"", "", "not-an-object"});
        return r;
    }
    const bool hasNs = hasKey(pkg, "policiesNamespace") && find(pkg, "policiesNamespace")->is_string();
    const bool hasVer = hasKey(pkg, "schemaVersion") && find(pkg, "schemaVersion")->is_string();
    const bool hasKind = hasKey(pkg, "kind") && find(pkg, "kind")->is_string();
    if (!hasNs) r.issues.push_back({"", "policiesNamespace", "missing-required-field"});
    if (!hasVer) r.issues.push_back({"", "schemaVersion", "missing-required-field"});
    if (!hasKind) r.issues.push_back({"", "kind", "missing-required-field"});
    if (!r.issues.empty()) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "policies skeleton incomplete";
        return r;
    }
    r.policiesNamespace = getString(pkg, "policiesNamespace");
    r.schemaVersion = getString(pkg, "schemaVersion");

    const std::string kind = getString(pkg, "kind");
    if (kind != expectedKind) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "policies kind mismatch";
        r.issues.push_back({"kind", "kind", "expected:" + expectedKind + ",actual:" + kind});
        return r;
    }

    const auto major = majorOfVersion(r.schemaVersion);
    if (!major) {
        r.code = static_cast<int>(ErrorCode::BadRequest);
        r.message = "schemaVersion MUST be MAJOR.MINOR.PATCH";
        r.issues.push_back({"schemaVersion", "schemaVersion", "not-semver"});
        return r;
    }
    if (*major != kSupportedPoliciesMajor) {
        // protocol §5.2：MAJOR 不匹配 MUST 拒绝装载（1006），MUST NOT 静默降级
        r.code = static_cast<int>(ErrorCode::VersionMismatch);
        r.message = "schemaVersion MAJOR not supported";
        r.issues.push_back({"schemaVersion", "schemaVersion",
                            "unsupported-major:" + std::to_string(*major)});
        return r;
    }

    // 摘要：规范化后的规则包字节（同一内容 → 同一 digest）
    const std::string canon = canonicalJson(pkg);
    r.digest = digestHex(fnv1a64(canon));

    // 未知顶层段 → 告警（MUST NOT 导致装载失败 —— CTR-PL-03）
    static const char* kKnown[] = {"policiesNamespace", "schemaVersion", "kind", "items",
                                   "aggregate",         "searchSpace",     "reasons",
                                   "profiles",          "confirmPrecondition", "_note"};
    for (auto it = pkg.begin(); it != pkg.end(); ++it) {
        bool known = false;
        for (const char* k : kKnown) {
            if (it.key() == k) {
                known = true;
                break;
            }
        }
        if (!known) r.warnings.push_back("unknown-top-level-section:" + it.key());
    }

    r.ok = true;
    r.code = 0;
    r.message = "ok";
    return r;
}

}  // namespace detail

// ---------------------------------------------------------------- 公开纯函数

int64_t roundHalfUpScaled(double value, int64_t scale) { return detail::scaled(value, scale); }

// ---------------------------------------------------------------- 标度约定（唯一）
//
// 内部定点值 ↔ 数值：`定点 = round(数值 × scale)`，`数值 = 定点 / scale`（scale == 1.0 即 100%）。
// 百分比数值 ↔ 百分比定点（outScale 为"100% 对应的定点基数"）：
//   `百分比定点 = round(百分比数值 / 100 × outScale)`
// 三个对外纯函数都是这条约定的直接实现（互为逆运算，SCD-SCORE-06）。

int64_t roundPercent(double percent, int64_t outScale) {
    // 约定：**`outScale` 是定点标度，`outScale` 为 100 时表示 1.0 ↔ 100%**。
    // 于是"百分比数值 → 标度 outScale 的定点值" = `round(percent / 100 × outScale)`：
    // `outScale` 取 1 → 整数百分比；取 100 → 两位小数百分比；取 kScale → 引擎内部定点。
    // 与 `scaledToDouble(scaled, scale)` 互为逆运算（SCD-SCORE-06）。
    const double v = percent * (static_cast<double>(outScale) / 100.0);
    const bool neg = v < 0;
    const double mag = neg ? -v : v;
    const int64_t q = static_cast<int64_t>(mag + 0.5);  // half-up
    return neg ? -q : q;
}

int roundToPercent(int64_t scaledValue, int64_t scale) {
    // 内部定点（92.96% ↔ 9296）→ 整数百分比（half-up）
    return static_cast<int>(roundPercent(scaledToDouble(scaledValue, scale), 100));
}

double scaledToDouble(int64_t scaledValue, int64_t scale) {
    // 内部定点（92.96% ↔ 9296）→ 对外百分比数值（两位小数）
    if (scale <= 0) return 0.0;
    return detail::unscaled(scaledValue, scale / 100);
}

double normalizeValue(const MetricDef& def, double raw) {
    double n = 0.0;
    switch (def.normalize.type) {
        case NormalizeType::Percent: {
            const double sc = def.normalize.scale;
            n = (sc == 0.0) ? 0.0 : (raw / sc);
            break;
        }
        case NormalizeType::Range: {
            const double lo = def.normalize.min;
            const double hi = def.normalize.max;
            if (hi <= lo) {
                n = 0.0;
            } else {
                n = (raw - lo) / (hi - lo);
                if (def.direction == MetricDirection::Lower) n = 1.0 - n;
            }
            break;
        }
        case NormalizeType::Threshold: {
            n = 0.0;
            bool hit = false;
            // bands 按 min 降序求值（装载期已排序）；取第一个 raw >= min 的 value
            for (const auto& b : def.normalize.bands) {
                if (raw >= b.first) {
                    n = b.second;
                    hit = true;
                    break;
                }
            }
            if (!hit && !def.normalize.bands.empty()) n = def.normalize.bands.back().second;
            break;
        }
    }
    return detail::clampTo(n, 0.0, 1.0);
}

int64_t fnv1a64(const std::string& bytes) {
    uint64_t h = 14695981039346656037ULL;  // FNV-1a 64 offset basis（无符号回绕，见算法规范）
    for (unsigned char c : bytes) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;  // FNV-1a 64 prime
    }
    return static_cast<int64_t>(h);
}

std::string digestHex(int64_t digest) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(digest));
    return std::string(buf);
}

const char* toString(MetricDirection d) {
    return d == MetricDirection::Higher ? "higher" : "lower";
}
const char* toString(NormalizeType t) {
    switch (t) {
        case NormalizeType::Percent: return "percent";
        case NormalizeType::Range: return "range";
        case NormalizeType::Threshold: return "threshold";
    }
    return "unknown";
}
const char* toString(MetricSource s) {
    switch (s) {
        case MetricSource::Template: return "template";
        case MetricSource::Snapshot: return "snapshot";
        case MetricSource::Profile: return "profile";
        case MetricSource::Scalar: return "scalar";
    }
    return "unknown";
}
const char* toString(ConfirmPrecondition p) {
    return p == ConfirmPrecondition::RejectIfNotAdopted ? "reject-if-not-adopted" : "auto-adopt";
}
const char* toString(ResultStatus s) {
    switch (s) {
        case ResultStatus::Ok: return "ok";
        case ResultStatus::Rejected: return "rejected";
        case ResultStatus::AlreadyApplied: return "already-applied";
    }
    return "unknown";
}

std::optional<MetricDirection> metricDirectionFromString(const std::string& s) {
    if (s == "higher") return MetricDirection::Higher;
    if (s == "lower") return MetricDirection::Lower;
    return std::nullopt;
}
std::optional<NormalizeType> normalizeTypeFromString(const std::string& s) {
    if (s == "percent") return NormalizeType::Percent;
    if (s == "range") return NormalizeType::Range;
    if (s == "threshold") return NormalizeType::Threshold;
    return std::nullopt;
}
std::optional<MetricSource> metricSourceFromString(const std::string& s) {
    if (s == "template") return MetricSource::Template;
    if (s == "snapshot") return MetricSource::Snapshot;
    if (s == "profile") return MetricSource::Profile;
    if (s == "scalar") return MetricSource::Scalar;
    return std::nullopt;
}
std::optional<ConfirmPrecondition> confirmPreconditionFromString(const std::string& s) {
    if (s == "reject-if-not-adopted") return ConfirmPrecondition::RejectIfNotAdopted;
    if (s == "auto-adopt") return ConfirmPrecondition::AutoAdopt;
    return std::nullopt;
}

std::optional<int> majorOfVersion(const std::string& v) {
    const std::size_t p1 = v.find('.');
    if (p1 == std::string::npos || p1 == 0) return std::nullopt;
    const std::size_t p2 = v.find('.', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1 || p2 + 1 >= v.size()) return std::nullopt;
    for (char c : v.substr(0, p1)) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    for (char c : v.substr(p1 + 1, p2 - p1 - 1)) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    for (char c : v.substr(p2 + 1)) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    return std::atoi(v.substr(0, p1).c_str());
}

const char* errorCodeName(int code) {
    switch (code) {
        case 0: return "ok";
        case 1000: return "bad-request";
        case 1002: return "conflict";
        case 1003: return "precondition-unmet";
        case 1004: return "not-found";
        case 1005: return "internal";
        case 1006: return "version-mismatch";
        default: return "unknown";
    }
}

}  // namespace scoring
