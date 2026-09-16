// src/snapshot.cc · scoring —— 输入快照的 JSON 形状与序列化（SCD-SCORE-05 / SCD-EXP-03）
//
// 口径：
//   · 快照是**唯一**的评分输入形状；引擎 MUST NOT 读全局状态或数据库（SCD-SCORE-05）。
//   · 缺项（如 topology 未就绪 —— 风险 R6）一律按"缺失"处理并标注，**不阻塞评分**。
//   · 字段命名 camelCase（protocol P4）；键序 = 成员声明顺序（确定性，SCD-SCORE-06）。
//   · 实体一律用 `id` 引用（protocol §2 CTR-EN-01：MUST NOT 用 `no` 作引用键）。
#include "internal.h"

namespace scoring {
namespace detail {

json toJson(const PhaseContext& v) {
    return json{{"phaseKey", v.phaseKey},
                {"seq", v.seq},
                {"scenarioKey", v.scenarioKey},
                {"enteredAt", v.enteredAt},
                {"missionId", v.missionId}};
}

json toJson(const ClusterUsage& v) {
    return json{{"clusterId", v.clusterId},
                {"phaseKey", v.phaseKey},
                {"total", v.total},
                {"available", v.available},
                {"present", v.present}};
}

json toJson(const ResourceSnapshot& v) {
    json clusters = json::array();
    for (const auto& c : v.clusters) clusters.push_back(toJson(c));
    return json{{"phase", toJson(v.phase)}, {"present", v.present}, {"clusters", clusters}};
}

json toJson(const LinkEval& v) {
    return json{{"linkId", v.linkId},       {"from", v.from},
                {"to", v.to},               {"state", v.state},
                {"signal", v.signal},       {"bandwidthMbps", v.bandwidthMbps},
                {"latencyMs", v.latencyMs}, {"lossRate", v.lossRate},
                {"coverageKm2", v.coverageKm2},
                {"present", v.present}};
}

json toJson(const TopologySnapshot& v) {
    json links = json::array();
    for (const auto& l : v.links) links.push_back(toJson(l));
    return json{{"phase", toJson(v.phase)}, {"present", v.present}, {"links", links}};
}

json toJson(const TargetList& v) {
    json ids = json::array();
    for (const auto& id : v.targetIds) ids.push_back(id);
    json conf = json::array();
    for (int c : v.confidences) conf.push_back(c);
    return json{{"phase", toJson(v.phase)},
                {"present", v.present},
                {"targetIds", ids},
                {"confidences", conf},
                {"modelCount", v.modelCount}};
}

json toJson(const ScoringSnapshot& v) {
    json out = json::object();
    out["missionId"] = v.missionId;
    out["phase"] = toJson(v.phase);
    out["resourceUtilization"] = v.resourceUtilization;
    out["hasResourceUtilization"] = v.hasResourceUtilization;
    if (v.resources) out["resources"] = toJson(*v.resources);
    if (v.topology) out["topology"] = toJson(*v.topology);
    if (v.targets) out["targets"] = toJson(*v.targets);
    out["extra"] = v.extra;
    return out;
}

// ---------------------------------------------------------------- 反序列化（宿主投喂）

static PhaseContext phaseFromJson(const json& v) {
    PhaseContext p;
    p.phaseKey = getString(v, "phaseKey");
    p.seq = getInt(v, "seq", 0);
    p.scenarioKey = getString(v, "scenarioKey");
    p.enteredAt = static_cast<int64_t>(getNumber(v, "enteredAt", 0.0));
    p.missionId = getString(v, "missionId");
    return p;
}

ResourceSnapshot resourceFromJson(const json& v) {
    ResourceSnapshot r;
    if (!v.is_object()) return r;
    r.present = getBool(v, "present", true);
    const json* ph = find(v, "phase");
    if (ph) r.phase = phaseFromJson(*ph);
    const json* clusters = find(v, "clusters");
    if (clusters && clusters->is_array()) {
        for (const auto& c : *clusters) {
            if (!c.is_object()) continue;
            ClusterUsage u;
            u.clusterId = getString(c, "clusterId");
            u.phaseKey = getString(c, "phaseKey");
            u.total = getInt(c, "total", 0);
            u.available = getBool(c, "available", true);
            u.present = getBool(c, "present", true);
            r.clusters.push_back(u);
        }
    }
    return r;
}

TopologySnapshot topologyFromJson(const json& v) {
    TopologySnapshot t;
    if (!v.is_object()) return t;
    t.present = getBool(v, "present", true);
    const json* ph = find(v, "phase");
    if (ph) t.phase = phaseFromJson(*ph);
    const json* links = find(v, "links");
    if (links && links->is_array()) {
        for (const auto& l : *links) {
            if (!l.is_object()) continue;
            LinkEval e;
            e.linkId = getString(l, "linkId");
            e.from = getString(l, "from");
            e.to = getString(l, "to");
            e.state = getString(l, "state");
            e.signal = getNumber(l, "signal", 0.0);
            e.bandwidthMbps = getNumber(l, "bandwidthMbps", 0.0);
            e.latencyMs = getNumber(l, "latencyMs", 0.0);
            e.lossRate = getNumber(l, "lossRate", 0.0);
            e.coverageKm2 = getNumber(l, "coverageKm2", 0.0);
            e.present = getBool(l, "present", true);
            t.links.push_back(e);
        }
    }
    return t;
}

TargetList targetsFromJson(const json& v) {
    TargetList t;
    if (!v.is_object()) return t;
    t.present = getBool(v, "present", true);
    const json* ph = find(v, "phase");
    if (ph) t.phase = phaseFromJson(*ph);
    t.targetIds = getStringArray(v, "targetIds");
    const json* conf = find(v, "confidences");
    if (conf && conf->is_array()) {
        for (const auto& c : *conf) {
            if (c.is_number()) t.confidences.push_back(static_cast<int>(c.get<double>()));
        }
    }
    t.modelCount = getInt(v, "modelCount", 0);
    return t;
}

ScoringSnapshot snapshotFromJson(const json& v) {
    ScoringSnapshot s;
    if (!v.is_object()) return s;
    s.missionId = getString(v, "missionId");
    const json* ph = find(v, "phase");
    if (ph) s.phase = phaseFromJson(*ph);
    const json* res = find(v, "resources");
    if (res && res->is_object()) s.resources = resourceFromJson(*res);
    const json* topo = find(v, "topology");
    if (topo && topo->is_object()) s.topology = topologyFromJson(*topo);
    const json* tg = find(v, "targets");
    if (tg && tg->is_object()) s.targets = targetsFromJson(*tg);
    if (hasKey(v, "resourceUtilization")) {
        s.resourceUtilization = getNumber(v, "resourceUtilization", 0.0);
        s.hasResourceUtilization = true;
    }
    const json* ex = find(v, "extra");
    if (ex && ex->is_object()) s.extra = *ex;
    return s;
}

// ---------------------------------------------------------------- 快照查询助手

}  // namespace detail

std::optional<ClusterUsage> ResourceSnapshot::cluster(const std::string& clusterId) const {
    for (const auto& c : clusters) {
        if (c.clusterId == clusterId) return c;
    }
    return std::nullopt;
}

}  // namespace scoring
