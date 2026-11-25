#include "fpp-pch.h"

#include <atomic>
#include <cstdlib>

#include "TmcApi.h"

#include "AxisWorker.h"
#include "TmcManager.h"

#include "Plugin.h"
#include "common.h"
#include "fpphttp.h"
#include "log.h"

namespace tmc {

/*
 * The path drogon routes internally. Apache exposes it as
 * /api/plugin-apis/stepper-motion ("RewriteRule ^plugin-apis/(.*)$
 * http://localhost:32322/$1" strips the prefix); anything registered under
 * /api/... here is unreachable from outside.
 */
const char* API_BASE_PATH = "/stepper-motion";
const char* API_PUBLIC_PATH = "api/plugin-apis/stepper-motion";

/*
 * Everything hangs off that ONE path, with the action and axis as query
 * parameters. Subpaths would need registerPluginApi's family=true, a drogon
 * REGEX route, and drogon only builds its regex router at startup: a plugin
 * loaded at runtime gets a silent 404 on every subpath. Static routes are
 * picked up whenever they are added.
 */

static HttpResponsePtr jsonResponse(const Json::Value& v, int status = 200) {
    return makeStringResponse(SaveJsonToString(v), status, "application/json");
}

static HttpResponsePtr errorResponse(const std::string& message, int status = 400) {
    Json::Value v;
    v["status"] = "ERROR";
    v["message"] = message;
    return jsonResponse(v, status);
}

// A refused save: every problem as a list, plus the same list as one message
// ("N problems:" with bullet lines, the phrasing the page shows) for a caller that
// only shows message.
static HttpResponsePtr errorsResponse(const std::vector<std::string>& errors, int status = 400) {
    Json::Value v;
    v["status"] = "ERROR";
    std::string message = errors.size() == 1 ? errors[0]
                                              : std::to_string(errors.size()) + " problems:";
    for (const auto& e : errors) {
        v["errors"].append(e);
        if (errors.size() > 1)
            message += "\n\u2022 " + e;
    }
    v["message"] = message;
    return jsonResponse(v, status);
}

// The configuration actions. None of them needs a stepper.
static bool handleConfigAction(const std::string& action, const Json::Value& body,
                               HttpCallback& callback) {
    TmcManager& mgr = TmcManager::instance();
    if (action == "config") {
        Json::Value v;
        v["status"] = "OK";
        int64_t revision = 0;
        v["config"] = mgr.configSnapshot(revision);
        v["revision"] = (Json::Int64)revision;
        // The resolved view next to the raw one: the page edits config and
        // shows these as the value an inheriting field will get.
        v["defaultsEffective"] = mgr.defaultsEffectiveJson();
        v["axesEffective"] = mgr.axesEffectiveJson();
        v["spiBuses"] = TmcManager::spiBusesJson();
        v["configErrors"] = Json::Value(Json::arrayValue);
        for (const auto& e : mgr.configErrors())
            v["configErrors"].append(e);
        v["structuralChangePending"] = mgr.structuralChangePending();
        callback(jsonResponse(v));
        return true;
    }
    if (action == "saveConfig" || action == "savePresets") {
        const char* key = action == "saveConfig" ? "config" : "presets";
        if (!body.isObject() || !body[key].isObject()) {
            callback(errorResponse(std::string("missing ") + key + " object"));
            return true;
        }
        std::vector<std::string> errors;
        bool restartNeeded = false;
        bool stale = false;
        // The revision the page loaded; absent (a script) skips the check.
        const int64_t expect = body["revision"].isIntegral() ? body["revision"].asInt64() : -1;
        int64_t revision = 0;
        bool ok;
        if (action == "saveConfig") {
            ok = mgr.saveConfig(body["config"], errors, restartNeeded, expect, &stale, &revision);
        } else {
            if (!FileExists(mgr.configFilePath())) {
                callback(errorResponse("no configuration yet -- add a stepper and save "
                                       "the configuration first"));
                return true;
            }
            ok = mgr.savePresets(body["presets"], errors, expect, &stale, &revision);
        }
        if (!ok) {
            callback(errorsResponse(errors, stale ? 409 : 400));
            return true;
        }
        Json::Value v;
        v["status"] = "OK";
        v["revision"] = (Json::Int64)revision;
        v["restartNeeded"] = restartNeeded;
        v["message"] = "";
        callback(jsonResponse(v));
        return true;
    }
    return false;
}

// Body values win over query parameters: the UI posts JSON, a curl caller
// can use ?irun=8.
static double argDouble(const Json::Value& body, const HttpRequestPtr& req,
                        const char* key, double dflt) {
    if (body.isMember(key) && body[key].isNumeric())
        return body[key].asDouble();
    std::string s = req->getParameter(key);
    if (!s.empty())
        return atof(s.c_str());
    return dflt;
}

static int argInt(const Json::Value& body, const HttpRequestPtr& req, const char* key,
                  int dflt) {
    if (body.isMember(key) && body[key].isNumeric())
        return body[key].asInt();
    std::string s = req->getParameter(key);
    if (!s.empty())
        return atoi(s.c_str());
    return dflt;
}

static std::string argString(const Json::Value& body, const HttpRequestPtr& req,
                             const char* key, const std::string& dflt) {
    if (body.isMember(key) && body[key].isString())
        return body[key].asString();
    std::string s = req->getParameter(key);
    if (!s.empty())
        return s;
    return dflt;
}

// Fills the motion parameters every job shares.
static void applyCommonArgs(Job& job, const Json::Value& body,
                            const HttpRequestPtr& req) {
    job.rps = argDouble(body, req, "rps", -1.0);
    job.irun = argInt(body, req, "irun", -1);
    job.ihold = argInt(body, req, "ihold", -1);
}

static HttpResponsePtr submitJob(AxisWorker* w, const Job& job) {
    std::string err;
    uint64_t id = w->submit(job, err);
    if (id == 0)
        return errorResponse(err, 409);
    Json::Value v;
    v["status"] = "OK";
    // Long operations run asynchronously and are watched through status.
    v["jobId"] = (Json::UInt64)id;
    v["kind"] = Job::kindName(job.kind);
    return jsonResponse(v);
}

static void handle(const HttpRequestPtr& req, HttpCallback&& callback) {
    Json::Value body;
    std::string content = getRequestContent(req);
    if (!content.empty())
        LoadJsonFromString(content, body);
    if (!body.isObject())
        body = Json::Value(Json::objectValue);

    std::string action = req->getParameter("action");
    if (action.empty() && body.isMember("action") && body["action"].isString())
        action = body["action"].asString();
    if (action.empty())
        action = "status";

    TmcManager& mgr = TmcManager::instance();

    // Only the reads answer a GET: anything that moves a motor or writes the
    // configuration needs a POST, so a link, a bookmark or a browser's
    // prefetch can never rehome a prop.
    if (req->method() != drogon::Post && action != "status" && action != "config" &&
        action != "axisNames") {
        callback(errorResponse("action '" + action + "' needs a POST", 405));
        return;
    }

    if (action == "status" && req->getParameter("axis").empty()) {
        Json::Value v = mgr.statusJson();
        v["status"] = "OK";
        callback(jsonResponse(v));
        return;
    }

    // Feeds the "Stepper" dropdown on every Stepper Motion command in the FPP UI.
    if (action == "axisNames") {
        Json::Value v(Json::arrayValue);
        v.append("all");
        int n = mgr.axisCount();
        for (int i = 0; i < n; i++) {
            if (AxisWorker* w = mgr.axis(i))
                v.append(w->name());
        }
        callback(jsonResponse(v));
        return;
    }

    if (handleConfigAction(action, body, callback))
        return;

    std::string axisArg = req->getParameter("axis");
    if (axisArg.empty() && body.isMember("axis"))
        axisArg = body["axis"].isString() ? body["axis"].asString()
                                          : std::to_string(body["axis"].asInt());
    if (axisArg.empty()) {
        callback(errorResponse("this action needs a stepper (axis=) parameter"));
        return;
    }

    // Either an index (what the UI sends) or a name.
    AxisWorker* w = nullptr;
    bool numeric = axisArg.find_first_not_of("0123456789") == std::string::npos;
    if (numeric)
        w = mgr.axis(atoi(axisArg.c_str()));
    if (!w)
        w = mgr.axisByName(axisArg);
    if (!w) {
        callback(errorResponse("no stepper '" + axisArg + "' is running (there are " +
                                   std::to_string(mgr.axisCount()) + ")",
                               404));
        return;
    }

    // Stop is deliberately outside every other guard: it must work while the
    // show owns the axis, while a job is running, and while the reference is
    // invalid.
    if (action == "stop") {
        w->abortNow();
        Json::Value v;
        v["status"] = "OK";
        v["message"] = "stop requested";
        callback(jsonResponse(v));
        return;
    }

    if (action == "status") {
        Json::Value v = snapshotToJson(w->snapshot(), w->config());
        v["job"] = jobToJson(w->lastJob());
        v["status"] = "OK";
        callback(jsonResponse(v));
        return;
    }

    Job job;
    if (action == "probe") {
        job.kind = Job::Kind::Probe;
        applyCommonArgs(job, body, req);
    } else if (action == "jog") {
        job.kind = Job::Kind::Jog;
        job.delta = argDouble(body, req, "deltaFullsteps", 0.0);
        if (job.delta == 0) {
            callback(errorResponse("jog needs a non-zero deltaFullsteps"));
            return;
        }
        applyCommonArgs(job, body, req);
    } else if (action == "setHome") {
        job.kind = Job::Kind::SetHome;
        job.minFullsteps = argInt(body, req, "minFullsteps", 0);
        job.maxFullsteps = argInt(body, req, "maxFullsteps", 0);
        std::string dir = argString(body, req, "dir", "");
        if (dir == "ccw" || dir == "cw")
            job.approachDir = dir;
    } else if (action == "setMax") {
        job.kind = Job::Kind::SetMax;
        job.flipDir = argInt(body, req, "flip", 0) != 0;
    } else if (action == "goto") {
        job.kind = Job::Kind::Goto;
        job.target = argDouble(body, req, "targetFullsteps", 0.0);
        applyCommonArgs(job, body, req);
    } else if (action == "rehome") {
        job.kind = Job::Kind::Rehome;
    } else if (action == "testTravel") {
        job.kind = Job::Kind::TestTravel;
        job.cycles = argInt(body, req, "cycles", 1);
        if (job.cycles < 1 || job.cycles > 100) {
            callback(errorResponse("cycles must be 1..100"));
            return;
        }
        applyCommonArgs(job, body, req);
    } else if (action == "runPreset") {
        job.kind = Job::Kind::RunPreset;
        job.value = argInt(body, req, "value", 0);
        if (job.value < 1 || job.value > 255) {
            callback(errorResponse("preset value must be 1..255"));
            return;
        }
    } else if (action == "release") {
        job.kind = Job::Kind::Release;
    } else if (action == "park") {
        job.kind = Job::Kind::Park;
        applyCommonArgs(job, body, req);
    } else {
        callback(errorResponse("unknown action '" + action + "'", 404));
        return;
    }

    callback(submitJob(w, job));
}

// Which plugin instance currently owns the registered path.
static std::atomic<const void*> s_apiOwner { nullptr };

void registerTmcApis(const void* owner) {
    s_apiOwner.store(owner);
    // family=false: see the comment on API_BASE_PATH. A regex subpath route
    // would not work for a runtime-loaded plugin.
    FPPPlugins::registerPluginApi(
        API_BASE_PATH,
        [](const HttpRequestPtr& req, HttpCallback&& cb) { handle(req, std::move(cb)); },
        { drogon::Get, drogon::Post }, false);
    LogInfo(VB_PLUGIN, "StepperMotion: API registered at %s (public: %s)\n",
            API_BASE_PATH, API_PUBLIC_PATH);
}

void unregisterTmcApis(const void* owner) {
    // Only the current owner may disarm. See the comment in TmcApi.h.
    const void* expected = owner;
    if (!s_apiOwner.compare_exchange_strong(expected, nullptr)) {
        LogWarn(VB_PLUGIN,
                "StepperMotion: ignoring API teardown from a retired plugin instance; "
                "the live one keeps %s\n",
                API_BASE_PATH);
        return;
    }
    FPPPlugins::unregisterPluginApi(API_BASE_PATH);
    LogInfo(VB_PLUGIN, "StepperMotion: API unregistered from %s\n", API_BASE_PATH);
}

} // namespace tmc
